#include "db_database.h"

#include <dztrader/core/exception.h>

#include <SQLiteCpp/Statement.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <set>
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

/// 后端列类型 (dztrader::db::legacy::ColumnType) -> SDK 公开列类型 (DzColumnType)。
DzColumnType db_col_type_to_dz(dztrader::db::legacy::ColumnType type) {
    switch (type) {
        case dztrader::db::legacy::ColumnType::Bool:
            return DZ_COL_TYPE_BOOL;
        case dztrader::db::legacy::ColumnType::Int64:
            return DZ_COL_TYPE_INT64;
        case dztrader::db::legacy::ColumnType::Float64:
            return DZ_COL_TYPE_FLOAT64;
        case dztrader::db::legacy::ColumnType::String:
            return DZ_COL_TYPE_STRING;
        case dztrader::db::legacy::ColumnType::Null:
            return DZ_COL_TYPE_NULL;
    }
    return DZ_COL_TYPE_NULL;
}

/// 按声明类型读取列值: REAL 声明列即使运行时存整数 (SQLite 的 REAL-affinity 空间优化:
/// 整数浮点值以 INTEGER 存储) 也归一化存 double, 保证 get_float64 返回正确值。
ColumnValue read_column_value(const SQLite::Column& col, DzColumnType declared) {
    const int t = col.getType();
    if (t == SQLite::Null) {
        return std::monostate{};
    }
    if (t == SQLite::INTEGER) {
        const int64_t v = col.getInt64();
        if (declared == DZ_COL_TYPE_FLOAT64) {
            return static_cast<double>(v);
        }
        return v;
    }
    if (t == SQLite::FLOAT) {
        return col.getDouble();
    }
    return col.getString();  // TEXT (含 BLOB 防御)
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
    std::vector<DzColumnType> declared_types;
    declared_types.reserve(static_cast<size_t>(col_count));
    for (int i = 0; i < col_count; ++i) {
        ColumnMeta meta;
        meta.name = stmt.getColumnName(i);
        meta.type = declared_type_to_col_type(stmt.getColumnDeclaredType(i));
        declared_types.push_back(meta.type);
        out->columns.push_back(std::move(meta));
    }

    while (stmt.executeStep()) {
        Row row;
        row.reserve(static_cast<size_t>(col_count));
        for (int i = 0; i < col_count; ++i) {
            row.emplace_back(read_column_value(stmt.getColumn(i), declared_types[i]));
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
    throw Exception(DZ_EC_INVALID_PARAM, "unknown query resource: query={}", q);
}

/// 表 -> 可过滤字段白名单 (与 libs/tdstore/src/schema.cpp 各表真实列名一一对应)。
/// filter 字段名必须先过本白名单再拼 SQL, 防注入 (值已参数绑定, 字段名只能 allowlist)。
/// 表结构变更时此处一并更新。
const std::set<std::string>& table_filterable_columns(const std::string_view table) {
    static const std::set<std::string> kOrders = {"account_id", "trading_day", "order_id",
        "order_ref", "external_order_id", "is_external", "instrument_id", "exchange_id",
        "direction", "position_effect", "price_type", "status", "price", "volume",
        "volume_traded", "volume_canceled", "insert_time", "update_time", "error_id",
        "error_msg", "strategy_id", "remark", "seq"};
    static const std::set<std::string> kTrades = {"account_id", "trading_day", "trade_id",
        "order_id", "instrument_id", "exchange_id", "direction", "position_effect", "price",
        "volume", "trade_time", "trade_date", "commission", "strategy_id", "seq"};
    static const std::set<std::string> kPositions = {"account_id", "trading_day", "instrument_id",
        "exchange_id", "direction", "volume", "frozen_volume", "today_volume", "yd_volume",
        "price", "seq"};
    static const std::set<std::string> kTradingAccounts = {"account_id", "trading_day", "balance",
        "available", "frozen", "commission", "margin", "withdraw_quota", "deposit", "withdraw",
        "seq"};
    if (table == "orders") return kOrders;
    if (table == "trades") return kTrades;
    if (table == "positions") return kPositions;
    return kTradingAccounts;
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
    throw Exception(DZ_EC_INVALID_PARAM, "filter value must be string/int/float");
}

/// 解析 filter JSON 对象 -> WHERE 子句 + 绑定值 (按出现顺序绑定 ?)
/// 支持三种形态:
///   {"field": value}                                -> field = ?
///   {"field": {"$gte":v,"$lt":v,...}}               -> field >= ? AND field < ?  (每算子)
///   {"field": {"$in":[a,b]}}                        -> field IN (?, ?)
/// 多字段按 AND 组合。filter 为空 -> 无过滤。
/// @param table 目标表 (用于字段名白名单校验; 字段必须为该表真实列, 否则 INVALID_PARAM)
/// 非法 JSON / 未知算子 / 非本表字段 / 非对象 filter 均抛 INVALID_PARAM 异常。
void build_filter_where(const std::string& table,
                        const std::string& filter,
                        std::string* where,
                        std::vector<BindValue>* values) {
    if (filter.empty()) {
        where->clear();
        return;
    }
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(filter);
    } catch (const nlohmann::json::exception&) {
        throw Exception(DZ_EC_INVALID_PARAM, "filter is not valid JSON");
    }
    if (!j.is_object()) {
        throw Exception(DZ_EC_INVALID_PARAM, "filter must be a JSON object");
    }
    const auto& columns = table_filterable_columns(table);
    std::vector<std::string> clauses;
    for (const auto& [field, cond] : j.items()) {
        if (columns.find(field) == columns.end()) {
            throw Exception(DZ_EC_INVALID_PARAM, "unknown filter field: field={}", field);
        }
        if (cond.is_object()) {
            // 算子对象: {"$gte":v,"$lt":v,...} 或 {"$in":[..]}
            const auto in_it = cond.find("$in");
            if (in_it != cond.end()) {
                if (!in_it->is_array()) {
                    throw Exception(DZ_EC_INVALID_PARAM, "$in must be an array");
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
                    throw Exception(DZ_EC_INVALID_PARAM, "unknown filter operator: op={}", op);
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
    // 只读连接也设 busy_timeout (与生产写端 td_persist_writer.cpp 一致, SQLiteCpp 默认 0):
    // td Writer 批量提交持写锁窗口内, SDK 水位装载/断档回补查询不得立即 SQLITE_BUSY.
    // spec §3.3: 低频读端可吸收毫秒级写锁.
    handle->db->exec("PRAGMA busy_timeout=5000");
    return handle;
}

DbQueryResult to_db_query_result(dztrader::db::legacy::QueryResult result) {
    DbQueryResult out;
    out.columns.reserve(result.columns.size());
    for (auto& column : result.columns) {
        out.columns.push_back(ColumnMeta{db_col_type_to_dz(column.type), std::move(column.name)});
    }
    out.rows = std::move(result.rows);
    return out;
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
    const char* table = resource_to_table(query);
    std::string where;
    std::vector<BindValue> bind_values;
    build_filter_where(table, filter, &where, &bind_values);
    DbQueryResult out;
    // 回补路径依赖: 行序 = seq 升序 (resource_to_table 各表均为 seq 表)
    load_select_all(*db->db, table, where, bind_values, /*order_by_seq=*/true, &out);
    return out;
}

std::unordered_map<std::string, uint64_t> db_query_max_seq_by_account(
    DzDatabase* db, const std::string& resource) {
    const char* table = resource_to_table(resource);
    std::unordered_map<std::string, uint64_t> result;
    // 终检发现 E: 聚合查询替代 SELECT * 全行物化 — 每账户一行, 库增长不线性恶化。
    SQLite::Statement stmt(*db->db, "SELECT account_id, MAX(seq) FROM " + std::string(table) +
                                        " GROUP BY account_id");
    while (stmt.executeStep()) {
        const std::string acct = stmt.getColumn(0).getString();
        if (acct.empty()) {
            continue;
        }
        result[acct] = static_cast<uint64_t>(stmt.getColumn(1).getInt64());
    }
    return result;
}

uint64_t db_query_account_max_seq(DzDatabase* db,
                                  const std::string& resource,
                                  const std::string& account_id) {
    const char* table = resource_to_table(resource);
    SQLite::Statement stmt(*db->db,
                           "SELECT COALESCE(MAX(seq), 0) FROM " + std::string(table) +
                               " WHERE account_id = ?");
    stmt.bind(1, account_id);
    uint64_t max_seq = 0;
    if (stmt.executeStep()) {
        max_seq = static_cast<uint64_t>(stmt.getColumn(0).getInt64());
    }
    return max_seq;
}

}  // namespace dztrader::strategy_api_internal
