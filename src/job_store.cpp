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
        job.id = std::atoll(PQgetvalue(res, i, 0));
        if (!PQgetisnull(res, i, 1)) job.source_table  = PQgetvalue(res, i, 1);
        if (!PQgetisnull(res, i, 2)) job.source_id     = std::atoll(PQgetvalue(res, i, 2));
        if (!PQgetisnull(res, i, 3)) job.source_column = PQgetvalue(res, i, 3);
        job.input_text = PQgetvalue(res, i, 4);
        job.job_type   = job_type_from_string(PQgetvalue(res, i, 5));
        if (!PQgetisnull(res, i, 6)) job.max_tokens = std::atoi(PQgetvalue(res, i, 6));
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

// A source_table can carry both an 'embed' and a 'generate' mapping at once
// (pgquarry.watch() + pgquarry.watch_generate()), each needing its own
// cached write-back SQL — hence the composite key.
std::string cache_key(const TableMapping& m)
{
    return m.source + "\x1f" + m.job_type;
}
} // namespace

void JobStore::write_back(const TableMapping& mapping, long long source_id, const std::vector<float>& embedding)
{
    auto key = cache_key(mapping);
    auto it = write_back_sql_cache_.find(key);
    if (it == write_back_sql_cache_.end()) {
        std::string built = mapping.same_table()
            ? WritebackSqlBuilder::build_update_sql(mapping)
            : WritebackSqlBuilder::build_upsert_sql(mapping);
        it = write_back_sql_cache_.emplace(std::move(key), std::move(built)).first;
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

void JobStore::write_back_text(const TableMapping& mapping, long long source_id, const std::string& text)
{
    auto key = cache_key(mapping);
    auto it = write_back_sql_cache_.find(key);
    if (it == write_back_sql_cache_.end()) {
        std::string built = mapping.same_table()
            ? WritebackSqlBuilder::build_update_sql_text(mapping)
            : WritebackSqlBuilder::build_upsert_sql_text(mapping);
        it = write_back_sql_cache_.emplace(std::move(key), std::move(built)).first;
    }

    const std::string id_str = std::to_string(source_id);
    const char* params[2] = { text.c_str(), id_str.c_str() };

    PGresult* res = PQexecParams(conn_, it->second.c_str(), 2, nullptr, params, nullptr, nullptr, 0);
    bool ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
    if (!ok) {
        std::string err = PQerrorMessage(conn_);
        PQclear(res);
        throw std::runtime_error("JobStore: write_back_text failed for " + mapping.target_table + ": " + err);
    }
    PQclear(res);
}

void JobStore::write_ad_hoc_result(long long id, const std::vector<float>& embedding)
{
    const std::string vec_literal = vector_literal(embedding);
    const std::string id_str = std::to_string(id);
    const char* params[2] = { id_str.c_str(), vec_literal.c_str() };

    PGresult* res = PQexecParams(conn_, sql::kWriteAdHocResult, 2, nullptr, params, nullptr, nullptr, 0);
    bool ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
    if (!ok) {
        std::string err = PQerrorMessage(conn_);
        PQclear(res);
        throw std::runtime_error("JobStore: write_ad_hoc_result failed for job " + id_str + ": " + err);
    }
    PQclear(res);
}

void JobStore::write_ad_hoc_result_text(long long id, const std::string& text)
{
    const std::string id_str = std::to_string(id);
    const char* params[2] = { id_str.c_str(), text.c_str() };

    PGresult* res = PQexecParams(conn_, sql::kWriteAdHocResultText, 2, nullptr, params, nullptr, nullptr, 0);
    bool ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
    if (!ok) {
        std::string err = PQerrorMessage(conn_);
        PQclear(res);
        throw std::runtime_error("JobStore: write_ad_hoc_result_text failed for job " + id_str + ": " + err);
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

std::vector<TableMapping> JobStore::load_watched_tables()
{
    PGresult* res = PQexecParams(conn_, sql::kSelectWatchedTables, 0, nullptr, nullptr, nullptr, nullptr, 0);
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        std::string err = PQerrorMessage(conn_);
        PQclear(res);
        throw std::runtime_error("JobStore: load_watched_tables failed: " + err);
    }

    std::vector<TableMapping> tables;
    int n = PQntuples(res);
    tables.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        TableMapping m;
        m.source           = PQgetvalue(res, i, 0);
        m.job_type         = PQgetvalue(res, i, 1);
        m.id_column        = PQgetvalue(res, i, 2);
        m.source_column     = PQgetvalue(res, i, 3);
        m.target_table      = PQgetvalue(res, i, 4);
        m.target_column     = PQgetvalue(res, i, 5);
        m.target_id_column  = PQgetvalue(res, i, 6);
        tables.push_back(std::move(m));
    }
    PQclear(res);
    return tables;
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
