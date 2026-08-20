#include <gtest/gtest.h>

#include <dztrader/data_type.h>
#include <dztrader/shm/channel_meta.h>
#include <dztrader/shm/frame_codec.h>
#include <dztrader/shm/frame_view.h>
#include <dztrader/shm/reader.h>
#include <dztrader/shm/writer.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <memory>
#include <optional>

#include "shm_writer.h"

using dztrader::shm::ChannelConfig;
using dztrader::shm::ChannelMeta;
using dztrader::shm::MultiWriter;
using dztrader::shm::Reader;

namespace dztrader::webui {
namespace {

class WebuiShmWriterTest : public ::testing::Test {
protected:
    std::string channel_name_;
    std::filesystem::path shm_dir_;
    std::shared_ptr<ChannelMeta> meta_;
    std::optional<Reader> reader_;
    std::shared_ptr<ShmWriter> shm_writer_;

    static constexpr uint64_t MB = 1024 * 1024;

    void SetUp() override {
        channel_name_ = "dz_test_webui_fw";
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
        reader_ = Reader::create(meta_, "test_reader");
        // ShmWriter 注入共享 writer (唯一写入者), reader 观察其写出的帧
        shm_writer_ = std::make_shared<ShmWriter>(
            std::make_shared<MultiWriter>(MultiWriter::create(meta_, "test_writer")));
    }

    void TearDown() override { std::filesystem::remove_all(shm_dir_); }
};

TEST_F(WebuiShmWriterTest, WriteSetEventShmConfig) {
    nlohmann::json patch = {
        {"check_interval_min", 10},
        {"preload_points", {{"08:45", {{"pages", 1}, {"bytes", 0}}}}},
    };
    shm_writer_->write_set_event_shm_config(patch);

    auto* frame = reader_->next_frame();
    ASSERT_NE(frame, nullptr);
    auto view = dztrader::shm::FrameView(frame);
    EXPECT_EQ(view.type(), DZ_FRAME_SET_EVENT_SHM_CONFIG);
    auto payload = nlohmann::json::parse(
        reinterpret_cast<const char*>(view.ext_payload()),
        reinterpret_cast<const char*>(view.ext_payload()) + view.ext_payload_size());
    EXPECT_EQ(payload["check_interval_min"], 10);
    EXPECT_EQ(payload["preload_points"]["08:45"]["pages"], 1);
    EXPECT_EQ(payload["preload_points"]["08:45"]["bytes"], 0);
    // 无 instance_id 帧: 使用 8B DzExtFrameHeader (而非 72B DzExtInstFrameHeader)。
    // 注: 不能对 DzExtFrame 调用 ext_inst_id() 判空——它会按 72B 头解读 data_size+payload,
    //     恒非空; 正确判定是按帧大小区分两种扩展头布局
    EXPECT_EQ(view.frame_size(), sizeof(DzFrameHeader) + sizeof(DzExtFrameHeader)
                                     + ((view.ext_payload_size() + 7) & ~7u));
}

TEST_F(WebuiShmWriterTest, WriteSetEventShmConfigNullWriterNoCrash) {
    // null writer: write_set_event_shm_config 内部 guard 直接返回, 不抛异常
    // 注: 必须用花括号初始化, 圆括号会被解析为函数声明 (most vexing parse)
    ShmWriter writer{std::shared_ptr<shm::MultiWriter>()};
    EXPECT_NO_THROW(writer.write_set_event_shm_config(nlohmann::json{{"check_interval_min", 5}}));
}

}  // namespace
}  // namespace dztrader::webui
