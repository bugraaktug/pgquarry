#include "writeback_sql_builder.hpp"

#include "config.hpp"

#include <sstream>

namespace pgquarry {

std::string WritebackSqlBuilder::build_update_sql(const TableMapping& m)
{
    std::ostringstream sql;
    sql << "UPDATE " << m.target_table
        << " SET " << m.target_column << " = $1::vector"
        << " WHERE " << m.target_id_column << " = $2";
    return sql.str();
}

std::string WritebackSqlBuilder::build_upsert_sql(const TableMapping& m)
{
    std::ostringstream sql;
    sql << "INSERT INTO " << m.target_table
        << " (" << m.target_id_column << ", " << m.target_column << ")"
        << " VALUES ($2, $1::vector)"
        << " ON CONFLICT (" << m.target_id_column << ")"
        << " DO UPDATE SET " << m.target_column << " = EXCLUDED." << m.target_column;
    return sql.str();
}

} // namespace pgquarry
