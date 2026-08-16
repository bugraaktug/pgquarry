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

#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/spdlog.h>

#include "config.hpp"
#include "embedding_provider.hpp"
#include "job_store.hpp"

namespace
{

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

std::string join_sources(const std::unordered_map<std::string, pgquarry::TableMapping>& mapping)
{
    std::string joined;
    for (const auto& [source, _] : mapping) {
        if (!joined.empty()) joined += ", ";
        joined += source;
    }
    return joined.empty() ? "(none)" : joined;
}

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

    pgquarry::JobStore store(cfg.conninfo);
    std::unordered_map<std::string, pgquarry::TableMapping> mapping_by_source;
    try {
        store.connect();
        spdlog::debug("[worker] connected to database");
        /// watched_tables (populated via pgquarry.watch()) is the worker's write-back mapping source.
        for (auto& t : store.load_watched_tables()) mapping_by_source.emplace(t.source, std::move(t));
        spdlog::info("[worker] watching {} table(s): {}", mapping_by_source.size(), join_sources(mapping_by_source));
    } catch (const std::exception& e) {
        spdlog::error("[worker] {}", e.what());
        return 1;
    }

    std::optional<std::string> purge_interval = pgquarry::parse_purge_after(cfg.retention.purge_after);
    auto last_purge_check = std::chrono::steady_clock::now();

    spdlog::info("[worker] ready — provider={}, dimensions={}, batch_size={}, poll_interval_ms={}, tables={}",
                 provider->name(), provider->dimensions(), cfg.embedding.max_batch_size,
                 cfg.poll_interval_ms, mapping_by_source.size());

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
            bool mapping_refreshed = false; // reload watched_tables at most once per batch on a cache miss
            for (size_t i = 0; i < vectors.size(); ++i) {
                const auto& job = claimed[i];
                if (vectors[i].empty()) {
                    store.mark_error(job.id, "no embedding produced (see worker log for tokenization details)");
                    continue;
                }

                if (!job.source_table.has_value()) {
                    // Ad hoc job (pgquarry.embed_async()) — no source table, result goes into the job row itself.
                    try {
                        store.write_ad_hoc_result(job.id, vectors[i]);
                        ++written;
                        spdlog::debug("[worker] job {} (ad hoc) written to jobs.result", job.id);
                    } catch (const std::exception& e) {
                        spdlog::error("[worker] write_ad_hoc_result failed for job {}: {}", job.id, e.what());
                        store.mark_error(job.id, e.what());
                    }
                    continue;
                }

                auto it = mapping_by_source.find(*job.source_table);
                if (it == mapping_by_source.end() && !mapping_refreshed) {
                    // A watch() call after this worker started won't be in the in-memory map yet.
                    mapping_refreshed = true;
                    spdlog::info("[worker] mapping cache miss for '{}', refreshing watched_tables from db", *job.source_table);
                    try {
                        mapping_by_source.clear();
                        for (auto& t : store.load_watched_tables()) mapping_by_source.emplace(t.source, std::move(t));
                        spdlog::info("[worker] watching {} table(s) after refresh: {}",
                                     mapping_by_source.size(), join_sources(mapping_by_source));
                        it = mapping_by_source.find(*job.source_table);
                    } catch (const std::exception& e) {
                        spdlog::error("[worker] failed to refresh watched_tables mapping: {}", e.what());
                    }
                }
                if (it == mapping_by_source.end()) {
                    store.mark_error(job.id, "no watch registered for '" + *job.source_table +
                                              "' — was it removed since this job was enqueued?");
                    continue;
                }

                try {
                    store.write_back(it->second, *job.source_id, vectors[i]);
                    store.mark_done(job.id);
                    ++written;
                    spdlog::debug("[worker] job {} written to {}.{}", job.id, it->second.target_table, it->second.target_column);
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
