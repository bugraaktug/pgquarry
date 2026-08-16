#include "job_store.hpp"

#include "config.hpp"
#include "job_sql.hpp"
#include "writeback_sql_builder.hpp"

#include <spdlog/spdlog.h>
#include <sstream>
#include <stdexcept>

namespace pgquarry {

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
        job.id           = std::atoll(PQgetvalue(res, i, 0));
        job.source_table = PQgetvalue(res, i, 1);
        job.source_id    = std::atoll(PQgetvalue(res, i, 2));
        job.embed_column = PQgetvalue(res, i, 3);
        job.input_text   = PQgetvalue(res, i, 4);
        claimed.push_back(std::move(job));
    }
    PQclear(res);
    return claimed;
}

namespace {
std::string vector_literal(const std::vector<float>& embedding)
{
    std::ostringstream vec_str;
    vec_str << "[";
    for (size_t i = 0; i < embedding.size(); ++i) {
        if (i > 0) vec_str << ",";
        vec_str << embedding[i];
    }
    vec_str << "]";
    return vec_str.str();
}
} // namespace

void JobStore::write_back(const TableMapping& mapping, long long source_id, const std::vector<float>& embedding)
{
    auto it = write_back_sql_cache_.find(mapping.source);
    if (it == write_back_sql_cache_.end()) {
        std::string built = mapping.same_table()
            ? WritebackSqlBuilder::build_update_sql(mapping)
            : WritebackSqlBuilder::build_upsert_sql(mapping);
        it = write_back_sql_cache_.emplace(mapping.source, std::move(built)).first;
    }

    const std::string vec_literal = vector_literal(embedding);
    const std::string id_str = std::to_string(source_id);
    const char* params[2] = { vec_literal.c_str(), id_str.c_str() };

    PGresult* res = PQexecParams(conn_, it->second.c_str(), 2, nullptr, params, nullptr, nullptr, 0);
    bool ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
    if (!ok) {
        std::string err = PQerrorMessage(conn_);
        PQclear(res);
        throw std::runtime_error("JobStore: write_back failed for " + mapping.target_table + ": " + err);
    }
    PQclear(res);
}

void JobStore::mark_done(long long id)
{
    const std::string id_str = std::to_string(id);
    const char* params[1] = { id_str.c_str() };
    PGresult* res = PQexecParams(conn_, sql::kMarkDone, 1, nullptr, params, nullptr, nullptr, 0);
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

void JobStore::sync_watched_tables(const std::vector<TableMapping>& tables)
{
    for (const auto& m : tables) {
        const char* params[6] = {
            m.source.c_str(), m.id_column.c_str(), m.embed_column.c_str(),
            m.target_table.c_str(), m.target_column.c_str(), m.target_id_column.c_str(),
        };
        PGresult* res = PQexecParams(conn_, sql::kUpsertWatchedTable, 6, nullptr, params, nullptr, nullptr, 0);
        bool ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
        if (!ok) {
            std::string err = PQerrorMessage(conn_);
            PQclear(res);
            throw std::runtime_error("JobStore: sync_watched_tables failed for '" + m.source + "': " + err);
        }
        PQclear(res);
    }
}

void JobStore::notify_jobs(int count)
{
    const std::string count_str = std::to_string(count);
    const char* params[1] = { count_str.c_str() };
    PGresult* res = PQexecParams(conn_, sql::kNotify, 1, nullptr, params, nullptr, nullptr, 0);
    bool ok = (PQresultStatus(res) == PGRES_TUPLES_OK);
    if (!ok) {
        spdlog::error("[JobStore] notify_jobs failed: {}", PQerrorMessage(conn_));
    }
    PQclear(res);
}

int JobStore::purge_done(const std::string& purge_after_interval, bool verbose)
{
    const char* params[1] = { purge_after_interval.c_str() };

    if (verbose) {
        PGresult* sel = PQexecParams(conn_, sql::kSelectPurgeCandidates, 1, nullptr, params, nullptr, nullptr, 0);
        if (PQresultStatus(sel) != PGRES_TUPLES_OK) {
            std::string err = PQerrorMessage(conn_);
            PQclear(sel);
            throw std::runtime_error("JobStore: purge_done candidate select failed: " + err);
        }
        int n = PQntuples(sel);
        for (int i = 0; i < n; ++i) {
            spdlog::info("[pgquarry_worker] purge id={} status={} created_at={}",
                         PQgetvalue(sel, i, 0), PQgetvalue(sel, i, 1), PQgetvalue(sel, i, 2));
        }
        PQclear(sel);
    }

    PGresult* res = PQexecParams(conn_, sql::kPurgeDone, 1, nullptr, params, nullptr, nullptr, 0);
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        std::string err = PQerrorMessage(conn_);
        PQclear(res);
        throw std::runtime_error("JobStore: purge_done failed: " + err);
    }
    int deleted = std::atoi(PQcmdTuples(res));
    PQclear(res);
    return deleted;
}

} // namespace pgquarry
