/**
 * @file td_data_controller_test.cpp
 * @brief 交易数据 REST handler（契约 rest §2.9.1/§2.9.2/§2.9.5/§2.9.8）。
 *
 * `GET /api/td/orders`（列表直查）与 `GET /api/td/stats`（内存计数器）。
 * 真 handler 直调 + 捕获响应（与 sysmon_controller_test 同型）；库用真 schema 建库。
 * 断言 HTTP 语义：200 信封、400 bad request、降级 200 available:false / not_ready。
 */

#include "td_data_controller.h"
#include "fake_data_change_notifier.h"
#include "td_domain_service.h"
#include "td_orders_query.h"
#include "td_positions_query.h"
#include "td_position_row_writer.h"
#include "td_stats_query.h"
#include "td_trade_row_writer.h"
#include "td_trades_query.h"
#include "test_temp_dir.h"
#include "td_order_row_writer.h"

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <dztrader/date_time/date.h>
#include <dztrader/shm/frame_view.h>
#include <dztrader/struct.h>
#include <dztrader/tdstore/trade_db.h>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <cstddef>
#include <cstring>
#include <filesystem>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace dztrader::webui {
namespace {

/// 记录广播的 WsBroadcaster 假实现（域服务构造需要；本文件不断言 WS）。
class NullBroadcaster : public WsBroadcaster {
public:
    void broadcast(const std::string&, const std::string&, const nlohmann::json&) override {}
};

class TdDataControllerTest : public ::testing::Test {
protected:
    std::filesystem::path dir_;
    std::string db_path_;
    FrameRouter router_{[](std::function<void()> f) { f(); }};
    NullBroadcaster ws_;
    FakeDataChangeNotifier notifier_;
    std::shared_ptr<TdDomainService> domain_;
    std::shared_ptr<TdOrdersQuery> query_;
    std::shared_ptr<TdTradesQuery> trades_query_;
    std::shared_ptr<TdPositionsQuery> positions_query_;
    std::shared_ptr<TdCtrl> ctrl_;

    void SetUp() override {
        dir_ = dztrader::test::unique_temp_dir("dz_webui_td_data_controller_test");
        std::filesystem::create_directories(dir_);
        db_path_ = (dir_ / "trade.db").string();
        domain_ = std::make_shared<TdDomainService>(router_, ws_, notifier_);
        domain_->set_trade_db_path_provider([this]() { return db_path_; });
        query_ = std::make_shared<TdOrdersQuery>([this]() { return db_path_; }, 200);
        trades_query_ = std::make_shared<TdTradesQuery>([this]() { return db_path_; }, 200);
        positions_query_ = std::make_shared<TdPositionsQuery>([this]() { return db_path_; }, 200);
        // 读端点不需要 Repository（认证在 JWT advice 层）；nullptr 与生产许可一致。
        ctrl_ = std::make_shared<TdCtrl>(nullptr, domain_, query_, trades_query_, positions_query_);
    }

    void TearDown() override {
        ctrl_.reset();
        positions_query_.reset();
        trades_query_.reset();
        query_.reset();
        domain_.reset();
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    void create_db() { (void)dztrader::tdstore::ensure_trade_db(db_path_); }

    /// 注入统计重算查询（注入即触发一次重算，契约 rest §2.9.5「启动」）。
    void inject_stats_query() {
        auto stats = std::make_shared<TdStatsQuery>([this]() -> std::string { return db_path_; },
                                                    kTdReadBusyTimeoutMs);
        domain_->set_stats_query(std::move(stats));
    }

    /// 一条委托（真 schema；status/volume/seq 可指定）。列清单来自共享夹具。
    void insert_order_row(const std::string& account_id,
                          int64_t order_id,
                          int status,
                          int volume,
                          int volume_traded,
                          uint64_t seq) {
        dztrader::test::TdOrderRowSeed seed;
        seed.account_id = account_id;
        seed.order_id = order_id;
        seed.status = status;
        seed.volume = volume;
        seed.volume_traded = volume_traded;
        seed.seq = seq;
        const std::string error = dztrader::test::insert_td_order_row(db_path_, seed);
        ASSERT_TRUE(error.empty()) << error;
    }

    /// 一条 NotTraded 委托，只指定报单时间（游标分页用例用）。
    void insert_order(const std::string& account_id, int64_t order_id, int32_t time_sec) {
        dztrader::test::TdOrderRowSeed seed;
        seed.account_id = account_id;
        seed.order_id = order_id;
        seed.time = time_sec;
        const std::string error = dztrader::test::insert_td_order_row(db_path_, seed);
        ASSERT_TRUE(error.empty()) << error;
    }

    /// 一条成交（真 schema；列清单来自共享夹具 `td_trade_row_writer.h`）。
    void insert_trade(const std::string& account_id,
                      const std::string& trade_id,
                      int32_t time_sec,
                      int64_t order_id = 1) {
        dztrader::test::TdTradeRowSeed seed;
        seed.account_id = account_id;
        seed.trade_id = trade_id;
        seed.order_id = order_id;
        seed.time = time_sec;
        const std::string error = dztrader::test::insert_td_trade_row(db_path_, seed);
        ASSERT_TRUE(error.empty()) << error;
    }

    /// 一条持仓（真 schema；列清单来自共享夹具 `td_position_row_writer.h`）。
    void insert_position(const std::string& account_id,
                         const std::string& instrument_id,
                         int volume,
                         uint64_t seq) {
        dztrader::test::TdPositionRowSeed seed;
        seed.account_id = account_id;
        seed.instrument_id = instrument_id;
        seed.volume = volume;
        seed.today_volume = volume;
        seed.yd_volume = 0;
        seed.seq = seq;
        const std::string error = dztrader::test::insert_td_position_row(db_path_, seed);
        ASSERT_TRUE(error.empty()) << error;
    }

    void ready(const std::string& account_id, int32_t date) {
        DzAccountStatus status{};
        dztrader::copy_string(status.account_id, account_id.c_str(), true);
        status.state = DZ_ACCOUNT_READY;
        status.date = date;
        alignas(8) std::byte frame[sizeof(DzFrameHeader) + sizeof(DzAccountStatus)];
        auto* header = reinterpret_cast<DzFrameHeader*>(frame);
        header->frame_size = static_cast<uint32_t>(sizeof(frame));
        header->frame_type = DZ_FRAME_ACCOUNT_STATUS;
        std::memcpy(frame + sizeof(DzFrameHeader), &status, sizeof(status));
        router_.dispatch(shm::FrameView(frame));
    }

    static int32_t day_epoch(int year, int month, int day) {
        return dztrader::Date::from_year_month_day(year, month, day).days_since_epoch();
    }

    drogon::HttpRequestPtr request(const std::map<std::string, std::string>& query = {}) {
        auto req = drogon::HttpRequest::newHttpRequest();
        req->setMethod(drogon::Get);
        req->setPath("/api/td/orders");
        for (const auto& [key, value] : query) {
            req->setParameter(key, value);
        }
        req->getAttributes()->insert("user_id", std::string("admin"));
        return req;
    }

    drogon::HttpResponsePtr invoke(const std::map<std::string, std::string>& query = {}) {
        drogon::HttpResponsePtr captured;
        ctrl_->get_orders(request(query),
                          [&captured](const drogon::HttpResponsePtr& resp) { captured = resp; });
        return captured;
    }

    /// `GET /api/td/trades`（真 handler 直调，与 invoke 同型）。
    drogon::HttpResponsePtr invoke_trades(const std::map<std::string, std::string>& query = {}) {
        auto req = drogon::HttpRequest::newHttpRequest();
        req->setMethod(drogon::Get);
        req->setPath("/api/td/trades");
        for (const auto& [key, value] : query) {
            req->setParameter(key, value);
        }
        req->getAttributes()->insert("user_id", std::string("admin"));
        drogon::HttpResponsePtr captured;
        ctrl_->get_trades(req,
                          [&captured](const drogon::HttpResponsePtr& resp) { captured = resp; });
        return captured;
    }

    /// `GET /api/td/positions`（真 handler 直调，与 invoke 同型）。
    drogon::HttpResponsePtr invoke_positions(const std::map<std::string, std::string>& query = {}) {
        auto req = drogon::HttpRequest::newHttpRequest();
        req->setMethod(drogon::Get);
        req->setPath("/api/td/positions");
        for (const auto& [key, value] : query) {
            req->setParameter(key, value);
        }
        req->getAttributes()->insert("user_id", std::string("admin"));
        drogon::HttpResponsePtr captured;
        ctrl_->get_positions(req,
                             [&captured](const drogon::HttpResponsePtr& resp) { captured = resp; });
        return captured;
    }

    /// `GET /api/td/stats`：请求可带任意 query（handler 必须忽略全部参数）。
    drogon::HttpResponsePtr invoke_stats(const std::map<std::string, std::string>& query = {}) {
        auto req = drogon::HttpRequest::newHttpRequest();
        req->setMethod(drogon::Get);
        req->setPath("/api/td/stats");
        for (const auto& [key, value] : query) {
            req->setParameter(key, value);
        }
        req->getAttributes()->insert("user_id", std::string("admin"));
        drogon::HttpResponsePtr captured;
        ctrl_->get_stats(req,
                         [&captured](const drogon::HttpResponsePtr& resp) { captured = resp; });
        return captured;
    }

    static nlohmann::json body_of(const drogon::HttpResponsePtr& resp) {
        return nlohmann::json::parse(resp->getBody());
    }
};

TEST_F(TdDataControllerTest, ReturnsEnvelopeWithItemsForOnlineAccounts) {
    create_db();
    insert_order("ACC1", 1, 32400);
    ready("ACC1", day_epoch(2026, 10, 9));

    const auto resp = invoke();
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->getStatusCode(), drogon::k200OK);
    const auto body = body_of(resp);
    EXPECT_TRUE(body["available"].get<bool>());
    EXPECT_EQ(body["trading_day"].get<std::string>(), "20261009");
    ASSERT_EQ(body["online_accounts"].size(), 1U);
    EXPECT_EQ(body["online_accounts"][0].get<std::string>(), "ACC1");
    ASSERT_EQ(body["items"].size(), 1U);
    EXPECT_EQ(body["items"][0]["order_id"].get<int64_t>(), 1);
    EXPECT_TRUE(body["next_cursor"].is_null());
}

TEST_F(TdDataControllerTest, Returns400OnInvalidParameters) {
    create_db();
    ready("ACC1", day_epoch(2026, 10, 9));
    const std::map<std::string, std::map<std::string, std::string>> bad_cases = {
        {"limit zero", {{"limit", "0"}}},
        {"limit too large", {{"limit", "1001"}}},
        {"limit non numeric", {{"limit", "abc"}}},
        {"malformed cursor", {{"cursor", "not-a-cursor"}}},
    };
    for (const auto& [label, query] : bad_cases) {
        const auto resp = invoke(query);
        ASSERT_NE(resp, nullptr) << label;
        EXPECT_EQ(resp->getStatusCode(), drogon::k400BadRequest) << label;
        EXPECT_EQ(body_of(resp).value("error", ""), "bad request") << label;
    }
}

TEST_F(TdDataControllerTest, Returns400WhenCursorScopeChanged) {
    create_db();
    for (int i = 0; i < 3; ++i) {
        insert_order("ACC1", 1 + i, 32400 + i);
    }
    ready("ACC1", day_epoch(2026, 10, 9));
    const auto first = invoke({{"limit", "1"}});
    ASSERT_NE(first, nullptr);
    ASSERT_EQ(first->getStatusCode(), drogon::k200OK);
    const auto cursor = body_of(first)["next_cursor"].get<std::string>();

    // accounts 作用域变化：游标作废（契约 rest §2.9.2）
    const auto resp = invoke({{"limit", "1"}, {"cursor", cursor}, {"accounts", "OTHER"}});
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->getStatusCode(), drogon::k400BadRequest);
    EXPECT_EQ(body_of(resp).value("error", ""), "bad request");
}

TEST_F(TdDataControllerTest, DegradesTo200WithReasonWhenDatabaseMissing) {
    ready("ACC1", day_epoch(2026, 10, 9));
    const auto resp = invoke();
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->getStatusCode(), drogon::k200OK);
    const auto body = body_of(resp);
    EXPECT_FALSE(body.value("available", true));
    EXPECT_EQ(body.value("reason", ""), "not_found");
    EXPECT_FALSE(body.contains("items"));
}

TEST_F(TdDataControllerTest, EmptyItemsWhenNoOnlineAccounts) {
    create_db();
    insert_order("ACC1", 1, 32400);
    // 无 ACCOUNT_STATUS → 无在线账户
    const auto resp = invoke();
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->getStatusCode(), drogon::k200OK);
    const auto body = body_of(resp);
    EXPECT_TRUE(body.value("available", false));
    EXPECT_TRUE(body["trading_day"].is_null());
    EXPECT_TRUE(body["items"].empty());
    EXPECT_TRUE(body["next_cursor"].is_null());
}

TEST_F(TdDataControllerTest, DegradesInsteadOf5xxWhenNotWired) {
    // 装配缺失（domain/query 为空）：契约 §2.9.8 不上 5xx
    auto ctrl = std::make_shared<TdCtrl>(nullptr, nullptr, nullptr, nullptr, nullptr);
    drogon::HttpResponsePtr captured;
    ctrl->get_orders(request(),
                     [&captured](const drogon::HttpResponsePtr& resp) { captured = resp; });
    ASSERT_NE(captured, nullptr);
    EXPECT_EQ(captured->getStatusCode(), drogon::k200OK);
    EXPECT_FALSE(body_of(captured).value("available", true));
}

// ===== GET /api/td/stats（契约 rest §2.9.5）=====

TEST_F(TdDataControllerTest, StatsReturnsEnvelopeWithFourCounters) {
    create_db();
    // 三笔：未成交 5 手 / 全成 3 手 / 撤单 4 手。
    insert_order_row("ACC1", 1, DZ_ORDER_NOT_TRADED, 5, 0, 11);
    insert_order_row("ACC1", 2, DZ_ORDER_ALL_TRADED, 3, 3, 12);
    insert_order_row("ACC1", 3, DZ_ORDER_CANCELLED, 4, 0, 13);
    inject_stats_query();
    ready("ACC1", day_epoch(2026, 10, 9));

    const auto resp = invoke_stats();
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->getStatusCode(), drogon::k200OK);
    const auto body = body_of(resp);
    EXPECT_TRUE(body["available"].get<bool>());
    EXPECT_EQ(body["trading_day"].get<std::string>(), "20261009");
    ASSERT_EQ(body["online_accounts"].size(), 1U);
    EXPECT_EQ(body["online_accounts"][0].get<std::string>(), "ACC1");
    ASSERT_TRUE(body.contains("stats"));
    // 手算（契约 §2.9.5）：total = 3 笔 / 12 手（含撤单）；未成交 = 1 笔 / 5 手。
    EXPECT_EQ(body["stats"]["total_orders"].get<int64_t>(), 3);
    EXPECT_EQ(body["stats"]["total_volume"].get<int64_t>(), 12);
    EXPECT_EQ(body["stats"]["unfilled_orders"].get<int64_t>(), 1);
    EXPECT_EQ(body["stats"]["unfilled_volume"].get<int64_t>(), 5);
    // stats 不是列表：无 items / next_cursor。
    EXPECT_FALSE(body.contains("items"));
    EXPECT_FALSE(body.contains("next_cursor"));
    EXPECT_FALSE(body.contains("reason"));
}

TEST_F(TdDataControllerTest, StatsIgnoresAllQueryParameters) {
    create_db();
    insert_order_row("ACC1", 1, DZ_ORDER_NOT_TRADED, 5, 0, 11);
    insert_order_row("ACC2", 2, DZ_ORDER_NOT_TRADED, 2, 0, 12);
    inject_stats_query();
    ready("ACC1", day_epoch(2026, 10, 9));
    ready("ACC2", day_epoch(2026, 10, 9));

    // 契约 rest §2.9.5：stats 不接受 accounts/limit/cursor 过滤，且不校验参数（传了也不 400）。
    const auto plain = invoke_stats();
    ASSERT_NE(plain, nullptr);
    const auto filtered = invoke_stats(
        {{"accounts", "ACC1"}, {"limit", "0"}, {"cursor", "not-a-cursor"}, {"bogus", "x"}});
    ASSERT_NE(filtered, nullptr);
    EXPECT_EQ(filtered->getStatusCode(), drogon::k200OK);
    // 全局口径：带 accounts=ACC1 仍是两笔（与不带参数完全同值）。
    EXPECT_EQ(body_of(filtered), body_of(plain));
    EXPECT_EQ(body_of(filtered)["stats"]["total_orders"].get<int64_t>(), 2);
    EXPECT_EQ(body_of(filtered)["stats"]["total_volume"].get<int64_t>(), 7);
}

TEST_F(TdDataControllerTest, StatsReturnsNotReadyWhenQueryNotInjected) {
    // 未注入 TdStatsQuery：200 + 降级体（只有 available/reason），绝不 5xx。
    const auto resp = invoke_stats();
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->getStatusCode(), drogon::k200OK);
    const auto body = body_of(resp);
    ASSERT_EQ(body.size(), 2U);
    EXPECT_FALSE(body["available"].get<bool>());
    EXPECT_EQ(body["reason"].get<std::string>(), "not_ready");
    EXPECT_FALSE(body.contains("stats"));
    EXPECT_FALSE(body.contains("trading_day"));
    EXPECT_FALSE(body.contains("online_accounts"));
}

TEST_F(TdDataControllerTest, StatsReturnsNotReadyWhenDomainNotWired) {
    // 装配缺失（domain 为空）：200 降级而不是 5xx（契约 rest §2.9.8 精神）。
    auto ctrl = std::make_shared<TdCtrl>(nullptr, nullptr, nullptr, nullptr, nullptr);
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Get);
    req->setPath("/api/td/stats");
    drogon::HttpResponsePtr captured;
    ctrl->get_stats(req, [&captured](const drogon::HttpResponsePtr& resp) { captured = resp; });
    ASSERT_NE(captured, nullptr);
    EXPECT_EQ(captured->getStatusCode(), drogon::k200OK);
    const auto body = body_of(captured);
    EXPECT_FALSE(body["available"].get<bool>());
    EXPECT_EQ(body["reason"].get<std::string>(), "not_ready");
}

TEST_F(TdDataControllerTest, StatsReflectsOfflineAndStaysReadyWithZeroCounters) {
    create_db();
    insert_order_row("ACC1", 1, DZ_ORDER_NOT_TRADED, 5, 0, 11);
    inject_stats_query();
    ready("ACC1", day_epoch(2026, 10, 9));
    EXPECT_EQ(body_of(invoke_stats())["stats"]["total_orders"].get<int64_t>(), 1);

    // 账户下线 → 在线集合变化触发重算：口径变空，但计数器仍 ready（不降级）。
    DzAccountStatus status{};
    dztrader::copy_string(status.account_id, "ACC1", true);
    status.state = DZ_ACCOUNT_OFFLINE;
    status.date = 0;
    alignas(8) std::byte frame[sizeof(DzFrameHeader) + sizeof(DzAccountStatus)];
    auto* header = reinterpret_cast<DzFrameHeader*>(frame);
    header->frame_size = static_cast<uint32_t>(sizeof(frame));
    header->frame_type = DZ_FRAME_ACCOUNT_STATUS;
    std::memcpy(frame + sizeof(DzFrameHeader), &status, sizeof(status));
    router_.dispatch(shm::FrameView(frame));

    const auto body = body_of(invoke_stats());
    EXPECT_TRUE(body["available"].get<bool>());
    EXPECT_TRUE(body["trading_day"].is_null());
    EXPECT_TRUE(body["online_accounts"].empty());
    EXPECT_EQ(body["stats"]["total_orders"].get<int64_t>(), 0);
    EXPECT_EQ(body["stats"]["unfilled_orders"].get<int64_t>(), 0);
}

TEST_F(TdDataControllerTest, StatsNotInjectedDoesNotAffectOrdersList) {
    create_db();
    insert_order("ACC1", 1, 32400);
    ready("ACC1", day_epoch(2026, 10, 9));
    // 未注入 TdStatsQuery：统计 not_ready，但委托列表端点完全不受影响。
    EXPECT_EQ(body_of(invoke_stats())["reason"].get<std::string>(), "not_ready");
    const auto resp = invoke();
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->getStatusCode(), drogon::k200OK);
    const auto body = body_of(resp);
    EXPECT_TRUE(body["available"].get<bool>());
    ASSERT_EQ(body["items"].size(), 1U);
}

// ===== GET /api/td/trades（契约 rest §2.9.1/§2.9.2/§2.9.3/§2.9.8）=====

TEST_F(TdDataControllerTest, TradesReturnsEnvelopeWithRowSchema) {
    create_db();
    insert_trade("ACC1", "0000000001", 32400, /*order_id=*/10231);
    ready("ACC1", day_epoch(2026, 10, 9));

    const auto resp = invoke_trades();
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->getStatusCode(), drogon::k200OK);
    const auto body = body_of(resp);
    EXPECT_TRUE(body["available"].get<bool>());
    EXPECT_EQ(body["trading_day"].get<std::string>(), "20261009");
    ASSERT_EQ(body["online_accounts"].size(), 1U);
    EXPECT_EQ(body["online_accounts"][0].get<std::string>(), "ACC1");
    ASSERT_EQ(body["items"].size(), 1U);
    const auto& row = body["items"][0];
    EXPECT_EQ(row["account_id"].get<std::string>(), "ACC1");
    EXPECT_EQ(row["date"].get<std::string>(), "20261009");
    EXPECT_EQ(row["trade_id"].get<std::string>(), "0000000001");
    EXPECT_EQ(row["order_id"].get<int64_t>(), 10231);
    EXPECT_EQ(row["direction"].get<std::string>(), "Long");
    EXPECT_EQ(row["position_effect"].get<std::string>(), "Open");
    EXPECT_EQ(row["time"].get<int>(), 32400);
    EXPECT_TRUE(row["sort_key"].is_string());
    EXPECT_TRUE(body["next_cursor"].is_null());
    // 契约 §2.9.3：成交行没有成交均价字段
    EXPECT_FALSE(row.contains("avg_price"));
}

TEST_F(TdDataControllerTest, TradesReturns400OnInvalidParameters) {
    create_db();
    ready("ACC1", day_epoch(2026, 10, 9));
    const std::map<std::string, std::map<std::string, std::string>> bad_cases = {
        {"limit zero", {{"limit", "0"}}},
        {"limit too large", {{"limit", "1001"}}},
        {"limit non numeric", {{"limit", "abc"}}},
        {"malformed cursor", {{"cursor", "not-a-cursor"}}},
    };
    for (const auto& [label, query] : bad_cases) {
        const auto resp = invoke_trades(query);
        ASSERT_NE(resp, nullptr) << label;
        EXPECT_EQ(resp->getStatusCode(), drogon::k400BadRequest) << label;
        EXPECT_EQ(body_of(resp).value("error", ""), "bad request") << label;
    }
}

TEST_F(TdDataControllerTest, TradesReturns400WhenCursorScopeChanged) {
    create_db();
    for (int i = 0; i < 3; ++i) {
        insert_trade("ACC1", std::format("{:010d}", 1 + i), 32400 + i);
    }
    ready("ACC1", day_epoch(2026, 10, 9));
    const auto first = invoke_trades({{"limit", "1"}});
    ASSERT_NE(first, nullptr);
    ASSERT_EQ(first->getStatusCode(), drogon::k200OK);
    const auto cursor = body_of(first)["next_cursor"].get<std::string>();

    // accounts 作用域变化：游标作废（契约 rest §2.9.2）
    const auto resp = invoke_trades({{"limit", "1"}, {"cursor", cursor}, {"accounts", "OTHER"}});
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->getStatusCode(), drogon::k400BadRequest);
    EXPECT_EQ(body_of(resp).value("error", ""), "bad request");

    // 同一作用域 → 续页正常
    const auto second = invoke_trades({{"limit", "1"}, {"cursor", cursor}});
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(second->getStatusCode(), drogon::k200OK);
}

TEST_F(TdDataControllerTest, TradesDegradesTo200WithReasonWhenDatabaseMissing) {
    ready("ACC1", day_epoch(2026, 10, 9));
    const auto resp = invoke_trades();
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->getStatusCode(), drogon::k200OK);
    const auto body = body_of(resp);
    EXPECT_FALSE(body.value("available", true));
    EXPECT_EQ(body.value("reason", ""), "not_found");
    EXPECT_FALSE(body.contains("items"));
}

TEST_F(TdDataControllerTest, TradesEmptyItemsWhenNoOnlineAccounts) {
    create_db();
    insert_trade("ACC1", "0000000001", 32400);
    // 无 ACCOUNT_STATUS → 无在线账户
    const auto resp = invoke_trades();
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->getStatusCode(), drogon::k200OK);
    const auto body = body_of(resp);
    EXPECT_TRUE(body.value("available", false));
    EXPECT_TRUE(body["trading_day"].is_null());
    EXPECT_TRUE(body["items"].empty());
    EXPECT_TRUE(body["next_cursor"].is_null());
}

TEST_F(TdDataControllerTest, TradesDegradesInsteadOf5xxWhenNotWired) {
    auto ctrl = std::make_shared<TdCtrl>(nullptr, domain_, query_, nullptr, positions_query_);
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Get);
    req->setPath("/api/td/trades");
    drogon::HttpResponsePtr captured;
    ctrl->get_trades(req, [&captured](const drogon::HttpResponsePtr& resp) { captured = resp; });
    ASSERT_NE(captured, nullptr);
    EXPECT_EQ(captured->getStatusCode(), drogon::k200OK);
    EXPECT_FALSE(body_of(captured).value("available", true));
}

TEST_F(TdDataControllerTest, OrdersAndTradesEndpointsAreIndependent) {
    create_db();
    insert_order("ACC1", 7, 32400);
    insert_trade("ACC1", "0000000001", 32401, /*order_id=*/7);
    ready("ACC1", day_epoch(2026, 10, 9));

    // 两个端点各自返回自己的行，互不影响（同一库、同一在线集合）。
    const auto orders = body_of(invoke());
    const auto trades = body_of(invoke_trades());
    ASSERT_EQ(orders["items"].size(), 1U);
    ASSERT_EQ(trades["items"].size(), 1U);
    EXPECT_TRUE(orders["items"][0].contains("order_id"));
    EXPECT_TRUE(orders["items"][0].contains("avg_price"));
    EXPECT_FALSE(trades["items"][0].contains("avg_price"));
    EXPECT_EQ(trades["items"][0]["order_id"].get<int64_t>(), 7);
    // 委托游标不能用于成交端点（端点参与游标作用域，契约 §2.9.2）
    for (int i = 0; i < 3; ++i) {
        insert_order("ACC1", 100 + i, 32500 + i);
    }
    const auto orders_page = body_of(invoke({{"limit", "1"}}));
    ASSERT_FALSE(orders_page["next_cursor"].is_null());
    const auto cross =
        invoke_trades({{"limit", "1"}, {"cursor", orders_page["next_cursor"].get<std::string>()}});
    ASSERT_NE(cross, nullptr);
    EXPECT_EQ(cross->getStatusCode(), drogon::k400BadRequest);
}

// ===== GET /api/td/positions（契约 rest §2.9.1/§2.9.2/§2.9.4/§2.9.8）=====

TEST_F(TdDataControllerTest, PositionsReturnsOfflineAccountsToo) {
    create_db();
    // ACC2 从不 Ready（离线），但库里有持仓行 → 必须出现在持仓列表（契约 §2.9.1）。
    insert_position("ACC1", "rb2601", 2, 1);
    insert_position("ACC2", "rb2610", 3, 2);
    ready("ACC1", day_epoch(2026, 10, 9));

    const auto resp = invoke_positions();
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->getStatusCode(), drogon::k200OK);
    const auto body = body_of(resp);
    EXPECT_TRUE(body["available"].get<bool>());
    EXPECT_EQ(body["items"].size(), 2U);
}

TEST_F(TdDataControllerTest, PositionsRowHasNoSortKeyAndCarriesAnchorDate) {
    create_db();
    insert_position("ACC1", "rb2601", 2, 7);
    ready("ACC1", day_epoch(2026, 10, 9));

    const auto body = body_of(invoke_positions());
    ASSERT_EQ(body["items"].size(), 1U);
    const auto& row = body["items"][0];
    EXPECT_EQ(row["account_id"].get<std::string>(), "ACC1");
    EXPECT_EQ(row["instrument_id"].get<std::string>(), "rb2601");
    EXPECT_EQ(row["date"].get<std::string>(), "20261009") << "row date is the account anchor day";
    EXPECT_EQ(row["direction"].get<std::string>(), "Long");
    EXPECT_EQ(row["volume"].get<int>(), 2);
    EXPECT_EQ(row["seq"].get<uint64_t>(), 7U);
    EXPECT_FALSE(row.contains("sort_key")) << "positions have no time-ordered sort key";
}

TEST_F(TdDataControllerTest, PositionsReturns400OnInvalidParameters) {
    create_db();
    insert_position("ACC1", "rb2601", 2, 1);
    ready("ACC1", day_epoch(2026, 10, 9));
    const std::map<std::string, std::map<std::string, std::string>> bad_cases = {
        {"limit zero", {{"limit", "0"}}},
        {"limit too large", {{"limit", "1001"}}},
        {"malformed cursor", {{"cursor", "not-a-cursor"}}},
    };
    for (const auto& [label, query] : bad_cases) {
        const auto resp = invoke_positions(query);
        ASSERT_NE(resp, nullptr) << label;
        EXPECT_EQ(resp->getStatusCode(), drogon::k400BadRequest) << label;
        EXPECT_EQ(body_of(resp).value("error", ""), "bad request") << label;
    }
}

TEST_F(TdDataControllerTest, PositionsDegradesTo200WhenDatabaseMissing) {
    ready("ACC1", day_epoch(2026, 10, 9));
    const auto resp = invoke_positions();
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->getStatusCode(), drogon::k200OK);
    const auto body = body_of(resp);
    EXPECT_FALSE(body["available"].get<bool>());
    EXPECT_EQ(body["reason"].get<std::string>(), "not_found");
    EXPECT_FALSE(body.contains("items"));
}

TEST_F(TdDataControllerTest, PositionsWithoutWiredQueryDegradesInsteadOf5xx) {
    // 装配缺失（测试/异常装配）：读端点不上 5xx（契约 rest §2.9.8）——持仓的 contract
    // 与委托/成交同型，用 query_failed 表达。
    auto ctrl = std::make_shared<TdCtrl>(nullptr, domain_, query_, trades_query_, nullptr);
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Get);
    req->setPath("/api/td/positions");
    drogon::HttpResponsePtr captured;
    ctrl->get_positions(req, [&captured](const drogon::HttpResponsePtr& resp) { captured = resp; });
    ASSERT_NE(captured, nullptr);
    EXPECT_EQ(captured->getStatusCode(), drogon::k200OK);
    const auto body = body_of(captured);
    EXPECT_FALSE(body.value("available", true));
    EXPECT_EQ(body.value("reason", ""), "query_failed");
}

TEST_F(TdDataControllerTest, PositionsCursorIsRejectedByOrdersEndpoint) {
    create_db();
    for (int i = 0; i < 3; ++i) {
        insert_position("ACC1", std::format("rb260{}", i), 2, static_cast<uint64_t>(i + 1));
    }
    ready("ACC1", day_epoch(2026, 10, 9));
    const auto page = body_of(invoke_positions({{"limit", "1"}}));
    ASSERT_FALSE(page["next_cursor"].is_null());

    // 持仓游标拿到委托端点 → 作用域不符 → 400（契约 rest §2.9.2）。
    const auto cross = invoke({{"limit", "1"}, {"cursor", page["next_cursor"].get<std::string>()}});
    ASSERT_NE(cross, nullptr);
    EXPECT_EQ(cross->getStatusCode(), drogon::k400BadRequest);
}

}  // namespace
}  // namespace dztrader::webui
