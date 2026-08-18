#include <chrono>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <llama.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/spdlog.h>

#include "config.hpp"
#include "embedding_provider.hpp"
#include "generation_provider.hpp"
#include "job_dispatcher.hpp"
#include "job_store.hpp"
#include "job_type.hpp"
#include "watch_cache.hpp"

namespace
{

// llama.cpp documents backend init/free as call-once-per-process. Both
// LlamaProvider and LlamaGenerationProvider are llama.cpp-backed but no
// longer call these themselves — this is the single process-wide owner,
// scoped to outlive both providers (declared before either in main()).
struct LlamaBackendGuard {
    LlamaBackendGuard()  { llama_backend_init(); }
    ~LlamaBackendGuard() { llama_backend_free(); }
};

// Config isn't loaded yet when this runs (log_file/log_level are themselves
// config), so failures here only ever reach stderr.
void init_logger(const pgquarry::LoggingConfig& cfg)
{
    try {
        std::filesystem::create_directories(std::filesystem::path(cfg.log_file).parent_path());
        auto file_logger = spdlog::rotating_logger_mt(
            "pgquarry", cfg.log_file,
            static_cast<size_t>(cfg.max_size_mb) * 1024 * 1024, cfg.max_files);
        spdlog::set_default_logger(file_logger);
        spdlog::set_level(spdlog::level::from_str(cfg.log_level));
        spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
        spdlog::flush_every(std::chrono::seconds(3));
        spdlog::info("[worker] logger initialized — file={}, level={}, max_size_mb={}, max_files={}",
                     cfg.log_file, cfg.log_level, cfg.max_size_mb, cfg.max_files);
    } catch (const spdlog::spdlog_ex& e) {
        std::cerr << "logger initialization failed: " << e.what() << "\n";
    }
}

void print_usage(const char* argv0)
{
    std::cerr <<
        "Usage: " << argv0 << " --config <pgquarry.toml path>\n"
        "\n"
        "See pgquarry.toml.example for the config file format ([worker] and "
        "[retention] — table watches are registered via SQL, see pgquarry.watch()).\n";
}

volatile std::sig_atomic_t g_shutdown = 0;
void handle_signal(int) { g_shutdown = 1; }

// Worker checks for purge-eligible rows on a wall-clock cadence independent
// of poll_interval_ms, rather than every single idle tick — retention has
// no dedicated interval in pgquarry.toml, so this is a fixed default.
constexpr auto kPurgeCheckInterval = std::chrono::seconds(60);

} // namespace

int main(int argc, char** argv)
{
    std::string config_path;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--config") {
            if (i + 1 >= argc) { std::cerr << "missing value for --config\n"; return 1; }
            config_path = argv[++i];
        } else if (a == "--help" || a == "-h") {
            print_usage(argv[0]);
            return 0;
        } else {
            std::cerr << "unknown argument: " << a << "\n";
            print_usage(argv[0]);
            return 1;
        }
    }

    if (config_path.empty()) {
        std::cerr << "--config is required\n";
        print_usage(argv[0]);
        return 1;
    }

    // log_file/log_level live in this same config, so the logger can't come up until after this parses.
    pgquarry::AppConfig cfg;
    try {
        cfg = pgquarry::load_config(config_path);
    } catch (const std::exception& e) {
        std::cerr << "[worker] " << e.what() << "\n";
        return 1;
    }

    init_logger(cfg.logging);

    auto errors = cfg.validate();
    if (!errors.empty()) {
        for (const auto& e : errors) spdlog::error("[worker] {}", e);
        return 1;
    }

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    // Declared before any provider — C++ destroys locals in reverse
    // declaration order, so this outlives every provider constructed below.
    LlamaBackendGuard llama_backend_guard;

    std::shared_ptr<pgquarry::EmbeddingProvider> provider;
    try {
        provider = pgquarry::make_embedding_provider(cfg.embedding);
        provider->init();
        spdlog::debug("[worker] embedding provider initialized: {} ({} dims)", provider->name(), provider->dimensions());
    } catch (const std::exception& e) {
        spdlog::error("[worker] failed to initialize embedding provider: {}", e.what());
        return 1;
    }

    if (provider->dimensions() != cfg.embedding.dimensions) {
        spdlog::error("[worker] model produces {}-dimensional vectors but [worker] dimensions was {} — "
                       "fix pgquarry.toml's [worker] dimensions or your target vector column width",
                       provider->dimensions(), cfg.embedding.dimensions);
        return 1;
    }

    // Generation is optional — no generation_model_path means the worker still
    // starts, but any claimed 'generate' job fails per-job (dispatch_generate_jobs)
    // instead of blocking startup the way a missing embedding model_path does above.
    std::shared_ptr<pgquarry::GenerationProvider> gen_provider;
    if (!cfg.generation.model_path.empty()) {
        try {
            gen_provider = pgquarry::make_generation_provider(cfg.generation);
            gen_provider->init();
            spdlog::info("[worker] generation provider initialized: {}", gen_provider->name());
        } catch (const std::exception& e) {
            spdlog::error("[worker] failed to initialize generation provider: {}", e.what());
            return 1;
        }
    } else {
        spdlog::info("[worker] no [worker] generation_model_path configured — 'generate' jobs will be marked error");
    }

    pgquarry::JobStore store(cfg.conninfo);
    pgquarry::WatchCache mappings(store);
    try {
        store.connect();
        spdlog::debug("[worker] connected to database");
        /// watched_tables (populated via pgquarry.watch()/.watch_generate()) is the worker's write-back mapping source.
        mappings.load();
        spdlog::info("[worker] watching {} mapping(s): {}", mappings.size(), mappings.describe());
    } catch (const std::exception& e) {
        spdlog::error("[worker] {}", e.what());
        return 1;
    }

    std::optional<std::string> purge_interval = pgquarry::parse_purge_after(cfg.retention.purge_after);
    auto last_purge_check = std::chrono::steady_clock::now();

    spdlog::info("[worker] ready — provider={}, dimensions={}, generation_provider={}, batch_size={}, poll_interval_ms={}, mappings={}",
                 provider->name(), provider->dimensions(), gen_provider ? gen_provider->name() : "(none)",
                 cfg.embedding.max_batch_size, cfg.poll_interval_ms, mappings.size());

    while (!g_shutdown) {
        std::vector<pgquarry::JobStore::ClaimedJob> claimed;
        try {
            claimed = store.claim_pending(cfg.embedding.max_batch_size);
        } catch (const std::exception& e) {
            spdlog::error("[worker] claim_pending failed: {}", e.what());
            std::this_thread::sleep_for(std::chrono::milliseconds(cfg.poll_interval_ms));
            continue;
        }

        if (claimed.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(cfg.poll_interval_ms));
        } else {
            spdlog::debug("[worker] claimed {} job(s): {}", claimed.size(), [&claimed] {
                std::string ids;
                for (const auto& job : claimed) {
                    if (!ids.empty()) ids += ", ";
                    ids += std::to_string(job.id) + (job.source_table ? "@" + *job.source_table : "@(ad hoc)");
                }
                return ids;
            }());

            std::vector<pgquarry::JobStore::ClaimedJob> embed_jobs, generate_jobs;
            for (auto& job : claimed) {
                (job.job_type == pgquarry::JobType::Generate ? generate_jobs : embed_jobs).push_back(job);
            }

            mappings.begin_batch();
            int written = pgquarry::dispatch_embed_jobs(store, *provider, mappings, embed_jobs)
                        + pgquarry::dispatch_generate_jobs(store, gen_provider.get(), cfg.generation.max_tokens, mappings, generate_jobs);

            if (written > 0) {
                store.notify_jobs(written);
                spdlog::info("[worker] processed {} job(s), {} written", claimed.size(), written);
            }
        }

        if (purge_interval && cfg.retention.enabled()) {
            auto now = std::chrono::steady_clock::now();
            if (now - last_purge_check >= kPurgeCheckInterval) {
                last_purge_check = now;
                try {
                    int deleted = store.purge_done(*purge_interval, cfg.retention.purge_verbose);
                    if (deleted > 0) {
                        spdlog::info("[worker] purge sweep: {} rows deleted", deleted);
                    }
                } catch (const std::exception& e) {
                    spdlog::error("[worker] purge sweep failed: {}", e.what());
                }
            }
        }
    }

    spdlog::info("[worker] shutting down");
    return 0;
}
