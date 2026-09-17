#include "sqlite_sql.h"

#include <dztrader/core/exception.h>
#include <dztrader/error.h>

namespace dztrader::db::internal {

namespace {

std::string quote_identifier(std::string_view name) { return "\"" + std::string(name) + "\""; }

const FieldSchema& require_field(const ResourceSchema& schema, std::string_view name) {
    const FieldSchema* field = find_field(schema, name);
    if (field == nullptr) {
        throw Exception(DZ_EC_DB_QUERY_FAILED, "unknown field | field={} collection={}", name,
                        schema.name);
    }
    return *field;
}

const char* compare_operator(CompareOp op) {
    switch (op) {
        case CompareOp::Eq: return "=";
        case CompareOp::Ne: return "!=";
        case CompareOp::Gt: return ">";
        case CompareOp::Gte: return ">=";
        case CompareOp::Lt: return "<";
        case CompareOp::Lte: return "<=";
        case CompareOp::In: return "IN";
    }
    return "=";
}

const char* aggregate_function(AggregateOp op) {
    switch (op) {
        case AggregateOp::Count: return "COUNT";
        case AggregateOp::Max: return "MAX";
        case AggregateOp::Min: return "MIN";
        case AggregateOp::Sum: return "SUM";
        case AggregateOp::Avg: return "AVG";
    }
    return "COUNT";
}

std::string order_by_clause(const ResourceSchema& schema, std::span<const SortSpec> sort) {
    if (sort.empty()) {
        return {};
    }
    std::string clause = " ORDER BY ";
    for (size_t i = 0; i < sort.size(); ++i) {
        if (i > 0) {
            clause += ", ";
        }
        clause += quote_identifier(require_field(schema, sort[i].field).name);
        clause += sort[i].order == SortOrder::Descending ? " DESC" : " ASC";
    }
    return clause;
}

void append_field_projection(std::string& sql, const ResourceSchema& schema) {
    for (size_t i = 0; i < schema.fields.size(); ++i) {
        if (i > 0) {
            sql += ", ";
        }
        sql += quote_identifier(schema.fields[i].name);
    }
}

}  // namespace

const FieldSchema* find_field(const ResourceSchema& schema, std::string_view name) {
    for (const FieldSchema& field : schema.fields) {
        if (field.name == name) {
            return &field;
        }
    }
    return nullptr;
}

CompiledFilter compile_filter(std::span<const Condition> conditions, const ResourceSchema& schema) {
    CompiledFilter compiled;
    for (const Condition& condition : conditions) {
        const FieldSchema& field = require_field(schema, condition.field);
        if (!compiled.where.empty()) {
            compiled.where += " AND ";
        }
        if (condition.op == CompareOp::In) {
            if (condition.values.empty()) {
                compiled.where += "0";  // 空 IN 恒假
                continue;
            }
            compiled.where += quote_identifier(field.name);
            compiled.where += " IN (";
            for (size_t i = 0; i < condition.values.size(); ++i) {
                if (i > 0) {
                    compiled.where += ", ";
                }
                compiled.where += "?";
            }
            compiled.where += ")";
            for (const Value& value : condition.values) {
                compiled.params.push_back(value);
            }
            continue;
        }
        if (condition.values.empty()) {
            throw Exception(DZ_EC_INVALID_PARAM, "filter condition missing value | field={}",
                            condition.field);
        }
        compiled.where += quote_identifier(field.name);
        compiled.where += " ";
        compiled.where += compare_operator(condition.op);
        compiled.where += " ?";
        compiled.params.push_back(condition.values.front());
    }
    return compiled;
}

std::string build_upsert_sql(const ResourceSchema& schema) {
    std::string sql = "INSERT OR REPLACE INTO ";
    sql += quote_identifier(schema.name);
    sql += " (";
    append_field_projection(sql, schema);
    sql += ") VALUES (";
    for (size_t i = 0; i < schema.fields.size(); ++i) {
        if (i > 0) {
            sql += ", ";
        }
        sql += "?";
    }
    sql += ")";
    return sql;
}

std::string build_delete_sql(const ResourceSchema& schema, const CompiledFilter& filter) {
    std::string sql = "DELETE FROM ";
    sql += quote_identifier(schema.name);
    if (!filter.where.empty()) {
        sql += " WHERE " + filter.where;
    }
    return sql;
}

std::string build_select_sql(const ResourceSchema& schema, const CompiledFilter& filter,
                             std::span<const SortSpec> sort, int64_t offset, int64_t limit) {
    std::string sql = "SELECT ";
    append_field_projection(sql, schema);
    sql += " FROM " + quote_identifier(schema.name);
    if (!filter.where.empty()) {
        sql += " WHERE " + filter.where;
    }
    sql += order_by_clause(schema, sort);
    if (limit >= 0) {
        sql += " LIMIT " + std::to_string(limit);
    }
    if (offset > 0) {
        if (limit < 0) {
            sql += " LIMIT -1";
        }
        sql += " OFFSET " + std::to_string(offset);
    }
    return sql;
}

std::string build_aggregate_sql(const ResourceSchema& schema, AggregateOp op, std::string_view field,
                                std::span<const std::string> group_by, const CompiledFilter& filter,
                                std::span<const SortSpec> sort) {
    std::string sql = "SELECT ";
    for (size_t i = 0; i < group_by.size(); ++i) {
        if (i > 0) {
            sql += ", ";
        }
        sql += quote_identifier(require_field(schema, group_by[i]).name);
    }
    if (!group_by.empty()) {
        sql += ", ";
    }
    if (op == AggregateOp::Count) {
        sql += "COUNT(*)";
    } else {
        sql += aggregate_function(op);
        sql += "(";
        sql += quote_identifier(require_field(schema, field).name);
        sql += ")";
    }
    sql += " FROM " + quote_identifier(schema.name);
    if (!filter.where.empty()) {
        sql += " WHERE " + filter.where;
    }
    if (!group_by.empty()) {
        sql += " GROUP BY ";
        for (size_t i = 0; i < group_by.size(); ++i) {
            if (i > 0) {
                sql += ", ";
            }
            sql += quote_identifier(group_by[i]);
        }
    }
    sql += order_by_clause(schema, sort);
    return sql;
}

}  // namespace dztrader::db::internal
