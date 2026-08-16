#include <chrono>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/spdlog.h>

#include "config.hpp"
#include "embedding_provider.hpp"
#include "job_store.hpp"

namespace
{

// v1: still a static path — log_file/log_level/rotation config is not part
// of pgquarry.toml yet (see walkrie's init_logger() for the TOML-driven
// version this could grow into later).
constexpr const char* kLogPath = "/tmp/pgquarry/worker.log";

void init_logger()
{
    try {
        std::filesystem::create_directories(std::filesystem::path(kLogPath).parent_path());
        auto file_logger = spdlog::basic_logger_mt("pgquarry", kLogPath);
        spdlog::set_default_logger(file_logger);
        spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
        spdlog::flush_every(std::chrono::seconds(3));
        spdlog::info("[worker] logger initialized — file={}", kLogPath);
    } catch (const spdlog::spdlog_ex& e) {
        std::cerr << "logger initialization failed: " << e.what() << "\n";
    }
}

void print_usage(const char* argv0)
{
    std::cerr <<
        "Usage: " << argv0 << " --config <pgquarry.toml path>\n"
        "\n"
        "See pgquarry.toml.example for the config file format ([worker], "
        "[retention], and one or more [[table]] entries).\n";
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
    init_logger();

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

    pgquarry::AppConfig cfg;
    try {
        cfg = pgquarry::load_config(config_path);
    } catch (const std::exception& e) {
        spdlog::error("[worker] {}", e.what());
        return 1;
    }

    auto errors = cfg.validate();
    if (!errors.empty()) {
        for (const auto& e : errors) spdlog::error("[worker] {}", e);
        return 1;
    }

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    std::shared_ptr<pgquarry::EmbeddingProvider> provider;
    try {
        provider = pgquarry::make_embedding_provider(cfg.embedding);
        provider->init();
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

    pgquarry::JobStore store(cfg.conninfo);
    try {
        store.connect();
        store.sync_watched_tables(cfg.tables);
    } catch (const std::exception& e) {
        spdlog::error("[worker] {}", e.what());
        return 1;
    }

    std::unordered_map<std::string, pgquarry::TableMapping> mapping_by_source;
    for (const auto& t : cfg.tables) mapping_by_source.emplace(t.source, t);

    std::optional<std::string> purge_interval = pgquarry::parse_purge_after(cfg.retention.purge_after);
    auto last_purge_check = std::chrono::steady_clock::now();

    spdlog::info("[worker] ready — provider={}, dimensions={}, batch_size={}, poll_interval_ms={}, tables={}",
                 provider->name(), provider->dimensions(), cfg.embedding.max_batch_size,
                 cfg.poll_interval_ms, cfg.tables.size());

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
            std::vector<std::string> texts;
            texts.reserve(claimed.size());
            for (const auto& job : claimed) texts.push_back(job.input_text);

            std::vector<std::vector<float>> vectors;
            try {
                vectors = provider->embed_batch(texts);
            } catch (const std::exception& e) {
                spdlog::error("[worker] embed_batch failed for a batch of {}: {}", claimed.size(), e.what());
                for (const auto& job : claimed) store.mark_error(job.id, e.what());
                vectors.clear();
            }

            if (!vectors.empty() && vectors.size() != claimed.size()) {
                spdlog::error("[worker] embed_batch returned {} vectors for {} jobs — marking batch as error",
                              vectors.size(), claimed.size());
                for (const auto& job : claimed) store.mark_error(job.id, "embed_batch size mismatch");
                vectors.clear();
            }

            int written = 0;
            for (size_t i = 0; i < vectors.size(); ++i) {
                const auto& job = claimed[i];
                if (vectors[i].empty()) {
                    store.mark_error(job.id, "no embedding produced (see worker log for tokenization details)");
                    continue;
                }

                auto it = mapping_by_source.find(job.source_table);
                if (it == mapping_by_source.end()) {
                    store.mark_error(job.id, "no [[table]] mapping for '" + job.source_table +
                                              "' in pgquarry.toml — was it removed since this job was enqueued?");
                    continue;
                }

                try {
                    store.write_back(it->second, job.source_id, vectors[i]);
                    store.mark_done(job.id);
                    ++written;
                } catch (const std::exception& e) {
                    spdlog::error("[worker] write_back failed for job {}: {}", job.id, e.what());
                    store.mark_error(job.id, e.what());
                }
            }

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
