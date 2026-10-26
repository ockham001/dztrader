#include <gtest/gtest.h>

#include "test_util.h"  // dzshm_test_support：cleanup_channel（内核命名对象回收）

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <nlohmann/json.hpp>

#include "mirror_store.h"
#include "repository.h"
#include "shm_writer.h"
#include "sysmon_controller.h"
#include "sysmon_history_service.h"
#include "test_temp_dir.h"

#include <dztrader/core/this_process.h>
#include <dztrader/platform/sysmon.h>
#include <dztrader/sysmonstore/history_store.h>
#include <dztrader/shm/channel_meta.h>
#include <dztrader/shm/frame_view.h>
#include <dztrader/shm/reader.h>
#include <dztrader/shm/writer.h>

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <string>

// 注意：不引入全局 Json 别名——drogon 依赖 jsoncpp，其 namespace Json 在全局作用域，
// 全局 using Json = nlohmann::json 会与之冲突（MSVC C2365）
using dztrader::webui::MirrorStore;
using dztrader::webui::Repository;
using dztrader::webui::ShmWriter;
using dztrader::webui::SysmonCtrl;

namespace {

using dztrader::shm::ChannelConfig;
using dztrader::shm::ChannelMeta;
using dztrader::shm::FrameView;
using dztrader::shm::MultiWriter;
using dztrader::shm::Reader;

constexpr uint64_t kMB = 1024 * 1024;

/// 进程唯一通道名（PID + 随机后缀）：ctest -j 下同一二进制各用例是独立进程，
/// 固定名会互相 remove_all shm 目录（与 shm_writer_test 同做法）。
std::string unique_channel_name(const std::string& base) {
    std::random_device rd;
    return base + "_" + std::to_string(static_cast<uint32_t>(dztrader::this_process::pid())) + "_" +
           std::to_string(rd());
}

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
        ctrl_ = std::make_shared<SysmonCtrl>(repo_, mirror_, nullptr);
    }

    /// 建一套真实事件通道 + reader，使 ShmWriter::is_ready() 为 true，
    /// 从而能观察 PUT /api/sysmon/config 写出的 SET_SYSMON_CONFIG 帧。
    void enable_shm() {
        channel_name_ = unique_channel_name("dz_test_sysmon_ctrl");
        reader_name_ = channel_name_ + "_reader";
        shm_dir_ = std::filesystem::temp_directory_path() / channel_name_;
        std::filesystem::remove_all(shm_dir_);
        ChannelConfig cfg{
            .channel_name = channel_name_,
            .shm_dir = shm_dir_,
            .meta_file_size = 4 * kMB,
            .page_size = 1 * kMB,
            .lock_memory = false,
            .prefetch_memory = false,
        };
        meta_ = std::make_shared<ChannelMeta>(ChannelMeta::open_or_create(cfg));
        // 先注册 reader 再建 writer：MultiWriter::create 构造时同步订阅列表
        ASSERT_TRUE(meta_->add_reader(reader_name_, 0));
        reader_ = Reader::create(meta_, reader_name_);
        shm_writer_ = std::make_shared<ShmWriter>(
            std::make_shared<MultiWriter>(MultiWriter::create(meta_, channel_name_ + "_writer")));
        ctrl_ = std::make_shared<SysmonCtrl>(repo_, mirror_, shm_writer_);
    }

    void TearDown() override {
        // 先释放映射再删目录：Windows 上已映射文件 remove_all 会失败
        ctrl_.reset();
        shm_writer_.reset();
        reader_.reset();
        meta_.reset();
        if (!channel_name_.empty()) {
            EXPECT_TRUE(
                dztrader::shm::test::cleanup_channel(channel_name_, shm_dir_, {reader_name_}));
        }
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

    drogon::HttpResponsePtr invoke_config() {
        drogon::HttpResponsePtr captured;
        ctrl_->get_config(req(), [&captured](const drogon::HttpResponsePtr& r) { captured = r; });
        return captured;
    }

    /// 排空 reader，返回第一个匹配 type（+ instance_id）的定向帧 payload
    std::optional<nlohmann::json> capture_inst_frame(DzFrameType type,
                                                     const std::string& instance) {
        if (!reader_) return std::nullopt;
        while (const auto* raw = reader_->next_frame()) {
            const FrameView view(raw);
            if (view.type() != type) continue;
            if (std::string(view.ext_inst_id()) != instance) continue;
            const auto* data = reinterpret_cast<const char*>(view.ext_inst_payload());
            return nlohmann::json::parse(data, data + view.ext_inst_payload_size());
        }
        return std::nullopt;
    }

    std::shared_ptr<Repository> repo_;
    std::shared_ptr<MirrorStore> mirror_;
    std::shared_ptr<SysmonCtrl> ctrl_;
    std::string channel_name_;
    std::string reader_name_;
    std::filesystem::path shm_dir_;
    std::shared_ptr<ChannelMeta> meta_;
    std::optional<Reader> reader_;
    std::shared_ptr<ShmWriter> shm_writer_;
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

// ===== GET /api/sysmon/config（《帧契约：系统监控》§6、《帧契约：REST》§2.8）=====

TEST_F(SysmonControllerTest, ConfigReturnsMirroredRuntimeConfig) {
    // 生效值 = RTN_SYSMON_CONFIG 回推写入镜像的 payload；GET 原样透传，不裁剪不归一化
    const nlohmann::json config = dztrader::platform::SysmonConfig::default_config();
    mirror_->update("dzsysmon", "sysmon_config", config);

    const auto resp = invoke_config();
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->getStatusCode(), drogon::k200OK);
    // 用 operator== 布尔比较而非 EXPECT_EQ：避免 gtest 实例化 nlohmann::json 的 printer
    // （MSVC /O2 下会因此触发 C4702，Release /WX 转错误）。
    EXPECT_TRUE(nlohmann::json::parse(resp->getBody()) == config);
}

TEST_F(SysmonControllerTest, Config503BeforeFirstReport) {
    const auto resp = invoke_config();
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->getStatusCode(), drogon::k503ServiceUnavailable);
    const auto body = nlohmann::json::parse(resp->getBody());
    EXPECT_EQ(body.value("error", ""), "sysmon config not available");
}

TEST_F(SysmonControllerTest, Config503WhenSnapshotArrivedButConfigMissing) {
    // 实例已存在（快照先到）但 sysmon_config 域尚未写入：按未就绪回 503，不拿快照顶替
    mirror_->update("dzsysmon", "sysmon_snapshot", minimal_snapshot());
    const auto resp = invoke_config();
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->getStatusCode(), drogon::k503ServiceUnavailable);
    const auto body = nlohmann::json::parse(resp->getBody());
    EXPECT_EQ(body.value("error", ""), "sysmon config not available");
}

// 读端点不做角色判断（认证在 JWT advice 层完成，见 sysmon_controller.h 类注释）：
// 非 admin 可读这条语义不存在 handler 分支，无法也不应由 handler 单测断言。

// ===== PUT /api/sysmon/config（《帧契约：REST》§2.8、《帧契约：系统监控》§3）=====

/// 构造带 user_id 属性与 body 的 PUT 请求（JWT advice 在真实链路里注入 user_id）。
drogon::HttpRequestPtr put_request(const std::string& user_id, const std::string& body) {
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Put);
    req->getAttributes()->insert("user_id", user_id);
    req->setBody(body);
    return req;
}

TEST_F(SysmonControllerTest, SetConfig403ForNonAdmin) {
    repo_->create_user("regular", "Regular User", "reg@test.com", "pass", "user");
    drogon::HttpResponsePtr captured;
    ctrl_->set_config(put_request("regular", R"({"network":{"hide_virtual":false}})"),
                      [&captured](const drogon::HttpResponsePtr& r) { captured = r; });
    ASSERT_NE(captured, nullptr);
    EXPECT_EQ(captured->getStatusCode(), drogon::k403Forbidden);
    EXPECT_EQ(nlohmann::json::parse(captured->getBody()).value("error", ""), "admin required");
}

TEST_F(SysmonControllerTest, SetConfig503WhenShmNotReady) {
    repo_->create_user("admin", "Administrator", "admin@test.com", "pass", "admin");
    // 无 ShmWriter 的控制器（API-only 模式，契约 rest §1 的 shm 未就绪降级）
    auto ctrl = std::make_shared<SysmonCtrl>(repo_, mirror_, nullptr);
    drogon::HttpResponsePtr captured;
    ctrl->set_config(put_request("admin", R"({"network":{"hide_virtual":false}})"),
                     [&captured](const drogon::HttpResponsePtr& r) { captured = r; });
    ASSERT_NE(captured, nullptr);
    EXPECT_EQ(captured->getStatusCode(), drogon::k503ServiceUnavailable);
    EXPECT_EQ(nlohmann::json::parse(captured->getBody()).value("error", ""), "shm not available");
}

TEST_F(SysmonControllerTest, SetConfig400OnInvalidJsonBody) {
    repo_->create_user("admin", "Administrator", "admin@test.com", "pass", "admin");
    enable_shm();
    drogon::HttpResponsePtr captured;
    ctrl_->set_config(put_request("admin", "{not json"),
                      [&captured](const drogon::HttpResponsePtr& r) { captured = r; });
    ASSERT_NE(captured, nullptr);
    EXPECT_EQ(captured->getStatusCode(), drogon::k400BadRequest);
    EXPECT_EQ(nlohmann::json::parse(captured->getBody()).value("error", ""), "bad request");
}

TEST_F(SysmonControllerTest, SetConfig400OnNonObjectBody) {
    repo_->create_user("admin", "Administrator", "admin@test.com", "pass", "admin");
    enable_shm();
    drogon::HttpResponsePtr captured;
    ctrl_->set_config(put_request("admin", "[1,2,3]"),
                      [&captured](const drogon::HttpResponsePtr& r) { captured = r; });
    ASSERT_NE(captured, nullptr);
    EXPECT_EQ(captured->getStatusCode(), drogon::k400BadRequest);
}

TEST_F(SysmonControllerTest, SetConfigDispatchesMergePatchToSysmon) {
    repo_->create_user("admin", "Administrator", "admin@test.com", "pass", "admin");
    enable_shm();
    const nlohmann::json patch = {{"network", {{"hide_virtual", false}}}};
    drogon::HttpResponsePtr captured;
    ctrl_->set_config(put_request("admin", patch.dump()),
                      [&captured](const drogon::HttpResponsePtr& r) { captured = r; });
    ASSERT_NE(captured, nullptr);
    // 契约 rest §1：200 只表示"已写事件通道"，生效信号是 WS sysmon_config
    EXPECT_EQ(captured->getStatusCode(), drogon::k200OK);
    const auto body = nlohmann::json::parse(captured->getBody());
    EXPECT_TRUE(body.value("ok", false));
    EXPECT_EQ(body.value("message", ""), "dispatched");
    // 帧侧：SET_SYSMON_CONFIG 定向到固定实例 dzsysmon，payload 原样透传 merge patch
    const auto frame = capture_inst_frame(DZ_FRAME_SET_SYSMON_CONFIG, "dzsysmon");
    ASSERT_TRUE(frame.has_value());
    EXPECT_TRUE(*frame == patch);
}

TEST_F(SysmonControllerTest, SetConfigEmptyPatchStillDispatched) {
    // 《帧契约：系统监控》§3：空对象 {} 为无操作但仍回 RTN —— REST 侧同样原样下发
    repo_->create_user("admin", "Administrator", "admin@test.com", "pass", "admin");
    enable_shm();
    drogon::HttpResponsePtr captured;
    ctrl_->set_config(put_request("admin", "{}"),
                      [&captured](const drogon::HttpResponsePtr& r) { captured = r; });
    ASSERT_NE(captured, nullptr);
    EXPECT_EQ(captured->getStatusCode(), drogon::k200OK);
    const auto frame = capture_inst_frame(DZ_FRAME_SET_SYSMON_CONFIG, "dzsysmon");
    ASSERT_TRUE(frame.has_value());
    EXPECT_TRUE(frame->is_object());
    EXPECT_TRUE(frame->empty());
}

// ===== GET /api/sysmon/history（《帧契约：系统监控》§6.1、《帧契约：REST》§2.8）=====

/// 构造带 query 参数的 GET 请求（drogon 的 getParameter 从 query 解析）。
drogon::HttpRequestPtr get_request(const std::map<std::string, std::string>& query = {}) {
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Get);
    req->setPath("/api/sysmon/history");
    for (const auto& [key, value] : query) {
        req->setParameter(key, value);
    }
    req->getAttributes()->insert("user_id", std::string("admin"));
    return req;
}

drogon::HttpResponsePtr invoke_history(const std::shared_ptr<SysmonCtrl>& ctrl,
                                       const std::map<std::string, std::string>& query = {}) {
    drogon::HttpResponsePtr captured;
    ctrl->get_history(get_request(query),
                      [&captured](const drogon::HttpResponsePtr& r) { captured = r; });
    return captured;
}

TEST_F(SysmonControllerTest, HistoryReturns400OnInvalidParameters) {
    // 参数非法一律 400（《帧契约：系统监控》§6.1），不静默钳制
    auto ctrl = std::make_shared<SysmonCtrl>(repo_, mirror_, nullptr,
                                             dztrader::test::unique_temp_path("nope", "db"));
    const std::map<std::string, std::map<std::string, std::string>> bad_cases = {
        {"unknown metric", {{"metric", "not_a_metric"}}},
        {"dimension metric without dimension", {{"metric", "network.rx_bytes_total"}}},
        {"scalar metric with dimension", {{"metric", "cpu.usage_ratio"}, {"dimension", "0"}}},
        {"bad level", {{"level", "hour"}}},
        {"bad max_points", {{"max_points", "5"}}},
        {"inverted range", {{"from", "200"}, {"to", "100"}}},
        {"non numeric from", {{"from", "abc"}}},
    };
    for (const auto& [label, query] : bad_cases) {
        const auto resp = invoke_history(ctrl, query);
        ASSERT_NE(resp, nullptr) << label;
        EXPECT_EQ(resp->getStatusCode(), drogon::k400BadRequest) << label;
        EXPECT_EQ(nlohmann::json::parse(resp->getBody()).value("error", ""), "bad request")
            << label;
    }
}

TEST_F(SysmonControllerTest, HistoryDegradesTo200WhenDatabaseMissing) {
    // 历史库缺失 ⇒ 200 + available=false（《帧契约：系统监控》§6.1.3：不崩溃、不 5xx、不伪造）
    auto ctrl = std::make_shared<SysmonCtrl>(
        repo_, mirror_, nullptr, dztrader::test::unique_temp_path("dz_missing_sysmon", "db"));
    const auto resp = invoke_history(ctrl, {{"metric", "cpu.usage_ratio"}});
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->getStatusCode(), drogon::k200OK);
    const auto body = nlohmann::json::parse(resp->getBody());
    EXPECT_FALSE(body.value("available", true));
    EXPECT_EQ(body.value("reason", ""), "not_found");
    EXPECT_FALSE(body.contains("series"));
}

TEST_F(SysmonControllerTest, HistoryDegradesWhenNoDatabasePathConfigured) {
    // 未注入历史库路径（测试/API-only 模式）：按「历史不可用」降级，不去猜一个路径读错文件
    auto ctrl = std::make_shared<SysmonCtrl>(repo_, mirror_, nullptr);
    const auto resp = invoke_history(ctrl, {{"metric", "cpu.usage_ratio"}});
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->getStatusCode(), drogon::k200OK);
    const auto body = nlohmann::json::parse(resp->getBody());
    EXPECT_FALSE(body.value("available", true));
    EXPECT_EQ(body.value("reason", ""), "not_found");
}

TEST_F(SysmonControllerTest, HistoryReturnsEmptySeriesFromRealTemporaryDatabase) {
    // 真实临时库（写入方建库）+ 真实 REST 调用：可用、空序列、level 回显、无截断
    const auto dir = std::filesystem::temp_directory_path() /
                     ("dz_sysmon_ctrl_hist_" + std::to_string(dztrader::this_process::pid()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const auto db_path = dir / "sysmon.db";
    {
        auto store = dztrader::sysmonstore::HistoryStore::open(db_path);
        (void)store;
    }
    auto ctrl = std::make_shared<SysmonCtrl>(repo_, mirror_, nullptr, db_path);
    const auto resp = invoke_history(ctrl, {{"metric", "cpu.usage_ratio"}, {"level", "raw"}});
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->getStatusCode(), drogon::k200OK);
    const auto body = nlohmann::json::parse(resp->getBody());
    EXPECT_TRUE(body.value("available", false));
    EXPECT_EQ(body.value("level", ""), "raw");
    EXPECT_EQ(body.value("metric", ""), "cpu.usage_ratio");
    EXPECT_FALSE(body.value("truncated", true));
    ASSERT_EQ(body["series"].size(), 1U);
    EXPECT_TRUE(body["series"][0]["points"].empty());
    EXPECT_TRUE(body["marks"].empty());
    EXPECT_TRUE(body["gaps"].empty());
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

}  // namespace
