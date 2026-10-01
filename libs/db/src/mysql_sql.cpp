#include "mysql_sql.h"

#include <string>
#include <vector>

#include <dztrader/core/exception.h>
#include <dztrader/error.h>

namespace dztrader::db::internal {

namespace {

std::string quote_identifier(std::string_view name) { return "`" + std::string(name) + "`"; }

const FieldSchema& require_field(const ResourceSchema& schema, std::string_view name) {
    for (const FieldSchema& field : schema.fields) {
        if (field.name == name) {
            return field;
        }
    }
    throw Exception(DZ_EC_INVALID_PARAM, "unknown field | field={} collection={}", name,
                    schema.name);
}

/// 主键/自增列必须 NOT NULL（MySQL 对 PK 隐式 NOT NULL，显式写出避免可空声明漂移）
bool is_not_null(const FieldSchema& field) {
    return !field.nullable || field.primary_key || field.auto_increment;
}

/// 列定义行（行内文本；行间逗号由 build_mysql_create_table_sql 统一 join）
std::string field_definition(const FieldSchema& field, const ResourceSchema& schema) {
    std::string line = "  ";
    line += quote_identifier(field.name);
    line += " ";
    line += mysql_column_type(field.type, field.name, schema.name);
    if (is_not_null(field)) {
        line += " NOT NULL";
    }
    if (field.auto_increment) {
        line += " AUTO_INCREMENT";
    }
    return line;
}

/// 命名索引行
std::string key_definition(std::string_view kind, std::string_view name,
                           std::span<const std::string_view> fields, const ResourceSchema& schema) {
    std::string line = "  ";
    line += kind;
    line += " ";
    line += quote_identifier(name);
    line += " (";
    for (size_t i = 0; i < fields.size(); ++i) {
        if (i > 0) {
            line += ", ";
        }
        line += quote_identifier(require_field(schema, fields[i]).name);
    }
    line += ")";
    return line;
}

/// PRIMARY KEY 不接受索引名（MySQL 语法：PRIMARY KEY (cols)）
std::string primary_key_definition(std::string_view column) {
    return "  PRIMARY KEY (" + quote_identifier(column) + ")";
}

}  // namespace

std::string_view mysql_column_type(ValueType type, std::string_view column,
                                   std::string_view collection) {
    switch (type) {
        case ValueType::Int64: return "BIGINT";
        case ValueType::Float64: return "DOUBLE";
        // 列级 utf8mb4_bin：对齐 SQLite 默认 BINARY 比较，避免默认 ai_ci 让唯一键/过滤大小写不敏感
        case ValueType::String:
            return "VARCHAR(255) CHARACTER SET utf8mb4 COLLATE utf8mb4_bin";
        case ValueType::Bool: return "TINYINT(1)";
        case ValueType::Null: break;
    }
    throw Exception(DZ_EC_INVALID_PARAM, "unsupported column type | column={} collection={}", column,
                    collection);
}

std::string build_mysql_create_table_sql(const ResourceSchema& schema) {
    if (schema.fields.empty()) {
        throw Exception(DZ_EC_INVALID_PARAM, "schema has no fields | collection={}", schema.name);
    }
    // 先校验索引引用的列，避免拼出半截语句才失败
    for (const IndexSchema& index : schema.indexes) {
        if (index.fields.empty()) {
            throw Exception(DZ_EC_INVALID_PARAM, "index has no fields | index={} collection={}",
                            index.name, schema.name);
        }
        for (const std::string_view field : index.fields) {
            (void)require_field(schema, field);
        }
    }
    // 所有定义行先收集再 join(",\n")：字段/主键/索引任一形态缺尾元素时都不得留尾随逗号
    // （真实漏网场景：instruments 28 字段 + 主键 + 零索引）
    std::vector<std::string> lines;
    lines.reserve(schema.fields.size() + 1 + schema.indexes.size());
    for (const FieldSchema& field : schema.fields) {
        lines.push_back(field_definition(field, schema));
    }
    const FieldSchema* primary = nullptr;
    for (const FieldSchema& field : schema.fields) {
        if (field.primary_key) {
            primary = &field;
            break;
        }
    }
    if (primary != nullptr) {
        lines.push_back(primary_key_definition(primary->name));
    }
    for (const IndexSchema& index : schema.indexes) {
        lines.push_back(key_definition(index.unique ? "UNIQUE KEY" : "KEY", index.name, index.fields,
                                       schema));
    }
    std::string sql = "CREATE TABLE IF NOT EXISTS ";
    sql += quote_identifier(schema.name);
    sql += " (\n";
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) {
            sql += ",\n";
        }
        sql += lines[i];
    }
    sql += "\n) ENGINE=InnoDB ROW_FORMAT=DYNAMIC DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin";
    return sql;
}

std::string build_mysql_upsert_sql(const ResourceSchema& schema) {
    if (schema.fields.empty()) {
        throw Exception(DZ_EC_INVALID_PARAM, "schema has no fields | collection={}", schema.name);
    }
    std::string sql = "INSERT INTO ";
    sql += quote_identifier(schema.name);
    sql += " (";
    for (size_t i = 0; i < schema.fields.size(); ++i) {
        if (i > 0) {
            sql += ", ";
        }
        sql += quote_identifier(schema.fields[i].name);
    }
    sql += ") VALUES (";
    for (size_t i = 0; i < schema.fields.size(); ++i) {
        if (i > 0) {
            sql += ", ";
        }
        sql += "?";
    }
    sql += ") ON DUPLICATE KEY UPDATE ";
    // 自增主键列不参与更新：SQLite 的 INSERT OR REPLACE 会换 id，MySQL 保持原 id 稳定
    // （spec §7.6：两者都不承诺 id 稳定，业务键才是幂等依据）
    bool first = true;
    for (const FieldSchema& field : schema.fields) {
        if (field.auto_increment) {
            continue;
        }
        if (!first) {
            sql += ", ";
        }
        first = false;
        sql += quote_identifier(field.name);
        sql += "=VALUES(";
        sql += quote_identifier(field.name);
        sql += ")";
    }
    if (first) {
        // 全表只有自增列：退化为自赋值，保证语法合法且不改变行数据
        sql += quote_identifier(schema.fields[0].name);
        sql += "=";
        sql += quote_identifier(schema.fields[0].name);
    }
    return sql;
}

std::string_view mysql_schema_version_ddl() noexcept {
    // 与 sqlite 侧 schema_version(version INTEGER PRIMARY KEY, applied_at TEXT NOT NULL) 对齐：
    // version 主键避免并发重复行；applied_at 沿用 "%FT%TZ" 文本形状
    return "CREATE TABLE IF NOT EXISTS `schema_version` (\n"
           "  `version` INT NOT NULL,\n"
           "  `applied_at` VARCHAR(32) NOT NULL,\n"
           "  PRIMARY KEY (`version`)\n"
           ") ENGINE=InnoDB ROW_FORMAT=DYNAMIC DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin";
}

std::string_view mysql_schema_version_select() noexcept {
    return "SELECT MAX(`version`) FROM `schema_version`";
}

std::string_view mysql_schema_version_insert() noexcept {
    return "INSERT INTO `schema_version` (`version`, `applied_at`) VALUES (?, ?) ON DUPLICATE KEY "
           "UPDATE `applied_at`=VALUES(`applied_at`)";
}

}  // namespace dztrader::db::internal
