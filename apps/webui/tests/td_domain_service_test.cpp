/**
 * @file td_domain_service_test.cpp
 * @brief TD 数据域服务（契约 td-data-sync §8 / webui-ws §2.3/§2.5 / rest §2.9.1/§2.9.3/§2.9.6）。
 *
 * 断言外部可观察行为：WS 广播（type/instance_id/data）、`td_recent` 载荷、`data_changed`
 * 广播时机、在线集合与当前交易日。不断言私有成员、不读内部容器。
 *
 * 库形态用**真 schema**（`tdstore::ensure_trade_db`）：精度派生依赖
 * `instruments.price_tick` 列名，手抄 DDL 会让 schema 漂移静默通过。
 */

#include "td_domain_service.h"
#include "test_temp_dir.h"

#include <dztrader/date_time/date.h>
#include <dztrader/shm/frame_view.h>
#include <dztrader/struct.h>
#include <dztrader/tdstore/trade_db.h>
#include <gtest/gtest.h>
#include <sqlite3.h>

#include <cstddef>
#include <cstring>
#include <filesystem>
#include <string>
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

}  // namespace
}  // namespace dztrader::webui
