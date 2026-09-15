#include <gtest/gtest.h>

#include <dztrader/api.h>
#include <dztrader/error.h>

#include <SQLiteCpp/Database.h>
#include <SQLiteCpp/Statement.h>

#include <cfloat>
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>

namespace {

// 建表 SQL 常量: 与 libs/tdstore/src/schema.cpp 的 migration_v2 建表语句逐字一致
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

// 发现 1 回归 (评审 Important): db_open_readonly 必须设 busy_timeout (与生产写端一致),
// 否则 SDK 水位装载/断档回补在 td Writer 批量提交 (持写锁) 窗口内立即 SQLITE_BUSY →
// 查询返回 NULL (DZ_EC_SYSTEM), 消费端降级不过滤。
// 测试: 另一连接 BEGIN IMMEDIATE 抢写锁, 释放线程 200ms 后 COMMIT — busy_timeout=5000
// 让查询阻塞等待锁释放后成功返回; 无 busy_timeout 则查询立即失败。
TEST_F(DbTest, ReadOnlyQueryWaitsOutWriteLockWindow) {
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
    }

    // 抢占写锁 (BEGIN IMMEDIATE)。
    SQLite::Database locker(db_path_, SQLite::OPEN_READWRITE);
    locker.exec("BEGIN IMMEDIATE");
    // 200ms 后释放写锁。
    std::thread releaser([&locker]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        locker.exec("COMMIT");
    });

    // 全新只读连接 (走 db_open_readonly): busy_timeout 生效 → 阻塞到锁释放后成功查询。
    DzDatabase* ro = dz_db_open(db_path_.c_str());
    ASSERT_NE(nullptr, ro) << dz_errmsg();
    DzResultSet* rs = dz_db_query_position(ro, "A", nullptr);
    releaser.join();
    ASSERT_NE(nullptr, rs) << dz_errmsg();  // 无 busy_timeout 则此处为 NULL (SQLITE_BUSY)
    ASSERT_EQ(0, dz_resultset_status(rs));
    ASSERT_TRUE(dz_resultset_next(rs));
    EXPECT_EQ(5, dz_resultset_get_int64(rs, 5));  // volume
    dz_resultset_close(rs);
    dz_db_close(ro);
}

// REAL 声明列归一化护栏 (评审 Important): 整数值以 INTEGER 存储 (SQLite REAL-affinity 空间优化)
// 时, read_column_value 必须按声明类型归一化为 double, get_float64 返回正确值而非 DBL_MAX。
// 覆盖在役公开路径 dz_db_query_order / dz_db_query_trade。
TEST_F(DbTest, RealDeclaredColumnStoresIntegerValue) {
    SQLite::Database db(db_path_, SQLite::OPEN_READWRITE);
    {
        // 整数值写进 REAL 声明列 (orders.price REAL, 绑定整数 3800 -> INTEGER 存储)
        SQLite::Statement ins(db,
            "INSERT INTO orders (account_id, trading_day, order_id, order_ref, instrument_id,"
            " exchange_id, price, seq)"
            " VALUES (?, '20260901', ?, ?, 'IF2401', 'CFFEX', ?, ?)");
        ins.bind(1, "A");
        ins.bind(2, static_cast<int64_t>(1001));
        ins.bind(3, "r1");
        ins.bind(4, static_cast<int64_t>(3800));
        ins.bind(5, static_cast<int64_t>(1));
        ins.exec();
    }
    {
        SQLite::Statement ins(db,
            "INSERT INTO trades (account_id, trading_day, trade_id, order_id, instrument_id,"
            " exchange_id, price, volume, seq)"
            " VALUES (?, '20260901', ?, ?, 'IF2401', 'CFFEX', ?, ?, ?)");
        ins.bind(1, "A");
        ins.bind(2, "t1");
        ins.bind(3, static_cast<int64_t>(1001));
        ins.bind(4, static_cast<int64_t>(3810));
        ins.bind(5, static_cast<int64_t>(2));
        ins.bind(6, static_cast<int64_t>(1));
        ins.exec();
    }

    // orders.price (索引 13) REAL 存整数 -> get_float64 应返回 3800.0 (非 DBL_MAX)
    DzResultSet* rs = dz_db_query_order(db_, "A", nullptr);
    ASSERT_NE(nullptr, rs) << dz_errmsg();
    ASSERT_EQ(0, dz_resultset_status(rs));
    ASSERT_TRUE(dz_resultset_next(rs));
    EXPECT_DOUBLE_EQ(3800.0, dz_resultset_get_float64(rs, 13));
    EXPECT_NE(DBL_MAX, dz_resultset_get_float64(rs, 13));
    dz_resultset_close(rs);

    // trades.price (索引 9) REAL 存整数 -> get_float64 应返回 3810.0
    rs = dz_db_query_trade(db_, "A", nullptr);
    ASSERT_NE(nullptr, rs) << dz_errmsg();
    ASSERT_EQ(0, dz_resultset_status(rs));
    ASSERT_TRUE(dz_resultset_next(rs));
    EXPECT_DOUBLE_EQ(3810.0, dz_resultset_get_float64(rs, 9));
    EXPECT_NE(DBL_MAX, dz_resultset_get_float64(rs, 9));
    dz_resultset_close(rs);
}
