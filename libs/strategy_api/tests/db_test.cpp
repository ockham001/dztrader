#include <gtest/gtest.h>

#include <dztrader/api.h>
#include <dztrader/core/env.h>
#include <dztrader/db/database.h>
#include <dztrader/error.h>
#include <dztrader/shm/channel_meta.h>
#include <dztrader/shm/frame_view.h>
#include <dztrader/shm/writer.h>
#include <dztrader/struct.h>
#include <dztrader/tdstore/schema_catalog.h>

#include <nlohmann/json.hpp>
#include <SQLiteCpp/Database.h>
#include <SQLiteCpp/Statement.h>

#include <cfloat>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include "db_database.h"

namespace {

/// 在临时目录建 td 库 (schema_catalog 声明 + 驱动迁移) 并写入样例行。
class DbTest : public ::testing::Test {
protected:
    std::string db_path_;
    DzDatabase* db_ = nullptr;

    void SetUp() override {
        const auto dir = std::filesystem::temp_directory_path() / "dz_test_strategy_db";
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        db_path_ = (dir / "td.db").string();

        // 回滚日志模式 (驱动默认 WAL): 本套件含写锁阻塞读的 busy_timeout 用例 — WAL 下写锁
        // 不阻塞读快照 (无法触发 SQLITE_BUSY), 与迁移前 fixture 的默认日志模式一致。
        auto database = dztrader::db::Database::open(
            dztrader::db::Config{.backend = "sqlite",
                                 .options = {{"path", db_path_}, {"journal_mode", "delete"}}},
            dztrader::tdstore::schemas());
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
// orders 表列序 (SELECT *): 0=id 1=account_id 2=trading_day 3=order_id 4=order_ref
//   5=external_order_id 6=is_external 7=instrument_id 8=exchange_id 9=direction
//   10=position_effect 11=price_type 12=status 13=price 14=volume 15=volume_traded
//   16=volume_canceled 17=insert_time 18=update_time 19=error_id 20=error_msg
//   21=strategy_id 22=remark 23=seq
TEST_F(DbTest, QueryOrderFiltersByAccount) {
    SQLite::Database db(db_path_, SQLite::OPEN_READWRITE);
    {
        SQLite::Statement ins(
            db,
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
    EXPECT_EQ(1001, dz_resultset_get_int64(rs, 3));              // order_id
    EXPECT_STREQ("IF2401", dz_resultset_get_string(rs, 7));      // instrument_id
    EXPECT_DOUBLE_EQ(3800.5, dz_resultset_get_float64(rs, 13));  // price
    EXPECT_EQ(2, dz_resultset_get_int64(rs, 14));                // volume
    EXPECT_STREQ("A", dz_resultset_get_string(rs, 1));           // account_id
    EXPECT_EQ(1, dz_resultset_get_int64(rs, 23));                // seq
    EXPECT_FALSE(dz_resultset_next(rs));                         // 只回 1 行
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
        SQLite::Statement ins(
            db,
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
        SQLite::Statement ins(
            db,
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
    EXPECT_STREQ("IF2401", dz_resultset_get_string(rs, 2));     // instrument_id
    EXPECT_STREQ("L", dz_resultset_get_string(rs, 4));          // direction
    EXPECT_EQ(5, dz_resultset_get_int64(rs, 5));                // volume
    EXPECT_DOUBLE_EQ(3810.0, dz_resultset_get_float64(rs, 9));  // price
    EXPECT_EQ(1, dz_resultset_get_int64(rs, 10));               // seq
    EXPECT_FALSE(dz_resultset_next(rs));
    dz_resultset_close(rs);

    rs = dz_db_query_trading_account(db_, "A");
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
        SQLite::Statement ins(
            db,
            "INSERT INTO positions (account_id, trading_day, instrument_id, exchange_id,"
            " direction, volume, price, seq) VALUES (?, '20260901', ?, 'CFFEX', 'L', ?, ?, ?)");
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
        SQLite::Statement ins(
            db,
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
        SQLite::Statement ins(
            db,
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

/* ── dz_db_open_default (ADR 0016: 按配置解析打开 td 库) ── */

namespace {

/// 建 td 库 (schema_catalog 声明 + 驱动迁移) 到指定路径。
void create_td_db(const std::string& path) {
    auto database = dztrader::db::Database::open(
        dztrader::db::Config{.backend = "sqlite", .options = {{"path", path}}},
        dztrader::tdstore::schemas());
    database->migrate();
}

/// 向 orders 表写一行 (账户 + order_id + seq)。
void insert_order_row(const std::string& path, const char* account, int64_t order_id, int64_t seq) {
    SQLite::Database db(path, SQLite::OPEN_READWRITE);
    SQLite::Statement ins(
        db,
        "INSERT INTO orders (account_id, trading_day, order_id, order_ref, instrument_id,"
        " exchange_id, direction, status, price, volume, seq)"
        " VALUES (?, '20260901', ?, 'r', 'IF2401', 'CFFEX', '0', 'a', 3800.5, 2, ?)");
    ins.bind(1, account);
    ins.bind(2, order_id);
    ins.bind(3, seq);
    ins.exec();
}

/// dz_db_open_default 套件: 自管临时 DZTRADER_HOME (AGENTS.md 硬门禁 2 的 SetUp 豁免)。
class DbOpenDefaultTest : public ::testing::Test {
protected:
    std::string home_;
    std::string db_path_;

    void SetUp() override {
        home_ = (std::filesystem::temp_directory_path() / "dz_test_db_open_default").string();
        std::filesystem::remove_all(home_);
        std::filesystem::create_directories(home_ + "/db");
        dztrader::env::set("DZTRADER_HOME", home_);
        dztrader::env::unset("DZTRADER_DB_BACKEND");
        dztrader::env::unset("DZTRADER_DB_OPT_PATH");
        db_path_ = home_ + "/db/td.db";
    }

    void TearDown() override {
        dztrader::env::unset("DZTRADER_DB_BACKEND");
        dztrader::env::unset("DZTRADER_DB_OPT_PATH");
        std::filesystem::remove_all(home_);
    }
};

}  // namespace

// 默认配置 (无 database.json): 打开 <DZTRADER_HOME>/db/td.db, 并自动生成 td 段配置文件。
TEST_F(DbOpenDefaultTest, OpensDefaultHomeTdDbAndGeneratesConfig) {
    create_td_db(db_path_);
    insert_order_row(db_path_, "A", 1001, 1);

    DzDatabase* db = dz_db_open_default();
    ASSERT_NE(nullptr, db) << dz_errmsg();
    DzResultSet* rs = dz_db_query_order(db, "A", nullptr);
    ASSERT_NE(nullptr, rs) << dz_errmsg();
    ASSERT_EQ(0, dz_resultset_status(rs));
    ASSERT_TRUE(dz_resultset_next(rs));
    EXPECT_EQ(1001, dz_resultset_get_int64(rs, 3));  // order_id
    EXPECT_FALSE(dz_resultset_next(rs));
    dz_resultset_close(rs);
    dz_db_close(db);

    EXPECT_TRUE(std::filesystem::exists(home_ + "/configs/database.json"));
}

// 环境变量 DZTRADER_DB_OPT_PATH 覆盖默认库路径 (配置解析 env > 文件 > 默认)。
TEST_F(DbOpenDefaultTest, EnvPathOverridesDefaultDb) {
    create_td_db(db_path_);
    insert_order_row(db_path_, "A", 1001, 1);
    const std::string alt_dir = home_ + "/alt";
    const std::string alt_path = alt_dir + "/other.db";
    std::filesystem::create_directories(alt_dir);
    create_td_db(alt_path);
    insert_order_row(alt_path, "B", 2001, 1);

    dztrader::env::set("DZTRADER_DB_OPT_PATH", alt_path);
    DzDatabase* db = dz_db_open_default();
    ASSERT_NE(nullptr, db) << dz_errmsg();

    DzResultSet* rs = dz_db_query_order(db, "A", nullptr);
    ASSERT_NE(nullptr, rs) << dz_errmsg();
    EXPECT_FALSE(dz_resultset_next(rs));  // 默认库未被读取
    dz_resultset_close(rs);

    rs = dz_db_query_order(db, "B", nullptr);
    ASSERT_NE(nullptr, rs) << dz_errmsg();
    ASSERT_TRUE(dz_resultset_next(rs));
    EXPECT_EQ(2001, dz_resultset_get_int64(rs, 3));
    dz_resultset_close(rs);
    dz_db_close(db);
}

// configs/database.json 的 td 段显式 path 生效 (env 未设置时)。
TEST_F(DbOpenDefaultTest, ConfigFilePathIsHonored) {
    create_td_db(db_path_);
    insert_order_row(db_path_, "A", 1001, 1);
    const std::string alt_dir = home_ + "/alt";
    const std::string alt_path = alt_dir + "/other.db";
    std::filesystem::create_directories(alt_dir);
    std::filesystem::create_directories(home_ + "/configs");
    create_td_db(alt_path);
    insert_order_row(alt_path, "B", 2001, 1);

    nlohmann::json full = nlohmann::json::object();
    full["td"] = {{"backend", "sqlite"}, {"options", {{"path", alt_path}}}};
    std::ofstream ofs(home_ + "/configs/database.json", std::ios::binary | std::ios::trunc);
    ofs << full.dump(2);
    ofs.close();

    DzDatabase* db = dz_db_open_default();
    ASSERT_NE(nullptr, db) << dz_errmsg();
    DzResultSet* rs = dz_db_query_order(db, "B", nullptr);
    ASSERT_NE(nullptr, rs) << dz_errmsg();
    ASSERT_TRUE(dz_resultset_next(rs));
    EXPECT_EQ(2001, dz_resultset_get_int64(rs, 3));
    dz_resultset_close(rs);
    dz_db_close(db);
}

// 默认库不存在: 返回 NULL + DZ_EC_DB_OPEN_FAILED (与 dz_db_open 同语义)。
TEST_F(DbOpenDefaultTest, MissingDefaultDbReturnsNull) {
    EXPECT_EQ(nullptr, dz_db_open_default());
    EXPECT_EQ(DZ_EC_DB_OPEN_FAILED, dz_errcode());
}

// 配置指定未编入的后端: 返回 NULL + DZ_EC_DB_UNSUPPORTED_BACKEND (FIX-9 早失败),
// 错误消息含 backend 名。
TEST_F(DbOpenDefaultTest, UnsupportedBackendReturnsNull) {
    dztrader::env::set("DZTRADER_DB_BACKEND", "nope");
    EXPECT_EQ(nullptr, dz_db_open_default());
    EXPECT_EQ(DZ_EC_DB_UNSUPPORTED_BACKEND, dz_errcode());
    const char* msg = dz_errmsg();
    ASSERT_NE(nullptr, msg);
    EXPECT_NE(std::string::npos, std::string(msg).find("nope"));
}

// 配置 JSON 损坏: 返回 NULL + DZ_EC_INVALID_PARAM (不静默回落默认库)。
TEST_F(DbOpenDefaultTest, InvalidConfigJsonReturnsNull) {
    std::filesystem::create_directories(home_ + "/configs");
    std::ofstream ofs(home_ + "/configs/database.json", std::ios::binary | std::ios::trunc);
    ofs << R"({"td": broken )";
    ofs.close();
    EXPECT_EQ(nullptr, dz_db_open_default());
    EXPECT_EQ(DZ_EC_INVALID_PARAM, dz_errcode());
}

/* ── ingest 句柄缓存 (FIX-5/FIX-7: 热路径复用 + 失败不缓存) ── */

namespace {

/// 缓存单测: 只碰显式路径的 sqlite 配置, 不依赖 DZTRADER_HOME。
/// 每个用例前后清空进程级缓存 (直跑整个二进制时用例间隔离)。
class IngestDbCacheTest : public ::testing::Test {
protected:
    std::filesystem::path dir_;

    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() / "dz_test_ingest_db_cache";
        std::filesystem::remove_all(dir_);
        std::filesystem::create_directories(dir_);
        dztrader::strategy_api_internal::reset_ingest_cache_for_test();
    }

    void TearDown() override {
        dztrader::strategy_api_internal::reset_ingest_cache_for_test();
        std::filesystem::remove_all(dir_);
    }

    dztrader::db::Config make_config(const std::string& file) const {
        return dztrader::db::Config{
            .backend = "sqlite",
            .options = {{"path", (dir_ / file).string()}, {"busy_timeout_ms", "5000"}}};
    }
};

}  // namespace

// 同一配置复用同一句柄 (MySQL 下即避免每次回补重试重连); 配置变化 → 关旧句柄按新配置重开,
// 保证测试换 DZTRADER_HOME/环境变量后可隔离。
TEST_F(IngestDbCacheTest, ReusesHandleAndReopensOnConfigChange) {
    create_td_db((dir_ / "a.db").string());
    create_td_db((dir_ / "b.db").string());
    const auto config_a = make_config("a.db");
    const auto config_b = make_config("b.db");

    const auto first = dztrader::strategy_api_internal::acquire_ingest_db(config_a);
    ASSERT_NE(nullptr, first.db);
    EXPECT_FALSE(first.report);
    const auto again = dztrader::strategy_api_internal::acquire_ingest_db(config_a);
    EXPECT_EQ(first.db, again.db);  // 同配置命中缓存
    EXPECT_EQ(1u, dztrader::strategy_api_internal::ingest_open_attempts_for_test());  // 命中不重开

    const auto changed = dztrader::strategy_api_internal::acquire_ingest_db(config_b);
    ASSERT_NE(nullptr, changed.db);
    EXPECT_NE(first.db, changed.db);  // 配置变化 → 重开

    const auto back = dztrader::strategy_api_internal::acquire_ingest_db(config_a);
    ASSERT_NE(nullptr, back.db);
    EXPECT_NE(changed.db, back.db);  // 同时只有一份句柄 (旧句柄已关)
}

// 关闭退避 (0ms) 时保持"失败不缓存, 每次调用重试一次, 同一配置只报一次"的原语义。
TEST_F(IngestDbCacheTest, FailureIsNotCachedAndReportedOnce) {
    dztrader::strategy_api_internal::set_ingest_failure_backoff_for_test(
        std::chrono::milliseconds(0));
    const auto config = make_config("later.db");
    const auto failed = dztrader::strategy_api_internal::acquire_ingest_db(config);
    EXPECT_EQ(nullptr, failed.db);
    EXPECT_TRUE(failed.report);
    EXPECT_FALSE(failed.error.empty());
    EXPECT_EQ(1u, dztrader::strategy_api_internal::ingest_open_attempts_for_test());

    const auto retry = dztrader::strategy_api_internal::acquire_ingest_db(config);
    EXPECT_EQ(nullptr, retry.db);
    EXPECT_FALSE(retry.report);  // 同一配置的失败只报一次

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
    const auto config = make_config("cold.db");
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
// 成功后缓存命中不再重开 (成功路径不受退避影响)。
TEST_F(IngestDbCacheTest, RetriesAfterWindowAndThenServesFromCache) {
    dztrader::strategy_api_internal::set_ingest_failure_backoff_for_test(
        std::chrono::milliseconds(100));
    const auto config = make_config("warm.db");
    const auto failed = dztrader::strategy_api_internal::acquire_ingest_db(config);
    EXPECT_EQ(nullptr, failed.db);
    EXPECT_TRUE(failed.report);
    EXPECT_EQ(1u, dztrader::strategy_api_internal::ingest_open_attempts_for_test());

    // 库在退避窗口内就绪: 窗口内的调用仍不尝试 (不能提前命中)
    create_td_db((dir_ / "warm.db").string());
    const auto within = dztrader::strategy_api_internal::acquire_ingest_db(config);
    EXPECT_EQ(nullptr, within.db);
    EXPECT_EQ(1u, dztrader::strategy_api_internal::ingest_open_attempts_for_test());

    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    const auto after = dztrader::strategy_api_internal::acquire_ingest_db(config);
    ASSERT_NE(nullptr, after.db);  // 窗口过后重试成功
    EXPECT_EQ(2u, dztrader::strategy_api_internal::ingest_open_attempts_for_test());

    const auto cached = dztrader::strategy_api_internal::acquire_ingest_db(config);
    EXPECT_EQ(after.db, cached.db);  // 成功后命中缓存
    EXPECT_EQ(2u, dztrader::strategy_api_internal::ingest_open_attempts_for_test());
}

// WP3b 退避按配置隔离: A 失败进入退避不影响 B; 且失败配置在窗口后重试仍只诊断一次。
TEST_F(IngestDbCacheTest, BackoffIsPerConfigAndRetryKeepsReportOnce) {
    dztrader::strategy_api_internal::set_ingest_failure_backoff_for_test(
        std::chrono::milliseconds(100));
    create_td_db((dir_ / "ok.db").string());
    const auto broken = make_config("missing.db");
    const auto healthy = make_config("ok.db");

    const auto failed = dztrader::strategy_api_internal::acquire_ingest_db(broken);
    EXPECT_EQ(nullptr, failed.db);
    EXPECT_TRUE(failed.report);
    EXPECT_EQ(1u, dztrader::strategy_api_internal::ingest_open_attempts_for_test());

    const auto other = dztrader::strategy_api_internal::acquire_ingest_db(healthy);
    ASSERT_NE(nullptr, other.db);  // 换配置立即尝试, 不受 broken 的退避影响
    EXPECT_EQ(2u, dztrader::strategy_api_internal::ingest_open_attempts_for_test());

    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    const auto retried = dztrader::strategy_api_internal::acquire_ingest_db(broken);
    EXPECT_EQ(nullptr, retried.db);  // 窗口过后确实重试 (attempts +1)
    EXPECT_FALSE(retried.report);    // 同一配置的失败仍只诊断一次
    EXPECT_EQ(3u, dztrader::strategy_api_internal::ingest_open_attempts_for_test());
}

/* ── ingest 配置错误降级 (FIX-3: 配置错误不得静默当作"库不存在") ── */

namespace {

using dztrader::shm::ChannelConfig;
using dztrader::shm::ChannelMeta;
using dztrader::shm::FrameView;
using dztrader::shm::MultiWriter;

constexpr uint64_t kChannelMB = 1024 * 1024;

/// database.json 损坏 (非法 JSON) 时 ingest 路径的降级: dz_init 不得失败
/// (异常不越过 C 边界), 库不可用 → 水位不过滤, 帧全放行。
/// 独立 fixture: 需要 SHM 通道 + dz_init (进程级 context)。
class IngestConfigErrorTest : public ::testing::Test {
protected:
    std::string home_;
    DzContext* ctx_ = nullptr;

    void SetUp() override {
        home_ = (std::filesystem::temp_directory_path() / "dz_test_ingest_config_error").string();
        std::filesystem::remove_all(home_);
        std::filesystem::create_directories(home_ + "/shm");
        std::filesystem::create_directories(home_ + "/configs");
        dztrader::env::set("DZTRADER_HOME", home_);
        dztrader::env::set("DZTRADER_MD_SOURCE", "test_md");
        dztrader::env::unset("DZTRADER_DB_BACKEND");
        dztrader::env::unset("DZTRADER_DB_OPT_PATH");

        std::ofstream ofs(home_ + "/configs/database.json", std::ios::binary | std::ios::trunc);
        ofs << R"({"td": broken )";
        ofs.close();

        (void)ChannelMeta::open_or_create(ChannelConfig{
            .channel_name = dztrader::shm::channel_name(dztrader::CHANNEL_NAME_EVENT),
            .shm_dir = home_ + "/shm",
            .meta_file_size = 1 * kChannelMB,
            .page_size = 1 * kChannelMB,
            .lock_memory = false,
            .prefetch_memory = false,
        });
        (void)ChannelMeta::open_or_create(ChannelConfig{
            .channel_name = "test_md",
            .shm_dir = home_ + "/shm",
            .meta_file_size = 1 * kChannelMB,
            .page_size = 1 * kChannelMB,
            .lock_memory = false,
            .prefetch_memory = false,
        });

        ctx_ = dz_init();  // 配置损坏: 降级不过滤, 不失败
        ASSERT_NE(nullptr, ctx_) << dz_errmsg();
    }

    void TearDown() override {
        if (ctx_ != nullptr) {
            dz_release(ctx_);
            ctx_ = nullptr;
        }
        std::filesystem::remove_all(home_);
    }
};

}  // namespace

// 配置损坏: dz_init 仍成功 (配置错误单独诊断 + 不缓存, FIX-3), 水位装载降级为不过滤,
// 帧全放行 (seq=5 无水位即放行 — 与"库不存在"同结果, 诊断路径不同)。
TEST_F(IngestConfigErrorTest, CorruptConfigDegradesWithoutFailingInit) {
    auto meta = std::make_shared<ChannelMeta>(ChannelMeta::open_only(
        dztrader::shm::channel_name(dztrader::CHANNEL_NAME_EVENT), home_ + "/shm"));
    DzOrderReport rpt{};
    dztrader::copy_string(rpt.account_id, "CFGERR", true);
    dztrader::copy_string(rpt.strategy_id, dz_strategy_id(ctx_), true);
    rpt.seq = 5;  // 无水位 (配置损坏 → 无库) → 不过滤, 全放行
    {
        MultiWriter writer = MultiWriter::create(meta, "cfg_err_writer");
        ASSERT_TRUE(writer.write_frame(DZ_FRAME_ORDER_REPORT, rpt));
        writer.notify_subscribers();
    }
    const void* frame = dz_next_event(ctx_);
    ASSERT_NE(nullptr, frame);
    EXPECT_EQ(DZ_FRAME_ORDER_REPORT, FrameView(static_cast<const std::byte*>(frame)).type());
    EXPECT_EQ(5u, FrameView(static_cast<const std::byte*>(frame)).payload<DzOrderReport>().seq);
}
