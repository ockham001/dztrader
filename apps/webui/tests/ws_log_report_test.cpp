#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <dztrader/data_type.h>
#include <dztrader/shm/channel_meta.h>
#include <dztrader/shm/reader.h>
#include <dztrader/shm/writer.h>
#include <dztrader/shm/frame_view.h>
#include <dztrader/core/path.h>
#include <dztrader/shm/frame_codec.h>
#include <filesystem>

using namespace dztrader;

// 验证写入 RTN_LOG_CONFIG 帧后，通过 shm::Reader + FrameView 能正确读取
TEST(WsLogReportTest, ReaderReturnsRtnLogConfig) {
    auto shm_dir = dztrader::paths::shm();

    const shm::ChannelConfig evt_cfg{
        .channel_name = dztrader::shm::channel_name("dzevent"),
        .shm_dir = shm_dir,
        .meta_file_size = 1 * 1024 * 1024,
        .page_size = 1024 * 1024,
        .lock_memory = false,
        .prefetch_memory = false,
    };
    (void)shm::ChannelMeta::open_or_create(evt_cfg);

    shm::Reader reader = shm::Reader::create(
        dztrader::shm::channel_name("dzevent"), shm_dir, "test_report_reader");

    auto meta = shm::ChannelMeta::open_only(dztrader::shm::channel_name("dzevent"), shm_dir);
    auto writer = shm::MultiWriter::create(
        std::make_shared<shm::ChannelMeta>(std::move(meta)), "test_report_writer");

    const nlohmann::json cfg = {{"level", "debug"}, {"flush_on", "info"}};
    shm::write_ext_inst_json(writer, DZ_FRAME_RTN_LOG_CONFIG, "ctp", cfg);
    writer.notify_subscribers();

    const std::byte* raw = reader.next_frame();
    ASSERT_NE(raw, nullptr);
    const shm::FrameView view(raw);
    EXPECT_EQ(view.type(), DZ_FRAME_RTN_LOG_CONFIG);
    EXPECT_EQ(std::string(view.ext_inst_id()), "ctp");

    const auto* data = reinterpret_cast<const char*>(view.ext_inst_payload());
    auto size = view.ext_inst_payload_size();
    nlohmann::json payload = nlohmann::json::parse(data, data + size);
    EXPECT_EQ(payload["level"], "debug");
    EXPECT_EQ(payload["flush_on"], "info");
}
