#include <gtest/gtest.h>

#include <dztrader/core/core_data_type.h>
#include <dztrader/platform/frame_codec.h>
#include <dztrader/platform/td_account_ops.h>
#include <dztrader/shm/channel_meta.h>
#include <dztrader/shm/frame_view.h>
#include <dztrader/shm/reader.h>
#include <dztrader/shm/writer.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <string>

using dztrader::platform::DzPasswordUpdateReq;
using dztrader::platform::DzPasswordUpdateRsp;
using dztrader::platform::DzTransferReq;
using dztrader::platform::DzTransferRsp;
using dztrader::shm::ChannelConfig;
using dztrader::shm::ChannelMeta;
using dztrader::shm::MultiWriter;
using dztrader::shm::Reader;

namespace {

class TdAccountOpsTest : public ::testing::Test {
protected:
    std::string channel_name_;
    std::filesystem::path shm_dir_;
    std::shared_ptr<ChannelMeta> meta_;
    std::optional<MultiWriter> writer_;
    std::optional<Reader> reader_;

    static constexpr uint64_t MB = 1024 * 1024;

    void SetUp() override {
        channel_name_ = "dz_test_td_account_ops";
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

// 契约 td-account-ops JSON schema 逐字段锁定: key 集合/类型变化必须同步契约
TEST_F(TdAccountOpsTest, TransferReqGoldenJson) {
    DzTransferReq req;
    req.account_id = "account_001";
    req.trade_code = "202001";
    req.bank_id = "1";
    req.bank_account = "6222020200112233";
    req.bank_password = "bank_pw";
    req.future_password = "future_pw";
    req.currency_id = "CNY";
    req.trade_amount = 10000.5;
    req.cust_type = 0;
    req.request_id = 42;

    EXPECT_EQ(nlohmann::json(req), nlohmann::json::parse(R"({
        "account_id": "account_001",
        "trade_code": "202001",
        "bank_id": "1",
        "bank_account": "6222020200112233",
        "bank_password": "bank_pw",
        "future_password": "future_pw",
        "currency_id": "CNY",
        "trade_amount": 10000.5,
        "cust_type": 0,
        "request_id": 42
    })"));
}

TEST_F(TdAccountOpsTest, TransferRspGoldenJson) {
    DzTransferRsp rsp;
    rsp.account_id = "account_001";
    rsp.trade_code = "202001";
    rsp.error_id = 0;
    rsp.error_msg = "";
    rsp.bank_balance = 0.0;
    rsp.trade_amount = 10000.5;
    rsp.transfer_status = "0";
    rsp.time = 34200;

    EXPECT_EQ(nlohmann::json(rsp), nlohmann::json::parse(R"({
        "account_id": "account_001",
        "trade_code": "202001",
        "error_id": 0,
        "error_msg": "",
        "bank_balance": 0.0,
        "trade_amount": 10000.5,
        "transfer_status": "0",
        "time": 34200
    })"));
}

TEST_F(TdAccountOpsTest, PasswordUpdateReqGoldenJson) {
    DzPasswordUpdateReq req;
    req.account_id = "account_001";
    req.password_type = "U";
    req.old_password = "old_pw";
    req.new_password = "new_pw";
    req.currency_id = "CNY";

    EXPECT_EQ(nlohmann::json(req), nlohmann::json::parse(R"({
        "account_id": "account_001",
        "password_type": "U",
        "old_password": "old_pw",
        "new_password": "new_pw",
        "currency_id": "CNY"
    })"));
}

TEST_F(TdAccountOpsTest, PasswordUpdateRspGoldenJson) {
    DzPasswordUpdateRsp rsp;
    rsp.account_id = "account_001";
    rsp.password_type = "A";
    rsp.error_id = 3;
    rsp.error_msg = "invalid password";
    rsp.time = 0;

    EXPECT_EQ(nlohmann::json(rsp), nlohmann::json::parse(R"({
        "account_id": "account_001",
        "password_type": "A",
        "error_id": 3,
        "error_msg": "invalid password",
        "time": 0
    })"));
}

// JSON 反序列化 round-trip (未来 dzweb -> td / td -> dzweb 双向使用)
TEST_F(TdAccountOpsTest, RoundTrip) {
    DzTransferReq in;
    in.account_id = "account_001";
    in.trade_code = "202002";
    in.bank_password = "bank_pw";
    in.future_password = "future_pw";
    in.trade_amount = 1234.5;
    in.cust_type = 1;
    in.request_id = 7;

    const auto out = nlohmann::json(in).get<DzTransferReq>();
    EXPECT_EQ(out.account_id, in.account_id);
    EXPECT_EQ(out.trade_code, in.trade_code);
    EXPECT_EQ(out.bank_password, in.bank_password);
    EXPECT_EQ(out.future_password, in.future_password);
    EXPECT_EQ(out.trade_amount, in.trade_amount);
    EXPECT_EQ(out.cust_type, in.cust_type);
    EXPECT_EQ(out.request_id, in.request_id);
}

// 五个帧号均为无 instance_id 的 JSON ext 帧, payload 可解析且 account_id 保留
TEST_F(TdAccountOpsTest, AllFramesAreJsonExtWithoutInstanceId) {
    DzTransferReq transfer_req;
    transfer_req.account_id = "a1";
    DzTransferRsp transfer_rsp;
    transfer_rsp.account_id = "a2";
    DzPasswordUpdateReq password_req;
    password_req.account_id = "a3";
    DzPasswordUpdateRsp password_rsp;
    password_rsp.account_id = "a4";

    ASSERT_TRUE(dztrader::platform::write_ext_json(*writer_, DZ_FRAME_TD_TRANSFER_REQ, transfer_req));
    ASSERT_TRUE(dztrader::platform::write_ext_json(*writer_, DZ_FRAME_TD_TRANSFER_RSP, transfer_rsp));
    ASSERT_TRUE(dztrader::platform::write_ext_json(*writer_, DZ_FRAME_TD_TRANSFER_RTN, transfer_rsp));
    ASSERT_TRUE(dztrader::platform::write_ext_json(*writer_, DZ_FRAME_TD_PASSWORD_UPDATE_REQ, password_req));
    ASSERT_TRUE(dztrader::platform::write_ext_json(*writer_, DZ_FRAME_TD_PASSWORD_UPDATE_RSP, password_rsp));

    struct Expect {
        DzFrameType type;
        const char* account_id;
    };
    const Expect expects[] = {
        {DZ_FRAME_TD_TRANSFER_REQ, "a1"},
        {DZ_FRAME_TD_TRANSFER_RSP, "a2"},
        {DZ_FRAME_TD_TRANSFER_RTN, "a2"},
        {DZ_FRAME_TD_PASSWORD_UPDATE_REQ, "a3"},
        {DZ_FRAME_TD_PASSWORD_UPDATE_RSP, "a4"},
    };
    for (const auto& e : expects) {
        auto* frame = reader_->next_frame();
        ASSERT_NE(frame, nullptr);
        const auto view = dztrader::shm::FrameView(frame);
        EXPECT_EQ(view.type(), e.type);
        // 无 instance_id: 帧头为 DzExtFrameHeader (data_size), payload 走 ext_payload
        const auto payload = nlohmann::json::parse(
            reinterpret_cast<const char*>(view.ext_payload()),
            reinterpret_cast<const char*>(view.ext_payload()) + view.ext_payload_size());
        EXPECT_EQ(payload["account_id"], e.account_id);
    }
}

// 契约 td-account-ops 校验节的现状锁定: WITH_DEFAULT 宏对缺失字段/整体 null payload
// 宽松回退默认值 (不抛错), handler 接线时必须显式校验必填字段
TEST_F(TdAccountOpsTest, WithDefaultDecodeIsLenient) {
    const auto missing_fields = nlohmann::json::parse("{}").get<DzTransferReq>();
    EXPECT_EQ(missing_fields.account_id, "");
    EXPECT_EQ(missing_fields.trade_amount, 0.0);

    const auto null_payload = nlohmann::json::parse("null").get<DzTransferReq>();
    EXPECT_EQ(null_payload.account_id, "");

    EXPECT_THROW(
        nlohmann::json::parse(R"({"account_id": null})").get<DzTransferReq>(),
        nlohmann::json::type_error);
}

}  // namespace
