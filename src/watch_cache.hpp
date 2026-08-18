#pragma once

#include <string>
#include <unordered_map>

#include "config.hpp" // TableMapping

namespace pgquarry {

class JobStore;

// Owns the worker's in-memory mirror of pgquarry.watched_tables (the
// write-back mapping registered via watch()/watch_generate()) and the
// "refresh from db at most once per claimed batch" cache-miss policy —
// shared by every job-type dispatcher, since a miss in any one of them
// should only trigger one reload per batch.
class WatchCache
{
public:
    explicit WatchCache(JobStore& store);

    void load(); // throws — caller decides whether that's fatal

    // Call once at the top of each claimed batch, before any find() calls.
    void begin_batch();

    using Iterator = std::unordered_map<std::string, TableMapping>::iterator;
    Iterator find(const std::string& source, const std::string& job_type);
    Iterator end();

    size_t size() const;
    std::string describe() const;

private:
    JobStore& store_;
    std::unordered_map<std::string, TableMapping> by_key_;
    bool refreshed_this_batch_ = false;
};

} // namespace pgquarry
