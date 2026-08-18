#pragma once

#include <string>

namespace pgquarry {

struct TableMapping;

// Builds the parameterized write-back SQL for a pgquarry.watched_tables
// mapping: $1 is always the embedding (cast to ::vector), $2 the source row's
// id. Table and column identifiers come from pgquarry.watch() calls (operator
// config, not client input) and are interpolated directly — same precedent as
// walkrie's PgSqlBuilder for its pgvector sink.
class WritebackSqlBuilder
{
public:
    // Same-table case (target_table == source): UPDATE ... SET target_column = $1 WHERE target_id_column = $2
    static std::string build_update_sql(const TableMapping& m);

    // Cross-table case: INSERT ... ON CONFLICT (target_id_column) DO UPDATE
    static std::string build_upsert_sql(const TableMapping& m);

    // Text-typed counterparts for job_type='generate' mappings — identical
    // shape, just $1 (plain text) instead of $1::vector.
    static std::string build_update_sql_text(const TableMapping& m);
    static std::string build_upsert_sql_text(const TableMapping& m);
};

} // namespace pgquarry
