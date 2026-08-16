#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include "log_controller.h"
#include "repository.h"
#include "config.h"
#include "mirror_store.h"
#include "ws_broadcaster.h"
#include <dztrader/core/path.h>
#include <dztrader/platform/log_config.h>

namespace fs = std::filesystem;
// 注意：不引入全局 Json 别名——drogon 依赖 jsoncpp，其 namespace Json 在全局作用域，
// 全局 using Json = nlohmann::json 会与之冲突（MSVC C2365）
using dztrader::webui::LogCtrl;

namespace {
/// 最小 WsBroadcaster 桩（domain_service_test.cpp 同款）：LogDomainService 构造依赖
class FakeBroadcaster : public dztrader::webui::WsBroadcaster {
public:
    void broadcast(const std::string&, const std::string&,
                   const nlohmann::json&) override {}
};
}  // namespace

class LogControllerTest : public ::testing::Test {
protected:
    std::shared_ptr<dztrader::webui::Repository> repo_;
    dztrader::webui::WebuiConfig cfg_;
    std::shared_ptr<dztrader::platform::LogConfig> self_log_;
    std::shared_ptr<dztrader::webui::MirrorStore> mirror_;
    std::shared_ptr<FakeBroadcaster> ws_;
    std::shared_ptr<dztrader::webui::LogDomainService> log_domain_;
    std::shared_ptr<LogCtrl> ctrl_;
    int64_t admin_id_ = 0;

    void SetUp() override {
        repo_ = std::make_shared<dztrader::webui::Repository>(":memory:");
        // Seed admin user (Repository::create_user takes individual fields, not a User struct)
        admin_id_ = repo_->create_user("admin", "Administrator", "admin@test.com", "test", "admin");

        // P4 Task 6：LogCtrl 构造注入 LogDomainService（归并 log_control 后
        // set_level/flush 经 handle_log_control 分发）。测试给 disposable self_log
        //（构造即含默认配置, 不 load()）和 nullptr event_writer。
        self_log_ = std::make_shared<dztrader::platform::LogConfig>("dzweb_test", ":memory:");
        mirror_ = std::make_shared<dztrader::webui::MirrorStore>();
        ws_ = std::make_shared<FakeBroadcaster>();
        log_domain_ = std::make_shared<dztrader::webui::LogDomainService>(*mirror_, *ws_,
                                                                          *self_log_, nullptr);
        ctrl_ = std::make_shared<LogCtrl>(repo_, log_domain_);
    }

    drogon::HttpRequestPtr admin_req() {
        auto req = drogon::HttpRequest::newHttpRequest();
        req->getAttributes()->insert("user_id", std::string("admin"));
        return req;
    }

    drogon::HttpRequestPtr user_req() {
        auto req = drogon::HttpRequest::newHttpRequest();
        req->getAttributes()->insert("user_id", std::string("regular"));
        return req;
    }

    template <typename Fn, typename... Args>
    drogon::HttpResponsePtr invoke(Fn fn, const drogon::HttpRequestPtr& req, Args&&... args) {
        drogon::HttpResponsePtr captured;
        (ctrl_.get()->*fn)(
            req, [&captured](const drogon::HttpResponsePtr& r) { captured = r; },
            std::forward<Args>(args)...);
        return captured;
    }

    nlohmann::json parse_resp(const drogon::HttpResponsePtr& resp) {
        return nlohmann::json::parse(resp->getBody());
    }
};

TEST_F(LogControllerTest, GetFilesReturnsList) {
    // Write a test log file to paths::logs()
    auto log_dir = dztrader::paths::logs();
    std::ofstream ofs(log_dir / "test_logctrl.log");
    ofs << "2026-07-13T14:23:45.000000000+08:00 info test_logctrl [func=main file=main.cpp:1 pid=1 tid=2] test\n";
    ofs.close();

    auto req = admin_req();
    auto resp = invoke(&LogCtrl::get_files, req);
    EXPECT_EQ(resp->getStatusCode(), drogon::k200OK);
    auto body = parse_resp(resp);
    EXPECT_TRUE(body.is_array());
    // Clean up
    fs::remove(log_dir / "test_logctrl.log");
}

TEST_F(LogControllerTest, GetContentReturnsLines) {
    auto log_dir = dztrader::paths::logs();
    std::ofstream ofs(log_dir / "test_content.log");
    ofs << "2026-07-13T14:23:45.000000000+08:00 info test_content [func=main file=main.cpp:1 pid=1 tid=2] hello\n";
    ofs.close();

    auto req = admin_req();
    // drogon HttpRequest::setPath does not parse query strings; use setParameter.
    req->setParameter("file", "test_content.log");
    req->setParameter("limit", "500");
    auto resp = invoke(&LogCtrl::get_content, req);
    EXPECT_EQ(resp->getStatusCode(), drogon::k200OK);

    fs::remove(log_dir / "test_content.log");
}

TEST_F(LogControllerTest, GetStatsReturnsCounts) {
    auto log_dir = dztrader::paths::logs();
    std::ofstream ofs(log_dir / "test_stats.log");
    ofs << "2026-07-13T14:23:45.000000000+08:00 info test_stats [func=m file=f.cpp:1 pid=1 tid=2] a\n";
    ofs << "2026-07-13T14:23:46.000000000+08:00 warning test_stats [func=m file=f.cpp:2 pid=1 tid=3] b\n";
    ofs.close();

    auto req = admin_req();
    req->setParameter("file", "test_stats.log");
    auto resp = invoke(&LogCtrl::get_stats, req);
    EXPECT_EQ(resp->getStatusCode(), drogon::k200OK);

    fs::remove(log_dir / "test_stats.log");
}

TEST_F(LogControllerTest, PostLevelRequiresAdmin) {
    auto req = user_req();  // non-admin
    req->setBody(R"({"targets":["dztraderd"],"level":"warning"})");
    auto resp = invoke(&LogCtrl::set_level, req);
    EXPECT_EQ(resp->getStatusCode(), drogon::k403Forbidden);
}

TEST_F(LogControllerTest, PostLevelRejectsInvalidLevel) {
    auto req = admin_req();
    req->setBody(R"({"targets":["dztraderd"],"level":"INVALID"})");
    auto resp = invoke(&LogCtrl::set_level, req);
    EXPECT_EQ(resp->getStatusCode(), drogon::k400BadRequest);
}

// 契约 rest §1: 响应 ok 表示"已写事件通道"。event_writer 为空（API-only 模式）
// 时帧未写入，ok 必须为 false--否则前端会把"从未下发"当作"已下发"。
TEST_F(LogControllerTest, PostLevelReturnsNullOldForUnknownTarget) {
    auto req = admin_req();
    req->setBody(R"({"targets":["dzmd_ctp"],"level":"warning"})");
    auto resp = invoke(&LogCtrl::set_level, req);
    EXPECT_EQ(resp->getStatusCode(), drogon::k200OK);
    auto body = parse_resp(resp);
    ASSERT_TRUE(body.contains("results"));
    ASSERT_EQ(body["results"].size(), 1u);
    EXPECT_EQ(body["results"][0]["name"], "dzmd_ctp");
    // old 必须是 JSON null（不是字符串 "null" 也不是崩溃）
    EXPECT_TRUE(body["results"][0]["old"].is_null());
    EXPECT_EQ(body["results"][0]["new"], "warning");
    // fixture 中 event_writer_ 为 nullptr: 帧未写入, ok=false
    EXPECT_FALSE(body["results"][0]["ok"].get<bool>());
}

// flush 同理: writer 缺失时 ok=false（FLUSH 无 RTN, 这是唯一的结果通道）
TEST_F(LogControllerTest, PostFlushReportsFailureWhenWriterUnavailable) {
    auto req = admin_req();
    req->setBody(R"({"targets":["dzmd_ctp"]})");
    auto resp = invoke(&LogCtrl::flush_log, req);
    EXPECT_EQ(resp->getStatusCode(), drogon::k200OK);
    auto body = parse_resp(resp);
    EXPECT_FALSE(body["results"][0]["ok"].get<bool>());
}

TEST_F(LogControllerTest, PostFlushRequiresAdmin) {
    auto req = user_req();
    req->setBody(R"({"targets":["dztraderd"]})");
    auto resp = invoke(&LogCtrl::flush_log, req);
    EXPECT_EQ(resp->getStatusCode(), drogon::k403Forbidden);
}

// I3 整型参数保护：非数字 limit/offset 必须回 400，禁止 std::stoi 抛异常逃逸 handler
TEST_F(LogControllerTest, GetFilesRejectsNonNumericLimit) {
    auto req = admin_req();
    req->setParameter("limit", "abc");
    auto resp = invoke(&LogCtrl::get_files, req);
    EXPECT_EQ(resp->getStatusCode(), drogon::k400BadRequest);
}

TEST_F(LogControllerTest, GetFilesRejectsNonNumericOffset) {
    auto req = admin_req();
    req->setParameter("offset", "xyz");
    auto resp = invoke(&LogCtrl::get_files, req);
    EXPECT_EQ(resp->getStatusCode(), drogon::k400BadRequest);
}

// 合法数字回归：?limit=10 应为 200
TEST_F(LogControllerTest, GetFilesAcceptsNumericLimit) {
    auto req = admin_req();
    req->setParameter("limit", "10");
    auto resp = invoke(&LogCtrl::get_files, req);
    EXPECT_EQ(resp->getStatusCode(), drogon::k200OK);
}

// get_content 的 limit 同样要隔离；需带 file 参数
TEST_F(LogControllerTest, GetContentRejectsNonNumericLimit) {
    auto log_dir = dztrader::paths::logs();
    std::ofstream ofs(log_dir / "test_content_limit.log");
    ofs << "2026-07-13T14:23:45.000000000+08:00 info test_content [func=m file=f.cpp:1 pid=1 tid=2] a\n";
    ofs.close();

    auto req = admin_req();
    req->setParameter("file", "test_content_limit.log");
    req->setParameter("limit", "abc");
    auto resp = invoke(&LogCtrl::get_content, req);
    EXPECT_EQ(resp->getStatusCode(), drogon::k400BadRequest);

    fs::remove(log_dir / "test_content_limit.log");
}

// get_aggregate 的 limit 同样要隔离；需带 file 参数
TEST_F(LogControllerTest, GetAggregateRejectsNonNumericLimit) {
    auto log_dir = dztrader::paths::logs();
    std::ofstream ofs(log_dir / "test_agg_limit.log");
    ofs << "2026-07-13T14:23:45.000000000+08:00 error test_agg [func=m file=f.cpp:1 pid=1 tid=2] boom\n";
    ofs.close();

    auto req = admin_req();
    req->setParameter("file", "test_agg_limit.log");
    req->setParameter("limit", "def");
    auto resp = invoke(&LogCtrl::get_aggregate, req);
    EXPECT_EQ(resp->getStatusCode(), drogon::k400BadRequest);

    fs::remove(log_dir / "test_agg_limit.log");
}
