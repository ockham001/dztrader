#include "shm_manager.h"

#include <dztrader/core/core_data_type.h>
#include <dztrader/core/env.h>
#include <dztrader/core/path.h>
#include <dztrader/core/this_process.h>
#include <dztrader/platform/frame_codec.h>
#include <dztrader/platform/process.h>
#include <dztrader/shm/channel_meta.h>
#include <dztrader/shm/frame_codec.h>
#include <dztrader/shm/frame_view.h>
#include <dztrader/shm/reader.h>
#include <dztrader/shm/writer.h>
#include <dztrader/data_type.h>
#include <dztrader/log/log.h>
#include "orphan_guard.h"
#include "process_registry.h"
#include "process_supervisor.h"

#include <gtest/gtest.h>
#include <boost/asio/io_context.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <thread>
#include <vector>

namespace dztrader::master {
namespace {

// 测试用默认配置
ShmGlobalConfig make_default_shm_global() {
    return {.meta_file_size = 1 * 1024 * 1024};
}

class ShmManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        tmp_dir_ = std::filesystem::temp_directory_path() / "dz_shm_mgr_test";
        std::filesystem::remove_all(tmp_dir_);
        std::filesystem::create_directories(tmp_dir_);
        orig_home_ = dztrader::env::get("DZTRADER_HOME");
        dztrader::env::set("DZTRADER_HOME", tmp_dir_.string());
        // 最小 dztraderd.json (init 配置由参数注入, 文件仅供 parse_master_json 测试用)
        // shm.event 含 page_size_mb=1 (测试用小页面, 降低内存占用)
        cfg_path_ = tmp_dir_ / "dztraderd.json";
        std::ofstream ofs(cfg_path_);
        ofs << R"({"log":{}, "master":{}, "shm":{"meta_file_size":1048576,"event":{"page_size_mb":1,"preload_points":{},"check_interval_min":5,"check_pages":1,"check_bytes":0}}})";
    }
    void TearDown() override {
        if (orig_home_) {
            dztrader::env::set("DZTRADER_HOME", *orig_home_);
        } else {
            dztrader::env::unset("DZTRADER_HOME");
        }
        std::filesystem::remove_all(tmp_dir_);
    }

    std::filesystem::path tmp_dir_;
    std::filesystem::path cfg_path_;
    std::optional<std::string> orig_home_;
};

TEST_F(ShmManagerTest, InitCreatesEventChannelAndWriterReaderCleaner) {
    ShmManager mgr(make_default_shm_global(), cfg_path_);
    // 构造后 event_writer_/event_reader_ 可用：send_shutdown 能写入 REQUEST_SHUTDOWN
    EXPECT_NO_THROW(mgr.send_shutdown());
}

TEST_F(ShmManagerTest, SendShutdownWritesFrame) {
    ShmManager mgr(make_default_shm_global(), cfg_path_);

    // 先注册独立 Reader（reader 只能看到注册之后写入的帧），
    // 再 send_shutdown，然后用该 Reader 验证 event channel 有 REQUEST_SHUTDOWN_ALL 帧
    shm::Reader reader = shm::Reader::create(shm::channel_name("dzevent"), dztrader::paths::shm(), "test_reader");
    mgr.send_shutdown();

    int seen = 0;
    for (int i = 0; i < 8; ++i) {
        const auto* frame = reader.next_frame();
        if (!frame) break;
        shm::FrameView view(frame);
        if (view.type() == DZ_FRAME_REQUEST_SHUTDOWN_ALL) {
            ++seen;
        }
    }
    EXPECT_GE(seen, 1);
}

// REQUEST_SHUTDOWN_ALL 使用无 instance_id 扩展头 (DzExtFrameHeader)
TEST_F(ShmManagerTest, SendShutdownNoArgNoInstanceId) {
    ShmManager mgr(make_default_shm_global(), cfg_path_);

    shm::Reader reader = shm::Reader::create(shm::channel_name("dzevent"), dztrader::paths::shm(), "shutdown_all_reader");
    mgr.send_shutdown();

    int seen = 0;
    for (int i = 0; i < 8; ++i) {
        const auto* frame = reader.next_frame();
        if (!frame) break;
        shm::FrameView view(frame);
        if (view.type() == DZ_FRAME_REQUEST_SHUTDOWN_ALL) {
            ++seen;
        }
    }
    EXPECT_GE(seen, 1);
}

TEST_F(ShmManagerTest, SendShutdownWithTargetWritesInstFrame) {
    ShmManager mgr(make_default_shm_global(), cfg_path_);

    shm::Reader reader = shm::Reader::create(shm::channel_name("dzevent"), dztrader::paths::shm(), "shutdown_target_reader");
    mgr.send_shutdown("dzmd_ctp");

    int seen = 0;
    for (int i = 0; i < 8; ++i) {
        const auto* frame = reader.next_frame();
        if (!frame) break;
        shm::FrameView view(frame);
        if (view.type() == DZ_FRAME_REQUEST_SHUTDOWN) {
            ++seen;
            EXPECT_STREQ(view.ext_inst_id(), "dzmd_ctp");
        }
    }
    EXPECT_GE(seen, 1);
}

TEST_F(ShmManagerTest, CleanupOldPagesNoThrow) {
    ShmManager mgr(make_default_shm_global(), cfg_path_);
    // 无 page 文件时 cleanup 应安全返回 0
    EXPECT_NO_THROW(mgr.cleanup_old_pages());
}

TEST_F(ShmManagerTest, PollEventChannelReadsStatusFrame) {
    ShmManager mgr(make_default_shm_global(), cfg_path_);

    // 用独立 MultiWriter 模拟子进程写一个 RTN_MD_STATUS 帧
    auto meta = shm::ChannelMeta::open_only(shm::channel_name("dzevent"), dztrader::paths::shm());
    auto writer = shm::MultiWriter::create(
        std::make_shared<shm::ChannelMeta>(std::move(meta)), "fake_md");
    nlohmann::json status = {{"state", 4}, {"trading_day", "20260704"}};
    std::string payload = status.dump();
    ASSERT_TRUE(writer.write_ext_inst_frame(DZ_FRAME_RTN_MD_STATUS, "ctp",
        reinterpret_cast<const std::byte*>(payload.c_str()),
        static_cast<uint32_t>(payload.size())));

    // master drain 应读到该帧（可能需多次 drain 排干初始化帧）
    for (int i = 0; i < 10; ++i) {
        mgr.drain_event_channel();
    }
    // 不直接断言内部 status_view_（private），仅验证 drain 不抛异常且 send_shutdown 仍可用
    EXPECT_NO_THROW(mgr.send_shutdown());
}

TEST_F(ShmManagerTest, MasterHandlesSetLogLevelForSelf) {
    ShmManager mgr(make_default_shm_global(), cfg_path_);

    // 注册一个 reader 来观察 master 自己回写的 RTN_LOG_CONFIG
    shm::Reader reader = shm::Reader::create(shm::channel_name("dzevent"), dztrader::paths::shm(), "lvl_report_reader");

    // 模拟 dzweb 写入 SET_LOG_CONFIG 帧 (target="dztraderd", 与 master exe stem 一致)
    auto meta = shm::ChannelMeta::open_only(shm::channel_name("dzevent"), dztrader::paths::shm());
    auto writer = shm::MultiWriter::create(
        std::make_shared<shm::ChannelMeta>(std::move(meta)), "fake_ui");
    nlohmann::json req = {{"level", "debug"}};
    ASSERT_TRUE(shm::write_ext_inst_json(writer, DZ_FRAME_SET_LOG_CONFIG, "dztraderd", req));
    writer.notify_subscribers();

    // master drain 应读到该帧并执行 set_level
    for (int i = 0; i < 10; ++i) {
        mgr.drain_event_channel();
    }

    // 验证 master 回写了 RTN_LOG_CONFIG (source="dztraderd", level="debug")
    bool reported = false;
    for (int i = 0; i < 16; ++i) {
        const auto* frame = reader.next_frame();
        if (!frame) break;
        shm::FrameView view(frame);
        if (view.type() == DZ_FRAME_RTN_LOG_CONFIG) {
            // 验证 source 是 "dztraderd" (与 exe stem 一致, 修复前是 "master")
            EXPECT_EQ(std::string_view(view.ext_inst_id()), "dztraderd");
            auto report = nlohmann::json::parse(
                reinterpret_cast<const char*>(view.ext_inst_payload()),
                reinterpret_cast<const char*>(view.ext_inst_payload()) + view.ext_inst_payload_size());
            if (report["level"] == "debug") {
                reported = true;
                break;
            }
        }
    }
    EXPECT_TRUE(reported);
}

TEST_F(ShmManagerTest, SetShmConfigAppliesAndPersists) {
    // 初始: dztraderd.json 的 shm.event 含 page_size_mb=1, check_interval_min=5
    nlohmann::json initial = {
        {"log", {{"level", "info"}, {"flush_on", "warning"}}},
        {"master", {{"single_stop_timeout_sec", 3}}},
        {"shm", {
            {"meta_file_size", 1 * 1024 * 1024},
            {"event", {{"page_size_mb", 1}, {"preload_points", nlohmann::json::object()},
                       {"check_interval_min", 5}, {"check_pages", 1}, {"check_bytes", 0}}}
        }}
    };
    std::ofstream(cfg_path_) << initial.dump(2);

    ShmManager mgr(make_default_shm_global(), cfg_path_);
    SPDLOG_INFO("BISECT: ctor done");

    // 构造 SET_EVENT_SHM_CONFIG 帧 (无 instance_id, payload = JSON Merge Patch)
    // page_size_mb 不可变, SET 中传 999 应被完全跳过 (不解析、不校验、不报错)
    // preload_points 为 object map (新 schema): {"08:45": {"pages":1, "bytes":0}}
    nlohmann::json req = {
        {"page_size_mb", 999},  // 不可变, 应被忽略
        {"preload_points", {{"08:45", {{"pages", 1}, {"bytes", 0}}}}},
        {"check_interval_min", 10},
        {"check_pages", 3},
        {"check_bytes", 2048}
    };

    // 注册 reader 观察 master 回写的 RTN_EVENT_SHM_CONFIG (reader 只能看到注册之后写入的帧)
    shm::Reader rtn_reader = shm::Reader::create(shm::channel_name("dzevent"), dztrader::paths::shm(), "shm_report_reader");
    SPDLOG_INFO("BISECT: reader done");

    // 用独立 MultiWriter 模拟 UI 写入 SET_EVENT_SHM_CONFIG 帧 (无 instance_id, DzExtFrameHeader)
    auto meta = shm::ChannelMeta::open_only(shm::channel_name("dzevent"), dztrader::paths::shm());
    auto writer = shm::MultiWriter::create(
        std::make_shared<shm::ChannelMeta>(std::move(meta)), "fake_ui");
    SPDLOG_INFO("BISECT: writer done");
    ASSERT_TRUE(shm::write_ext_json(writer, DZ_FRAME_SET_EVENT_SHM_CONFIG, req));
    writer.notify_subscribers();
    SPDLOG_INFO("BISECT: write done");

    // master drain 应读到该帧并执行 SET_EVENT_SHM_CONFIG
    for (int i = 0; i < 10; ++i) {
        mgr.drain_event_channel();
    }
    SPDLOG_INFO("BISECT: drains done");

    // 验证: 持久化到磁盘的可变字段已更新, 不可变字段保留原值
    std::ifstream ifs(cfg_path_);
    nlohmann::json saved;
    ifs >> saved;
    EXPECT_EQ(saved["shm"]["meta_file_size"], 1 * 1024 * 1024);  // 保留
    EXPECT_EQ(saved["shm"]["event"]["page_size_mb"], 1);          // 保留原值, 未被 999 覆盖
    EXPECT_EQ(saved["shm"]["event"]["check_interval_min"], 10);   // 已更新
    EXPECT_EQ(saved["shm"]["event"]["check_pages"], 3);           // 已更新
    EXPECT_EQ(saved["shm"]["event"]["check_bytes"], 2048);        // 已更新

    // 验证 preload_points object map 持久化 (新 schema: key="HH:MM", value={pages,bytes})
    ASSERT_TRUE(saved["shm"]["event"].contains("preload_points"));
    ASSERT_TRUE(saved["shm"]["event"]["preload_points"].is_object());
    ASSERT_TRUE(saved["shm"]["event"]["preload_points"].contains("08:45"));
    EXPECT_EQ(saved["shm"]["event"]["preload_points"]["08:45"]["pages"], 1);
    EXPECT_EQ(saved["shm"]["event"]["preload_points"]["08:45"]["bytes"], 0);

    // 验证 master 回写了 RTN_EVENT_SHM_CONFIG (无 instance_id, 含不可变的 page_size_mb)
    bool reported = false;
    for (int i = 0; i < 16; ++i) {
        const auto* frame = rtn_reader.next_frame();
        if (!frame) break;
        shm::FrameView view(frame);
        if (view.type() == DZ_FRAME_RTN_EVENT_SHM_CONFIG) {
            // RTN 无 instance_id (契约 02-shm：事件通道帧头无 instance_id), 用 ext_payload 读取
            auto cfg = nlohmann::json::parse(
                reinterpret_cast<const char*>(view.ext_payload()),
                reinterpret_cast<const char*>(view.ext_payload()) + view.ext_payload_size());
            EXPECT_EQ(cfg["check_interval_min"], 10);
            EXPECT_EQ(cfg["check_pages"], 3);
            EXPECT_EQ(cfg["check_bytes"], 2048);
            EXPECT_EQ(cfg["page_size_mb"], 1);  // 不可变, 保留原值
            reported = true;
            break;
        }
    }
    EXPECT_TRUE(reported);
}

TEST_F(ShmManagerTest, ReadMdPageSizeReturnsConfiguredValue) {
    // 准备 configs 目录 + test_source.json
    auto configs_dir = tmp_dir_ / "configs";
    std::filesystem::create_directories(configs_dir);
    auto json_path = configs_dir / "test_source.json";
    std::ofstream(json_path) << R"({"shm": {"page_size_mb": 512}})";

    ShmManager mgr(make_default_shm_global(), cfg_path_);
    auto size = mgr.read_md_page_size("test_source");
    ASSERT_TRUE(size.has_value());
    EXPECT_EQ(*size, 512ull * 1024 * 1024);
}

TEST_F(ShmManagerTest, ReadMdPageSizeReturnsDefaultWhenFileMissing) {
    ShmManager mgr(make_default_shm_global(), cfg_path_);
    auto size = mgr.read_md_page_size("nonexistent_source");
    // 默认 kDefaultMdPageSize = 1024MB (与 MdShmConfig 默认值一致)
    ASSERT_TRUE(size.has_value());
    EXPECT_EQ(*size, ShmManager::kDefaultMdPageSize);
}

TEST_F(ShmManagerTest, ReadMdPageSizeReturnsDefaultWhenFieldMissing) {
    auto configs_dir = tmp_dir_ / "configs";
    std::filesystem::create_directories(configs_dir);
    auto json_path = configs_dir / "no_field.json";
    std::ofstream(json_path) << R"({"shm": {"check_interval_min": 5}})";  // 无 page_size_mb

    ShmManager mgr(make_default_shm_global(), cfg_path_);
    auto size = mgr.read_md_page_size("no_field");
    ASSERT_TRUE(size.has_value());
    EXPECT_EQ(*size, ShmManager::kDefaultMdPageSize);
}

TEST_F(ShmManagerTest, ReadMdPageSizeReturnsNulloptOnMalformedJson) {
    auto configs_dir = tmp_dir_ / "configs";
    std::filesystem::create_directories(configs_dir);
    auto json_path = configs_dir / "malformed.json";
    // 写入格式错误的 json (未闭合字符串)
    std::ofstream(json_path) << R"({"shm": {"page_size_mb": "unclosed})";

    ShmManager mgr(make_default_shm_global(), cfg_path_);
    auto size = mgr.read_md_page_size("malformed");
    EXPECT_FALSE(size.has_value());
}

TEST_F(ShmManagerTest, SetSupervisorStoresPointer) {
    ShmManager mgr(make_default_shm_global(), cfg_path_);
    // 不能构造真正的 ProcessSupervisor (需要 io_context 等), 只验证 set_supervisor 可调用
    // 用 nullptr 验证存储语义
    mgr.set_supervisor(nullptr);
    // 无 crash 即通过; 真正集成在 main.cpp 中
}

// ============================================================================
// start_event_shm_maintenance / reschedule_event_shm_maintenance 测试
// ============================================================================

// I3-1: start_event_shm_maintenance 幂等性 — 重复调用不应崩溃或创建重复定时器
// 验证方式: 第二次调用应走 "already started" 分支, recycle lambda 不被重复投递
// poll 一次 io_context 确认无异常
TEST_F(ShmManagerTest, StartEventShmMaintenanceIsIdempotent) {
    ShmManager mgr(make_default_shm_global(), cfg_path_);

    boost::asio::io_context ioc;
    mgr.start_event_shm_maintenance(ioc);
    mgr.start_event_shm_maintenance(ioc);  // 第二次调用应被忽略

    // poll 不应崩溃 (定时器未到期, 但 poll 安全即可)
    ioc.poll();
    // 清理 (取消定时器, 避免析构时回调访问已销毁的 mgr)
    mgr.release_all();
    SUCCEED();  // 主要验证不崩溃
}

// I3-2: 不调 start 直接 reschedule 应是 no-op, 不崩溃
// 验证 timer 为 null 时的早返回路径
TEST_F(ShmManagerTest, RescheduleEventShmMaintenanceWithoutStartIsNoOp) {
    ShmManager mgr(make_default_shm_global(), cfg_path_);
    // 不调 start_event_shm_maintenance, 直接 reschedule
    // 应该是 no-op (timer_ 为 null), 不崩溃
    mgr.reschedule_event_shm_maintenance();
    SUCCEED();
}

// I3-3: check_interval_min=0 时 start 不应创建 event_maintenance_timer_
// 但 preload_points_timer_ 仍应被创建 (独立于 check_interval_min)
// 验证方式: reschedule (check_interval_min=0) 不应崩溃, release_all 清理两个 timer
TEST_F(ShmManagerTest, StartEventShmMaintenanceWithZeroIntervalIsDisabled) {
    // check_interval_min = 0 (禁用周期检查)
    nlohmann::json cfg = {
        {"log", {{"level", "info"}, {"flush_on", "warning"}}},
        {"master", {{"single_stop_timeout_sec", 3}}},
        {"shm", {
            {"meta_file_size", 1 * 1024 * 1024},
            {"event", {{"page_size_mb", 1}, {"preload_points", nlohmann::json::object()},
                       {"check_interval_min", 0}, {"check_pages", 1}, {"check_bytes", 0}}}
        }}
    };
    std::ofstream(cfg_path_) << cfg.dump(2);

    ShmManager mgr(make_default_shm_global(), cfg_path_);

    boost::asio::io_context ioc;
    mgr.start_event_shm_maintenance(ioc);  // 应走 "disabled" 分支, 不创建 event_maintenance_timer_

    // reschedule 也应走 "stopped" 分支 (timer 为 null)
    mgr.reschedule_event_shm_maintenance();

    // release_all 应安全 (preload_points_timer_ 仍存在, 需 cancel)
    mgr.release_all();
    SUCCEED();
}

// ============================================================================
// 新协议 (115-118) 帧处理测试: REQUEST_PROCESS_CONTROL / SET_PROCESS_CONFIG / 全量快照
// ============================================================================
// 真实 ProcessSupervisor + registry 从 dztraderd.json 加载 ("md" section 注册 dzmd_ctp)。
// 用例只走"未注册/未启动"路径 (不 spawn 真实子进程), 帧驱动方式:
//   独立 reader 注册 -> 独立 writer 写请求帧 -> drain_event_channel
//   -> reader 排空断言响应帧 (116/118/101)。
// 注: handle_process_control 要求 supervisor_ 非空, 故必须 set_supervisor 注入真实 supervisor
//     (注入时同时触发 store 初始镜像 load, 见 set_supervisor 实现)。
class ProcessControlFrameTest : public ::testing::Test {
protected:
    void SetUp() override {
        tmp_dir_ = std::filesystem::temp_directory_path() / "dz_pc_frame_test";
        std::filesystem::remove_all(tmp_dir_);
        std::filesystem::create_directories(tmp_dir_);
        orig_home_ = dztrader::env::get("DZTRADER_HOME");
        dztrader::env::set("DZTRADER_HOME", tmp_dir_.string());
        // 最小 dztraderd.json + md section 注册 dzmd_ctp (registry 从文件加载,
        // display_name="CTP行情" 供 SET/快照用例断言旧值/全量 map)
        cfg_path_ = tmp_dir_ / "dztraderd.json";
        std::ofstream(cfg_path_) << R"({"log":{}, "master":{}, "shm":{"meta_file_size":1048576,"event":{"page_size_mb":1,"preload_points":{},"check_interval_min":5,"check_pages":1,"check_bytes":0}},"md":{"dzmd_ctp":{"args":[],"env":{},"restart":{"enabled":false,"max_attempts":0,"backoff_sec":5},"display_name":"CTP行情"}}})";
        registry_.load(cfg_path_);
        shm_mgr_ = std::make_unique<ShmManager>(make_default_shm_global(), cfg_path_);
        // 真实 supervisor (不 start_all, 不 spawn 子进程)
        supervisor_ = std::make_unique<ProcessSupervisor>(ioc_, registry_, *shm_mgr_, orphan_guard_);
        shm_mgr_->set_supervisor(supervisor_.get());  // 注入 + store 初始镜像 load
    }
    void TearDown() override {
        // 兜底: 清理任何残留子进程 (StartRunningPushesStartSucceeded 等真实 spawn 用例
        // 在断言失败时会跳过测试内的 Stop 清理, 防止 test_worker 成为孤儿进程)
        if (supervisor_) {
            supervisor_->force_terminate_all();
        }
        shm_mgr_.reset();  // 先于 supervisor (supervisor 持有 mgr 引用)
        supervisor_.reset();
        if (orig_home_) {
            dztrader::env::set("DZTRADER_HOME", *orig_home_);
        } else {
            dztrader::env::unset("DZTRADER_HOME");
        }
        std::filesystem::remove_all(tmp_dir_);
    }

    // ---- 帧驱动 helpers ----
    // 创建独立 reader (必须注册在写帧之前才能看到 master 的响应帧)
    shm::Reader create_reader(const std::string& suffix) {
        return shm::Reader::create(shm::channel_name("dzevent"), dztrader::paths::shm(), suffix);
    }
    // 创建独立 writer (模拟 dzweb 写请求帧)
    shm::MultiWriter create_writer(const std::string& suffix) {
        auto meta = shm::ChannelMeta::open_only(shm::channel_name("dzevent"), dztrader::paths::shm());
        return shm::MultiWriter::create(std::make_shared<shm::ChannelMeta>(std::move(meta)), suffix);
    }
    // 写 REQUEST_PROCESS_CONTROL 帧 (115, 无 instance_id) + notify
    void write_process_control_frame(shm::MultiWriter& w, platform::ProcessAction action,
                                     const std::string& target,
                                     std::optional<nlohmann::json> config = std::nullopt) {
        platform::ProcessControlReq req{.action = action, .target = target, .config = std::move(config)};
        platform::write_ext_json(w, DZ_FRAME_REQUEST_PROCESS_CONTROL, req);
        w.notify_subscribers();
    }
    // 写 SET_PROCESS_CONFIG 帧 (117, 无 instance_id) + notify
    void write_set_process_config_frame(shm::MultiWriter& w, const std::string& target,
                                        const nlohmann::json& config) {
        platform::SetProcessConfigReq req{.target = target, .config = config};
        platform::write_ext_json(w, DZ_FRAME_SET_PROCESS_CONFIG, req);
        w.notify_subscribers();
    }
    // 写 QUERY_FULL_SNAPSHOT 帧 (113, 空 payload) + notify
    void write_query_full_snapshot(shm::MultiWriter& w) {
        ASSERT_TRUE(w.write_ext_frame(DZ_FRAME_QUERY_FULL_SNAPSHOT, nullptr, 0));
        w.notify_subscribers();
    }
    // 排空 reader 并收集所有帧 (type -> payloads; 空 payload 帧记为空 json)
    // 注意: 帧可能带 instance_id (如 RTN_LOG_CONFIG) — 按帧类型选择解析入口
    std::map<DzFrameType, std::vector<nlohmann::json>> drain_all(shm::Reader& reader) {
        std::map<DzFrameType, std::vector<nlohmann::json>> frames;
        for (int i = 0; i < 128; ++i) {
            const auto* frame = reader.next_frame();
            if (!frame) break;
            shm::FrameView view(frame);
            const bool has_inst = (view.type() != DZ_FRAME_PRELOAD_EVENT_SHM &&
                view.type() != DZ_FRAME_REQUEST_SHUTDOWN_ALL &&
                view.type() != DZ_FRAME_UPDATE_SHM_EVENT_SUBSCRIBER &&
                view.type() != DZ_FRAME_SET_EVENT_SHM_CONFIG &&
                view.type() != DZ_FRAME_RTN_EVENT_SHM_CONFIG &&
                view.type() != DZ_FRAME_QUERY_FULL_SNAPSHOT &&
                view.type() != DZ_FRAME_NOTIFY_UI &&
                view.type() != DZ_FRAME_REQUEST_PROCESS_CONTROL &&
                view.type() != DZ_FRAME_RTN_PROCESS_STATUS &&
                view.type() != DZ_FRAME_SET_PROCESS_CONFIG &&
                view.type() != DZ_FRAME_RTN_PROCESS_CONFIG);
            const auto* data = reinterpret_cast<const char*>(
                has_inst ? view.ext_inst_payload() : view.ext_payload());
            const auto size = has_inst ? view.ext_inst_payload_size() : view.ext_payload_size();
            if (size == 0) {
                frames[view.type()].emplace_back();  // 纯信号帧
                continue;
            }
            frames[view.type()].push_back(nlohmann::json::parse(data, data + size));
        }
        return frames;
    }

    std::filesystem::path tmp_dir_;
    std::filesystem::path cfg_path_;
    std::optional<std::string> orig_home_;
    boost::asio::io_context ioc_;
    ProcessRegistry registry_;
    OrphanGuard orphan_guard_;
    std::unique_ptr<ProcessSupervisor> supervisor_;
    std::unique_ptr<ShmManager> shm_mgr_;
};

// 1. Start 未注册 target: 116{event=StartFailed} + NOTIFY_UI (101)
TEST_F(ProcessControlFrameTest, StartUnknownTargetPushesStartFailed) {
    auto reader = create_reader("start_fail_reader");
    auto writer = create_writer("fake_ui");

    write_process_control_frame(writer, platform::ProcessAction::Start, "no_such_target");

    for (int i = 0; i < 10; ++i) {
        shm_mgr_->drain_event_channel();
    }

    auto frames = drain_all(reader);
    auto it = frames.find(DZ_FRAME_RTN_PROCESS_STATUS);
    ASSERT_NE(it, frames.end());
    const auto& status = it->second.back();
    EXPECT_EQ(status["name"], "no_such_target");
    EXPECT_EQ(status["state"], "Crashed");
    EXPECT_EQ(status["event"], "StartFailed");
    // 失败路径必须通过 NOTIFY_UI 反馈 (契约 03: 错误级别弹窗; 契约 02: level 为字符串)
    auto nit = frames.find(DZ_FRAME_NOTIFY_UI);
    ASSERT_NE(nit, frames.end());
    EXPECT_EQ(nit->second.back()["level"], "error");
}

// 2. 已注册但未启动 -> Stop 幂等成功 (契约 04-process)
TEST_F(ProcessControlFrameTest, StopIdempotentWhenNotRunning) {
    auto reader = create_reader("stop_idem_reader");
    auto writer = create_writer("fake_ui");

    write_process_control_frame(writer, platform::ProcessAction::Stop, "dzmd_ctp");

    for (int i = 0; i < 10; ++i) {
        shm_mgr_->drain_event_channel();
    }

    auto frames = drain_all(reader);
    auto it = frames.find(DZ_FRAME_RTN_PROCESS_STATUS);
    ASSERT_NE(it, frames.end());
    const auto& status = it->second.back();
    EXPECT_EQ(status["name"], "dzmd_ctp");
    EXPECT_EQ(status["state"], "Stopped");
    EXPECT_EQ(status["event"], "StopSucceeded");
}

// 3. Remove 未注册 target: 116{event=RemoveFailed} (不幂等, 契约 04-process) + NOTIFY_UI
TEST_F(ProcessControlFrameTest, RemoveUnknownTargetPushesRemoveFailed) {
    auto reader = create_reader("remove_fail_reader");
    auto writer = create_writer("fake_ui");

    write_process_control_frame(writer, platform::ProcessAction::Remove, "no_such_target");

    for (int i = 0; i < 10; ++i) {
        shm_mgr_->drain_event_channel();
    }

    auto frames = drain_all(reader);
    auto it = frames.find(DZ_FRAME_RTN_PROCESS_STATUS);
    ASSERT_NE(it, frames.end());
    const auto& status = it->second.back();
    EXPECT_EQ(status["name"], "no_such_target");
    EXPECT_EQ(status["event"], "RemoveFailed");
    EXPECT_NE(status["event"], "RemoveSucceeded");  // 未注册不幂等
    // 契约 03 第 132 行: Remove 未注册 target 必须错误级别弹窗 (契约 02: level 为字符串)
    auto nit = frames.find(DZ_FRAME_NOTIFY_UI);
    ASSERT_NE(nit, frames.end());
    EXPECT_EQ(nit->second.back()["level"], "error");
}

// 4. SET_PROCESS_CONFIG 合法 patch: 118 全量 map 含新值 (RFC 7386 覆盖)
TEST_F(ProcessControlFrameTest, SetProcessConfigPushesFullMap) {
    auto reader = create_reader("set_cfg_reader");
    auto writer = create_writer("fake_ui");

    write_set_process_config_frame(writer, "dzmd_ctp", nlohmann::json{{"display_name", "新名"}});

    for (int i = 0; i < 10; ++i) {
        shm_mgr_->drain_event_channel();
    }

    auto frames = drain_all(reader);
    auto it = frames.find(DZ_FRAME_RTN_PROCESS_CONFIG);
    ASSERT_NE(it, frames.end());
    // 118 始终全量镜像 (契约 04-process)
    const auto& cfg = it->second.back();
    ASSERT_TRUE(cfg.contains("dzmd_ctp"));
    EXPECT_EQ(cfg["dzmd_ctp"]["display_name"], "新名");
    EXPECT_TRUE(cfg["dzmd_ctp"]["args"].is_array());    // 全量字段保留
    EXPECT_TRUE(cfg["dzmd_ctp"]["env"].is_object());
    EXPECT_TRUE(cfg["dzmd_ctp"]["restart"].is_object());
}

// 5. SET_PROCESS_CONFIG 非法 patch: store 强保证镜像不变, 118 推旧值 + NOTIFY_UI
TEST_F(ProcessControlFrameTest, SetProcessConfigInvalidPushesOldMapAndNotify) {
    auto reader = create_reader("set_cfg_invalid_reader");
    auto writer = create_writer("fake_ui");

    // 非法 patch: restart=null (契约: 任何位置 null 均为校验失败)
    nlohmann::json bad_patch;
    bad_patch["restart"] = nullptr;
    write_set_process_config_frame(writer, "dzmd_ctp", bad_patch);

    for (int i = 0; i < 10; ++i) {
        shm_mgr_->drain_event_channel();
    }

    auto frames = drain_all(reader);
    // 118 推旧值 (契约 04-process: 失败回滚旧 map)
    auto it = frames.find(DZ_FRAME_RTN_PROCESS_CONFIG);
    ASSERT_NE(it, frames.end());
    EXPECT_EQ(it->second.back()["dzmd_ctp"]["display_name"], "CTP行情");
    // 失败路径带 NOTIFY_UI (error, 契约 02: level 为字符串)
    auto nit = frames.find(DZ_FRAME_NOTIFY_UI);
    ASSERT_NE(nit, frames.end());
    EXPECT_EQ(nit->second.back()["level"], "error");
}

// 6. QUERY_FULL_SNAPSHOT: 118 全量配置 + 每条注册进程一条 116 (event 缺失 = 自发)
TEST_F(ProcessControlFrameTest, FullSnapshotPushesConfigAndStatuses) {
    auto reader = create_reader("snapshot_reader");
    auto writer = create_writer("fake_ui");

    write_query_full_snapshot(writer);

    for (int i = 0; i < 10; ++i) {
        shm_mgr_->drain_event_channel();
    }

    auto frames = drain_all(reader);
    // 118: 全量配置 map
    auto cit = frames.find(DZ_FRAME_RTN_PROCESS_CONFIG);
    ASSERT_NE(cit, frames.end());
    ASSERT_TRUE(cit->second.back().contains("dzmd_ctp"));
    EXPECT_EQ(cit->second.back()["dzmd_ctp"]["display_name"], "CTP行情");
    // 116: 注册进程状态, 无 event 字段 (自发状态变化, 契约 04-process)
    auto sit = frames.find(DZ_FRAME_RTN_PROCESS_STATUS);
    ASSERT_NE(sit, frames.end());
    EXPECT_GE(sit->second.size(), 1u);
    for (const auto& st : sit->second) {
        EXPECT_FALSE(st.contains("event"));
    }
}

// 7. Start 已运行进程 → 幂等 StartSucceeded (P1 回归: 不重复 spawn, 不推 Crashed/StartFailed)
// 第一次 Start 真实 spawn test_worker (长驻); 第二次 Start 必须走幂等分支 (契约 04-process)。
// 契约 03 修订: 未注册 target 但 App Root 下存在同名网关 exe (dzmd_*)
// -> 动态注册 (registry + dztraderd.json 持久化 + 118 全量) 后正常启动 (116 Running/StartSucceeded)。
TEST_F(ProcessControlFrameTest, StartUnregisteredGatewayDynamicallyRegistersAndStarts) {
    const auto worker_exe = dztrader::this_process::exe_dir() / "test_worker"
#ifdef _WIN32
        ".exe"
#endif
        ;
    // 复制为 dzmd_ 前缀网关 exe (扫描器按前缀判 GatewayMd), 用例结束后清理
    const auto gw_exe = worker_exe.parent_path() / "dzmd_test_worker"
#ifdef _WIN32
        ".exe"
#endif
        ;
    std::filesystem::remove(gw_exe);
    std::filesystem::copy_file(worker_exe, gw_exe);

    auto reader = create_reader("dynreg_reader");
    auto writer = create_writer("fake_ui");

    // 未注册 Start: 扫描命中 -> 动态注册 + 启动
    write_process_control_frame(writer, platform::ProcessAction::Start, "dzmd_test_worker");
    for (int i = 0; i < 10; ++i) {
        shm_mgr_->drain_event_channel();
    }
    {
        auto frames = drain_all(reader);
        // 118 全量含新条目 (dzweb 注册守卫依赖; 必须先于 116)
        auto cit = frames.find(DZ_FRAME_RTN_PROCESS_CONFIG);
        ASSERT_NE(cit, frames.end());
        const auto& cfg = cit->second.back();
        EXPECT_TRUE(cfg.contains("dzmd_test_worker"));
        EXPECT_TRUE(cfg["dzmd_test_worker"].contains("restart"));
        // 116 Running + StartSucceeded
        auto it = frames.find(DZ_FRAME_RTN_PROCESS_STATUS);
        ASSERT_NE(it, frames.end());
        const auto& status = it->second.back();
        EXPECT_EQ(status["name"], "dzmd_test_worker");
        EXPECT_EQ(status["event"], "StartSucceeded");
        EXPECT_EQ(status["state"], "Running");
    }
    // registry 已含新条目
    EXPECT_NE(supervisor_->find_registry_entry("dzmd_test_worker"), nullptr);
    // dztraderd.json 已持久化 md 段 (下次 master 启动自动拉起)
    {
        std::ifstream ifs(cfg_path_);
        nlohmann::json j;
        ifs >> j;
        EXPECT_TRUE(j.contains("md"));
        EXPECT_TRUE(j["md"].contains("dzmd_test_worker"));
    }

    // 清理: 帧驱动 Stop + 跑 io_context 让 force-kill 定时器触发 (worker 不读事件通道)
    write_process_control_frame(writer, platform::ProcessAction::Stop, "dzmd_test_worker");
    for (int i = 0; i < 10; ++i) {
        shm_mgr_->drain_event_channel();
    }
    ioc_.restart();
    ioc_.run_for(std::chrono::seconds(5));
    std::filesystem::remove(gw_exe);
}

// 契约 03 修订: 未注册 target 扫描命中但非网关进程 (test_worker 无前缀, 策略有独立注册流程)
// -> 回 StartFailed, 不 spawn。
// 契约 03 修订组合: 未注册网关 + Start 携带 config patch
// -> 先动态注册（默认配置）再应用 patch, 118 全量为 patch 后的值。
TEST_F(ProcessControlFrameTest, StartUnregisteredGatewayWithConfigAppliesPatchAfterDynamicRegister) {
    const auto worker_exe = dztrader::this_process::exe_dir() / "test_worker"
#ifdef _WIN32
        ".exe"
#endif
        ;
    const auto gw_exe = worker_exe.parent_path() / "dzmd_test_worker"
#ifdef _WIN32
        ".exe"
#endif
        ;
    std::filesystem::remove(gw_exe);
    std::filesystem::copy_file(worker_exe, gw_exe);

    auto reader = create_reader("dynreg_cfg_reader");
    auto writer = create_writer("fake_ui");

    write_process_control_frame(writer, platform::ProcessAction::Start, "dzmd_test_worker",
                                nlohmann::json{{"display_name", "新网关"}});
    for (int i = 0; i < 10; ++i) {
        shm_mgr_->drain_event_channel();
    }
    {
        auto frames = drain_all(reader);
        auto cit = frames.find(DZ_FRAME_RTN_PROCESS_CONFIG);
        ASSERT_NE(cit, frames.end());
        const auto& cfg = cit->second.back();
        ASSERT_TRUE(cfg.contains("dzmd_test_worker"));
        // patch 应用在动态注册的默认配置上: 118 为 patch 后全量
        EXPECT_EQ(cfg["dzmd_test_worker"].value("display_name", ""), "新网关");
        EXPECT_TRUE(cfg["dzmd_test_worker"].contains("restart"));
        auto it = frames.find(DZ_FRAME_RTN_PROCESS_STATUS);
        ASSERT_NE(it, frames.end());
        const auto& status = it->second.back();
        EXPECT_EQ(status["event"], "StartSucceeded");
        EXPECT_EQ(status["state"], "Running");
    }
    // registry 条目 display_name 已应用 patch
    const auto* entry = supervisor_->find_registry_entry("dzmd_test_worker");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->display_name, "新网关");

    // 清理: 帧驱动 Stop + 跑 io_context 让 force-kill 定时器触发
    write_process_control_frame(writer, platform::ProcessAction::Stop, "dzmd_test_worker");
    for (int i = 0; i < 10; ++i) {
        shm_mgr_->drain_event_channel();
    }
    ioc_.restart();
    ioc_.run_for(std::chrono::seconds(5));
    std::filesystem::remove(gw_exe);
}

TEST_F(ProcessControlFrameTest, StartUnregisteredNonGatewayExeReturnsStartFailed) {
    auto reader = create_reader("dynreg_non_gw_reader");
    auto writer = create_writer("fake_ui");

    write_process_control_frame(writer, platform::ProcessAction::Start, "test_worker");
    for (int i = 0; i < 10; ++i) {
        shm_mgr_->drain_event_channel();
    }
    {
        auto frames = drain_all(reader);
        auto it = frames.find(DZ_FRAME_RTN_PROCESS_STATUS);
        ASSERT_NE(it, frames.end());
        const auto& status = it->second.back();
        EXPECT_EQ(status["name"], "test_worker");
        EXPECT_EQ(status["event"], "StartFailed");
        EXPECT_EQ(status["state"], "Crashed");
    }
    // 未注册: 不得动态注册 (非网关)
    EXPECT_EQ(supervisor_->find_registry_entry("test_worker"), nullptr);
}

TEST_F(ProcessControlFrameTest, StartRunningPushesStartSucceeded) {
    // 动态注册策略条目 (worker exe 与测试二进制同目录, 见 process_supervisor_test.cpp)
    const auto worker_exe = dztrader::this_process::exe_dir() / "test_worker"
#ifdef _WIN32
        ".exe"
#endif
        ;
    ProcessEntry entry;
    entry.name = "stg_worker";
    entry.category = Category::Strategy;
    entry.exe = worker_exe;
    entry.start_dir = worker_exe.parent_path().empty()
        ? std::filesystem::current_path()
        : worker_exe.parent_path();
    entry.restart = default_restart_policy(Category::Strategy);
    entry.restart.enabled = false;  // 测试场景禁用重启
    registry_.register_strategy(entry);

    auto reader = create_reader("start_running_reader");
    auto writer = create_writer("fake_ui");

    // 第一次 Start: 真实 spawn, 最后一条 116 应为 event=StartSucceeded
    write_process_control_frame(writer, platform::ProcessAction::Start, "stg_worker");
    for (int i = 0; i < 10; ++i) {
        shm_mgr_->drain_event_channel();
    }
    {
        auto frames = drain_all(reader);
        auto it = frames.find(DZ_FRAME_RTN_PROCESS_STATUS);
        ASSERT_NE(it, frames.end());
        const auto& status = it->second.back();  // launch_child 先推自发 116, 最后一条带 event
        EXPECT_EQ(status["name"], "stg_worker");
        EXPECT_EQ(status["event"], "StartSucceeded");
    }

    // 第二次 Start: 已在运行 → 幂等成功 (回归点: 不得推 Crashed/StartFailed)
    write_process_control_frame(writer, platform::ProcessAction::Start, "stg_worker");
    for (int i = 0; i < 10; ++i) {
        shm_mgr_->drain_event_channel();
    }
    {
        auto frames = drain_all(reader);
        auto it = frames.find(DZ_FRAME_RTN_PROCESS_STATUS);
        ASSERT_NE(it, frames.end());
        // 第二次 drain 的 116 中不得出现 StartFailed
        for (const auto& st : it->second) {
            EXPECT_NE(st.value("event", std::string()), "StartFailed");
        }
        const auto& status = it->second.back();
        EXPECT_EQ(status["name"], "stg_worker");
        EXPECT_EQ(status["event"], "StartSucceeded");  // 幂等成功 (P1 回归)
        EXPECT_NE(status["state"], "Crashed");         // 绝不是 StartFailed/Crashed
        EXPECT_EQ(status["state"], "Running");         // 子进程仍在运行 (未重复 spawn)
    }

    // 清理: 帧驱动 Stop + 跑 io_context 让 force-kill 定时器 (3s) 触发, 终止 worker 进程
    // (worker 不读事件通道, 不会自行退出; orphan_guard_ 未 startup 不跟踪子进程)
    write_process_control_frame(writer, platform::ProcessAction::Stop, "stg_worker");
    for (int i = 0; i < 10; ++i) {
        shm_mgr_->drain_event_channel();
    }
    ioc_.restart();
    ioc_.run_for(std::chrono::seconds(5));  // single_stop_timeout_sec_=3, 超时强制 terminate
}

// ---- 帧 1013/1014: 行情通道读者注册/注销 (契约 02-shm) ----

namespace {

/// 向 registry 注册策略条目 (1013/1014 身份校验用)。
void register_strategy_for_test(ProcessRegistry& registry, const std::string& name) {
    ProcessEntry e;
    e.name = name;
    e.category = Category::Strategy;
    e.restart = default_restart_policy(Category::Strategy);
    registry.register_strategy(std::move(e));
}

/// 写 1013/1014 帧 (instance_id=行情源名, payload={"subscriber": ...}) + notify。
void write_md_reader_frame(shm::MultiWriter& w, DzFrameType type, const std::string& source,
                           const std::string& subscriber) {
    nlohmann::json payload = {{"subscriber", subscriber}};
    ASSERT_TRUE(shm::write_ext_inst_json(w, type, source, payload));
    w.notify_subscribers();
}

}  // namespace

// 注册成功: 身份校验通过 + 通道存在 -> readers map 新增条目 + 下发 UPDATE_SHM_MD_SUBSCRIBER
TEST_F(ProcessControlFrameTest, MdReaderRegisterAddsReaderAndNotifies) {
    register_strategy_for_test(registry_, "alpha");
    shm_mgr_->create_md_channel("dzmd_ctp");

    auto writer = create_writer("stg_sim");
    write_md_reader_frame(writer, DZ_FRAME_REQUEST_MD_READER_REGISTER, "dzmd_ctp", "stg.alpha");
    shm_mgr_->drain_event_channel();

    auto meta = shm::ChannelMeta::open_only("dzmd_ctp", dztrader::paths::shm());
    auto names = meta.reader_names();
    EXPECT_TRUE(std::find(names.begin(), names.end(), "stg.alpha") != names.end());
}

// 身份校验: 未注册的 subscriber 被拒绝
TEST_F(ProcessControlFrameTest, MdReaderRegisterRejectsUnknownSubscriber) {
    shm_mgr_->create_md_channel("dzmd_ctp");

    auto writer = create_writer("stg_sim");
    write_md_reader_frame(writer, DZ_FRAME_REQUEST_MD_READER_REGISTER, "dzmd_ctp",
                          "stg.ghost");
    shm_mgr_->drain_event_channel();

    auto meta = shm::ChannelMeta::open_only("dzmd_ctp", dztrader::paths::shm());
    auto names = meta.reader_names();
    EXPECT_TRUE(std::find(names.begin(), names.end(), "stg.ghost") == names.end());
}

// 通道缺失: md 未启动 (start_all 顺序保证外) -> 拒绝且不创建通道
TEST_F(ProcessControlFrameTest, MdReaderRegisterRejectsMissingChannel) {
    register_strategy_for_test(registry_, "alpha");

    auto writer = create_writer("stg_sim");
    write_md_reader_frame(writer, DZ_FRAME_REQUEST_MD_READER_REGISTER, "no_such_md", "stg.alpha");
    shm_mgr_->drain_event_channel();

    // 拒绝路径不创建通道: shm 目录下不应出现该通道目录 (meta.dat)
    EXPECT_FALSE(std::filesystem::exists(dztrader::paths::shm() / "no_such_md"));
}

// 注销: readers map 移除 + 重复注销幂等不抛
TEST_F(ProcessControlFrameTest, MdReaderUnregisterRemovesReaderIdempotent) {
    register_strategy_for_test(registry_, "alpha");
    shm_mgr_->create_md_channel("dzmd_ctp");

    auto writer = create_writer("stg_sim");
    write_md_reader_frame(writer, DZ_FRAME_REQUEST_MD_READER_REGISTER, "dzmd_ctp", "stg.alpha");
    shm_mgr_->drain_event_channel();
    write_md_reader_frame(writer, DZ_FRAME_REQUEST_MD_READER_UNREGISTER, "dzmd_ctp", "stg.alpha");
    shm_mgr_->drain_event_channel();

    auto meta = shm::ChannelMeta::open_only("dzmd_ctp", dztrader::paths::shm());
    auto names = meta.reader_names();
    EXPECT_TRUE(std::find(names.begin(), names.end(), "stg.alpha") == names.end());

    // 重复注销: remove 对缺失 key 为 no-op, 不抛异常 (异常即测试失败)
    write_md_reader_frame(writer, DZ_FRAME_REQUEST_MD_READER_UNREGISTER, "dzmd_ctp", "stg.alpha");
    shm_mgr_->drain_event_channel();
}

// 退出兜底: remove_reader_from_all_md_channels 清理所有通道的该读者
// (经 1013 帧注册 — master 持 creator 角色写 meta, open_only 无权 add_reader)
TEST_F(ProcessControlFrameTest, RemoveReaderFromAllMdChannels) {
    register_strategy_for_test(registry_, "alpha");
    shm_mgr_->create_md_channel("dzmd_ctp");

    auto writer = create_writer("stg_sim");
    write_md_reader_frame(writer, DZ_FRAME_REQUEST_MD_READER_REGISTER, "dzmd_ctp", "stg.alpha");
    shm_mgr_->drain_event_channel();
    shm_mgr_->remove_reader_from_all_md_channels("stg.alpha");

    auto meta = shm::ChannelMeta::open_only("dzmd_ctp", dztrader::paths::shm());
    auto names = meta.reader_names();
    EXPECT_TRUE(std::find(names.begin(), names.end(), "stg.alpha") == names.end());
}

}  // namespace
}  // namespace dztrader::master
