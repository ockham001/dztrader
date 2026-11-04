#include <dztrader/tdstore/schema_dzdb.h>

#include <deque>
#include <string>
#include <string_view>
#include <vector>

#include <dztrader/tdstore/schema_catalog.h>

namespace dztrader::tdstore {

namespace {

/// dzdb 投影的稳定存储：string_view 指向 deque 元素（push_back 不失效既有引用）。
struct ProjectionStorage {
    std::deque<std::string> strings;
    std::vector<dztrader::db::ResourceSchema> schemas;
};

std::string_view intern(ProjectionStorage& storage, std::string_view text) {
    storage.strings.emplace_back(text);
    return storage.strings.back();
}

dztrader::db::ValueType to_db_type(ColumnType type) {
    switch (type) {
        case ColumnType::Integer:
            return dztrader::db::ValueType::Int64;
        case ColumnType::Real:
            return dztrader::db::ValueType::Float64;
        case ColumnType::Text:
            return dztrader::db::ValueType::String;
    }
    return dztrader::db::ValueType::String;
}

ProjectionStorage build_storage() {
    ProjectionStorage out;
    for (const TableSchema& table : tables()) {
        dztrader::db::ResourceSchema projected;
        projected.name = intern(out, table.name);
        projected.fields.reserve(table.columns.size());
        for (const ColumnSchema& column : table.columns) {
            projected.fields.push_back(dztrader::db::FieldSchema{
                .name = intern(out, column.name),
                .type = to_db_type(column.type),
                .nullable = !column.not_null,
                .primary_key = column.primary_key,
                .auto_increment = false,
            });
        }
        projected.indexes.reserve(table.indexes.size());
        for (const IndexSchema& index : table.indexes) {
            dztrader::db::IndexSchema projected_index;
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

std::span<const dztrader::db::ResourceSchema> schemas() { return storage().schemas; }

const dztrader::db::ResourceSchema& instruments_schema_db() {
    return storage().schemas.back();  // tdstore::tables() 末项 = instruments
}

}  // namespace dztrader::tdstore
