#include "writeback_sql_builder.hpp"

#include "config.hpp"

#include <sstream>

namespace pgquarry {

namespace {

// value_expr is "$1::vector" for embed mappings, plain "$1" for generate
// (text) mappings — the only thing that differs between the two.
std::string update_sql(const TableMapping& m, const char* value_expr)
{
    std::ostringstream sql;
    sql << "UPDATE " << m.target_table
        << " SET " << m.target_column << " = " << value_expr
        << " WHERE " << m.target_id_column << " = $2";
    return sql.str();
}

std::string upsert_sql(const TableMapping& m, const char* value_expr)
{
    std::ostringstream sql;
    sql << "INSERT INTO " << m.target_table
        << " (" << m.target_id_column << ", " << m.target_column << ")"
        << " VALUES ($2, " << value_expr << ")"
        << " ON CONFLICT (" << m.target_id_column << ")"
        << " DO UPDATE SET " << m.target_column << " = EXCLUDED." << m.target_column;
    return sql.str();
}

} // namespace

std::string WritebackSqlBuilder::build_update_sql(const TableMapping& m)      { return update_sql(m, "$1::vector"); }
std::string WritebackSqlBuilder::build_upsert_sql(const TableMapping& m)      { return upsert_sql(m, "$1::vector"); }
std::string WritebackSqlBuilder::build_update_sql_text(const TableMapping& m) { return update_sql(m, "$1"); }
std::string WritebackSqlBuilder::build_upsert_sql_text(const TableMapping& m) { return upsert_sql(m, "$1"); }

} // namespace pgquarry
