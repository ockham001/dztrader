#include "mirror_store.h"
#include "ws_broadcaster.h"
#include "log_domain_service.h"
#include "process_domain_service.h"
#include "md_config_domain_service.h"
#include "shm_domain_service.h"
#include "auto_login_domain_service.h"
#include "progress_domain_service.h"
#include "process_mirror.h"
#include "repository.h"
#include <dztrader/platform/process.h>
#include <dztrader/platform/log_config.h>
#include <dztrader/core/core_data_type.h>
#include <dztrader/core/this_process.h>
#include <dztrader/shm/channel_meta.h>
#include <dztrader/shm/frame_codec.h>
#include <dztrader/shm/frame_view.h>
#include <dztrader/shm/reader.h>
#include <dztrader/shm/writer.h>
#include <gtest/gtest.h>

#include <filesystem>

namespace dztrader::webui {
namespace {

class FakeBroadcaster : public WsBroadcaster {
public:
    struct Msg { std::string type; std::string instance_id; nlohmann::json data; };
    std::vector<Msg> messages;
    void broadcast(const std::string& type, const std::string& instance_id,
                   const nlohmann::json& data) override {
        messages.push_back({.type = type, .instance_id = instance_id, .data = data});
    }
};

// SHM 写读 fixture（迁移自 log_control_test.cpp：验证 handle_log_control 的
// 其他进程写 SHM 帧 / 失败 NOTIFY_UI 帧）
struct ShmFixture {
    std::string channel_name;
    std::filesystem::path shm_dir;
    std::shared_ptr<shm::ChannelMeta> meta;
    std::optional<shm::MultiWriter> writer;
    std::optional<shm::Reader> reader;
    static constexpr uint64_t MB = 1024ull * 1024;

    ShmFixture() {
        channel_name = "dz_test_log_ctrl_" + std::to_string(reinterpret_cast<uintptr_t>(this));
        shm_dir = std::filesystem::temp_directory_path() / channel_name;
        std::filesystem::remove_all(shm_dir);
        std::filesystem::create_directories(shm_dir);
        const shm::ChannelConfig cfg{.channel_name = channel_name,
                                     .shm_dir = shm_dir,
                                     .meta_file_size = 4 * MB,
                                     .page_size = 1 * MB,
                                     .lock_memory = false,
                                     .prefetch_memory = false};
        meta = std::make_shared<shm::ChannelMeta>(shm::ChannelMeta::open_or_create(cfg));
        writer = shm::MultiWriter::create(meta, "test_writer");
        reader = shm::Reader::create(meta, "test_reader");
    }
    ~ShmFixture() {
        reader.reset();
        writer.reset();
        meta.reset();
        std::filesystem::remove_all(shm_dir);
    }
    // 排空 reader，返回最后一帧类型
    DzFrameType last_type() {
        auto last = static_cast<DzFrameType>(-1);
        while (const auto* f = reader->next_frame()) {
            last = shm::FrameView(f).type();
        }
        return last;
    }
};

// ===== LogDomainService::handle_log_control（迁移自 log_control_test.cpp，
// dispatch_log_control 归并后行为等价：self 直调/其他 SHM/失败 NOTIFY_UI/未知类型）=====

// 自身 target：直调 LogConfig，不写 SHM，publish 推当前生效值（镜像 + 广播）
TEST(LogDomainServiceTest, SelfTargetDirectCallsLogConfig) {
    auto cfg_path = std::filesystem::temp_directory_path() / "dzweb_dispatch_self.json";
    std::filesystem::remove(cfg_path);
    dztrader::platform::LogConfig self_log("dzweb", cfg_path);
    self_log.load();

    MirrorStore mirror;
    FakeBroadcaster ws;
    LogDomainService svc(mirror, ws, self_log, nullptr);
    const bool handled = svc.handle_log_control(dztrader::this_process::exe_stem(),
                                                DZ_FRAME_SET_LOG_CONFIG, {{"level", "error"}});
    EXPECT_TRUE(handled);
    EXPECT_EQ(self_log.current().at("level"), "error");
    // publish 回推：镜像 + WS 广播（type=log_config, instance=target）
    EXPECT_EQ(mirror.instance(dztrader::this_process::exe_stem())["log_config"]["level"], "error");
    ASSERT_EQ(ws.messages.size(), 1u);
    EXPECT_EQ(ws.messages[0].type, "log_config");
    EXPECT_EQ(ws.messages[0].instance_id, dztrader::this_process::exe_stem());
    EXPECT_EQ(ws.messages[0].data["level"], "error");
    std::filesystem::remove(cfg_path);
}

// 自身 target 非法 level：set_log_config 抛，publish 回推旧值，self_log 未变
TEST(LogDomainServiceTest, SelfTargetInvalidLevelPublishesOld) {
    auto cfg_path = std::filesystem::temp_directory_path() / "dzweb_dispatch_invalid.json";
    std::filesystem::remove(cfg_path);
    dztrader::platform::LogConfig self_log("dzweb", cfg_path);
    self_log.load();  // 默认 debug

    MirrorStore mirror;
    FakeBroadcaster ws;
    LogDomainService svc(mirror, ws, self_log, nullptr);
    const bool handled = svc.handle_log_control(dztrader::this_process::exe_stem(),
                                                DZ_FRAME_SET_LOG_CONFIG, {{"level", "NOPE"}});
    EXPECT_TRUE(handled);
    EXPECT_EQ(self_log.current().at("level"), "debug");  // 未变
    // 回推旧值
    ASSERT_EQ(ws.messages.size(), 1u);
    EXPECT_EQ(ws.messages[0].data["level"], "debug");
    std::filesystem::remove(cfg_path);
}

// 自身 SET 失败（不可写路径）：publish 旧值 + writer 非空时发 NOTIFY_UI 帧
TEST(LogDomainServiceTest, SelfTargetSetFailureNotifiesUi) {
    ShmFixture fx;
    auto bad_path = fx.shm_dir / "no_such_dir" / "webui.json";  // save 必失败
    dztrader::platform::LogConfig self_log("dzweb", bad_path);
    self_log.load();  // 文件缺失，cfg_ = 默认 debug

    MirrorStore mirror;
    FakeBroadcaster ws;
    LogDomainService svc(mirror, ws, self_log, &*fx.writer);
    const bool handled = svc.handle_log_control(dztrader::this_process::exe_stem(),
                                                DZ_FRAME_SET_LOG_CONFIG, {{"level", "error"}});
    EXPECT_TRUE(handled);
    EXPECT_EQ(self_log.current().at("level"), "debug");  // 回滚
    // publish 回推旧值
    ASSERT_EQ(ws.messages.size(), 1u);
    EXPECT_EQ(ws.messages[0].data["level"], "debug");
    EXPECT_EQ(fx.last_type(), DZ_FRAME_NOTIFY_UI);  // 契约：失败必发 NOTIFY_UI
}

// 其他进程 target：写 SHM SET_LOG_CONFIG 帧，不改自身
TEST(LogDomainServiceTest, OtherTargetWritesShmFrame) {
    ShmFixture fx;
    auto cfg_path = std::filesystem::temp_directory_path() / "dzweb_dispatch_other.json";
    std::filesystem::remove(cfg_path);
    dztrader::platform::LogConfig self_log("dzweb", cfg_path);
    self_log.load();  // 默认 debug

    MirrorStore mirror;
    FakeBroadcaster ws;
    LogDomainService svc(mirror, ws, self_log, &*fx.writer);
    const bool handled =
        svc.handle_log_control("dzmd_ctp", DZ_FRAME_SET_LOG_CONFIG, {{"level", "debug"}});
    EXPECT_TRUE(handled);
    EXPECT_EQ(self_log.current().at("level"), "debug");   // 自身不被改写
    EXPECT_TRUE(mirror.instance("dzweb").empty());        // 其他进程分支不 publish
    EXPECT_TRUE(ws.messages.empty());
    EXPECT_EQ(fx.last_type(), DZ_FRAME_SET_LOG_CONFIG);    // 帧已写入 SHM
    std::filesystem::remove(cfg_path);
}

// 未知帧类型：返回 false
TEST(LogDomainServiceTest, NonLogControlFrameReturnsFalse) {
    auto cfg_path = std::filesystem::temp_directory_path() / "dzweb_dispatch_unknown.json";
    std::filesystem::remove(cfg_path);
    dztrader::platform::LogConfig self_log("dzweb", cfg_path);

    MirrorStore mirror;
    FakeBroadcaster ws;
    LogDomainService svc(mirror, ws, self_log, nullptr);
    const bool handled = svc.handle_log_control("x", DZ_FRAME_NOTIFY_UI, nlohmann::json::object());
    EXPECT_FALSE(handled);
    std::filesystem::remove(cfg_path);
}


TEST(LogDomainServiceTest, OnRtnLogConfigUpdatesMirrorAndBroadcasts) {
    MirrorStore mirror;
    FakeBroadcaster ws;
    dztrader::platform::LogConfig self_log("dzweb_test", ":memory:");
    LogDomainService svc(mirror, ws, self_log, nullptr);

    const nlohmann::json payload = {{"level", "warning"}, {"flush_on", "error"}};
    svc.on_rtn_log_config("dzmd_ctp", payload);

    // 镜像更新
    EXPECT_EQ(mirror.instance("dzmd_ctp")["log_config"]["level"], "warning");
    // 广播：type=log_config, instance=dzmd_ctp, data=payload
    ASSERT_EQ(ws.messages.size(), 1u);
    EXPECT_EQ(ws.messages[0].type, "log_config");
    EXPECT_EQ(ws.messages[0].instance_id, "dzmd_ctp");
    EXPECT_EQ(ws.messages[0].data["level"], "warning");
}

TEST(LogDomainServiceTest, PublishIsIdempotent) {
    MirrorStore mirror;
    FakeBroadcaster ws;
    dztrader::platform::LogConfig self_log("dzweb_test", ":memory:");
    LogDomainService svc(mirror, ws, self_log, nullptr);

    const nlohmann::json payload = {{"level", "info"}, {"flush_on", "info"}};
    svc.publish("dzweb", payload);
    svc.publish("dzweb", payload);  // 重复发布（自身直调 + RTN 共用）不累积
    EXPECT_EQ(mirror.instance("dzweb")["log_config"]["level"], "info");
    ASSERT_EQ(ws.messages.size(), 2u);
}

TEST(LogDomainServiceTest, PayloadOverwrites) {
    MirrorStore mirror;
    FakeBroadcaster ws;
    dztrader::platform::LogConfig self_log("dzweb_test", ":memory:");
    LogDomainService svc(mirror, ws, self_log, nullptr);
    svc.on_rtn_log_config("a", nlohmann::json{{"level", "debug"}});
    svc.on_rtn_log_config("a", nlohmann::json{{"level", "error"}});
    EXPECT_EQ(mirror.instance("a")["log_config"]["level"], "error");
    EXPECT_EQ(mirror.instance("a")["log_config"].size(), 1u);
}

TEST(ProcessDomainServiceTest, UnregisteredProcessNotMirrored) {
    MirrorStore mirror;
    FakeBroadcaster ws;
    ProcessMirror pm(mirror);
    ProcessDomainService svc(pm, ws);

    const nlohmann::json payload = {{"name", "ghost"}, {"state", "Running"},
                                    {"pid", 1},        {"display_name", "ghost"},
                                    {"message", ""},   {"event", nlohmann::json()}};
    svc.on_rtn_process_status(payload);
    // 注册守卫：dztraderd.process_config 无 ghost 条目 → 不入镜像
    EXPECT_TRUE(mirror.instance("ghost").empty());
    // 但广播仍发生（对照 ws_controller.cpp:380-410：广播在守卫之外，无条件广播）
    ASSERT_EQ(ws.messages.size(), 1u);
    EXPECT_EQ(ws.messages[0].type, "process_status");
    // 广播 payload 与原实现逐字段一致（event 为 nullopt 时序列化为 null）
    EXPECT_EQ(ws.messages[0].data["name"], "ghost");
    EXPECT_EQ(ws.messages[0].data["state"], "Running");
    EXPECT_TRUE(ws.messages[0].data["event"].is_null());
}

TEST(ProcessDomainServiceTest, RegisteredProcessMirrored) {
    MirrorStore mirror;
    FakeBroadcaster ws;
    ProcessMirror pm(mirror);
    pm.update_process_configs(nlohmann::json{{"dzmd_ctp", nlohmann::json{
        {"args", nlohmann::json::array()}, {"env", nlohmann::json::object()},
        {"restart", nlohmann::json{{"enabled", false}, {"max_attempts", 0}, {"backoff_sec", 5}}}}}});
    ProcessDomainService svc(pm, ws);

    const nlohmann::json payload = {{"name", "dzmd_ctp"}, {"state", "Running"}, {"pid", 123}};
    svc.on_rtn_process_status(payload);
    EXPECT_EQ(mirror.instance("dzmd_ctp")["process_status"]["pid"], 123);
    EXPECT_EQ(ws.messages.size(), 1u);
}

TEST(ProcessDomainServiceTest, ProcessConfigFullOverwrite) {
    MirrorStore mirror;
    FakeBroadcaster ws;
    ProcessMirror pm(mirror);
    ProcessDomainService svc(pm, ws);

    svc.on_rtn_process_config(nlohmann::json{{"dzmd_ctp", nlohmann::json{{"args", nlohmann::json::array()}}}});
    EXPECT_TRUE(mirror.instance("dztraderd")["process_config"].contains("dzmd_ctp"));
    EXPECT_EQ(ws.messages[0].type, "process_config");
    // 全量覆盖：第二次不含 dzmd_xtp 时条目消失
    svc.on_rtn_process_config(nlohmann::json{{"dzmd_xtp", nlohmann::json{{"args", nlohmann::json::array()}}}});
    EXPECT_FALSE(mirror.instance("dztraderd")["process_config"].contains("dzmd_ctp"));
}

TEST(MdConfigDomainServiceTest, SourceAutoRegisteredFromRtn) {
    Repository repo{":memory:"};  // 直接内存库（repository_test.cpp 先例）
    MirrorStore mirror;
    FakeBroadcaster ws;
    ProcessMirror pm(mirror);
    MdConfigDomainService svc(repo, pm, ws);

    const nlohmann::json cfg = {{"brokers", nlohmann::json::array()}};
    svc.on_rtn_md_config("dzmd_ctp", cfg);
    // 镜像更新
    EXPECT_EQ(mirror.instance("dzmd_ctp")["md_config"]["brokers"].size(), 0u);
    // 广播
    ASSERT_EQ(ws.messages.size(), 1u);
    EXPECT_EQ(ws.messages[0].type, "md_rtn_config");
    EXPECT_EQ(ws.messages[0].data["source"], "dzmd_ctp");
    EXPECT_EQ(ws.messages[0].data["config"]["brokers"].size(), 0u);
    // source 自动入库：repo 中已存在 source_name=dzmd_ctp 的 market source（source_type 大写）
    auto src = repo.find_market_source_by_source_name("dzmd_ctp");
    ASSERT_TRUE(src.has_value());
    EXPECT_EQ(src->source_type, "CTP");
    // 重复 RTN 不重复入库（已存在则跳过），广播仍发生
    svc.on_rtn_md_config("dzmd_ctp", cfg);
    EXPECT_EQ(ws.messages.size(), 2u);
}

TEST(ShmDomainServiceTest, EventShmConfigMountedOnMaster) {
    MirrorStore mirror;
    FakeBroadcaster ws;
    ShmDomainService svc(mirror, ws);
    const nlohmann::json payload = {{"check_interval_min", 5},
                                    {"check_pages", 1},
                                    {"check_bytes", 0},
                                    {"page_size_mb", 64},
                                    {"preload_points", nlohmann::json::object()}};
    svc.on_rtn_event_shm_config(payload);
    // 无 instance_id 帧统一挂 dztraderd（P1 镜像约定）
    EXPECT_EQ(mirror.instance("dztraderd")["event_shm_config"]["check_interval_min"], 5);
    ASSERT_EQ(ws.messages.size(), 1u);
    EXPECT_EQ(ws.messages[0].type, "event_shm_config");
    EXPECT_EQ(ws.messages[0].instance_id, "");   // 无 instance_id，广播不带该字段
    EXPECT_EQ(ws.messages[0].data["check_interval_min"], 5);
}

TEST(ShmDomainServiceTest, MdShmConfigMountedOnSource) {
    MirrorStore mirror;
    FakeBroadcaster ws;
    ShmDomainService svc(mirror, ws);
    const nlohmann::json payload = {{"check_interval_min", 10},
                                    {"page_size_mb", 1024},
                                    {"preload_points", nlohmann::json::object()},
                                    {"check_pages", 1},
                                    {"check_bytes", 0}};
    svc.on_rtn_md_shm_config("dzmd_ctp", payload);
    EXPECT_EQ(mirror.instance("dzmd_ctp")["md_shm_config"]["check_interval_min"], 10);
    ASSERT_EQ(ws.messages.size(), 1u);
    EXPECT_EQ(ws.messages[0].type, "md_shm_config");
    EXPECT_EQ(ws.messages[0].instance_id, "dzmd_ctp");
}

TEST(AutoLoginDomainServiceTest, ValidPayloadUpdatesMirror) {
    MirrorStore mirror;
    FakeBroadcaster ws;
    AutoLoginDomainService svc(mirror, ws);
    const nlohmann::json payload = {
        {"enabled", true},
        {"schedules", nlohmann::json::array(
                          {nlohmann::json{{"login_time", "08:45"}, {"logout_time", "15:30"}}})}};
    svc.on_rtn_auto_login("dzmd_ctp", payload);
    EXPECT_EQ(mirror.instance("dzmd_ctp")["auto_login"]["enabled"], true);
    EXPECT_EQ(mirror.instance("dzmd_ctp")["auto_login"]["schedules"][0]["login_time"], "08:45");
    ASSERT_EQ(ws.messages.size(), 1u);
    EXPECT_EQ(ws.messages[0].type, "auto_login");
    EXPECT_EQ(ws.messages[0].instance_id, "dzmd_ctp");
}

TEST(AutoLoginDomainServiceTest, InvalidPayloadIgnored) {
    MirrorStore mirror;
    FakeBroadcaster ws;
    AutoLoginDomainService svc(mirror, ws);
    // 非法：enabled 非 bool（契约 auto-login 校验失败场景）
    const nlohmann::json bad = {{"enabled", "not_bool"}, {"schedules", nlohmann::json::array()}};
    svc.on_rtn_auto_login("dzmd_ctp", bad);
    // 不更新镜像、不广播（契约 auto-login：非法则记日志并忽略）
    EXPECT_TRUE(mirror.instance("dzmd_ctp").empty());
    EXPECT_TRUE(ws.messages.empty());
    // 合法 payload 之后仍可正常更新（非法帧不污染状态）
    const nlohmann::json good = {{"enabled", true}, {"schedules", nlohmann::json::array()}};
    svc.on_rtn_auto_login("dzmd_ctp", good);
    EXPECT_EQ(mirror.instance("dzmd_ctp")["auto_login"]["enabled"], true);
    EXPECT_EQ(ws.messages.size(), 1u);
}

TEST(AutoLoginDomainServiceTest, InvalidScheduleIgnored) {
    MirrorStore mirror;
    FakeBroadcaster ws;
    AutoLoginDomainService svc(mirror, ws);
    // 非法：login_time == logout_time（会话区间必须非空，契约 auto-login）
    const nlohmann::json bad = {
        {"enabled", true},
        {"schedules", nlohmann::json::array(
                          {nlohmann::json{{"login_time", "08:45"}, {"logout_time", "08:45"}}})}};
    svc.on_rtn_auto_login("dzmd_ctp", bad);
    EXPECT_TRUE(mirror.instance("dzmd_ctp").empty());
    EXPECT_TRUE(ws.messages.empty());
}

TEST(ProgressDomainServiceTest, ProgressOverwrites) {
    MirrorStore mirror;
    FakeBroadcaster ws;
    ProgressDomainService svc(mirror, ws);
    svc.on_rtn_progress("dzmd_ctp", nlohmann::json{{"min", 0}, {"max", 4}, {"current", 2}, {"desc", "订阅合约中"}});
    EXPECT_EQ(mirror.instance("dzmd_ctp")["progress"]["current"], 2);
    // 后到覆盖先到（契约 progress：单条完整状态）
    svc.on_rtn_progress("dzmd_ctp", nlohmann::json{{"min", 0}, {"max", 0}, {"current", 0}, {"desc", ""}});
    EXPECT_EQ(mirror.instance("dzmd_ctp")["progress"]["max"], 0);
    EXPECT_EQ(mirror.instance("dzmd_ctp")["progress"]["desc"], "");
    ASSERT_EQ(ws.messages.size(), 2u);
    EXPECT_EQ(ws.messages[1].type, "progress");
    EXPECT_EQ(ws.messages[1].instance_id, "dzmd_ctp");
}

}  // namespace
}  // namespace dztrader::webui
