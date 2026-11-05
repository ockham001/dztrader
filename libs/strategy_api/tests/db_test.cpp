#include <gtest/gtest.h>

#include <dztrader/api.h>
#include <dztrader/core/env.h>
#include <dztrader/core/path.h>
#include <dztrader/date_time/date_time.h>
#include <dztrader/db/database.h>
#include <dztrader/error.h>
#include <dztrader/shm/channel_meta.h>
#include <dztrader/shm/frame_view.h>
#include <dztrader/shm/writer.h>
#include <dztrader/struct.h>
#include <dztrader/tdstore/trade_db.h>

#include <SQLiteCpp/Database.h>
#include <SQLiteCpp/Statement.h>

#include <cfloat>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <thread>

#include "db_database.h"
#include "td_schema.h"
#include "test_temp_dir.h"

namespace {

/// 交易日字面量助手: `day(2026, 9, 1)` = 距纪元天数 (DzDate)
DzDate day(int y, int m, int d) {
    return dztrader::Date::from_year_month_day(y, m, d).days_since_epoch();
}

/// 在临时目录建 td 库 (schema_catalog 声明 + 驱动迁移) 并写入样例行。
class DbTest : public ::testing::Test {
protected:
    std::string db_path_;
    DzDatabase* db_ = nullptr;

    void SetUp() override {
        const auto dir = dztrader::test::unique_temp_dir("dz_test_strategy_db");
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        db_path_ = (dir / "trade.db").string();

        // 交易库 schema v1 由 tdstore 建库门面创建 (master 同路径; 测试直接用)。
        (void)dztrader::tdstore::ensure_trade_db(db_path_);

        // 回滚日志模式 (驱动默认 WAL): 本套件含写锁阻塞读的 busy_timeout 用例 — WAL 下写锁
        // 不阻塞读快照 (无法触发 SQLITE_BUSY), 与迁移前 fixture 的默认日志模式一致。
        auto database = dztrader::db::Database::open(
            dztrader::db::Config{.backend = "sqlite",
                                 .options = {{"path", db_path_}, {"journal_mode", "delete"}}},
            dztrader::db::td_schemas());
        database->migrate();

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

// 打开不存在的库文件应失败返回 NULL (策略据此降级), 错误码为 DZ_EC_DB_OPEN_FAILED。
TEST_F(DbTest, OpenReadOnlyMissingFileFails) {
    EXPECT_EQ(nullptr, dz_db_open("/nonexistent/dz_does_not_exist.db"));
    EXPECT_EQ(DZ_EC_DB_OPEN_FAILED, dz_errcode());
}

// NULL 参数 (path) 应返回 NULL 且错误码 DZ_EC_INVALID_PARAM。
TEST_F(DbTest, OpenNullPathFails) {
    EXPECT_EQ(nullptr, dz_db_open(nullptr));
    EXPECT_EQ(DZ_EC_INVALID_PARAM, dz_errcode());
}

// 按账户过滤: 2 账户各 1 行, 查账户 A 只回 1 行, 列值断言。
// orders C API 列序 = 业务列 + seq（不含平台内部列 gateway/ext, ADR 0044 决策 2）:
//   0=account_id 1=date 2=order_id 3=strategy_id 4=instrument_id 5=exchange_id
//   6=direction 7=position_effect 8=price_type 9=status 10=price 11=volume
//   12=volume_traded 13=remark 14=time 15=avg_price 16=seq
TEST_F(DbTest, QueryOrderFiltersByAccount) {
    SQLite::Database db(db_path_, SQLite::OPEN_READWRITE);
    {
        SQLite::Statement ins(
            db,
            "INSERT INTO orders (account_id, date, order_id, instrument_id, exchange_id,"
            " direction, status, price, volume, seq, gateway)"
            " VALUES (?, '20260901', ?, ?, 'CFFEX', '0', 'a', ?, ?, ?, 'dztd_test')");
        ins.bind(1, "A");
        ins.bind(2, static_cast<int64_t>(1001));
        ins.bind(3, "IF2401");
        ins.bind(4, 3800.5);
        ins.bind(5, static_cast<int64_t>(2));
        ins.bind(6, static_cast<int64_t>(1));
        ins.exec();
        ins.reset();
        ins.bind(1, "B");
        ins.bind(2, static_cast<int64_t>(2001));
        ins.bind(3, "IF2402");
        ins.bind(4, 3900.5);
        ins.bind(5, static_cast<int64_t>(3));
        ins.bind(6, static_cast<int64_t>(2));
        ins.exec();
    }

    DzResultSet* rs = dz_db_query_order(db_, "A", nullptr, day(2026, 9, 1));
    ASSERT_NE(nullptr, rs) << dz_errmsg();
    ASSERT_EQ(0, dz_resultset_status(rs));
    ASSERT_EQ(17u, dz_resultset_column_count(rs));

    ASSERT_TRUE(dz_resultset_next(rs));
    EXPECT_EQ(1001, dz_resultset_get_int64(rs, 2));              // order_id
    EXPECT_STREQ("IF2401", dz_resultset_get_string(rs, 4));      // instrument_id
    EXPECT_DOUBLE_EQ(3800.5, dz_resultset_get_float64(rs, 10));  // price
    EXPECT_EQ(2, dz_resultset_get_int64(rs, 11));                // volume
    EXPECT_STREQ("A", dz_resultset_get_string(rs, 0));           // account_id
    EXPECT_EQ(1, dz_resultset_get_int64(rs, 16));                // seq
    EXPECT_FALSE(dz_resultset_next(rs));                         // 只回 1 行
    dz_resultset_close(rs);

    // 账户 + 合约双条件过滤
    rs = dz_db_query_order(db_, "A", "IF9999", day(2026, 9, 1));
    ASSERT_NE(nullptr, rs);
    EXPECT_FALSE(dz_resultset_next(rs));
    dz_resultset_close(rs);
}

// 新表查询: positions + trading_accounts（均只回业务列 + seq, 无 gateway/ext）。
// positions 列序: 0=account_id 1=date 2=instrument_id 3=exchange_id 4=direction
//   5=volume 6=frozen_volume 7=today_volume 8=yd_volume 9=price 10=seq
// trading_accounts 列序: 0=account_id 1=date 2=balance 3=available 4=frozen
//   5=commission 6=margin 7=withdraw_quota 8=deposit 9=withdraw 10=seq
TEST_F(DbTest, QueryPositionAndTradingAccount) {
    SQLite::Database db(db_path_, SQLite::OPEN_READWRITE);
    {
        SQLite::Statement ins(db,
                              "INSERT INTO positions (account_id, date, instrument_id, exchange_id,"
                              " direction, volume, price, seq, gateway)"
                              " VALUES (?, '20260901', ?, 'CFFEX', 'L', ?, ?, ?, 'dztd_test')");
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
        SQLite::Statement ins(
            db,
            "INSERT INTO trading_accounts (account_id, date, balance, available, seq, gateway)"
            " VALUES (?, '20260901', ?, ?, ?, 'dztd_test')");
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

    DzResultSet* rs = dz_db_query_position(db_, "A", nullptr, day(2026, 9, 1));
    ASSERT_NE(nullptr, rs);
    ASSERT_EQ(0, dz_resultset_status(rs));
    ASSERT_EQ(11u, dz_resultset_column_count(rs));
    ASSERT_TRUE(dz_resultset_next(rs));
    EXPECT_STREQ("IF2401", dz_resultset_get_string(rs, 2));     // instrument_id
    EXPECT_STREQ("L", dz_resultset_get_string(rs, 4));          // direction
    EXPECT_EQ(5, dz_resultset_get_int64(rs, 5));                // volume
    EXPECT_DOUBLE_EQ(3810.0, dz_resultset_get_float64(rs, 9));  // price
    EXPECT_EQ(1, dz_resultset_get_int64(rs, 10));               // seq
    EXPECT_FALSE(dz_resultset_next(rs));
    dz_resultset_close(rs);

    rs = dz_db_query_trading_account(db_, "A", day(2026, 9, 1));
    ASSERT_NE(nullptr, rs);
    ASSERT_EQ(0, dz_resultset_status(rs));
    ASSERT_EQ(11u, dz_resultset_column_count(rs));
    ASSERT_TRUE(dz_resultset_next(rs));
    EXPECT_STREQ("A", dz_resultset_get_string(rs, 0));            // account_id
    EXPECT_DOUBLE_EQ(100000.5, dz_resultset_get_float64(rs, 2));  // balance
    EXPECT_DOUBLE_EQ(90000.25, dz_resultset_get_float64(rs, 3));  // available
    EXPECT_EQ(1, dz_resultset_get_int64(rs, 10));                 // seq
    EXPECT_FALSE(dz_resultset_next(rs));
    dz_resultset_close(rs);
}

// NULL 句柄: 四个查询 API 均返回 NULL 且错误码 DZ_EC_INVALID_PARAM (契约 instrument §7 同语义)。
TEST_F(DbTest, QueryNullHandleRejected) {
    EXPECT_EQ(nullptr, dz_db_query_order(nullptr, "A", nullptr, DZ_DATE_NA));
    EXPECT_EQ(DZ_EC_INVALID_PARAM, dz_errcode());
    EXPECT_EQ(nullptr, dz_db_query_trade(nullptr, "A", nullptr, DZ_DATE_NA));
    EXPECT_EQ(DZ_EC_INVALID_PARAM, dz_errcode());
    EXPECT_EQ(nullptr, dz_db_query_position(nullptr, "A", nullptr, DZ_DATE_NA));
    EXPECT_EQ(DZ_EC_INVALID_PARAM, dz_errcode());
    EXPECT_EQ(nullptr, dz_db_query_trading_account(nullptr, "A", DZ_DATE_NA));
    EXPECT_EQ(DZ_EC_INVALID_PARAM, dz_errcode());
}

// 发现 1 回归 (评审 Important): db_open_readonly 必须设 busy_timeout (与生产写端一致),
// 否则 SDK 水位装载/断档回补在 td Writer 批量提交 (持写锁) 窗口内立即 SQLITE_BUSY →
// 查询返回 NULL (DZ_EC_SYSTEM), 消费端降级不过滤。
// 前置: fixture 以回滚日志模式建库 (WAL 下写锁不阻塞读快照, BEGIN EXCLUSIVE 无法触发
// SQLITE_BUSY)。测试: 只读连接先开启 (走 db_open_readonly, busy_timeout=5000 已生效),
// 另一连接 BEGIN EXCLUSIVE 抢写锁, 释放线程 200ms 后 COMMIT — 查询阻塞等待锁释放后
// 成功返回; 无 busy_timeout 则查询在持锁窗口内立即 SQLITE_BUSY 失败。
TEST_F(DbTest, ReadOnlyQueryWaitsOutWriteLockWindow) {
    SQLite::Database db(db_path_, SQLite::OPEN_READWRITE);
    {
        SQLite::Statement ins(db,
                              "INSERT INTO positions (account_id, date, instrument_id, exchange_id,"
                              " direction, volume, price, seq, gateway)"
                              " VALUES (?, '20260901', ?, 'CFFEX', 'L', ?, ?, ?, 'dztd_test')");
        ins.bind(1, "A");
        ins.bind(2, "IF2401");
        ins.bind(3, static_cast<int64_t>(5));
        ins.bind(4, 3810.0);
        ins.bind(5, static_cast<int64_t>(1));
        ins.exec();
    }

    // 先开只读连接 (走 db_open_readonly): busy_timeout 在抢锁前已设置;
    // 只读连接无法转 WAL, 库保持回滚日志模式。
    DzDatabase* ro = dz_db_open(db_path_.c_str());
    ASSERT_NE(nullptr, ro) << dz_errmsg();

    // 抢占写锁 (回滚日志模式下 BEGIN EXCLUSIVE 阻塞新读者 →
    // 无 busy_timeout 时查询立即 SQLITE_BUSY)。
    SQLite::Database locker(db_path_, SQLite::OPEN_READWRITE);
    locker.exec("BEGIN EXCLUSIVE");
    // 200ms 后释放写锁。
    std::thread releaser([&locker]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        locker.exec("COMMIT");
    });

    // busy_timeout 生效 → 阻塞到锁释放后成功查询。
    DzResultSet* rs = dz_db_query_position(ro, "A", nullptr, day(2026, 9, 1));
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
        SQLite::Statement ins(
            db,
            "INSERT INTO orders (account_id, date, order_id, instrument_id, exchange_id,"
            " price, seq, gateway)"
            " VALUES (?, '20260901', ?, 'IF2401', 'CFFEX', ?, ?, 'dztd_test')");
        ins.bind(1, "A");
        ins.bind(2, static_cast<int64_t>(1001));
        ins.bind(3, static_cast<int64_t>(3800));
        ins.bind(4, static_cast<int64_t>(1));
        ins.exec();
    }
    {
        SQLite::Statement ins(
            db,
            "INSERT INTO trades (account_id, date, trade_id, order_id, instrument_id,"
            " exchange_id, price, volume, seq, gateway)"
            " VALUES (?, '20260901', ?, ?, 'IF2401', 'CFFEX', ?, ?, ?, 'dztd_test')");
        ins.bind(1, "A");
        ins.bind(2, "t1");
        ins.bind(3, static_cast<int64_t>(1001));
        ins.bind(4, static_cast<int64_t>(3810));
        ins.bind(5, static_cast<int64_t>(2));
        ins.bind(6, static_cast<int64_t>(1));
        ins.exec();
    }

    // orders.price (索引 10) REAL 存整数 -> get_float64 应返回 3800.0 (非 DBL_MAX)
    DzResultSet* rs = dz_db_query_order(db_, "A", nullptr, day(2026, 9, 1));
    ASSERT_NE(nullptr, rs) << dz_errmsg();
    ASSERT_EQ(0, dz_resultset_status(rs));
    ASSERT_TRUE(dz_resultset_next(rs));
    EXPECT_DOUBLE_EQ(3800.0, dz_resultset_get_float64(rs, 10));
    EXPECT_NE(DBL_MAX, dz_resultset_get_float64(rs, 10));
    dz_resultset_close(rs);

    // trades.price (索引 9) REAL 存整数 -> get_float64 应返回 3810.0
    rs = dz_db_query_trade(db_, "A", nullptr, day(2026, 9, 1));
    ASSERT_NE(nullptr, rs) << dz_errmsg();
    ASSERT_EQ(0, dz_resultset_status(rs));
    ASSERT_TRUE(dz_resultset_next(rs));
    EXPECT_DOUBLE_EQ(3810.0, dz_resultset_get_float64(rs, 9));
    EXPECT_NE(DBL_MAX, dz_resultset_get_float64(rs, 9));
    dz_resultset_close(rs);
}

/* ── dz_db_open_default (ADR 0044 决策 3: 固定本地 trade.db + 懒打开) ── */

namespace {

/// 建交易库 (tdstore schema v1 建库门面) 到指定路径。
void create_td_db(const std::string& path) { (void)dztrader::tdstore::ensure_trade_db(path); }

/// 向 orders 表写一行 (账户 + order_id + seq + date)。
void insert_order_row(const std::string& path,
                      const char* account,
                      int64_t order_id,
                      int64_t seq,
                      const char* db_date = "20260901") {
    SQLite::Database db(path, SQLite::OPEN_READWRITE);
    SQLite::Statement ins(
        db,
        "INSERT INTO orders (account_id, date, order_id, instrument_id,"
        " exchange_id, direction, status, price, volume, seq, gateway)"
        " VALUES (?, ?, ?, 'IF2401', 'CFFEX', '0', 'a', 3800.5, 2, ?, 'dztd_test')");
    ins.bind(1, account);
    ins.bind(2, db_date);
    ins.bind(3, order_id);
    ins.bind(4, seq);
    ins.exec();
}

/// dz_db_open_default 套件: 自管临时 DZTRADER_HOME (AGENTS.md 硬门禁 2 的 SetUp 豁免)。
class DbOpenDefaultTest : public ::testing::Test {
protected:
    std::string home_;
    std::string db_path_;
    std::optional<std::string> orig_home_;

    void SetUp() override {
        home_ = dztrader::test::unique_temp_dir("dz_test_db_open_default").string();
        std::filesystem::remove_all(home_);
        std::filesystem::create_directories(home_ + "/db");
        orig_home_ = dztrader::env::get("DZTRADER_HOME");
        dztrader::env::set("DZTRADER_HOME", home_);
        dztrader::paths::reset_for_test();  // 清空进程级缓存, 指向本用例 home
        db_path_ = home_ + "/db/trade.db";
    }

    void TearDown() override {
        // 释放默认库的进程级连接缓存 (否则 Windows 上连接占住临时目录, remove_all 失败)
        dztrader::strategy_api_internal::reset_default_db_cache_for_test();
        if (orig_home_) {
            dztrader::env::set("DZTRADER_HOME", *orig_home_);
        } else {
            dztrader::env::unset("DZTRADER_HOME");
        }
        dztrader::paths::reset_for_test();
        std::filesystem::remove_all(home_);
    }
};

}  // namespace

// 默认库 = <DZTRADER_HOME>/db/trade.db, 只读打开; 不再生成/读取 configs/database.json
// (交易段已废弃, ADR 0044 决策 3)。
TEST_F(DbOpenDefaultTest, OpensFixedLocalTradeDb) {
    create_td_db(db_path_);
    insert_order_row(db_path_, "A", 1001, 1);

    DzDatabase* db = dz_db_open_default();
    ASSERT_NE(nullptr, db) << dz_errmsg();
    DzResultSet* rs = dz_db_query_order(db, "A", nullptr, day(2026, 9, 1));
    ASSERT_NE(nullptr, rs) << dz_errmsg();
    ASSERT_EQ(0, dz_resultset_status(rs));
    ASSERT_TRUE(dz_resultset_next(rs));
    EXPECT_EQ(1001, dz_resultset_get_int64(rs, 2));  // order_id
    EXPECT_FALSE(dz_resultset_next(rs));
    dz_resultset_close(rs);
    dz_db_close(db);

    // 交易段废弃: 打开默认库不产生配置文件
    EXPECT_FALSE(std::filesystem::exists(home_ + "/configs/database.json"));
}

// 懒打开 + 进程级入口复用: 首次调用才建立入口; 每次调用返回独立句柄（各自一个只读
// Session）, 都可独立关闭, 关闭一个不影响另一个仍在用的句柄。
TEST_F(DbOpenDefaultTest, LazilyOpensAndGivesIndependentSessions) {
    create_td_db(db_path_);
    insert_order_row(db_path_, "A", 1001, 1);

    DzDatabase* first = dz_db_open_default();
    ASSERT_NE(nullptr, first) << dz_errmsg();
    DzDatabase* second = dz_db_open_default();
    ASSERT_NE(nullptr, second) << dz_errmsg();
    EXPECT_NE(first, second);  // 独立句柄（各自 Session, 关闭互不影响）

    dz_db_close(first);  // 关掉第一个: 第二个必须仍然可用
    DzResultSet* rs = dz_db_query_order(second, "A", nullptr, day(2026, 9, 1));
    ASSERT_NE(nullptr, rs) << dz_errmsg();
    ASSERT_TRUE(dz_resultset_next(rs));
    EXPECT_EQ(1001, dz_resultset_get_int64(rs, 2));
    dz_resultset_close(rs);
    dz_db_close(second);

    // 全部关闭后再打开: 仍能懒打开（连接仍在缓存里, 无需重建, 也不泄漏）
    DzDatabase* reopened = dz_db_open_default();
    ASSERT_NE(nullptr, reopened) << dz_errmsg();
    rs = dz_db_query_order(reopened, "A", nullptr, day(2026, 9, 1));
    ASSERT_NE(nullptr, rs) << dz_errmsg();
    EXPECT_TRUE(dz_resultset_next(rs));
    dz_resultset_close(rs);
    dz_db_close(reopened);
}

// 默认库不存在: 返回 NULL + DZ_EC_DB_OPEN_FAILED (与 dz_db_open 同语义), 不自动建库。
TEST_F(DbOpenDefaultTest, MissingDefaultDbReturnsNull) {
    EXPECT_EQ(nullptr, dz_db_open_default());
    EXPECT_EQ(DZ_EC_DB_OPEN_FAILED, dz_errcode());
    EXPECT_FALSE(std::filesystem::exists(db_path_));  // 不自动建库
}

// DZTRADER_DB_OPT_PATH / database.json 交易段都不再生效: 默认库恒为固定本地路径。
TEST_F(DbOpenDefaultTest, ConfigAndEnvNoLongerSelectTradeDb) {
    create_td_db(db_path_);
    insert_order_row(db_path_, "A", 1001, 1);
    // 另建一个库并用旧配置手段指向它 — 必须被忽略
    const std::string alt_path = home_ + "/alt/other.db";
    std::filesystem::create_directories(home_ + "/alt");
    create_td_db(alt_path);
    insert_order_row(alt_path, "B", 2001, 1, "20260901");
    std::filesystem::create_directories(home_ + "/configs");
    {
        std::ofstream ofs(home_ + "/configs/database.json", std::ios::binary | std::ios::trunc);
        ofs << R"({"td": {"backend": "sqlite", "options": {"path": ")" << alt_path << R"("}}})";
        ofs.close();
    }
    dztrader::env::set("DZTRADER_DB_OPT_PATH", alt_path);
    // 用例内自己恢复: 不把环境变量泄漏给同二进制的其它用例 (unit-test-standard §3.4)
    const auto restore_env = [&] { dztrader::env::unset("DZTRADER_DB_OPT_PATH"); };

    DzDatabase* db = dz_db_open_default();
    ASSERT_NE(nullptr, db) << dz_errmsg();
    DzResultSet* rs = dz_db_query_order(db, "B", nullptr, day(2026, 9, 1));
    ASSERT_NE(nullptr, rs) << dz_errmsg();
    EXPECT_FALSE(dz_resultset_next(rs));  // B 只在 alt 库: 未被读取
    dz_resultset_close(rs);
    rs = dz_db_query_order(db, "A", nullptr, day(2026, 9, 1));
    ASSERT_NE(nullptr, rs) << dz_errmsg();
    ASSERT_TRUE(dz_resultset_next(rs));  // A 在固定本地库: 才是实际打开的库
    EXPECT_EQ(1001, dz_resultset_get_int64(rs, 2));
    dz_resultset_close(rs);
    dz_db_close(db);
    restore_env();
}

/* ── ingest 句柄缓存 (FIX-5/FIX-7: 热路径复用 + 失败不缓存) ── */

namespace {

/// 缓存单测: 只碰显式路径的 sqlite 配置, 不依赖 DZTRADER_HOME。
/// 每个用例前后清空进程级缓存 (直跑整个二进制时用例间隔离)。
class IngestDbCacheTest : public ::testing::Test {
protected:
    std::filesystem::path dir_;

    void SetUp() override {
        dir_ = dztrader::test::unique_temp_dir("dz_test_ingest_db_cache");
        std::filesystem::remove_all(dir_);
        std::filesystem::create_directories(dir_);
        dztrader::strategy_api_internal::reset_ingest_cache_for_test();
    }

    void TearDown() override {
        dztrader::strategy_api_internal::reset_ingest_cache_for_test();
        std::filesystem::remove_all(dir_);
    }

    std::string db_file(const std::string& file) const { return (dir_ / file).string(); }
};

}  // namespace

// 同一路径复用同一句柄 (避免每次回补重试重开); 路径变化 → 关旧句柄重开,
// 保证测试/部署换目标可隔离。
TEST_F(IngestDbCacheTest, ReusesHandleAndReopensOnPathChange) {
    create_td_db((dir_ / "a.db").string());
    create_td_db((dir_ / "b.db").string());
    const std::string config_a = db_file("a.db");
    const std::string config_b = db_file("b.db");

    const auto first = dztrader::strategy_api_internal::acquire_ingest_db(config_a);
    ASSERT_NE(nullptr, first.db);
    EXPECT_FALSE(first.report);
    const auto again = dztrader::strategy_api_internal::acquire_ingest_db(config_a);
    EXPECT_EQ(first.db, again.db);  // 同配置命中缓存
    EXPECT_EQ(1u, dztrader::strategy_api_internal::ingest_open_attempts_for_test());  // 命中不重开

    // 路径变化 → 关旧句柄后重开。用打开次数断言而非指针不等: 旧句柄释放后
    // 分配器可能把同一地址复用给新句柄 (Windows MSVC 已实测偶发)。
    const auto changed = dztrader::strategy_api_internal::acquire_ingest_db(config_b);
    ASSERT_NE(nullptr, changed.db);
    EXPECT_EQ(2u, dztrader::strategy_api_internal::ingest_open_attempts_for_test());

    const auto back = dztrader::strategy_api_internal::acquire_ingest_db(config_a);
    ASSERT_NE(nullptr, back.db);
    EXPECT_EQ(3u, dztrader::strategy_api_internal::ingest_open_attempts_for_test());
    // 命中缓存不再打开 (同时只有一份句柄, 旧句柄已关)
    const auto back_again = dztrader::strategy_api_internal::acquire_ingest_db(config_a);
    EXPECT_EQ(back.db, back_again.db);
    EXPECT_EQ(3u, dztrader::strategy_api_internal::ingest_open_attempts_for_test());
}

// 关闭退避 (0ms) 时保持"失败不缓存, 每次调用重试一次, 同一路径只报一次"的原语义。
TEST_F(IngestDbCacheTest, FailureIsNotCachedAndReportedOnce) {
    dztrader::strategy_api_internal::set_ingest_failure_backoff_for_test(
        std::chrono::milliseconds(0));
    const std::string config = db_file("later.db");
    const auto failed = dztrader::strategy_api_internal::acquire_ingest_db(config);
    EXPECT_EQ(nullptr, failed.db);
    EXPECT_TRUE(failed.report);
    EXPECT_FALSE(failed.error.empty());
    EXPECT_EQ(1u, dztrader::strategy_api_internal::ingest_open_attempts_for_test());

    const auto retry = dztrader::strategy_api_internal::acquire_ingest_db(config);
    EXPECT_EQ(nullptr, retry.db);
    EXPECT_FALSE(retry.report);  // 同一路径的失败只报一次

    create_td_db((dir_ / "later.db").string());
    const auto ok = dztrader::strategy_api_internal::acquire_ingest_db(config);
    ASSERT_NE(nullptr, ok.db);  // 失败不缓存 → 重新尝试成功
    EXPECT_EQ(3u, dztrader::strategy_api_internal::ingest_open_attempts_for_test());
}

// WP3b 退避: 首次失败后, 窗口内紧随的 N 次 acquire 不产生新的打开尝试
// (MySQL 不可达时不再每个 dz_next_event 阻塞一次 connect_timeout)。
TEST_F(IngestDbCacheTest, BackoffSuppressesRetryWithinWindow) {
    dztrader::strategy_api_internal::set_ingest_failure_backoff_for_test(
        std::chrono::milliseconds(10000));
    const std::string config = db_file("cold.db");
    const auto failed = dztrader::strategy_api_internal::acquire_ingest_db(config);
    EXPECT_EQ(nullptr, failed.db);
    EXPECT_TRUE(failed.report);  // 第一次失败仍诊断 (不被退避吞掉)
    EXPECT_EQ(1u, dztrader::strategy_api_internal::ingest_open_attempts_for_test());

    for (int i = 0; i < 5; ++i) {
        const auto skipped = dztrader::strategy_api_internal::acquire_ingest_db(config);
        EXPECT_EQ(nullptr, skipped.db);
        EXPECT_FALSE(skipped.report);  // 窗口内不重复诊断
        EXPECT_TRUE(skipped.error.empty());
    }
    // 真实断言: 窗口内 5 次调用零打开尝试
    EXPECT_EQ(1u, dztrader::strategy_api_internal::ingest_open_attempts_for_test());
}

// WP3b 退避: 窗口过后重试 — 库在窗口内恢复也不提前尝试, 窗口一到即成功;
// 成功后缓存命中不再重开 (成功路径不受退避影响)。窗口到期用测试钩子立即
// 结束, 不 sleep: Windows CI 上 create_td_db 较慢, 靠墙钟窗口判定会偶发越过。
TEST_F(IngestDbCacheTest, RetriesAfterWindowAndThenServesFromCache) {
    dztrader::strategy_api_internal::set_ingest_failure_backoff_for_test(
        std::chrono::milliseconds(60000));
    const std::string config = db_file("warm.db");
    const auto failed = dztrader::strategy_api_internal::acquire_ingest_db(config);
    EXPECT_EQ(nullptr, failed.db);
    EXPECT_TRUE(failed.report);
    EXPECT_EQ(1u, dztrader::strategy_api_internal::ingest_open_attempts_for_test());

    // 库在退避窗口内就绪: 窗口内的调用仍不尝试 (不能提前命中)
    create_td_db((dir_ / "warm.db").string());
    const auto within = dztrader::strategy_api_internal::acquire_ingest_db(config);
    EXPECT_EQ(nullptr, within.db);
    EXPECT_EQ(1u, dztrader::strategy_api_internal::ingest_open_attempts_for_test());

    dztrader::strategy_api_internal::expire_ingest_backoff_for_test();
    const auto after = dztrader::strategy_api_internal::acquire_ingest_db(config);
    ASSERT_NE(nullptr, after.db);  // 窗口过后重试成功
    EXPECT_EQ(2u, dztrader::strategy_api_internal::ingest_open_attempts_for_test());

    const auto cached = dztrader::strategy_api_internal::acquire_ingest_db(config);
    EXPECT_EQ(after.db, cached.db);  // 成功后命中缓存
    EXPECT_EQ(2u, dztrader::strategy_api_internal::ingest_open_attempts_for_test());
}

// WP3b 退避按路径隔离: A 失败进入退避不影响 B; 且失败路径在窗口后重试仍只诊断一次。
TEST_F(IngestDbCacheTest, BackoffIsPerConfigAndRetryKeepsReportOnce) {
    dztrader::strategy_api_internal::set_ingest_failure_backoff_for_test(
        std::chrono::milliseconds(100));
    create_td_db((dir_ / "ok.db").string());
    const std::string broken = db_file("missing.db");
    const std::string healthy = db_file("ok.db");

    const auto failed = dztrader::strategy_api_internal::acquire_ingest_db(broken);
    EXPECT_EQ(nullptr, failed.db);
    EXPECT_TRUE(failed.report);
    EXPECT_EQ(1u, dztrader::strategy_api_internal::ingest_open_attempts_for_test());

    const auto other = dztrader::strategy_api_internal::acquire_ingest_db(healthy);
    ASSERT_NE(nullptr, other.db);  // 换路径立即尝试, 不受 broken 的退避影响
    EXPECT_EQ(2u, dztrader::strategy_api_internal::ingest_open_attempts_for_test());

    // 窗口到期用测试钩子立即结束, 不 sleep (与 RetriesAfterWindowAndThenServesFromCache 一致)
    dztrader::strategy_api_internal::expire_ingest_backoff_for_test();
    const auto retried = dztrader::strategy_api_internal::acquire_ingest_db(broken);
    EXPECT_EQ(nullptr, retried.db);  // 窗口过后确实重试 (attempts +1)
    EXPECT_FALSE(retried.report);    // 同一路径的失败仍只诊断一次
    EXPECT_EQ(3u, dztrader::strategy_api_internal::ingest_open_attempts_for_test());
}

/* ── ingest 配置错误降级 (FIX-3: 配置错误不得静默当作"库不存在") ── */

namespace {

using dztrader::shm::ChannelConfig;
using dztrader::shm::ChannelMeta;
using dztrader::shm::FrameView;
using dztrader::shm::MultiWriter;

constexpr uint64_t kChannelMB = 1024 * 1024;

/// `database.json` 交易段废弃后的行为 (ADR 0044 决策 3): 配置文件**损坏**或指向别的库
/// 都不再影响 SDK 打开的库 —— 交易库恒为 `<DZTRADER_HOME>/db/trade.db`。
/// 独立 fixture: 需要 SHM 通道 + dz_init (进程级 context)。
class IngestConfigObsolescenceTest : public ::testing::Test {
protected:
    std::string home_;
    DzContext* ctx_ = nullptr;
    std::optional<std::string> orig_home_;
    std::optional<std::string> orig_md_source_;

    void SetUp() override {
        home_ = dztrader::test::unique_temp_dir("dz_test_ingest_config_obsolete").string();
        std::filesystem::remove_all(home_);
        std::filesystem::create_directories(home_ + "/shm");
        std::filesystem::create_directories(home_ + "/db");
        std::filesystem::create_directories(home_ + "/configs");
        orig_home_ = dztrader::env::get("DZTRADER_HOME");
        orig_md_source_ = dztrader::env::get("DZTRADER_MD_SOURCE");
        dztrader::env::set("DZTRADER_HOME", home_);
        dztrader::paths::reset_for_test();  // 清空进程级缓存, 指向本用例 home
        dztrader::env::set("DZTRADER_MD_SOURCE", "test_md");

        // 损坏的 database.json（旧行为下会让 ingest 配置解析失败 → 降级不过滤）
        std::ofstream ofs(home_ + "/configs/database.json", std::ios::binary | std::ios::trunc);
        ofs << R"({"td": broken )";
        ofs.close();

        // 固定本地交易库存在, orders 有 seq=5 的水位行
        const std::string db_path = home_ + "/db/trade.db";
        create_td_db(db_path);
        insert_order_row(db_path, "CFGOBS", 1001, 5);

        (void)ChannelMeta::open_or_create(ChannelConfig{
            .channel_name = dztrader::shm::channel_name(dztrader::CHANNEL_NAME_EVENT),
            .shm_dir = home_ + "/shm",
            .meta_file_size = 1 * kChannelMB,
            .page_size = 1 * kChannelMB,
            .lock_memory = false,
            .prefetch_memory = false,
        });
        (void)ChannelMeta::open_or_create(ChannelConfig{
            // NOLINT(temp-dir-unique): test_md 是 SDK 约定的行情通道名，其 shm_dir 每用例唯一
            .channel_name = "test_md",
            .shm_dir = home_ + "/shm",
            .meta_file_size = 1 * kChannelMB,
            .page_size = 1 * kChannelMB,
            .lock_memory = false,
            .prefetch_memory = false,
        });

        ctx_ = dz_init();
        ASSERT_NE(nullptr, ctx_) << dz_errmsg();
    }

    void TearDown() override {
        if (ctx_ != nullptr) {
            dz_release(ctx_);
            ctx_ = nullptr;
        }
        dztrader::strategy_api_internal::reset_ingest_cache_for_test();
        dztrader::strategy_api_internal::reset_default_db_cache_for_test();
        if (orig_home_) {
            dztrader::env::set("DZTRADER_HOME", *orig_home_);
        } else {
            dztrader::env::unset("DZTRADER_HOME");
        }
        if (orig_md_source_) {
            dztrader::env::set("DZTRADER_MD_SOURCE", *orig_md_source_);
        } else {
            dztrader::env::unset("DZTRADER_MD_SOURCE");
        }
        dztrader::paths::reset_for_test();
        std::filesystem::remove_all(home_);
    }

    void emit_order(const char* account, uint64_t seq) {
        auto meta = std::make_shared<ChannelMeta>(ChannelMeta::open_only(
            dztrader::shm::channel_name(dztrader::CHANNEL_NAME_EVENT), home_ + "/shm"));
        DzOrderReport rpt{};
        dztrader::copy_string(rpt.account_id, account, true);
        dztrader::copy_string(rpt.strategy_id, dz_strategy_id(ctx_), true);
        rpt.seq = seq;
        MultiWriter writer = MultiWriter::create(meta, "cfg_obs_writer");
        ASSERT_TRUE(writer.write_frame(DZ_FRAME_ORDER_REPORT, rpt));
        writer.notify_subscribers();
    }
};

}  // namespace

// AC: `database.json` 交易段修改/损坏不再影响打开的库 —— 水位仍从固定本地
// `db/trade.db` 装载（seq=5 → seq≤5 拦截、seq=6 放行）。旧行为下损坏配置会让
// 配置解析失败、水位装载降级为全放行（seq=5 会被放行）——本用例区分两种行为。
TEST_F(IngestConfigObsolescenceTest, CorruptConfigDoesNotAffectOpenedDb) {
    emit_order("CFGOBS", 5);  // ≤ W(=5): 已同步过, 拦截
    EXPECT_EQ(nullptr, dz_next_event(ctx_)) << "seq=5 must be filtered by watermark from trade.db";
    emit_order("CFGOBS", 6);  // > W: 放行
    const void* frame = dz_next_event(ctx_);
    ASSERT_NE(nullptr, frame);
    EXPECT_EQ(DZ_FRAME_ORDER_REPORT, FrameView(static_cast<const std::byte*>(frame)).type());
    EXPECT_EQ(6u, FrameView(static_cast<const std::byte*>(frame)).payload<DzOrderReport>().seq);
}
