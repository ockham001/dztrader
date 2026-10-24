#include "test_util.h"

#include <gtest/gtest.h>

#include <dztrader/core/this_process.h>
#include <dztrader/data_type.h>
#include <dztrader/core/core_data_type.h>
#include <dztrader/platform/process.h>
#include <dztrader/shm/channel_meta.h>
#include <dztrader/shm/frame_codec.h>
#include <dztrader/shm/frame_view.h>
#include <dztrader/shm/named_semaphore.h>
#include <dztrader/shm/reader.h>
#include <dztrader/shm/writer.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <random>
#include <string>

#include "shm_writer.h"

using dztrader::shm::ChannelConfig;
using dztrader::shm::ChannelMeta;
using dztrader::shm::MultiWriter;
using dztrader::shm::Reader;

namespace dztrader::webui {
namespace {

constexpr uint64_t kMB = 1024 * 1024;

/// 进程唯一通道名（PID + 随机后缀）：ctest -j 下同二进制各用例是独立进程，
/// 固定名会互相 remove_all shm 目录 → 用例互踩（§3.5）。
std::string unique_channel_name(const std::string& base) {
    std::random_device rd;
    return base + "_" + std::to_string(static_cast<uint32_t>(dztrader::this_process::pid())) + "_" +
           std::to_string(rd());
}

class WebuiShmWriterTest : public ::testing::Test {
protected:
    std::string channel_name_;
    std::string reader_name_;
    std::filesystem::path shm_dir_;
    std::shared_ptr<ChannelMeta> meta_;
    std::optional<Reader> reader_;
    std::shared_ptr<ShmWriter> shm_writer_;

    void SetUp() override {
        channel_name_ = unique_channel_name("dz_test_webui_fw");
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
        // 先注册 reader 再建 writer：MultiWriter::create 在构造时同步订阅列表，
        // 写帧后 notify_subscribers 才能唤醒该 reader（通用规则 §4.1）
        ASSERT_TRUE(meta_->add_reader(reader_name_, 0));
        reader_ = Reader::create(meta_, reader_name_);
        // ShmWriter 注入共享 writer (唯一写入者), reader 观察其写出的帧
        shm_writer_ = std::make_shared<ShmWriter>(
            std::make_shared<MultiWriter>(MultiWriter::create(meta_, channel_name_ + "_writer")));
    }

    void TearDown() override {
        // 先释放映射再删目录：成员在 TearDown 之后才析构，Windows 上
        // 已映射文件 remove_all 会失败（§3.5；domain_service_test 同型）
        shm_writer_.reset();
        reader_.reset();
        meta_.reset();
        EXPECT_TRUE(dztrader::shm::test::cleanup_channel(channel_name_, shm_dir_, {reader_name_}));
    }

    /// 取下一帧并断言类型；FrameView 在 reader_ 存活期间有效
    std::optional<shm::FrameView> next_frame_view(DzFrameType expected_type) {
        auto* frame = reader_->next_frame();
        if (frame == nullptr) {
            ADD_FAILURE() << "no frame written";
            return std::nullopt;
        }
        const shm::FrameView view(frame);
        EXPECT_EQ(view.type(), expected_type);
        return view;
    }

    static nlohmann::json parse_payload(const shm::FrameView& view) {
        const auto size = view.ext_payload_size();
        if (size == 0) {
            return nlohmann::json::object();
        }
        return nlohmann::json::parse(reinterpret_cast<const char*>(view.ext_payload()),
                                     reinterpret_cast<const char*>(view.ext_payload()) + size);
    }

    static nlohmann::json parse_inst_payload(const shm::FrameView& view) {
        const auto size = view.ext_inst_payload_size();
        if (size == 0) {
            return nlohmann::json::object();
        }
        return nlohmann::json::parse(reinterpret_cast<const char*>(view.ext_inst_payload()),
                                     reinterpret_cast<const char*>(view.ext_inst_payload()) + size);
    }
};

TEST_F(WebuiShmWriterTest, WriteSetEventShmConfig) {
    nlohmann::json patch = {
        {"check_interval_min", 10},
        {"preload_points", {{"08:45", {{"pages", 1}, {"bytes", 0}}}}},
    };
    shm_writer_->write_set_event_shm_config(patch);

    auto view = next_frame_view(DZ_FRAME_SET_EVENT_SHM_CONFIG);
    ASSERT_TRUE(view.has_value());
    EXPECT_EQ(parse_payload(*view), patch);
    // 无 instance_id 帧: 使用 8B DzExtFrameHeader (而非 72B DzExtInstFrameHeader)。
    // 注: 不能对 DzExtFrame 调用 ext_inst_id() 判空——它会按 72B 头解读 data_size+payload,
    //     恒非空; 正确判定是按帧大小区分两种扩展头布局
    EXPECT_EQ(view->frame_size(), sizeof(DzFrameHeader) + sizeof(DzExtFrameHeader) +
                                      ((view->ext_payload_size() + 7) & ~7u));
}

TEST_F(WebuiShmWriterTest, WriteMdConnect) {
    ASSERT_TRUE(shm_writer_->write_md_connect("dzmd_ctp"));
    auto view = next_frame_view(DZ_FRAME_REQUEST_MD_CONNECT);
    ASSERT_TRUE(view.has_value());
    EXPECT_STREQ(view->ext_inst_id(), "dzmd_ctp");
    EXPECT_EQ(view->ext_inst_payload_size(), 0u);
}

TEST_F(WebuiShmWriterTest, WriteMdDisconnect) {
    ASSERT_TRUE(shm_writer_->write_md_disconnect("dzmd_ctp"));
    auto view = next_frame_view(DZ_FRAME_REQUEST_MD_DISCONNECT);
    ASSERT_TRUE(view.has_value());
    EXPECT_STREQ(view->ext_inst_id(), "dzmd_ctp");
    EXPECT_EQ(view->ext_inst_payload_size(), 0u);
}

TEST_F(WebuiShmWriterTest, WriteProcessControl) {
    ASSERT_TRUE(shm_writer_->write_process_control(platform::ProcessAction::Start, "dzmd_ctp",
                                                   nlohmann::json{{"display_name", "CTP"}}));
    auto view = next_frame_view(DZ_FRAME_REQUEST_PROCESS_CONTROL);
    ASSERT_TRUE(view.has_value());
    const auto req = parse_payload(*view).get<platform::ProcessControlReq>();
    EXPECT_EQ(req.action, platform::ProcessAction::Start);
    EXPECT_EQ(req.target, "dzmd_ctp");
    ASSERT_TRUE(req.config.has_value());
    EXPECT_EQ(req.config->at("display_name"), "CTP");
}

TEST_F(WebuiShmWriterTest, WriteSetProcessConfig) {
    shm_writer_->write_set_process_config("dzmd_ctp", nlohmann::json{{"log_level", "debug"}});
    auto view = next_frame_view(DZ_FRAME_SET_PROCESS_CONFIG);
    ASSERT_TRUE(view.has_value());
    const auto req = parse_payload(*view).get<platform::SetProcessConfigReq>();
    EXPECT_EQ(req.target, "dzmd_ctp");
    EXPECT_EQ(req.config.at("log_level"), "debug");
}

TEST_F(WebuiShmWriterTest, WriteQueryAll) {
    shm_writer_->write_query_all();
    auto view = next_frame_view(DZ_FRAME_QUERY_FULL_SNAPSHOT);
    ASSERT_TRUE(view.has_value());
    EXPECT_EQ(view->ext_payload_size(), 0u);
    EXPECT_EQ(view->frame_size(), sizeof(DzFrameHeader) + sizeof(DzExtFrameHeader));
}

TEST_F(WebuiShmWriterTest, WriteMdSetConfig) {
    const nlohmann::json payload = {{"op", "set"},
                                    {"params", {{"brokers", nlohmann::json::array({"ctp"})}}}};
    shm_writer_->write_md_set_config("dzmd_ctp", payload);
    auto view = next_frame_view(DZ_FRAME_SET_MD_CONFIG);
    ASSERT_TRUE(view.has_value());
    EXPECT_STREQ(view->ext_inst_id(), "dzmd_ctp");
    EXPECT_EQ(parse_inst_payload(*view), payload);
}

TEST_F(WebuiShmWriterTest, WriteSetAutoLogin) {
    const nlohmann::json payload = {
        {"enabled", true},
        {"schedules", nlohmann::json::array({{{"time", "08:45"}, {"action", "login"}}})}};
    shm_writer_->write_set_auto_login("dzmd_ctp", payload);
    auto view = next_frame_view(DZ_FRAME_SET_AUTO_LOGIN);
    ASSERT_TRUE(view.has_value());
    EXPECT_STREQ(view->ext_inst_id(), "dzmd_ctp");
    EXPECT_EQ(parse_inst_payload(*view), payload);
}

TEST_F(WebuiShmWriterTest, WriteSetMdShmConfig) {
    const nlohmann::json payload = {{"page_size_mb", 2},
                                    {"preload_points", {{"08:45", {{"pages", 1}, {"bytes", 0}}}}}};
    shm_writer_->write_set_md_shm_config("dzmd_ctp", payload);
    auto view = next_frame_view(DZ_FRAME_SET_MD_SHM_CONFIG);
    ASSERT_TRUE(view.has_value());
    EXPECT_STREQ(view->ext_inst_id(), "dzmd_ctp");
    EXPECT_EQ(parse_inst_payload(*view), payload);
}

TEST_F(WebuiShmWriterTest, WriteSetSysmonConfig) {
    // 契约 sysmon §3：SET_SYSMON_CONFIG 定向到固定实例 dzsysmon，payload 为 merge patch 透传
    const nlohmann::json payload = {{"network", {{"hide_virtual", false}}}};
    shm_writer_->write_set_sysmon_config(payload);
    auto view = next_frame_view(DZ_FRAME_SET_SYSMON_CONFIG);
    ASSERT_TRUE(view.has_value());
    EXPECT_STREQ(view->ext_inst_id(), "dzsysmon");
    EXPECT_EQ(parse_inst_payload(*view), payload);
}

TEST_F(WebuiShmWriterTest, WriteSetSysmonConfigNullWriterWritesNothing) {
    ShmWriter writer{std::shared_ptr<shm::MultiWriter>()};
    writer.write_set_sysmon_config(nlohmann::json{{"network", {{"hide_virtual", true}}}});
    EXPECT_EQ(reader_->next_frame(), nullptr);
}

TEST_F(WebuiShmWriterTest, WriteNotifiesRegisteredReaders) {
    // 通用规则 §4.1：写帧后必须唤醒等待进程；无 notify 时 wait_for 会超时
    dztrader::shm::NamedSemaphore sem(reader_name_);
    while (sem.wait_for(0)) {
    }  // 清空既有计数，确保唤醒来自本次写帧
    shm_writer_->write_set_event_shm_config(nlohmann::json{{"check_interval_min", 15}});
    EXPECT_TRUE(sem.wait_for(500));
}

TEST_F(WebuiShmWriterTest, NullWriterIsNotReadyAndWritesNothing) {
    // null writer: guard 直接返回；用可观察断言替代纯 no-throw 冒烟
    ShmWriter writer{std::shared_ptr<shm::MultiWriter>()};
    EXPECT_FALSE(writer.is_ready());
    writer.write_set_event_shm_config(nlohmann::json{{"check_interval_min", 5}});
    EXPECT_EQ(reader_->next_frame(), nullptr);  // 无任何帧写入
}

}  // namespace
}  // namespace dztrader::webui
