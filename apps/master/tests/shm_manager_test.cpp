#include "shm_manager.h"

#include <dztrader/core/core_data_type.h>
#include <dztrader/core/core_struct.h>
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
#include <random>
#include <thread>
#include <vector>

namespace dztrader::master {
namespace {

// 测试用默认配置
ShmGlobalConfig make_default_shm_global() {
    return {.meta_file_size = 1 * 1024 * 1024};
}

/// 进程唯一临时目录名（PID + 随机数）：ctest -j 并行时避免多个测试 exe
/// 共用固定目录名（如 dz_shm_mgr_test / dz_pc_frame_test）导致 SHM 文件互相占用冲突。
std::filesystem::path unique_temp_dir(const std::string& name) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dist;
    return std::filesystem::temp_directory_path() /
           (name + "_" + std::to_string(static_cast<uint32_t>(dztrader::this_process::pid())) +
            "_" + std::to_string(dist(gen)));
}

class ShmManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        tmp_dir_ = unique_temp_dir("dz_shm_mgr_test");
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
    // 构造后 event_writer_/event_reader_ 可用：send_shutdown 能写入定向 SHUTDOWN
    EXPECT_NO_THROW(mgr.send_shutdown("dztraderd"));
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
        if (view.type() == DZ_FRAME_SHUTDOWN) {
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
    EXPECT_NO_THROW(mgr.send_shutdown("dztraderd"));
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
            // RTN 无 instance_id (契约 shm：事件通道帧头无 instance_id), 用 ext_payload 读取
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
    // 注: paths::configs() 为进程级缓存 (首个调用者的 DZTRADER_HOME), 不随本测试
    // tmp_dir_ 变化, 故配置文件必须写进缓存目录 (读写同源, 否则全量运行时读到
    // 别的测试目录而误判缺失)。
    std::filesystem::create_directories(dztrader::paths::configs());
    std::ofstream(dztrader::paths::configs() / "test_source.json")
        << R"({"shm": {"page_size_mb": 512}})";

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
    // 同上: 配置文件必须写进 paths::configs() 缓存目录 (读写同源)
    std::filesystem::create_directories(dztrader::paths::configs());
    // 写入格式错误的 json (未闭合字符串)
    std::ofstream(dztrader::paths::configs() / "malformed.json")
        << R"({"shm": {"page_size_mb": "unclosed})";

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
    /// 每进程唯一动态注册网关名（PID + 随机数）：避免多个并行用例/测试 exe 复制删除
    /// 同名的 dzmd_test_worker*.exe（exe_dir 共享）互相冲突。以 dzmd_ 前缀供扫描器判 GatewayMd。
    std::string gw_name_;

    void SetUp() override {
        tmp_dir_ = unique_temp_dir("dz_pc_frame_test");
        std::filesystem::remove_all(tmp_dir_);
        std::filesystem::create_directories(tmp_dir_);
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<uint32_t> dist;
        gw_name_ = "dzmd_test_worker_" +
                   std::to_string(static_cast<uint32_t>(dztrader::this_process::pid())) + "_" +
                   std::to_string(dist(gen));
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
    // 失败路径必须通过 NOTIFY_UI 反馈 (契约 process: 错误级别弹窗; 契约 notify-ui: level 为字符串)
    auto nit = frames.find(DZ_FRAME_NOTIFY_UI);
    ASSERT_NE(nit, frames.end());
    EXPECT_EQ(nit->second.back()["level"], "error");
}

// 2. 已注册但未启动 -> Stop 幂等成功 (契约 process)
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

// 3. Remove 未注册 target: 116{event=RemoveFailed} (不幂等, 契约 process) + NOTIFY_UI
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
    // 契约 process: Remove 未注册 target 必须错误级别弹窗 (契约 notify-ui: level 为字符串)
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
    // 118 始终全量镜像 (契约 process)
    const auto& cfg = it->second.back();
    ASSERT_TRUE(cfg.contains("dzmd_ctp"));
    EXPECT_EQ(cfg["dzmd_ctp"]["display_name"], "新名");
    EXPECT_TRUE(cfg["dzmd_ctp"]["args"].is_array());    // 全量字段保留
    EXPECT_TRUE(cfg["dzmd_ctp"]["env"].is_object());
    EXPECT_TRUE(cfg["dzmd_ctp"]["restart"].is_object());
}

// 策略条目 SET_PROCESS_CONFIG: 持久化写 strategy section (非 md.<name> 段),
// 且保留既有 md_source/exe (修复: 编辑运行配置写丢绑定源的 BUG)。
TEST_F(ProcessControlFrameTest, SetProcessConfigOnStrategyPersistsToStrategySection) {
    // 向 registry 动态注册带 md_source 的策略条目, 然后重建 manager 栈,
    // 使 store 初始镜像 (build_initial_config_map) 包含该策略 (经 set_supervisor 加载)。
    ProcessEntry entry;
    entry.name = "stg_persist";
    entry.category = Category::Strategy;
    entry.md_source = "dzmd_ctp";
    entry.restart = default_restart_policy(Category::Strategy);
    entry.restart.enabled = false;
    registry_.register_strategy(entry);
    supervisor_.reset();
    shm_mgr_.reset();
    shm_mgr_ = std::make_unique<ShmManager>(make_default_shm_global(), cfg_path_);
    supervisor_ = std::make_unique<ProcessSupervisor>(ioc_, registry_, *shm_mgr_, orphan_guard_);
    shm_mgr_->set_supervisor(supervisor_.get());

    auto reader = create_reader("set_stg_cfg_reader");
    auto writer = create_writer("fake_ui");

    // 更新策略的运行配置 (args), 不含 md_source —— md_source 必须保留
    write_set_process_config_frame(writer, "stg_persist",
                                   nlohmann::json{{"args", nlohmann::json::array({"--x"})}});
    for (int i = 0; i < 10; ++i) {
        shm_mgr_->drain_event_channel();
    }

    // 持久化: dztraderd.json 的 strategy section 包含该条目且 md_source 保留
    nlohmann::json cfg;
    {
        std::ifstream ifs(cfg_path_);
        ifs >> cfg;
    }
    ASSERT_TRUE(cfg.contains("strategy") && cfg["strategy"].is_array());
    bool found = false;
    for (const auto& item : cfg["strategy"]) {
        if (item.value("name", "") == "stg_persist") {
            found = true;
            EXPECT_EQ(item.value("md_source", ""), "dzmd_ctp");
            EXPECT_EQ(item["args"], nlohmann::json::array({"--x"}));
        }
    }
    EXPECT_TRUE(found) << "strategy entry missing from strategy section";
    // 不得误写 md.stg_persist 段
    EXPECT_FALSE(cfg.contains("md") && cfg["md"].contains("stg_persist"));
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
    // 118 推旧值 (契约 process: 失败回滚旧 map)
    auto it = frames.find(DZ_FRAME_RTN_PROCESS_CONFIG);
    ASSERT_NE(it, frames.end());
    EXPECT_EQ(it->second.back()["dzmd_ctp"]["display_name"], "CTP行情");
    // 失败路径带 NOTIFY_UI (error, 契约 notify-ui: level 为字符串)
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
    // 116: 注册进程状态, 无 event 字段 (自发状态变化, 契约 process)
    auto sit = frames.find(DZ_FRAME_RTN_PROCESS_STATUS);
    ASSERT_NE(sit, frames.end());
    EXPECT_GE(sit->second.size(), 1u);
    for (const auto& st : sit->second) {
        EXPECT_FALSE(st.contains("event"));
    }
}

// 7. Start 已运行进程 → 幂等 StartSucceeded (P1 回归: 不重复 spawn, 不推 Crashed/StartFailed)
// 第一次 Start 真实 spawn test_worker (长驻); 第二次 Start 必须走幂等分支 (契约 process)。
// 契约 process 修订: 未注册 target 但 App Root 下存在同名网关 exe (dzmd_*)
// -> 动态注册 (registry + dztraderd.json 持久化 + 118 全量) 后正常启动 (116 Running/StartSucceeded)。
TEST_F(ProcessControlFrameTest, StartUnregisteredGatewayDynamicallyRegistersAndStarts) {
    const auto worker_exe = dztrader::this_process::exe_dir() / "test_worker"
#ifdef _WIN32
        ".exe"
#endif
        ;
    // 复制为 dzmd_ 前缀网关 exe (扫描器按前缀判 GatewayMd), 用例结束后清理
    const auto gw_exe = worker_exe.parent_path() /
                        (gw_name_
#ifdef _WIN32
                         + ".exe"
#endif
                        );
    std::filesystem::remove(gw_exe);
    std::filesystem::copy_file(worker_exe, gw_exe);

    auto reader = create_reader("dynreg_reader");
    auto writer = create_writer("fake_ui");

    // 未注册 Start: 扫描命中 -> 动态注册 + 启动
    write_process_control_frame(writer, platform::ProcessAction::Start, gw_name_);
    for (int i = 0; i < 10; ++i) {
        shm_mgr_->drain_event_channel();
    }
    {
        auto frames = drain_all(reader);
        // 118 全量含新条目 (dzweb 注册守卫依赖; 必须先于 116)
        auto cit = frames.find(DZ_FRAME_RTN_PROCESS_CONFIG);
        ASSERT_NE(cit, frames.end());
        const auto& cfg = cit->second.back();
        EXPECT_TRUE(cfg.contains(gw_name_));
        EXPECT_TRUE(cfg[gw_name_].contains("restart"));
        // 116 Running + StartSucceeded
        auto it = frames.find(DZ_FRAME_RTN_PROCESS_STATUS);
        ASSERT_NE(it, frames.end());
        const auto& status = it->second.back();
        EXPECT_EQ(status["name"], gw_name_);
        EXPECT_EQ(status["event"], "StartSucceeded");
        EXPECT_EQ(status["state"], "Running");
    }
    // registry 已含新条目
    EXPECT_NE(supervisor_->find_registry_entry(gw_name_), nullptr);
    // dztraderd.json 已持久化 md 段 (下次 master 启动自动拉起)
    {
        std::ifstream ifs(cfg_path_);
        nlohmann::json j;
        ifs >> j;
        EXPECT_TRUE(j.contains("md"));
        EXPECT_TRUE(j["md"].contains(gw_name_));
    }

    // 清理: 帧驱动 Stop + 跑 io_context 让 force-kill 定时器触发 (worker 不读事件通道)
    write_process_control_frame(writer, platform::ProcessAction::Stop, gw_name_);
    for (int i = 0; i < 10; ++i) {
        shm_mgr_->drain_event_channel();
    }
    ioc_.restart();
    ioc_.run_for(std::chrono::seconds(5));
    std::filesystem::remove(gw_exe);
}

// 契约 process 修订: 未注册 target 扫描命中但非网关进程 (test_worker 无前缀, 策略有独立注册流程)
// -> 回 StartFailed, 不 spawn。
// 契约 process 修订组合: 未注册网关 + Start 携带 config patch
// -> 先动态注册（默认配置）再应用 patch, 118 全量为 patch 后的值。
TEST_F(ProcessControlFrameTest, StartUnregisteredGatewayWithConfigAppliesPatchAfterDynamicRegister) {
    const auto worker_exe = dztrader::this_process::exe_dir() / "test_worker"
#ifdef _WIN32
        ".exe"
#endif
        ;
    const auto gw_exe = worker_exe.parent_path() /
                        (gw_name_
#ifdef _WIN32
                         + ".exe"
#endif
                        );
    std::filesystem::remove(gw_exe);
    std::filesystem::copy_file(worker_exe, gw_exe);

    auto reader = create_reader("dynreg_cfg_reader");
    auto writer = create_writer("fake_ui");

    write_process_control_frame(writer, platform::ProcessAction::Start, gw_name_,
                                nlohmann::json{{"display_name", "新网关"}});
    for (int i = 0; i < 10; ++i) {
        shm_mgr_->drain_event_channel();
    }
    {
        auto frames = drain_all(reader);
        auto cit = frames.find(DZ_FRAME_RTN_PROCESS_CONFIG);
        ASSERT_NE(cit, frames.end());
        const auto& cfg = cit->second.back();
        ASSERT_TRUE(cfg.contains(gw_name_));
        // patch 应用在动态注册的默认配置上: 118 为 patch 后全量
        EXPECT_EQ(cfg[gw_name_].value("display_name", ""), "新网关");
        EXPECT_TRUE(cfg[gw_name_].contains("restart"));
        auto it = frames.find(DZ_FRAME_RTN_PROCESS_STATUS);
        ASSERT_NE(it, frames.end());
        const auto& status = it->second.back();
        EXPECT_EQ(status["event"], "StartSucceeded");
        EXPECT_EQ(status["state"], "Running");
    }
    // registry 条目 display_name 已应用 patch
    const auto* entry = supervisor_->find_registry_entry(gw_name_);
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->display_name, "新网关");

    // 清理: 帧驱动 Stop + 跑 io_context 让 force-kill 定时器触发
    write_process_control_frame(writer, platform::ProcessAction::Stop, gw_name_);
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

// ---- 帧 1013/1014: 行情通道读者注册/注销 (契约 shm) ----

namespace {

/// 向 registry 注册策略条目 (1013/1014 身份校验用)。
void register_strategy_for_test(ProcessRegistry& registry, const std::string& name) {
    ProcessEntry e;
    e.name = name;
    e.category = Category::Strategy;
    e.restart = default_restart_policy(Category::Strategy);
    registry.register_strategy(std::move(e));
}

/// 向 registry 注册内部进程条目 (1013/1014 身份校验用, 任意类别非策略)。
void register_internal_for_test(ProcessRegistry& registry, const std::string& name) {
    ProcessEntry e;
    e.name = name;
    e.category = Category::GatewayTd;
    e.restart = default_restart_policy(Category::GatewayTd);
    registry.register_gateway(std::move(e));
}

/// 写 NOTIFY_MD_STARTED 帧 (instance_id=行情源名) 模拟行情进程就绪宣告 + notify。
void mark_channel_ready(shm::MultiWriter& w, const std::string& source) {
    platform::write_ext_inst_raw(w, DZ_FRAME_NOTIFY_MD_STARTED, source);
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
    mark_channel_ready(writer, "dzmd_ctp");
    shm_mgr_->drain_event_channel();
    write_md_reader_frame(writer, DZ_FRAME_REQUEST_MD_READER_REGISTER, "dzmd_ctp", "stg.alpha");
    shm_mgr_->drain_event_channel();

    auto meta = shm::ChannelMeta::open_only("dzmd_ctp", dztrader::paths::shm());
    auto names = meta.reader_names();
    EXPECT_TRUE(std::find(names.begin(), names.end(), "stg.alpha") != names.end());
}

// 身份校验: 未注册的 subscriber 被拒绝, 且回 RTN 精确文案 "unknown subscriber"
TEST_F(ProcessControlFrameTest, MdReaderRegisterRejectsUnknownSubscriber) {
    shm_mgr_->create_md_channel("dzmd_ctp");

    auto writer = create_writer("stg_sim");
    shm::Reader reader = create_reader("rtn_unknown_sub");
    mark_channel_ready(writer, "dzmd_ctp");
    shm_mgr_->drain_event_channel();
    write_md_reader_frame(writer, DZ_FRAME_REQUEST_MD_READER_REGISTER, "dzmd_ctp",
                          "stg.ghost");
    shm_mgr_->drain_event_channel();

    auto meta = shm::ChannelMeta::open_only("dzmd_ctp", dztrader::paths::shm());
    auto names = meta.reader_names();
    EXPECT_TRUE(std::find(names.begin(), names.end(), "stg.ghost") == names.end());

    // RTN 精确文案: unknown subscriber (身份校验失败)
    bool rtn_msg_ok = false;
    for (int i = 0; i < 16; ++i) {
        const auto* frame = reader.next_frame();
        if (!frame) break;
        shm::FrameView view(frame);
        if (view.type() == DZ_FRAME_RTN_MD_READER_REGISTER &&
            std::string_view(view.ext_inst_id()) == "stg.ghost") {
            auto j = shm::decode_ext_inst_json<nlohmann::json>(view);
            rtn_msg_ok = !j.value("ok", true) &&
                         j.value("message", std::string{}) == "unknown subscriber";
        }
    }
    EXPECT_TRUE(rtn_msg_ok);
}

// 通道缺失: md 未启动 (start_all 顺序保证外) -> 拒绝且不创建通道, 回 RTN "channel not configured"
TEST_F(ProcessControlFrameTest, MdReaderRegisterRejectsMissingChannel) {
    register_strategy_for_test(registry_, "alpha");

    auto writer = create_writer("stg_sim");
    shm::Reader reader = create_reader("rtn_missing_ch");
    write_md_reader_frame(writer, DZ_FRAME_REQUEST_MD_READER_REGISTER, "no_such_md", "stg.alpha");
    shm_mgr_->drain_event_channel();

    // 拒绝路径不创建通道: shm 目录下不应出现该通道目录 (meta.dat)
    EXPECT_FALSE(std::filesystem::exists(dztrader::paths::shm() / "no_such_md"));

    // RTN 精确文案: channel not configured (通道未配置)
    bool rtn_msg_ok = false;
    for (int i = 0; i < 16; ++i) {
        const auto* frame = reader.next_frame();
        if (!frame) break;
        shm::FrameView view(frame);
        if (view.type() == DZ_FRAME_RTN_MD_READER_REGISTER &&
            std::string_view(view.ext_inst_id()) == "stg.alpha") {
            auto j = shm::decode_ext_inst_json<nlohmann::json>(view);
            rtn_msg_ok = !j.value("ok", true) &&
                         j.value("message", std::string{}) == "channel not configured";
        }
    }
    EXPECT_TRUE(rtn_msg_ok);
}

// 注销: readers map 移除 + 重复注销幂等不抛
TEST_F(ProcessControlFrameTest, MdReaderUnregisterRemovesReaderIdempotent) {
    register_strategy_for_test(registry_, "alpha");
    shm_mgr_->create_md_channel("dzmd_ctp");

    auto writer = create_writer("stg_sim");
    mark_channel_ready(writer, "dzmd_ctp");
    shm_mgr_->drain_event_channel();
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
    mark_channel_ready(writer, "dzmd_ctp");
    shm_mgr_->drain_event_channel();
    write_md_reader_frame(writer, DZ_FRAME_REQUEST_MD_READER_REGISTER, "dzmd_ctp", "stg.alpha");
    shm_mgr_->drain_event_channel();
    shm_mgr_->remove_reader_from_all_md_channels("stg.alpha");

    auto meta = shm::ChannelMeta::open_only("dzmd_ctp", dztrader::paths::shm());
    auto names = meta.reader_names();
    EXPECT_TRUE(std::find(names.begin(), names.end(), "stg.alpha") == names.end());
}

// 通道未就绪 (行情进程未发 NOTIFY_MD_STARTED): 拒绝接入, 读者不入列表, 回 RTN "channel not ready"
TEST_F(ProcessControlFrameTest, MdReaderRegisterRejectsNotReadyChannel) {
    register_strategy_for_test(registry_, "alpha");
    shm_mgr_->create_md_channel("dzmd_ctp");  // 建通道但不宣告就绪

    auto writer = create_writer("stg_sim");
    shm::Reader reader = create_reader("rtn_not_ready");
    write_md_reader_frame(writer, DZ_FRAME_REQUEST_MD_READER_REGISTER, "dzmd_ctp", "stg.alpha");
    shm_mgr_->drain_event_channel();

    auto meta = shm::ChannelMeta::open_only("dzmd_ctp", dztrader::paths::shm());
    auto names = meta.reader_names();
    EXPECT_TRUE(std::find(names.begin(), names.end(), "stg.alpha") == names.end());

    // RTN 精确文案: channel not ready (通道未就绪)
    bool rtn_msg_ok = false;
    for (int i = 0; i < 16; ++i) {
        const auto* frame = reader.next_frame();
        if (!frame) break;
        shm::FrameView view(frame);
        if (view.type() == DZ_FRAME_RTN_MD_READER_REGISTER &&
            std::string_view(view.ext_inst_id()) == "stg.alpha") {
            auto j = shm::decode_ext_inst_json<nlohmann::json>(view);
            rtn_msg_ok = !j.value("ok", true) &&
                         j.value("message", std::string{}) == "channel not ready";
        }
    }
    EXPECT_TRUE(rtn_msg_ok);
}

// 坏 payload 拒绝路径: 缺 subscriber 字段的 1013 帧被忽略 (无法获知请求方身份, 不回 RTN)
TEST_F(ProcessControlFrameTest, MdReaderRegisterBadPayloadIsIgnored) {
    register_strategy_for_test(registry_, "alpha");
    shm_mgr_->create_md_channel("dzmd_ctp");

    auto writer = create_writer("stg_sim");
    mark_channel_ready(writer, "dzmd_ctp");
    shm_mgr_->drain_event_channel();
    shm::Reader reader = create_reader("rtn_bad_payload");

    // 写缺 subscriber 字段的坏 payload 帧 (契约 shm bad_payload 路径: 不崩溃且不回 RTN)
    nlohmann::json payload = {{"foo", 1}};
    ASSERT_TRUE(shm::write_ext_inst_json(writer, DZ_FRAME_REQUEST_MD_READER_REGISTER, "dzmd_ctp",
                                         payload));
    writer.notify_subscribers();
    shm_mgr_->drain_event_channel();

    // 读者不入列
    auto meta = shm::ChannelMeta::open_only("dzmd_ctp", dztrader::paths::shm());
    auto names = meta.reader_names();
    EXPECT_TRUE(std::find(names.begin(), names.end(), "stg.alpha") == names.end());

    // 无法获知请求方身份 -> 不回 RTN
    bool saw_rtn = false;
    for (int i = 0; i < 16; ++i) {
        const auto* frame = reader.next_frame();
        if (!frame) break;
        shm::FrameView view(frame);
        if (view.type() == DZ_FRAME_RTN_MD_READER_REGISTER) {
            saw_rtn = true;
        }
    }
    EXPECT_FALSE(saw_rtn);
}

// 任意已注册进程可接入 (不限策略): 内部进程裸名身份注册成功
TEST_F(ProcessControlFrameTest, MdReaderRegisterAllowsNonStrategyProcess) {
    register_internal_for_test(registry_, "dzstore");
    shm_mgr_->create_md_channel("dzmd_ctp");

    auto writer = create_writer("dzstore_sim");
    mark_channel_ready(writer, "dzmd_ctp");
    shm_mgr_->drain_event_channel();
    write_md_reader_frame(writer, DZ_FRAME_REQUEST_MD_READER_REGISTER, "dzmd_ctp", "dzstore");
    shm_mgr_->drain_event_channel();

    auto meta = shm::ChannelMeta::open_only("dzmd_ctp", dztrader::paths::shm());
    auto names = meta.reader_names();
    EXPECT_TRUE(std::find(names.begin(), names.end(), "dzstore") != names.end());
}

// 注册成功必回 RTN (1015): instance_id=请求进程名, payload ok=true
TEST_F(ProcessControlFrameTest, MdReaderRegisterWritesSuccessRtn) {
    register_strategy_for_test(registry_, "alpha");
    shm_mgr_->create_md_channel("dzmd_ctp");

    auto writer = create_writer("stg_sim");
    shm::Reader reader = create_reader("rtn_probe1");
    mark_channel_ready(writer, "dzmd_ctp");
    shm_mgr_->drain_event_channel();
    write_md_reader_frame(writer, DZ_FRAME_REQUEST_MD_READER_REGISTER, "dzmd_ctp", "stg.alpha");
    shm_mgr_->drain_event_channel();

    bool rtn_ok = false;
    for (int i = 0; i < 16; ++i) {
        const auto* frame = reader.next_frame();
        if (!frame) break;
        shm::FrameView view(frame);
        if (view.type() == DZ_FRAME_RTN_MD_READER_REGISTER &&
            std::string_view(view.ext_inst_id()) == "stg.alpha") {
            auto j = shm::decode_ext_inst_json<nlohmann::json>(view);
            rtn_ok = j.value("ok", false) && j.value("channel", "") == "dzmd_ctp";
        }
    }
    EXPECT_TRUE(rtn_ok);
}

// 注册失败必回 RTN: ok=false 且 message 必填 (此处: 通道未就绪)
TEST_F(ProcessControlFrameTest, MdReaderRegisterWritesFailRtnWithMessage) {
    register_strategy_for_test(registry_, "alpha");
    shm_mgr_->create_md_channel("dzmd_ctp");  // 未就绪

    auto writer = create_writer("stg_sim");
    shm::Reader reader = create_reader("rtn_probe2");
    write_md_reader_frame(writer, DZ_FRAME_REQUEST_MD_READER_REGISTER, "dzmd_ctp", "stg.alpha");
    shm_mgr_->drain_event_channel();

    bool rtn_fail = false;
    for (int i = 0; i < 16; ++i) {
        const auto* frame = reader.next_frame();
        if (!frame) break;
        shm::FrameView view(frame);
        if (view.type() == DZ_FRAME_RTN_MD_READER_REGISTER &&
            std::string_view(view.ext_inst_id()) == "stg.alpha") {
            auto j = shm::decode_ext_inst_json<nlohmann::json>(view);
            rtn_fail = !j.value("ok", true) && !j.value("message", std::string{}).empty();
        }
    }
    EXPECT_TRUE(rtn_fail);
}

// 通道已配置但已关闭 (行情进程未运行, meta==null): 拒绝注册并回失败 RTN
TEST_F(ProcessControlFrameTest, MdReaderRegisterRejectsClosedChannel) {
    register_strategy_for_test(registry_, "alpha");
    shm_mgr_->create_md_channel("dzmd_ctp");
    shm_mgr_->close_md_channel("dzmd_ctp");  // 停止后果: 句柄释放

    auto writer = create_writer("stg_sim");
    shm::Reader reader = create_reader("rtn_probe4");
    write_md_reader_frame(writer, DZ_FRAME_REQUEST_MD_READER_REGISTER, "dzmd_ctp", "stg.alpha");
    shm_mgr_->drain_event_channel();

    bool rtn_fail = false;
    for (int i = 0; i < 16; ++i) {
        const auto* frame = reader.next_frame();
        if (!frame) break;
        shm::FrameView view(frame);
        if (view.type() == DZ_FRAME_RTN_MD_READER_REGISTER &&
            std::string_view(view.ext_inst_id()) == "stg.alpha") {
            auto j = shm::decode_ext_inst_json<nlohmann::json>(view);
            rtn_fail = !j.value("ok", true) &&
                       j.value("message", std::string{}) == "market process not running";
        }
    }
    EXPECT_TRUE(rtn_fail);
}

// 注销对已关闭/不存在通道幂等成功: 回 RTN ok=true
TEST_F(ProcessControlFrameTest, MdReaderUnregisterMissingChannelReturnsOkRtn) {
    register_strategy_for_test(registry_, "alpha");

    auto writer = create_writer("stg_sim");
    shm::Reader reader = create_reader("rtn_probe3");
    write_md_reader_frame(writer, DZ_FRAME_REQUEST_MD_READER_UNREGISTER, "dzmd_ctp", "stg.alpha");
    shm_mgr_->drain_event_channel();

    bool rtn_ok = false;
    for (int i = 0; i < 16; ++i) {
        const auto* frame = reader.next_frame();
        if (!frame) break;
        shm::FrameView view(frame);
        if (view.type() == DZ_FRAME_RTN_MD_READER_UNREGISTER &&
            std::string_view(view.ext_inst_id()) == "stg.alpha") {
            auto j = shm::decode_ext_inst_json<nlohmann::json>(view);
            rtn_ok = j.value("ok", false) && j.value("channel", "") == "dzmd_ctp";
        }
    }
    EXPECT_TRUE(rtn_ok);
}

// 停止后果: close_md_channel 标记 Stopped 但保留读者表与句柄 (契约 4.3/4.7,
// 策略 reader 与进程同生命周期, PageCleaner 仍需遍历); 未就绪时读者接入被拒;
// 重建+就绪后可再接入。
TEST_F(ProcessControlFrameTest, CloseMdChannelKeepsReadersAndRejectsUntilReady) {
    register_strategy_for_test(registry_, "alpha");
    shm_mgr_->create_md_channel("dzmd_ctp");

    auto writer = create_writer("stg_sim");
    mark_channel_ready(writer, "dzmd_ctp");
    shm_mgr_->drain_event_channel();
    write_md_reader_frame(writer, DZ_FRAME_REQUEST_MD_READER_REGISTER, "dzmd_ctp", "stg.alpha");
    shm_mgr_->drain_event_channel();

    // 关闭: 标记 Stopped, 读者表保留 (契约: 停止/删除不清空 readers 表)
    shm_mgr_->close_md_channel("dzmd_ctp");
    {
        auto* state = shm_mgr_->md_channel_state("dzmd_ctp");
        ASSERT_NE(state, nullptr);
        EXPECT_EQ(state->status, MdChannelStatus::Stopped);
        auto meta = shm::ChannelMeta::open_only("dzmd_ctp", dztrader::paths::shm());
        auto names = meta.reader_names();
        EXPECT_TRUE(std::find(names.begin(), names.end(), "stg.alpha") != names.end());
    }

    // 关闭(Stopped)后接入被拒 (行情进程未运行), 不新增读者
    write_md_reader_frame(writer, DZ_FRAME_REQUEST_MD_READER_REGISTER, "dzmd_ctp", "stg.alpha");
    shm_mgr_->drain_event_channel();
    {
        auto meta = shm::ChannelMeta::open_only("dzmd_ctp", dztrader::paths::shm());
        auto names = meta.reader_names();
        EXPECT_TRUE(std::find(names.begin(), names.end(), "stg.alpha") != names.end());
    }

    // 重建 (重启路径) + 就绪后接入成功
    shm_mgr_->create_md_channel("dzmd_ctp");
    mark_channel_ready(writer, "dzmd_ctp");
    shm_mgr_->drain_event_channel();
    write_md_reader_frame(writer, DZ_FRAME_REQUEST_MD_READER_REGISTER, "dzmd_ctp", "stg.alpha");
    shm_mgr_->drain_event_channel();
    {
        auto meta = shm::ChannelMeta::open_only("dzmd_ctp", dztrader::paths::shm());
        auto names = meta.reader_names();
        EXPECT_TRUE(std::find(names.begin(), names.end(), "stg.alpha") != names.end());
    }
}

// 移除语义: destroy_md_channel 清读者+删通道目录+删条目, 再注册回 channel not configured
TEST_F(ProcessControlFrameTest, DestroyMdChannelRemovesFilesAndEntry) {
    register_strategy_for_test(registry_, "alpha");
    shm_mgr_->create_md_channel("dzmd_ctp");
    const auto ch_dir = dztrader::paths::shm() / "dzmd_ctp";
    ASSERT_TRUE(std::filesystem::exists(ch_dir));

    shm_mgr_->destroy_md_channel("dzmd_ctp");

    // 通道目录已删除
    EXPECT_FALSE(std::filesystem::exists(ch_dir));

    // 条目已删除: 再注册回 "channel not configured"
    auto writer = create_writer("stg_sim");
    shm::Reader reader = create_reader("rtn_probe_destroy");
    write_md_reader_frame(writer, DZ_FRAME_REQUEST_MD_READER_REGISTER, "dzmd_ctp", "stg.alpha");
    shm_mgr_->drain_event_channel();

    bool rtn_conf = false;
    for (int i = 0; i < 16; ++i) {
        const auto* frame = reader.next_frame();
        if (!frame) break;
        shm::FrameView view(frame);
        if (view.type() == DZ_FRAME_RTN_MD_READER_REGISTER &&
            std::string_view(view.ext_inst_id()) == "stg.alpha") {
            auto j = shm::decode_ext_inst_json<nlohmann::json>(view);
            rtn_conf = !j.value("ok", true) &&
                       j.value("message", std::string{}) == "channel not configured";
        }
    }
    EXPECT_TRUE(rtn_conf);
}

// destroy 幂等: 重复调用不抛、无副作用
TEST_F(ProcessControlFrameTest, DestroyMdChannelIdempotent) {
    shm_mgr_->create_md_channel("dzmd_ctp");
    shm_mgr_->destroy_md_channel("dzmd_ctp");
    EXPECT_NO_THROW(shm_mgr_->destroy_md_channel("dzmd_ctp"));  // 条目不存在 no-op
}

// page_size 变更约束 (契约 4.8): 存在运行中绑定策略时拒绝启动 md (错误信息列出策略名);
// 无运行中策略时允许重置 (last_page_size 更新, readers 清空)。
TEST_F(ProcessControlFrameTest, PageSizeChangeRejectedWithRunningBoundStrategy) {
    // 注册运行中绑定策略 (test_worker 为真实可 spawn 的进程)
    const auto worker_exe = dztrader::this_process::exe_dir() / "test_worker"
#ifdef _WIN32
        ".exe"
#endif
        ;
    ProcessEntry entry;
    entry.name = "stg_ps";
    entry.category = Category::Strategy;
    entry.exe = worker_exe;
    entry.start_dir = worker_exe.parent_path();
    entry.restart = default_restart_policy(Category::Strategy);
    entry.restart.enabled = false;
    entry.md_source = "dzmd_ctp";
    registry_.register_strategy(entry);

    // 建通道 (page_size 默认 1024MB) + 宣告就绪 + 启动策略
    shm_mgr_->create_md_channel("dzmd_ctp");
    auto ready_writer = create_writer("ps_ready_sim");
    mark_channel_ready(ready_writer, "dzmd_ctp");
    shm_mgr_->drain_event_channel();
    ProcessSupervisor supervisor(ioc_, registry_, *shm_mgr_, orphan_guard_);
    shm_mgr_->set_supervisor(&supervisor);
    ASSERT_TRUE(supervisor.start_process("stg_ps"));
    ASSERT_NE(supervisor.find_child("stg_ps"), nullptr);

    // 人工改配置文件 page_size=2MB -> create_md_channel 检测到变更 + 运行中绑定策略 -> 拒绝
    const auto cfg_json = dztrader::paths::configs() / "dzmd_ctp.json";
    std::filesystem::create_directories(dztrader::paths::configs());
    std::ofstream(cfg_json) << R"({"shm": {"page_size_mb": 2}})";
    bool rejected = false;
    try {
        shm_mgr_->close_md_channel("dzmd_ctp");  // 模拟 md 停止后重启路径
        shm_mgr_->create_md_channel("dzmd_ctp");
    } catch (const std::exception& e) {
        rejected = true;
        EXPECT_NE(std::string(e.what()).find("stg_ps"), std::string::npos)
            << "error must list bound strategy names: " << e.what();
    }
    EXPECT_TRUE(rejected) << "page_size change must be rejected with running bound strategy";

    // 清理: 停止策略 (页大小校验走 Stopped 状态, 此处策略仍在运行, 直接强停)
    supervisor.shutdown();
    ioc_.restart();
    ioc_.run_for(std::chrono::seconds(5));
}

// page_size 变更正向路径 (契约 4.8): 无运行中绑定策略时允许重置,
// readers 清空 (全量 clear 时机 2) 且 last_page_size 更新。
// 注: 配置文件写入与读取均经 paths::configs() (进程级缓存), 前后一致; 页大小
// 取 2MB/3MB 与前序用例泄漏值 (2MB) 及默认值 (1024MB) 均可区分, 保证确定性。
TEST_F(ProcessControlFrameTest, PageSizeChangeResetsWhenNoRunningStrategy) {
    register_strategy_for_test(registry_, "alpha");
    const auto cfg_json = dztrader::paths::configs() / "dzmd_ctp.json";
    std::filesystem::create_directories(dztrader::paths::configs());

    // 第一次创建: 2MB
    std::ofstream(cfg_json) << R"({"shm": {"page_size_mb": 2}})";
    shm_mgr_->create_md_channel("dzmd_ctp");
    {
        auto* state = shm_mgr_->md_channel_state("dzmd_ctp");
        ASSERT_NE(state, nullptr);
        ASSERT_EQ(state->last_page_size, 2u * 1024 * 1024);
        // 预注册一个读者 (模拟策略 reader 残留)
        (void)state->meta->add_reader("stg.alpha", /*pid=*/0);
    }

    // 改配置 page_size=3MB -> 无运行中策略 -> close + create 触发重置
    std::ofstream(cfg_json) << R"({"shm": {"page_size_mb": 3}})";
    shm_mgr_->close_md_channel("dzmd_ctp");
    EXPECT_NO_THROW(shm_mgr_->create_md_channel("dzmd_ctp"));

    // last_page_size 更新为 3MB, readers 清空 (全量 clear 时机 2)
    {
        auto* state = shm_mgr_->md_channel_state("dzmd_ctp");
        ASSERT_NE(state, nullptr);
        EXPECT_EQ(state->last_page_size, 3u * 1024 * 1024);
        auto meta = shm::ChannelMeta::open_only("dzmd_ctp", dztrader::paths::shm());
        EXPECT_TRUE(meta.reader_names().empty());
    }
}

// close 保留文件, destroy 删除: 生命周期语义区分
TEST_F(ProcessControlFrameTest, CloseKeepsFilesDestroyRemoves) {
    shm_mgr_->create_md_channel("dzmd_ctp");
    const auto ch_dir = dztrader::paths::shm() / "dzmd_ctp";

    shm_mgr_->close_md_channel("dzmd_ctp");
    EXPECT_TRUE(std::filesystem::exists(ch_dir));  // 停止保留文件

    shm_mgr_->destroy_md_channel("dzmd_ctp");
    EXPECT_FALSE(std::filesystem::exists(ch_dir));  // 移除删除文件
}

// Remove 未运行进程(兜底路径 notify_removed_for_inactive): 配置段删除 + 118 条目消失
// + 116 RemoveSucceeded + 通道标记 tombstone (目录与 readers 表保留, 契约 4.7)
// + 二次 Remove 不幂等(RemoveFailed)。
// 模拟"曾配置/曾启动"的通道态: 目录存在(进程未运行, 走 notify_removed_for_inactive)。
TEST_F(ProcessControlFrameTest, RemoveInactiveGatewayFinalizesFully) {
    shm_mgr_->create_md_channel("dzmd_ctp");
    const auto ch_dir = dztrader::paths::shm() / "dzmd_ctp";
    ASSERT_TRUE(std::filesystem::exists(ch_dir));

    auto writer = create_writer("remove_probe");
    auto reader = create_reader("remove_rtn_probe");
    write_process_control_frame(writer, platform::ProcessAction::Remove, "dzmd_ctp");
    for (int i = 0; i < 10; ++i) {
        shm_mgr_->drain_event_channel();
    }

    // 契约 4.7: Remove 不删文件 (tombstone), 目录保留供 PageCleaner 清理与同源重加复用
    EXPECT_TRUE(std::filesystem::exists(ch_dir));
    // 通道标记 tombstone: 读者注册被拒 (channel not configured)
    {
        auto* state = shm_mgr_->md_channel_state("dzmd_ctp");
        ASSERT_NE(state, nullptr);
        EXPECT_EQ(state->status, MdChannelStatus::Tombstone);
    }

    // json 配置段删除 (persist 链, 重新解析 cfg_path_)
    nlohmann::json cfg;
    {
        std::ifstream ifs(cfg_path_);
        ifs >> cfg;
    }
    EXPECT_FALSE(cfg.contains("md") && cfg["md"].contains("dzmd_ctp"));

    // 帧序(写入顺序): 118(条目消失) -> 116 自发 Stopped -> 116 RemoveSucceeded。
    // 一次性收集后断言: 118 先于 116 写入, 顺序 next_frame 扫描会先消费掉 118。
    // event 为 optional, 缺失时序列化省略该字段 (platform/process.h to_json),
    // value("event", "") 对自发帧返回空串、对请求帧返回事件名字符串。
    auto frames = drain_all(reader);
    // 116: RemoveSucceeded + Stopped
    bool remove_ok = false;
    auto it = frames.find(DZ_FRAME_RTN_PROCESS_STATUS);
    ASSERT_NE(it, frames.end());
    for (const auto& st : it->second) {
        if (st.value("name", "") == "dzmd_ctp" &&
            st.value("event", "") == "RemoveSucceeded" &&
            st.value("state", "") == "Stopped") {
            remove_ok = true;
        }
    }
    EXPECT_TRUE(remove_ok);

    // 118: 全量配置 map 中 dzmd_ctp 条目消失 (移除完成权威信号)
    auto cit = frames.find(DZ_FRAME_RTN_PROCESS_CONFIG);
    ASSERT_NE(cit, frames.end());
    EXPECT_FALSE(cit->second.back().contains("dzmd_ctp"));

    // 二次 Remove 不幂等: RemoveFailed (registry/store 已全清, 与契约 process 一致)
    write_process_control_frame(writer, platform::ProcessAction::Remove, "dzmd_ctp");
    for (int i = 0; i < 10; ++i) {
        shm_mgr_->drain_event_channel();
    }
    bool second_failed = false;
    {
        auto frames2 = drain_all(reader);
        auto it2 = frames2.find(DZ_FRAME_RTN_PROCESS_STATUS);
        ASSERT_NE(it2, frames2.end());
        for (const auto& st : it2->second) {
            if (st.value("name", "") == "dzmd_ctp" &&
                st.value("event", "") == "RemoveFailed") {
                second_failed = true;
            }
        }
    }
    EXPECT_TRUE(second_failed);
}

// ---- 契约 account-status: master 账户镜像 (2018 建镜像) + 2115 兜底应答 ----
// 帧驱动模式同既有用例: 独立 MultiWriter 模拟 td 写 2018/2115 basic 帧
// (payload=定长结构体, 无扩展头) -> drain -> probe Reader 断言 master 兜底帧。

namespace {

/// 写 2018 账户状态帧 (basic 广播帧, payload=DzAccountStatus)
void write_account_status_frame(shm::MultiWriter& w, const std::string& gateway,
                                const std::string& account, DzAccountState state) {
    DzAccountStatus status{};
    dztrader::copy_string(status.gateway_name, gateway.c_str(), true);
    dztrader::copy_string(status.account_id, account.c_str(), true);
    status.state = state;
    status.trading_day = 0;
    ASSERT_TRUE(platform::write_struct(w, DZ_FRAME_ACCOUNT_STATUS, status));
}

/// 写 2115 账户状态查询帧 (basic 广播帧, payload=DzAccountStatusReq)
void write_query_account_status_frame(shm::MultiWriter& w, const std::string& account) {
    DzAccountStatusReq req{};
    dztrader::copy_string(req.account_id, account.c_str(), true);
    ASSERT_TRUE(platform::write_struct(w, DZ_FRAME_TD_QUERY_ACCOUNT_STATUS, req));
}

/// 排空 reader 并收集全部 2018 账户状态帧 payload (帧指针下次 next_frame 失效, 拷出)
std::vector<DzAccountStatus> collect_account_status_frames(shm::Reader& reader) {
    std::vector<DzAccountStatus> out;
    for (int i = 0; i < 64; ++i) {
        const auto* frame = reader.next_frame();
        if (!frame) break;
        shm::FrameView view(frame);
        if (view.type() != DZ_FRAME_ACCOUNT_STATUS) continue;
        DzAccountStatus status;
        std::memcpy(&status, &view.payload<DzAccountStatus>(), sizeof(status));
        out.push_back(status);
    }
    return out;
}

}  // namespace

// 2018 建镜像: td 上报 CTP001 后, 2115 查询命中镜像 -> master 不兜底 (td 权威应答)
TEST_F(ShmManagerTest, AccountStatusFrameBuildsMirror) {
    ShmManager mgr(make_default_shm_global(), cfg_path_);

    auto meta = shm::ChannelMeta::open_only(shm::channel_name("dzevent"), dztrader::paths::shm());
    auto writer = shm::MultiWriter::create(
        std::make_shared<shm::ChannelMeta>(std::move(meta)), "fake_td");
    write_account_status_frame(writer, "dztd_ctp", "CTP001", DZ_ACCOUNT_READY);
    for (int i = 0; i < 10; ++i) {
        mgr.drain_event_channel();
    }

    // reader 必须注册在查询帧写入之前才能看到 master 的应答帧
    shm::Reader reader = shm::Reader::create(shm::channel_name("dzevent"), dztrader::paths::shm(),
                                             "mirror_probe");
    write_query_account_status_frame(writer, "CTP001");
    for (int i = 0; i < 10; ++i) {
        mgr.drain_event_channel();
    }

    // 镜像命中: 无任何 2018 应答 (该网关 td 会权威应答)
    EXPECT_TRUE(collect_account_status_frames(reader).empty());
}

// 2115 未命中: 查询账户不在任何网关镜像中 -> master 兜底回 Offline (gateway_name="")
TEST_F(ShmManagerTest, QueryUnknownAccountGetsOfflineFallback) {
    ShmManager mgr(make_default_shm_global(), cfg_path_);

    auto meta = shm::ChannelMeta::open_only(shm::channel_name("dzevent"), dztrader::paths::shm());
    auto writer = shm::MultiWriter::create(
        std::make_shared<shm::ChannelMeta>(std::move(meta)), "fake_td");
    write_account_status_frame(writer, "dztd_ctp", "CTP001", DZ_ACCOUNT_READY);
    for (int i = 0; i < 10; ++i) {
        mgr.drain_event_channel();
    }

    shm::Reader reader = shm::Reader::create(shm::channel_name("dzevent"), dztrader::paths::shm(),
                                             "fallback_probe");
    write_query_account_status_frame(writer, "UNKNOWN");
    for (int i = 0; i < 10; ++i) {
        mgr.drain_event_channel();
    }

    auto frames = collect_account_status_frames(reader);
    ASSERT_EQ(frames.size(), 1u);
    EXPECT_STREQ(frames[0].account_id, "UNKNOWN");
    EXPECT_STREQ(frames[0].gateway_name, "");
    EXPECT_EQ(frames[0].state, DZ_ACCOUNT_OFFLINE);
    EXPECT_EQ(frames[0].trading_day, 0);
}

// 回声防自锁: master 自己写的兜底帧 (gateway_name="") 回流后不得入镜像
// (否则 2115 查询会命中镜像而不再兜底, 兜底链路永久失效)
TEST_F(ShmManagerTest, FallbackEchoSkippedFromMirror) {
    ShmManager mgr(make_default_shm_global(), cfg_path_);

    auto meta = shm::ChannelMeta::open_only(shm::channel_name("dzevent"), dztrader::paths::shm());
    auto writer = shm::MultiWriter::create(
        std::make_shared<shm::ChannelMeta>(std::move(meta)), "fake_td");
    // 模拟 master 兜底应答回声: gateway_name="" 的 2018 帧
    write_account_status_frame(writer, "", "CTP001", DZ_ACCOUNT_READY);
    for (int i = 0; i < 10; ++i) {
        mgr.drain_event_channel();
    }

    shm::Reader reader = shm::Reader::create(shm::channel_name("dzevent"), dztrader::paths::shm(),
                                             "echo_probe");
    write_query_account_status_frame(writer, "CTP001");
    for (int i = 0; i < 10; ++i) {
        mgr.drain_event_channel();
    }

    // 回声未入镜像: 查询仍走兜底, 回 Offline
    auto frames = collect_account_status_frames(reader);
    ASSERT_EQ(frames.size(), 1u);
    EXPECT_STREQ(frames[0].account_id, "CTP001");
    EXPECT_STREQ(frames[0].gateway_name, "");
    EXPECT_EQ(frames[0].state, DZ_ACCOUNT_OFFLINE);
}

// td 退出兜底: notify_td_stopped 对镜像内账户逐账户写 Offline (gateway_name=真实网关名),
// 且写完即清镜像 (新语义: 运行中网关当前管理的账户集; td 退出后 2115 查询走 master 兜底,
// 消除 dead-td + 镜像保留导致的静默自锁窗口)
TEST_F(ShmManagerTest, NotifyTdStoppedWritesOfflineAndClearsMirror) {
    ShmManager mgr(make_default_shm_global(), cfg_path_);

    auto meta = shm::ChannelMeta::open_only(shm::channel_name("dzevent"), dztrader::paths::shm());
    auto writer = shm::MultiWriter::create(
        std::make_shared<shm::ChannelMeta>(std::move(meta)), "fake_td");
    write_account_status_frame(writer, "dztd_ctp", "CTP001", DZ_ACCOUNT_READY);
    write_account_status_frame(writer, "dztd_ctp", "CTP002", DZ_ACCOUNT_READY);
    for (int i = 0; i < 10; ++i) {
        mgr.drain_event_channel();
    }

    shm::Reader reader = shm::Reader::create(shm::channel_name("dzevent"), dztrader::paths::shm(),
                                             "td_stop_probe");
    mgr.notify_td_stopped("dztd_ctp");

    // 逐账户 2 条 Offline 帧, gateway_name=真实网关名
    auto frames = collect_account_status_frames(reader);
    ASSERT_EQ(frames.size(), 2u);
    std::set<std::string> accounts;
    for (const auto& st : frames) {
        EXPECT_STREQ(st.gateway_name, "dztd_ctp");
        EXPECT_EQ(st.state, DZ_ACCOUNT_OFFLINE);
        EXPECT_EQ(st.trading_day, 0);
        accounts.insert(std::string(st.account_id));
    }
    EXPECT_EQ(accounts, (std::set<std::string>{"CTP001", "CTP002"}));

    // 镜像已清: 2115 查 CTP001 走 master 兜底, 回 Offline (gateway_name="")
    write_query_account_status_frame(writer, "CTP001");
    for (int i = 0; i < 10; ++i) {
        mgr.drain_event_channel();
    }
    auto query_frames = collect_account_status_frames(reader);
    ASSERT_EQ(query_frames.size(), 1u);
    EXPECT_STREQ(query_frames[0].account_id, "CTP001");
    EXPECT_STREQ(query_frames[0].gateway_name, "");
    EXPECT_EQ(query_frames[0].state, DZ_ACCOUNT_OFFLINE);
}

// remove 流程清理: forget_td_accounts 删除镜像条目, 之后 2115 查询走兜底
// (防僵尸账户集污染兜底应答)
TEST_F(ShmManagerTest, ForgetTdAccountsClearsMirror) {
    ShmManager mgr(make_default_shm_global(), cfg_path_);

    auto meta = shm::ChannelMeta::open_only(shm::channel_name("dzevent"), dztrader::paths::shm());
    auto writer = shm::MultiWriter::create(
        std::make_shared<shm::ChannelMeta>(std::move(meta)), "fake_td");
    write_account_status_frame(writer, "dztd_ctp", "CTP001", DZ_ACCOUNT_READY);
    for (int i = 0; i < 10; ++i) {
        mgr.drain_event_channel();
    }

    mgr.forget_td_accounts("dztd_ctp");

    shm::Reader reader = shm::Reader::create(shm::channel_name("dzevent"), dztrader::paths::shm(),
                                             "forget_probe");
    write_query_account_status_frame(writer, "CTP001");
    for (int i = 0; i < 10; ++i) {
        mgr.drain_event_channel();
    }

    // 镜像已清: 查询走兜底, 回 Offline
    auto frames = collect_account_status_frames(reader);
    ASSERT_EQ(frames.size(), 1u);
    EXPECT_STREQ(frames[0].account_id, "CTP001");
    EXPECT_STREQ(frames[0].gateway_name, "");
    EXPECT_EQ(frames[0].state, DZ_ACCOUNT_OFFLINE);
}

// C-F3: 空 account_id 的 2115 全量查询 master 静默 (全量查询由各 td 权威应答,
// master 无账户全集可兜底, 盲回会制造假阴性)
TEST_F(ShmManagerTest, QueryWithEmptyAccountGetsNoFallback) {
    ShmManager mgr(make_default_shm_global(), cfg_path_);

    auto meta = shm::ChannelMeta::open_only(shm::channel_name("dzevent"), dztrader::paths::shm());
    auto writer = shm::MultiWriter::create(
        std::make_shared<shm::ChannelMeta>(std::move(meta)), "fake_td");
    // 建镜像: 有运行中网关管理 CTP001, 但全量查询仍不兜底
    write_account_status_frame(writer, "dztd_ctp", "CTP001", DZ_ACCOUNT_READY);
    for (int i = 0; i < 10; ++i) {
        mgr.drain_event_channel();
    }

    shm::Reader reader = shm::Reader::create(shm::channel_name("dzevent"), dztrader::paths::shm(),
                                             "empty_query_probe");
    write_query_account_status_frame(writer, "");
    for (int i = 0; i < 10; ++i) {
        mgr.drain_event_channel();
    }

    // master 静默: 无任何 2018 帧产出
    EXPECT_TRUE(collect_account_status_frames(reader).empty());
}

}  // namespace
}  // namespace dztrader::master
