#include "td_schema.h"

#include <deque>
#include <string>
#include <string_view>
#include <vector>

#include <dztrader/core/exception.h>
#include <dztrader/error.h>
#include <dztrader/tdstore/schema_catalog.h>

namespace dztrader::db {

namespace {

/// 投影的稳定存储：string_view 指向 deque 元素（push_back 不失效既有引用）。
struct ProjectionStorage {
    std::deque<std::string> strings;
    std::vector<ResourceSchema> schemas;
};

std::string_view intern(ProjectionStorage& storage, std::string_view text) {
    storage.strings.emplace_back(text);
    return storage.strings.back();
}

ValueType to_db_type(tdstore::ColumnType type) {
    switch (type) {
        case tdstore::ColumnType::Integer:
            return ValueType::Int64;
        case tdstore::ColumnType::Real:
            return ValueType::Float64;
        case tdstore::ColumnType::Text:
            return ValueType::String;
    }
    return ValueType::String;
}

ProjectionStorage build_storage() {
    ProjectionStorage out;
    for (const tdstore::TableSchema& table : tdstore::tables()) {
        ResourceSchema projected;
        projected.name = intern(out, table.name);
        projected.fields.reserve(table.columns.size());
        for (const tdstore::ColumnSchema& column : table.columns) {
            projected.fields.push_back(FieldSchema{
                .name = intern(out, column.name),
                .type = to_db_type(column.type),
                .nullable = !column.not_null,
                .primary_key = column.primary_key,
                .auto_increment = false,
            });
        }
        projected.indexes.reserve(table.indexes.size());
        for (const tdstore::IndexSchema& index : table.indexes) {
            IndexSchema projected_index;
            projected_index.name = intern(out, index.name);
            projected_index.unique = index.unique;
            projected_index.fields.reserve(index.columns.size());
            for (const std::string_view column : index.columns) {
                projected_index.fields.push_back(intern(out, column));
            }
            projected.indexes.push_back(std::move(projected_index));
        }
        out.schemas.push_back(std::move(projected));
    }
    return out;
}

const ProjectionStorage& storage() {
    static const ProjectionStorage kStorage = build_storage();
    return kStorage;
}

}  // namespace

std::span<const ResourceSchema> td_schemas() { return storage().schemas; }

const ResourceSchema& td_instruments_schema() {
    return storage().schemas.back();  // tdstore::tables() 末项 = instruments
}

std::vector<std::string> strategy_visible_column_names(std::string_view table) {
    for (const tdstore::TableSchema& declared : tdstore::tables()) {
        if (declared.name != table) {
            continue;
        }
        std::vector<std::string> names;
        for (const tdstore::ColumnSchema& column : tdstore::strategy_visible_columns(declared)) {
            names.emplace_back(column.name);
        }
        return names;
    }
    throw Exception(DZ_EC_INVALID_PARAM, "unknown table: table={}", table);
}

}  // namespace dztrader::db
