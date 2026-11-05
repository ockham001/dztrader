#include "sqlite_sql.h"

#include <string>

#include "sql_compiler.h"

namespace dztrader::db::internal {

std::string build_upsert_sql(const ResourceSchema& schema) {
    std::string sql = "INSERT OR REPLACE INTO ";
    sql += quote_ident(kSqliteDialect, schema.name);
    sql += " (";
    sql += build_column_list(schema, kSqliteDialect);
    sql += ") VALUES (";
    sql += build_placeholders(schema.fields.size());
    sql += ")";
    return sql;
}

}  // namespace dztrader::db::internal
