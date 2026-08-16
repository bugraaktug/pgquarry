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

namespace pgquarry {

// One [[table]] entry: watches source.embed_column for INSERT/UPDATE and
// writes the resulting vector into target_table.target_column, keyed by
// target_id_column. target_table/target_id_column default to source/id_column
// (same-table write-back) when left unset in the TOML.
struct TableMapping
{
    std::string source;
    std::string embed_column;
    std::string id_column;
    std::string target_table;
    std::string target_column;
    std::string target_id_column;

    bool same_table() const { return target_table == source; }
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
    int                poll_interval_ms = 1000;
    RetentionConfig    retention;
    std::vector<TableMapping> tables;

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
        if (embedding.max_batch_size <= 0) errors.push_back("[worker] batch_size must be > 0");
        if (embedding.n_gpu_layers < 0)    errors.push_back("[worker] n_gpu_layers must be >= 0");
        if (poll_interval_ms <= 0)         errors.push_back("[worker] poll_interval_ms must be > 0");

        if (!parse_purge_after(retention.purge_after)) {
            errors.push_back("[retention] purge_after must be empty, '0', or a duration like "
                              "'7d', '12h', '30m', '45s' (got '" + retention.purge_after + "')");
        }

        if (tables.empty()) {
            errors.push_back("[[table]] at least one table mapping is required");
        }

        std::vector<std::string> seen_sources;
        for (size_t i = 0; i < tables.size(); ++i) {
            const auto& t = tables[i];
            const std::string ctx = "[table][" + std::to_string(i) + "]";

            if (t.source.empty())       errors.push_back(ctx + " source is required");
            if (t.embed_column.empty()) errors.push_back(ctx + " embed_column is required");
            if (t.id_column.empty())    errors.push_back(ctx + " id_column is required");
            if (t.target_column.empty()) errors.push_back(ctx + " target_column is required");

            if (!t.source.empty()) {
                if (std::find(seen_sources.begin(), seen_sources.end(), t.source) != seen_sources.end()) {
                    errors.push_back(ctx + " duplicate source '" + t.source + "' — pgquarry only "
                                      "supports one [[table]] mapping per source table in v1");
                } else {
                    seen_sources.push_back(t.source);
                }
            }
        }

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
    }

    if (auto* r = tbl["retention"].as_table()) {
        cfg.retention.purge_after   = str(r, "purge_after",   cfg.retention.purge_after);
        cfg.retention.purge_verbose = bl(r,  "purge_verbose", cfg.retention.purge_verbose);
    }

    if (auto* arr = tbl["table"].as_array()) {
        for (auto& elem : *arr) {
            if (auto* t = elem.as_table()) {
                TableMapping m;
                m.source            = str(t, "source",            m.source);
                m.embed_column      = str(t, "embed_column",      m.embed_column);
                m.id_column         = str(t, "id_column",         m.id_column);
                m.target_table      = str(t, "target_table",      m.source);       // default: same table
                m.target_column     = str(t, "target_column",     m.target_column);
                m.target_id_column  = str(t, "target_id_column",  m.id_column);    // default: same id column
                cfg.tables.push_back(std::move(m));
            }
        }
    }

    return cfg;
}

} // namespace pgquarry
