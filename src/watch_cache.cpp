#include "watch_cache.hpp"

#include "job_store.hpp"

#include <spdlog/spdlog.h>

namespace pgquarry {

namespace {
// A source_table can carry both an 'embed' and a 'generate' watch at once
// (pgquarry.watch() + pgquarry.watch_generate()), so entries are keyed by
// (source, job_type), not source alone.
std::string mapping_key(const std::string& source, const std::string& job_type)
{
    return source + "\x1f" + job_type;
}
} // namespace

WatchCache::WatchCache(JobStore& store) : store_(store) {}

void WatchCache::load()
{
    by_key_.clear();
    for (auto& t : store_.load_watched_tables()) {
        auto key = mapping_key(t.source, t.job_type);
        by_key_.emplace(std::move(key), std::move(t));
    }
}

void WatchCache::begin_batch()
{
    refreshed_this_batch_ = false;
}

WatchCache::Iterator WatchCache::find(const std::string& source, const std::string& job_type)
{
    auto it = by_key_.find(mapping_key(source, job_type));
    if (it == by_key_.end() && !refreshed_this_batch_) {
        // A watch()/watch_generate() call after this worker started (or after
        // this batch's mappings were last loaded) won't be in the cache yet.
        refreshed_this_batch_ = true;
        spdlog::info("[worker] mapping cache miss for '{}:{}', refreshing watched_tables from db", source, job_type);
        try {
            load();
            spdlog::info("[worker] watching {} mapping(s) after refresh: {}", size(), describe());
            it = by_key_.find(mapping_key(source, job_type));
        } catch (const std::exception& e) {
            spdlog::error("[worker] failed to refresh watched_tables mapping: {}", e.what());
        }
    }
    return it;
}

WatchCache::Iterator WatchCache::end()
{
    return by_key_.end();
}

size_t WatchCache::size() const
{
    return by_key_.size();
}

std::string WatchCache::describe() const
{
    std::string joined;
    for (const auto& [key, m] : by_key_) {
        (void)key;
        if (!joined.empty()) joined += ", ";
        joined += m.source + ":" + m.job_type;
    }
    return joined.empty() ? "(none)" : joined;
}

} // namespace pgquarry
