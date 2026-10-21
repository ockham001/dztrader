#include <gtest/gtest.h>

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <nlohmann/json.hpp>

#include "mirror_store.h"
#include "repository.h"
#include "sysmon_controller.h"

#include <memory>

// 注意：不引入全局 Json 别名——drogon 依赖 jsoncpp，其 namespace Json 在全局作用域，
// 全局 using Json = nlohmann::json 会与之冲突（MSVC C2365）
using dztrader::webui::MirrorStore;
using dztrader::webui::Repository;
using dztrader::webui::SysmonCtrl;

namespace {

nlohmann::json minimal_snapshot() {
    return {
        {"source", "dzsysmon"},
        {"timestamp", 1790000005000},
        {"sampling",
         {{"measure_interval_ms", 1000},
          {"last_sample_unix_ms", 1790000005123},
          {"missed_ticks", 0}}},
        {"platform",
         {{"family", "linux"},
          {"version", "Ubuntu 24.04.1 LTS"},
          {"kernel", "6.8.0-45-generic"},
          {"arch", "x86_64"},
          {"build", 0}}},
        {"system",
         {{"hostname", "trade-01"},
          {"cpu_model", "test cpu"},
          {"logical_cores", 8},
          {"boot_time_unix_sec", 1789990000},
          {"uptime_sec", 10005}}},
        {"cpu",
         {{"total", {{"usage_ratio", 0.25}}},
          {"cores", nlohmann::json::array()},
          {"saturated_cores", nlohmann::json::array()},
          {"load_average", nullptr},
          {"context_switches_per_sec", nullptr},
          {"interrupts_per_sec", nullptr}}},
        {"memory", nullptr},
        {"swap", nullptr},
        {"volumes", nullptr},
        {"disk_io", nullptr},
        {"network", nullptr},
        {"shm_dir", nullptr},
        {"availability",
         {{"platform_description", "Ubuntu 24.04.1 LTS (kernel 6.8.0-45-generic)"},
          {"metrics",
           {{"system", {{"available", true}}},
            {"cpu.total", {{"available", true}}},
            {"cpu.cores", {{"available", false}, {"reason", "not_implemented"}}},
            {"cpu.load_average", {{"available", false}, {"reason", "not_implemented"}}},
            {"cpu.context_switches", {{"available", false}, {"reason", "not_implemented"}}},
            {"memory", {{"available", false}, {"reason", "not_implemented"}}},
            {"swap", {{"available", false}, {"reason", "not_implemented"}}},
            {"volumes", {{"available", false}, {"reason", "not_implemented"}}},
            {"disk_io", {{"available", false}, {"reason", "not_implemented"}}},
            {"network", {{"available", false}, {"reason", "not_implemented"}}},
            {"shm_dir", {{"available", false}, {"reason", "not_implemented"}}}}},
          {"semantic_hints", nlohmann::json::object()}}},
    };
}

class SysmonControllerTest : public ::testing::Test {
protected:
    void SetUp() override {
        repo_ = std::make_shared<Repository>(":memory:");
        mirror_ = std::make_shared<MirrorStore>();
        ctrl_ = std::make_shared<SysmonCtrl>(repo_, mirror_);
    }

    drogon::HttpRequestPtr req() {
        auto req = drogon::HttpRequest::newHttpRequest();
        req->getAttributes()->insert("user_id", std::string("admin"));
        return req;
    }

    drogon::HttpResponsePtr invoke() {
        drogon::HttpResponsePtr captured;
        ctrl_->get_snapshot(req(), [&captured](const drogon::HttpResponsePtr& r) { captured = r; });
        return captured;
    }

    std::shared_ptr<Repository> repo_;
    std::shared_ptr<MirrorStore> mirror_;
    std::shared_ptr<SysmonCtrl> ctrl_;
};

TEST_F(SysmonControllerTest, ReturnsCurrentSnapshot) {
    const auto snapshot = minimal_snapshot();
    mirror_->update("dzsysmon", "sysmon_snapshot", snapshot);

    auto resp = invoke();
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->getStatusCode(), drogon::k200OK);
    // 用 operator== 布尔比较而非 EXPECT_EQ：避免 gtest 实例化 nlohmann::json 的
    // printer（MSVC /O2 下 gtest-printers.h 会因此触发 C4702，Release /WX 转错误）。
    EXPECT_TRUE(nlohmann::json::parse(resp->getBody()) == snapshot);
}

TEST_F(SysmonControllerTest, Returns503BeforeFirstReport) {
    auto resp = invoke();
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->getStatusCode(), drogon::k503ServiceUnavailable);
    const auto body = nlohmann::json::parse(resp->getBody());
    EXPECT_EQ(body.value("error", ""), "sysmon snapshot not available");
}

}  // namespace
