#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <SQLiteCpp/Database.h>
#include <SQLiteCpp/Statement.h>

#include <dztrader/tdstore/records.h>
#include <dztrader/tdstore/schema_catalog.h>
#include <dztrader/tdstore/trade_db.h>

#include "td/td_persist_records.h"
#include "td/td_persist_writer.h"
#include "test_temp_dir.h"

namespace dztrader::ctp {
namespace {

// ============================================================================
// 外部观察 / 确定性故障注入 (独立 SQLiteCpp 连接, 不经过 PersistWriter)
//
// 测试只断言外部可观察行为: DB 已提交内容 (独立连接读) / 通知回调 / flush 返回值.
// 真 SQLite 临时库, 不 mock SQL.
// ============================================================================

/// 原生 SQL 写入 (测试播种/破坏用; 独立连接, 不经过 PersistWriter).
void raw_exec(const std::string& path, const std::string& sql) {
    SQLite::Database db(path, SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
    db.exec("PRAGMA busy_timeout=5000");
    db.exec(sql);
}

/// 只读查询连接 (WAL 库允许与 Writer 连接并发读; busy_timeout 防瞬时提交锁).
SQLite::Database open_readonly(const std::string& path) {
    SQLite::Database db(path, SQLite::OPEN_READONLY);
    db.exec("PRAGMA busy_timeout=5000");
    return db;
}

/// 原生 SQL 标量读取 (只读连接; 整数/文本列按列亲和性转换).
int64_t query_int(const std::string& path, const std::string& sql) {
    SQLite::Database db = open_readonly(path);
    SQLite::Statement query(db, sql);
    return query.executeStep() ? query.getColumn(0).getInt64() : 0;
}

double query_double(const std::string& path, const std::string& sql) {
    SQLite::Database db = open_readonly(path);
    SQLite::Statement query(db, sql);
    return query.executeStep() ? query.getColumn(0).getDouble() : 0.0;
}

std::string query_text(const std::string& path, const std::string& sql) {
    SQLite::Database db = open_readonly(path);
    SQLite::Statement query(db, sql);
    return query.executeStep() ? query.getColumn(0).getString() : "";
}

void write_file_bytes(const std::filesystem::path& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

std::string read_file_bytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

/// 目录下文件名前缀匹配项 (坏库改名重建断言 `<db>.old-*` 用).
std::vector<std::filesystem::path> files_with_prefix(const std::filesystem::path& dir,
                                                     const std::string& prefix) {
    std::vector<std::filesystem::path> matched;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.path().filename().string().starts_with(prefix)) {
            matched.push_back(entry.path());
        }
    }
    return matched;
}

// ============================================================================
// 条件等待 (判据 §1.5: 同步用条件等待, 禁止把 sleep 当同步手段)
// ============================================================================

/// 轮询到谓词成立或超时 (超时返回 false). poll 间隔只控制观测粒度, 不承担同步语义 —
/// 判定边界是 deadline, 上层断言据此报红.
template <typename Predicate>
bool wait_until(Predicate predicate, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

// ============================================================================
// 通知记录器 (线程安全: 通知可能来自 Writer 线程或调用 enqueue 的线程)
// ============================================================================

struct PersistNotification {
    DzNotifyLevel level = DZ_NOTIFY_INFO;
    std::string message;
    bool popup = false;
};

class NotificationRecorder {
public:
    /// 注入 PersistWriter 的回调 (实现方须线程安全, 见 td_persist_writer.h).
    PersistNotifyFn callback() {
        return [this](DzNotifyLevel level, const std::string& message, bool popup) {
            std::lock_guard<std::mutex> lk(mtx_);
            notifications_.push_back(PersistNotification{level, message, popup});
        };
    }

    std::size_t count(DzNotifyLevel level) const {
        std::lock_guard<std::mutex> lk(mtx_);
        return static_cast<std::size_t>(
            std::count_if(notifications_.begin(), notifications_.end(),
                          [level](const PersistNotification& n) { return n.level == level; }));
    }

    std::vector<PersistNotification> notifications_at(DzNotifyLevel level) const {
        std::lock_guard<std::mutex> lk(mtx_);
        std::vector<PersistNotification> matched;
        for (const PersistNotification& notification : notifications_) {
            if (notification.level == level) {
                matched.push_back(notification);
            }
        }
        return matched;
    }

    bool wait_for_count(DzNotifyLevel level,
                        std::size_t expected,
                        std::chrono::milliseconds timeout) const {
        return wait_until([this, level, expected] { return count(level) >= expected; }, timeout);
    }

private:
    mutable std::mutex mtx_;
    std::vector<PersistNotification> notifications_;
};

// ============================================================================
// 测试策略 / 记录构造
// ============================================================================

/// 快速失败策略 (设计 §5): 短退避 + 短持续告警间隔 + 低积压阈值, 保证确定性且快.
PersistPolicy fast_policy() {
    PersistPolicy policy;
    policy.retry_delays = {std::chrono::milliseconds(10), std::chrono::milliseconds(20),
                           std::chrono::milliseconds(50)};
    policy.sustained_notify_interval = std::chrono::milliseconds(50);
    policy.backlog_warn_threshold = 8;
    return policy;
}

/// DzDate (距纪元天数) -> 日期文本的测试常量: 20696 = 20260831, 20697 = 20260901.
constexpr int64_t kPrevTradingDay = 20696;
constexpr int64_t kTradingDay = 20697;

OrderRecord make_order(int64_t order_id, uint64_t seq, const char* day = "20260901") {
    OrderRecord record{};
    record.base.order_id = order_id;
    record.base.seq = seq;
    std::strcpy(record.base.account_id, "acc1");
    std::strcpy(record.trading_day, day);
    std::strcpy(record.order_ref, "000001");
    std::strcpy(record.base.instrument_id, "IF2506");
    std::strcpy(record.base.exchange_id, "CFFEX");
    record.base.direction = DZ_DIRECTION_LONG;
    record.base.position_effect = DZ_POSITION_EFFECT_OPEN;
    record.base.price_type = DZ_PRICE_LIMIT;
    record.base.status = DZ_ORDER_NOT_TRADED;
    record.base.price = 3900.0;
    record.base.volume = 1;
    record.base.volume_traded = 0;
    record.base.time = 34200;
    return record;
}

TradeRecord make_trade(const char* trade_id, uint64_t seq, const char* day = "20260901") {
    TradeRecord record{};
    record.base.seq = seq;
    std::strcpy(record.base.account_id, "acc1");
    std::strcpy(record.trading_day, day);
    std::strcpy(record.base.trade_id, trade_id);
    record.base.order_id = 1;
    std::strcpy(record.base.instrument_id, "IF2506");
    std::strcpy(record.base.exchange_id, "CFFEX");
    record.base.direction = DZ_DIRECTION_LONG;
    record.base.position_effect = DZ_POSITION_EFFECT_OPEN;
    record.base.price = 3900.0;
    record.base.volume = 1;
    record.base.time = 34201;
    return record;
}

/// 构造持仓记录 (DzPositionInfo). direction: DZ_DIRECTION_LONG=1 / SHORT=-1.
DzPositionInfo make_position(std::int64_t volume, std::int64_t seq, int8_t direction) {
    DzPositionInfo position{};
    std::strcpy(position.instrument_id, "IF2506");
    std::strcpy(position.exchange_id, "CFFEX");
    std::strcpy(position.account_id, "acc1");
    position.volume = volume;
    position.frozen_volume = 0;
    position.price = 3900.0;
    position.yd_volume = 0;
    position.today_volume = volume;
    position.direction = direction;
    position.seq = static_cast<uint64_t>(seq);
    return position;
}

DzTradingAccount make_trading_account(std::int64_t seq) {
    DzTradingAccount account{};
    std::strcpy(account.account_id, "acc1");
    account.balance = 1000000.0;
    account.available = 900000.0;
    account.frozen = 0.0;
    account.commission = 0.0;
    account.margin = 0.0;
    account.withdraw_quota = 800000.0;
    account.deposit = 0.0;
    account.withdraw = 0.0;
    account.seq = static_cast<uint64_t>(seq);
    return account;
}

/// 入队 flush 哨兵并等待提交完成 (FIFO: 此前任务必已提交).
bool flush_writer(PersistWriter& writer, std::chrono::milliseconds timeout) {
    const uint64_t token = writer.enqueue_flush_signal();
    return writer.wait_flush(token, timeout);
}

// ============================================================================
// fixture: 每个用例独立临时目录 (判据 §1.5 / §3.5)
// ============================================================================

class TdPersistWriterTest : public ::testing::Test {
protected:
    void SetUp() override {
        tmp_dir_ = dztrader::test::unique_temp_dir("dz_td_persist_test");
        std::filesystem::create_directories(tmp_dir_);
        db_path_ = (tmp_dir_ / "test.db").string();
    }

    void TearDown() override { std::filesystem::remove_all(tmp_dir_); }

    int64_t scalar_int(const std::string& sql) { return query_int(db_path_, sql); }

    double scalar_double(const std::string& sql) { return query_double(db_path_, sql); }

    std::string scalar_text(const std::string& sql) { return query_text(db_path_, sql); }

    std::filesystem::path tmp_dir_;
    std::string db_path_;
    NotificationRecorder recorder_;
};

// ============================================================================
// open + 表结构 / WAL 收敛
// ============================================================================

TEST_F(TdPersistWriterTest, OpenCreatesAllTables) {
    PersistWriter writer(db_path_, "dztd_test");
    writer.open();
    EXPECT_EQ(
        scalar_int("SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND "
                   "name IN ('orders','trades','instruments','positions','trading_accounts')"),
        5);
    EXPECT_EQ(scalar_int("PRAGMA user_version"), tdstore::kTdSchemaVersion);
    writer.stop();
}

// 无锁: WAL 转换成功, 最终 journal_mode = wal (多网关共写 + 多读者前提).
TEST_F(TdPersistWriterTest, OpenConvertsToWalWhenUnlocked) {
    PersistWriter writer(db_path_, "dztd_test");
    writer.open();
    EXPECT_EQ(scalar_text("PRAGMA journal_mode"), "wal");
    writer.stop();
}

// WAL 收敛 + 健康库不被误改名 (§1.2 / §5.11):
// 另一连接持 SHARED 读锁时, open() 的 WAL 转换必然失败 (转换需 EXCLUSIVE) — 新契约是
// open() 不抛、记 ERROR, 由 Writer 线程退避重连; 锁释放后自动收敛到 wal, 且全程不得
// 把健康库改名重建 (标记行必须保留).
// 耗时说明 (§4.1 成本判据): 生产建库门面 ensure_trade_db 对 WAL 转换使用
// busy_timeout=5000, 故 open() 在锁竞争下最多等待一次 5s 超时 — 这是被测"真实锁竞争 +
// 建连失败降级"语义的固有成本 (真锁、真超时), 不是测试自造等待.
TEST_F(TdPersistWriterTest, OpenConvergesToWalAfterReadLockClears) {
    // 先建库并播一行标记 (证明后续收敛不是"改名重建后的新库").
    ASSERT_NO_THROW(tdstore::ensure_trade_db(db_path_));
    raw_exec(db_path_,
             "INSERT INTO orders (account_id, date, order_id, instrument_id, exchange_id,"
             " seq, gateway) VALUES ('acc1', '20260901', 7, 'IF2506', 'CFFEX', 1, 'seed')");
    // 回到 rollback journal: 制造"WAL 转换需独占锁"的前置 (已经是 wal 时转换是 no-op).
    raw_exec(db_path_, "PRAGMA journal_mode=DELETE");
    ASSERT_EQ(scalar_text("PRAGMA journal_mode"), "delete");

    // 持 SHARED 读锁 (显式读事务): 迁移连接的 wal 转换立即失败.
    SQLite::Database reader(db_path_, SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
    reader.exec("PRAGMA busy_timeout=5000");
    reader.exec("BEGIN");
    reader.exec("SELECT COUNT(*) FROM sqlite_master");

    PersistWriter writer(db_path_, "dztd_test", fast_policy(), recorder_.callback());
    EXPECT_NO_THROW(writer.open());  // 冷启动不得因建连失败中止 (不抛)
    ASSERT_TRUE(recorder_.wait_for_count(DZ_NOTIFY_ERROR, 1, std::chrono::seconds(2)));

    // 锁释放 → Writer 线程下一轮建连成功 → WAL 收敛 (flush 哨兵只会在提交成功后返回).
    reader.exec("COMMIT");
    writer.start_writer();
    ASSERT_TRUE(flush_writer(writer, std::chrono::seconds(10)));
    EXPECT_EQ(scalar_text("PRAGMA journal_mode"), "wal");
    // 标记行保留 + 无备份副本: 健康库即使暂时不可读也不得被改名重建.
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM orders WHERE order_id=7"), 1);
    EXPECT_TRUE(files_with_prefix(tmp_dir_, "test.db.old-").empty())
        << "healthy db must not be quarantined under lock contention";
    writer.stop();
}

// ============================================================================
// 四类任务逐列写入正确
//
// 逐列断言: 列序错位/漏绑/枚举表示错误都必须变红; 只有 COUNT(*)==1 的旧断言不会红.
// ============================================================================

TEST_F(TdPersistWriterTest, OrderPersistedColumnsExact) {
    PersistWriter writer(db_path_, "gateway_a");
    writer.open();
    writer.start_writer();

    OrderRecord record{};
    record.base.order_id = 7001;
    std::strcpy(record.base.account_id, "acc1");
    std::strcpy(record.trading_day, "20260901");
    std::strcpy(record.order_ref, "000123");
    std::strcpy(record.external_order_id, "SYS-42");
    record.is_external = 0;
    std::strcpy(record.base.strategy_id, "stg7");
    std::strcpy(record.base.instrument_id, "IF2509");
    std::strcpy(record.base.exchange_id, "CFFEX");
    record.base.direction = DZ_DIRECTION_SHORT;              // -1
    record.base.position_effect = DZ_POSITION_EFFECT_CLOSE;  // 2
    record.base.price_type = DZ_PRICE_MARKET;                // 1
    record.base.status = DZ_ORDER_ALL_TRADED;                // 4
    record.base.price = 3901.25;
    record.base.volume = 5;
    record.base.volume_traded = 3;
    std::strcpy(record.base.remark, "remark-x");
    record.base.time = 64800;
    record.base.avg_price = 3900.75;
    record.base.seq = 42;
    record.volume_canceled = 2;
    record.update_time = 1700000000;
    record.error_id = 31;
    std::strcpy(record.error_msg, "ctp error");
    writer.enqueue(PersistTask{.kind = PersistTask::Kind::Order, .data = record});
    ASSERT_TRUE(flush_writer(writer, std::chrono::seconds(10)));
    writer.stop();

    ASSERT_EQ(scalar_int("SELECT COUNT(*) FROM orders"), 1);
    EXPECT_EQ(scalar_text("SELECT account_id FROM orders"), "acc1");
    EXPECT_EQ(scalar_text("SELECT date FROM orders"), "20260901");  // trading_day 文本列
    EXPECT_EQ(scalar_int("SELECT order_id FROM orders"), 7001);
    EXPECT_EQ(scalar_text("SELECT strategy_id FROM orders"), "stg7");
    EXPECT_EQ(scalar_text("SELECT instrument_id FROM orders"), "IF2509");
    EXPECT_EQ(scalar_text("SELECT exchange_id FROM orders"), "CFFEX");
    EXPECT_EQ(scalar_int("SELECT CAST(direction AS INTEGER) FROM orders"), DZ_DIRECTION_SHORT);
    EXPECT_EQ(scalar_int("SELECT CAST(position_effect AS INTEGER) FROM orders"),
              DZ_POSITION_EFFECT_CLOSE);
    EXPECT_EQ(scalar_int("SELECT CAST(price_type AS INTEGER) FROM orders"), DZ_PRICE_MARKET);
    EXPECT_EQ(scalar_int("SELECT CAST(status AS INTEGER) FROM orders"), DZ_ORDER_ALL_TRADED);
    EXPECT_DOUBLE_EQ(scalar_double("SELECT price FROM orders"), 3901.25);
    EXPECT_EQ(scalar_int("SELECT volume FROM orders"), 5);
    EXPECT_EQ(scalar_int("SELECT volume_traded FROM orders"), 3);
    EXPECT_EQ(scalar_text("SELECT remark FROM orders"), "remark-x");
    EXPECT_EQ(scalar_int("SELECT time FROM orders"), 64800);
    EXPECT_DOUBLE_EQ(scalar_double("SELECT avg_price FROM orders"), 3900.75);
    EXPECT_EQ(scalar_int("SELECT seq FROM orders"), 42);
    EXPECT_EQ(scalar_text("SELECT gateway FROM orders"), "gateway_a");

    // CTP 柜台扩展字段走 ext JSON (schema v1 不进列); 按键取值断言, 不绑定 JSON 键序.
    nlohmann::json ext;
    ASSERT_NO_THROW(ext = nlohmann::json::parse(scalar_text("SELECT ext FROM orders")));
    ASSERT_TRUE(ext.is_object());
    EXPECT_EQ(ext.at(kOrderExtOrderRef).get<std::string>(), "000123");
    EXPECT_EQ(ext.at(kOrderExtOrderSysId).get<std::string>(), "SYS-42");
    EXPECT_EQ(ext.at(kOrderExtVolumeCanceled).get<int64_t>(), 2);
    EXPECT_EQ(ext.at(kOrderExtUpdateTime).get<int64_t>(), 1700000000);
    EXPECT_EQ(ext.at(kOrderExtErrorId).get<int64_t>(), 31);
    EXPECT_EQ(ext.at(kOrderExtErrorMessage).get<std::string>(), "ctp error");
}

// 外部单/手工单/同步单以保留策略标识表达 (ADR 0041 §1: 无 is_external 列).
TEST_F(TdPersistWriterTest, ExternalOrderUsesReservedStrategyId) {
    PersistWriter writer(db_path_, "dztd_test");
    writer.open();
    writer.start_writer();

    OrderRecord record = make_order(8001, 1);
    record.is_external = 1;
    record.base.strategy_id[0] = '\0';
    writer.enqueue(PersistTask{.kind = PersistTask::Kind::Order, .data = record});
    ASSERT_TRUE(flush_writer(writer, std::chrono::seconds(10)));
    writer.stop();

    EXPECT_EQ(scalar_text("SELECT strategy_id FROM orders WHERE order_id=8001"),
              DZ_STRATEGY_ID_EXTERNAL);
}

TEST_F(TdPersistWriterTest, TradePersistedColumnsExact) {
    PersistWriter writer(db_path_, "gateway_b");
    writer.open();
    writer.start_writer();

    TradeRecord record{};
    std::strcpy(record.base.account_id, "acc1");
    std::strcpy(record.trading_day, "20260901");
    std::strcpy(record.base.trade_id, "T900");
    record.base.order_id = 7001;
    std::strcpy(record.base.strategy_id, "stg7");
    std::strcpy(record.base.instrument_id, "IF2509");
    std::strcpy(record.base.exchange_id, "CFFEX");
    record.base.direction = DZ_DIRECTION_LONG;
    record.base.position_effect = DZ_POSITION_EFFECT_OPEN;
    record.base.price = 3902.5;
    record.base.volume = 2;
    record.base.time = 7200;
    record.base.seq = 43;
    writer.enqueue(PersistTask{.kind = PersistTask::Kind::Trade, .data = record});
    ASSERT_TRUE(flush_writer(writer, std::chrono::seconds(10)));
    writer.stop();

    ASSERT_EQ(scalar_int("SELECT COUNT(*) FROM trades"), 1);
    EXPECT_EQ(scalar_text("SELECT account_id FROM trades"), "acc1");
    EXPECT_EQ(scalar_text("SELECT date FROM trades"), "20260901");
    EXPECT_EQ(scalar_text("SELECT trade_id FROM trades"), "T900");
    EXPECT_EQ(scalar_int("SELECT order_id FROM trades"), 7001);
    EXPECT_EQ(scalar_text("SELECT strategy_id FROM trades"), "stg7");
    EXPECT_EQ(scalar_text("SELECT instrument_id FROM trades"), "IF2509");
    EXPECT_EQ(scalar_text("SELECT exchange_id FROM trades"), "CFFEX");
    EXPECT_EQ(scalar_int("SELECT CAST(direction AS INTEGER) FROM trades"), DZ_DIRECTION_LONG);
    EXPECT_EQ(scalar_int("SELECT CAST(position_effect AS INTEGER) FROM trades"),
              DZ_POSITION_EFFECT_OPEN);
    EXPECT_DOUBLE_EQ(scalar_double("SELECT price FROM trades"), 3902.5);
    EXPECT_EQ(scalar_int("SELECT volume FROM trades"), 2);
    EXPECT_EQ(scalar_int("SELECT time FROM trades"), 7200);
    EXPECT_EQ(scalar_int("SELECT seq FROM trades"), 43);
    EXPECT_EQ(scalar_text("SELECT gateway FROM trades"), "gateway_b");
    // ext: CTP 无逐笔扩展, 写 "{}" (契约默认值, 不是 NULL).
    nlohmann::json ext;
    ASSERT_NO_THROW(ext = nlohmann::json::parse(scalar_text("SELECT ext FROM trades")));
    EXPECT_TRUE(ext.is_object());
    EXPECT_TRUE(ext.empty());
}

TEST_F(TdPersistWriterTest, PositionPersistedColumnsExact) {
    PersistWriter writer(db_path_, "gateway_c");
    writer.open();
    writer.start_writer();

    DzPositionInfo record{};
    std::strcpy(record.account_id, "acc1");
    std::strcpy(record.instrument_id, "IF2509");
    std::strcpy(record.exchange_id, "CFFEX");
    record.direction = DZ_DIRECTION_SHORT;  // -1
    record.volume = 9;
    record.frozen_volume = 2;
    record.today_volume = 4;
    record.yd_volume = 5;
    record.price = 3901.5;
    record.seq = 44;
    writer.enqueue(PersistTask{.kind = PersistTask::Kind::Position,
                               .data = std::vector<DzPositionInfo>{record},
                               .account_id = "acc1",
                               .trading_day = kTradingDay});
    ASSERT_TRUE(flush_writer(writer, std::chrono::seconds(10)));
    writer.stop();

    ASSERT_EQ(scalar_int("SELECT COUNT(*) FROM positions"), 1);
    EXPECT_EQ(scalar_text("SELECT account_id FROM positions"), "acc1");
    EXPECT_EQ(scalar_text("SELECT date FROM positions"), "20260901");  // DzDate -> 文本列
    EXPECT_EQ(scalar_text("SELECT instrument_id FROM positions"), "IF2509");
    EXPECT_EQ(scalar_text("SELECT exchange_id FROM positions"), "CFFEX");
    EXPECT_EQ(scalar_int("SELECT CAST(direction AS INTEGER) FROM positions"), DZ_DIRECTION_SHORT);
    EXPECT_EQ(scalar_int("SELECT volume FROM positions"), 9);
    EXPECT_EQ(scalar_int("SELECT frozen_volume FROM positions"), 2);
    EXPECT_EQ(scalar_int("SELECT today_volume FROM positions"), 4);
    EXPECT_EQ(scalar_int("SELECT yd_volume FROM positions"), 5);
    EXPECT_DOUBLE_EQ(scalar_double("SELECT price FROM positions"), 3901.5);
    EXPECT_EQ(scalar_int("SELECT seq FROM positions"), 44);
    EXPECT_EQ(scalar_text("SELECT gateway FROM positions"), "gateway_c");
    EXPECT_EQ(scalar_text("SELECT ext FROM positions"), "{}");
}

TEST_F(TdPersistWriterTest, TradingAccountPersistedColumnsExact) {
    PersistWriter writer(db_path_, "gateway_d");
    writer.open();
    writer.start_writer();

    DzTradingAccount record{};
    std::strcpy(record.account_id, "acc1");
    record.balance = 123456.5;
    record.available = 120000.25;
    record.frozen = 1000.5;
    record.commission = 12.25;
    record.margin = 4444.5;
    record.withdraw_quota = 100000.5;
    record.deposit = 5000.25;
    record.withdraw = 300.75;
    record.seq = 45;
    writer.enqueue(PersistTask{.kind = PersistTask::Kind::TradingAccount,
                               .data = record,
                               .account_id = "acc1",
                               .trading_day = kTradingDay});
    ASSERT_TRUE(flush_writer(writer, std::chrono::seconds(10)));
    writer.stop();

    ASSERT_EQ(scalar_int("SELECT COUNT(*) FROM trading_accounts"), 1);
    EXPECT_EQ(scalar_text("SELECT account_id FROM trading_accounts"), "acc1");
    EXPECT_EQ(scalar_text("SELECT date FROM trading_accounts"), "20260901");
    EXPECT_DOUBLE_EQ(scalar_double("SELECT balance FROM trading_accounts"), 123456.5);
    EXPECT_DOUBLE_EQ(scalar_double("SELECT available FROM trading_accounts"), 120000.25);
    EXPECT_DOUBLE_EQ(scalar_double("SELECT frozen FROM trading_accounts"), 1000.5);
    EXPECT_DOUBLE_EQ(scalar_double("SELECT commission FROM trading_accounts"), 12.25);
    EXPECT_DOUBLE_EQ(scalar_double("SELECT margin FROM trading_accounts"), 4444.5);
    EXPECT_DOUBLE_EQ(scalar_double("SELECT withdraw_quota FROM trading_accounts"), 100000.5);
    EXPECT_DOUBLE_EQ(scalar_double("SELECT deposit FROM trading_accounts"), 5000.25);
    EXPECT_DOUBLE_EQ(scalar_double("SELECT withdraw FROM trading_accounts"), 300.75);
    EXPECT_EQ(scalar_int("SELECT seq FROM trading_accounts"), 45);
    EXPECT_EQ(scalar_text("SELECT gateway FROM trading_accounts"), "gateway_d");
    EXPECT_EQ(scalar_text("SELECT ext FROM trading_accounts"), "{}");
}

// instruments 保留 (ADR 0041 §1: 行情域落地后迁 market.db).
TEST_F(TdPersistWriterTest, InstrumentPersistedColumnsExact) {
    PersistWriter writer(db_path_, "dztd_test");
    writer.open();
    writer.start_writer();

    tdstore::InstrumentRecord record{};
    record.instrument_id = "IF2509";
    record.exchange_id = "CFFEX";
    record.symbol = "IF2509";
    record.name = "hs300 index future";
    record.product_class = DZ_PRODUCT_FUTURES;
    record.product_code = "IF";
    record.settle_cycle = 0;
    record.currency = "CNY";
    record.volume_multiple = 300;
    record.volume_step = 1;
    record.price_tick = 0.2;
    record.min_limit_order_volume = 1;
    record.max_limit_order_volume = 100;
    record.min_market_order_volume = 1;
    record.max_market_order_volume = 5;
    record.listed_date = 20000;
    record.delisted_date = 20600;
    record.update_day = "20260901";
    record.updated_at = 1700000000123;
    writer.enqueue(PersistTask{.kind = PersistTask::Kind::Instrument, .data = record});
    ASSERT_TRUE(flush_writer(writer, std::chrono::seconds(10)));
    writer.stop();

    ASSERT_EQ(scalar_int("SELECT COUNT(*) FROM instruments"), 1);
    EXPECT_EQ(scalar_text("SELECT instrument_id FROM instruments"), "IF2509");
    EXPECT_EQ(scalar_text("SELECT exchange_id FROM instruments"), "CFFEX");
    EXPECT_EQ(scalar_text("SELECT symbol FROM instruments"), "IF2509");
    EXPECT_EQ(scalar_text("SELECT name FROM instruments"), "hs300 index future");
    EXPECT_EQ(scalar_int("SELECT product_class FROM instruments"), DZ_PRODUCT_FUTURES);
    EXPECT_EQ(scalar_text("SELECT product_code FROM instruments"), "IF");
    EXPECT_EQ(scalar_int("SELECT settle_cycle FROM instruments"), 0);
    EXPECT_EQ(scalar_text("SELECT currency FROM instruments"), "CNY");
    EXPECT_DOUBLE_EQ(scalar_double("SELECT volume_multiple FROM instruments"), 300.0);
    EXPECT_DOUBLE_EQ(scalar_double("SELECT price_tick FROM instruments"), 0.2);
    EXPECT_EQ(scalar_int("SELECT min_limit_order_volume FROM instruments"), 1);
    EXPECT_EQ(scalar_int("SELECT max_limit_order_volume FROM instruments"), 100);
    EXPECT_EQ(scalar_int("SELECT min_market_order_volume FROM instruments"), 1);
    EXPECT_EQ(scalar_int("SELECT max_market_order_volume FROM instruments"), 5);
    EXPECT_EQ(scalar_int("SELECT listed_date FROM instruments"), 20000);
    EXPECT_EQ(scalar_int("SELECT delisted_date FROM instruments"), 20600);
    EXPECT_EQ(scalar_int("SELECT updated_at FROM instruments"), 1700000000123LL);
}

// 同键重复 upsert 幂等覆盖 (RESTART 重传 / 盘中状态更新): 四表都只保留最新态, 不追加.
TEST_F(TdPersistWriterTest, UpsertOverwritesLatestStateForAllKinds) {
    PersistWriter writer(db_path_, "dztd_test");
    writer.open();
    writer.start_writer();

    // ---- 第一版 ----
    {
        OrderRecord order = make_order(100, 1);
        order.base.status = DZ_ORDER_PART_TRADED;
        order.base.volume_traded = 1;
        writer.enqueue(PersistTask{.kind = PersistTask::Kind::Order, .data = order});

        TradeRecord trade = make_trade("T001", 2);
        trade.base.price = 3900.0;
        writer.enqueue(PersistTask{.kind = PersistTask::Kind::Trade, .data = trade});

        writer.enqueue(
            PersistTask{.kind = PersistTask::Kind::Position,
                        .data = std::vector<DzPositionInfo>{make_position(5, 3, DZ_DIRECTION_LONG)},
                        .account_id = "acc1",
                        .trading_day = kTradingDay});
        writer.enqueue(PersistTask{.kind = PersistTask::Kind::TradingAccount,
                                   .data = make_trading_account(4),
                                   .account_id = "acc1",
                                   .trading_day = kTradingDay});
    }
    ASSERT_TRUE(flush_writer(writer, std::chrono::seconds(10)));

    // ---- 第二版 (同键, 最新态) ----
    {
        OrderRecord order = make_order(100, 11);
        order.base.status = DZ_ORDER_ALL_TRADED;
        order.base.volume_traded = 2;
        writer.enqueue(PersistTask{.kind = PersistTask::Kind::Order, .data = order});

        TradeRecord trade = make_trade("T001", 12);
        trade.base.price = 3910.5;
        writer.enqueue(PersistTask{.kind = PersistTask::Kind::Trade, .data = trade});

        writer.enqueue(PersistTask{
            .kind = PersistTask::Kind::Position,
            .data = std::vector<DzPositionInfo>{make_position(7, 13, DZ_DIRECTION_LONG)},
            .account_id = "acc1",
            .trading_day = kTradingDay});
        writer.enqueue(PersistTask{.kind = PersistTask::Kind::TradingAccount,
                                   .data = make_trading_account(14),
                                   .account_id = "acc1",
                                   .trading_day = kTradingDay});
    }
    ASSERT_TRUE(flush_writer(writer, std::chrono::seconds(10)));
    writer.stop();

    // 每表仍 1 行 (幂等 upsert, 不是追加), 且各列是最新态.
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM orders"), 1);
    EXPECT_EQ(scalar_int("SELECT CAST(status AS INTEGER) FROM orders"), DZ_ORDER_ALL_TRADED);
    EXPECT_EQ(scalar_int("SELECT volume_traded FROM orders"), 2);
    EXPECT_EQ(scalar_int("SELECT seq FROM orders"), 11);
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM trades"), 1);
    EXPECT_DOUBLE_EQ(scalar_double("SELECT price FROM trades"), 3910.5);
    EXPECT_EQ(scalar_int("SELECT seq FROM trades"), 12);
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM positions"), 1);
    EXPECT_EQ(scalar_int("SELECT volume FROM positions"), 7);
    EXPECT_EQ(scalar_int("SELECT seq FROM positions"), 13);
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM trading_accounts"), 1);
    EXPECT_DOUBLE_EQ(scalar_double("SELECT balance FROM trading_accounts"), 1000000.0);
    EXPECT_EQ(scalar_int("SELECT seq FROM trading_accounts"), 14);
}

// ============================================================================
// 失败策略 (ADR 0042): 批事务原子 / 失败批保留 + 退避重试 + 恢复追平
// ============================================================================

// 批事务原子 (回滚): 删 trades 表注入失败 → 同批 Order 不落库; 外部只读连接只见提交后状态.
// 失败批保留重试 (不是丢弃), 故在失败批 pending 期间断言外部读不到半提交状态.
TEST_F(TdPersistWriterTest, BatchTransactionAtomicOnFailure) {
    PersistWriter writer(db_path_, "dztd_test", fast_policy(), recorder_.callback());
    writer.open();
    raw_exec(db_path_, "DROP TABLE trades");  // 失败注入: Trade upsert 必失败

    // start_writer() 前入队 → 单批 drain, 保证 Order/Trade 同批 (无批切分竞态).
    writer.enqueue(PersistTask{.kind = PersistTask::Kind::Order, .data = make_order(1, 1)});
    writer.enqueue(PersistTask{.kind = PersistTask::Kind::Trade, .data = make_trade("T001", 2)});
    writer.start_writer();

    // 首次失败: ERROR popup=true 仅一次 (ADR 0042 §4).
    ASSERT_TRUE(recorder_.wait_for_count(DZ_NOTIFY_ERROR, 1, std::chrono::seconds(5)));
    const std::vector<PersistNotification> errors = recorder_.notifications_at(DZ_NOTIFY_ERROR);
    ASSERT_EQ(errors.size(), 1u);
    EXPECT_TRUE(errors.front().popup);

    // 回滚: 同批 Order 不可见 (外部只读连接只见提交后状态).
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM orders"), 0);
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='trades'"),
              0);

    writer.stop();  // 失败批做过最后一次立即重试仍失败 → 丢弃并退出, 不挂死
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM orders"), 0);
}

// 失败批不丢、恢复后自动追平 (§5.3): 删表 → 首错 ERROR popup 一次 → ensure_trade_db
// 幂等自愈出缺失表 → 退避重试自动提交同一批; flush token 最终 true, 数据齐全,
// 恢复 INFO 一次; token 已 erase (不泄漏).
TEST_F(TdPersistWriterTest, FailedBatchRetriedUntilRecovery) {
    PersistWriter writer(db_path_, "dztd_test", fast_policy(), recorder_.callback());
    writer.open();
    raw_exec(db_path_, "DROP TABLE trades");

    writer.enqueue(PersistTask{.kind = PersistTask::Kind::Order, .data = make_order(100, 1)});
    const uint64_t pending_token = writer.enqueue_flush_signal();
    writer.enqueue(PersistTask{.kind = PersistTask::Kind::Trade, .data = make_trade("T001", 2)});
    writer.start_writer();

    // 首错: ERROR popup=true 且仅一次 (持续失败不重复弹 ERROR).
    ASSERT_TRUE(recorder_.wait_for_count(DZ_NOTIFY_ERROR, 1, std::chrono::seconds(5)));
    ASSERT_EQ(recorder_.count(DZ_NOTIFY_ERROR), 1u);
    EXPECT_TRUE(recorder_.notifications_at(DZ_NOTIFY_ERROR).front().popup);
    // 失败批的 token 在重试期间保持 pending: 不得超时误报成功 (wait 只等 100ms, 批必失败).
    EXPECT_FALSE(writer.wait_flush(pending_token, std::chrono::milliseconds(100)));
    // 持续失败 = WARN popup=true (fast_policy interval=50ms); ERROR 仍只有一次.
    ASSERT_TRUE(recorder_.wait_for_count(DZ_NOTIFY_WARN, 1, std::chrono::seconds(5)));
    EXPECT_TRUE(recorder_.notifications_at(DZ_NOTIFY_WARN).front().popup);
    EXPECT_EQ(recorder_.count(DZ_NOTIFY_ERROR), 1u);

    // 自愈: ensure_trade_db 幂等补建缺失表 (真实数据库, 不 mock).
    ASSERT_NO_THROW(tdstore::ensure_trade_db(db_path_));

    // 退避重试自动追平: 自愈后新入队的 flush 哨兵 FIFO 排在同批之后 → true 时同批必已提交.
    // (同一 pending token 不支持二次等待: get_future 只可取一次; 已提交 token 被 erase,
    //  再次 wait_flush 按"未知 token=已消费"返回 true — 下方以此断言无 token 泄漏.)
    const uint64_t recovered_token = writer.enqueue_flush_signal();
    ASSERT_TRUE(writer.wait_flush(recovered_token, std::chrono::seconds(10)));
    EXPECT_TRUE(writer.wait_flush(pending_token, std::chrono::milliseconds(0)))
        << "committed flush token must be erased (no pending_flushes_ leak)";
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM orders"), 1);
    EXPECT_EQ(scalar_int("SELECT order_id FROM orders"), 100);
    EXPECT_EQ(scalar_text("SELECT trade_id FROM trades"), "T001");
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM trades"), 1);

    // 恢复 INFO popup=false 仅一次.
    ASSERT_TRUE(recorder_.wait_for_count(DZ_NOTIFY_INFO, 1, std::chrono::seconds(2)));
    const std::vector<PersistNotification> infos = recorder_.notifications_at(DZ_NOTIFY_INFO);
    ASSERT_EQ(infos.size(), 1u);
    EXPECT_FALSE(infos.front().popup);
    EXPECT_EQ(recorder_.count(DZ_NOTIFY_ERROR), 1u);
    writer.stop();
}

// ============================================================================
// 队列: 无上限、入队不阻塞 / 积压阈值告警
// ============================================================================

// 队列无上限不阻塞 (§5.4, 替代已废的 QueueLimitBlocksEnqueue): Writer 未启动时入队
// 1000 条必须立即返回 (旧实现队列满会阻塞调用线程); 启动后全部落库.
TEST_F(TdPersistWriterTest, PersistQueueUnboundedDoesNotBlock) {
    PersistWriter writer(db_path_, "dztd_test");  // 生产默认策略: 无队列上限
    writer.open();

    constexpr int kCount = 1000;
    const auto enqueue_start = std::chrono::steady_clock::now();
    for (int i = 1; i <= kCount; ++i) {
        writer.enqueue(PersistTask{.kind = PersistTask::Kind::Order,
                                   .data = make_order(i, static_cast<uint64_t>(i))});
    }
    const auto enqueue_elapsed = std::chrono::steady_clock::now() - enqueue_start;
    // 负断言: 1000 次纯内存入队应远低于 1s; 旧实现在上限处阻塞调用线程 (永不返回).
    EXPECT_LT(enqueue_elapsed, std::chrono::seconds(1))
        << "enqueue blocked while writer is not running";

    writer.start_writer();
    ASSERT_TRUE(flush_writer(writer, std::chrono::seconds(10)));
    writer.stop();
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM orders"), kCount);
}

// 积压阈值告警 (§5.5): 队列不设上限, 越过 backlog_warn_threshold 只 WARN 一次.
TEST_F(TdPersistWriterTest, BacklogThresholdWarnsOnce) {
    PersistPolicy policy = fast_policy();  // backlog_warn_threshold = 8
    PersistWriter writer(db_path_, "dztd_test", policy, recorder_.callback());
    writer.open();  // 不启动 Writer: 队列持续堆积

    // 未达阈值: 不告警.
    for (int i = 1; i < static_cast<int>(policy.backlog_warn_threshold); ++i) {
        writer.enqueue(PersistTask{.kind = PersistTask::Kind::Order,
                                   .data = make_order(i, static_cast<uint64_t>(i))});
    }
    EXPECT_EQ(recorder_.count(DZ_NOTIFY_WARN), 0u);

    // 第 8 条越过阈值: WARN popup=false 一次 (来自调用 enqueue 的线程).
    writer.enqueue(PersistTask{.kind = PersistTask::Kind::Order, .data = make_order(8, 8)});
    ASSERT_TRUE(recorder_.wait_for_count(DZ_NOTIFY_WARN, 1, std::chrono::seconds(2)));
    const std::vector<PersistNotification> warns = recorder_.notifications_at(DZ_NOTIFY_WARN);
    ASSERT_EQ(warns.size(), 1u);
    EXPECT_FALSE(warns.front().popup);
    EXPECT_FALSE(warns.front().message.empty());

    // 积压期内继续入队不再重复告警.
    for (int i = 9; i <= 20; ++i) {
        writer.enqueue(PersistTask{.kind = PersistTask::Kind::Order,
                                   .data = make_order(i, static_cast<uint64_t>(i))});
    }
    EXPECT_EQ(recorder_.count(DZ_NOTIFY_WARN), 1u);

    writer.start_writer();
    ASSERT_TRUE(flush_writer(writer, std::chrono::seconds(10)));
    writer.stop();
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM orders"), 20);
}

// ============================================================================
// 建连失败策略: 坏库改名重建 / 打开失败退避重连
// ============================================================================

// 坏库改名重建 (§5.6): 垃圾字节写入 db 路径 → open() 重建 (旧文件 .old-* 保留 + ERROR),
// 随后任务照常落库 (交易路径未阻塞).
TEST_F(TdPersistWriterTest, CorruptDbQuarantinedAndRebuilt) {
    const std::string garbage = "this is definitely not a sqlite database file";
    write_file_bytes(db_path_, garbage);

    PersistWriter writer(db_path_, "dztd_test", fast_policy(), recorder_.callback());
    EXPECT_NO_THROW(writer.open());  // 坏库重建不得抛给交易路径
    // 旧文件改名保留 (主文件 *.old-*), 内容不删除 (供排查).
    const std::vector<std::filesystem::path> backups = files_with_prefix(tmp_dir_, "test.db.old-");
    ASSERT_EQ(backups.size(), 1u);
    EXPECT_EQ(read_file_bytes(backups.front()), garbage);
    // 重建事件 ERROR 通知 (ADR 0042 §3/§4: 坏库改名重建记 ERROR).
    ASSERT_TRUE(recorder_.wait_for_count(DZ_NOTIFY_ERROR, 1, std::chrono::seconds(2)));
    EXPECT_TRUE(recorder_.notifications_at(DZ_NOTIFY_ERROR).front().popup);
    // 新库 schema 完整.
    EXPECT_EQ(
        scalar_int("SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND "
                   "name IN ('orders','trades','instruments','positions','trading_accounts')"),
        5);

    // 交易路径未阻塞: 启动后任务照常落库.
    writer.start_writer();
    writer.enqueue(PersistTask{.kind = PersistTask::Kind::Order, .data = make_order(5, 1)});
    ASSERT_TRUE(flush_writer(writer, std::chrono::seconds(10)));
    EXPECT_EQ(scalar_int("SELECT order_id FROM orders"), 5);
    writer.stop();
}

// 打开失败退避重连 (§5.7): db_path 指向目录 (确定性"打不开") → open() 不抛 + ERROR 通知;
// 故障期间入队的任务保留; 移除故障后 Writer 线程重连 (内部走 ensure_trade_db 建库) 并追平积压.
TEST_F(TdPersistWriterTest, OpenFailureRetriesUntilPathRecovers) {
    const std::filesystem::path blocked_path = tmp_dir_ / "blocked.db";
    std::filesystem::create_directories(blocked_path);  // 目录不是交易库文件
    const std::string blocked_db = blocked_path.string();

    PersistWriter writer(blocked_db, "dztd_test", fast_policy(), recorder_.callback());
    EXPECT_NO_THROW(writer.open());  // 建连失败不抛 (ADR 0042 §5: 降级不得阻塞交易)
    ASSERT_TRUE(recorder_.wait_for_count(DZ_NOTIFY_ERROR, 1, std::chrono::seconds(2)));
    EXPECT_TRUE(recorder_.notifications_at(DZ_NOTIFY_ERROR).front().popup);

    // 故障期间入队: 任务保留在内存队列, flush 不误报成功.
    writer.enqueue(PersistTask{.kind = PersistTask::Kind::Order, .data = make_order(1, 1)});
    const uint64_t pending_token = writer.enqueue_flush_signal();
    writer.start_writer();
    EXPECT_FALSE(writer.wait_flush(pending_token, std::chrono::milliseconds(200)));

    // 移除故障 → Writer 线程下一轮建连走 ensure_trade_db 自动建库 → 自动追平积压.
    // 新哨兵 FIFO 排在此前积压之后: 它返回 true 即证明积压任务已提交.
    ASSERT_TRUE(std::filesystem::remove(blocked_path));
    const uint64_t recovered_token = writer.enqueue_flush_signal();
    ASSERT_TRUE(writer.wait_flush(recovered_token, std::chrono::seconds(10)));
    EXPECT_TRUE(writer.wait_flush(pending_token, std::chrono::milliseconds(0)));
    EXPECT_EQ(query_int(blocked_db, "SELECT COUNT(*) FROM orders"), 1);
    EXPECT_EQ(query_int(blocked_db, "SELECT order_id FROM orders"), 1);
    writer.stop();
}

// ============================================================================
// flush 屏障 (FIFO 哨兵) / stop 排空
// ============================================================================

// FIFO 哨兵: 入队哨兵保证此前任务先提交 (wait_flush 返回 ⇒ 数据已提交可见).
TEST_F(TdPersistWriterTest, FlushWaitsForPriorTasksCommitted) {
    PersistWriter writer(db_path_, "dztd_test");
    writer.open();
    writer.start_writer();

    writer.enqueue(PersistTask{.kind = PersistTask::Kind::Order, .data = make_order(1, 1)});
    OrderRecord second = make_order(2, 2);
    std::strcpy(second.order_ref, "000002");
    writer.enqueue(PersistTask{.kind = PersistTask::Kind::Order, .data = second});

    const uint64_t token = writer.enqueue_flush_signal();
    ASSERT_TRUE(writer.wait_flush(token, std::chrono::seconds(10)));  // 上限仅作挂死兜底
    // 此刻 DB 已含 2 行 (此前任务必已提交 — FIFO 哨兵语义).
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM orders"), 2);
    writer.stop();
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM orders"), 2);
}

// 失败批 pending 期间 wait_flush 超时返回 false (不误报成功).
TEST_F(TdPersistWriterTest, FlushTimesOutWhileBatchPending) {
    PersistWriter writer(db_path_, "dztd_test", fast_policy(), recorder_.callback());
    writer.open();
    raw_exec(db_path_, "DROP TABLE trades");  // 失败注入

    writer.enqueue(PersistTask{.kind = PersistTask::Kind::Order, .data = make_order(1, 1)});
    writer.enqueue(PersistTask{.kind = PersistTask::Kind::Trade, .data = make_trade("T001", 2)});
    const uint64_t token = writer.enqueue_flush_signal();
    writer.start_writer();
    ASSERT_TRUE(recorder_.wait_for_count(DZ_NOTIFY_ERROR, 1, std::chrono::seconds(5)));

    // 批未提交: 等待上限 100ms 内必超时 (重试期间 token 保持 pending).
    EXPECT_FALSE(writer.wait_flush(token, std::chrono::milliseconds(100)));
    writer.stop();
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM orders"), 0);
}

// stop() 后入队哨兵丢弃路径须立即 set: 哨兵被"消费"即视为提交完成, wait 恒 true.
TEST_F(TdPersistWriterTest, FlushAfterStopSucceedsImmediately) {
    PersistWriter writer(db_path_, "dztd_test");
    writer.open();
    writer.start_writer();
    const uint64_t token = writer.enqueue_flush_signal();
    ASSERT_TRUE(writer.wait_flush(token, std::chrono::seconds(2)));
    writer.stop();
    // stop() 后再入队哨兵: 丢弃分支立即 set_value, 不得死锁/超时假负.
    const uint64_t token_after_stop = writer.enqueue_flush_signal();
    EXPECT_TRUE(writer.wait_flush(token_after_stop, std::chrono::milliseconds(500)));
}

// 退出排空: 启动 Writer 前入队 100 条 → stop() 必须全部落库.
TEST_F(TdPersistWriterTest, StopDrainsResidualTasks) {
    {
        PersistWriter writer(db_path_, "dztd_test");
        writer.open();
        for (int i = 1; i <= 100; ++i) {
            writer.enqueue(PersistTask{.kind = PersistTask::Kind::Order,
                                       .data = make_order(i, static_cast<uint64_t>(i))});
        }
        writer.start_writer();
        writer.stop();  // 排空残留
    }
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM orders"), 100);
}

// 库不可恢复时 stop 只记 ERROR 不挂死 (ADR 0042 §6: 未提交积压丢弃为已知限制).
TEST_F(TdPersistWriterTest, StopWithUnrecoverableBatchReturnsWithoutHang) {
    PersistWriter writer(db_path_, "dztd_test", fast_policy(), recorder_.callback());
    writer.open();
    raw_exec(db_path_, "DROP TABLE trades");
    writer.enqueue(PersistTask{.kind = PersistTask::Kind::Order, .data = make_order(1, 1)});
    writer.enqueue(PersistTask{.kind = PersistTask::Kind::Trade, .data = make_trade("T001", 2)});
    writer.start_writer();
    ASSERT_TRUE(recorder_.wait_for_count(DZ_NOTIFY_ERROR, 1, std::chrono::seconds(5)));

    const auto stop_start = std::chrono::steady_clock::now();
    writer.stop();  // 最后一次立即重试仍失败 → 记 ERROR 后退出
    const auto stop_elapsed = std::chrono::steady_clock::now() - stop_start;
    EXPECT_LT(stop_elapsed, std::chrono::seconds(5)) << "stop hung on unrecoverable batch";
    // 未提交积压丢弃: 失败批已回滚, 外部无残留行.
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM orders"), 0);
}

// ============================================================================
// PositionRebuild 语义 (ADR 0045 §持仓重建)
// ============================================================================

// 单事务重灌: 清该账户**当日**持仓行 + upsert 本组; 历史交易日行不可变.
TEST_F(TdPersistWriterTest, PositionRebuildReplacesCurrentDayOnly) {
    PersistWriter writer(db_path_, "dztd_test");
    writer.open();
    writer.start_writer();

    // 先灌 trading_day=20260831 的 1 行 (旧日).
    writer.enqueue(
        PersistTask{.kind = PersistTask::Kind::Position,
                    .data = std::vector<DzPositionInfo>{make_position(5, 10, DZ_DIRECTION_LONG)},
                    .account_id = "acc1",
                    .trading_day = kPrevTradingDay});
    ASSERT_TRUE(flush_writer(writer, std::chrono::seconds(2)));
    ASSERT_EQ(scalar_int("SELECT COUNT(*) FROM positions WHERE account_id='acc1'"), 1);

    // rebuild{day=20260901, 2 行}: 同账户两个不同 (instrument, direction).
    std::vector<DzPositionInfo> group;
    group.push_back(make_position(8, 11, DZ_DIRECTION_LONG));                  // IF2506 多
    DzPositionInfo short_position = make_position(3, 12, DZ_DIRECTION_SHORT);  // rb2510 空
    std::strcpy(short_position.instrument_id, "rb2510");
    std::strcpy(short_position.exchange_id, "SHFE");
    group.push_back(short_position);
    writer.enqueue(PersistTask{.kind = PersistTask::Kind::PositionRebuild,
                               .data = std::move(group),
                               .account_id = "acc1",
                               .trading_day = kTradingDay});
    ASSERT_TRUE(flush_writer(writer, std::chrono::seconds(2)));

    // 历史日行原样保留, 当日 2 行在 (同事务原子切换, 无中间态).
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM positions WHERE account_id='acc1'"), 3);
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM positions WHERE date='20260831'"), 1);
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM positions WHERE date='20260901'"), 2);
    writer.stop();
}

// 同事务原子性: rebuild 的 remove+upsert 位于批事务内 — 批内后续任务失败时整体回滚
// (旧行原样保留). 失败批保留重试期间外部读者也只见提交后状态.
TEST_F(TdPersistWriterTest, PositionRebuildRollsBackWhenBatchFails) {
    PersistWriter writer(db_path_, "dztd_test", fast_policy(), recorder_.callback());
    writer.open();
    // 原生连接播种旧日 1 行 (不经 Writer, 便于在 start_writer 前完成).
    raw_exec(db_path_,
             "INSERT INTO positions (account_id, date, instrument_id, exchange_id,"
             " direction, volume, frozen_volume, today_volume, yd_volume, price, seq, gateway)"
             " VALUES ('acc1', '20260831', 'IF2506', 'CFFEX', 1, 5, 0, 5, 0, 3900.0, 10,"
             " 'dztd_test')");
    ASSERT_EQ(scalar_int("SELECT COUNT(*) FROM positions WHERE account_id='acc1'"), 1);

    // 失败注入: 删 trades 表 — 排在 rebuild 之后的 Trade 任务必然失败.
    raw_exec(db_path_, "DROP TABLE trades");
    // start_writer() 前全部入队 → 单批: [PositionRebuild, Trade].
    writer.enqueue(
        PersistTask{.kind = PersistTask::Kind::PositionRebuild,
                    .data = std::vector<DzPositionInfo>{make_position(8, 11, DZ_DIRECTION_LONG)},
                    .account_id = "acc1",
                    .trading_day = kTradingDay});
    writer.enqueue(PersistTask{.kind = PersistTask::Kind::Trade, .data = make_trade("T001", 12)});
    writer.start_writer();
    ASSERT_TRUE(recorder_.wait_for_count(DZ_NOTIFY_ERROR, 1, std::chrono::seconds(5)));

    // 整批回滚: 旧日行原样保留, rebuild 的当日行未提交.
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM positions WHERE account_id='acc1'"), 1);
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM positions WHERE date='20260831'"), 1);
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM positions WHERE date='20260901'"), 0);
    writer.stop();
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM positions WHERE date='20260831'"), 1);
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM positions WHERE date='20260901'"), 0);
}

// 盘中全平: 当日持仓通过 Kind::Position upsert 入 DB, CTP 全平后查询响应不再含该合约 →
// 空组 PositionRebuild 必须清除当日行, 否则幽灵持仓永驻.
TEST_F(TdPersistWriterTest, PositionRebuildClearsClosedCurrentDayRows) {
    PersistWriter writer(db_path_, "dztd_test");
    writer.open();
    writer.start_writer();

    // 当日开仓: 单行 upsert (模拟盘中已落库的持仓).
    writer.enqueue(
        PersistTask{.kind = PersistTask::Kind::Position,
                    .data = std::vector<DzPositionInfo>{make_position(5, 10, DZ_DIRECTION_LONG)},
                    .account_id = "acc1",
                    .trading_day = kTradingDay});
    ASSERT_TRUE(flush_writer(writer, std::chrono::seconds(2)));
    ASSERT_EQ(scalar_int("SELECT COUNT(*) FROM positions WHERE account_id='acc1'"), 1);

    // 全平: 空组 PositionRebuild (当日).
    writer.enqueue(PersistTask{.kind = PersistTask::Kind::PositionRebuild,
                               .data = std::vector<DzPositionInfo>{},
                               .account_id = "acc1",
                               .trading_day = kTradingDay});
    ASSERT_TRUE(flush_writer(writer, std::chrono::seconds(2)));

    // 当日行也被删除 → 幽灵持仓清除.
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM positions WHERE account_id='acc1'"), 0);
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM positions WHERE date='20260901'"), 0);
    writer.stop();
}

// FIFO 原位语义 (load-bearing): 批内任务按原始序生效 — 早于 Rebuild 的 Position 行被
// Rebuild 清除, 晚于 Rebuild 的 Position 行覆盖 Rebuild 结果. 若实现把所有 delete
// 提前到所有 upsert 之前, 早于 Rebuild 的行会意外存活 (本用例可区分).
TEST_F(TdPersistWriterTest, PositionRebuildRespectsTaskOrderWithinBatch) {
    PersistWriter writer(db_path_, "dztd_test");
    writer.open();

    // start_writer() 前全部入队 → 单批 drain, 强制三任务同批处理.
    writer.enqueue(
        PersistTask{.kind = PersistTask::Kind::Position,
                    .data = std::vector<DzPositionInfo>{make_position(5, 10, DZ_DIRECTION_LONG)},
                    .account_id = "acc1",
                    .trading_day = kTradingDay});
    DzPositionInfo rebuild_position = make_position(8, 11, DZ_DIRECTION_SHORT);
    std::strcpy(rebuild_position.instrument_id, "rb2510");
    std::strcpy(rebuild_position.exchange_id, "SHFE");
    writer.enqueue(PersistTask{.kind = PersistTask::Kind::PositionRebuild,
                               .data = std::vector<DzPositionInfo>{rebuild_position},
                               .account_id = "acc1",
                               .trading_day = kTradingDay});
    DzPositionInfo late_position = make_position(9, 12, DZ_DIRECTION_SHORT);
    std::strcpy(late_position.instrument_id, "rb2510");
    std::strcpy(late_position.exchange_id, "SHFE");
    writer.enqueue(PersistTask{.kind = PersistTask::Kind::Position,
                               .data = std::vector<DzPositionInfo>{late_position},
                               .account_id = "acc1",
                               .trading_day = kTradingDay});

    writer.start_writer();
    ASSERT_TRUE(flush_writer(writer, std::chrono::seconds(2)));

    // 早于 Rebuild 的 IF2506 行被清除; rb2510 为晚到 Position 的覆盖值 9.
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM positions WHERE instrument_id='IF2506'"), 0);
    EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM positions WHERE account_id='acc1'"), 1);
    EXPECT_EQ(scalar_int("SELECT volume FROM positions WHERE instrument_id='rb2510'"), 9);
    writer.stop();
}

// Kind::Position / Kind::TradingAccount 单行 upsert (盘中有变化时走它).
// 每列用互不相同的非零值 + 逐列断言: 列序错位 (frozen↔yd、seq 丢失等) 或漏绑必然变红.
TEST_F(TdPersistWriterTest, SingleUpsertPositionAndTradingAccount) {
    {
        PersistWriter writer(db_path_, "dztd_test");
        writer.open();
        writer.start_writer();

        DzPositionInfo position{};
        std::strcpy(position.instrument_id, "IF2506");
        std::strcpy(position.exchange_id, "CFFEX");
        std::strcpy(position.account_id, "acc1");
        position.volume = 6;
        position.frozen_volume = 1;
        position.yd_volume = 2;
        position.today_volume = 4;  // 6 = 4 + 2
        position.price = 3900.5;
        position.direction = DZ_DIRECTION_LONG;
        position.seq = 21;
        writer.enqueue(PersistTask{.kind = PersistTask::Kind::Position,
                                   .data = std::vector<DzPositionInfo>{position},
                                   .account_id = "acc1",
                                   .trading_day = kTradingDay});

        DzTradingAccount account{};
        std::strcpy(account.account_id, "acc1");
        account.balance = 987654.25;
        account.available = 900000.5;
        account.frozen = 20000.25;
        account.commission = 123.5;
        account.margin = 54321.75;
        account.withdraw_quota = 800000.5;
        account.deposit = 1000.25;
        account.withdraw = 500.5;
        account.seq = 22;
        writer.enqueue(PersistTask{.kind = PersistTask::Kind::TradingAccount,
                                   .data = account,
                                   .account_id = "acc1",
                                   .trading_day = kTradingDay});

        ASSERT_TRUE(flush_writer(writer, std::chrono::seconds(10)));

        EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM positions WHERE account_id='acc1'"), 1);
        EXPECT_EQ(scalar_int("SELECT volume FROM positions WHERE instrument_id='IF2506'"), 6);
        EXPECT_EQ(scalar_int("SELECT today_volume FROM positions WHERE instrument_id='IF2506'"), 4);
        EXPECT_EQ(scalar_int("SELECT yd_volume FROM positions WHERE instrument_id='IF2506'"), 2);
        EXPECT_EQ(scalar_int("SELECT frozen_volume FROM positions WHERE instrument_id='IF2506'"),
                  1);
        EXPECT_DOUBLE_EQ(scalar_double("SELECT price FROM positions WHERE instrument_id='IF2506'"),
                         3900.5);
        EXPECT_EQ(scalar_int("SELECT seq FROM positions WHERE instrument_id='IF2506'"), 21);
        EXPECT_EQ(scalar_int("SELECT COUNT(*) FROM trading_accounts WHERE account_id='acc1'"), 1);
        EXPECT_DOUBLE_EQ(
            scalar_double("SELECT balance FROM trading_accounts WHERE account_id='acc1'"),
            987654.25);
        EXPECT_DOUBLE_EQ(scalar_double("SELECT available FROM trading_accounts"), 900000.5);
        EXPECT_DOUBLE_EQ(scalar_double("SELECT frozen FROM trading_accounts"), 20000.25);
        EXPECT_DOUBLE_EQ(scalar_double("SELECT commission FROM trading_accounts"), 123.5);
        EXPECT_DOUBLE_EQ(scalar_double("SELECT margin FROM trading_accounts"), 54321.75);
        EXPECT_DOUBLE_EQ(scalar_double("SELECT withdraw_quota FROM trading_accounts"), 800000.5);
        EXPECT_DOUBLE_EQ(scalar_double("SELECT deposit FROM trading_accounts"), 1000.25);
        EXPECT_DOUBLE_EQ(scalar_double("SELECT withdraw FROM trading_accounts"), 500.5);
        EXPECT_EQ(scalar_int("SELECT seq FROM trading_accounts WHERE account_id='acc1'"), 22);
        writer.stop();
    }
}

}  // namespace
}  // namespace dztrader::ctp
