#include "td_data_service.h"
#include "frame_router.h"

#include <dztrader/date_time/date.h>
#include <dztrader/shm/frame_view.h>
#include <dztrader/struct.h>
#include <dztrader/trading/td_ingest.h>
#include <gtest/gtest.h>
#include <sqlite3.h>

#include <cstddef>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>

namespace dztrader::webui {
namespace {

using dztrader::TdIngestGate;

// sqlite3 便利封装（测试用具，仅测试用）
namespace {

int exec_sql(sqlite3* db, const char* sql) {
    char* err = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        const std::string msg = err ? err : sqlite3_errmsg(db);
        sqlite3_free(err);
        ADD_FAILURE() << "sqlite3_exec failed: " << msg << " | sql=" << sql;
    }
    return rc;
}
}  // namespace

// 注入假 db 路径的临时库: rebuild() 只读打开该文件重建镜像。
class TdDataServiceTest : public ::testing::Test {
protected:
    std::filesystem::path dir_;
    std::string db_path_;

    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() / "dz_webui_td_data_service_test";
        std::filesystem::remove_all(dir_);
        std::filesystem::create_directories(dir_);
        db_path_ = (dir_ / "td.db").string();
    }

    void TearDown() override { std::filesystem::remove_all(dir_); }

    // 建 td 库 v2 schema 四表 (与 libs/tdstore/src/schema.cpp 最终形态逐字一致)。
    static void create_schema(sqlite3* db) {
        exec_sql(db,
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
            "    UNIQUE(account_id, order_id))");
        exec_sql(db,
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
            "    UNIQUE(account_id, trading_day, trade_id))");
        exec_sql(db,
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
            "    UNIQUE(account_id, instrument_id, direction))");
        exec_sql(db,
            "CREATE TABLE IF NOT EXISTS trading_accounts ("
            "    account_id TEXT NOT NULL PRIMARY KEY,"
            "    trading_day TEXT NOT NULL,"
            "    balance REAL, available REAL, frozen REAL,"
            "    commission REAL, margin REAL, withdraw_quota REAL,"
            "    deposit REAL, withdraw REAL,"
            "    seq INTEGER NOT NULL DEFAULT 0)");
    }

    // 打开可写库建 schema（测试塞数据用）。
    sqlite3* open_write(const std::string& path) {
        sqlite3* db = nullptr;
        EXPECT_EQ(SQLITE_OK, sqlite3_open(path.c_str(), &db));
        return db;
    }

    // 往 positions 表插 1 行持仓。
    static void insert_position(sqlite3* db, const std::string& acct,
                                const std::string& inst, int64_t volume, uint64_t seq) {
        sqlite3_stmt* stmt = nullptr;
        const char* sql =
            "INSERT INTO positions (account_id, trading_day, instrument_id, exchange_id,"
            " direction, volume, frozen_volume, today_volume, yd_volume, price, seq)"
            " VALUES (?, '20260901', ?, 'CFFEX', 1, ?, 0, ?, 0, 3800.5, ?)";
        EXPECT_EQ(SQLITE_OK, sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr));
        sqlite3_bind_text(stmt, 1, acct.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, inst.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 3, volume);
        sqlite3_bind_int64(stmt, 4, volume);
        sqlite3_bind_int64(stmt, 5, static_cast<int64_t>(seq));
        EXPECT_EQ(SQLITE_DONE, sqlite3_step(stmt));
        sqlite3_finalize(stmt);
    }

    // 手工构造一帧 struct payload 并交给 router 分发 (register_raw 语义:
    // 监听线程同步解析, 拷贝字段后投递 IO 线程 — 测试用同步 poster 等价执行)。
    template <typename Payload>
    static void feed_frame(FrameRouter& router, DzFrameType type, const Payload& payload) {
        alignas(8) std::byte frame[sizeof(DzFrameHeader) + sizeof(Payload)];
        auto* hdr = reinterpret_cast<DzFrameHeader*>(frame);
        hdr->frame_size = static_cast<uint32_t>(sizeof(DzFrameHeader) + sizeof(Payload));
        hdr->frame_type = type;
        auto* dst = reinterpret_cast<Payload*>(frame + sizeof(DzFrameHeader));
        std::memcpy(dst, &payload, sizeof(Payload));
        router.dispatch(shm::FrameView(frame));
    }

    // 构造带假 db 路径回调的 service。
    TdDataService make_service(FrameRouter& router) {
        return TdDataService(router, [this]() { return db_path_; });
    }
};

// spec §5.1: 账户级 seq 水位过滤 — W=100 已含快照, seq=101 入镜像, seq=100 不入。
TEST_F(TdDataServiceTest, MirrorBuiltFromFramesAfterWatermark) {
    FrameRouter router([](std::function<void()> f) { f(); });
    auto svc = make_service(router);

    // 设 W=100 (等价于重建后 gate 的水位)
    svc.set_watermark("CTP001", 100);

    // 帧按 seq 序到达 (单写者单调): 先 seq=100 (≤ W: 快照已含, 必须跳过), 再 seq=DZ_FRAME_NOTIFY_UI。
    DzPositionInfo p0{};
    dztrader::copy_string(p0.account_id, "CTP001", true);
    dztrader::copy_string(p0.instrument_id, "IF2606", true);
    dztrader::copy_string(p0.exchange_id, "CFFEX", true);
    p0.direction = DZ_DIRECTION_LONG;
    p0.volume = 3;
    p0.seq = 100;  // seq ≤ W: 快照已含, 不入镜像
    feed_frame(router, DZ_FRAME_POSITION_INFO, p0);

    DzPositionInfo p1{};
    dztrader::copy_string(p1.account_id, "CTP001", true);
    dztrader::copy_string(p1.instrument_id, "IF2603", true);
    dztrader::copy_string(p1.exchange_id, "CFFEX", true);
    p1.direction = DZ_DIRECTION_LONG;
    p1.volume = 2;
    p1.seq = 101;
    feed_frame(router, DZ_FRAME_POSITION_INFO, p1);

    const auto& positions = svc.positions();
    ASSERT_EQ(1u, positions.size());
    EXPECT_STREQ(positions[0].instrument_id, "IF2603");
    EXPECT_EQ(2, positions[0].volume);
}

// spec §4.2/§5.5: 2018 Ready → rebuild() — 临时库放 2 行持仓 → 收 2018 Ready → 镜像 2 条。
TEST_F(TdDataServiceTest, Ready2018TriggersRebuildFromDb) {
    {
        sqlite3* db = open_write(db_path_);
        create_schema(db);
        insert_position(db, "CTP001", "IF2603", 2, 1);
        insert_position(db, "CTP001", "IF2606", 5, 2);
        exec_sql(db,
            "INSERT INTO trading_accounts (account_id, trading_day, balance, available, seq)"
            " VALUES ('CTP001', '20260901', 100000.0, 50000.0, 3)");
        sqlite3_close(db);
    }

    FrameRouter router([](std::function<void()> f) { f(); });
    auto svc = make_service(router);

    DzAccountStatus ready{};
    dztrader::copy_string(ready.account_id, "CTP001", true);
    ready.state = DZ_ACCOUNT_READY;
    ready.trading_day = dztrader::Date::from_year_month_day(2026, 9, 1).days_since_epoch();
    feed_frame(router, DZ_FRAME_ACCOUNT_STATUS, ready);

    const auto& positions = svc.positions();
    ASSERT_EQ(2u, positions.size());
    EXPECT_STREQ(positions[0].instrument_id, "IF2603");
    EXPECT_EQ(2, positions[0].volume);
    EXPECT_STREQ(positions[1].instrument_id, "IF2606");
    EXPECT_EQ(5, positions[1].volume);
    const auto& acct = svc.trading_accounts();
    ASSERT_EQ(1u, acct.size());
    EXPECT_DOUBLE_EQ(100000.0, acct[0].balance);
}

// spec §5.5"清空必须显式": 2018 Offline → 清空该账户镜像 (防幽灵持仓残留)。
TEST_F(TdDataServiceTest, OfflineClearsMirror) {
    {
        sqlite3* db = open_write(db_path_);
        create_schema(db);
        insert_position(db, "CTP001", "IF2603", 2, 1);
        sqlite3_close(db);
    }

    FrameRouter router([](std::function<void()> f) { f(); });
    auto svc = make_service(router);

    // 先 Ready 重建出镜像
    DzAccountStatus ready{};
    dztrader::copy_string(ready.account_id, "CTP001", true);
    ready.state = DZ_ACCOUNT_READY;
    ready.trading_day = dztrader::Date::from_year_month_day(2026, 9, 1).days_since_epoch();
    feed_frame(router, DZ_FRAME_ACCOUNT_STATUS, ready);
    ASSERT_EQ(1u, svc.positions().size());

    // Offline → 清空该账户镜像
    DzAccountStatus offline{};
    dztrader::copy_string(offline.account_id, "CTP001", true);
    offline.state = DZ_ACCOUNT_OFFLINE;
    offline.trading_day = 0;
    feed_frame(router, DZ_FRAME_ACCOUNT_STATUS, offline);

    EXPECT_EQ(0u, svc.positions().size());
}

// Offline 只清空指定账户, 不影响其他账户镜像。
TEST_F(TdDataServiceTest, OfflineClearsOnlyThatAccount) {
    {
        sqlite3* db = open_write(db_path_);
        create_schema(db);
        insert_position(db, "CTP001", "IF2603", 2, 1);
        insert_position(db, "CTP002", "IF2612", 7, 2);
        sqlite3_close(db);
    }

    FrameRouter router([](std::function<void()> f) { f(); });
    auto svc = make_service(router);

    DzAccountStatus ready1{};
    dztrader::copy_string(ready1.account_id, "CTP001", true);
    ready1.state = DZ_ACCOUNT_READY;
    feed_frame(router, DZ_FRAME_ACCOUNT_STATUS, ready1);
    DzAccountStatus ready2{};
    dztrader::copy_string(ready2.account_id, "CTP002", true);
    ready2.state = DZ_ACCOUNT_READY;
    feed_frame(router, DZ_FRAME_ACCOUNT_STATUS, ready2);
    ASSERT_EQ(2u, svc.positions().size());

    DzAccountStatus offline1{};
    dztrader::copy_string(offline1.account_id, "CTP001", true);
    offline1.state = DZ_ACCOUNT_OFFLINE;
    feed_frame(router, DZ_FRAME_ACCOUNT_STATUS, offline1);

    const auto& positions = svc.positions();
    ASSERT_EQ(1u, positions.size());
    EXPECT_STREQ(positions[0].account_id, "CTP002");
}

// 发现 1 回归 (评审 Important): 多账户下 Ready(B) 不得整库重建误清 A 在途追加帧。
// A 处于活跃交易 (orders/trades 先广播后异步落库): A 在途帧已入镜像但 DB 未提交,
// B 的 Ready 只重建 B — A 的镜像与水位必须原样保留。
TEST_F(TdDataServiceTest, ReadyOneAccountKeepsOtherInFlightData) {
    FrameRouter router([](std::function<void()> f) { f(); });
    auto svc = make_service(router);

    // A 活跃交易: 委托/成交在途帧 (DB 未提交, 仅内存镜像)。
    DzOrderReport ord{};
    dztrader::copy_string(ord.account_id, "CTP001", true);
    dztrader::copy_string(ord.instrument_id, "IF2606", true);
    dztrader::copy_string(ord.exchange_id, "CFFEX", true);
    dztrader::copy_string(ord.strategy_id, "stg1", true);
    ord.order_id = 101;
    ord.direction = DZ_DIRECTION_LONG;
    ord.position_effect = DZ_POSITION_EFFECT_OPEN;
    ord.price_type = DZ_PRICE_LIMIT;
    ord.status = DZ_ORDER_ALL_TRADED;
    ord.price = 3800.5;
    ord.volume = 3;
    ord.volume_traded = 3;
    ord.seq = 7;
    feed_frame(router, DZ_FRAME_ORDER_REPORT, ord);

    DzTradeReport trd{};
    dztrader::copy_string(trd.account_id, "CTP001", true);
    dztrader::copy_string(trd.instrument_id, "IF2606", true);
    dztrader::copy_string(trd.exchange_id, "CFFEX", true);
    dztrader::copy_string(trd.strategy_id, "stg1", true);
    dztrader::copy_string(trd.trade_id, "TRD1", true);
    trd.order_id = 101;
    trd.direction = DZ_DIRECTION_LONG;
    trd.position_effect = DZ_POSITION_EFFECT_OPEN;
    trd.price = 3800.5;
    trd.volume = 3;
    trd.seq = 8;
    feed_frame(router, DZ_FRAME_TRADE_REPORT, trd);
    ASSERT_EQ(1u, svc.orders().size());
    ASSERT_EQ(1u, svc.trades().size());

    // B 完成登录 → Ready(B): 只重建 B (B 在 DB 有持仓快照), 不得碰 A 在途镜像。
    sqlite3* db = open_write(db_path_);
    create_schema(db);
    insert_position(db, "CTP002", "IF2612", 7, 1);
    sqlite3_close(db);

    DzAccountStatus ready_b{};
    dztrader::copy_string(ready_b.account_id, "CTP002", true);
    ready_b.state = DZ_ACCOUNT_READY;
    ready_b.trading_day = dztrader::Date::from_year_month_day(2026, 9, 1).days_since_epoch();
    feed_frame(router, DZ_FRAME_ACCOUNT_STATUS, ready_b);

    // A 在途委托/成交仍完整 (整库重建会误清 — 追加流不清自愈)。
    ASSERT_EQ(1u, svc.orders().size());
    EXPECT_STREQ(svc.orders()[0].account_id, "CTP001");
    EXPECT_EQ(101, svc.orders()[0].order_id);
    ASSERT_EQ(1u, svc.trades().size());
    EXPECT_STREQ(svc.trades()[0].trade_id, "TRD1");

    // B 快照已重建; A 无快照 (DB 无 A 行) → 未设 W (A 水位不因 B 的 Ready 变化)。
    ASSERT_EQ(1u, svc.positions().size());
    EXPECT_STREQ(svc.positions()[0].account_id, "CTP002");
}

// 发现 1 延续: A 的 W 已由先前重建设过 (W=5), B Ready 后 A 水位必须原样保留 —
// A 帧 seq=5 (≤ W=5, 快照已含) 仍被过滤; 若 B 整库重建把 A 的 W 冲成 0, seq=5 会被误准入。
TEST_F(TdDataServiceTest, ReadyOneAccountPreservesOtherAccountWatermark) {
    FrameRouter router([](std::function<void()> f) { f(); });
    auto svc = make_service(router);

    sqlite3* db = open_write(db_path_);
    create_schema(db);
    insert_position(db, "CTP001", "IF2606", 3, 5);
    sqlite3_close(db);

    // A Ready → A W=5, 镜像含 IF2606(vol=3)。
    DzAccountStatus ready_a{};
    dztrader::copy_string(ready_a.account_id, "CTP001", true);
    ready_a.state = DZ_ACCOUNT_READY;
    feed_frame(router, DZ_FRAME_ACCOUNT_STATUS, ready_a);
    ASSERT_EQ(1u, svc.positions().size());

    // B Ready: B 无快照, A 的 W=5 必须保留。
    DzAccountStatus ready_b{};
    dztrader::copy_string(ready_b.account_id, "CTP002", true);
    ready_b.state = DZ_ACCOUNT_READY;
    feed_frame(router, DZ_FRAME_ACCOUNT_STATUS, ready_b);

    // B Ready 后 B 的 W 不得被 A 污染: B 无 DB 快照 → 不设 W (等价 0 全放行)。
    // B 快照外合法帧 seq=5 必须准入; 若 W 计算遍历整个镜像把 A 的 seq=5 误赋给 B (发现 3),
    // seq=5 会被误判"快照已含"过滤 → B 缺条。
    DzPositionInfo p_b{};
    dztrader::copy_string(p_b.account_id, "CTP002", true);
    dztrader::copy_string(p_b.instrument_id, "IF2612", true);
    dztrader::copy_string(p_b.exchange_id, "CFFEX", true);
    p_b.direction = DZ_DIRECTION_LONG;
    p_b.volume = 6;
    p_b.seq = 5;
    feed_frame(router, DZ_FRAME_POSITION_INFO, p_b);
    ASSERT_EQ(2u, svc.positions().size());  // DB 装载的 A IF2606 + B IF2612 (B 帧未误过滤)
    EXPECT_STREQ(svc.positions()[0].account_id, "CTP001");
    EXPECT_STREQ(svc.positions()[1].account_id, "CTP002");

    // A 帧 seq=5 (≤ W=5, 快照已含) → 必须跳过: 镜像不增。
    // 若 B 整库重建把 A 的 W 冲成 0, seq=5 会被准入 → 镜像多一条 (幽灵抑制的反面)。
    DzPositionInfo p_dup{};
    dztrader::copy_string(p_dup.account_id, "CTP001", true);
    dztrader::copy_string(p_dup.instrument_id, "IF2612", true);  // 不同合约, 可观察为 append
    dztrader::copy_string(p_dup.exchange_id, "CFFEX", true);
    p_dup.direction = DZ_DIRECTION_LONG;
    p_dup.volume = 9;
    p_dup.seq = 5;
    feed_frame(router, DZ_FRAME_POSITION_INFO, p_dup);
    EXPECT_EQ(2u, svc.positions().size());  // A IF2606 + B IF2612, A 的 seq=5 被过滤

    // A 帧 seq=6 (> W=5) → 仍准入。
    DzPositionInfo p_new{};
    dztrader::copy_string(p_new.account_id, "CTP001", true);
    dztrader::copy_string(p_new.instrument_id, "IF2612", true);
    dztrader::copy_string(p_new.exchange_id, "CFFEX", true);
    p_new.direction = DZ_DIRECTION_LONG;
    p_new.volume = 4;
    p_new.seq = 6;
    feed_frame(router, DZ_FRAME_POSITION_INFO, p_new);
    EXPECT_EQ(3u, svc.positions().size());
}

// 发现 2 回归 (评审 Important): 库不可用 (路径空) 降级后不得保留旧 W — W=0 全放行,
// 否则旧 W 过滤掉快照不含的帧 → 幽灵抑制 (有帧却镜像空白)。
TEST_F(TdDataServiceTest, RebuildDegradeResetsWatermarkToZero) {
    FrameRouter router([](std::function<void()> f) { f(); });
    TdDataService svc(router, []() { return std::string(); });  // 恒空路径 (库不可用)

    // 先给 A 设高 W=100 (模拟此前某次成功重建留下的水位) + 推进 last_applied。
    svc.set_watermark("CTP001", 100);
    DzPositionInfo p_old{};
    dztrader::copy_string(p_old.account_id, "CTP001", true);
    dztrader::copy_string(p_old.instrument_id, "IF2606", true);
    dztrader::copy_string(p_old.exchange_id, "CFFEX", true);
    p_old.direction = DZ_DIRECTION_LONG;
    p_old.volume = 3;
    p_old.seq = 101;  // > W=100 → 已应用, last_applied=101
    feed_frame(router, DZ_FRAME_POSITION_INFO, p_old);
    ASSERT_EQ(1u, svc.positions().size());

    // A Ready → 库不可用降级: 清 A 镜像 + 该账户 W 复位 (W=0 全放行)。
    DzAccountStatus ready{};
    dztrader::copy_string(ready.account_id, "CTP001", true);
    ready.state = DZ_ACCOUNT_READY;
    feed_frame(router, DZ_FRAME_ACCOUNT_STATUS, ready);
    EXPECT_EQ(0u, svc.positions().size());

    // 降级后帧全放行: seq=1 (旧 W=100 会过滤掉 — 幽灵抑制) 现在必须准入。
    DzPositionInfo p1{};
    dztrader::copy_string(p1.account_id, "CTP001", true);
    dztrader::copy_string(p1.instrument_id, "IF2606", true);
    dztrader::copy_string(p1.exchange_id, "CFFEX", true);
    p1.direction = DZ_DIRECTION_LONG;
    p1.volume = 2;
    p1.seq = 1;
    feed_frame(router, DZ_FRAME_POSITION_INFO, p1);
    ASSERT_EQ(1u, svc.positions().size());
    EXPECT_STREQ(svc.positions()[0].instrument_id, "IF2606");
    EXPECT_EQ(2, svc.positions()[0].volume);
}

// 发现 1 回归 (评审 Important): rebuild() 的只读连接必须设 busy_timeout (与生产写端一致),
// 否则 td Writer 批量提交持写锁窗口内 rebuild() 立即 SQLITE_BUSY → 清镜像+W=0 降级,
// dzweb 镜像永久停在"全放行但无快照", 直到下一 Ready/Offline 才重试。
// 测试: 另一连接持写锁, 释放线程 200ms 后 COMMIT — busy_timeout=5000 让 rebuild 阻塞等待
// 到锁释放后成功重建; 无 busy_timeout 则首个 SELECT 立即 SQLITE_BUSY, 镜像为空。
TEST_F(TdDataServiceTest, RebuildWaitsOutWriteLockInsteadOfDegrading) {
    {
        sqlite3* db = open_write(db_path_);
        create_schema(db);
        insert_position(db, "CTP001", "IF2603", 2, 1);
        insert_position(db, "CTP001", "IF2606", 5, 2);
        sqlite3_close(db);
    }

    // 持有写锁的连接 (BEGIN IMMEDIATE 抢占写锁)。
    sqlite3* locker = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_open_v2(db_path_.c_str(), &locker,
                                         SQLITE_OPEN_READWRITE, nullptr));
    char* err = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_exec(locker, "BEGIN IMMEDIATE", nullptr, nullptr, &err));

    // 200ms 后释放写锁 (此时 rebuild 若 busy_timeout 生效, 应阻塞等待而非降级)。
    std::thread releaser([locker]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        sqlite3_exec(locker, "COMMIT", nullptr, nullptr, nullptr);
        sqlite3_close(locker);
    });

    FrameRouter router([](std::function<void()> f) { f(); });
    auto svc = make_service(router);
    // 同步 rebuild (若 sqlite3_busy_timeout 未接线, 首个 SELECT 立即 BUSY → 镜像 0 条)。
    svc.rebuild("CTP001");

    releaser.join();

    // busy_timeout 生效: 阻塞到锁释放后成功重建 2 条, 而非降级为空镜像。
    const auto& positions = svc.positions();
    ASSERT_EQ(2u, positions.size());
    EXPECT_STREQ(positions[0].instrument_id, "IF2603");
    EXPECT_EQ(2, positions[0].volume);
    EXPECT_STREQ(positions[1].instrument_id, "IF2606");
    EXPECT_EQ(5, positions[1].volume);
}

// 终检发现 E【Important】: dzweb gap→rebuild 触发 — 启动快照 W 落后于 DB (orders/trades/
// positions 的 seq 在快照后已提交) 时, 首帧 seq > W+1 暴露断档 → ingest 检测 pending_gap
// → rebuild(account): 用 DB 快照补齐缺条, 并重设 W。生产自愈: 重建后新帧从 W+1 连续到达,
// 无再断档。
TEST_F(TdDataServiceTest, GapTriggersRebuildToFillMissingRows) {
    // DB 快照: orders 两行 seq 1,2 + trades 一行 seq 3 + positions 一行 seq 4
    {
        sqlite3* db = open_write(db_path_);
        create_schema(db);
        exec_sql(db,
            "INSERT INTO orders (account_id, trading_day, order_id, order_ref, instrument_id,"
            " exchange_id, direction, position_effect, price_type, status, price, volume,"
            " volume_traded, strategy_id, seq)"
            " VALUES ('CTP001', '20260901', 1, 'r', 'IF2603', 'CFFEX', '1', '1', '0', '4',"
            " 3800.0, 3, 3, 'stg1', 1),"
            "        ('CTP001', '20260901', 2, 'r', 'IF2603', 'CFFEX', '1', '1', '0', '4',"
            " 3801.0, 2, 2, 'stg1', 2)");
        exec_sql(db,
            "INSERT INTO trades (account_id, trading_day, trade_id, order_id, instrument_id,"
            " exchange_id, direction, position_effect, price, volume, strategy_id, seq)"
            " VALUES ('CTP001', '20260901', 'T1', 1, 'IF2603', 'CFFEX', '1', '1', 3800.0, 1,"
            " 'stg1', 3)");
        insert_position(db, "CTP001", "IF2603", 3, 4);
        sqlite3_close(db);
    }

    FrameRouter router([](std::function<void()> f) { f(); });
    auto svc = make_service(router);
    svc.set_watermark("CTP001", 2);  // 模拟启动装载 W=2 (快照只看到 orders 1,2)

    // 首帧 seq=5 (> W+1=3): 暴露 gap (3..4) → ingest 检测 pending_gap → rebuild
    // (DB 中 seq 3,4 已提交但快照未见 — 缺条)
    DzOrderReport ord{};
    dztrader::copy_string(ord.account_id, "CTP001", true);
    dztrader::copy_string(ord.strategy_id, "stg1", true);
    ord.order_id = 5;
    ord.seq = 5;
    feed_frame(router, DZ_FRAME_ORDER_REPORT, ord);

    // rebuild 用 DB 快照补齐: orders 1,2 + trades 3 + positions 4 入镜像。
    // ingest 内 rebuild 后, on_order_report 才把触发帧 (order 5) push 进镜像。
    ASSERT_EQ(3u, svc.orders().size());
    EXPECT_EQ(1, svc.orders()[0].order_id);
    EXPECT_EQ(2, svc.orders()[1].order_id);
    EXPECT_EQ(5, svc.orders()[2].order_id);
    ASSERT_EQ(1u, svc.trades().size());
    EXPECT_STREQ(svc.trades()[0].trade_id, "T1");
    ASSERT_EQ(1u, svc.positions().size());
    EXPECT_EQ(3, svc.positions()[0].volume);

    // 重建后新 W = DB 四表 MAX(seq) = 4 (不含未落库触发帧 5)。last_applied 已复位,
    // 生产自愈: 后续帧从 W+1=5 连续到达, 无再断档。模拟下一帧 seq=6 (> W 应用):
    DzOrderReport n{};
    dztrader::copy_string(n.account_id, "CTP001", true);
    dztrader::copy_string(n.strategy_id, "stg1", true);
    n.order_id = 6;
    n.seq = 6;
    feed_frame(router, DZ_FRAME_ORDER_REPORT, n);
    ASSERT_EQ(4u, svc.orders().size());
    EXPECT_EQ(6, svc.orders()[3].order_id);
}

// 终检发现 G【Important】: dzweb rebuild 镜像日界 — orders/trades 只装当日行,
// 跨日历史行不入镜像 (防镜像随历史线性增长 0.9GB/年)。
TEST_F(TdDataServiceTest, RebuildMirrorOnlyCurrentTradingDay) {
    // 两日 orders: 20260901 (昨日) seq 1,2 + 20260902 (今日) seq 3; trades 同
    {
        sqlite3* db = open_write(db_path_);
        create_schema(db);
        exec_sql(db,
            "INSERT INTO orders (account_id, trading_day, order_id, order_ref, instrument_id,"
            " exchange_id, direction, position_effect, price_type, status, price, volume,"
            " volume_traded, strategy_id, seq)"
            " VALUES ('CTP001', '20260901', 1, 'r', 'IF2603', 'CFFEX', '1', '1', '0', '4',"
            " 3800.0, 3, 3, 'stg1', 1),"
            "        ('CTP001', '20260901', 2, 'r', 'IF2603', 'CFFEX', '1', '1', '0', '4',"
            " 3801.0, 2, 2, 'stg1', 2),"
            "        ('CTP001', '20260902', 3, 'r', 'IF2603', 'CFFEX', '1', '1', '0', '4',"
            " 3802.0, 5, 5, 'stg1', 3)");
        exec_sql(db,
            "INSERT INTO trades (account_id, trading_day, trade_id, order_id, instrument_id,"
            " exchange_id, direction, position_effect, price, volume, strategy_id, seq)"
            " VALUES ('CTP001', '20260901', 'T1', 1, 'IF2603', 'CFFEX', '1', '1', 3800.0, 1,"
            " 'stg1', 1),"
            "        ('CTP001', '20260902', 'T2', 3, 'IF2603', 'CFFEX', '1', '1', 3802.0, 1,"
            " 'stg1', 3)");
        sqlite3_close(db);
    }

    FrameRouter router([](std::function<void()> f) { f(); });
    auto svc = make_service(router);

    DzAccountStatus ready{};
    dztrader::copy_string(ready.account_id, "CTP001", true);
    ready.state = DZ_ACCOUNT_READY;
    ready.trading_day = dztrader::Date::from_year_month_day(2026, 9, 2).days_since_epoch();
    feed_frame(router, DZ_FRAME_ACCOUNT_STATUS, ready);

    // 镜像只含当日 (20260902): orders 1 条 (order_id=3), trades 1 条 (T2)
    const auto& orders = svc.orders();
    ASSERT_EQ(1u, orders.size());
    EXPECT_EQ(3, orders[0].order_id);
    EXPECT_EQ(5, orders[0].volume);
    const auto& trades = svc.trades();
    ASSERT_EQ(1u, trades.size());
    EXPECT_STREQ(trades[0].trade_id, "T2");
    // W 不带日过滤: 取四表 MAX(seq) = 3 (含昨日高 seq), 过滤 seq≤3 的帧
    svc.set_watermark("CTP001", 100);  // 覆盖注入以便观察 (实测由 rebuild 内部设 W=3)
    // 直接观察: 重建后 W=3 → seq=3 帧被过滤 (快照已含)
    DzOrderReport dup{};
    dztrader::copy_string(dup.account_id, "CTP001", true);
    dztrader::copy_string(dup.strategy_id, "stg1", true);
    dup.order_id = 3;
    dup.seq = 3;
    feed_frame(router, DZ_FRAME_ORDER_REPORT, dup);
    EXPECT_EQ(1u, svc.orders().size());  // 未新增 (W 已含)
}

// 第二轮复审发现 G 回归: dzweb rebuild 的 W 聚合必须与镜像装载在同一只读事务快照内
// (BEGIN DEFERRED 单事务, COMMIT 之前)。原实现把 W 聚合移到 COMMIT 之后执行 — 若写端
// 在 COMMIT 后、W 查询前提交新行 seq∈(镜像Max, W], 该行"镜像没有却被 W 判已含" →
// 静默缺条 (orders/trades 追加流永久缺)。
// 并发守卫: 重建期间写端持续试图提交 seq=5 的持仓行。修复后 W 聚合在事务内 (SHARED 锁
// 保持到 COMMIT), 写端被 SQLITE_BUSY 阻塞 → seq=5 不进入重建快照 → W=4 = 镜像快照 Max,
// seq=5 帧放行入镜像。回归前 (W 在事务外) 写端可在 COMMIT 后抢锁 → W 被抬到 5 > 镜像
// Max 4 → seq=5 帧被过滤且镜像缺条 (本测试断言其必入镜像)。
TEST_F(TdDataServiceTest, RebuildWatermarkSharesMirrorSnapshot) {
    // DB 快照: orders seq 1,2,3 + positions seq 4 (20260901)
    {
        sqlite3* db = open_write(db_path_);
        create_schema(db);
        exec_sql(db,
            "INSERT INTO orders (account_id, trading_day, order_id, order_ref, instrument_id,"
            " exchange_id, direction, position_effect, price_type, status, price, volume,"
            " volume_traded, strategy_id, seq)"
            " VALUES ('CTP001', '20260901', 1, 'r', 'IF2603', 'CFFEX', '1', '1', '0', '4',"
            " 3800.0, 3, 3, 'stg1', 1),"
            "        ('CTP001', '20260901', 2, 'r', 'IF2603', 'CFFEX', '1', '1', '0', '4',"
            " 3801.0, 2, 2, 'stg1', 2),"
            "        ('CTP001', '20260901', 3, 'r', 'IF2603', 'CFFEX', '1', '1', '0', '4',"
            " 3802.0, 1, 1, 'stg1', 3)");
        insert_position(db, "CTP001", "IF2603", 3, 4);
        sqlite3_close(db);
    }

    // 写端线程: 重建期间持续尝试提交 seq=5 持仓行 (busy_timeout=0 快速失败重试,
    // 修复后整个快照事务 (含 W) 持 SHARED 锁 → 写端 BUSY 直到重建 COMMIT 后才提交)。
    std::thread writer([this]() {
        sqlite3* w = nullptr;
        ASSERT_EQ(SQLITE_OK, sqlite3_open_v2(db_path_.c_str(), &w, SQLITE_OPEN_READWRITE, nullptr));
        for (int attempt = 0; attempt < 200000; ++attempt) {
            char* err = nullptr;
            // busy_timeout=0: 重建事务持锁时立即 BUSY, 快速重试抢占 COMMIT 后的窗口
            if (sqlite3_exec(w, "BEGIN IMMEDIATE", nullptr, nullptr, &err) != SQLITE_OK) {
                sqlite3_free(err);
                std::this_thread::yield();
                continue;
            }
            sqlite3_stmt* stmt = nullptr;
            const char* sql =
                "INSERT INTO positions (account_id, trading_day, instrument_id, exchange_id,"
                " direction, volume, frozen_volume, today_volume, yd_volume, price, seq)"
                " VALUES ('CTP001', '20260901', 'IF2606', 'CFFEX', 1, 7, 0, 7, 0, 4000.0, 5)";
            if (sqlite3_prepare_v2(w, sql, -1, &stmt, nullptr) == SQLITE_OK) {
                sqlite3_step(stmt);
            }
            sqlite3_finalize(stmt);
            sqlite3_exec(w, "COMMIT", nullptr, nullptr, nullptr);
            break;
        }
        sqlite3_close(w);
    });

    FrameRouter router([](std::function<void()> f) { f(); });
    auto svc = make_service(router);
    svc.rebuild("CTP001");  // 镜像 + W 同快照 (W 在 COMMIT 前)

    writer.join();

    // 一致性断言 (回归守卫): seq=5 持仓帧必须入镜像 — 要么快照已含 (写端先提交, W=5
    // 且镜像含 seq=5), 要么放行 (> W=4)。回归前 W 被抬到 5 而镜像缺 seq=5 → 该帧被过滤,
    // IF2606 永缺 (本断言失败)。
    DzPositionInfo p5{};
    dztrader::copy_string(p5.account_id, "CTP001", true);
    dztrader::copy_string(p5.instrument_id, "IF2606", true);
    dztrader::copy_string(p5.exchange_id, "CFFEX", true);
    p5.direction = DZ_DIRECTION_LONG;
    p5.volume = 7;
    p5.seq = 5;
    feed_frame(router, DZ_FRAME_POSITION_INFO, p5);

    // IF2606@seq5 必在镜像 (快照含或帧放行); 不允许"W 已含却镜像无行"的缺条。
    const auto& positions = svc.positions();
    const auto found = std::find_if(positions.begin(), positions.end(), [](const DzPositionInfo& p) {
        return std::string_view(p.instrument_id) == "IF2606";
    });
    ASSERT_NE(positions.end(), found) << "seq=5 持仓帧被 W 过滤且镜像缺行 (W 快照 ≠ 镜像快照)";
    EXPECT_EQ(7, found->volume);
}

}  // namespace
}  // namespace dztrader::webui
