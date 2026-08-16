#pragma once

#include <libpq-fe.h>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace pgquarry {

struct TableMapping;

// Claim/lease access to pgquarry.jobs, plus write-back into the user's own
// table via the pgquarry.watched_tables mapping pgquarry.watch() maintains.
// Pattern-inspired by walkrie's BackfillStore (claim/mark_done shape), but
// backed by Postgres itself rather than a local SQLite staging file — there's
// no separate store to go stale-claim-reset on restart: `processing` rows
// just sit there in v1.x (no lease timeout yet).
class JobStore
{
public:
    explicit JobStore(std::string conninfo);
    ~JobStore();

    JobStore(const JobStore&) = delete;
    JobStore& operator=(const JobStore&) = delete;

    void connect(); // throws std::runtime_error on failure

    // source_table/source_id/embed_column are unset for ad hoc jobs enqueued
    // via pgquarry.embed_async() — those have no watched source row.
    struct ClaimedJob
    {
        long long                 id;
        std::optional<std::string> source_table;
        std::optional<long long>   source_id;
        std::optional<std::string> embed_column;
        std::string                input_text;
    };

    // Atomic claim: UPDATE ... WHERE id IN (SELECT ... FOR UPDATE SKIP LOCKED)
    // RETURNING — one round trip, safe for multiple concurrent workers.
    std::vector<ClaimedJob> claim_pending(int limit);

    // Writes the embedding into the mapping's target_table/target_column —
    // UPDATE for same-table, UPSERT for a separate target_table. SQL text is
    // cached per source_table so it's built once, not per row.
    void write_back(const TableMapping& mapping, long long source_id, const std::vector<float>& embedding);

    void mark_done(long long id);
    void mark_error(long long id, const std::string& error);

    /// Ad hoc jobs (source_table IS NULL) write their result into the job row itself, not a user table.
    void write_ad_hoc_result(long long id, const std::vector<float>& embedding);

    /// Reads pgquarry.watched_tables, populated via pgquarry.watch() — the worker's write-back mapping source.
    std::vector<TableMapping> load_watched_tables();

    void notify_jobs(int count);

    // Deletes status='done' rows older than purge_after (a Postgres interval
    // literal, e.g. "7 days"). Logs id/status/created_at per row first when
    // verbose is true; returns the number of rows deleted.
    int purge_done(const std::string& purge_after_interval, bool verbose);

private:
    std::string conninfo_;
    PGconn* conn_ = nullptr;
    std::unordered_map<std::string, std::string> write_back_sql_cache_; // source_table -> SQL
};

} // namespace pgquarry
