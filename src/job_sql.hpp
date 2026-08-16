#pragma once

namespace pgquarry::sql {

// Atomic claim: SKIP LOCKED avoids blocking on rows other workers already
// hold; the outer UPDATE+RETURNING makes claim-and-mark-processing one
// round trip instead of two racing statements.
inline constexpr const char* kClaimPending =
    "UPDATE pgquarry.jobs SET status = 'processing', updated_at = now() "
    "WHERE id IN ("
    "  SELECT id FROM pgquarry.jobs WHERE status = 'pending' "
    "  ORDER BY id FOR UPDATE SKIP LOCKED LIMIT $1"
    ") "
    "RETURNING id, source_table, source_id, embed_column, input_text";

// v1: no embedding param — the vector goes to the user's table via
// WritebackSqlBuilder, not into pgquarry.jobs. status='done' here just
// marks the audit-trail row complete.
inline constexpr const char* kMarkDone =
    "UPDATE pgquarry.jobs SET status = 'done', error = NULL, updated_at = now() "
    "WHERE id = $1";

inline constexpr const char* kMarkError =
    "UPDATE pgquarry.jobs SET status = 'error', error = $2, updated_at = now() "
    "WHERE id = $1";

inline constexpr const char* kNotify =
    "SELECT pg_notify('pgquarry_jobs', $1)";

// Read side of pgquarry.watched_tables — the worker's write-back mapping is
// built from this, not from pgquarry.toml alone, so a table registered via
// pgquarry.watch() (SQL-side, no toml entry) works without a worker restart.
inline constexpr const char* kSelectWatchedTables =
    "SELECT source_table, id_column, embed_column, target_table, target_column, target_id_column "
    "FROM pgquarry.watched_tables";

// Ad hoc jobs (source_table IS NULL, enqueued via pgquarry.embed_async())
// have no user table to write back into — the vector lands in the job row
// itself for pgquarry.embed_sync() to read back out.
inline constexpr const char* kWriteAdHocResult =
    "UPDATE pgquarry.jobs SET status = 'done', error = NULL, result = $2::vector, updated_at = now() "
    "WHERE id = $1";

// purge_verbose logging needs id/status/created_at for every row about to
// be deleted, so the worker SELECTs before it DELETEs when that's on.
inline constexpr const char* kSelectPurgeCandidates =
    "SELECT id, status, created_at FROM pgquarry.jobs "
    "WHERE status = 'done' AND updated_at < now() - $1::interval";

inline constexpr const char* kPurgeDone =
    "DELETE FROM pgquarry.jobs WHERE status = 'done' AND updated_at < now() - $1::interval";

} // namespace pgquarry::sql
