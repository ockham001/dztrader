#include <dztrader/tdstore/schema_catalog.h>

#include <algorithm>
#include <array>
#include <format>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace dztrader::tdstore {

namespace {

// ============================================================================
// 四张交易表（ADR 0041 §1）：业务列 = 帧业务字段 + seq + gateway + ext
// 列序 = 建表列序 = dzdb 投影 Row 位置序（schema_dzdb.h）。
// ============================================================================

// orders：委托最新态（UNIQUE(account_id, date, order_id)；avg_price 为派生列）
constexpr ColumnSchema kOrdersColumns[] = {
    {"account_id", ColumnType::Text, true, false, ""},
    {"date", ColumnType::Text, true, false, ""},
    {"order_id", ColumnType::Integer, true, false, ""},
    {"strategy_id", ColumnType::Text, false, false, ""},
    {"instrument_id", ColumnType::Text, true, false, ""},
    {"exchange_id", ColumnType::Text, true, false, ""},
    {"direction", ColumnType::Text, false, false, ""},
    {"position_effect", ColumnType::Text, false, false, ""},
    {"price_type", ColumnType::Text, false, false, ""},
    {"status", ColumnType::Text, false, false, ""},
    {"price", ColumnType::Real, false, false, ""},
    {"volume", ColumnType::Integer, false, false, ""},
    {"volume_traded", ColumnType::Integer, false, false, ""},
    {"remark", ColumnType::Text, false, false, ""},
    {"time", ColumnType::Integer, false, false, ""},
    {"avg_price", ColumnType::Real, false, false, "0"},
    {"seq", ColumnType::Integer, true, false, ""},
    {"gateway", ColumnType::Text, true, false, ""},
    {"ext", ColumnType::Text, false, false, "'{}'"},
};

constexpr std::string_view kOrdersUniqueKey[] = {"account_id", "date", "order_id"};
constexpr std::string_view kOrdersOrderId[] = {"order_id"};
constexpr std::string_view kOrdersAccountSeq[] = {"account_id", "seq"};
constexpr std::string_view kOrdersAccountDate[] = {"account_id", "date"};

constexpr IndexSchema kOrdersIndexes[] = {
    {"uq_orders_key", std::span<const std::string_view>(kOrdersUniqueKey), true},
    {"idx_orders_order_id", std::span<const std::string_view>(kOrdersOrderId), false},
    {"idx_orders_acct_seq", std::span<const std::string_view>(kOrdersAccountSeq), false},
    {"idx_orders_account_date", std::span<const std::string_view>(kOrdersAccountDate), false},
};

// trades：成交逐笔（UNIQUE(account_id, date, exchange_id, trade_id)）
constexpr ColumnSchema kTradesColumns[] = {
    {"account_id", ColumnType::Text, true, false, ""},
    {"date", ColumnType::Text, true, false, ""},
    {"trade_id", ColumnType::Text, true, false, ""},
    {"order_id", ColumnType::Integer, true, false, ""},
    {"strategy_id", ColumnType::Text, false, false, ""},
    {"instrument_id", ColumnType::Text, true, false, ""},
    {"exchange_id", ColumnType::Text, true, false, ""},
    {"direction", ColumnType::Text, false, false, ""},
    {"position_effect", ColumnType::Text, false, false, ""},
    {"price", ColumnType::Real, true, false, ""},
    {"volume", ColumnType::Integer, true, false, ""},
    {"time", ColumnType::Integer, false, false, ""},
    {"seq", ColumnType::Integer, true, false, ""},
    {"gateway", ColumnType::Text, true, false, ""},
    {"ext", ColumnType::Text, false, false, "'{}'"},
};

constexpr std::string_view kTradesUniqueKey[] = {"account_id", "date", "exchange_id", "trade_id"};
constexpr std::string_view kTradesAccountSeq[] = {"account_id", "seq"};
constexpr std::string_view kTradesAccountDate[] = {"account_id", "date"};
constexpr std::string_view kTradesDateInstr[] = {"date", "instrument_id"};

constexpr IndexSchema kTradesIndexes[] = {
    {"uq_trades_key", std::span<const std::string_view>(kTradesUniqueKey), true},
    {"idx_trades_acct_seq", std::span<const std::string_view>(kTradesAccountSeq), false},
    {"idx_trades_account_date", std::span<const std::string_view>(kTradesAccountDate), false},
    {"idx_trades_date_instr", std::span<const std::string_view>(kTradesDateInstr), false},
};

// positions：当日持仓绝对态（UNIQUE(account_id, date, instrument_id, direction)）
constexpr ColumnSchema kPositionsColumns[] = {
    {"account_id", ColumnType::Text, true, false, ""},
    {"date", ColumnType::Text, true, false, ""},
    {"instrument_id", ColumnType::Text, true, false, ""},
    {"exchange_id", ColumnType::Text, true, false, ""},
    {"direction", ColumnType::Text, true, false, ""},
    {"volume", ColumnType::Integer, false, false, ""},
    {"frozen_volume", ColumnType::Integer, false, false, ""},
    {"today_volume", ColumnType::Integer, false, false, ""},
    {"yd_volume", ColumnType::Integer, false, false, ""},
    {"price", ColumnType::Real, false, false, ""},
    {"seq", ColumnType::Integer, true, false, ""},
    {"gateway", ColumnType::Text, true, false, ""},
    {"ext", ColumnType::Text, false, false, "'{}'"},
};

constexpr std::string_view kPositionsUniqueKey[] = {"account_id", "date", "instrument_id",
                                                    "direction"};
constexpr std::string_view kPositionsAccountSeq[] = {"account_id", "seq"};
constexpr std::string_view kPositionsAccountDate[] = {"account_id", "date"};
constexpr std::string_view kPositionsDateInstr[] = {"date", "instrument_id"};

constexpr IndexSchema kPositionsIndexes[] = {
    {"uq_positions_key", std::span<const std::string_view>(kPositionsUniqueKey), true},
    {"idx_positions_acct_seq", std::span<const std::string_view>(kPositionsAccountSeq), false},
    {"idx_positions_account_date", std::span<const std::string_view>(kPositionsAccountDate), false},
    {"idx_positions_date_instr", std::span<const std::string_view>(kPositionsDateInstr), false},
};

// trading_accounts：当日资金快照（PRIMARY KEY(account_id, date)）
constexpr ColumnSchema kTradingAccountsColumns[] = {
    {"account_id", ColumnType::Text, true, true, ""},
    {"date", ColumnType::Text, true, true, ""},
    {"balance", ColumnType::Real, false, false, ""},
    {"available", ColumnType::Real, false, false, ""},
    {"frozen", ColumnType::Real, false, false, ""},
    {"commission", ColumnType::Real, false, false, ""},
    {"margin", ColumnType::Real, false, false, ""},
    {"withdraw_quota", ColumnType::Real, false, false, ""},
    {"deposit", ColumnType::Real, false, false, ""},
    {"withdraw", ColumnType::Real, false, false, ""},
    {"seq", ColumnType::Integer, true, false, ""},
    {"gateway", ColumnType::Text, true, false, ""},
    {"ext", ColumnType::Text, false, false, "'{}'"},
};

constexpr std::string_view kTradingAccountsAccountSeq[] = {"account_id", "seq"};
constexpr std::string_view kTradingAccountsAccountDate[] = {"account_id", "date"};

constexpr IndexSchema kTradingAccountsIndexes[] = {
    {"idx_taccount_acct_seq", std::span<const std::string_view>(kTradingAccountsAccountSeq), false},
    {"idx_taccount_account_date", std::span<const std::string_view>(kTradingAccountsAccountDate),
     false},
};

// instruments：合约信息（暂留 trade.db；行情域落地后迁 market.db，ADR 0041 §1）
// 28 物理列 = 25 承诺列 + 3 保留列（settlement_method/option_exercise_style/option_series）
constexpr ColumnSchema kInstrumentsColumns[] = {
    {"instrument_id", ColumnType::Text, true, true, ""},
    {"exchange_id", ColumnType::Text, true, false, ""},
    {"symbol", ColumnType::Text, true, false, "''"},
    {"name", ColumnType::Text, false, false, ""},
    {"product_class", ColumnType::Integer, true, false, "0"},
    {"settle_cycle", ColumnType::Integer, true, false, "-1"},
    {"settlement_method", ColumnType::Integer, true, false, "0"},
    {"is_inverse", ColumnType::Integer, true, false, "0"},
    {"currency", ColumnType::Text, true, false, "''"},
    {"base_asset", ColumnType::Text, true, false, "''"},
    {"min_limit_order_volume", ColumnType::Integer, true, false, "0"},
    {"max_limit_order_volume", ColumnType::Integer, true, false, "0"},
    {"volume_multiple", ColumnType::Real, true, false, "0"},
    {"price_tick", ColumnType::Real, true, false, "0"},
    {"volume_step", ColumnType::Real, true, false, "1"},
    {"listed_date", ColumnType::Integer, true, false, "0"},
    {"delisted_date", ColumnType::Integer, true, false, "0"},
    {"option_type", ColumnType::Integer, true, false, "0"},
    {"option_exercise_style", ColumnType::Integer, true, false, "0"},
    {"underlying_id", ColumnType::Text, true, false, "''"},
    {"option_strike", ColumnType::Real, true, false, "0"},
    {"option_series", ColumnType::Text, true, false, "''"},
    {"update_day", ColumnType::Text, false, false, ""},
    {"product_code", ColumnType::Text, true, false, "''"},
    {"min_market_order_volume", ColumnType::Integer, true, false, "0"},
    {"max_market_order_volume", ColumnType::Integer, true, false, "0"},
    {"underlying_multiple", ColumnType::Real, true, false, "0"},
    {"updated_at", ColumnType::Integer, true, false, "0"},
};

// 游标分页排序用表达式索引（18:00 锚点只出现在排序表达式，不改写真实 time；ADR 0041 §1）。
// 破同秒并列：orders 用 order_id、trades 用 trade_id。positions/trading_accounts 无 time 列。
constexpr ExpressionIndexSchema kOrdersExpressionIndexes[] = {
    {"idx_orders_sort",
     "CREATE INDEX IF NOT EXISTS idx_orders_sort ON orders("
     "account_id, date, ((time + 21600) % 86400) DESC, order_id)"},
};

constexpr TableSchema kOrders = {
    .name = "orders",
    .columns = std::span<const ColumnSchema>(kOrdersColumns),
    .indexes = std::span<const IndexSchema>(kOrdersIndexes),
    .expression_indexes = std::span<const ExpressionIndexSchema>(kOrdersExpressionIndexes),
};

constexpr ExpressionIndexSchema kTradesExpressionIndexes[] = {
    {"idx_trades_sort",
     "CREATE INDEX IF NOT EXISTS idx_trades_sort ON trades("
     "account_id, date, ((time + 21600) % 86400) DESC, trade_id)"},
};

constexpr TableSchema kTrades = {
    .name = "trades",
    .columns = std::span<const ColumnSchema>(kTradesColumns),
    .indexes = std::span<const IndexSchema>(kTradesIndexes),
    .expression_indexes = std::span<const ExpressionIndexSchema>(kTradesExpressionIndexes),
};

constexpr TableSchema kPositions = {
    .name = "positions",
    .columns = std::span<const ColumnSchema>(kPositionsColumns),
    .indexes = std::span<const IndexSchema>(kPositionsIndexes),
    .expression_indexes = {},
};

// account_date 索引与 PK(account_id, date) 等效：按 spec §1「四表各建 (account_id, date)」
// 字面保留（该表行量 = 账户数/日，冗余索引成本可忽略）。
constexpr TableSchema kTradingAccounts = {
    .name = "trading_accounts",
    .columns = std::span<const ColumnSchema>(kTradingAccountsColumns),
    .indexes = std::span<const IndexSchema>(kTradingAccountsIndexes),
    .expression_indexes = {},
};

constexpr TableSchema kInstruments = {
    .name = "instruments",
    .columns = std::span<const ColumnSchema>(kInstrumentsColumns),
    .indexes = {},
    .expression_indexes = {},
};

std::string_view column_type_sql(ColumnType type) {
    switch (type) {
        case ColumnType::Integer:
            return "INTEGER";
        case ColumnType::Real:
            return "REAL";
        case ColumnType::Text:
            return "TEXT";
    }
    return "TEXT";
}

/// 建表 DDL：单列主键内联，多列主键用表级 PRIMARY KEY 约束
std::string build_create_table(const TableSchema& table) {
    std::vector<std::string_view> primary_keys;
    for (const ColumnSchema& column : table.columns) {
        if (column.primary_key) {
            primary_keys.push_back(column.name);
        }
    }
    const bool inline_primary_key = primary_keys.size() == 1;

    std::string sql = "CREATE TABLE IF NOT EXISTS ";
    sql += table.name;
    sql += " (";
    bool first = true;
    for (const ColumnSchema& column : table.columns) {
        if (!first) {
            sql += ",";
        }
        first = false;
        sql += "\n    ";
        sql += column.name;
        sql += " ";
        sql += column_type_sql(column.type);
        if (inline_primary_key && column.primary_key) {
            sql += " PRIMARY KEY";
            if (column.not_null) {
                sql += " NOT NULL";
            }
        } else if (column.not_null) {
            sql += " NOT NULL";
        }
        if (!column.default_sql.empty()) {
            sql += " DEFAULT ";
            sql += column.default_sql;
        }
    }
    if (!inline_primary_key && !primary_keys.empty()) {
        sql += ",\n    PRIMARY KEY (";
        for (size_t i = 0; i < primary_keys.size(); ++i) {
            if (i > 0) {
                sql += ", ";
            }
            sql += primary_keys[i];
        }
        sql += ")";
    }
    sql += "\n)";
    return sql;
}

std::string build_create_index(const TableSchema& table, const IndexSchema& index) {
    std::string sql = "CREATE ";
    if (index.unique) {
        sql += "UNIQUE ";
    }
    sql += "INDEX IF NOT EXISTS ";
    sql += index.name;
    sql += " ON ";
    sql += table.name;
    sql += " (";
    for (size_t i = 0; i < index.columns.size(); ++i) {
        if (i > 0) {
            sql += ", ";
        }
        sql += index.columns[i];
    }
    sql += ")";
    return sql;
}

/// 双引号标识符：表名/列名均为声明内的简单标识符，无需转义内部引号。
std::string quote_identifier(std::string_view identifier) {
    std::string quoted = "\"";
    quoted += identifier;
    quoted += "\"";
    return quoted;
}

/// 列级 PRIMARY KEY 的键列（列声明不保证键列连续，需物化为可 span 的列名序列）。
/// 进程内静态缓存：每张表只物化一次、此后只读，返回的 span 在进程生命周期内有效；
/// mutex 只保护首次插入（并发首次调用的 map 写入安全，之后无写）。
std::span<const std::string_view> column_primary_key_columns(const TableSchema& table) {
    using ColumnNames = std::vector<std::string_view>;
    static std::mutex mutex;
    static std::unordered_map<std::string_view, ColumnNames> cache;
    const std::lock_guard<std::mutex> lock(mutex);
    auto it = cache.find(table.name);
    if (it == cache.end()) {
        ColumnNames columns;
        for (const ColumnSchema& column : table.columns) {
            if (column.primary_key) {
                columns.push_back(column.name);
            }
        }
        it = cache.emplace(table.name, std::move(columns)).first;
    }
    return it->second;
}

bool is_key_column(std::span<const std::string_view> key_columns, std::string_view column) {
    return std::find(key_columns.begin(), key_columns.end(), column) != key_columns.end();
}

}  // namespace

const TableSchema& orders_schema() { return kOrders; }
const TableSchema& trades_schema() { return kTrades; }
const TableSchema& positions_schema() { return kPositions; }
const TableSchema& trading_accounts_schema() { return kTradingAccounts; }
const TableSchema& instruments_schema() { return kInstruments; }

std::span<const TableSchema> tables() {
    static const std::array<TableSchema, 5> kAll = {kOrders, kTrades, kPositions, kTradingAccounts,
                                                    kInstruments};
    return kAll;
}

std::string build_schema_ddl() {
    std::string ddl;
    for (const TableSchema& table : tables()) {
        ddl += build_create_table(table);
        ddl += ";\n";
        for (const IndexSchema& index : table.indexes) {
            ddl += build_create_index(table, index);
            ddl += ";\n";
        }
        for (const ExpressionIndexSchema& index : table.expression_indexes) {
            ddl += index.sql;
            ddl += ";\n";
        }
    }
    return ddl;
}

std::span<const std::string_view> business_key_columns(const TableSchema& table) {
    // 列级 PRIMARY KEY 优先（按列声明序）：多列主键的列名不连续，走物化缓存
    for (const ColumnSchema& column : table.columns) {
        if (column.primary_key) {
            return column_primary_key_columns(table);
        }
    }
    // 否则取唯一索引列（按索引声明序；当前每表至多一个业务键）
    for (const IndexSchema& index : table.indexes) {
        if (index.unique) {
            return index.columns;
        }
    }
    return {};
}

std::string build_upsert_sql(const TableSchema& table) {
    const std::span<const std::string_view> key_columns = business_key_columns(table);

    // INSERT INTO "t" ("col", ...) VALUES (:col, ...)
    std::string sql = "INSERT INTO ";
    sql += quote_identifier(table.name);
    sql += " (";
    for (size_t i = 0; i < table.columns.size(); ++i) {
        if (i > 0) {
            sql += ", ";
        }
        sql += quote_identifier(table.columns[i].name);
    }
    sql += ") VALUES (";
    for (size_t i = 0; i < table.columns.size(); ++i) {
        if (i > 0) {
            sql += ", ";
        }
        sql += ":";
        sql += table.columns[i].name;
    }
    sql += ")";

    // 无业务键：退化为纯 INSERT（冲突行为交给 DDL 唯一约束）
    if (key_columns.empty()) {
        return sql;
    }

    // ON CONFLICT ("key", ...) DO UPDATE SET "col"=excluded."col", ...
    sql += " ON CONFLICT (";
    for (size_t i = 0; i < key_columns.size(); ++i) {
        if (i > 0) {
            sql += ", ";
        }
        sql += quote_identifier(key_columns[i]);
    }
    sql += ") DO ";
    if (key_columns.size() == table.columns.size()) {
        // 所有列都是业务键：没有可更新的非键列（防御分支；当前 5 张表不触发）
        sql += "NOTHING";
        return sql;
    }
    sql += "UPDATE SET ";
    bool first = true;
    for (const ColumnSchema& column : table.columns) {
        if (is_key_column(key_columns, column.name)) {
            continue;
        }
        if (!first) {
            sql += ", ";
        }
        first = false;
        sql += quote_identifier(column.name);
        sql += "=excluded.";
        sql += quote_identifier(column.name);
    }
    return sql;
}

}  // namespace dztrader::tdstore
