/**
 * @file td_domain_service_test.cpp
 * @brief TD 数据域服务（契约 td-data-sync §8 / webui-ws §2.3/§2.5 / rest
 *        §2.9.1/§2.9.3/§2.9.5/§2.9.6）。
 *
 * 断言外部可观察行为：WS 广播（type/instance_id/data）、`td_recent` 载荷、`data_changed`
 * 广播时机、在线集合与当前交易日、统计计数器（`stats_body()` 的公开 JSON）。不断言私有
 * 成员、不读内部容器。
 *
 * 库形态用**真 schema**（`tdstore::ensure_trade_db`）：精度派生依赖
 * `instruments.price_tick` 列名，手抄 DDL 会让 schema 漂移静默通过。
 *
 * 时序一律用注入调度器 / 直接调用 `reconcile_stats()` 触发，不 sleep（设计 §2.6）。
 */

#include "td_domain_service.h"
#include "td_stats_query.h"
#include "test_temp_dir.h"
#include "td_order_row_writer.h"

#include <dztrader/date_time/date.h>
#include <dztrader/shm/frame_view.h>
#include <dztrader/struct.h>
#include <dztrader/tdstore/trade_db.h>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace dztrader::webui {
namespace {

/// 记录 WS 广播（契约 webui-ws §2.2：领域消息用 data 键，instance_id 为空时不携带）。
class FakeBroadcaster : public WsBroadcaster {
public:
    struct Msg {
        std::string type;
        std::string instance_id;
        nlohmann::json data;
    };
    std::vector<Msg> messages;

    void broadcast(const std::string& type,
                   const std::string& instance_id,
                   const nlohmann::json& data) override {
        messages.push_back({.type = type, .instance_id = instance_id, .data = data});
    }
};

/// 记录 `data_changed` 广播的 scope。
class FakeNotifier : public DataChangeNotifier {
public:
    std::vector<std::string> scopes;

    void broadcast_data_changed(const std::string& scope) override { scopes.push_back(scope); }
    void kick_user(const std::string&) override {}
};

class TdDomainServiceTest : public ::testing::Test {
protected:
    std::filesystem::path dir_;
    FrameRouter router_{[](std::function<void()> f) { f(); }};  // 同步 poster
    FakeBroadcaster ws_;
    FakeNotifier notifier_;
    std::unique_ptr<TdDomainService> service_;

    void SetUp() override {
        dir_ = dztrader::test::unique_temp_dir("dz_webui_td_domain_service_test");
        std::filesystem::create_directories(dir_);
        service_ = std::make_unique<TdDomainService>(router_, ws_, notifier_);
    }

    void TearDown() override {
        service_.reset();
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    void set_db_path(const std::string& path) {
        service_->set_trade_db_path_provider([path]() { return path; });
    }

    /// 构造一帧 struct payload 交给 router 分发（register_raw 语义：监听线程同步解析，
    /// 拷贝字段后投递 IO 线程 — 测试用同步 poster 等价执行）。
    template <typename Payload>
    static void feed_frame(FrameRouter& router, DzFrameType type, const Payload& payload) {
        alignas(8) std::byte frame[sizeof(DzFrameHeader) + sizeof(Payload)];
        auto* header = reinterpret_cast<DzFrameHeader*>(frame);
        header->frame_size = static_cast<uint32_t>(sizeof(DzFrameHeader) + sizeof(Payload));
        header->frame_type = type;
        std::memcpy(frame + sizeof(DzFrameHeader), &payload, sizeof(Payload));
        router.dispatch(shm::FrameView(frame));
    }

    static int32_t day_epoch(int year, int month, int day) {
        return dztrader::Date::from_year_month_day(year, month, day).days_since_epoch();
    }

    void ready(const std::string& account, int32_t date) {
        DzAccountStatus status{};
        dztrader::copy_string(status.account_id, account.c_str(), true);
        status.state = DZ_ACCOUNT_READY;
        status.date = date;
        feed_frame(router_, DZ_FRAME_ACCOUNT_STATUS, status);
    }

    void offline(const std::string& account) {
        DzAccountStatus status{};
        dztrader::copy_string(status.account_id, account.c_str(), true);
        status.state = DZ_ACCOUNT_OFFLINE;
        status.date = 0;
        feed_frame(router_, DZ_FRAME_ACCOUNT_STATUS, status);
    }

    void logging_in(const std::string& account) {
        DzAccountStatus status{};
        dztrader::copy_string(status.account_id, account.c_str(), true);
        status.state = DZ_ACCOUNT_LOGGING_IN;
        status.date = 0;
        feed_frame(router_, DZ_FRAME_ACCOUNT_STATUS, status);
    }

    static DzOrderReport make_order(const std::string& account,
                                    int64_t order_id,
                                    int32_t date,
                                    int32_t time,
                                    uint64_t seq,
                                    const std::string& instrument = "rb2601") {
        DzOrderReport report{};
        dztrader::copy_string(report.account_id, account.c_str(), true);
        dztrader::copy_string(report.instrument_id, instrument.c_str(), true);
        dztrader::copy_string(report.exchange_id, "SHFE", true);
        dztrader::copy_string(report.strategy_id, "stg1", true);
        report.order_id = order_id;
        report.direction = DZ_DIRECTION_LONG;
        report.position_effect = DZ_POSITION_EFFECT_OPEN;
        report.price_type = DZ_PRICE_LIMIT;
        report.status = DZ_ORDER_NOT_TRADED;
        report.price = 3500.0;
        report.volume = 1;
        report.volume_traded = 0;
        report.date = date;
        report.time = time;
        report.seq = seq;
        return report;
    }

    void feed_order(const DzOrderReport& report) {
        feed_frame(router_, DZ_FRAME_ORDER_REPORT, report);
    }

    static DzTradeReport make_trade(const std::string& account,
                                    const std::string& trade_id,
                                    int64_t order_id,
                                    int32_t date,
                                    int32_t time,
                                    uint64_t seq,
                                    const std::string& instrument = "rb2601") {
        DzTradeReport report{};
        dztrader::copy_string(report.account_id, account.c_str(), true);
        dztrader::copy_string(report.trade_id, trade_id.c_str(), true);
        dztrader::copy_string(report.instrument_id, instrument.c_str(), true);
        dztrader::copy_string(report.exchange_id, "SHFE", true);
        dztrader::copy_string(report.strategy_id, "stg1", true);
        report.order_id = order_id;
        report.direction = DZ_DIRECTION_LONG;
        report.position_effect = DZ_POSITION_EFFECT_OPEN;
        report.price = 3512.3;
        report.volume = 1;
        report.date = date;
        report.time = time;
        report.seq = seq;
        return report;
    }

    void feed_trade(const DzTradeReport& report) {
        feed_frame(router_, DZ_FRAME_TRADE_REPORT, report);
    }

    static DzPositionInfo make_position(const std::string& account,
                                        const std::string& instrument,
                                        int32_t date,
                                        uint64_t seq) {
        DzPositionInfo info{};
        dztrader::copy_string(info.account_id, account.c_str(), true);
        dztrader::copy_string(info.instrument_id, instrument.c_str(), true);
        dztrader::copy_string(info.exchange_id, "SHFE", true);
        info.direction = DZ_DIRECTION_LONG;
        info.volume = 2;
        info.frozen_volume = 1;
        info.today_volume = 1;
        info.yd_volume = 1;
        info.price = 3512.35;
        info.date = date;
        info.seq = seq;
        return info;
    }

    void feed_position(const DzPositionInfo& info) {
        feed_frame(router_, DZ_FRAME_POSITION_INFO, info);
    }

    static DzTradingAccount make_trading_account(const std::string& account,
                                                 int32_t date,
                                                 uint64_t seq) {
        DzTradingAccount fund{};
        dztrader::copy_string(fund.account_id, account.c_str(), true);
        fund.balance = 1000000.0;
        fund.available = 850000.5;
        fund.frozen = 12000.25;
        fund.commission = 345.75;
        fund.margin = 150000.0;
        fund.withdraw_quota = 800000.0;
        fund.deposit = 50000.0;
        fund.withdraw = 300.5;
        fund.date = date;
        fund.seq = seq;
        return fund;
    }

    void feed_trading_account(const DzTradingAccount& fund) {
        feed_frame(router_, DZ_FRAME_TRADING_ACCOUNT, fund);
    }

    /// 建真 schema 库并写入一条合约 price_tick。
    std::string create_db_with_tick(const std::string& instrument_id, double price_tick) {
        const std::string path = (dir_ / "trade.db").string();
        (void)dztrader::tdstore::ensure_trade_db(path);
        sqlite3* db = nullptr;
        EXPECT_EQ(SQLITE_OK, sqlite3_open(path.c_str(), &db));
        sqlite3_stmt* stmt = nullptr;
        EXPECT_EQ(SQLITE_OK,
                  sqlite3_prepare_v2(db,
                                     "INSERT INTO instruments (instrument_id, exchange_id,"
                                     " price_tick) VALUES (?, 'SHFE', ?)",
                                     -1, &stmt, nullptr));
        sqlite3_bind_text(stmt, 1, instrument_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(stmt, 2, price_tick);
        EXPECT_EQ(SQLITE_DONE, sqlite3_step(stmt));
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        return path;
    }

    const FakeBroadcaster::Msg* find_broadcast(const std::string& type) const {
        for (const auto& message : ws_.messages) {
            if (message.type == type) {
                return &message;
            }
        }
        return nullptr;
    }

    // ===== 统计计数器夹具（契约 rest §2.9.5）=====

    /// 建真 schema 库并把路径同时接到域服务的 tick 查询与统计重算查询上。
    std::string create_stats_db() {
        const std::string path = (dir_ / "trade.db").string();
        (void)dztrader::tdstore::ensure_trade_db(path);
        set_db_path(path);
        return path;
    }

    /// 注入统计重算查询（注入即触发一次重算，契约 rest §2.9.5「启动」）。
    void inject_stats_query(const std::string& path) {
        auto query = std::make_shared<TdStatsQuery>([path]() -> std::string { return path; },
                                                    kTdReadBusyTimeoutMs);
        service_->set_stats_query(std::move(query));
    }

    /// 直接写一条委托行（真 schema；列清单来自共享夹具 `td_order_row_writer.h`）。
    void insert_order_row(const std::string& account_id,
                          const std::string& date,
                          int64_t order_id,
                          int status,
                          int volume,
                          int volume_traded,
                          uint64_t seq) {
        dztrader::test::TdOrderRowSeed seed;
        seed.account_id = account_id;
        seed.date = date;
        seed.order_id = order_id;
        seed.status = status;
        seed.volume = volume;
        seed.volume_traded = volume_traded;
        seed.seq = seq;
        const std::string error =
            dztrader::test::insert_td_order_row((dir_ / "trade.db").string(), seed);
        ASSERT_TRUE(error.empty()) << error;
    }

    /// `stats_body()` 的 4 值（外部可观察 JSON）。
    struct StatsValues {
        int64_t total_orders = 0;
        int64_t total_volume = 0;
        int64_t unfilled_orders = 0;
        int64_t unfilled_volume = 0;
    };

    StatsValues stats_values() const {
        const nlohmann::json body = service_->stats_body();
        StatsValues values;
        if (body.contains("stats")) {
            values.total_orders = body["stats"]["total_orders"].get<int64_t>();
            values.total_volume = body["stats"]["total_volume"].get<int64_t>();
            values.unfilled_orders = body["stats"]["unfilled_orders"].get<int64_t>();
            values.unfilled_volume = body["stats"]["unfilled_volume"].get<int64_t>();
        }
        return values;
    }

    void expect_stats(int64_t total_orders,
                      int64_t total_volume,
                      int64_t unfilled_orders,
                      int64_t unfilled_volume) const {
        const StatsValues values = stats_values();
        EXPECT_EQ(values.total_orders, total_orders);
        EXPECT_EQ(values.total_volume, total_volume);
        EXPECT_EQ(values.unfilled_orders, unfilled_orders);
        EXPECT_EQ(values.unfilled_volume, unfilled_volume);
    }

    /// 一条 ORDER_REPORT（状态/量可指定）。
    static DzOrderReport make_order_with(int64_t order_id,
                                         int status,
                                         int volume,
                                         int volume_traded,
                                         uint64_t seq,
                                         int32_t date,
                                         const std::string& account = "ACC1") {
        DzOrderReport report = make_order(account, order_id, date, 32400, seq);
        report.status = static_cast<DzOrderStatus>(status);
        report.volume = volume;
        report.volume_traded = volume_traded;
        return report;
    }
};

// ===== 在线集合与当前交易日（契约 rest §2.9.1）=====

TEST_F(TdDomainServiceTest, ReadyAccountJoinsOnlineSetAndCurrentTradingDay) {
    ready("ACC1", day_epoch(2026, 10, 9));
    EXPECT_EQ(service_->online_accounts(), std::vector<std::string>{"ACC1"});
    ASSERT_TRUE(service_->current_trading_day().has_value());
    EXPECT_EQ(*service_->current_trading_day(), "20261009");
}

TEST_F(TdDomainServiceTest, OfflineRemovesAccountAndClearsTradingDay) {
    ready("ACC1", day_epoch(2026, 10, 9));
    offline("ACC1");
    EXPECT_TRUE(service_->online_accounts().empty());
    EXPECT_FALSE(service_->current_trading_day().has_value());
}

TEST_F(TdDomainServiceTest, LoggingInDoesNotJoinOnlineSet) {
    logging_in("ACC1");
    EXPECT_TRUE(service_->online_accounts().empty());
    EXPECT_FALSE(service_->current_trading_day().has_value());
}

TEST_F(TdDomainServiceTest, CurrentTradingDayIsMaxAcrossOnlineAccounts) {
    ready("ACC1", day_epoch(2026, 10, 8));
    ready("ACC2", day_epoch(2026, 10, 9));
    ASSERT_TRUE(service_->current_trading_day().has_value());
    EXPECT_EQ(*service_->current_trading_day(), "20261009");
}

TEST_F(TdDomainServiceTest, OnlineAccountsSortedAscending) {
    ready("ACC2", day_epoch(2026, 10, 9));
    ready("ACC1", day_epoch(2026, 10, 9));
    const auto accounts = service_->online_accounts();
    ASSERT_EQ(accounts.size(), 2U);
    EXPECT_EQ(accounts[0], "ACC1");
    EXPECT_EQ(accounts[1], "ACC2");
}

// ===== data_changed 广播时机（契约 webui-ws §2.3 / td-data-sync §8）=====

TEST_F(TdDomainServiceTest, BroadcastsDataChangedOnReadyFlip) {
    ready("ACC1", day_epoch(2026, 10, 9));
    ASSERT_FALSE(notifier_.scopes.empty());
    EXPECT_EQ(notifier_.scopes.back(), "td");
}

TEST_F(TdDomainServiceTest, BroadcastsDataChangedOnOfflineFlip) {
    ready("ACC1", day_epoch(2026, 10, 9));
    notifier_.scopes.clear();
    offline("ACC1");
    ASSERT_FALSE(notifier_.scopes.empty());
    EXPECT_EQ(notifier_.scopes.back(), "td");
}

TEST_F(TdDomainServiceTest, BroadcastsDataChangedOnTradingDayChange) {
    ready("ACC1", day_epoch(2026, 10, 8));
    notifier_.scopes.clear();
    ready("ACC1", day_epoch(2026, 10, 9));  // 日切（同一账户 date 变化）
    ASSERT_FALSE(notifier_.scopes.empty());
    EXPECT_EQ(notifier_.scopes.back(), "td");
}

TEST_F(TdDomainServiceTest, NoDataChangedWhenOnlineSetAndDayUnchanged) {
    ready("ACC1", day_epoch(2026, 10, 9));
    notifier_.scopes.clear();
    ready("ACC1", day_epoch(2026, 10, 9));  // 重复 Ready，集合与日切均未变化
    EXPECT_TRUE(notifier_.scopes.empty());
}

TEST_F(TdDomainServiceTest, OfflineForUnknownAccountDoesNotBroadcast) {
    offline("ACC_X");
    EXPECT_TRUE(notifier_.scopes.empty());
}

TEST_F(TdDomainServiceTest, LoggingInForOnlineAccountLeavesOnlineSetAndBroadcasts) {
    ready("ACC1", day_epoch(2026, 10, 9));
    notifier_.scopes.clear();
    logging_in("ACC1");  // 契约 account-status：LoggingIn 下线
    EXPECT_TRUE(service_->online_accounts().empty());
    ASSERT_FALSE(notifier_.scopes.empty());
    EXPECT_EQ(notifier_.scopes.back(), "td");
}

// ===== ORDER_REPORT → WS td_order_report（契约 webui-ws §2.5）=====

TEST_F(TdDomainServiceTest, OrderReportForOnlineAccountIsBroadcastAsRowPayload) {
    const std::string db_path = create_db_with_tick("rb2601", 0.5);
    set_db_path(db_path);
    ready("ACC1", day_epoch(2026, 10, 9));
    ws_.messages.clear();

    feed_order(make_order("ACC1", 10231, day_epoch(2026, 10, 9), 32400, 1842));

    const auto* message = find_broadcast("td_order_report");
    ASSERT_NE(message, nullptr);
    // 契约 webui-ws §2.5：payload 即行对象，instance_id 为空
    EXPECT_TRUE(message->instance_id.empty());
    const auto& row = message->data;
    EXPECT_EQ(row["account_id"].get<std::string>(), "ACC1");
    EXPECT_EQ(row["date"].get<std::string>(), "20261009");
    EXPECT_EQ(row["order_id"].get<int64_t>(), 10231);
    EXPECT_EQ(row["direction"].get<std::string>(), "Long");
    EXPECT_EQ(row["position_effect"].get<std::string>(), "Open");
    EXPECT_EQ(row["price_type"].get<std::string>(), "Limit");
    EXPECT_EQ(row["status"].get<std::string>(), "NotTraded");
    EXPECT_EQ(row["time"].get<int>(), 32400);
    EXPECT_TRUE(row["avg_price"].is_null());             // volume_traded = 0
    EXPECT_EQ(row["price_decimals"].get<int>(), 1);      // tick 0.5 → 1
    EXPECT_EQ(row["avg_price_decimals"].get<int>(), 3);  // 1 + 2
    EXPECT_EQ(row["seq"].get<uint64_t>(), 1842U);
    EXPECT_TRUE(row["sort_key"].is_string());
}

TEST_F(TdDomainServiceTest, OrderReportForOfflineAccountIsNotBroadcast) {
    ready("ACC1", day_epoch(2026, 10, 9));
    ws_.messages.clear();
    feed_order(make_order("ACC2", 7, day_epoch(2026, 10, 9), 32400, 1));
    EXPECT_EQ(find_broadcast("td_order_report"), nullptr);
}

TEST_F(TdDomainServiceTest, OrderReportBeforeAnyReadyIsNotBroadcast) {
    ws_.messages.clear();
    feed_order(make_order("ACC1", 7, day_epoch(2026, 10, 9), 32400, 1));
    EXPECT_EQ(find_broadcast("td_order_report"), nullptr);
}

// ===== POSITION_INFO → WS td_position_info（契约 webui-ws §2.2/§2.5、rest §2.9.4）=====

TEST_F(TdDomainServiceTest, PositionInfoForOnlineAccountIsBroadcastAsRowPayload) {
    const std::string db_path = create_db_with_tick("rb2601", 0.5);
    set_db_path(db_path);
    ready("ACC1", day_epoch(2026, 10, 9));
    ws_.messages.clear();

    feed_position(make_position("ACC1", "rb2601", day_epoch(2026, 10, 9), 1844));

    const auto* message = find_broadcast("td_position_info");
    ASSERT_NE(message, nullptr);
    // 契约 webui-ws §2.5：payload 即行对象（与 REST items 同 schema），instance_id 为空。
    EXPECT_TRUE(message->instance_id.empty());
    const auto& row = message->data;
    EXPECT_EQ(row["account_id"].get<std::string>(), "ACC1");
    EXPECT_EQ(row["date"].get<std::string>(), "20261009");
    EXPECT_EQ(row["instrument_id"].get<std::string>(), "rb2601");
    EXPECT_EQ(row["exchange_id"].get<std::string>(), "SHFE");
    EXPECT_EQ(row["direction"].get<std::string>(), "Long");
    EXPECT_EQ(row["volume"].get<int>(), 2);
    EXPECT_EQ(row["frozen_volume"].get<int>(), 1);
    EXPECT_EQ(row["today_volume"].get<int>(), 1);
    EXPECT_EQ(row["yd_volume"].get<int>(), 1);
    EXPECT_DOUBLE_EQ(row["price"].get<double>(), 3512.35);
    EXPECT_EQ(row["price_decimals"].get<int>(), 1);  // tick 0.5 → 1
    EXPECT_EQ(row["seq"].get<uint64_t>(), 1844U);
    // 持仓行没有 sort_key（契约 rest §2.9.4）；也不返回平台内部列。
    EXPECT_FALSE(row.contains("sort_key"));
    EXPECT_FALSE(row.contains("gateway"));
    EXPECT_FALSE(row.contains("ext"));
}

TEST_F(TdDomainServiceTest, PositionInfoForOfflineAccountIsNotBroadcast) {
    ready("ACC1", day_epoch(2026, 10, 9));
    ws_.messages.clear();
    feed_position(make_position("ACC2", "rb2601", day_epoch(2026, 10, 9), 1));
    EXPECT_EQ(find_broadcast("td_position_info"), nullptr);
}

TEST_F(TdDomainServiceTest, PositionInfoBeforeAnyReadyIsNotBroadcast) {
    // 登录查询阶段的持仓帧在 Ready 之前到达：不进补发环也不广播（前端靠 Ready 后的
    // REST 基线拿同一批数据）。契约 td-data-sync §8：只转发在线账户的帧。
    ws_.messages.clear();
    feed_position(make_position("ACC1", "rb2601", day_epoch(2026, 10, 9), 1));
    EXPECT_EQ(find_broadcast("td_position_info"), nullptr);
}

TEST_F(TdDomainServiceTest, PositionInfoJoinsTheSharedRecentRing) {
    // 契约 td-data-sync §8：同一账户环容纳四类交易事件，容量 300 共享。
    ready("ACC1", day_epoch(2026, 10, 9));
    feed_order(make_order("ACC1", 1, day_epoch(2026, 10, 9), 32400, 1));
    feed_trade(make_trade("ACC1", "T001", 1, day_epoch(2026, 10, 9), 32401, 2));
    feed_position(make_position("ACC1", "rb2601", day_epoch(2026, 10, 9), 3));

    const nlohmann::json payload = service_->recent_events();
    const auto& events = payload["events"];
    ASSERT_EQ(events.size(), 3U);
    EXPECT_EQ(events[0]["type"].get<std::string>(), "td_order_report");
    EXPECT_EQ(events[1]["type"].get<std::string>(), "td_trade_report");
    EXPECT_EQ(events[2]["type"].get<std::string>(), "td_position_info");
    EXPECT_EQ(events[2]["data"]["seq"].get<uint64_t>(), 3U);
}

TEST_F(TdDomainServiceTest, PositionInfoPriceDecimalsFallBackWhenDatabaseUnavailable) {
    ready("ACC1", day_epoch(2026, 10, 9));
    ws_.messages.clear();
    DzPositionInfo info = make_position("ACC1", "rb2601", day_epoch(2026, 10, 9), 1);
    info.price = 3512.35;  // 合约查不到 → 用价格小数位兜底（契约 rest §2.9.6）
    feed_position(info);
    const auto* message = find_broadcast("td_position_info");
    ASSERT_NE(message, nullptr);
    EXPECT_EQ(message->data["price_decimals"].get<int>(), 2);
}

TEST_F(TdDomainServiceTest, PriceDecimalsFallBackToPriceDigitsWhenDatabaseUnavailable) {
    // 未设 provider（库不可用）→ 契约 rest §2.9.6 兜底：用该行价格的小数位
    ready("ACC1", day_epoch(2026, 10, 9));
    ws_.messages.clear();
    DzOrderReport report = make_order("ACC1", 1, day_epoch(2026, 10, 9), 32400, 1);
    report.price = 3512.55;  // 2 位
    feed_order(report);
    const auto* message = find_broadcast("td_order_report");
    ASSERT_NE(message, nullptr);
    EXPECT_EQ(message->data["price_decimals"].get<int>(), 2);
    EXPECT_EQ(message->data["avg_price_decimals"].get<int>(), 4);
}

TEST_F(TdDomainServiceTest, PriceTickNegativeCacheIsInvalidatedOnReady) {
    // 库/合约尚未就绪时先收到一帧 → 走价格小数位兜底（负缓存）；
    // Ready（= td 已装载合约）后同一合约必须重新查 tick，而不是永久停在兜底口径。
    const std::string db_path = (dir_ / "trade.db").string();
    set_db_path(db_path);  // 库此刻还不存在
    ready("ACC1", day_epoch(2026, 10, 9));
    ws_.messages.clear();
    // 价格 3500.0 无小数 → 无 tick 时兜底 0 位；真 tick 0.5 会给 1 位（可区分）
    feed_order(make_order("ACC1", 1, day_epoch(2026, 10, 9), 32400, 1));
    ASSERT_EQ(ws_.messages.size(), 1U);
    EXPECT_EQ(ws_.messages[0].data["price_decimals"].get<int>(), 0);

    // td 登录完成并写库；再次 Ready → 负缓存失效
    (void)dztrader::tdstore::ensure_trade_db(db_path);
    sqlite3* db = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_open(db_path.c_str(), &db));
    sqlite3_stmt* stmt = nullptr;
    ASSERT_EQ(SQLITE_OK,
              sqlite3_prepare_v2(db,
                                 "INSERT INTO instruments (instrument_id, exchange_id, price_tick)"
                                 " VALUES ('rb2601', 'SHFE', 0.5)",
                                 -1, &stmt, nullptr));
    ASSERT_EQ(SQLITE_DONE, sqlite3_step(stmt));
    sqlite3_finalize(stmt);
    sqlite3_close(db);

    ready("ACC1", day_epoch(2026, 10, 9));
    ws_.messages.clear();
    feed_order(make_order("ACC1", 2, day_epoch(2026, 10, 9), 32401, 2));
    ASSERT_EQ(ws_.messages.size(), 1U);
    // 同一个价格 3500.0：兜底口径给 0，取到真 tick（0.5）才给 1
    EXPECT_EQ(ws_.messages[0].data["price_decimals"].get<int>(), 1);
}

TEST_F(TdDomainServiceTest, PriceTickIsCachedAcrossReports) {
    const std::string db_path = create_db_with_tick("rb2601", 0.5);
    set_db_path(db_path);
    ready("ACC1", day_epoch(2026, 10, 9));
    ws_.messages.clear();
    feed_order(make_order("ACC1", 1, day_epoch(2026, 10, 9), 32400, 1));
    // 库在第二次查询前消失：若未缓存，价格小数位会退化为 0（价格 3500 无小数）。
    std::error_code ec;
    std::filesystem::remove(db_path, ec);
    feed_order(make_order("ACC1", 2, day_epoch(2026, 10, 9), 32401, 2));
    ASSERT_EQ(ws_.messages.size(), 2U);
    EXPECT_EQ(ws_.messages[0].data["price_decimals"].get<int>(), 1);
    EXPECT_EQ(ws_.messages[1].data["price_decimals"].get<int>(), 1);
}

// ===== td_recent 建连补发（契约 webui-ws §2.5）=====

TEST_F(TdDomainServiceTest, RecentEventsIsEmptyArrayWhenNoEvents) {
    const auto payload = service_->recent_events();
    ASSERT_TRUE(payload.contains("events"));
    EXPECT_TRUE(payload["events"].is_array());
    EXPECT_TRUE(payload["events"].empty());
}

TEST_F(TdDomainServiceTest, RecentEventsCarriesTypeAndRowDataInArrivalOrder) {
    ready("ACC1", day_epoch(2026, 10, 9));
    feed_order(make_order("ACC1", 1, day_epoch(2026, 10, 9), 32400, 1));
    feed_order(make_order("ACC1", 2, day_epoch(2026, 10, 9), 32401, 2));

    const auto payload = service_->recent_events();
    const auto& events = payload["events"];
    ASSERT_EQ(events.size(), 2U);
    EXPECT_EQ(events[0]["type"].get<std::string>(), "td_order_report");
    EXPECT_EQ(events[0]["data"]["order_id"].get<int64_t>(), 1);
    EXPECT_EQ(events[1]["data"]["order_id"].get<int64_t>(), 2);
}

TEST_F(TdDomainServiceTest, RecentEventsCappedAtThreeHundredPerAccount) {
    ready("ACC1", day_epoch(2026, 10, 9));
    constexpr int kSent = 305;
    for (int i = 0; i < kSent; ++i) {
        feed_order(make_order("ACC1", 1000 + i, day_epoch(2026, 10, 9), 32400 + (i % 60),
                              static_cast<uint64_t>(i) + 1));
    }
    const auto payload = service_->recent_events();
    const auto& events = payload["events"];
    ASSERT_EQ(events.size(), kTdRecentCapacityPerAccount);
    // 丢最旧：首条 = 第 (305-300) 个（order_id = 1005），末条 = 最后一个
    EXPECT_EQ(events.front()["data"]["order_id"].get<int64_t>(), 1005);
    EXPECT_EQ(events.back()["data"]["order_id"].get<int64_t>(), 1000 + kSent - 1);
}

TEST_F(TdDomainServiceTest, RecentEventsCapacityIsPerAccount) {
    ready("ACC1", day_epoch(2026, 10, 9));
    ready("ACC2", day_epoch(2026, 10, 9));
    for (int i = 0; i < 200; ++i) {
        feed_order(make_order("ACC1", 1000 + i, day_epoch(2026, 10, 9), 32400,
                              static_cast<uint64_t>(i) + 1));
        feed_order(make_order("ACC2", 2000 + i, day_epoch(2026, 10, 9), 32400,
                              static_cast<uint64_t>(i) + 1));
    }
    const auto payload = service_->recent_events();
    EXPECT_EQ(payload["events"].size(), 400U);
}

TEST_F(TdDomainServiceTest, OfflineClearsThatAccountsRecentRing) {
    ready("ACC1", day_epoch(2026, 10, 9));
    feed_order(make_order("ACC1", 1, day_epoch(2026, 10, 9), 32400, 1));
    ASSERT_EQ(service_->recent_events()["events"].size(), 1U);
    offline("ACC1");
    EXPECT_TRUE(service_->recent_events()["events"].empty());
}

TEST_F(TdDomainServiceTest, LoggingInKeepsRecentRingForRepush) {
    ready("ACC1", day_epoch(2026, 10, 9));
    feed_order(make_order("ACC1", 1, day_epoch(2026, 10, 9), 32400, 1));
    logging_in("ACC1");
    // 契约 td-data-sync §8 只在 Offline 清环：LoggingIn 是过渡态，环内容保留；
    // 但 webui-ws §2.5 的 td_recent 载荷只含**在线**账户 → 该账户重新 Ready 后才会补发。
    EXPECT_TRUE(service_->recent_events()["events"].empty());
    ready("ACC1", day_epoch(2026, 10, 9));
    EXPECT_EQ(service_->recent_events()["events"].size(), 1U);
}

// ===== 统计计数器：重算触发点（契约 rest §2.9.5）=====

TEST_F(TdDomainServiceTest, StatsIsNotReadyWithoutInjectedQuery) {
    // 未注入 TdStatsQuery → 统计恒 not_ready，且不影响委托列表（本条只断言统计体）。
    const auto body = service_->stats_body();
    EXPECT_FALSE(body["available"].get<bool>());
    EXPECT_EQ(body["reason"].get<std::string>(), "not_ready");
    EXPECT_FALSE(body.contains("stats"));
    EXPECT_TRUE(service_->online_accounts().empty());
}

TEST_F(TdDomainServiceTest, InjectedStatsQueryRecomputesImmediatelyOnStartup) {
    const std::string path = create_stats_db();
    insert_order_row("ACC1", "20261009", 1, DZ_ORDER_NOT_TRADED, 5, 0, 11);
    inject_stats_query(path);

    // 注入即重算（启动触发点）：此时无在线账户 → 全 0 且 ready。
    const auto body = service_->stats_body();
    EXPECT_TRUE(body["available"].get<bool>());
    EXPECT_TRUE(body["trading_day"].is_null());
    expect_stats(0, 0, 0, 0);
}

TEST_F(TdDomainServiceTest, ReadyTriggersRecomputeReflectingDatabaseRows) {
    const std::string path = create_stats_db();
    insert_order_row("ACC1", "20261009", 1, DZ_ORDER_NOT_TRADED, 5, 0, 11);
    insert_order_row("ACC1", "20261009", 2, DZ_ORDER_ALL_TRADED, 3, 3, 12);
    insert_order_row("ACC1", "20261009", 3, DZ_ORDER_CANCELLED, 4, 0, 13);
    inject_stats_query(path);
    ready("ACC1", day_epoch(2026, 10, 9));

    // 手算：total 3 笔 / 12 手（含撤单）；未成交 1 笔 / 5 手。
    expect_stats(3, 12, 1, 5);
    const auto body = service_->stats_body();
    EXPECT_TRUE(body["available"].get<bool>());
    EXPECT_EQ(body["trading_day"].get<std::string>(), "20261009");
    ASSERT_EQ(body["online_accounts"].size(), 1U);
    EXPECT_EQ(body["online_accounts"][0].get<std::string>(), "ACC1");
}

TEST_F(TdDomainServiceTest, RepeatedReadyResyncsBecauseTdRestartSkipsOffline) {
    const std::string path = create_stats_db();
    insert_order_row("ACC1", "20261009", 1, DZ_ORDER_NOT_TRADED, 5, 0, 11);
    inject_stats_query(path);
    ready("ACC1", day_epoch(2026, 10, 9));
    expect_stats(1, 5, 1, 5);

    // td 崩溃重启来不及推 Offline，重登后直接再来一条**同账户同日** Ready：
    // 账户本就在线、日切也没发生，但这次 Ready 是重同步点——期间可能有别的终端
    // 或重启前未落库的委托已进库，计数器必须重算到最新，而不是等到周期对账。
    insert_order_row("ACC1", "20261009", 2, DZ_ORDER_ALL_TRADED, 3, 3, 12);
    ready("ACC1", day_epoch(2026, 10, 9));

    // 手算：total 2 笔 / 8 手；未成交仍 1 笔 / 5 手。
    expect_stats(2, 8, 1, 5);
}

TEST_F(TdDomainServiceTest, OfflineTriggersRecomputeAndCounterFallsBackToZero) {
    const std::string path = create_stats_db();
    insert_order_row("ACC1", "20261009", 1, DZ_ORDER_NOT_TRADED, 5, 0, 11);
    insert_order_row("ACC2", "20261009", 2, DZ_ORDER_NOT_TRADED, 2, 0, 12);
    inject_stats_query(path);
    ready("ACC1", day_epoch(2026, 10, 9));
    ready("ACC2", day_epoch(2026, 10, 9));
    expect_stats(2, 7, 2, 7);

    // ACC2 下线 → 重算后只含 ACC1；计数器回落。
    offline("ACC2");
    expect_stats(1, 5, 1, 5);

    // 全部下线 → 无在线账户，口径为空（0 值但 ready）。
    offline("ACC1");
    expect_stats(0, 0, 0, 0);
    const auto body = service_->stats_body();
    EXPECT_TRUE(body["available"].get<bool>());
    EXPECT_TRUE(body["trading_day"].is_null());
}

TEST_F(TdDomainServiceTest, TradingDayChangeDoesNotCarryOverPreviousDayRows) {
    const std::string path = create_stats_db();
    // 旧交易日两笔、新交易日一笔。
    insert_order_row("ACC1", "20261008", 1, DZ_ORDER_NOT_TRADED, 5, 0, 11);
    insert_order_row("ACC1", "20261008", 2, DZ_ORDER_ALL_TRADED, 3, 3, 12);
    insert_order_row("ACC1", "20261009", 3, DZ_ORDER_NOT_TRADED, 2, 0, 13);
    inject_stats_query(path);

    ready("ACC1", day_epoch(2026, 10, 8));
    expect_stats(2, 8, 1, 5);

    // 日切：同一账户 date 变化 → 重算只看新交易日。
    ready("ACC1", day_epoch(2026, 10, 9));
    EXPECT_EQ(service_->current_trading_day().value_or(""), "20261009");
    expect_stats(1, 2, 1, 2);
}

// ===== 统计增量：ORDER_REPORT（契约 rest §2.9.5）=====

TEST_F(TdDomainServiceTest, OrderReportIncrementCountsNewOrderAndReachesStatsBody) {
    const std::string path = create_stats_db();
    inject_stats_query(path);
    ready("ACC1", day_epoch(2026, 10, 9));
    // 重算后水位 = 0（库为空）→ seq 1 的帧走增量。
    feed_order(make_order_with(1, DZ_ORDER_NOT_TRADED, 5, 0, 1, day_epoch(2026, 10, 9)));
    expect_stats(1, 5, 1, 5);

    // 部分成交 → 未成交剩余量 5 → 3，total 不变。
    feed_order(make_order_with(1, DZ_ORDER_PART_TRADED, 5, 2, 2, day_epoch(2026, 10, 9)));
    expect_stats(1, 5, 1, 3);

    // 全成 → 退出未成交集合。
    feed_order(make_order_with(1, DZ_ORDER_ALL_TRADED, 5, 5, 3, day_epoch(2026, 10, 9)));
    expect_stats(1, 5, 0, 0);
}

TEST_F(TdDomainServiceTest, OrderReportCancelAndRejectAreCountedInTotalOnly) {
    const std::string path = create_stats_db();
    inject_stats_query(path);
    ready("ACC1", day_epoch(2026, 10, 9));

    feed_order(make_order_with(1, DZ_ORDER_NOT_TRADED, 4, 0, 1, day_epoch(2026, 10, 9)));
    feed_order(make_order_with(1, DZ_ORDER_CANCELLED, 4, 0, 2, day_epoch(2026, 10, 9)));
    // 新委托直接废单（本地拒单）：计 total，不进未成交。
    feed_order(make_order_with(2, DZ_ORDER_REJECTED, 3, 0, 3, day_epoch(2026, 10, 9)));

    expect_stats(2, 7, 0, 0);
}

TEST_F(TdDomainServiceTest, ReplayedOrderReportAtOrBelowWatermarkIsNotCountedAgain) {
    const std::string path = create_stats_db();
    // 库里已有一笔 seq=11 的委托 => 重算后水位 = 11。
    insert_order_row("ACC1", "20261009", 1, DZ_ORDER_NOT_TRADED, 5, 0, 11);
    inject_stats_query(path);
    ready("ACC1", day_epoch(2026, 10, 9));
    expect_stats(1, 5, 1, 5);

    // 登录「重推最后一条」用原 seq 重发同一帧：seq <= 水位 → 跳过，不重复计数。
    feed_order(make_order_with(1, DZ_ORDER_NOT_TRADED, 5, 0, 11, day_epoch(2026, 10, 9)));
    expect_stats(1, 5, 1, 5);

    // 更旧的 seq 同样跳过。
    feed_order(make_order_with(1, DZ_ORDER_NOT_TRADED, 5, 0, 7, day_epoch(2026, 10, 9)));
    expect_stats(1, 5, 1, 5);

    // seq > 水位的新事件正常增量。
    feed_order(make_order_with(2, DZ_ORDER_NOT_TRADED, 2, 0, 12, day_epoch(2026, 10, 9)));
    expect_stats(2, 7, 2, 7);
}

TEST_F(TdDomainServiceTest, OrderReportFromOfflineAccountDoesNotTouchStats) {
    const std::string path = create_stats_db();
    inject_stats_query(path);
    ready("ACC1", day_epoch(2026, 10, 9));
    // ACC2 不在线：帧既不转发也不计统计。
    feed_order(make_order_with(1, DZ_ORDER_NOT_TRADED, 5, 0, 1, day_epoch(2026, 10, 9), "ACC2"));
    expect_stats(0, 0, 0, 0);
}

TEST_F(TdDomainServiceTest, OrderReportIncrementAndReconcileAgreeOnSameData) {
    const std::string path = create_stats_db();
    inject_stats_query(path);
    ready("ACC1", day_epoch(2026, 10, 9));

    // 增量路径：三帧（新单 / 部分成交 / 撤单）。
    feed_order(make_order_with(1, DZ_ORDER_NOT_TRADED, 5, 0, 1, day_epoch(2026, 10, 9)));
    feed_order(make_order_with(1, DZ_ORDER_PART_TRADED, 5, 2, 2, day_epoch(2026, 10, 9)));
    feed_order(make_order_with(1, DZ_ORDER_CANCELLED, 5, 2, 3, day_epoch(2026, 10, 9)));
    feed_order(make_order_with(2, DZ_ORDER_NOT_TRADED, 4, 0, 4, day_epoch(2026, 10, 9)));
    const StatsValues from_increment = stats_values();
    // 手算：total 2 笔 / 9 手；未成交 1 笔 / 4 手（1 号已撤）。
    EXPECT_EQ(from_increment.total_orders, 2);
    EXPECT_EQ(from_increment.total_volume, 9);
    EXPECT_EQ(from_increment.unfilled_orders, 1);
    EXPECT_EQ(from_increment.unfilled_volume, 4);

    // 把同样的事实写进库（模拟 persist 追上），再全量重算：两条路径必须同值。
    insert_order_row("ACC1", "20261009", 1, DZ_ORDER_CANCELLED, 5, 2, 3);
    insert_order_row("ACC1", "20261009", 2, DZ_ORDER_NOT_TRADED, 4, 0, 4);
    service_->reconcile_stats();
    expect_stats(2, 9, 1, 4);
}

// ===== 周期对账：可注入调度器（设计 §2.4 / §5.3）=====

TEST_F(TdDomainServiceTest, InjectedSchedulerRegistersTaskAndManualTickReconciles) {
    // 假调度器：记录注册的 interval 与 task，测试直接 tick（不 sleep）。
    double registered_interval = 0.0;
    std::function<void()> registered_task;
    int registrations = 0;
    service_->set_reconcile_scheduler([&registered_interval, &registered_task, &registrations](
                                          double interval, std::function<void()> task) {
        ++registrations;
        registered_interval = interval;
        registered_task = std::move(task);
    });
    ASSERT_EQ(registrations, 1);
    // 契约 rest §2.9.5：建议 30s（实现可调，常量单源）。
    EXPECT_DOUBLE_EQ(registered_interval, kTdStatsReconcileIntervalSec);
    ASSERT_TRUE(registered_task != nullptr);

    const std::string path = create_stats_db();
    inject_stats_query(path);
    ready("ACC1", day_epoch(2026, 10, 9));
    expect_stats(0, 0, 0, 0);

    // 库在两次重算之间多了一笔（例如 td 直写 / 上一轮失败后恢复）：tick 后收敛。
    insert_order_row("ACC1", "20261009", 1, DZ_ORDER_NOT_TRADED, 6, 0, 21);
    registered_task();
    expect_stats(1, 6, 1, 6);
}

TEST_F(TdDomainServiceTest, ReconcileKeepsCounterWhenDatabaseBecomesUnreadable) {
    const std::string path = create_stats_db();
    insert_order_row("ACC1", "20261009", 1, DZ_ORDER_NOT_TRADED, 5, 0, 11);
    inject_stats_query(path);
    ready("ACC1", day_epoch(2026, 10, 9));
    expect_stats(1, 5, 1, 5);

    // 契约 rest §2.9.5：计数器就绪后交易库暂时不可读**不降级**——保持旧值与 ready。
    std::error_code ec;
    std::filesystem::remove(path, ec);
    service_->reconcile_stats();
    expect_stats(1, 5, 1, 5);
    EXPECT_TRUE(service_->stats_body()["available"].get<bool>());
}

TEST_F(TdDomainServiceTest, StatsReconcileDoesNotDisturbOrdersListOrWsForwarding) {
    const std::string path = create_stats_db();
    inject_stats_query(path);
    ready("ACC1", day_epoch(2026, 10, 9));
    ws_.messages.clear();
    // WS 转发逻辑与 #246 完全不变：ORDER_REPORT 仍照常广播。
    feed_order(make_order_with(1, DZ_ORDER_NOT_TRADED, 5, 0, 1, day_epoch(2026, 10, 9)));
    ASSERT_NE(find_broadcast("td_order_report"), nullptr);
    // 统计重算不影响在线集合 / 交易日。
    EXPECT_EQ(service_->online_accounts(), std::vector<std::string>{"ACC1"});
    EXPECT_EQ(service_->current_trading_day().value_or(""), "20261009");
}

// ===== TRADE_REPORT → WS td_trade_report（契约 webui-ws §2.5）=====

TEST_F(TdDomainServiceTest, TradeReportForOnlineAccountIsBroadcastAsRowPayload) {
    const std::string db_path = create_db_with_tick("rb2601", 0.5);
    set_db_path(db_path);
    ready("ACC1", day_epoch(2026, 10, 9));
    ws_.messages.clear();

    DzTradeReport report =
        make_trade("ACC1", "00000000001", 10231, day_epoch(2026, 10, 9), 32405, 1843);
    report.direction = DZ_DIRECTION_SHORT;
    report.price = 3512.3;
    report.volume = 2;
    feed_trade(report);

    const auto* message = find_broadcast("td_trade_report");
    ASSERT_NE(message, nullptr);
    // 契约 webui-ws §2.5：payload 即行对象，instance_id 为空
    EXPECT_TRUE(message->instance_id.empty());
    const auto& row = message->data;
    EXPECT_EQ(row["account_id"].get<std::string>(), "ACC1");
    EXPECT_EQ(row["date"].get<std::string>(), "20261009");
    EXPECT_EQ(row["trade_id"].get<std::string>(), "00000000001");
    EXPECT_EQ(row["order_id"].get<int64_t>(), 10231);
    EXPECT_EQ(row["strategy_id"].get<std::string>(), "stg1");
    EXPECT_EQ(row["instrument_id"].get<std::string>(), "rb2601");
    EXPECT_EQ(row["exchange_id"].get<std::string>(), "SHFE");
    EXPECT_EQ(row["direction"].get<std::string>(), "Short");
    EXPECT_EQ(row["position_effect"].get<std::string>(), "Open");
    EXPECT_DOUBLE_EQ(row["price"].get<double>(), 3512.3);
    EXPECT_EQ(row["volume"].get<int>(), 2);
    EXPECT_EQ(row["time"].get<int>(), 32405);
    EXPECT_EQ(row["price_decimals"].get<int>(), 1);  // tick 0.5 → 1
    EXPECT_EQ(row["seq"].get<uint64_t>(), 1843U);
    EXPECT_TRUE(row["sort_key"].is_string());
    // 契约 rest §2.9.3：成交行没有成交均价字段
    EXPECT_FALSE(row.contains("avg_price"));
    EXPECT_FALSE(row.contains("avg_price_decimals"));
}

TEST_F(TdDomainServiceTest, TradeReportForOfflineAccountIsNotBroadcast) {
    ready("ACC1", day_epoch(2026, 10, 9));
    ws_.messages.clear();
    feed_trade(make_trade("ACC2", "0000000001", 7, day_epoch(2026, 10, 9), 32400, 1));
    EXPECT_EQ(find_broadcast("td_trade_report"), nullptr);
}

TEST_F(TdDomainServiceTest, TradeReportBeforeAnyReadyIsNotBroadcast) {
    ws_.messages.clear();
    feed_trade(make_trade("ACC1", "0000000001", 7, day_epoch(2026, 10, 9), 32400, 1));
    EXPECT_EQ(find_broadcast("td_trade_report"), nullptr);
}

TEST_F(TdDomainServiceTest, TradeReportDoesNotTouchStatsCounters) {
    const std::string path = create_stats_db();
    inject_stats_query(path);
    ready("ACC1", day_epoch(2026, 10, 9));
    // 契约 rest §2.9.5：统计 4 值只来自**委托**行；成交帧不推进计数器。
    feed_trade(make_trade("ACC1", "0000000001", 1, day_epoch(2026, 10, 9), 32400, 1));
    expect_stats(0, 0, 0, 0);
}

TEST_F(TdDomainServiceTest, RecentEventsCarryBothOrderAndTradeRowsInArrivalOrder) {
    ready("ACC1", day_epoch(2026, 10, 9));
    feed_order(make_order("ACC1", 1, day_epoch(2026, 10, 9), 32400, 1));
    feed_trade(make_trade("ACC1", "0000000001", 1, day_epoch(2026, 10, 9), 32401, 2));
    feed_order(make_order("ACC1", 2, day_epoch(2026, 10, 9), 32402, 3));

    const auto payload = service_->recent_events();
    const auto& events = payload["events"];
    // 契约 td-data-sync §8：同一账户环同时容纳委托与成交事件，按入环先后从旧到新。
    ASSERT_EQ(events.size(), 3U);
    EXPECT_EQ(events[0]["type"].get<std::string>(), "td_order_report");
    EXPECT_EQ(events[0]["data"]["order_id"].get<int64_t>(), 1);
    EXPECT_EQ(events[1]["type"].get<std::string>(), "td_trade_report");
    EXPECT_EQ(events[1]["data"]["trade_id"].get<std::string>(), "0000000001");
    EXPECT_EQ(events[2]["type"].get<std::string>(), "td_order_report");
    EXPECT_EQ(events[2]["data"]["order_id"].get<int64_t>(), 2);
}

TEST_F(TdDomainServiceTest, RecentEventsRingCapacityIsSharedByOrdersAndTrades) {
    ready("ACC1", day_epoch(2026, 10, 9));
    // 契约 td-data-sync §8：容量 300 条/账户，四类事件共享（不是每类各 300）。
    for (size_t i = 0; i < kTdRecentCapacityPerAccount; ++i) {
        feed_order(make_order("ACC1", 1000 + static_cast<int64_t>(i), day_epoch(2026, 10, 9), 32400,
                              static_cast<uint64_t>(i) + 1));
    }
    feed_trade(make_trade("ACC1", "TRADE_LAST", 9999, day_epoch(2026, 10, 9), 32401,
                          static_cast<uint64_t>(kTdRecentCapacityPerAccount) + 1));

    const nlohmann::json payload = service_->recent_events();
    const auto& events = payload["events"];
    ASSERT_EQ(events.size(), kTdRecentCapacityPerAccount);
    // 丢最旧：首条不再是第一个委托，末条是新到的成交。
    EXPECT_EQ(events.back()["type"].get<std::string>(), "td_trade_report");
    EXPECT_EQ(events.back()["data"]["trade_id"].get<std::string>(), "TRADE_LAST");
    EXPECT_EQ(events.front()["data"]["order_id"].get<int64_t>(), 1001);
}

TEST_F(TdDomainServiceTest, OfflineClearsBothOrderAndTradeEventsForThatAccount) {
    ready("ACC1", day_epoch(2026, 10, 9));
    feed_order(make_order("ACC1", 1, day_epoch(2026, 10, 9), 32400, 1));
    feed_trade(make_trade("ACC1", "0000000001", 1, day_epoch(2026, 10, 9), 32401, 2));
    ASSERT_EQ(service_->recent_events()["events"].size(), 2U);
    offline("ACC1");
    EXPECT_TRUE(service_->recent_events()["events"].empty());
}

// ===== TRADING_ACCOUNT → WS td_trading_account（契约 webui-ws §2.2/§2.5、rest §2.9.4）=====

TEST_F(TdDomainServiceTest, TradingAccountForOnlineAccountIsBroadcastAsRowPayload) {
    ready("ACC1", day_epoch(2026, 10, 9));
    ws_.messages.clear();

    feed_trading_account(make_trading_account("ACC1", day_epoch(2026, 10, 9), 1850));

    const auto* message = find_broadcast("td_trading_account");
    ASSERT_NE(message, nullptr);
    // 契约 webui-ws §2.5：payload 即行对象（与 REST items 同 schema），instance_id 为空。
    EXPECT_TRUE(message->instance_id.empty());
    const auto& row = message->data;
    EXPECT_EQ(row["account_id"].get<std::string>(), "ACC1");
    EXPECT_EQ(row["date"].get<std::string>(), "20261009");
    EXPECT_TRUE(row["has_data"].get<bool>());
    EXPECT_DOUBLE_EQ(row["balance"].get<double>(), 1000000.0);
    EXPECT_DOUBLE_EQ(row["available"].get<double>(), 850000.5);
    EXPECT_DOUBLE_EQ(row["frozen"].get<double>(), 12000.25);
    EXPECT_DOUBLE_EQ(row["commission"].get<double>(), 345.75);
    EXPECT_DOUBLE_EQ(row["margin"].get<double>(), 150000.0);
    EXPECT_DOUBLE_EQ(row["withdraw_quota"].get<double>(), 800000.0);
    EXPECT_DOUBLE_EQ(row["deposit"].get<double>(), 50000.0);
    EXPECT_DOUBLE_EQ(row["withdraw"].get<double>(), 300.5);
    EXPECT_EQ(row["seq"].get<uint64_t>(), 1850U);
    // 资金行没有 sort_key（契约 rest §2.9.4）；也不返回平台内部列。
    EXPECT_FALSE(row.contains("sort_key"));
    EXPECT_FALSE(row.contains("gateway"));
    EXPECT_FALSE(row.contains("ext"));
}

TEST_F(TdDomainServiceTest, TradingAccountForOfflineAccountIsNotBroadcast) {
    ready("ACC1", day_epoch(2026, 10, 9));
    ws_.messages.clear();
    feed_trading_account(make_trading_account("ACC2", day_epoch(2026, 10, 9), 1));
    EXPECT_EQ(find_broadcast("td_trading_account"), nullptr);
}

TEST_F(TdDomainServiceTest, TradingAccountBeforeAnyReadyIsNotBroadcast) {
    // 登录收尾链的资金帧在 Ready 之前到达：不进补发环也不广播（前端靠 Ready 后的
    // REST 基线拿同一批数据）。契约 td-data-sync §8：只转发在线账户的帧。
    ws_.messages.clear();
    feed_trading_account(make_trading_account("ACC1", day_epoch(2026, 10, 9), 1));
    EXPECT_EQ(find_broadcast("td_trading_account"), nullptr);
}

TEST_F(TdDomainServiceTest, TradingAccountJoinsTheSharedRecentRing) {
    ready("ACC1", day_epoch(2026, 10, 9));
    feed_order(make_order("ACC1", 1, day_epoch(2026, 10, 9), 32400, 1));
    feed_trade(make_trade("ACC1", "0000000001", 1, day_epoch(2026, 10, 9), 32401, 2));
    feed_position(make_position("ACC1", "rb2601", day_epoch(2026, 10, 9), 3));
    feed_trading_account(make_trading_account("ACC1", day_epoch(2026, 10, 9), 4));

    const nlohmann::json payload = service_->recent_events();
    const auto& events = payload["events"];
    // 契约 td-data-sync §8：同一账户环同时容纳委托/成交/持仓/资金事件，按入环先后从旧到新。
    ASSERT_EQ(events.size(), 4U);
    EXPECT_EQ(events[0]["type"].get<std::string>(), "td_order_report");
    EXPECT_EQ(events[1]["type"].get<std::string>(), "td_trade_report");
    EXPECT_EQ(events[2]["type"].get<std::string>(), "td_position_info");
    EXPECT_EQ(events[3]["type"].get<std::string>(), "td_trading_account");
    EXPECT_EQ(events[3]["data"]["account_id"].get<std::string>(), "ACC1");
}

TEST_F(TdDomainServiceTest, TradingAccountDoesNotTouchStatsCounters) {
    const std::string path = create_stats_db();
    inject_stats_query(path);
    ready("ACC1", day_epoch(2026, 10, 9));
    // 契约 rest §2.9.5：统计 4 值只来自**委托**行；资金帧不推进计数器。
    feed_trading_account(make_trading_account("ACC1", day_epoch(2026, 10, 9), 1));
    expect_stats(0, 0, 0, 0);
}

TEST_F(TdDomainServiceTest, TradingAccountRingCapacityIsSharedWithOtherTradeEvents) {
    ready("ACC1", day_epoch(2026, 10, 9));
    for (size_t i = 0; i < kTdRecentCapacityPerAccount; ++i) {
        feed_order(make_order("ACC1", 1000 + static_cast<int64_t>(i), day_epoch(2026, 10, 9), 32400,
                              static_cast<uint64_t>(i) + 1));
    }
    feed_trading_account(make_trading_account(
        "ACC1", day_epoch(2026, 10, 9), static_cast<uint64_t>(kTdRecentCapacityPerAccount) + 1));

    const nlohmann::json payload = service_->recent_events();
    const auto& events = payload["events"];
    ASSERT_EQ(events.size(), kTdRecentCapacityPerAccount);
    EXPECT_EQ(events.back()["type"].get<std::string>(), "td_trading_account");
    EXPECT_EQ(events.front()["data"]["order_id"].get<int64_t>(), 1001);
}

}  // namespace
}  // namespace dztrader::webui
