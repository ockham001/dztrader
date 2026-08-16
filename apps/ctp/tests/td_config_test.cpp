#include "td/td_config.h"

#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>

namespace dztrader::ctp {
namespace {

class TdConfigTest : public ::testing::Test {
protected:
    void SetUp() override {
        tmp_dir_ = std::filesystem::temp_directory_path() / "dz_td_config_test";
        std::filesystem::create_directories(tmp_dir_);
    }
    void TearDown() override { std::filesystem::remove_all(tmp_dir_); }

    void write_json(const std::string& content) {
        path_ = tmp_dir_ / "dztd_ctp.json";
        std::ofstream ofs(path_);
        ofs << "{\"td\": " << content << "}";
    }

    std::filesystem::path tmp_dir_;
    std::filesystem::path path_;
};

// --- JSON 加载 ---

TEST_F(TdConfigTest, LoadFullConfig) {
    write_json(R"json({
  "enable_auto_login_logout": true,
  "schedules": [
    {"login_time": "08:45", "logout_time": "15:30"},
    {"login_time": "20:45", "logout_time": "02:35"}
  ],
  "qry_account_interval_s": 5,
  "qry_position_interval_s": 5,
  "qry_flush_interval_ms": 1500,
  "enable_lock_mode": true,
  "accounts": [
    {
      "account_id": "account_001",
      "broker": {
        "name": "simnow",
        "broker_id": "9999",
        "user_id": "00001",
        "password": "123456",
        "product_info": "",
        "frontends": [
          {"address": "tcp://180.168.146.187:10130", "label": "SimNow 7x24", "enabled": true}
        ]
      },
      "auth_code": "",
      "app_id": "",
      "flow_dir": "",
      "enabled": true,
      "risk_control_enabled": false,
      "currency_id": "CNY"
    }
  ]
})json");

    auto cfg = TdConfig::load(path_, "td");
    // 旧字段 enable_auto_login_logout/schedules 随契约 auto-login 迁移移除,
    // 文件中残留的旧字段被未知字段忽略 (兼容旧配置文件)
    EXPECT_EQ(cfg.qry_account_interval_s, 5);
    EXPECT_EQ(cfg.qry_position_interval_s, 5);
    EXPECT_EQ(cfg.qry_flush_interval_ms, 1500);
    EXPECT_TRUE(cfg.enable_lock_mode);
    ASSERT_EQ(cfg.accounts.size(), 1u);
    EXPECT_EQ(cfg.accounts[0].account_id, "account_001");
    EXPECT_EQ(cfg.accounts[0].broker.name, "simnow");
    EXPECT_EQ(cfg.accounts[0].broker.broker_id, "9999");
    EXPECT_EQ(cfg.accounts[0].broker.password, "123456");
    ASSERT_EQ(cfg.accounts[0].broker.frontends.size(), 1u);
    EXPECT_EQ(cfg.accounts[0].broker.frontends[0].address, "tcp://180.168.146.187:10130");
    EXPECT_EQ(cfg.accounts[0].currency_id, "CNY");
    EXPECT_FALSE(cfg.accounts[0].risk_control_enabled);
}

TEST_F(TdConfigTest, LoadDefaultsOmittedFields) {
    write_json(R"json({"accounts": []})json");
    auto cfg = TdConfig::load(path_, "td");
    EXPECT_EQ(cfg.qry_account_interval_s, 5);
    EXPECT_EQ(cfg.qry_position_interval_s, 5);
    EXPECT_EQ(cfg.qry_flush_interval_ms, 1500);
    EXPECT_TRUE(cfg.enable_lock_mode);
    EXPECT_TRUE(cfg.accounts.empty());
}

TEST_F(TdConfigTest, LoadFileNotExistsReturnsDefaults) {
    auto cfg = TdConfig::load(tmp_dir_ / "nonexistent.json", "td");
    EXPECT_TRUE(cfg.accounts.empty());
}

TEST_F(TdConfigTest, LoadSectionMissingReturnsDefaults) {
    path_ = tmp_dir_ / "dztd_ctp.json";
    std::ofstream ofs(path_);
    ofs << R"json({"log": {"level": "info"}})json";
    ofs.close();  // 显式关闭, 确保数据落盘后再 load
    auto cfg = TdConfig::load(path_, "td");
    EXPECT_TRUE(cfg.accounts.empty());
    // 校验其他默认值
    EXPECT_EQ(cfg.qry_account_interval_s, 5);
    EXPECT_EQ(cfg.qry_position_interval_s, 5);
    EXPECT_EQ(cfg.qry_flush_interval_ms, 1500);
    EXPECT_TRUE(cfg.enable_lock_mode);
}

// --- Save + Load 往返 ---

TEST_F(TdConfigTest, SaveAndLoadRoundTrip) {
    TdConfig cfg;
    cfg.qry_account_interval_s = 10;
    AccountConfig acct;
    acct.account_id = "test_001";
    acct.broker.name = "broker1";
    acct.broker.broker_id = "8888";
    acct.broker.user_id = "user1";
    acct.broker.password = "secret";
    acct.risk_control_enabled = true;
    cfg.accounts.push_back(std::move(acct));

    path_ = tmp_dir_ / "dztd_ctp.json";
    cfg.save(path_, "td");

    auto loaded = TdConfig::load(path_, "td");
    EXPECT_EQ(loaded.qry_account_interval_s, 10);
    ASSERT_EQ(loaded.accounts.size(), 1u);
    EXPECT_EQ(loaded.accounts[0].account_id, "test_001");
    EXPECT_EQ(loaded.accounts[0].broker.name, "broker1");
    EXPECT_EQ(loaded.accounts[0].broker.password, "secret");
    EXPECT_TRUE(loaded.accounts[0].risk_control_enabled);
}

TEST_F(TdConfigTest, SavePreservesOtherSections) {
    path_ = tmp_dir_ / "dztd_ctp.json";
    // 先写一个含 log section 的文件
    std::ofstream ofs(path_);
    ofs << R"json({"log": {"level": "info", "flush_on": "warning"}})json";
    ofs.close();

    // 保存 td section
    TdConfig cfg;
    cfg.accounts.push_back({});
    cfg.accounts[0].account_id = "a1";
    cfg.save(path_, "td");

    // 验证 log section 仍在
    std::ifstream ifs(path_);
    nlohmann::json j;
    ifs >> j;
    EXPECT_TRUE(j.contains("log"));
    EXPECT_EQ(j["log"]["level"], "info");
    EXPECT_TRUE(j.contains("td"));
}

// --- to_safe_json 脱敏 ---

TEST_F(TdConfigTest, ToSafeJsonMasksPasswords) {
    TdConfig cfg;
    AccountConfig acct;
    acct.account_id = "a1";
    acct.broker.name = "b1";
    acct.broker.password = "super_secret";
    cfg.accounts.push_back(acct);

    auto j = cfg.to_safe_json();
    ASSERT_TRUE(j.contains("accounts"));
    ASSERT_EQ(j["accounts"].size(), 1u);
    EXPECT_EQ(j["accounts"][0]["broker"]["password"], "****");
}

TEST_F(TdConfigTest, ToSafeJsonDoesNotModifyOriginal) {
    TdConfig cfg;
    cfg.accounts.push_back({});
    cfg.accounts[0].broker.password = "original";
    auto j = cfg.to_safe_json();
    EXPECT_EQ(j["accounts"][0]["broker"]["password"], "****");
    // 原对象未被修改
    EXPECT_EQ(cfg.accounts[0].broker.password, "original");
}

// --- validate ---

TEST_F(TdConfigTest, ValidatePassesForValidConfig) {
    TdConfig cfg;
    AccountConfig acct;
    acct.account_id = "a1";
    acct.broker.name = "b1";
    acct.broker.broker_id = "broker1";
    acct.broker.user_id = "user1";
    cfg.accounts.push_back(acct);
    EXPECT_EQ(validate(cfg), std::nullopt);
}

TEST_F(TdConfigTest, ValidateRejectsDuplicateAccountId) {
    TdConfig cfg;
    AccountConfig a1;
    a1.account_id = "dup";
    a1.broker.name = "b1";
    a1.broker.broker_id = "broker1";
    a1.broker.user_id = "user1";
    AccountConfig a2;
    a2.account_id = "dup";
    a2.broker.name = "b2";
    a2.broker.broker_id = "broker1";
    a2.broker.user_id = "user2";
    cfg.accounts.push_back(a1);
    cfg.accounts.push_back(a2);
    auto err = validate(cfg);
    ASSERT_TRUE(err.has_value());
    EXPECT_NE(err->find("duplicate"), std::string::npos);
}

TEST_F(TdConfigTest, ValidateRejectsEmptyAccountId) {
    TdConfig cfg;
    AccountConfig acct;
    acct.account_id = "";  // 空
    acct.broker.name = "b1";
    cfg.accounts.push_back(acct);
    auto err = validate(cfg);
    ASSERT_TRUE(err.has_value());
    EXPECT_NE(err->find("empty"), std::string::npos);
}

TEST_F(TdConfigTest, ValidateRejectsZeroQryInterval) {
    TdConfig cfg;
    cfg.qry_account_interval_s = 0;
    auto err = validate(cfg);
    ASSERT_TRUE(err.has_value());
    EXPECT_NE(err->find("qry_account_interval_s"), std::string::npos);
}

TEST_F(TdConfigTest, ValidateRejectsNegativeFlushInterval) {
    TdConfig cfg;
    cfg.qry_flush_interval_ms = -1;
    auto err = validate(cfg);
    ASSERT_TRUE(err.has_value());
    EXPECT_NE(err->find("qry_flush_interval_ms"), std::string::npos);
}

TEST_F(TdConfigTest, ValidateRejectsZeroFlushInterval) {
    TdConfig cfg;
    cfg.qry_flush_interval_ms = 0;  // 0 应被拒绝 (会导致忙循环)
    auto err = validate(cfg);
    ASSERT_TRUE(err.has_value());
    EXPECT_NE(err->find("qry_flush_interval_ms"), std::string::npos);
}

TEST_F(TdConfigTest, ValidateRejectsZeroAccountInterval) {
    TdConfig cfg;
    cfg.qry_account_interval_s = 0;
    auto err = validate(cfg);
    ASSERT_TRUE(err.has_value());
    EXPECT_NE(err->find("qry_account_interval_s"), std::string::npos);
}

TEST_F(TdConfigTest, ValidateRejectsZeroPositionInterval) {
    TdConfig cfg;
    cfg.qry_position_interval_s = 0;
    auto err = validate(cfg);
    ASSERT_TRUE(err.has_value());
    EXPECT_NE(err->find("qry_position_interval_s"), std::string::npos);
}

TEST_F(TdConfigTest, ValidateRejectsEmptyBrokerId) {
    TdConfig cfg;
    AccountConfig acct;
    acct.account_id = "acc1";
    acct.broker.broker_id = "";  // 空
    acct.broker.user_id = "user1";
    cfg.accounts.push_back(acct);
    auto err = validate(cfg);
    ASSERT_TRUE(err.has_value());
    EXPECT_NE(err->find("broker_id"), std::string::npos);
}

TEST_F(TdConfigTest, ValidateRejectsEmptyUserId) {
    TdConfig cfg;
    AccountConfig acct;
    acct.account_id = "acc1";
    acct.broker.broker_id = "broker1";
    acct.broker.user_id = "";  // 空
    cfg.accounts.push_back(acct);
    auto err = validate(cfg);
    ASSERT_TRUE(err.has_value());
    EXPECT_NE(err->find("user_id"), std::string::npos);
}

}  // namespace
}  // namespace dztrader::ctp
