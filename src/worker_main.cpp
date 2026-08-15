#include <chrono>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/spdlog.h>

#include "embedding_config.hpp"
#include "embedding_provider.hpp"
#include "job_store.hpp"

namespace
{

// v0: static path, no config file to read log_file/log_level/rotation from
// yet — see walkrie's init_logger() in main.cpp for the TOML-driven version
// this will grow into at v1.
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

struct WorkerArgs
{
    std::string conninfo;
    pgquarry::EmbeddingConfig embedding;
    int poll_interval_ms = 1000;
};

void print_usage(const char* argv0)
{
    std::cerr <<
        "Usage: " << argv0 << " --conninfo <pg conninfo> --model-path <gguf path> [options]\n"
        "\n"
        "Required:\n"
        "  --conninfo <str>          libpq connection string, e.g. 'host=localhost dbname=pgquarry'\n"
        "  --model-path <path>       path to a GGUF embedding model\n"
        "\n"
        "Options:\n"
        "  --dimensions <int>        expected embedding width (default 1024)\n"
        "  --n-threads <int>         llama.cpp CPU threads (default 4)\n"
        "  --n-ctx <int>             llama.cpp context window (default 512)\n"
        "  --batch-size <int>        jobs claimed per poll (default 8)\n"
        "  --n-gpu-layers <int>      llama.cpp GPU layers (default 0)\n"
        "  --poll-interval-ms <int>  sleep between polls when no jobs are pending (default 1000)\n";
}

// (walkrie's AppConfig::validate() style).
std::vector<std::string> validate(const WorkerArgs& args)
{
    std::vector<std::string> errors;

    if (args.conninfo.empty()) {
        errors.push_back("--conninfo is required");
    }

    if (args.embedding.model_path.empty()) {
        errors.push_back("--model-path is required");
    } else {
        namespace fs = std::filesystem;
        std::error_code ec;
        const std::string& path = args.embedding.model_path;
        if (!fs::exists(path, ec)) {
            errors.push_back("--model-path does not exist: '" + path + "'");
        } else if (!fs::is_regular_file(path, ec)) {
            errors.push_back("--model-path exists but is not a regular file: '" + path + "'");
        } else if (access(path.c_str(), R_OK) != 0) {
            errors.push_back("--model-path exists but is not readable by the current user: '" + path + "'");
        } else if (fs::file_size(path, ec) == 0) {
            errors.push_back("--model-path points to an empty (0-byte) file: '" + path + "'");
        }
    }

    if (args.embedding.dimensions <= 0) {
        errors.push_back("--dimensions must be > 0");
    }
    if (args.embedding.n_threads <= 0) {
        errors.push_back("--n-threads must be > 0");
    }
    if (args.embedding.n_ctx <= 0) {
        errors.push_back("--n-ctx must be > 0");
    }
    if (args.embedding.max_batch_size <= 0) {
        errors.push_back("--batch-size must be > 0");
    }
    if (args.embedding.n_gpu_layers < 0) {
        errors.push_back("--n-gpu-layers must be >= 0");
    }
    if (args.poll_interval_ms <= 0) {
        errors.push_back("--poll-interval-ms must be > 0");
    }

    return errors;
}

enum class ParseResult { Ok, Help, Error };

ParseResult parse_args(int argc, char** argv, WorkerArgs& args)
{
    auto next = [&](int& i) -> std::string {
        if (i + 1 >= argc) throw std::runtime_error(std::string("missing value for ") + argv[i]);
        return argv[++i];
    };

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        try {
            if (a == "--conninfo")          args.conninfo = next(i);
            else if (a == "--model-path")   args.embedding.model_path = next(i);
            else if (a == "--dimensions")   args.embedding.dimensions = std::stoi(next(i));
            else if (a == "--n-threads")    args.embedding.n_threads = std::stoi(next(i));
            else if (a == "--n-ctx")        args.embedding.n_ctx = std::stoi(next(i));
            else if (a == "--batch-size")   args.embedding.max_batch_size = std::stoi(next(i));
            else if (a == "--n-gpu-layers") args.embedding.n_gpu_layers = std::stoi(next(i));
            else if (a == "--poll-interval-ms") args.poll_interval_ms = std::stoi(next(i));
            else if (a == "--help" || a == "-h") { print_usage(argv[0]); return ParseResult::Help; }
            else {
                std::cerr << "unknown argument: " << a << "\n";
                print_usage(argv[0]);
                return ParseResult::Error;
            }
        } catch (const std::exception& e) {
            std::cerr << "error parsing " << a << ": " << e.what() << "\n";
            return ParseResult::Error;
        }
    }
    return ParseResult::Ok;
}

volatile std::sig_atomic_t g_shutdown = 0;
void handle_signal(int) { g_shutdown = 1; }

} // namespace

int main(int argc, char** argv)
{
    init_logger();

    WorkerArgs args;
    switch (parse_args(argc, argv, args)) {
        case ParseResult::Help:  return 0;
        case ParseResult::Error: return 1;
        case ParseResult::Ok:    break;
    }

    auto errors = validate(args);
    if (!errors.empty()) {
        for (const auto& e : errors) spdlog::error("[worker] {}", e);
        print_usage(argv[0]);
        return 1;
    }

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    std::shared_ptr<pgquarry::EmbeddingProvider> provider;
    try {
        provider = pgquarry::make_embedding_provider(args.embedding);
        provider->init();
    } catch (const std::exception& e) {
        spdlog::error("[worker] failed to initialize embedding provider: {}", e.what());
        return 1;
    }

    if (provider->dimensions() != args.embedding.dimensions) {
        spdlog::error("[worker] model produces {}-dimensional vectors but --dimensions was {} — "
                       "fix --dimensions or your pgquarry.jobs.embedding column width",
                       provider->dimensions(), args.embedding.dimensions);
        return 1;
    }

    pgquarry::JobStore store(args.conninfo);
    try {
        store.connect();
    } catch (const std::exception& e) {
        spdlog::error("[worker] {}", e.what());
        return 1;
    }

    spdlog::info("[worker] ready — provider={}, dimensions={}, batch_size={}, poll_interval_ms={}",
                 provider->name(), provider->dimensions(), args.embedding.max_batch_size, args.poll_interval_ms);

    while (!g_shutdown) {
        std::vector<pgquarry::JobStore::ClaimedJob> claimed;
        try {
            claimed = store.claim_pending(args.embedding.max_batch_size);
        } catch (const std::exception& e) {
            spdlog::error("[worker] claim_pending failed: {}", e.what());
            std::this_thread::sleep_for(std::chrono::milliseconds(args.poll_interval_ms));
            continue;
        }

        if (claimed.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(args.poll_interval_ms));
            continue;
        }

        std::vector<std::string> texts;
        texts.reserve(claimed.size());
        for (const auto& job : claimed) texts.push_back(job.input_text);

        std::vector<std::vector<float>> vectors;
        try {
            vectors = provider->embed_batch(texts);
        } catch (const std::exception& e) {
            spdlog::error("[worker] embed_batch failed for a batch of {}: {}", claimed.size(), e.what());
            for (const auto& job : claimed) store.mark_error(job.id, e.what());
            continue;
        }

        if (vectors.size() != claimed.size()) {
            spdlog::error("[worker] embed_batch returned {} vectors for {} jobs — marking batch as error",
                          vectors.size(), claimed.size());
            for (const auto& job : claimed) store.mark_error(job.id, "embed_batch size mismatch");
            continue;
        }

        for (size_t i = 0; i < claimed.size(); ++i) {
            if (vectors[i].empty()) {
                store.mark_error(claimed[i].id, "no embedding produced (see worker log for tokenization details)");
            } else {
                store.mark_done(claimed[i].id, vectors[i]);
            }
        }

        spdlog::info("[worker] processed {} job(s)", claimed.size());
    }

    spdlog::info("[worker] shutting down");
    return 0;
}
