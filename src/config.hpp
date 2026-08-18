#pragma once

#include <algorithm>
#include <filesystem>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

#include <toml.hpp>

#include "embedding_config.hpp"
#include "generation_config.hpp"

namespace pgquarry {

/// One pgquarry.watched_tables row, registered via pgquarry.watch()/watch_generate(): watches
/// source.source_column for INSERT/UPDATE, writes the result (vector for job_type='embed', text for
/// 'generate') into target_table.target_column keyed by target_id_column.
struct TableMapping
{
    std::string source;
    std::string job_type;
    std::string source_column;
    std::string id_column;
    std::string target_table;
    std::string target_column;
    std::string target_id_column;

    bool same_table() const { return target_table == source; }
};

struct LoggingConfig
{
    std::string log_file      = "/tmp/pgquarry/worker.log";
    std::string log_level     = "info"; // trace|debug|info|warn|error|critical|off
    int         max_size_mb   = 10;     // rotate once the active file crosses this size
    int         max_files     = 3;      // rotated files kept, oldest deleted beyond this
};

struct RetentionConfig
{
    std::string purge_after;         // raw TOML value, e.g. "7d", "0", or unset
    bool        purge_verbose = false;

    // Empty/"0" disables purging entirely.
    bool enabled() const { return !purge_after.empty() && purge_after != "0"; }
};

// Parses a duration like "7d" / "12h" / "30m" / "45s" into a Postgres
// interval literal ("7 days" / "12 hours" / ...). Returns std::nullopt if
// the string doesn't match — caller turns that into a validate() error.
inline std::optional<std::string> parse_purge_after(const std::string& raw)
{
    if (raw.empty() || raw == "0") return "0 seconds";

    if (raw.size() < 2) return std::nullopt;
    char unit = raw.back();
    std::string digits = raw.substr(0, raw.size() - 1);
    if (digits.empty() || !std::all_of(digits.begin(), digits.end(), [](unsigned char c) { return std::isdigit(c); })) {
        return std::nullopt;
    }

    const char* unit_name = nullptr;
    switch (unit) {
        case 's': unit_name = "seconds"; break;
        case 'm': unit_name = "minutes"; break;
        case 'h': unit_name = "hours";   break;
        case 'd': unit_name = "days";    break;
        default: return std::nullopt;
    }

    return digits + " " + unit_name;
}

struct AppConfig
{
    std::string       conninfo;
    EmbeddingConfig    embedding;
    GenerationConfig   generation;
    int                poll_interval_ms = 1000;
    RetentionConfig    retention;
    LoggingConfig      logging;

    std::vector<std::string> validate() const
    {
        std::vector<std::string> errors;

        if (conninfo.empty()) {
            errors.push_back("[worker] conninfo is required");
        }

        if (embedding.model_path.empty()) {
            errors.push_back("[worker] model_path is required");
        } else {
            namespace fs = std::filesystem;
            std::error_code ec;
            const std::string& path = embedding.model_path;
            if (!fs::exists(path, ec)) {
                errors.push_back("[worker] model_path does not exist: '" + path + "'");
            } else if (!fs::is_regular_file(path, ec)) {
                errors.push_back("[worker] model_path exists but is not a regular file: '" + path + "'");
            } else if (access(path.c_str(), R_OK) != 0) {
                errors.push_back("[worker] model_path exists but is not readable by the current user: '" + path + "'");
            } else if (fs::file_size(path, ec) == 0) {
                errors.push_back("[worker] model_path points to an empty (0-byte) file: '" + path + "'");
            }
        }

        if (embedding.dimensions <= 0)     errors.push_back("[worker] dimensions must be > 0");
        if (embedding.n_threads <= 0)      errors.push_back("[worker] n_threads must be > 0");
        if (embedding.n_ctx <= 0)          errors.push_back("[worker] n_ctx must be > 0");
        // generation_model_path is OPTIONAL (unlike model_path above) — only validated if set.
        if (!generation.model_path.empty()) {
            namespace fs = std::filesystem;
            std::error_code ec;
            const std::string& path = generation.model_path;
            if (!fs::exists(path, ec)) {
                errors.push_back("[worker] generation_model_path does not exist: '" + path + "'");
            } else if (!fs::is_regular_file(path, ec)) {
                errors.push_back("[worker] generation_model_path exists but is not a regular file: '" + path + "'");
            } else if (access(path.c_str(), R_OK) != 0) {
                errors.push_back("[worker] generation_model_path exists but is not readable by the current user: '" + path + "'");
            } else if (fs::file_size(path, ec) == 0) {
                errors.push_back("[worker] generation_model_path points to an empty (0-byte) file: '" + path + "'");
            }
        }
        if (generation.max_tokens <= 0)    errors.push_back("[worker] generation_max_tokens must be > 0");
        if (generation.n_ctx <= 0)         errors.push_back("[worker] generation_n_ctx must be > 0");
        if (generation.n_threads <= 0)     errors.push_back("[worker] generation_n_threads must be > 0");
        if (generation.n_gpu_layers < 0)   errors.push_back("[worker] generation_n_gpu_layers must be >= 0");
        if (embedding.max_batch_size <= 0) errors.push_back("[worker] batch_size must be > 0");
        if (embedding.n_gpu_layers < 0)    errors.push_back("[worker] n_gpu_layers must be >= 0");
        if (poll_interval_ms <= 0)         errors.push_back("[worker] poll_interval_ms must be > 0");

        if (!parse_purge_after(retention.purge_after)) {
            errors.push_back("[retention] purge_after must be empty, '0', or a duration like "
                              "'7d', '12h', '30m', '45s' (got '" + retention.purge_after + "')");
        }

        static const std::vector<std::string> kLogLevels =
            { "trace", "debug", "info", "warn", "error", "critical", "off" };
        if (std::find(kLogLevels.begin(), kLogLevels.end(), logging.log_level) == kLogLevels.end()) {
            errors.push_back("[logging] log_level must be one of trace|debug|info|warn|error|critical|off "
                              "(got '" + logging.log_level + "')");
        }
        if (logging.max_size_mb <= 0) errors.push_back("[logging] max_size_mb must be > 0");
        if (logging.max_files <= 0)   errors.push_back("[logging] max_files must be > 0");

        return errors;
    }
};

inline AppConfig load_config(const std::string& path)
{
    toml::table tbl;
    try {
        tbl = toml::parse_file(path);
    } catch (const toml::parse_error& e) {
        throw std::runtime_error(std::string("config parse error in '") + path + "': " + e.what());
    }

    AppConfig cfg;

    auto str = [](const toml::table* t, const char* key, const std::string& def) -> std::string {
        if (!t) return def;
        auto v = t->get_as<std::string>(key);
        return v ? **v : def;
    };
    auto i32 = [](const toml::table* t, const char* key, int def) -> int {
        if (!t) return def;
        auto v = t->get_as<int64_t>(key);
        return v ? static_cast<int>(**v) : def;
    };
    auto bl = [](const toml::table* t, const char* key, bool def) -> bool {
        if (!t) return def;
        auto v = t->get_as<bool>(key);
        return v ? **v : def;
    };

    if (auto* w = tbl["worker"].as_table()) {
        cfg.conninfo                 = str(w, "conninfo",        cfg.conninfo);
        cfg.embedding.model_path     = str(w, "model_path",      cfg.embedding.model_path);
        cfg.embedding.dimensions     = i32(w, "dimensions",      cfg.embedding.dimensions);
        cfg.embedding.n_threads      = i32(w, "n_threads",       cfg.embedding.n_threads);
        cfg.embedding.n_ctx          = i32(w, "n_ctx",           cfg.embedding.n_ctx);
        cfg.embedding.max_batch_size = i32(w, "batch_size",      cfg.embedding.max_batch_size);
        cfg.embedding.n_gpu_layers   = i32(w, "n_gpu_layers",    cfg.embedding.n_gpu_layers);
        cfg.poll_interval_ms         = i32(w, "poll_interval_ms", cfg.poll_interval_ms);

        cfg.generation.model_path    = str(w, "generation_model_path",    cfg.generation.model_path);
        cfg.generation.max_tokens    = i32(w, "generation_max_tokens",    cfg.generation.max_tokens);
        cfg.generation.n_ctx         = i32(w, "generation_n_ctx",         cfg.generation.n_ctx);
        cfg.generation.n_threads     = i32(w, "generation_n_threads",     cfg.generation.n_threads);
        cfg.generation.n_gpu_layers  = i32(w, "generation_n_gpu_layers",  cfg.generation.n_gpu_layers);
    }

    if (auto* r = tbl["retention"].as_table()) {
        cfg.retention.purge_after   = str(r, "purge_after",   cfg.retention.purge_after);
        cfg.retention.purge_verbose = bl(r,  "purge_verbose", cfg.retention.purge_verbose);
    }

    if (auto* l = tbl["logging"].as_table()) {
        cfg.logging.log_file    = str(l, "log_file",    cfg.logging.log_file);
        cfg.logging.log_level   = str(l, "log_level",   cfg.logging.log_level);
        cfg.logging.max_size_mb = i32(l, "max_size_mb", cfg.logging.max_size_mb);
        cfg.logging.max_files   = i32(l, "max_files",   cfg.logging.max_files);
    }

    return cfg;
}

} // namespace pgquarry
