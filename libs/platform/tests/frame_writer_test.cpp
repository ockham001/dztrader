#include <gtest/gtest.h>

#include <dztrader/data_type.h>
#include <dztrader/platform/frame_codec.h>
#include <dztrader/platform/notify_ui.h>
#include <dztrader/shm/channel_meta.h>
#include <dztrader/shm/frame_codec.h>
#include <dztrader/shm/frame_view.h>
#include <dztrader/shm/named_semaphore.h>
#include <dztrader/shm/reader.h>
#include <dztrader/shm/writer.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <string>

using dztrader::shm::ChannelConfig;
using dztrader::shm::ChannelMeta;
using dztrader::shm::MultiWriter;
using dztrader::shm::Reader;

namespace {

class PlatformFrameWriterTest : public ::testing::Test {
protected:
    std::string channel_name_;
    std::filesystem::path shm_dir_;
    std::shared_ptr<ChannelMeta> meta_;
    std::optional<MultiWriter> writer_;
    std::optional<Reader> reader_;

    static constexpr uint64_t MB = 1024 * 1024;

    void SetUp() override {
        channel_name_ = "dz_test_platform_fw";
        shm_dir_ = std::filesystem::temp_directory_path() / channel_name_;
        std::filesystem::remove_all(shm_dir_);

        ChannelConfig cfg{
            .channel_name = channel_name_,
            .shm_dir = shm_dir_,
            .meta_file_size = 4 * MB,
            .page_size = 1 * MB,
            .lock_memory = false,
            .prefetch_memory = false,
        };
        meta_ = std::make_shared<ChannelMeta>(ChannelMeta::open_or_create(cfg));
        writer_ = MultiWriter::create(meta_, "test_writer");
        reader_ = Reader::create(meta_, "test_reader");
    }

    void TearDown() override { std::filesystem::remove_all(shm_dir_); }
};

// 测试 NotifyUi：验证无 instance_id 帧 JSON payload 正确 (source 在 payload, 不在帧头)
TEST_F(PlatformFrameWriterTest, WriteNotifyUi) {
    dztrader::platform::NotifyUi notifier("src1", *writer_);
    notifier.error("test msg");

    auto* frame = reader_->next_frame();
    ASSERT_NE(frame, nullptr);
    auto view = dztrader::shm::FrameView(frame);
    EXPECT_EQ(view.type(), DZ_FRAME_NOTIFY_UI);
    // 无 instance_id 帧: 用 ext_payload 解析
    auto payload = nlohmann::json::parse(
        reinterpret_cast<const char*>(view.ext_payload()),
        reinterpret_cast<const char*>(view.ext_payload()) + view.ext_payload_size());
    EXPECT_EQ(payload["source"], "src1");
    EXPECT_EQ(payload["level"], "error");
    EXPECT_EQ(payload["message"], "test msg");
    EXPECT_EQ(payload["popup"], true);
    EXPECT_TRUE(payload.contains("timestamp"));
}

// 测试 write_request_shutdown：空 payload 控制帧
TEST_F(PlatformFrameWriterTest, WriteRequestShutdown) {
    dztrader::platform::write_ext_inst_raw(*writer_, DZ_FRAME_REQUEST_SHUTDOWN, "dzmd_ctp");

    auto* frame = reader_->next_frame();
    ASSERT_NE(frame, nullptr);
    auto view = dztrader::shm::FrameView(frame);
    EXPECT_EQ(view.type(), DZ_FRAME_REQUEST_SHUTDOWN);
}

// 测试 write_td_order_rpt：定长结构体帧
TEST_F(PlatformFrameWriterTest, WriteTdOrderRpt) {
    DzOrderReport rpt{};
    rpt.order_id = 12345;
    dztrader::platform::write_struct(*writer_, DZ_FRAME_TD_ORDER_RPT, rpt);

    auto* frame = reader_->next_frame();
    ASSERT_NE(frame, nullptr);
    auto view = dztrader::shm::FrameView(frame);
    EXPECT_EQ(view.type(), DZ_FRAME_TD_ORDER_RPT);
}

// 测试 write_set_md_config：JSON obj 帧
TEST_F(PlatformFrameWriterTest, WriteSetMdConfig) {
    nlohmann::json req = {{"instrument_id", "IF2401"}};
    dztrader::platform::write_ext_inst_json_obj(*writer_, DZ_FRAME_SET_MD_CONFIG, "dzmd_ctp", req);

    auto* frame = reader_->next_frame();
    ASSERT_NE(frame, nullptr);
    auto view = dztrader::shm::FrameView(frame);
    EXPECT_EQ(view.type(), DZ_FRAME_SET_MD_CONFIG);
}

// 测试 write_ext_json：写帧后必须 notify 订阅者 (契约 shm: 写入任何帧都唤醒等待进程)
// 回归: 曾缺失 notify, master/dzweb 阻塞在 NamedSemaphore::wait 收不到
// REQUEST_PROCESS_CONTROL/RTN_PROCESS_STATUS, 进程控制请求无人处理
TEST_F(PlatformFrameWriterTest, WriteExtJsonNotifiesSubscriber) {
    const std::string reader_name = "fw_test_ext_json_reader";
    dztrader::shm::NamedSemaphore::remove(reader_name);
    dztrader::shm::NamedSemaphore sem(reader_name);
    ASSERT_TRUE(meta_->add_reader(reader_name, /*pid=*/0));
    ASSERT_TRUE(writer_->refresh_subscribers());

    nlohmann::json req = {{"action", "Start"}, {"target", "dzmd_ctp"}};
    dztrader::platform::write_ext_json(*writer_, DZ_FRAME_REQUEST_PROCESS_CONTROL, req);

    auto* frame = reader_->next_frame();
    ASSERT_NE(frame, nullptr);
    EXPECT_EQ(dztrader::shm::FrameView(frame).type(), DZ_FRAME_REQUEST_PROCESS_CONTROL);
    // 修复前 notify 缺失, 此处 wait_for 超时返回 false
    EXPECT_TRUE(sem.wait_for(1000));
}

// 测试 write_ext_inst_json：含 instance_id 帧同样必须 notify
TEST_F(PlatformFrameWriterTest, WriteExtInstJsonNotifiesSubscriber) {
    const std::string reader_name = "fw_test_ext_inst_json_reader";
    dztrader::shm::NamedSemaphore::remove(reader_name);
    dztrader::shm::NamedSemaphore sem(reader_name);
    ASSERT_TRUE(meta_->add_reader(reader_name, /*pid=*/0));
    ASSERT_TRUE(writer_->refresh_subscribers());

    nlohmann::json req = {{"enabled", true}};
    dztrader::platform::write_ext_inst_json(*writer_, DZ_FRAME_SET_AUTO_LOGIN, "dzmd_ctp", req);

    auto* frame = reader_->next_frame();
    ASSERT_NE(frame, nullptr);
    EXPECT_EQ(dztrader::shm::FrameView(frame).type(), DZ_FRAME_SET_AUTO_LOGIN);
    EXPECT_TRUE(sem.wait_for(1000));
}

}  // namespace
