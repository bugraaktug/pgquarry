#pragma once

#include <libpq-fe.h>
#include <string>
#include <vector>

namespace pgquarry 
{

// Claim/lease access to pgquarry.jobs. Pattern-inspired by walkrie's
// BackfillStore (claim/mark_done shape), but backed by Postgres itself
// rather than a local SQLite staging file — there's no separate store to
// go stale-claim-reset on restart: `processing` rows just sit there in v0
// (no lease timeout yet; see README's v1 roadmap for retention/cleanup).
class JobStore
{
public:
    explicit JobStore(std::string conninfo);
    ~JobStore();

    JobStore(const JobStore&) = delete;
    JobStore& operator=(const JobStore&) = delete;

    void connect(); // throws std::runtime_error on failure

    struct ClaimedJob
    {
        long long   id;
        std::string input_text;
    };

    // Atomic claim: UPDATE ... WHERE id IN (SELECT ... FOR UPDATE SKIP LOCKED)
    // RETURNING — one round trip, safe for multiple concurrent workers.
    std::vector<ClaimedJob> claim_pending(int limit);

    void mark_done(long long id, const std::vector<float>& embedding);
    void mark_error(long long id, const std::string& error);

private:
    std::string conninfo_;
    PGconn* conn_ = nullptr;
};

} // namespace pgquarry
