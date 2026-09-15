#include <gtest/gtest.h>

#include <dztrader/core/core_data_type.h>
#include <dztrader/platform/frame_codec.h>
#include <dztrader/platform/risk_reject.h>
#include <dztrader/shm/channel_meta.h>
#include <dztrader/shm/frame_view.h>
#include <dztrader/shm/reader.h>
#include <dztrader/shm/writer.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <string>

using dztrader::platform::DzRiskReject;
using dztrader::shm::ChannelConfig;
using dztrader::shm::ChannelMeta;
using dztrader::shm::MultiWriter;
using dztrader::shm::Reader;

namespace {

class RiskRejectTest : public ::testing::Test {
protected:
    std::string channel_name_;
    std::filesystem::path shm_dir_;
    std::shared_ptr<ChannelMeta> meta_;
    std::optional<MultiWriter> writer_;
    std::optional<Reader> reader_;

    static constexpr uint64_t MB = 1024 * 1024;

    void SetUp() override {
        channel_name_ = "dz_test_risk_reject";
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

// 契约 td-risk-reject JSON schema 锁定
TEST_F(RiskRejectTest, GoldenJson) {
    DzRiskReject reject;
    reject.account_id = "account_001";
    reject.rule_name = "max_order_volume";
    reject.reason = "order volume exceeds limit";
    reject.timestamp_ns = 1757500000000000000LL;

    EXPECT_EQ(nlohmann::json(reject), nlohmann::json::parse(R"({
        "account_id": "account_001",
        "rule_name": "max_order_volume",
        "reason": "order volume exceeds limit",
        "timestamp_ns": 1757500000000000000
    })"));
}

TEST_F(RiskRejectTest, RoundTrip) {
    DzRiskReject in;
    in.account_id = "account_001";
    in.rule_name = "price_tick";
    in.reason = "price not multiple of tick";
    in.timestamp_ns = 123456789;

    const auto out = nlohmann::json(in).get<DzRiskReject>();
    EXPECT_EQ(out.account_id, in.account_id);
    EXPECT_EQ(out.rule_name, in.rule_name);
    EXPECT_EQ(out.reason, in.reason);
    EXPECT_EQ(out.timestamp_ns, in.timestamp_ns);
}

// 无 instance_id 的 JSON ext 帧, payload 可解析
TEST_F(RiskRejectTest, WritesJsonExtWithoutInstanceId) {
    DzRiskReject reject;
    reject.account_id = "account_001";
    reject.rule_name = "max_order_volume";
    reject.reason = "order volume exceeds limit";
    reject.timestamp_ns = 1757500000000000000LL;

    ASSERT_TRUE(dztrader::platform::write_ext_json(*writer_, DZ_FRAME_TD_RISK_REJECT, reject));

    auto* frame = reader_->next_frame();
    ASSERT_NE(frame, nullptr);
    const auto view = dztrader::shm::FrameView(frame);
    EXPECT_EQ(view.type(), DZ_FRAME_TD_RISK_REJECT);
    const auto payload = nlohmann::json::parse(
        reinterpret_cast<const char*>(view.ext_payload()),
        reinterpret_cast<const char*>(view.ext_payload()) + view.ext_payload_size());
    EXPECT_EQ(payload["account_id"], "account_001");
    EXPECT_EQ(payload["rule_name"], "max_order_volume");
    EXPECT_EQ(payload["timestamp_ns"], 1757500000000000000LL);
}

}  // namespace
