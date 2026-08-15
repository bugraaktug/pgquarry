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
    "RETURNING id, input_text";

inline constexpr const char* kMarkDone =
    "UPDATE pgquarry.jobs SET status = 'done', embedding = $2, error = NULL, updated_at = now() "
    "WHERE id = $1";

inline constexpr const char* kMarkError =
    "UPDATE pgquarry.jobs SET status = 'error', error = $2, updated_at = now() "
    "WHERE id = $1";

} // namespace pgquarry::sql
