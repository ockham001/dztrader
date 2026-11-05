#include "td/td_config.h"
#include "test_temp_dir.h"
#include "tests/td_config_fixture.h"

#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace dztrader::ctp {
namespace {

class TdConfigTest : public ::testing::Test {
protected:
    void SetUp() override {
        tmp_dir_ = dztrader::test::unique_temp_dir("dz_td_config_test");
        std::filesystem::create_directories(tmp_dir_);
    }
    void TearDown() override { std::filesystem::remove_all(tmp_dir_); }

    void write_json(const std::string& content) {
        path_ = tmp_dir_ / "dztd_ctp.json";
        std::ofstream ofs(path_);
        ofs << "{\"td\": " << content << "}";
    }

    /// 用夹具 seed 写 `dztd_ctp.json`：文件形态与真实配置同源（见 tests/td_config_fixture.h），
    /// 不在用例里手写 TdConfig 的形状。
    void write_config(const dztrader::test::TdConfigSeed& seed) {
        path_ = tmp_dir_ / "dztd_ctp.json";
        std::ofstream ofs(path_);
        ofs << dztrader::test::make_td_config_json(seed).dump();
    }

    std::filesystem::path tmp_dir_;
    std::filesystem::path path_;
};

// --- JSON 加载（契约 td-config §2.4 通用层 + ext 分层）---

TEST_F(TdConfigTest, LoadFullConfig) {
    dztrader::test::TdConfigSeed seed;
    seed.qry_position_interval_s = 5;
    seed.frontend_address = "tcp://180.168.146.187:10130";
    seed.frontend_label = "SimNow 7x24";
    write_config(seed);

    auto cfg = TdConfig::load(path_, "td");
    EXPECT_EQ(cfg.qry_account_interval_s, 5);
    EXPECT_EQ(cfg.qry_position_interval_s, 5);
    EXPECT_EQ(cfg.qry_flush_interval_ms, 1500);
    EXPECT_TRUE(cfg.ext.enable_lock_mode);
    ASSERT_EQ(cfg.accounts.size(), 1u);
    EXPECT_EQ(cfg.accounts[0].account_id, "account_001");
    EXPECT_EQ(cfg.accounts[0].ext.broker_id, "9999");
    EXPECT_EQ(cfg.accounts[0].ext.user_id, "00001");
    EXPECT_EQ(cfg.accounts[0].ext.password, "123456");
    ASSERT_EQ(cfg.accounts[0].ext.frontends.size(), 1u);
    EXPECT_EQ(cfg.accounts[0].ext.frontends[0].address, "tcp://180.168.146.187:10130");
    EXPECT_EQ(cfg.accounts[0].currency_id, "CNY");
    EXPECT_FALSE(cfg.accounts[0].risk_control_enabled);
}

TEST_F(TdConfigTest, LoadDefaultsOmittedFields) {
    write_json(R"json({"accounts": []})json");
    auto cfg = TdConfig::load(path_, "td");
    EXPECT_EQ(cfg.qry_account_interval_s, 5);
    EXPECT_EQ(cfg.qry_position_interval_s, 60);
    EXPECT_EQ(cfg.qry_flush_interval_ms, 1500);
    EXPECT_TRUE(cfg.ext.enable_lock_mode);
    EXPECT_TRUE(cfg.accounts.empty());
}

TEST_F(TdConfigTest, LoadFileNotExistsReturnsDefaults) {
    const auto missing = tmp_dir_ / "nonexistent.json";
    auto cfg = TdConfig::load(missing, "td");
    EXPECT_TRUE(cfg.accounts.empty());
    EXPECT_EQ(cfg.qry_account_interval_s, 5);
    EXPECT_EQ(cfg.qry_position_interval_s, 60);
    EXPECT_EQ(cfg.qry_flush_interval_ms, 1500);
    EXPECT_TRUE(cfg.ext.enable_lock_mode);
}

/// 契约 td-config §6「文件缺失」路径：自愈写回，使下次启动拿到可编辑的文件。
TEST_F(TdConfigTest, LoadFileNotExistsRepairsFile) {
    const auto missing = tmp_dir_ / "nonexistent.json";
    ASSERT_FALSE(std::filesystem::exists(missing));
    (void)TdConfig::load(missing, "td");
    ASSERT_TRUE(std::filesystem::exists(missing));

    std::ifstream ifs(missing);
    nlohmann::json j;
    ifs >> j;
    ASSERT_TRUE(j.contains("td"));
    EXPECT_TRUE(j["td"].contains("ext"));
    EXPECT_TRUE(j["td"]["accounts"].is_array());
}

TEST_F(TdConfigTest, LoadMalformedJsonThrowsWithPath) {
    path_ = tmp_dir_ / "dztd_ctp.json";
    std::ofstream ofs(path_);
    ofs << R"json({"td": {"qry_account_interval_s": )json";  // 截断的非法 JSON
    ofs.close();

    try {
        auto cfg = TdConfig::load(path_, "td");
        (void)cfg;
        FAIL() << "expected std::runtime_error for malformed json";
    } catch (const std::runtime_error& e) {
        const std::string msg = e.what();
        EXPECT_NE(msg.find("parse failed"), std::string::npos) << msg;
        EXPECT_NE(msg.find(path_.string()), std::string::npos) << msg;
    }
}

TEST_F(TdConfigTest, LoadSectionMissingReturnsDefaults) {
    path_ = tmp_dir_ / "dztd_ctp.json";
    std::ofstream ofs(path_);
    ofs << R"json({"log": {"level": "info"}})json";
    ofs.close();  // 显式关闭, 确保数据落盘后再 load
    auto cfg = TdConfig::load(path_, "td");
    EXPECT_TRUE(cfg.accounts.empty());
    EXPECT_EQ(cfg.qry_account_interval_s, 5);
    EXPECT_EQ(cfg.qry_position_interval_s, 60);
    EXPECT_EQ(cfg.qry_flush_interval_ms, 1500);
    EXPECT_TRUE(cfg.ext.enable_lock_mode);
}

/// 契约 td-config §6「section 缺失」也走自愈：补出 td section，且保留同文件其他 section。
TEST_F(TdConfigTest, LoadSectionMissingRepairsFilePreservingOtherSections) {
    path_ = tmp_dir_ / "dztd_ctp.json";
    {
        std::ofstream ofs(path_);
        ofs << R"json({"log": {"level": "info"}})json";
    }
    (void)TdConfig::load(path_, "td");

    std::ifstream ifs(path_);
    nlohmann::json j;
    ifs >> j;
    EXPECT_TRUE(j.contains("td"));
    ASSERT_TRUE(j.contains("log"));
    EXPECT_EQ(j["log"]["level"], "info");
}

/// 契约 td-config §6 + 总则 §8：旧扁平格式（连接参数直接在账户对象上）不再兼容。
/// 未知字段按总则 §8 忽略，因此解码**不报错**——但账户的 `ext` 为空，`validate()` 以
/// `broker_id empty` 拒绝，`td_main` 据此致命退出（旧文件按 §6「校验失败」处理）。
TEST_F(TdConfigTest, LoadLegacyFlatAccountFormatIsRejectedByValidate) {
    write_json(R"json({
  "qry_account_interval_s": 5,
  "accounts": [
    {
      "account_id": "account_001",
      "broker": {"name": "simnow", "broker_id": "9999", "user_id": "00001", "password": "123456"}
    }
  ]
})json");

    auto cfg = TdConfig::load(path_, "td");  // 解码宽容：未知字段 broker 被忽略（总则 §8）
    ASSERT_EQ(cfg.accounts.size(), 1u);
    EXPECT_EQ(cfg.accounts[0].account_id, "account_001");
    EXPECT_TRUE(cfg.accounts[0].ext.broker_id.empty());  // 连接参数丢失

    // 关键断言：旧格式的文件不会被静默接受——校验失败，启动方走致命退出（契约 §6）
    auto err = validate(cfg);
    ASSERT_TRUE(err.has_value());
    EXPECT_NE(err->find("broker_id empty"), std::string::npos) << *err;
}

/// 契约 td-config §6：**内容非法致命**——类型不符（不是 JSON 语法错）必须抛异常，
/// 而不是静默回落默认值把账户配置清空。
TEST_F(TdConfigTest, LoadTypeMismatchIsFatal) {
    write_json(R"json({"accounts": "not-an-array"})json");
    try {
        auto cfg = TdConfig::load(path_, "td");
        (void)cfg;
        FAIL() << "expected std::runtime_error for type mismatch";
    } catch (const std::runtime_error& e) {
        const std::string msg = e.what();
        EXPECT_NE(msg.find("invalid"), std::string::npos) << msg;
        EXPECT_NE(msg.find(path_.string()), std::string::npos) << msg;
    }
}

/// 非法内容不写回文件（避免把坏文件覆盖成默认值，用户失去修复依据）。
TEST_F(TdConfigTest, LoadInvalidContentDoesNotOverwriteFile) {
    write_json(R"json({"qry_account_interval_s": "not-a-number"})json");
    std::string before;
    {
        std::ifstream ifs(path_);
        before.assign(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
    }
    EXPECT_THROW((void)TdConfig::load(path_, "td"), std::runtime_error);
    std::string after;
    {
        std::ifstream ifs(path_);
        after.assign(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
    }
    EXPECT_EQ(before, after);
}

// --- Save + Load 往返 ---

TEST_F(TdConfigTest, SaveAndLoadRoundTrip) {
    TdConfig cfg;
    cfg.qry_account_interval_s = 10;
    cfg.ext.enable_lock_mode = false;
    AccountConfig acct;
    acct.account_id = "test_001";
    acct.ext.broker_id = "8888";
    acct.ext.user_id = "user1";
    acct.ext.password = "secret";
    acct.ext.auth_code = "auth";
    acct.ext.app_id = "app";
    acct.risk_control_enabled = true;
    cfg.accounts.push_back(std::move(acct));

    path_ = tmp_dir_ / "dztd_ctp.json";
    cfg.save(path_, "td");

    auto loaded = TdConfig::load(path_, "td");
    EXPECT_EQ(loaded.qry_account_interval_s, 10);
    EXPECT_FALSE(loaded.ext.enable_lock_mode);
    ASSERT_EQ(loaded.accounts.size(), 1u);
    EXPECT_EQ(loaded.accounts[0].account_id, "test_001");
    EXPECT_EQ(loaded.accounts[0].ext.broker_id, "8888");
    EXPECT_EQ(loaded.accounts[0].ext.password, "secret");
    EXPECT_EQ(loaded.accounts[0].ext.auth_code, "auth");
    EXPECT_EQ(loaded.accounts[0].ext.app_id, "app");
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

// --- to_safe_json 脱敏（契约 td-config §2.3：回报中 password 恒为 "****"）---

TEST_F(TdConfigTest, ToSafeJsonMasksPasswords) {
    TdConfig cfg;
    AccountConfig acct;
    acct.account_id = "a1";
    acct.ext.password = "super_secret";
    cfg.accounts.push_back(acct);

    auto j = cfg.to_safe_json();
    ASSERT_TRUE(j.contains("accounts"));
    ASSERT_EQ(j["accounts"].size(), 1u);
    EXPECT_EQ(j["accounts"][0]["ext"]["password"], "****");
}

TEST_F(TdConfigTest, ToSafeJsonDoesNotModifyOriginal) {
    TdConfig cfg;
    cfg.accounts.push_back({});
    cfg.accounts[0].ext.password = "original";
    auto j = cfg.to_safe_json();
    EXPECT_EQ(j["accounts"][0]["ext"]["password"], "****");
    // 原对象未被修改
    EXPECT_EQ(cfg.accounts[0].ext.password, "original");
}

TEST_F(TdConfigTest, ToSafeJsonKeepsTopLevelProcessExt) {
    TdConfig cfg;
    cfg.ext.enable_lock_mode = false;
    auto j = cfg.to_safe_json();
    ASSERT_TRUE(j.contains("ext"));
    EXPECT_FALSE(j["ext"]["enable_lock_mode"].get<bool>());
}

/// 契约 td-config §2.4：回报 payload 的全量结构（通用层 + ext 分层）。
/// 这是 dzweb 注册表 reconcile 与前端配置页解析的输入，形状漂移会让它们静默读空。
TEST_F(TdConfigTest, ToSafeJsonMatchesContractFullShape) {
    TdConfig cfg;
    cfg.qry_account_interval_s = 7;
    cfg.qry_position_interval_s = 30;
    cfg.qry_flush_interval_ms = 800;
    cfg.ext.enable_lock_mode = false;
    AccountConfig acct;
    acct.account_id = "account_001";
    acct.enabled = true;
    acct.risk_control_enabled = true;
    acct.currency_id = "CNY";
    acct.ext.broker_id = "9999";
    acct.ext.user_id = "00001";
    acct.ext.password = "123456";
    acct.ext.product_info = "prod";
    acct.ext.auth_code = "auth";
    acct.ext.app_id = "app";
    acct.ext.flow_dir = "";
    acct.ext.frontends.push_back({"tcp://127.0.0.1:1", "label", true});
    cfg.accounts.push_back(acct);

    const auto j = cfg.to_safe_json();

    // 契约 §2.4 顶层键
    EXPECT_EQ(j["qry_account_interval_s"], 7);
    EXPECT_EQ(j["qry_position_interval_s"], 30);
    EXPECT_EQ(j["qry_flush_interval_ms"], 800);
    ASSERT_TRUE(j["ext"].is_object());
    EXPECT_FALSE(j["ext"]["enable_lock_mode"].get<bool>());
    ASSERT_TRUE(j["accounts"].is_array());
    ASSERT_EQ(j["accounts"].size(), 1u);

    // 契约 §2.2 账户通用层
    const auto& a = j["accounts"][0];
    EXPECT_EQ(a["account_id"], "account_001");
    EXPECT_TRUE(a["enabled"].get<bool>());
    EXPECT_TRUE(a["risk_control_enabled"].get<bool>());
    EXPECT_EQ(a["currency_id"], "CNY");

    // 契约 §2.3 账户 ext：连接参数在 ext 内，且密码脱敏
    ASSERT_TRUE(a["ext"].is_object());
    EXPECT_EQ(a["ext"]["broker_id"], "9999");
    EXPECT_EQ(a["ext"]["user_id"], "00001");
    EXPECT_EQ(a["ext"]["password"], "****");
    EXPECT_EQ(a["ext"]["product_info"], "prod");
    EXPECT_EQ(a["ext"]["auth_code"], "auth");
    EXPECT_EQ(a["ext"]["app_id"], "app");
    ASSERT_EQ(a["ext"]["frontends"].size(), 1u);
    EXPECT_EQ(a["ext"]["frontends"][0]["address"], "tcp://127.0.0.1:1");

    // 旧的扁平位置不再出现（避免 dzweb 按旧形状读空）
    EXPECT_FALSE(a.contains("broker"));
    EXPECT_FALSE(a.contains("auth_code"));
    EXPECT_FALSE(a.contains("flow_dir"));
}

// --- validate ---

TEST_F(TdConfigTest, ValidatePassesForValidConfig) {
    TdConfig cfg;
    AccountConfig acct;
    acct.account_id = "a1";
    acct.ext.broker_id = "broker1";
    acct.ext.user_id = "user1";
    cfg.accounts.push_back(acct);
    EXPECT_EQ(validate(cfg), std::nullopt);
}

TEST_F(TdConfigTest, ValidateRejectsDuplicateAccountId) {
    TdConfig cfg;
    AccountConfig a1;
    a1.account_id = "dup";
    a1.ext.broker_id = "broker1";
    a1.ext.user_id = "user1";
    AccountConfig a2;
    a2.account_id = "dup";
    a2.ext.broker_id = "broker1";
    a2.ext.user_id = "user2";
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
    // 显式填好其余必填字段, 隔离校验顺序 (否则会先命中 ext.broker_id empty)
    acct.ext.broker_id = "broker1";
    acct.ext.user_id = "user1";
    cfg.accounts.push_back(acct);
    auto err = validate(cfg);
    ASSERT_TRUE(err.has_value());
    EXPECT_NE(err->find("account_id"), std::string::npos);
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
    acct.ext.broker_id = "";  // 空
    acct.ext.user_id = "user1";
    cfg.accounts.push_back(acct);
    auto err = validate(cfg);
    ASSERT_TRUE(err.has_value());
    EXPECT_NE(err->find("broker_id"), std::string::npos);
}

TEST_F(TdConfigTest, ValidateRejectsEmptyUserId) {
    TdConfig cfg;
    AccountConfig acct;
    acct.account_id = "acc1";
    acct.ext.broker_id = "broker1";
    acct.ext.user_id = "";  // 空
    cfg.accounts.push_back(acct);
    auto err = validate(cfg);
    ASSERT_TRUE(err.has_value());
    EXPECT_NE(err->find("user_id"), std::string::npos);
}

}  // namespace
}  // namespace dztrader::ctp
