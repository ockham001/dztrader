/**
 * @file db_query_date_test.cpp
 * @brief `dz_db_query_{order,trade,position,trading_account}` 的 `date` 参数 /
 *        `DZ_DATE_NA` 锚点日 / 返回列集合（业务列 + `seq`）
 *
 * 契约锚点：
 * - ADR 0044 决策 2：四个查询增加 `DzDate date`；`DZ_DATE_NA` = 账户锚点日；
 *   返回业务列 + `seq`，不返回 `gateway` / `ext`。
 * - ADR 0046：锚点日 = 该账户 `positions` 与 `trading_accounts` **两张快照表**的
 *   `MAX(date)`（不含 orders/trades 追加流表）。
 *
 * 建库走 tdstore schema v1 平台声明（单源），SQL 直接对库写入样例行。
 */
#include <gtest/gtest.h>

#include <dztrader/api.h>
#include <dztrader/date_time/date_time.h>
#include <dztrader/error.h>
#include <dztrader/tdstore/trade_db.h>

#include <SQLiteCpp/Database.h>
#include <SQLiteCpp/Statement.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "test_temp_dir.h"

namespace {

/// 交易日字面量助手: `day(2026, 9, 1)` = 距纪元天数 (DzDate)
DzDate day(int y, int m, int d) {
    return dztrader::Date::from_year_month_day(y, m, d).days_since_epoch();
}

/// `date` 参数 / 锚点日 / 列集合的测试夹具。
///
/// 预置两个账户跨两个交易日的数据：
/// - A：20260901 与 20260902 各有委托/成交/持仓/资金（两表快照都有两日）
/// - B：只有 20260901 的快照；另有 20260902 的委托（追加流）——用来说明锚点日
///   **不看** orders/trades（ADR 0046 的核心区分点）
class DbQueryDateTest : public ::testing::Test {
protected:
    std::string db_path_;
    DzDatabase* db_ = nullptr;

    void SetUp() override {
        const auto dir = dztrader::test::unique_temp_dir("dz_test_db_query_date");
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        db_path_ = (dir / "trade.db").string();
        (void)dztrader::tdstore::ensure_trade_db(db_path_);

        SQLite::Database db(db_path_, SQLite::OPEN_READWRITE);
        insert_order(db, "A", "20260901", 1001, 1);
        insert_order(db, "A", "20260902", 1002, 2);
        insert_order(db, "B", "20260901", 2001, 3);
        insert_order(db, "B", "20260902", 2002, 4);  // 追加流推进到 20260902

        insert_trade(db, "A", "20260901", "T1", 1001, 1);
        insert_trade(db, "A", "20260902", "T2", 1002, 2);

        insert_position(db, "A", "20260901", "IF2401", 1);
        insert_position(db, "A", "20260902", "IF2401", 2);
        insert_position(db, "B", "20260901", "IF2402", 3);  // B 快照只到 20260901

        insert_account(db, "A", "20260901", 1);
        insert_account(db, "A", "20260902", 2);
        insert_account(db, "B", "20260901", 3);

        db_ = dz_db_open(db_path_.c_str());
        ASSERT_NE(nullptr, db_) << dz_errmsg();
    }

    void TearDown() override {
        if (db_ != nullptr) {
            dz_db_close(db_);
            db_ = nullptr;
        }
        std::filesystem::remove_all(std::filesystem::path(db_path_).parent_path());
    }

    static void insert_order(SQLite::Database& db,
                             const char* account,
                             const char* date,
                             int64_t order_id,
                             int64_t seq) {
        SQLite::Statement ins(
            db,
            "INSERT INTO orders (account_id, date, order_id, instrument_id, exchange_id,"
            " direction, position_effect, price_type, status, price, volume, volume_traded,"
            " strategy_id, time, avg_price, seq, gateway, ext)"
            " VALUES (?, ?, ?, 'IF2401', 'CFFEX', '0', '1', '0', 'a', 3800.5, 2, 1,"
            " 'stg_demo', 36000, 3800.5, ?, 'dztd_test', '{\"k\":1}')");
        ins.bind(1, account);
        ins.bind(2, date);
        ins.bind(3, order_id);
        ins.bind(4, seq);
        ins.exec();
    }

    static void insert_trade(SQLite::Database& db,
                             const char* account,
                             const char* date,
                             const char* trade_id,
                             int64_t order_id,
                             int64_t seq) {
        SQLite::Statement ins(
            db,
            "INSERT INTO trades (account_id, date, trade_id, order_id, strategy_id,"
            " instrument_id, exchange_id, direction, position_effect, price, volume, time,"
            " seq, gateway, ext)"
            " VALUES (?, ?, ?, ?, 'stg_demo', 'IF2401', 'CFFEX', '0', '1', 3801.5, 1, 36001,"
            " ?, 'dztd_test', '{\"k\":1}')");
        ins.bind(1, account);
        ins.bind(2, date);
        ins.bind(3, trade_id);
        ins.bind(4, order_id);
        ins.bind(5, seq);
        ins.exec();
    }

    static void insert_position(SQLite::Database& db,
                                const char* account,
                                const char* date,
                                const char* instrument_id,
                                int64_t seq) {
        SQLite::Statement ins(
            db,
            "INSERT INTO positions (account_id, date, instrument_id, exchange_id, direction,"
            " volume, frozen_volume, today_volume, yd_volume, price, seq, gateway, ext)"
            " VALUES (?, ?, ?, 'CFFEX', 'L', 5, 0, 5, 0, 3810.0, ?, 'dztd_test', '{}')");
        ins.bind(1, account);
        ins.bind(2, date);
        ins.bind(3, instrument_id);
        ins.bind(4, seq);
        ins.exec();
    }

    static void insert_account(SQLite::Database& db,
                               const char* account,
                               const char* date,
                               int64_t seq) {
        SQLite::Statement ins(
            db,
            "INSERT INTO trading_accounts (account_id, date, balance, available, frozen,"
            " commission, margin, withdraw_quota, deposit, withdraw, seq, gateway, ext)"
            " VALUES (?, ?, 100000.5, 90000.25, 0, 1.5, 1000.0, 500.0, 0, 0, ?, 'dztd_test',"
            " '{}')");
        ins.bind(1, account);
        ins.bind(2, date);
        ins.bind(3, seq);
        ins.exec();
    }

    /// 结果集全部列名
    static std::vector<std::string> column_names(DzResultSet* rs) {
        std::vector<std::string> names;
        const uint32_t count = dz_resultset_column_count(rs);
        names.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            names.emplace_back(dz_resultset_column_name(rs, i));
        }
        return names;
    }

    /// 取某列索引; 不存在返回列数 (越界哨兵)
    static uint32_t column_index(DzResultSet* rs, const char* column) {
        for (uint32_t i = 0; i < dz_resultset_column_count(rs); ++i) {
            if (column == std::string(dz_resultset_column_name(rs, i))) {
                return i;
            }
        }
        return dz_resultset_column_count(rs);
    }

    /// 一次性读完整结果集的指定列（行序 = 结果行序）。
    /// 列不存在时返回 lead 哨兵值 `<missing:name>`，使"列为空"与"列不存在"可区分
    /// （否则投影丢了 date 列时 `EXPECT_TRUE(values.empty())` 会假通过）。
    static std::vector<std::string> column_values(DzResultSet* rs, const char* column) {
        const uint32_t index = column_index(rs, column);
        std::vector<std::string> values;
        if (index == dz_resultset_column_count(rs)) {
            values.emplace_back(std::string("<missing:") + column + ">");
            return values;
        }
        while (dz_resultset_next(rs)) {
            values.emplace_back(dz_resultset_get_string(rs, index));
        }
        return values;
    }

    /// 读完整结果集的 (account_id, date) 两列
    static std::vector<std::pair<std::string, std::string>> account_dates(DzResultSet* rs) {
        const uint32_t account = column_index(rs, "account_id");
        const uint32_t date = column_index(rs, "date");
        std::vector<std::pair<std::string, std::string>> rows;
        if (account == dz_resultset_column_count(rs) || date == dz_resultset_column_count(rs)) {
            return rows;
        }
        while (dz_resultset_next(rs)) {
            rows.emplace_back(dz_resultset_get_string(rs, account),
                              dz_resultset_get_string(rs, date));
        }
        return rows;
    }
};

// AC: 四个查询返回**业务列 + seq**，不含平台内部列 gateway/ext；
// 列序与 tdstore schema v1 声明一致（`seq` 是最后一列）。
TEST_F(DbQueryDateTest, ReturnsBusinessColumnsPlusSeqOnly) {
    struct Case {
        const char* name;
        DzResultSet* rs;
        std::vector<std::string> expected;
    };
    std::vector<Case> cases{
        {"orders",
         dz_db_query_order(db_, "A", nullptr, day(2026, 9, 1)),
         {"account_id", "date", "order_id", "strategy_id", "instrument_id", "exchange_id",
          "direction", "position_effect", "price_type", "status", "price", "volume",
          "volume_traded", "remark", "time", "avg_price", "seq"}},
        {"trades",
         dz_db_query_trade(db_, "A", nullptr, day(2026, 9, 1)),
         {"account_id", "date", "trade_id", "order_id", "strategy_id", "instrument_id",
          "exchange_id", "direction", "position_effect", "price", "volume", "time", "seq"}},
        {"positions",
         dz_db_query_position(db_, "A", nullptr, day(2026, 9, 1)),
         {"account_id", "date", "instrument_id", "exchange_id", "direction", "volume",
          "frozen_volume", "today_volume", "yd_volume", "price", "seq"}},
        {"trading_accounts",
         dz_db_query_trading_account(db_, "A", day(2026, 9, 1)),
         {"account_id", "date", "balance", "available", "frozen", "commission", "margin",
          "withdraw_quota", "deposit", "withdraw", "seq"}},
    };
    for (const Case& c : cases) {
        ASSERT_NE(nullptr, c.rs) << c.name << ": " << dz_errmsg();
        const std::vector<std::string> names = column_names(c.rs);
        EXPECT_EQ(c.expected, names) << c.name;
        ASSERT_FALSE(names.empty()) << c.name;
        EXPECT_EQ("seq", names.back()) << c.name;
        EXPECT_EQ(names.end(), std::find(names.begin(), names.end(), "gateway"))
            << c.name << ": gateway must not be returned";
        EXPECT_EQ(names.end(), std::find(names.begin(), names.end(), "ext"))
            << c.name << ": ext must not be returned";
        dz_resultset_close(c.rs);
    }
}

// AC: 显式 `date` 只返回该交易日行（多日表下"全量语义"不再成立）。
TEST_F(DbQueryDateTest, ExplicitDateFiltersToThatTradingDay) {
    DzResultSet* rs = dz_db_query_order(db_, "A", nullptr, day(2026, 9, 2));
    ASSERT_NE(nullptr, rs) << dz_errmsg();
    EXPECT_EQ(std::vector<std::string>({"20260902"}), column_values(rs, "date"));
    dz_resultset_close(rs);

    rs = dz_db_query_order(db_, "A", nullptr, day(2026, 9, 1));
    ASSERT_NE(nullptr, rs) << dz_errmsg();
    EXPECT_EQ(std::vector<std::string>({"20260901"}), column_values(rs, "date"));
    dz_resultset_close(rs);

    // 无该交易日数据: 空结果集（非错误），列元信息仍按声明填充
    rs = dz_db_query_order(db_, "A", nullptr, day(2026, 8, 31));
    ASSERT_NE(nullptr, rs) << dz_errmsg();
    EXPECT_EQ(0, dz_resultset_status(rs));
    EXPECT_TRUE(column_values(rs, "date").empty());
    dz_resultset_close(rs);
}

// AC: `DZ_DATE_NA` + 指定账户 = 该账户锚点日（两张快照表 MAX(date)）。
// A 快照两日都有 → 锚点为 20260902。
TEST_F(DbQueryDateTest, DateNaWithAccountUsesThatAccountAnchorDay) {
    DzResultSet* rs = dz_db_query_position(db_, "A", nullptr, DZ_DATE_NA);
    ASSERT_NE(nullptr, rs) << dz_errmsg();
    EXPECT_EQ(std::vector<std::string>({"20260902"}), column_values(rs, "date"));
    dz_resultset_close(rs);

    rs = dz_db_query_trading_account(db_, "A", DZ_DATE_NA);
    ASSERT_NE(nullptr, rs) << dz_errmsg();
    EXPECT_EQ(std::vector<std::string>({"20260902"}), column_values(rs, "date"));
    dz_resultset_close(rs);
}

// AC: `DZ_DATE_NA` + 空 `account_id` = **每账户各自**锚点日过滤
// （A → 20260902、B → 20260901；两个账户各回其锚点日的行）。
TEST_F(DbQueryDateTest, DateNaWithoutAccountUsesPerAccountAnchorDay) {
    DzResultSet* rs = dz_db_query_position(db_, nullptr, nullptr, DZ_DATE_NA);
    ASSERT_NE(nullptr, rs) << dz_errmsg();
    // 行序 = seq 升序: A(seq=2) 在 B(seq=3) 之前; 两账户各回自己的锚点日
    const std::vector<std::pair<std::string, std::string>> expected = {{"A", "20260902"},
                                                                       {"B", "20260901"}};
    EXPECT_EQ(expected, account_dates(rs));
    dz_resultset_close(rs);

    rs = dz_db_query_trading_account(db_, nullptr, DZ_DATE_NA);
    ASSERT_NE(nullptr, rs) << dz_errmsg();
    EXPECT_EQ(expected, account_dates(rs));
    dz_resultset_close(rs);
}

// ADR 0046 核心区分点: 锚点日**不含** orders/trades 追加流表。
// B 的委托已推进到 20260902，但快照表只到 20260901 → B 的锚点日仍是 20260901。
TEST_F(DbQueryDateTest, AnchorDayIgnoresAppendStreamTables) {
    DzResultSet* rs = dz_db_query_position(db_, "B", nullptr, DZ_DATE_NA);
    ASSERT_NE(nullptr, rs) << dz_errmsg();
    // 快照表只到 20260901 → B 的锚点日仍是 20260901 (追加流不动锚点)
    EXPECT_EQ(std::vector<std::string>({"20260901"}), column_values(rs, "date"));
    dz_resultset_close(rs);

    // 委托 `DZ_DATE_NA` 也按锚点日过滤（不是该账户委托的最新 date）：
    // B 的委托有 20260901/20260902 两行，锚点 20260901 → 只回 1 行。
    rs = dz_db_query_order(db_, "B", nullptr, DZ_DATE_NA);
    ASSERT_NE(nullptr, rs) << dz_errmsg();
    EXPECT_EQ(std::vector<std::string>({"20260901"}), column_values(rs, "date"));
    dz_resultset_close(rs);
}

// AC: 无数据账户的 `DZ_DATE_NA`（锚点日 = null）→ 空结果集，不是错误。
TEST_F(DbQueryDateTest, DateNaForAccountWithoutSnapshotReturnsEmpty) {
    DzResultSet* rs = dz_db_query_position(db_, "NO_SUCH_ACCOUNT", nullptr, DZ_DATE_NA);
    ASSERT_NE(nullptr, rs) << dz_errmsg();
    EXPECT_EQ(0, dz_resultset_status(rs));
    EXPECT_TRUE(column_values(rs, "date").empty());
    EXPECT_FALSE(column_names(rs).empty()) << "column meta must still be filled";
    dz_resultset_close(rs);

    rs = dz_db_query_trading_account(db_, "NO_SUCH_ACCOUNT", DZ_DATE_NA);
    ASSERT_NE(nullptr, rs) << dz_errmsg();
    EXPECT_TRUE(account_dates(rs).empty());
    dz_resultset_close(rs);
}

// `date` 只能是有效交易日或 DZ_DATE_NA: 非法值（负/非 0 的 epoch day 0 之外的坏值）
// 由上层日期类型保证; 这里锁定 DZ_DATE_NA 与真实日可用, 且返回值与 `seq` 一致。
TEST_F(DbQueryDateTest, DateAndSeqValuesAreConsistent) {
    DzResultSet* rs = dz_db_query_trade(db_, "A", nullptr, day(2026, 9, 2));
    ASSERT_NE(nullptr, rs) << dz_errmsg();
    // trades 列序: 12=seq、2=trade_id、1=date
    ASSERT_TRUE(dz_resultset_next(rs));
    EXPECT_EQ(2, dz_resultset_get_int64(rs, 12));
    EXPECT_STREQ("T2", dz_resultset_get_string(rs, 2));
    EXPECT_STREQ("20260902", dz_resultset_get_string(rs, 1));
    EXPECT_FALSE(dz_resultset_next(rs));
    dz_resultset_close(rs);
}

// 非法 `date`（既不是 DZ_DATE_NA 也不是有效 epoch day）→ NULL + DZ_EC_INVALID_PARAM。
// 覆盖 `add_date_filter` 的错误路径（development-checklist D3: 每个公开 API 的错误路径）。
TEST_F(DbQueryDateTest, InvalidDateRejected) {
    EXPECT_EQ(nullptr, dz_db_query_order(db_, "A", nullptr, -1));
    EXPECT_EQ(DZ_EC_INVALID_PARAM, dz_errcode());
    EXPECT_EQ(nullptr, dz_db_query_position(db_, "A", nullptr, -1));
    EXPECT_EQ(DZ_EC_INVALID_PARAM, dz_errcode());
    EXPECT_EQ(nullptr, dz_db_query_trading_account(db_, "A", -1));
    EXPECT_EQ(DZ_EC_INVALID_PARAM, dz_errcode());
    EXPECT_EQ(nullptr, dz_db_query_trade(db_, "A", nullptr, -1));
    EXPECT_EQ(DZ_EC_INVALID_PARAM, dz_errcode());
}

// `DZ_DATE_NA` + 空账户: 无快照行的账户必须被丢弃（不因"无锚点"而混入其它账户的行）。
// A/B 有锚点行, GHOST 只有追加流(orders)行 → 结果只含 A、B 两行且日期各为其锚点日。
TEST_F(DbQueryDateTest, DateNaEmptyAccountDropsAccountsWithoutSnapshot) {
    SQLite::Database raw(db_path_, SQLite::OPEN_READWRITE);
    insert_order(raw, "GHOST", "20260901", 9001, 50);  // 只有追加流, 无快照
    DzResultSet* rs = dz_db_query_position(db_, nullptr, nullptr, DZ_DATE_NA);
    ASSERT_NE(nullptr, rs) << dz_errmsg();
    const std::vector<std::pair<std::string, std::string>> expected = {{"A", "20260902"},
                                                                       {"B", "20260901"}};
    EXPECT_EQ(expected, account_dates(rs)) << "GHOST has no snapshot row and must be dropped";
    dz_resultset_close(rs);
}

}  // namespace
