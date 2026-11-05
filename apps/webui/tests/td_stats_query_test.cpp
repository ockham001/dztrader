/**
 * @file td_stats_query_test.cpp
 * @brief `TdStatsQuery::recompute` 只读重算（契约 rest §2.9.1/§2.9.5/§2.9.8 +
 *        td-data-sync §1/§2/§5）。
 *
 * 真 schema 建库（`tdstore::ensure_trade_db`，不手抄 DDL）：列名/列序漂移必须变红。
 * 断言外部可观察行为：快照 4 值、活动委托集合（只含未成交）、每账户水位、空/降级语义。
 *
 * **total_orders 的独立 oracle**：直接与 `TdOrdersQuery` 首页条数对比（真库同一作用域），
 * 不用被测实现自我推导（契约 rest §2.9.5「与未过滤的委托列表总行数一致」）。
 */

#include "td_list_query.h"
#include "td_orders_query.h"
#include "td_stats_query.h"
#include "td_order_row_writer.h"
#include "td_position_row_writer.h"
#include "test_temp_dir.h"

#include <dztrader/data_type.h>
#include <dztrader/tdstore/trade_db.h>
#include <gtest/gtest.h>
#include <sqlite3.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace dztrader::webui {
namespace {

/// 一条委托的构造参数：列清单来自共享夹具（`td_order_row_writer.h`），默认账户 ACC1。
/// 用别名而非派生结构，保留聚合语义——用例大量使用 designated initializer。
using OrderSeed = dztrader::test::TdOrderRowSeed;

/// 其他三张表的最小可写行（水位 = 四表 MAX(seq)）。
struct SeqRow {
    std::string account_id;
    std::string date = "20261009";
    uint64_t seq = 0;
};

int exec_sql(sqlite3* db, const char* sql) {
    char* err = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        const std::string message = err != nullptr ? err : sqlite3_errmsg(db);
        sqlite3_free(err);
        ADD_FAILURE() << "sqlite3_exec failed: " << message << " | sql=" << sql;
    }
    return rc;
}

class TdStatsQueryTest : public ::testing::Test {
protected:
    std::filesystem::path dir_;
    std::string db_path_;

    void SetUp() override {
        dir_ = dztrader::test::unique_temp_dir("dz_webui_td_stats_query_test");
        std::filesystem::create_directories(dir_);
        db_path_ = (dir_ / "trade.db").string();
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    /// 真 schema 建库（列名/键/索引与生产同源）。
    void create_db() { (void)dztrader::tdstore::ensure_trade_db(db_path_); }

    sqlite3* open() {
        sqlite3* db = nullptr;
        EXPECT_EQ(SQLITE_OK, sqlite3_open(db_path_.c_str(), &db));
        return db;
    }

    /// 写一行委托（共享夹具 `td_order_row_writer.h` 是列清单的唯一实现）。
    void insert_order(const OrderSeed& seed) {
        const std::string error = dztrader::test::insert_td_order_row(db_path_, seed);
        ASSERT_TRUE(error.empty()) << error;
    }

    void insert_trade(const SeqRow& row, const std::string& trade_id) {
        sqlite3* db = open();
        sqlite3_stmt* stmt = nullptr;
        const char* sql =
            "INSERT INTO trades (account_id, date, trade_id, order_id, strategy_id,"
            " instrument_id, exchange_id, direction, position_effect, price, volume, time,"
            " seq, gateway, ext)"
            " VALUES (?,?,?,1,'stg1','rb2601','SHFE',1,1,3500.0,1,32400,?,'dztd_ctp','{}')";
        ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr));
        sqlite3_bind_text(stmt, 1, row.account_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, row.date.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, trade_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 4, static_cast<int64_t>(row.seq));
        EXPECT_EQ(SQLITE_DONE, sqlite3_step(stmt));
        sqlite3_finalize(stmt);
        sqlite3_close(db);
    }

    void insert_position(const SeqRow& row) {
        // 列清单走共享夹具（`td_position_row_writer.h`）：positions 有 13 列，
        // 各测试手抄一份会让 schema 加列时只让抄得最晚的那份失败（unit-test-standard §5.4）。
        dztrader::test::TdPositionRowSeed seed;
        seed.account_id = row.account_id;
        seed.date = row.date;
        seed.instrument_id = "rb2601";
        seed.exchange_id = "SHFE";
        seed.seq = row.seq;
        const std::string error = dztrader::test::insert_td_position_row(db_path_, seed);
        ASSERT_TRUE(error.empty()) << error;
    }

    void insert_trading_account(const SeqRow& row) {
        sqlite3* db = open();
        sqlite3_stmt* stmt = nullptr;
        const char* sql =
            "INSERT INTO trading_accounts (account_id, date, balance, available, frozen,"
            " commission, margin, withdraw_quota, deposit, withdraw, seq, gateway, ext)"
            " VALUES (?,?,0,0,0,0,0,0,0,0,?,'dztd_ctp','{}')";
        ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr));
        sqlite3_bind_text(stmt, 1, row.account_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, row.date.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 3, static_cast<int64_t>(row.seq));
        EXPECT_EQ(SQLITE_DONE, sqlite3_step(stmt));
        sqlite3_finalize(stmt);
        sqlite3_close(db);
    }

    TdStatsQuery query() {
        return TdStatsQuery([this]() -> std::string { return db_path_; }, kTdReadBusyTimeoutMs);
    }

    std::optional<TdStatsSnapshot> recompute(
        std::vector<std::string> online = {"ACC1", "ACC2"},
        std::optional<std::string> day = std::string("20261009")) {
        return query().recompute(online, day);
    }

    /// total_orders 的独立 oracle（契约 rest §2.9.5）：同一作用域下委托列表首页条数。
    /// 用足够大的 limit 取全量，直接数 `items`。
    size_t order_list_row_count(const std::vector<std::string>& online,
                                const std::optional<std::string>& day) {
        TdOrdersQuery orders([this]() -> std::string { return db_path_; }, kTdReadBusyTimeoutMs);
        auto params = TdOrdersQuery::parse([](std::string_view) { return std::string{}; });
        EXPECT_TRUE(params.has_value());
        params->limit = kTdListMaxLimit;
        const auto body = orders.execute(*params, online, day);
        EXPECT_TRUE(body.has_value());
        EXPECT_TRUE((*body)["available"].get<bool>());
        return (*body)["items"].size();
    }

    static void expect_values(const TdStatsValues& values,
                              int64_t total_orders,
                              int64_t total_volume,
                              int64_t unfilled_orders,
                              int64_t unfilled_volume) {
        EXPECT_EQ(values.total_orders, total_orders);
        EXPECT_EQ(values.total_volume, total_volume);
        EXPECT_EQ(values.unfilled_orders, unfilled_orders);
        EXPECT_EQ(values.unfilled_volume, unfilled_volume);
    }
};

// ===== 三场景口径（契约 rest §2.9.5）=====

TEST_F(TdStatsQueryTest, AllTradedScenarioCountsTowardTotalOnly) {
    create_db();
    OrderSeed seed;
    seed.order_id = 1;
    seed.status = DZ_ORDER_ALL_TRADED;
    seed.volume = 3;
    seed.volume_traded = 3;
    insert_order(seed);

    const auto snapshot = recompute();
    ASSERT_TRUE(snapshot.has_value());
    // total = 1 笔 / 3 手；全成 → 未成交 0。
    expect_values(snapshot->values, 1, 3, 0, 0);
    EXPECT_TRUE(snapshot->active.empty());
}

TEST_F(TdStatsQueryTest, PartTradedThenCancelledScenarioCountsTotalAndNoUnfilled) {
    create_db();
    OrderSeed seed;
    seed.order_id = 1;
    seed.status = DZ_ORDER_CANCELLED;
    seed.volume = 5;
    seed.volume_traded = 2;  // 部分成交后撤单：仍计入 total，未成交退出集合
    insert_order(seed);

    const auto snapshot = recompute();
    ASSERT_TRUE(snapshot.has_value());
    expect_values(snapshot->values, 1, 5, 0, 0);
    EXPECT_TRUE(snapshot->active.empty());
}

TEST_F(TdStatsQueryTest, RejectedAndCancelledOrdersCountTowardTotal) {
    create_db();
    OrderSeed rejected;
    rejected.order_id = 1;
    rejected.status = DZ_ORDER_REJECTED;
    rejected.volume = 4;
    insert_order(rejected);
    OrderSeed cancelled;
    cancelled.order_id = 2;
    cancelled.status = DZ_ORDER_CANCELLED;
    cancelled.volume = 2;
    insert_order(cancelled);
    OrderSeed unfilled;
    unfilled.order_id = 3;
    unfilled.status = DZ_ORDER_NOT_TRADED;
    unfilled.volume = 6;
    unfilled.volume_traded = 0;
    insert_order(unfilled);

    const auto snapshot = recompute();
    ASSERT_TRUE(snapshot.has_value());
    // total = 3 笔 / 12 手（含废单、撤单）；未成交 = 1 笔 / 6 手。
    expect_values(snapshot->values, 3, 12, 1, 6);
    ASSERT_EQ(snapshot->active.size(), 1U);
    EXPECT_EQ(snapshot->active[0].order_id, 3);
}

TEST_F(TdStatsQueryTest, UnfilledVolumeIsRemainingVolumeAcrossMixedStates) {
    create_db();
    OrderSeed submitting;
    submitting.order_id = 1;
    submitting.status = DZ_ORDER_SUBMITTING;
    submitting.volume = 2;
    insert_order(submitting);
    OrderSeed not_traded;
    not_traded.order_id = 2;
    not_traded.status = DZ_ORDER_NOT_TRADED;
    not_traded.volume = 5;
    insert_order(not_traded);
    OrderSeed part;
    part.order_id = 3;
    part.status = DZ_ORDER_PART_TRADED;
    part.volume = 8;
    part.volume_traded = 3;
    insert_order(part);

    const auto snapshot = recompute();
    ASSERT_TRUE(snapshot.has_value());
    // total = 3 笔 / 15 手；未成交 = 3 笔 / (2-0 + 5-0 + 8-3) = 12 手。
    expect_values(snapshot->values, 3, 15, 3, 12);
}

// ===== total_orders == 委托列表行数（独立 oracle，契约 rest §2.9.5）=====

TEST_F(TdStatsQueryTest, TotalOrdersEqualsOrderListRowCountForSameScope) {
    create_db();
    // 六笔覆盖全部状态 + 两个账户 + 一笔非当前交易日（不应计入）。
    const std::vector<int> statuses = {DZ_ORDER_SUBMITTING,  DZ_ORDER_NOT_TRADED,
                                       DZ_ORDER_PART_TRADED, DZ_ORDER_ALL_TRADED,
                                       DZ_ORDER_CANCELLED,   DZ_ORDER_REJECTED};
    int64_t order_id = 1;
    for (const int status : statuses) {
        OrderSeed seed;
        seed.account_id = (order_id % 2 == 0) ? "ACC2" : "ACC1";
        seed.order_id = order_id;
        seed.status = status;
        seed.volume = static_cast<int>(order_id) + 1;
        seed.volume_traded = status == DZ_ORDER_PART_TRADED ? 1 : 0;
        insert_order(seed);
        ++order_id;
    }
    OrderSeed other_day;
    other_day.account_id = "ACC1";
    other_day.order_id = 100;
    other_day.date = "20261008";
    insert_order(other_day);
    OrderSeed offline_account;
    offline_account.account_id = "ACC_OFFLINE";
    offline_account.order_id = 200;
    insert_order(offline_account);

    const std::vector<std::string> online = {"ACC1", "ACC2"};
    const auto snapshot = recompute(online, std::string("20261009"));
    ASSERT_TRUE(snapshot.has_value());

    const size_t list_rows = order_list_row_count(online, std::string("20261009"));
    EXPECT_EQ(static_cast<size_t>(snapshot->values.total_orders), list_rows);
    EXPECT_EQ(list_rows, 6U);

    // 手算：volume = 2+3+4+5+6+7 = 27。
    EXPECT_EQ(snapshot->values.total_volume, 27);
    // 未成交 = Submitting(2) + NotTraded(3) + PartTraded(4-1=3) = 3 笔 / 8 手。
    EXPECT_EQ(snapshot->values.unfilled_orders, 3);
    EXPECT_EQ(snapshot->values.unfilled_volume, 8);
}

TEST_F(TdStatsQueryTest, TotalOrdersMatchesListCountWithAccountsScopeOfOnlineOnly) {
    create_db();
    for (int i = 0; i < 4; ++i) {
        OrderSeed seed;
        seed.account_id = "ACC1";
        seed.order_id = 1 + i;
        seed.volume = 1;
        insert_order(seed);
    }
    OrderSeed acc2;
    acc2.account_id = "ACC2";
    acc2.order_id = 10;
    acc2.volume = 2;
    insert_order(acc2);

    const auto snapshot = recompute({"ACC1"}, std::string("20261009"));
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_EQ(snapshot->values.total_orders,
              static_cast<int64_t>(order_list_row_count({"ACC1"}, std::string("20261009"))));
    // 手算 ACC1 四笔 × 1 手。
    expect_values(snapshot->values, 4, 4, 4, 4);
}

// ===== 活动委托种子 =====

TEST_F(TdStatsQueryTest, ActiveSeedsCarryOnlyUnfilledStatuses) {
    create_db();
    OrderSeed part;
    part.account_id = "ACC2";
    part.order_id = 9;
    part.status = DZ_ORDER_PART_TRADED;
    part.volume = 7;
    part.volume_traded = 2;
    insert_order(part);
    OrderSeed cancelled;
    cancelled.order_id = 10;
    cancelled.status = DZ_ORDER_CANCELLED;
    insert_order(cancelled);

    const auto snapshot = recompute();
    ASSERT_TRUE(snapshot.has_value());
    ASSERT_EQ(snapshot->active.size(), 1U);
    const TdStatsActiveOrder& seed = snapshot->active[0];
    EXPECT_EQ(seed.account_id, "ACC2");
    EXPECT_EQ(seed.order_id, 9);
    EXPECT_EQ(seed.status, DZ_ORDER_PART_TRADED);
    EXPECT_EQ(seed.volume, 7);
    EXPECT_EQ(seed.volume_traded, 2);
}

// ===== 水位 = 四表 MAX(seq)（契约 td-data-sync §1/§2/§5）=====

TEST_F(TdStatsQueryTest, WatermarkIsMaxSeqAcrossFourTablesPerAccount) {
    create_db();
    // ACC1：orders seq 5、trades seq 9、positions seq 7、trading_accounts seq 6 → 9
    insert_order(OrderSeed{.order_id = 1, .seq = 5});
    insert_trade(SeqRow{.account_id = "ACC1", .seq = 9}, "T1");
    insert_position(SeqRow{.account_id = "ACC1", .seq = 7});
    insert_trading_account(SeqRow{.account_id = "ACC1", .seq = 6});
    // ACC2：仅 trades seq 3 → 3
    insert_trade(SeqRow{.account_id = "ACC2", .seq = 3}, "T2");

    const auto snapshot = recompute();
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_EQ(snapshot->watermarks.at("ACC1"), 9U);
    EXPECT_EQ(snapshot->watermarks.at("ACC2"), 3U);
}

TEST_F(TdStatsQueryTest, WatermarkIgnoresDateFilterAndAccumulatesAcrossDays) {
    create_db();
    // 旧交易日的行：水位是账户级跨日累积（不带 date 过滤），必须取到。
    OrderSeed old_day;
    old_day.order_id = 1;
    old_day.date = "20261008";
    old_day.seq = 42;
    insert_order(old_day);

    const auto snapshot = recompute();
    ASSERT_TRUE(snapshot.has_value());
    // 旧日委托不计入 aggregate（date = 当前交易日）。
    expect_values(snapshot->values, 0, 0, 0, 0);
    // 但水位取到旧日的 MAX(seq)。
    EXPECT_EQ(snapshot->watermarks.at("ACC1"), 42U);
}

TEST_F(TdStatsQueryTest, WatermarkMissingAccountIsAbsent) {
    create_db();
    insert_order(OrderSeed{.account_id = "ACC1", .order_id = 1, .seq = 4});

    const auto snapshot = recompute({"ACC1", "ACC2"}, std::string("20261009"));
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_EQ(snapshot->watermarks.at("ACC1"), 4U);
    EXPECT_EQ(snapshot->watermarks.count("ACC2"), 0U);
}

// ===== 空语义（契约 rest §2.9.1/§2.9.5）=====

TEST_F(TdStatsQueryTest, NoOnlineAccountsYieldsZeroSnapshotWithoutReadingDb) {
    // 不建库：无在线账户 → 全 0 快照（不读库，因此不可能是降级）。
    const auto snapshot = recompute(/*online=*/{}, /*day=*/std::nullopt);
    ASSERT_TRUE(snapshot.has_value());
    expect_values(snapshot->values, 0, 0, 0, 0);
    EXPECT_TRUE(snapshot->active.empty());
    EXPECT_TRUE(snapshot->watermarks.empty());
}

TEST_F(TdStatsQueryTest, NoTradingDayYieldsZeroSnapshot) {
    create_db();
    insert_order(OrderSeed{});
    const auto snapshot = recompute({"ACC1"}, /*day=*/std::nullopt);
    ASSERT_TRUE(snapshot.has_value());
    expect_values(snapshot->values, 0, 0, 0, 0);
    EXPECT_TRUE(snapshot->watermarks.empty());
}

TEST_F(TdStatsQueryTest, EmptyDatabaseForOnlineAccountsYieldsZeroValuesButWatermarks) {
    create_db();
    insert_order(OrderSeed{.order_id = 1, .seq = 3});
    const auto snapshot = recompute({"ACC1", "ACC2"}, std::string("20261010"));
    ASSERT_TRUE(snapshot.has_value());
    // 当日无行 → 0 值；水位仍取账户级 MAX(seq)。
    expect_values(snapshot->values, 0, 0, 0, 0);
    EXPECT_EQ(snapshot->watermarks.at("ACC1"), 3U);
}

TEST_F(TdStatsQueryTest, AggregateUsesSqlNotIntegerOverflowOfSmallVolumes) {
    create_db();
    // 大 volume 求和落在 int64（SQLite SUM 走 64 位整数），不因 int32 求和溢出。
    for (int i = 0; i < 3; ++i) {
        OrderSeed seed;
        seed.order_id = 1 + i;
        seed.volume = 1000000000;  // 3 × 1e9 > INT32_MAX
        insert_order(seed);
    }
    const auto snapshot = recompute();
    ASSERT_TRUE(snapshot.has_value());
    expect_values(snapshot->values, 3, 3000000000LL, 3, 3000000000LL);
}

// ===== 降级（契约 rest §2.9.8 / §2.9.5：nullopt = 保持旧计数器）=====

TEST_F(TdStatsQueryTest, MissingDatabaseYieldsNullopt) { EXPECT_FALSE(recompute().has_value()); }

TEST_F(TdStatsQueryTest, EmptyPathProviderYieldsNullopt) {
    TdStatsQuery not_found([]() -> std::string { return std::string{}; });
    EXPECT_FALSE(not_found.recompute({"ACC1"}, std::string("20261009")).has_value());
}

TEST_F(TdStatsQueryTest, SchemaVersionMismatchYieldsNullopt) {
    create_db();
    {
        sqlite3* db = open();
        EXPECT_EQ(SQLITE_OK, exec_sql(db, "PRAGMA user_version=999"));
        sqlite3_close(db);
    }
    EXPECT_FALSE(recompute().has_value());
}

TEST_F(TdStatsQueryTest, NonSqliteFileYieldsNullopt) {
    // 非 SQLite 内容：`sqlite3_open` 惰性成功，schema 探测首次读页失败 → open_failed。
    {
        std::FILE* file = std::fopen(db_path_.c_str(), "wb");
        ASSERT_NE(file, nullptr);
        const char* garbage = "this is definitely not a sqlite database file";
        ASSERT_GT(std::fwrite(garbage, 1, std::strlen(garbage), file), 0U);
        std::fclose(file);
    }
    EXPECT_FALSE(recompute().has_value());
}

TEST_F(TdStatsQueryTest, MissingOrdersTableYieldsNullopt) {
    create_db();
    {
        sqlite3* db = open();
        EXPECT_EQ(SQLITE_OK, exec_sql(db, "DROP TABLE orders"));
        sqlite3_close(db);
    }
    // schema 版本仍匹配，但查询执行失败 → 保持旧计数器（契约 rest §2.9.5）。
    EXPECT_FALSE(recompute().has_value());
}

TEST_F(TdStatsQueryTest, ReadLockContentionYieldsNullopt) {
    create_db();
    // WAL 下读写不冲突（生产库形态）；回滚日志模式下用另一连接持 EXCLUSIVE 才能制造
    // 读锁竞争 → 200ms busy_timeout 内拿不到读锁。
    {
        sqlite3* setup = open();
        EXPECT_EQ(SQLITE_OK, exec_sql(setup, "PRAGMA journal_mode=DELETE"));
        sqlite3_close(setup);
    }
    sqlite3* locker = nullptr;
    ASSERT_EQ(SQLITE_OK,
              sqlite3_open_v2(db_path_.c_str(), &locker, SQLITE_OPEN_READWRITE, nullptr));
    ASSERT_EQ(SQLITE_OK, exec_sql(locker, "BEGIN EXCLUSIVE"));

    EXPECT_FALSE(recompute().has_value());

    EXPECT_EQ(SQLITE_OK, exec_sql(locker, "ROLLBACK"));
    sqlite3_close(locker);
}

}  // namespace
}  // namespace dztrader::webui
