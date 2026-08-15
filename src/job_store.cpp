#include "job_store.hpp"

#include "job_sql.hpp"

#include <spdlog/spdlog.h>
#include <sstream>
#include <stdexcept>

namespace pgquarry 
{

JobStore::JobStore(std::string conninfo) : conninfo_(std::move(conninfo)) {}

JobStore::~JobStore()
{
    if (conn_) {
        PQfinish(conn_);
        conn_ = nullptr;
    }
}

void JobStore::connect()
{
    conn_ = PQconnectdb(conninfo_.c_str());
    if (PQstatus(conn_) != CONNECTION_OK) {
        std::string err = PQerrorMessage(conn_);
        PQfinish(conn_);
        conn_ = nullptr;
        throw std::runtime_error("JobStore: connection failed: " + err);
    }
}

std::vector<JobStore::ClaimedJob> JobStore::claim_pending(int limit)
{
    const std::string limit_str = std::to_string(limit);
    const char* params[1] = { limit_str.c_str() };

    PGresult* res = PQexecParams(conn_, sql::kClaimPending, 1, nullptr, params, nullptr, nullptr, 0);
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        std::string err = PQerrorMessage(conn_);
        PQclear(res);
        throw std::runtime_error("JobStore: claim_pending failed: " + err);
    }

    std::vector<ClaimedJob> claimed;
    int n = PQntuples(res);
    claimed.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        ClaimedJob job;
        job.id         = std::atoll(PQgetvalue(res, i, 0));
        job.input_text = PQgetvalue(res, i, 1);
        claimed.push_back(std::move(job));
    }
    PQclear(res);
    return claimed;
}

void JobStore::mark_done(long long id, const std::vector<float>& embedding)
{
    std::ostringstream vec_str;
    vec_str << "[";
    for (size_t i = 0; i < embedding.size(); ++i) {
        if (i > 0) vec_str << ",";
        vec_str << embedding[i];
    }
    vec_str << "]";
    const std::string vec_literal = vec_str.str();
    const std::string id_str = std::to_string(id);

    const char* params[2] = { id_str.c_str(), vec_literal.c_str() };
    PGresult* res = PQexecParams(conn_, sql::kMarkDone, 2, nullptr, params, nullptr, nullptr, 0);
    bool ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
    if (!ok) {
        spdlog::error("[JobStore] mark_done failed id={}: {}", id, PQerrorMessage(conn_));
    }
    PQclear(res);
}

void JobStore::mark_error(long long id, const std::string& error)
{
    const std::string id_str = std::to_string(id);
    const char* params[2] = { id_str.c_str(), error.c_str() };
    PGresult* res = PQexecParams(conn_, sql::kMarkError, 2, nullptr, params, nullptr, nullptr, 0);
    bool ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
    if (!ok) {
        spdlog::error("[JobStore] mark_error failed id={}: {}", id, PQerrorMessage(conn_));
    }
    PQclear(res);
}

} // namespace pgquarry
