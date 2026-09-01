#include <gtest/gtest.h>

#include <dztrader/api.h>
#include <dztrader/error.h>

#include <SQLiteCpp/Database.h>
#include <SQLiteCpp/Statement.h>

#include <filesystem>
#include <string>
#include <vector>

namespace {

// 建表 SQL 常量: 与 apps/ctp/td/td_schema.cpp 的 migration_v2 建表语句逐字一致
// (orders/trades 为 v2 重建后的最终形态, 含 seq 列)。schema 变更时两处同步。
constexpr const char* kCreateOrders =
    "CREATE TABLE IF NOT EXISTS orders ("
    "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "    account_id TEXT NOT NULL,"
    "    trading_day TEXT NOT NULL,"
    "    order_id INTEGER NOT NULL,"
    "    order_ref TEXT NOT NULL,"
    "    external_order_id TEXT,"
    "    is_external INTEGER NOT NULL DEFAULT 0,"
    "    instrument_id TEXT NOT NULL,"
    "    exchange_id TEXT NOT NULL,"
    "    direction CHAR(1),"
    "    position_effect CHAR(1),"
    "    price_type CHAR(1),"
    "    status CHAR(1),"
    "    price REAL,"
    "    volume INTEGER,"
    "    volume_traded INTEGER,"
    "    volume_canceled INTEGER,"
    "    insert_time INTEGER,"
    "    update_time INTEGER,"
    "    error_id INTEGER,"
    "    error_msg TEXT,"
    "    strategy_id TEXT,"
    "    remark TEXT,"
    "    seq INTEGER NOT NULL DEFAULT 0,"
    "    UNIQUE(account_id, order_id))";

constexpr const char* kCreateTrades =
    "CREATE TABLE IF NOT EXISTS trades ("
    "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "    account_id TEXT NOT NULL,"
    "    trading_day TEXT NOT NULL,"
    "    trade_id TEXT NOT NULL,"
    "    order_id INTEGER NOT NULL,"
    "    instrument_id TEXT NOT NULL,"
    "    exchange_id TEXT NOT NULL,"
    "    direction CHAR(1),"
    "    position_effect CHAR(1),"
    "    price REAL NOT NULL,"
    "    volume INTEGER NOT NULL,"
    "    trade_time INTEGER,"
    "    trade_date INTEGER,"
    "    commission REAL,"
    "    strategy_id TEXT,"
    "    seq INTEGER NOT NULL DEFAULT 0,"
    "    UNIQUE(account_id, trading_day, trade_id))";

constexpr const char* kCreatePositions =
    "CREATE TABLE IF NOT EXISTS positions ("
    "    account_id TEXT NOT NULL,"
    "    trading_day TEXT NOT NULL,"
    "    instrument_id TEXT NOT NULL,"
    "    exchange_id TEXT NOT NULL,"
    "    direction CHAR(1) NOT NULL,"
    "    volume INTEGER,"
    "    frozen_volume INTEGER,"
    "    today_volume INTEGER,"
    "    yd_volume INTEGER,"
    "    price REAL,"
    "    seq INTEGER NOT NULL DEFAULT 0,"
    "    UNIQUE(account_id, instrument_id, direction))";

constexpr const char* kCreateTradingAccounts =
    "CREATE TABLE IF NOT EXISTS trading_accounts ("
    "    account_id TEXT NOT NULL PRIMARY KEY,"
    "    trading_day TEXT NOT NULL,"
    "    balance REAL, available REAL, frozen REAL,"
    "    commission REAL, margin REAL, withdraw_quota REAL,"
    "    deposit REAL, withdraw REAL,"
    "    seq INTEGER NOT NULL DEFAULT 0)";

constexpr const char* kCreateCommissionRates =
    "CREATE TABLE IF NOT EXISTS commission_rates ("
    "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "    account_id TEXT NOT NULL,"
    "    instrument_id TEXT NOT NULL,"
    "    product_code TEXT NOT NULL,"
    "    exchange_id TEXT NOT NULL,"
    "    open_ratio_by_money REAL,"
    "    open_ratio_by_volume REAL,"
    "    close_ratio_by_money REAL,"
    "    close_ratio_by_volume REAL,"
    "    close_today_ratio_by_money REAL,"
    "    close_today_ratio_by_volume REAL,"
    "    date INTEGER,"
    "    UNIQUE(account_id, date, product_code))";

constexpr const char* kCreateMarginRates =
    "CREATE TABLE IF NOT EXISTS margin_rates ("
    "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "    account_id TEXT NOT NULL,"
    "    instrument_id TEXT NOT NULL,"
    "    product_code TEXT NOT NULL,"
    "    exchange_id TEXT NOT NULL,"
    "    hedge_flag CHAR(1),"
    "    is_relative CHAR(1),"
    "    long_margin_ratio_by_money REAL,"
    "    long_margin_ratio_by_volume REAL,"
    "    short_margin_ratio_by_money REAL,"
    "    short_margin_ratio_by_volume REAL,"
    "    date INTEGER,"
    "    UNIQUE(account_id, date, product_code))";

/// 在临时目录建 td 库 (schema v2) 并写入样例行。
class DbTest : public ::testing::Test {
protected:
    std::string db_path_;
    DzDatabase* db_ = nullptr;

    void SetUp() override {
        const auto dir = std::filesystem::temp_directory_path() / "dz_test_strategy_db";
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        db_path_ = (dir / "td.db").string();

        SQLite::Database db(db_path_, SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
        db.exec(kCreateOrders);
        db.exec(kCreateTrades);
        db.exec(kCreatePositions);
        db.exec(kCreateTradingAccounts);
        db.exec(kCreateCommissionRates);
        db.exec(kCreateMarginRates);

        db_ = dz_db_open(db_path_.c_str());
        ASSERT_NE(nullptr, db_) << "dz_db_open failed: " << dz_errmsg();
    }

    void TearDown() override {
        if (db_ != nullptr) {
            dz_db_close(db_);
            db_ = nullptr;
        }
    }
};

}  // namespace

// 打开不存在的库文件应失败返回 NULL (策略据此降级), 错误码为 DZ_EC_SYSTEM
// (控制器裁决: 不新增 DZ_EC_DB_* 公开错误码, open/query 失败用 DZ_EC_SYSTEM + 描述)。
TEST_F(DbTest, OpenReadOnlyMissingFileFails) {
    EXPECT_EQ(nullptr, dz_db_open("/nonexistent/dz_does_not_exist.db"));
    EXPECT_EQ(DZ_EC_SYSTEM, dz_errcode());
}

// NULL 参数 (path) 应返回 NULL 且错误码 DZ_EC_INVALID_PARAM。
TEST_F(DbTest, OpenNullPathFails) {
    EXPECT_EQ(nullptr, dz_db_open(nullptr));
    EXPECT_EQ(DZ_EC_INVALID_PARAM, dz_errcode());
}

// 按账户过滤: 2 账户各 1 行, 查账户 A 只回 1 行, 列值断言。
// orders 表列序 (SELECT *): 0=id 1=account_id 2=trading_day 3=order_id 4=order_ref
//   5=external_order_id 6=is_external 7=instrument_id 8=exchange_id 9=direction
//   10=position_effect 11=price_type 12=status 13=price 14=volume 15=volume_traded
//   16=volume_canceled 17=insert_time 18=update_time 19=error_id 20=error_msg
//   21=strategy_id 22=remark 23=seq
TEST_F(DbTest, QueryOrderFiltersByAccount) {
    SQLite::Database db(db_path_, SQLite::OPEN_READWRITE);
    {
        SQLite::Statement ins(db,
            "INSERT INTO orders (account_id, trading_day, order_id, order_ref, instrument_id,"
            " exchange_id, direction, status, price, volume, seq)"
            " VALUES (?, '20260901', ?, ?, ?, 'CFFEX', '0', 'a', ?, ?, ?)");
        ins.bind(1, "A");
        ins.bind(2, static_cast<int64_t>(1001));
        ins.bind(3, "r1");
        ins.bind(4, "IF2401");
        ins.bind(5, 3800.5);
        ins.bind(6, static_cast<int64_t>(2));
        ins.bind(7, static_cast<int64_t>(1));
        ins.exec();
        ins.reset();
        ins.bind(1, "B");
        ins.bind(2, static_cast<int64_t>(2001));
        ins.bind(3, "r2");
        ins.bind(4, "IF2402");
        ins.bind(5, 3900.5);
        ins.bind(6, static_cast<int64_t>(3));
        ins.bind(7, static_cast<int64_t>(2));
        ins.exec();
    }

    DzResultSet* rs = dz_db_query_order(db_, "A", nullptr);
    ASSERT_NE(nullptr, rs) << dz_errmsg();
    ASSERT_EQ(0, dz_resultset_status(rs));
    ASSERT_EQ(24u, dz_resultset_column_count(rs));

    ASSERT_TRUE(dz_resultset_next(rs));
    EXPECT_EQ(1001, dz_resultset_get_int64(rs, 3));      // order_id
    EXPECT_STREQ("IF2401", dz_resultset_get_string(rs, 7));  // instrument_id
    EXPECT_DOUBLE_EQ(3800.5, dz_resultset_get_float64(rs, 13));  // price
    EXPECT_EQ(2, dz_resultset_get_int64(rs, 14));        // volume
    EXPECT_STREQ("A", dz_resultset_get_string(rs, 1));   // account_id
    EXPECT_EQ(1, dz_resultset_get_int64(rs, 23));        // seq
    EXPECT_FALSE(dz_resultset_next(rs));                 // 只回 1 行
    dz_resultset_close(rs);

    // 账户 + 合约双条件过滤
    rs = dz_db_query_order(db_, "A", "IF9999");
    ASSERT_NE(nullptr, rs);
    EXPECT_FALSE(dz_resultset_next(rs));
    dz_resultset_close(rs);
}

// 新表查询: positions + trading_accounts。
// positions 列序 (SELECT *): 0=account_id 1=trading_day 2=instrument_id 3=exchange_id
//   4=direction 5=volume 6=frozen_volume 7=today_volume 8=yd_volume 9=price 10=seq
// trading_accounts 列序: 0=account_id 1=trading_day 2=balance 3=available 4=frozen
//   5=commission 6=margin 7=withdraw_quota 8=deposit 9=withdraw 10=seq
TEST_F(DbTest, QueryPositionAndTradingAccount) {
    SQLite::Database db(db_path_, SQLite::OPEN_READWRITE);
    {
        SQLite::Statement ins(db,
            "INSERT INTO positions (account_id, trading_day, instrument_id, exchange_id,"
            " direction, volume, price, seq) VALUES (?, '20260901', ?, 'CFFEX', 'L', ?, ?, ?)");
        ins.bind(1, "A");
        ins.bind(2, "IF2401");
        ins.bind(3, static_cast<int64_t>(5));
        ins.bind(4, 3810.0);
        ins.bind(5, static_cast<int64_t>(1));
        ins.exec();
        ins.reset();
        ins.bind(1, "B");
        ins.bind(2, "IF2402");
        ins.bind(3, static_cast<int64_t>(7));
        ins.bind(4, 3910.0);
        ins.bind(5, static_cast<int64_t>(2));
        ins.exec();
    }
    {
        SQLite::Statement ins(db,
            "INSERT INTO trading_accounts (account_id, trading_day, balance, available, seq)"
            " VALUES (?, '20260901', ?, ?, ?)");
        ins.bind(1, "A");
        ins.bind(2, 100000.5);
        ins.bind(3, 90000.25);
        ins.bind(4, static_cast<int64_t>(1));
        ins.exec();
        ins.reset();
        ins.bind(1, "B");
        ins.bind(2, 200000.5);
        ins.bind(3, 190000.25);
        ins.bind(4, static_cast<int64_t>(2));
        ins.exec();
    }

    DzResultSet* rs = dz_db_query_position(db_, "A", nullptr);
    ASSERT_NE(nullptr, rs);
    ASSERT_EQ(0, dz_resultset_status(rs));
    ASSERT_EQ(11u, dz_resultset_column_count(rs));
    ASSERT_TRUE(dz_resultset_next(rs));
    EXPECT_STREQ("IF2401", dz_resultset_get_string(rs, 2));  // instrument_id
    EXPECT_STREQ("L", dz_resultset_get_string(rs, 4));       // direction
    EXPECT_EQ(5, dz_resultset_get_int64(rs, 5));             // volume
    EXPECT_DOUBLE_EQ(3810.0, dz_resultset_get_float64(rs, 9));  // price
    EXPECT_EQ(1, dz_resultset_get_int64(rs, 10));            // seq
    EXPECT_FALSE(dz_resultset_next(rs));
    dz_resultset_close(rs);

    rs = dz_db_query_trading_account(db_, "A");
    ASSERT_NE(nullptr, rs);
    ASSERT_EQ(0, dz_resultset_status(rs));
    ASSERT_EQ(11u, dz_resultset_column_count(rs));
    ASSERT_TRUE(dz_resultset_next(rs));
    EXPECT_STREQ("A", dz_resultset_get_string(rs, 0));       // account_id
    EXPECT_DOUBLE_EQ(100000.5, dz_resultset_get_float64(rs, 2));  // balance
    EXPECT_DOUBLE_EQ(90000.25, dz_resultset_get_float64(rs, 3));  // available
    EXPECT_EQ(1, dz_resultset_get_int64(rs, 10));            // seq
    EXPECT_FALSE(dz_resultset_next(rs));
    dz_resultset_close(rs);
}

// 结构化查询: filter JSON 范围算子 $gte/$lt 按 seq 过滤, 行序 = seq 序 (回补路径依赖)。
TEST_F(DbTest, GenericQuerySupportsSeqRange) {
    SQLite::Database db(db_path_, SQLite::OPEN_READWRITE);
    {
        SQLite::Statement ins(db,
            "INSERT INTO orders (account_id, trading_day, order_id, order_ref, instrument_id,"
            " exchange_id, seq) VALUES ('A', '20260901', ?, ?, 'IF2401', 'CFFEX', ?)");
        for (int64_t seq = 1; seq <= 5; ++seq) {
            ins.bind(1, static_cast<int64_t>(1000 + seq));
            ins.bind(2, "r" + std::to_string(seq));
            ins.bind(3, seq);
            ins.exec();
            ins.reset();
        }
    }

    DzResultSet* rs = dz_db_query(db_, "order", "{\"seq\": {\"$gte\": 1, \"$lt\": 4}}", 0);
    ASSERT_NE(nullptr, rs) << dz_errmsg();
    ASSERT_EQ(0, dz_resultset_status(rs));

    int64_t last = 0;
    int count = 0;
    while (dz_resultset_next(rs)) {
        const int64_t seq = dz_resultset_get_int64(rs, 23);
        EXPECT_GT(seq, last);  // 行序 = seq 序
        last = seq;
        ++count;
    }
    EXPECT_EQ(3, count);  // seq 1,2,3
    dz_resultset_close(rs);
}

// 结构化查询算子全覆盖: $gte/$lt/$lte/$gt/$in + 简单相等 + 多字段 AND。
// 数据: 5 行, seq 1..5, volume 10/20/30/40/50。
//   $lte/$gt 组合            -> seq 2..4 (seq 1,2,3,4 中 >1)
//   $in                       -> volume IN (20,40) -> seq 2,4
//   instrument_id 简单相等     -> IF2401 全部 5 行
TEST_F(DbTest, GenericQuerySupportsAllOperators) {
    SQLite::Database db(db_path_, SQLite::OPEN_READWRITE);
    {
        SQLite::Statement ins(db,
            "INSERT INTO orders (account_id, trading_day, order_id, order_ref, instrument_id,"
            " exchange_id, volume, seq) VALUES ('A', '20260901', ?, ?, 'IF2401', 'CFFEX', ?, ?)");
        for (int64_t seq = 1; seq <= 5; ++seq) {
            ins.bind(1, static_cast<int64_t>(1000 + seq));
            ins.bind(2, "r" + std::to_string(seq));
            ins.bind(3, static_cast<int64_t>(seq * 10));
            ins.bind(4, seq);
            ins.exec();
            ins.reset();
        }
    }

    auto* rs = dz_db_query(db_, "order", "{\"seq\": {\"$lte\": 4, \"$gt\": 1}}", 0);
    ASSERT_NE(nullptr, rs) << dz_errmsg();
    std::vector<int64_t> got;
    while (dz_resultset_next(rs)) {
        got.push_back(dz_resultset_get_int64(rs, 23));
    }
    dz_resultset_close(rs);
    EXPECT_EQ((std::vector<int64_t>{2, 3, 4}), got);

    rs = dz_db_query(db_, "order", "{\"volume\": {\"$in\": [20, 40]}}", 0);
    ASSERT_NE(nullptr, rs) << dz_errmsg();
    got.clear();
    while (dz_resultset_next(rs)) {
        got.push_back(dz_resultset_get_int64(rs, 23));
    }
    dz_resultset_close(rs);
    EXPECT_EQ((std::vector<int64_t>{2, 4}), got);

    // 多字段 AND + 简单相等
    rs = dz_db_query(db_, "order", "{\"account_id\": \"A\", \"instrument_id\": \"IF9999\"}", 0);
    ASSERT_NE(nullptr, rs) << dz_errmsg();
    EXPECT_FALSE(dz_resultset_next(rs));
    dz_resultset_close(rs);

    // 未知资源 -> NULL + DZ_EC_INVALID_PARAM
    rs = dz_db_query(db_, "not_a_resource", "{}", 0);
    EXPECT_EQ(nullptr, rs);
    EXPECT_EQ(DZ_EC_INVALID_PARAM, dz_errcode());
}

// 结构化查询: filter 为 NULL 或无过滤 -> 全量返回。
TEST_F(DbTest, GenericQueryNullFilterReturnsAll) {
    SQLite::Database db(db_path_, SQLite::OPEN_READWRITE);
    {
        SQLite::Statement ins(db,
            "INSERT INTO orders (account_id, trading_day, order_id, order_ref, instrument_id,"
            " exchange_id, seq) VALUES ('A', '20260901', ?, ?, 'IF2401', 'CFFEX', ?)");
        ins.bind(1, static_cast<int64_t>(1));
        ins.bind(2, "r1");
        ins.bind(3, static_cast<int64_t>(1));
        ins.exec();
        ins.reset();
        ins.bind(1, static_cast<int64_t>(2));
        ins.bind(2, "r2");
        ins.bind(3, static_cast<int64_t>(2));
        ins.exec();
    }

    DzResultSet* rs = dz_db_query(db_, "order", nullptr, 0);
    ASSERT_NE(nullptr, rs) << dz_errmsg();
    ASSERT_EQ(0, dz_resultset_status(rs));
    int count = 0;
    while (dz_resultset_next(rs)) {
        ++count;
    }
    EXPECT_EQ(2, count);
    dz_resultset_close(rs);
}

// 空结果集: 列元信息仍正确 (声明类型为准, 不依赖首行实际值)。
// TEXT 列 (instrument_id/account_id) 应报 DZ_COL_TYPE_STRING, 数值列 (seq) 报 INT64。
TEST_F(DbTest, EmptyResultKeepsColumnTypes) {
    // 库为空 (SetUp 只建表无数据) -> 无条件查询返回空结果集
    DzResultSet* rs = dz_db_query(db_, "order", "{\"account_id\": \"NO_SUCH\"}", 0);
    ASSERT_NE(nullptr, rs) << dz_errmsg();
    ASSERT_EQ(0, dz_resultset_status(rs));
    ASSERT_EQ(24u, dz_resultset_column_count(rs));
    EXPECT_EQ(DZ_COL_TYPE_STRING, dz_resultset_column_type(rs, 1));   // account_id TEXT
    EXPECT_EQ(DZ_COL_TYPE_STRING, dz_resultset_column_type(rs, 7));   // instrument_id TEXT
    EXPECT_EQ(DZ_COL_TYPE_INT64, dz_resultset_column_type(rs, 23));   // seq INTEGER
    EXPECT_STREQ("account_id", dz_resultset_column_name(rs, 1));
    EXPECT_STREQ("seq", dz_resultset_column_name(rs, 23));
    EXPECT_FALSE(dz_resultset_next(rs));
    dz_resultset_close(rs);
}

// commission/margin 表无 seq 列 (非 seq 跟踪表), 条件查询不得 ORDER BY seq (否则
// "no such column: seq" 报错)。验证 dz_db_query_commission 可查询且按账户过滤。
TEST_F(DbTest, QueryCommissionNoSeqColumn) {
    SQLite::Database db(db_path_, SQLite::OPEN_READWRITE);
    {
        SQLite::Statement ins(db,
            "INSERT INTO commission_rates (account_id, instrument_id, product_code, exchange_id,"
            " open_ratio_by_volume, close_ratio_by_volume, date)"
            " VALUES (?, 'IF2401', 'IF', 'CFFEX', ?, ?, ?)");
        ins.bind(1, "A");
        ins.bind(2, 0.3);
        ins.bind(3, 0.3);
        ins.bind(4, static_cast<int64_t>(19736));
        ins.exec();
        ins.reset();
        ins.bind(1, "B");
        ins.bind(2, 0.5);
        ins.bind(3, 0.5);
        ins.bind(4, static_cast<int64_t>(19736));
        ins.exec();
    }

    DzResultSet* rs = dz_db_query_commission(db_, "A", nullptr);
    ASSERT_NE(nullptr, rs) << dz_errmsg();
    ASSERT_EQ(0, dz_resultset_status(rs));
    ASSERT_EQ(12u, dz_resultset_column_count(rs));
    ASSERT_TRUE(dz_resultset_next(rs));
    EXPECT_STREQ("A", dz_resultset_get_string(rs, 1));           // account_id
    EXPECT_STREQ("IF2401", dz_resultset_get_string(rs, 2));      // instrument_id
    EXPECT_DOUBLE_EQ(0.3, dz_resultset_get_float64(rs, 6));      // open_ratio_by_volume
    EXPECT_DOUBLE_EQ(0.3, dz_resultset_get_float64(rs, 8));      // close_ratio_by_volume
    EXPECT_EQ(19736, dz_resultset_get_int64(rs, 11));            // date
    EXPECT_FALSE(dz_resultset_next(rs));
    dz_resultset_close(rs);
}
