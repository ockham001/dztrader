#include "sqlite_migrations.h"

#include <string>
#include <string_view>
#include <vector>

#include <SQLiteCpp/Database.h>
#include <SQLiteCpp/Statement.h>

#include <dztrader/core/exception.h>
#include <dztrader/error.h>

namespace dztrader::db::internal {

namespace {

std::string quote_identifier(std::string_view name) { return "\"" + std::string(name) + "\""; }

std::string sqlite_type(ValueType type) {
    switch (type) {
        case ValueType::Bool:
        case ValueType::Int64:
            return "INTEGER";
        case ValueType::Float64:
            return "REAL";
        case ValueType::String:
        case ValueType::Null:
            return "TEXT";
    }
    return "TEXT";
}

bool table_exists(SQLite::Database& db, std::string_view name) {
    SQLite::Statement stmt(db, "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name=?");
    stmt.bind(1, std::string(name));
    return stmt.executeStep() && stmt.getColumn(0).getInt() > 0;
}

}  // namespace

void create_missing_collections(SQLite::Database& db, std::span<const ResourceSchema> schemas) {
    try {
        for (const ResourceSchema& schema : schemas) {
            if (table_exists(db, schema.name)) {
                continue;
            }
            std::vector<std::string_view> primary_keys;
            for (const FieldSchema& field : schema.fields) {
                if (field.primary_key) {
                    primary_keys.push_back(field.name);
                }
            }
            const bool inline_primary_key = primary_keys.size() == 1;
            std::string sql = "CREATE TABLE IF NOT EXISTS " + quote_identifier(schema.name) + " (";
            for (size_t i = 0; i < schema.fields.size(); ++i) {
                const FieldSchema& field = schema.fields[i];
                if (i > 0) {
                    sql += ", ";
                }
                sql += quote_identifier(field.name) + " " + sqlite_type(field.type);
                if (inline_primary_key && field.primary_key) {
                    sql += " PRIMARY KEY";
                    if (field.auto_increment) {
                        sql += " AUTOINCREMENT";
                    }
                    if (!field.nullable) {
                        sql += " NOT NULL";
                    }
                } else if (!field.nullable) {
                    sql += " NOT NULL";
                }
            }
            if (!inline_primary_key && !primary_keys.empty()) {
                sql += ", PRIMARY KEY (";
                for (size_t i = 0; i < primary_keys.size(); ++i) {
                    if (i > 0) {
                        sql += ", ";
                    }
                    sql += quote_identifier(primary_keys[i]);
                }
                sql += ")";
            }
            sql += ")";
            db.exec(sql);
            for (const IndexSchema& index : schema.indexes) {
                std::string create = "CREATE " + std::string(index.unique ? "UNIQUE " : "") +
                                     "INDEX IF NOT EXISTS " + quote_identifier(index.name) +
                                     " ON " + quote_identifier(schema.name) + " (";
                for (size_t i = 0; i < index.fields.size(); ++i) {
                    if (i > 0) {
                        create += ", ";
                    }
                    create += quote_identifier(index.fields[i]);
                }
                create += ")";
                db.exec(create);
            }
        }
    } catch (const std::exception& e) {
        throw Exception(DZ_EC_DB_MIGRATE_FAILED, "schema create failed: {}", e.what());
    }
}

}  // namespace dztrader::db::internal
