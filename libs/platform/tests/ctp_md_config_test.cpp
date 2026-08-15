#include <dztrader/platform/ctp_md_config.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <dztrader/core/core_data_type.h>
#include <dztrader/shm/channel_meta.h>
#include <dztrader/shm/frame_codec.h>
#include <dztrader/shm/frame_view.h>
#include <dztrader/shm/reader.h>
#include <dztrader/shm/writer.h>

#include <filesystem>
#include <fstream>

using dztrader::platform::CtpBrokerEntry;
using dztrader::platform::CtpBrokerFrontend;
using dztrader::platform::CtpMdConfig;
using dztrader::platform::CtpMdConfigData;
using dztrader::platform::CtpMdConfigOp;
using dztrader::platform::CtpMdConfigOpReq;

using dztrader::shm::ChannelConfig;
using dztrader::shm::ChannelMeta;
using dztrader::shm::MultiWriter;
using dztrader::shm::Reader;

namespace {

// ===== 数据结构序列化测试 =====

TEST(CtpMdConfigDataSerialization, DefaultConfig) {
    CtpMdConfigData cfg;
    nlohmann::json j = cfg;
    EXPECT_EQ(j["brokers"], nlohmann::json::array());
    EXPECT_TRUE(j["current_broker_name"].is_string());
    EXPECT_EQ(j["subscribe_batch_size"], 1000);
    EXPECT_EQ(j["subscribe_batch_delay_ms"], 1000);
    EXPECT_EQ(j["sub_check_interval_ms"], 3000);
    EXPECT_EQ(j["sub_max_retry"], 3);
}

TEST(CtpMdConfigDataSerialization, RoundTripWithBrokers) {
    CtpMdConfigData cfg;
    CtpBrokerEntry b;
    b.name = "simnow";
    b.broker_id = "9999";
    b.user_id = "00001";
    b.password = "123456";
    b.product_info = "";
    b.frontends.push_back({"tcp://180.168.146.187:10131", "电信", true});
    cfg.brokers.push_back(b);
    cfg.current_broker_name = "simnow";

    nlohmann::json j = cfg;
    CtpMdConfigData restored = j.get<CtpMdConfigData>();

    EXPECT_EQ(restored.brokers.size(), 1u);
    EXPECT_EQ(restored.brokers[0].name, "simnow");
    EXPECT_EQ(restored.brokers[0].password, "123456");
    EXPECT_EQ(restored.brokers[0].frontends.size(), 1u);
    EXPECT_EQ(restored.brokers[0].frontends[0].address, "tcp://180.168.146.187:10131");
    EXPECT_EQ(restored.current_broker_name, "simnow");
}

TEST(CtpMdConfigDataSerialization, OpReqRoundTrip) {
    CtpMdConfigOpReq req;
    req.op = CtpMdConfigOp::AddBroker;
    req.params = {{"name", "simnow"}, {"broker_id", "9999"}};

    nlohmann::json j = req;
    EXPECT_EQ(j["op"], "AddBroker");
    EXPECT_EQ(j["params"]["name"], "simnow");

    CtpMdConfigOpReq restored = j.get<CtpMdConfigOpReq>();
    EXPECT_EQ(restored.op, CtpMdConfigOp::AddBroker);
    EXPECT_EQ(restored.params["name"], "simnow");
}

TEST(IsConnectionOp, AllOps) {
    EXPECT_FALSE(dztrader::platform::is_ctp_connection_op(CtpMdConfigOp::AddBroker));
    EXPECT_TRUE(dztrader::platform::is_ctp_connection_op(CtpMdConfigOp::RemoveBroker));
    EXPECT_TRUE(dztrader::platform::is_ctp_connection_op(CtpMdConfigOp::UpdateBroker));
    EXPECT_TRUE(dztrader::platform::is_ctp_connection_op(CtpMdConfigOp::SetFrontends));
    EXPECT_TRUE(dztrader::platform::is_ctp_connection_op(CtpMdConfigOp::SetCurrentBroker));
    EXPECT_FALSE(dztrader::platform::is_ctp_connection_op(CtpMdConfigOp::SetSubscribeParams));
}

// ===== validate_ctp_op_req 测试 =====

TEST(ValidateOpReq, AddBrokerValid) {
    CtpMdConfigData cfg;
    CtpMdConfigOpReq req{CtpMdConfigOp::AddBroker, {{"name", "simnow"}}};
    auto err = dztrader::platform::validate_ctp_op_req(req, cfg);
    EXPECT_FALSE(err.has_value());
}

TEST(ValidateOpReq, AddBrokerEmptyName) {
    CtpMdConfigData cfg;
    CtpMdConfigOpReq req{CtpMdConfigOp::AddBroker, {{"name", ""}}};
    auto err = dztrader::platform::validate_ctp_op_req(req, cfg);
    ASSERT_TRUE(err.has_value());
}

TEST(ValidateOpReq, AddBrokerDuplicateName) {
    CtpMdConfigData cfg;
    CtpBrokerEntry b;
    b.name = "simnow";
    cfg.brokers.push_back(b);
    CtpMdConfigOpReq req{CtpMdConfigOp::AddBroker, {{"name", "simnow"}}};
    auto err = dztrader::platform::validate_ctp_op_req(req, cfg);
    ASSERT_TRUE(err.has_value());
}

TEST(ValidateOpReq, AddBrokerMissingName) {
    CtpMdConfigData cfg;
    CtpMdConfigOpReq req{CtpMdConfigOp::AddBroker, nlohmann::json::object()};
    auto err = dztrader::platform::validate_ctp_op_req(req, cfg);
    ASSERT_TRUE(err.has_value());
}

TEST(ValidateOpReq, AddBrokerBrokerIdWrongType) {
    CtpMdConfigData cfg;
    CtpMdConfigOpReq req{CtpMdConfigOp::AddBroker, {{"name", "x"}, {"broker_id", 123}}};
    auto err = dztrader::platform::validate_ctp_op_req(req, cfg);
    ASSERT_TRUE(err.has_value());
}

TEST(ValidateOpReq, AddBrokerUserIdWrongType) {
    CtpMdConfigData cfg;
    CtpMdConfigOpReq req{CtpMdConfigOp::AddBroker, {{"name", "x"}, {"user_id", 123}}};
    auto err = dztrader::platform::validate_ctp_op_req(req, cfg);
    ASSERT_TRUE(err.has_value());
}

TEST(ValidateOpReq, UpdateBrokerProductInfoWrongType) {
    CtpMdConfigData cfg;
    CtpBrokerEntry b;
    b.name = "simnow";
    cfg.brokers.push_back(b);
    CtpMdConfigOpReq req{CtpMdConfigOp::UpdateBroker, {{"name", "simnow"}, {"product_info", 123}}};
    auto err = dztrader::platform::validate_ctp_op_req(req, cfg);
    ASSERT_TRUE(err.has_value());
}

TEST(ValidateOpReq, AddBrokerOptionalStringFieldsValid) {
    CtpMdConfigData cfg;
    CtpMdConfigOpReq req{CtpMdConfigOp::AddBroker, {
        {"name", "x"}, {"broker_id", "9999"}, {"user_id", "u"},
        {"password", "p"}, {"product_info", "pi"}
    }};
    auto err = dztrader::platform::validate_ctp_op_req(req, cfg);
    EXPECT_FALSE(err.has_value());
}

TEST(ValidateOpReq, RemoveBrokerNotFound) {
    CtpMdConfigData cfg;
    CtpMdConfigOpReq req{CtpMdConfigOp::RemoveBroker, {{"name", "ghost"}}};
    auto err = dztrader::platform::validate_ctp_op_req(req, cfg);
    ASSERT_TRUE(err.has_value());
}

TEST(ValidateOpReq, RemoveBrokerValid) {
    CtpMdConfigData cfg;
    CtpBrokerEntry b;
    b.name = "simnow";
    cfg.brokers.push_back(b);
    CtpMdConfigOpReq req{CtpMdConfigOp::RemoveBroker, {{"name", "simnow"}}};
    auto err = dztrader::platform::validate_ctp_op_req(req, cfg);
    EXPECT_FALSE(err.has_value());
}

TEST(ValidateOpReq, UpdateBrokerNotFound) {
    CtpMdConfigData cfg;
    CtpMdConfigOpReq req{CtpMdConfigOp::UpdateBroker, {{"name", "ghost"}}};
    auto err = dztrader::platform::validate_ctp_op_req(req, cfg);
    ASSERT_TRUE(err.has_value());
}

TEST(ValidateOpReq, SetFrontendsValid) {
    CtpMdConfigData cfg;
    CtpBrokerEntry b;
    b.name = "simnow";
    cfg.brokers.push_back(b);
    CtpMdConfigOpReq req{CtpMdConfigOp::SetFrontends, {
        {"name", "simnow"},
        {"frontends", nlohmann::json::array({
            {{"address", "tcp://1.2.3.4:10131"}, {"label", ""}, {"enabled", true}}
        })}
    }};
    auto err = dztrader::platform::validate_ctp_op_req(req, cfg);
    EXPECT_FALSE(err.has_value());
}

TEST(ValidateOpReq, SetFrontendsEmptyArray) {
    CtpMdConfigData cfg;
    CtpBrokerEntry b;
    b.name = "simnow";
    cfg.brokers.push_back(b);
    CtpMdConfigOpReq req{CtpMdConfigOp::SetFrontends, {
        {"name", "simnow"},
        {"frontends", nlohmann::json::array()}
    }};
    auto err = dztrader::platform::validate_ctp_op_req(req, cfg);
    EXPECT_FALSE(err.has_value());
}

TEST(ValidateOpReq, SetFrontendsBrokerNotFound) {
    CtpMdConfigData cfg;
    CtpMdConfigOpReq req{CtpMdConfigOp::SetFrontends, {
        {"name", "ghost"},
        {"frontends", nlohmann::json::array()}
    }};
    auto err = dztrader::platform::validate_ctp_op_req(req, cfg);
    ASSERT_TRUE(err.has_value());
}

TEST(ValidateOpReq, SetFrontendsNotArray) {
    CtpMdConfigData cfg;
    CtpBrokerEntry b;
    b.name = "simnow";
    cfg.brokers.push_back(b);
    CtpMdConfigOpReq req{CtpMdConfigOp::SetFrontends, {
        {"name", "simnow"},
        {"frontends", "not_an_array"}
    }};
    auto err = dztrader::platform::validate_ctp_op_req(req, cfg);
    ASSERT_TRUE(err.has_value());
}

TEST(ValidateOpReq, SetFrontendsLabelWrongType) {
    CtpMdConfigData cfg;
    CtpBrokerEntry b;
    b.name = "simnow";
    cfg.brokers.push_back(b);
    CtpMdConfigOpReq req{CtpMdConfigOp::SetFrontends, {
        {"name", "simnow"},
        {"frontends", nlohmann::json::array({
            {{"address", "tcp://1.2.3.4:10131"}, {"label", 123}, {"enabled", true}}
        })}
    }};
    auto err = dztrader::platform::validate_ctp_op_req(req, cfg);
    ASSERT_TRUE(err.has_value());
}

TEST(ValidateOpReq, SetCurrentBrokerEmptyNameClears) {
    CtpMdConfigData cfg;
    CtpMdConfigOpReq req{CtpMdConfigOp::SetCurrentBroker, {{"name", ""}}};
    auto err = dztrader::platform::validate_ctp_op_req(req, cfg);
    EXPECT_FALSE(err.has_value());
}

TEST(ValidateOpReq, SetCurrentBrokerMissingNameClears) {
    CtpMdConfigData cfg;
    CtpMdConfigOpReq req{CtpMdConfigOp::SetCurrentBroker, nlohmann::json::object()};
    auto err = dztrader::platform::validate_ctp_op_req(req, cfg);
    EXPECT_FALSE(err.has_value());
}

TEST(ValidateOpReq, SetCurrentBrokerNotFound) {
    CtpMdConfigData cfg;
    CtpMdConfigOpReq req{CtpMdConfigOp::SetCurrentBroker, {{"name", "ghost"}}};
    auto err = dztrader::platform::validate_ctp_op_req(req, cfg);
    ASSERT_TRUE(err.has_value());
}

TEST(ValidateOpReq, SetSubscribeParamsAllValid) {
    CtpMdConfigData cfg;
    CtpMdConfigOpReq req{CtpMdConfigOp::SetSubscribeParams, {
        {"subscribe_batch_size", 500},
        {"subscribe_batch_delay_ms", 2000},
        {"sub_check_interval_ms", 5000},
        {"sub_max_retry", 5}
    }};
    auto err = dztrader::platform::validate_ctp_op_req(req, cfg);
    EXPECT_FALSE(err.has_value());
}

TEST(ValidateOpReq, SetSubscribeParamsInvalidValue) {
    CtpMdConfigData cfg;
    CtpMdConfigOpReq req{CtpMdConfigOp::SetSubscribeParams, {{"subscribe_batch_size", 0}}};
    auto err = dztrader::platform::validate_ctp_op_req(req, cfg);
    ASSERT_TRUE(err.has_value());
}

TEST(ValidateOpReq, NullInParams) {
    CtpMdConfigData cfg;
    CtpMdConfigOpReq req{CtpMdConfigOp::AddBroker, {{"name", nullptr}}};
    auto err = dztrader::platform::validate_ctp_op_req(req, cfg);
    ASSERT_TRUE(err.has_value());
}

TEST(ValidateOpReq, ParamsNotObject) {
    CtpMdConfigData cfg;
    CtpMdConfigOpReq req{CtpMdConfigOp::AddBroker, "not_an_object"};
    auto err = dztrader::platform::validate_ctp_op_req(req, cfg);
    ASSERT_TRUE(err.has_value());
}

// ===== apply_ctp_config_op 测试 =====

TEST(ApplyConfigOp, AddBroker) {
    CtpMdConfigData cfg;
    CtpMdConfigOpReq req{CtpMdConfigOp::AddBroker, {
        {"name", "simnow"}, {"broker_id", "9999"}, {"user_id", "00001"},
        {"password", "123456"}, {"product_info", ""}
    }};
    dztrader::platform::apply_ctp_config_op(cfg, req);
    ASSERT_EQ(cfg.brokers.size(), 1u);
    EXPECT_EQ(cfg.brokers[0].name, "simnow");
    EXPECT_EQ(cfg.brokers[0].broker_id, "9999");
    EXPECT_EQ(cfg.brokers[0].password, "123456");
}

TEST(ApplyConfigOp, AddBrokerMissingOptionalFields) {
    CtpMdConfigData cfg;
    CtpMdConfigOpReq req{CtpMdConfigOp::AddBroker, {{"name", "simnow"}}};
    dztrader::platform::apply_ctp_config_op(cfg, req);
    ASSERT_EQ(cfg.brokers.size(), 1u);
    EXPECT_EQ(cfg.brokers[0].broker_id, "");
    EXPECT_EQ(cfg.brokers[0].password, "");
}

TEST(ApplyConfigOp, RemoveBroker) {
    CtpMdConfigData cfg;
    CtpBrokerEntry b;
    b.name = "simnow";
    cfg.brokers.push_back(b);
    cfg.current_broker_name = "simnow";
    CtpMdConfigOpReq req{CtpMdConfigOp::RemoveBroker, {{"name", "simnow"}}};
    dztrader::platform::apply_ctp_config_op(cfg, req);
    EXPECT_TRUE(cfg.brokers.empty());
    EXPECT_TRUE(cfg.current_broker_name.empty());
}

TEST(ApplyConfigOp, RemoveBrokerKeepsOthers) {
    CtpMdConfigData cfg;
    cfg.brokers.push_back({"a", "", "", "", "", {}});
    cfg.brokers.push_back({"b", "", "", "", "", {}});
    CtpMdConfigOpReq req{CtpMdConfigOp::RemoveBroker, {{"name", "a"}}};
    dztrader::platform::apply_ctp_config_op(cfg, req);
    ASSERT_EQ(cfg.brokers.size(), 1u);
    EXPECT_EQ(cfg.brokers[0].name, "b");
}

TEST(ApplyConfigOp, UpdateBrokerPasswordMask) {
    CtpMdConfigData cfg;
    CtpBrokerEntry b;
    b.name = "simnow";
    b.password = "old_pwd";
    b.user_id = "00001";
    cfg.brokers.push_back(b);
    CtpMdConfigOpReq req{CtpMdConfigOp::UpdateBroker, {{"name", "simnow"}, {"password", "****"}}};
    dztrader::platform::apply_ctp_config_op(cfg, req);
    EXPECT_EQ(cfg.brokers[0].password, "old_pwd");
}

TEST(ApplyConfigOp, UpdateBrokerPasswordEmpty) {
    CtpMdConfigData cfg;
    CtpBrokerEntry b;
    b.name = "simnow";
    b.password = "old_pwd";
    cfg.brokers.push_back(b);
    CtpMdConfigOpReq req{CtpMdConfigOp::UpdateBroker, {{"name", "simnow"}, {"password", ""}}};
    dztrader::platform::apply_ctp_config_op(cfg, req);
    EXPECT_EQ(cfg.brokers[0].password, "old_pwd");
}

TEST(ApplyConfigOp, UpdateBrokerPasswordNew) {
    CtpMdConfigData cfg;
    CtpBrokerEntry b;
    b.name = "simnow";
    b.password = "old_pwd";
    cfg.brokers.push_back(b);
    CtpMdConfigOpReq req{CtpMdConfigOp::UpdateBroker, {{"name", "simnow"}, {"password", "new_pwd"}}};
    dztrader::platform::apply_ctp_config_op(cfg, req);
    EXPECT_EQ(cfg.brokers[0].password, "new_pwd");
}

TEST(ApplyConfigOp, UpdateBrokerMissingFieldsKeepOld) {
    CtpMdConfigData cfg;
    CtpBrokerEntry b;
    b.name = "simnow";
    b.user_id = "00001";
    b.broker_id = "9999";
    cfg.brokers.push_back(b);
    CtpMdConfigOpReq req{CtpMdConfigOp::UpdateBroker, {{"name", "simnow"}, {"user_id", "00002"}}};
    dztrader::platform::apply_ctp_config_op(cfg, req);
    EXPECT_EQ(cfg.brokers[0].user_id, "00002");
    EXPECT_EQ(cfg.brokers[0].broker_id, "9999");
}

TEST(ApplyConfigOp, SetFrontends) {
    CtpMdConfigData cfg;
    CtpBrokerEntry b;
    b.name = "simnow";
    cfg.brokers.push_back(b);
    CtpMdConfigOpReq req{CtpMdConfigOp::SetFrontends, {
        {"name", "simnow"},
        {"frontends", nlohmann::json::array({
            {{"address", "tcp://1.2.3.4:10131"}, {"label", "电信"}, {"enabled", true}}
        })}
    }};
    dztrader::platform::apply_ctp_config_op(cfg, req);
    ASSERT_EQ(cfg.brokers[0].frontends.size(), 1u);
    EXPECT_EQ(cfg.brokers[0].frontends[0].address, "tcp://1.2.3.4:10131");
}

TEST(ApplyConfigOp, SetFrontendsEmptyArray) {
    CtpMdConfigData cfg;
    CtpBrokerEntry b;
    b.name = "simnow";
    b.frontends.push_back({"tcp://old:10131", "", true});
    cfg.brokers.push_back(b);
    CtpMdConfigOpReq req{CtpMdConfigOp::SetFrontends, {
        {"name", "simnow"},
        {"frontends", nlohmann::json::array()}
    }};
    dztrader::platform::apply_ctp_config_op(cfg, req);
    EXPECT_TRUE(cfg.brokers[0].frontends.empty());
}

TEST(ApplyConfigOp, SetCurrentBroker) {
    CtpMdConfigData cfg;
    cfg.brokers.push_back({"simnow", "", "", "", "", {}});
    CtpMdConfigOpReq req{CtpMdConfigOp::SetCurrentBroker, {{"name", "simnow"}}};
    dztrader::platform::apply_ctp_config_op(cfg, req);
    EXPECT_EQ(cfg.current_broker_name, "simnow");
}

TEST(ApplyConfigOp, SetCurrentBrokerClear) {
    CtpMdConfigData cfg;
    cfg.current_broker_name = "simnow";
    CtpMdConfigOpReq req{CtpMdConfigOp::SetCurrentBroker, {{"name", ""}}};
    dztrader::platform::apply_ctp_config_op(cfg, req);
    EXPECT_TRUE(cfg.current_broker_name.empty());
}

TEST(ApplyConfigOp, SetSubscribeParams) {
    CtpMdConfigData cfg;
    CtpMdConfigOpReq req{CtpMdConfigOp::SetSubscribeParams, {{"subscribe_batch_size", 500}}};
    dztrader::platform::apply_ctp_config_op(cfg, req);
    EXPECT_EQ(cfg.subscribe_batch_size, 500);
    EXPECT_EQ(cfg.subscribe_batch_delay_ms, 1000);  // 未修改
}

TEST(ApplyConfigOp, SetSubscribeParamsAllFields) {
    CtpMdConfigData cfg;
    CtpMdConfigOpReq req{CtpMdConfigOp::SetSubscribeParams, {
        {"subscribe_batch_size", 500},
        {"subscribe_batch_delay_ms", 2000},
        {"sub_check_interval_ms", 5000},
        {"sub_max_retry", 5}
    }};
    dztrader::platform::apply_ctp_config_op(cfg, req);
    EXPECT_EQ(cfg.subscribe_batch_size, 500);
    EXPECT_EQ(cfg.subscribe_batch_delay_ms, 2000);
    EXPECT_EQ(cfg.sub_check_interval_ms, 5000);
    EXPECT_EQ(cfg.sub_max_retry, 5);
}

// ===== validate_ctp_config 测试 =====

TEST(Validate, DefaultConfig) {
    CtpMdConfigData cfg;
    auto err = dztrader::platform::validate_ctp_config(cfg);
    EXPECT_FALSE(err.has_value());
}

TEST(Validate, DuplicateBrokerNames) {
    CtpMdConfigData cfg;
    cfg.brokers.push_back({"simnow", "", "", "", "", {}});
    cfg.brokers.push_back({"simnow", "", "", "", "", {}});
    auto err = dztrader::platform::validate_ctp_config(cfg);
    ASSERT_TRUE(err.has_value());
}

TEST(Validate, EmptyBrokerName) {
    CtpMdConfigData cfg;
    cfg.brokers.push_back({"", "", "", "", "", {}});
    auto err = dztrader::platform::validate_ctp_config(cfg);
    ASSERT_TRUE(err.has_value());
}

TEST(Validate, CurrentBrokerNotInList) {
    CtpMdConfigData cfg;
    cfg.current_broker_name = "ghost";
    auto err = dztrader::platform::validate_ctp_config(cfg);
    ASSERT_TRUE(err.has_value());
}

TEST(Validate, CurrentBrokerInList) {
    CtpMdConfigData cfg;
    cfg.brokers.push_back({"simnow", "", "", "", "", {}});
    cfg.current_broker_name = "simnow";
    auto err = dztrader::platform::validate_ctp_config(cfg);
    EXPECT_FALSE(err.has_value());
}

TEST(Validate, CurrentBrokerEmpty) {
    CtpMdConfigData cfg;
    cfg.current_broker_name = "";
    auto err = dztrader::platform::validate_ctp_config(cfg);
    EXPECT_FALSE(err.has_value());
}

TEST(Validate, FrontendEmptyAddress) {
    CtpMdConfigData cfg;
    CtpBrokerEntry b;
    b.name = "simnow";
    b.frontends.push_back({"", "label", true});
    cfg.brokers.push_back(b);
    auto err = dztrader::platform::validate_ctp_config(cfg);
    ASSERT_TRUE(err.has_value());
}

TEST(Validate, BatchSizeZero) {
    CtpMdConfigData cfg;
    cfg.subscribe_batch_size = 0;
    auto err = dztrader::platform::validate_ctp_config(cfg);
    ASSERT_TRUE(err.has_value());
}

TEST(Validate, BatchDelayNegative) {
    CtpMdConfigData cfg;
    cfg.subscribe_batch_delay_ms = -1;
    auto err = dztrader::platform::validate_ctp_config(cfg);
    ASSERT_TRUE(err.has_value());
}

TEST(Validate, CheckIntervalZero) {
    CtpMdConfigData cfg;
    cfg.sub_check_interval_ms = 0;
    auto err = dztrader::platform::validate_ctp_config(cfg);
    ASSERT_TRUE(err.has_value());
}

TEST(Validate, MaxRetryNegative) {
    CtpMdConfigData cfg;
    cfg.sub_max_retry = -1;
    auto err = dztrader::platform::validate_ctp_config(cfg);
    ASSERT_TRUE(err.has_value());
}

// ===== ctp_config_to_safe_json 测试 =====

TEST(ToSafeJson, PasswordMasked) {
    CtpMdConfigData cfg;
    CtpBrokerEntry b;
    b.name = "simnow";
    b.password = "secret123";
    cfg.brokers.push_back(b);
    nlohmann::json j = dztrader::platform::ctp_config_to_safe_json(cfg);
    EXPECT_EQ(j["brokers"][0]["password"], "****");
    EXPECT_EQ(j["brokers"][0]["name"], "simnow");
}

TEST(ToSafeJson, MultipleBrokers) {
    CtpMdConfigData cfg;
    cfg.brokers.push_back({"a", "", "", "pwd1", "", {}});
    cfg.brokers.push_back({"b", "", "", "pwd2", "", {}});
    nlohmann::json j = dztrader::platform::ctp_config_to_safe_json(cfg);
    EXPECT_EQ(j["brokers"][0]["password"], "****");
    EXPECT_EQ(j["brokers"][1]["password"], "****");
}

TEST(ToSafeJson, EmptyBrokers) {
    CtpMdConfigData cfg;
    nlohmann::json j = dztrader::platform::ctp_config_to_safe_json(cfg);
    EXPECT_EQ(j["brokers"], nlohmann::json::array());
    EXPECT_EQ(j["subscribe_batch_size"], 1000);
}

// ===== CtpMdConfig 配置类测试 =====

class CtpMdConfigTest : public ::testing::Test {
protected:
    std::string channel_name_;
    std::filesystem::path shm_dir_;
    std::filesystem::path cfg_path_;
    std::shared_ptr<ChannelMeta> meta_;
    std::optional<MultiWriter> writer_;
    std::optional<Reader> reader_;

    static constexpr uint64_t MB = 1024 * 1024;

    void SetUp() override {
        channel_name_ =
            "dz_test_ctp_md_cfg_" + std::to_string(reinterpret_cast<uintptr_t>(this));
        shm_dir_ = std::filesystem::temp_directory_path() / channel_name_;
        std::filesystem::remove_all(shm_dir_);
        std::filesystem::create_directories(shm_dir_);

        cfg_path_ = shm_dir_ / "config.json";
        std::filesystem::remove(cfg_path_);

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

    void TearDown() override {
        reader_.reset();
        writer_.reset();
        meta_.reset();
        std::error_code ec;
        std::filesystem::remove_all(shm_dir_, ec);
    }

    void write_cfg_file(const std::string& content) {
        std::ofstream ofs(cfg_path_, std::ios::binary | std::ios::trunc);
        ofs << content;
    }

    std::string read_cfg_file() {
        std::ifstream ifs(cfg_path_);
        return std::string(std::istreambuf_iterator<char>(ifs),
                           std::istreambuf_iterator<char>());
    }

    nlohmann::json read_last_rtn() {
        nlohmann::json last;
        while (auto* frame = reader_->next_frame()) {
            auto view = dztrader::shm::FrameView(frame);
            if (view.type() == DZ_FRAME_RTN_MD_CONFIG) {
                const auto* data = reinterpret_cast<const char*>(view.ext_inst_payload());
                last = nlohmann::json::parse(data, data + view.ext_inst_payload_size());
            }
        }
        return last;
    }
};

TEST_F(CtpMdConfigTest, LoadDefaultWhenFileMissing) {
    CtpMdConfig md("dzmd_ctp", cfg_path_, *writer_);
    md.load();
    const auto& cfg = md.config();
    EXPECT_TRUE(cfg.brokers.empty());
    EXPECT_EQ(cfg.subscribe_batch_size, 1000);
}

TEST_F(CtpMdConfigTest, LoadFromFile) {
    write_cfg_file(R"({"md": {
        "brokers": [{"name": "simnow", "broker_id": "9999", "user_id": "00001",
                       "password": "123456", "product_info": "", "frontends": []}],
        "current_broker_name": "simnow",
        "subscribe_batch_size": 500,
        "subscribe_batch_delay_ms": 2000,
        "sub_check_interval_ms": 5000,
        "sub_max_retry": 5
    }})");
    CtpMdConfig md("dzmd_ctp", cfg_path_, *writer_);
    md.load();
    const auto& cfg = md.config();
    ASSERT_EQ(cfg.brokers.size(), 1u);
    EXPECT_EQ(cfg.brokers[0].name, "simnow");
    EXPECT_EQ(cfg.subscribe_batch_size, 500);
}

TEST_F(CtpMdConfigTest, LoadStaleCurrentBrokerHealed) {
    write_cfg_file(R"({"md": {
        "brokers": [{"name": "simnow", "broker_id": "9999", "user_id": "00001",
                       "password": "123456", "product_info": "", "frontends": []}],
        "current_broker_name": "ghost",
        "subscribe_batch_size": 1000,
        "subscribe_batch_delay_ms": 1000,
        "sub_check_interval_ms": 3000,
        "sub_max_retry": 3
    }})");
    CtpMdConfig md("dzmd_ctp", cfg_path_, *writer_);
    md.load();
    EXPECT_TRUE(md.config().current_broker_name.empty());
}

TEST_F(CtpMdConfigTest, LoadDegradedUsesDefaults) {
    write_cfg_file("not valid json");
    CtpMdConfig md("dzmd_ctp", cfg_path_, *writer_);
    md.load();
    EXPECT_TRUE(md.config().brokers.empty());
    EXPECT_EQ(md.config().subscribe_batch_size, 1000);
}

TEST_F(CtpMdConfigTest, LoadSectionNotObjectUsesDefaults) {
    write_cfg_file(R"({"md": "not_an_object"})");
    CtpMdConfig md("dzmd_ctp", cfg_path_, *writer_);
    md.load();
    EXPECT_TRUE(md.config().brokers.empty());
    EXPECT_EQ(md.config().subscribe_batch_size, 1000);
}

TEST_F(CtpMdConfigTest, SetMdConfigAddBroker) {
    CtpMdConfig md("dzmd_ctp", cfg_path_, *writer_);
    md.load();
    CtpMdConfigOpReq req{CtpMdConfigOp::AddBroker, {
        {"name", "simnow"}, {"broker_id", "9999"}, {"user_id", "00001"},
        {"password", "123456"}, {"product_info", ""}
    }};
    md.set_md_config(req);
    ASSERT_EQ(md.config().brokers.size(), 1u);
    EXPECT_EQ(md.config().brokers[0].name, "simnow");
}

TEST_F(CtpMdConfigTest, SetMdConfigRejectsDuplicate) {
    CtpMdConfig md("dzmd_ctp", cfg_path_, *writer_);
    md.load();
    CtpMdConfigOpReq req1{CtpMdConfigOp::AddBroker, {{"name", "simnow"}}};
    md.set_md_config(req1);
    CtpMdConfigOpReq req2{CtpMdConfigOp::AddBroker, {{"name", "simnow"}}};
    EXPECT_THROW(md.set_md_config(req2), std::runtime_error);
    EXPECT_EQ(md.config().brokers.size(), 1u);
}

TEST_F(CtpMdConfigTest, SetMdConfigPersistsToFile) {
    CtpMdConfig md("dzmd_ctp", cfg_path_, *writer_);
    md.load();
    CtpMdConfigOpReq req{CtpMdConfigOp::AddBroker, {{"name", "simnow"}}};
    md.set_md_config(req);
    // 重新加载验证持久化
    CtpMdConfig md2("dzmd_ctp", cfg_path_, *writer_);
    md2.load();
    ASSERT_EQ(md2.config().brokers.size(), 1u);
    EXPECT_EQ(md2.config().brokers[0].name, "simnow");
}

TEST_F(CtpMdConfigTest, RtnMdConfigSendsFrame) {
    CtpMdConfig md("dzmd_ctp", cfg_path_, *writer_);
    md.load();
    CtpMdConfigOpReq req{CtpMdConfigOp::AddBroker, {
        {"name", "simnow"}, {"password", "secret"}
    }};
    md.set_md_config(req);
    md.rtn_md_config();
    auto rtn = read_last_rtn();
    ASSERT_FALSE(rtn.is_null());
    EXPECT_EQ(rtn["brokers"][0]["name"], "simnow");
    EXPECT_EQ(rtn["brokers"][0]["password"], "****");
}

TEST_F(CtpMdConfigTest, SetSubscribeParams) {
    CtpMdConfig md("dzmd_ctp", cfg_path_, *writer_);
    md.load();
    CtpMdConfigOpReq req{CtpMdConfigOp::SetSubscribeParams, {{"subscribe_batch_size", 500}}};
    md.set_md_config(req);
    EXPECT_EQ(md.config().subscribe_batch_size, 500);
}

TEST_F(CtpMdConfigTest, RemoveBrokerClearsCurrent) {
    CtpMdConfig md("dzmd_ctp", cfg_path_, *writer_);
    md.load();
    md.set_md_config({CtpMdConfigOp::AddBroker, {{"name", "simnow"}}});
    md.set_md_config({CtpMdConfigOp::SetCurrentBroker, {{"name", "simnow"}}});
    EXPECT_EQ(md.config().current_broker_name, "simnow");
    md.set_md_config({CtpMdConfigOp::RemoveBroker, {{"name", "simnow"}}});
    EXPECT_TRUE(md.config().current_broker_name.empty());
}

// ===== 端到端集成测试 =====

TEST_F(CtpMdConfigTest, CoexistsWithOtherSections) {
    // 先写入含 log 和 md 两个 section 的配置文件
    write_cfg_file(R"({"log": {"level": "info", "flush_on": "warning"}, "md": {
        "brokers": [{"name": "simnow", "broker_id": "9999", "user_id": "00001",
                       "password": "123456", "product_info": "", "frontends": []}],
        "current_broker_name": "simnow",
        "subscribe_batch_size": 1000,
        "subscribe_batch_delay_ms": 1000,
        "sub_check_interval_ms": 3000,
        "sub_max_retry": 3
    }})");
    CtpMdConfig md("dzmd_ctp", cfg_path_, *writer_);
    md.load();
    ASSERT_EQ(md.config().brokers.size(), 1u);

    // 修改 md 配置
    md.set_md_config({CtpMdConfigOp::AddBroker, {{"name", "second"}}});

    // 验证 log section 未被破坏
    std::ifstream ifs(cfg_path_);
    nlohmann::json full;
    ifs >> full;
    EXPECT_EQ(full["log"]["level"], "info");
    EXPECT_EQ(full["md"]["brokers"][1]["name"], "second");
}

TEST_F(CtpMdConfigTest, RtnAlwaysFullConfig) {
    CtpMdConfig md("dzmd_ctp", cfg_path_, *writer_);
    md.load();
    md.set_md_config({CtpMdConfigOp::AddBroker, {{"name", "a"}, {"password", "pw1"}}});
    md.set_md_config({CtpMdConfigOp::AddBroker, {{"name", "b"}, {"password", "pw2"}}});
    md.set_md_config({CtpMdConfigOp::SetSubscribeParams, {{"subscribe_batch_size", 500}}});
    md.rtn_md_config();
    auto rtn = read_last_rtn();
    ASSERT_FALSE(rtn.is_null());
    EXPECT_EQ(rtn["brokers"].size(), 2u);
    EXPECT_EQ(rtn["brokers"][0]["password"], "****");
    EXPECT_EQ(rtn["brokers"][1]["password"], "****");
    EXPECT_EQ(rtn["subscribe_batch_size"], 500);
    EXPECT_EQ(rtn["subscribe_batch_delay_ms"], 1000);
}

TEST_F(CtpMdConfigTest, SetMdConfigFailureRollsBack) {
    CtpMdConfig md("dzmd_ctp", cfg_path_, *writer_);
    md.load();
    md.set_md_config({CtpMdConfigOp::AddBroker, {{"name", "simnow"}}});
    auto state_before = md.config();

    // 尝试一个会失败的 op（subscribe_batch_size = 0）
    EXPECT_THROW(md.set_md_config({CtpMdConfigOp::SetSubscribeParams, {
        {"subscribe_batch_size", 0}
    }}), std::runtime_error);

    // cfg_ 不变
    EXPECT_EQ(md.config().brokers.size(), state_before.brokers.size());
    EXPECT_EQ(md.config().subscribe_batch_size, state_before.subscribe_batch_size);
}

TEST_F(CtpMdConfigTest, LoadDegradedRepairsFile) {
    write_cfg_file("corrupt json {{{");
    CtpMdConfig md("dzmd_ctp", cfg_path_, *writer_);
    md.load();
    // 文件应被修复为合法 JSON
    std::ifstream ifs(cfg_path_);
    ASSERT_TRUE(ifs.good());
    nlohmann::json full;
    EXPECT_NO_THROW(ifs >> full);
    EXPECT_TRUE(full.is_object());
}

}  // namespace