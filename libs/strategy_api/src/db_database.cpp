#include "db_database.h"

#include <dztrader/core/exception.h>

#include <SQLiteCpp/Statement.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace dztrader::strategy_api_internal {

namespace {

/// WHERE 绑定值: 仅字符串/整数/浮点三种 (filter JSON 的取值类型)
using BindValue = std::variant<std::string, int64_t, double>;

void bind_value(SQLite::Statement& stmt, int index, const BindValue& v) {
    std::visit([&](const auto& val) { stmt.bind(index, val); }, v);
}

/// SQLite 声明类型名 (getColumnDeclaredType) -> DzColumnType。
/// 空结果集时列类型仍正确 (声明类型为准, 不依赖首行实际值)。
DzColumnType declared_type_to_col_type(const char* declared) {
    if (declared == nullptr) {
        return DZ_COL_TYPE_NULL;
    }
    const std::string_view t(declared);
    if (t.find("INT") != std::string_view::npos) {
        return DZ_COL_TYPE_INT64;
    }
    if (t.find("CHAR") != std::string_view::npos || t.find("TEXT") != std::string_view::npos ||
        t.find("CLOB") != std::string_view::npos) {
        return DZ_COL_TYPE_STRING;
    }
    if (t.find("REAL") != std::string_view::npos || t.find("FLOA") != std::string_view::npos ||
        t.find("DOUB") != std::string_view::npos) {
        return DZ_COL_TYPE_FLOAT64;
    }
    return DZ_COL_TYPE_NULL;
}

/// 通用: 执行 SELECT * 并将结果装入行容器。
/// order_by_seq=true 时按 seq 升序 (回补路径依赖: 行序 = seq 序)。
/// 异常抛给调用方。
void load_select_all(SQLite::Database& db,
                     const std::string& table,
                     const std::string& where,
                     const std::vector<BindValue>& bind_values,
                     bool order_by_seq,
                     DbQueryResult* out) {
    std::string sql = "SELECT * FROM " + table;
    if (!where.empty()) {
        sql += " WHERE " + where;
    }
    if (order_by_seq) {
        sql += " ORDER BY seq ASC";
    }

    SQLite::Statement stmt(db, sql);
    for (size_t i = 0; i < bind_values.size(); ++i) {
        bind_value(stmt, static_cast<int>(i + 1), bind_values[i]);
    }

    const int col_count = stmt.getColumnCount();
    for (int i = 0; i < col_count; ++i) {
        ColumnMeta meta;
        meta.name = stmt.getColumnName(i);
        meta.type = declared_type_to_col_type(stmt.getColumnDeclaredType(i));
        out->columns.push_back(std::move(meta));
    }

    while (stmt.executeStep()) {
        Row row;
        row.reserve(static_cast<size_t>(col_count));
        for (int i = 0; i < col_count; ++i) {
            const SQLite::Column col = stmt.getColumn(i);
            const int t = col.getType();
            if (t == SQLite::Null) {
                row.emplace_back(std::monostate{});
            } else if (t == SQLite::INTEGER) {
                row.emplace_back(col.getInt64());
            } else if (t == SQLite::FLOAT) {
                row.emplace_back(col.getDouble());
            } else {  // TEXT (含 BLOB 防御)
                row.emplace_back(col.getString());
            }
        }
        out->rows.push_back(std::move(row));
    }
}

/// 条件查询: 构造 WHERE 子句 + 绑定值。account_id/instrument_id 空串表示不限定。
void build_account_instrument_where(const std::string& account_id,
                                    const std::string& instrument_id,
                                    const char* account_col,
                                    const char* instrument_col,
                                    std::string* where,
                                    std::vector<BindValue>* values) {
    std::string clauses;
    if (!account_id.empty()) {
        clauses += std::string(account_col) + " = ?";
        values->emplace_back(account_id);
    }
    if (!instrument_id.empty()) {
        if (!clauses.empty()) {
            clauses += " AND ";
        }
        clauses += std::string(instrument_col) + " = ?";
        values->emplace_back(instrument_id);
    }
    *where = clauses;
}

/// 资源路径 -> 表名 (仅本接口支持的表; 其余抛 INVALID_PARAM 异常)
const char* resource_to_table(const std::string_view q) {
    if (q == "order") return "orders";
    if (q == "trade") return "trades";
    if (q == "position") return "positions";
    if (q == "trading_account") return "trading_accounts";
    if (q == "commission") return "commission_rates";
    if (q == "margin") return "margin_rates";
    throw Exception(DZ_EC_INVALID_PARAM, "unknown query resource: %s", std::string(q).c_str());
}

/// JSON 值 -> BindValue (字符串/整数/浮点)
BindValue json_to_bind_value(const nlohmann::json& v) {
    if (v.is_string()) {
        return v.get<std::string>();
    }
    if (v.is_number_integer() || v.is_number_unsigned()) {
        return static_cast<int64_t>(v.get<int64_t>());
    }
    if (v.is_number_float()) {
        return v.get<double>();
    }
    throw std::invalid_argument("filter value must be string/int/float");
}

/// 解析 filter JSON 对象 -> WHERE 子句 + 绑定值 (按出现顺序绑定 ?)
/// 支持三种形态:
///   {"field": value}                                -> field = ?
///   {"field": {"$gte":v,"$lt":v,...}}               -> field >= ? AND field < ?  (每算子)
///   {"field": {"$in":[a,b]}}                        -> field IN (?, ?)
/// 多字段按 AND 组合。filter 为空 -> 无过滤。
void build_filter_where(const std::string& filter, std::string* where, std::vector<BindValue>* values) {
    if (filter.empty()) {
        where->clear();
        return;
    }
    const nlohmann::json j = nlohmann::json::parse(filter);  // 非法 JSON 抛异常
    if (!j.is_object()) {
        throw std::invalid_argument("filter must be a JSON object");
    }
    std::vector<std::string> clauses;
    for (const auto& [field, cond] : j.items()) {
        if (cond.is_object()) {
            // 算子对象: {"$gte":v,"$lt":v,...} 或 {"$in":[..]}
            const auto in_it = cond.find("$in");
            if (in_it != cond.end()) {
                if (!in_it->is_array()) {
                    throw std::invalid_argument("$in must be an array");
                }
                std::string placeholders;
                for (size_t i = 0; i < in_it->size(); ++i) {
                    if (!placeholders.empty()) {
                        placeholders += ", ";
                    }
                    placeholders += "?";
                    values->push_back(json_to_bind_value((*in_it)[i]));
                }
                clauses.push_back(field + " IN (" + placeholders + ")");
            }
            for (const auto& [op, val] : cond.items()) {
                if (op == "$in") {
                    continue;
                }
                std::string_view sql_op;
                if (op == "$gte") {
                    sql_op = ">=";
                } else if (op == "$gt") {
                    sql_op = ">";
                } else if (op == "$lte") {
                    sql_op = "<=";
                } else if (op == "$lt") {
                    sql_op = "<";
                } else {
                    throw std::invalid_argument("unknown filter operator: " + op);
                }
                clauses.push_back(field + " " + std::string(sql_op) + " ?");
                values->push_back(json_to_bind_value(val));
            }
        } else {
            // 简单相等
            clauses.push_back(field + " = ?");
            values->push_back(json_to_bind_value(cond));
        }
    }
    std::string joined;
    for (size_t i = 0; i < clauses.size(); ++i) {
        if (i > 0) {
            joined += " AND ";
        }
        joined += clauses[i];
    }
    *where = joined;
}

}  // namespace

std::unique_ptr<DzDatabase> db_open_readonly(const std::string& path) {
    auto handle = std::make_unique<DzDatabase>();
    handle->db = std::make_unique<SQLite::Database>(path, SQLite::OPEN_READONLY);
    return handle;
}

DbQueryResult db_query_order_trade(DzDatabase* db,
                                   const std::string& account_id,
                                   const std::string& instrument_id,
                                   const std::string& table,
                                   bool order_by_seq) {
    std::string where;
    std::vector<BindValue> bind_values;
    build_account_instrument_where(account_id, instrument_id, "account_id", "instrument_id",
                                   &where, &bind_values);
    DbQueryResult out;
    load_select_all(*db->db, table, where, bind_values, order_by_seq, &out);
    return out;
}

DbQueryResult db_query_position(DzDatabase* db,
                                const std::string& account_id,
                                const std::string& instrument_id) {
    std::string where;
    std::vector<BindValue> bind_values;
    build_account_instrument_where(account_id, instrument_id, "account_id", "instrument_id",
                                   &where, &bind_values);
    DbQueryResult out;
    load_select_all(*db->db, "positions", where, bind_values, /*order_by_seq=*/true, &out);
    return out;
}

DbQueryResult db_query_trading_account(DzDatabase* db, const std::string& account_id) {
    std::string where;
    std::vector<BindValue> bind_values;
    if (!account_id.empty()) {
        where = "account_id = ?";
        bind_values.emplace_back(account_id);
    }
    DbQueryResult out;
    load_select_all(*db->db, "trading_accounts", where, bind_values, /*order_by_seq=*/true, &out);
    return out;
}

DbQueryResult db_generic_query(DzDatabase* db, const std::string& query, const std::string& filter) {
    // 仅 seq 表按 seq 升序 (回补路径依赖); commission/margin 无 seq 列, 不排序
    const bool order_by_seq =
        query != "commission" && query != "margin";
    const char* table = resource_to_table(query);
    std::string where;
    std::vector<BindValue> bind_values;
    build_filter_where(filter, &where, &bind_values);
    DbQueryResult out;
    load_select_all(*db->db, table, where, bind_values, order_by_seq, &out);
    return out;
}

}  // namespace dztrader::strategy_api_internal
