#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include "log_controller.h"
#include "repository.h"
#include "config.h"
#include "mirror_store.h"
#include "unique_temp.h"
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
    void broadcast(const std::string&, const std::string&, const nlohmann::json&) override {}
};

/// 唯一化日志文件名（§3.5）：ctest -j 下用例为独立进程，固定名会与并发用例冲突
std::string unique_log(const std::string& base) {
    return base + "_" + dztrader::webui::test::unique_suffix() + ".log";
}
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
        // （构造即含默认配置, 不 load()）和 nullptr event_writer。
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
    const std::string file_name = unique_log("test_logctrl");
    const std::string content =
        "2026-07-13T14:23:45.000000000+08:00 info test_logctrl [func=main file=main.cpp:1 pid=1 "
        "tid=2] test\n";
    std::ofstream ofs(log_dir / file_name);
    ofs << content;
    ofs.close();

    auto req = admin_req();
    auto resp = invoke(&LogCtrl::get_files, req);
    EXPECT_EQ(resp->getStatusCode(), drogon::k200OK);
    auto body = parse_resp(resp);
    ASSERT_TRUE(body.is_array());

    // 刚写入的文件必须被列出，且 size/mtime 正确（文件未被列出或 ofstream 打开失败必红）
    bool found = false;
    for (const auto& f : body) {
        if (f.value("name", "") == file_name) {
            found = true;
            EXPECT_EQ(f.value("logger", ""), file_name.substr(0, file_name.size() - 4));
            EXPECT_EQ(f["size"].get<uintmax_t>(), content.size());
            EXPECT_FALSE(f.value("mtime", "").empty());
        }
    }
    EXPECT_TRUE(found) << file_name << " 应出现在文件列表中";

    // Clean up
    fs::remove(log_dir / file_name);
}

TEST_F(LogControllerTest, GetContentReturnsLines) {
    auto log_dir = dztrader::paths::logs();
    const std::string file_name = unique_log("test_content");
    std::ofstream ofs(log_dir / file_name);
    ofs << "2026-07-13T14:23:45.000000000+08:00 info test_content [func=main file=main.cpp:1 pid=1 "
           "tid=2] hello\n";
    ofs.close();

    auto req = admin_req();
    // drogon HttpRequest::setPath does not parse query strings; use setParameter.
    req->setParameter("file", file_name);
    req->setParameter("limit", "500");
    auto resp = invoke(&LogCtrl::get_content, req);
    ASSERT_EQ(resp->getStatusCode(), drogon::k200OK);

    // 解析 body：行数/内容/total 必须正确（行内容丢失或 total 错误必红）
    auto body = parse_resp(resp);
    ASSERT_EQ(body["lines"].size(), 1u);
    EXPECT_EQ(body["lines"][0]["n"].get<int>(), 1);
    EXPECT_EQ(body["lines"][0]["level"].get<std::string>(), "info");
    EXPECT_EQ(body["lines"][0]["logger"].get<std::string>(), "test_content");
    EXPECT_EQ(body["lines"][0]["msg"].get<std::string>(), "hello");
    EXPECT_EQ(body["total"].get<int>(), 1);

    fs::remove(log_dir / file_name);
}

TEST_F(LogControllerTest, GetStatsReturnsCounts) {
    auto log_dir = dztrader::paths::logs();
    const std::string file_name = unique_log("test_stats");
    std::ofstream ofs(log_dir / file_name);
    ofs << "2026-07-13T14:23:45.000000000+08:00 info test_stats [func=m file=f.cpp:1 pid=1 tid=2] "
           "a\n";
    ofs << "2026-07-13T14:23:46.000000000+08:00 warning test_stats [func=m file=f.cpp:2 pid=1 "
           "tid=3] b\n";
    ofs.close();

    auto req = admin_req();
    req->setParameter("file", file_name);
    auto resp = invoke(&LogCtrl::get_stats, req);
    ASSERT_EQ(resp->getStatusCode(), drogon::k200OK);

    auto body = parse_resp(resp);
    EXPECT_EQ(body["total"].get<int>(), 2);
    ASSERT_EQ(body["by_level"].size(), 2u);
    EXPECT_EQ(body["by_level"]["info"].get<int>(), 1);
    EXPECT_EQ(body["by_level"]["warning"].get<int>(), 1);
    EXPECT_FALSE(body["timespan"].get<std::string>().empty());

    fs::remove(log_dir / file_name);
}

// ---- 契约 rest §2.5: timeline / aggregate ----

TEST_F(LogControllerTest, GetTimelineReturnsBuckets) {
    auto log_dir = dztrader::paths::logs();
    const std::string file_name = unique_log("test_timeline");
    std::ofstream ofs(log_dir / file_name);
    ofs << "2026-07-13T14:23:45.000000000+08:00 info test_tl [func=m file=f.cpp:1 pid=1 tid=2] "
           "a\n";
    ofs << "2026-07-13T14:23:46.000000000+08:00 warning test_tl [func=m file=f.cpp:2 pid=1 "
           "tid=3] b\n";
    ofs << "2026-07-13T14:25:00.000000000+08:00 error test_tl [func=m file=f.cpp:3 pid=1 tid=4] "
           "c\n";
    ofs.close();

    auto req = admin_req();
    req->setParameter("file", file_name);
    req->setParameter("bucket", "minute");
    auto resp = invoke(&LogCtrl::get_timeline, req);
    ASSERT_EQ(resp->getStatusCode(), drogon::k200OK);

    auto body = parse_resp(resp);
    ASSERT_EQ(body.size(), 2u);  // 14:23 一桶（info+warning）、14:25 一桶（error）
    EXPECT_EQ(body[0]["counts"]["info"].get<int>(), 1);
    EXPECT_EQ(body[0]["counts"]["warning"].get<int>(), 1);
    EXPECT_EQ(body[1]["counts"]["error"].get<int>(), 1);
    EXPECT_FALSE(body[0]["ts"].get<std::string>().empty());

    fs::remove(log_dir / file_name);
}

TEST_F(LogControllerTest, GetTimelineMissingFileReturns400) {
    auto req = admin_req();
    auto resp = invoke(&LogCtrl::get_timeline, req);
    EXPECT_EQ(resp->getStatusCode(), drogon::k400BadRequest);
}

TEST_F(LogControllerTest, GetAggregateReturnsGroups) {
    auto log_dir = dztrader::paths::logs();
    const std::string file_name = unique_log("test_agg_ok");
    std::ofstream ofs(log_dir / file_name);
    ofs << "2026-07-13T14:23:45.000000000+08:00 error test_agg [func=m file=f.cpp:1 pid=1 tid=2] "
           "connect failed\n";
    ofs << "2026-07-13T14:23:46.000000000+08:00 error test_agg [func=m file=f.cpp:2 pid=1 tid=3] "
           "connect failed\n";
    ofs << "2026-07-13T14:23:47.000000000+08:00 error test_agg [func=m file=f.cpp:3 pid=1 tid=4] "
           "other failure\n";
    ofs.close();

    auto req = admin_req();
    req->setParameter("file", file_name);
    auto resp = invoke(&LogCtrl::get_aggregate, req);
    ASSERT_EQ(resp->getStatusCode(), drogon::k200OK);

    auto body = parse_resp(resp);
    ASSERT_TRUE(body.is_array());
    ASSERT_GE(body.size(), 2u);
    // 按 count 降序：相同消息的两条聚为一组
    EXPECT_EQ(body[0]["count"].get<int>(), 2);
    EXPECT_FALSE(body[0]["msg_pattern"].get<std::string>().empty());
    EXPECT_EQ(body[0]["samples"].size(), 2u);
    EXPECT_FALSE(body[0]["first_ts"].get<std::string>().empty());
    EXPECT_FALSE(body[0]["last_ts"].get<std::string>().empty());

    fs::remove(log_dir / file_name);
}

TEST_F(LogControllerTest, MissingFileParameterReturns400) {
    // content/stats/aggregate 缺 file -> 400（各分支独立）
    EXPECT_EQ(invoke(&LogCtrl::get_content, admin_req())->getStatusCode(), drogon::k400BadRequest);
    EXPECT_EQ(invoke(&LogCtrl::get_stats, admin_req())->getStatusCode(), drogon::k400BadRequest);
    EXPECT_EQ(invoke(&LogCtrl::get_aggregate, admin_req())->getStatusCode(),
              drogon::k400BadRequest);
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

// 契约 log §SET_LOG_CONFIG: warn/err 为输入别名，接受并规范化为 warning/error
TEST_F(LogControllerTest, PostLevelNormalizesWarnAndErrAliases) {
    auto req = admin_req();
    req->setBody(R"({"targets":["dzmd_ctp"],"level":"warn"})");
    auto resp = invoke(&LogCtrl::set_level, req);
    ASSERT_EQ(resp->getStatusCode(), drogon::k200OK);
    auto body = parse_resp(resp);
    ASSERT_EQ(body["results"].size(), 1u);
    EXPECT_EQ(body["results"][0]["new"].get<std::string>(), "warning");

    auto req2 = admin_req();
    req2->setBody(R"({"targets":["dzmd_ctp"],"level":"err"})");
    auto resp2 = invoke(&LogCtrl::set_level, req2);
    ASSERT_EQ(resp2->getStatusCode(), drogon::k200OK);
    EXPECT_EQ(parse_resp(resp2)["results"][0]["new"].get<std::string>(), "error");
}

TEST_F(LogControllerTest, PostLevelRejectsEmptyTargets) {
    auto req = admin_req();
    req->setBody(R"({"targets":[],"level":"warning"})");
    EXPECT_EQ(invoke(&LogCtrl::set_level, req)->getStatusCode(), drogon::k400BadRequest);
    auto req2 = admin_req();
    req2->setBody(R"({"level":"warning"})");
    EXPECT_EQ(invoke(&LogCtrl::set_level, req2)->getStatusCode(), drogon::k400BadRequest);
}

TEST_F(LogControllerTest, PostFlushRejectsEmptyTargets) {
    auto req = admin_req();
    req->setBody(R"({"targets":[]})");
    EXPECT_EQ(invoke(&LogCtrl::flush_log, req)->getStatusCode(), drogon::k400BadRequest);
}

TEST_F(LogControllerTest, PostLogEndpointsRejectInvalidJson) {
    auto lvl = admin_req();
    lvl->setBody("{not json");
    EXPECT_EQ(invoke(&LogCtrl::set_level, lvl)->getStatusCode(), drogon::k400BadRequest);
    auto fl = admin_req();
    fl->setBody("{not json");
    EXPECT_EQ(invoke(&LogCtrl::flush_log, fl)->getStatusCode(), drogon::k400BadRequest);
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
    const std::string file_name = unique_log("test_content_limit");
    std::ofstream ofs(log_dir / file_name);
    ofs << "2026-07-13T14:23:45.000000000+08:00 info test_content [func=m file=f.cpp:1 pid=1 "
           "tid=2] a\n";
    ofs.close();

    auto req = admin_req();
    req->setParameter("file", file_name);
    req->setParameter("limit", "abc");
    auto resp = invoke(&LogCtrl::get_content, req);
    EXPECT_EQ(resp->getStatusCode(), drogon::k400BadRequest);

    fs::remove(log_dir / file_name);
}

// get_content 的 offset 同样要隔离；需带 file 参数
TEST_F(LogControllerTest, GetContentRejectsNonNumericOffset) {
    auto log_dir = dztrader::paths::logs();
    const std::string file_name = unique_log("test_content_offset");
    std::ofstream ofs(log_dir / file_name);
    ofs << "2026-07-13T14:23:45.000000000+08:00 info test_content [func=m file=f.cpp:1 pid=1 "
           "tid=2] a\n";
    ofs.close();

    auto req = admin_req();
    req->setParameter("file", file_name);
    req->setParameter("offset", "abc");
    auto resp = invoke(&LogCtrl::get_content, req);
    EXPECT_EQ(resp->getStatusCode(), drogon::k400BadRequest);

    fs::remove(log_dir / file_name);
}

// get_aggregate 的 limit 同样要隔离；需带 file 参数
TEST_F(LogControllerTest, GetAggregateRejectsNonNumericLimit) {
    auto log_dir = dztrader::paths::logs();
    const std::string file_name = unique_log("test_agg_limit");
    std::ofstream ofs(log_dir / file_name);
    ofs << "2026-07-13T14:23:45.000000000+08:00 error test_agg [func=m file=f.cpp:1 pid=1 tid=2] "
           "boom\n";
    ofs.close();

    auto req = admin_req();
    req->setParameter("file", file_name);
    req->setParameter("limit", "def");
    auto resp = invoke(&LogCtrl::get_aggregate, req);
    EXPECT_EQ(resp->getStatusCode(), drogon::k400BadRequest);

    fs::remove(log_dir / file_name);
}
