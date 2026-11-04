/**
 * @file td_data_controller_test.cpp
 * @brief `GET /api/td/orders` REST handler（契约 rest §2.9.1/§2.9.2/§2.9.8）。
 *
 * 真 handler 直调 + 捕获响应（与 sysmon_controller_test 同型）；库用真 schema 建库。
 * 断言 HTTP 语义：200 信封、400 bad request、降级 200 available:false。
 */

#include "td_data_controller.h"
#include "fake_data_change_notifier.h"
#include "td_domain_service.h"
#include "td_orders_query.h"
#include "test_temp_dir.h"

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
    std::shared_ptr<TdCtrl> ctrl_;

    void SetUp() override {
        dir_ = dztrader::test::unique_temp_dir("dz_webui_td_data_controller_test");
        std::filesystem::create_directories(dir_);
        db_path_ = (dir_ / "trade.db").string();
        domain_ = std::make_shared<TdDomainService>(router_, ws_, notifier_);
        domain_->set_trade_db_path_provider([this]() { return db_path_; });
        query_ = std::make_shared<TdOrdersQuery>([this]() { return db_path_; }, 200);
        // 读端点不需要 Repository（认证在 JWT advice 层）；nullptr 与生产许可一致。
        ctrl_ = std::make_shared<TdCtrl>(nullptr, domain_, query_);
    }

    void TearDown() override {
        ctrl_.reset();
        query_.reset();
        domain_.reset();
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    void create_db() { (void)dztrader::tdstore::ensure_trade_db(db_path_); }

    void insert_order(const std::string& account_id, int64_t order_id, int32_t time_sec) {
        sqlite3* db = nullptr;
        ASSERT_EQ(SQLITE_OK, sqlite3_open(db_path_.c_str(), &db));
        sqlite3_stmt* stmt = nullptr;
        const char* sql =
            "INSERT INTO orders (account_id, date, order_id, strategy_id, instrument_id,"
            " exchange_id, direction, position_effect, price_type, status, price, volume,"
            " volume_traded, remark, time, avg_price, seq, gateway, ext)"
            " VALUES (?, '20261009', ?, 'stg1', 'rb2601', 'SHFE', 1, 1, 0, 2, 3500.0, 1, 0,"
            " '', ?, 0.0, 1, 'dztd_ctp', '{}')";
        ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr));
        sqlite3_bind_text(stmt, 1, account_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 2, order_id);
        sqlite3_bind_int(stmt, 3, time_sec);
        EXPECT_EQ(SQLITE_DONE, sqlite3_step(stmt));
        sqlite3_finalize(stmt);
        sqlite3_close(db);
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
    auto ctrl = std::make_shared<TdCtrl>(nullptr, nullptr, nullptr);
    drogon::HttpResponsePtr captured;
    ctrl->get_orders(request(),
                     [&captured](const drogon::HttpResponsePtr& resp) { captured = resp; });
    ASSERT_NE(captured, nullptr);
    EXPECT_EQ(captured->getStatusCode(), drogon::k200OK);
    EXPECT_FALSE(body_of(captured).value("available", true));
}

}  // namespace
}  // namespace dztrader::webui
