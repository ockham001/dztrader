#include "test_util.h"

#include <dztrader/core/core_data_type.h>
#include <dztrader/platform/auto_login.h>
#include <dztrader/shm/channel_meta.h>
#include <dztrader/shm/frame_codec.h>
#include <dztrader/shm/frame_view.h>
#include <dztrader/shm/reader.h>
#include <dztrader/shm/writer.h>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <string>

using dztrader::shm::ChannelConfig;
using dztrader::shm::ChannelMeta;
using dztrader::shm::MultiWriter;
using dztrader::shm::Reader;

namespace {

class AutoLoginConfigTest : public ::testing::Test {
protected:
    std::string channel_name_;
    std::filesystem::path shm_dir_;
    std::filesystem::path cfg_path_;
    std::shared_ptr<ChannelMeta> meta_;
    std::optional<MultiWriter> writer_;
    std::optional<Reader> reader_;

    static constexpr uint64_t MB = 1024 * 1024;

    void SetUp() override {
        channel_name_ = "dz_test_auto_login_" + std::to_string(reinterpret_cast<uintptr_t>(this));
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
        dztrader::shm::test::cleanup_channel(channel_name_, shm_dir_);
    }

    void write_cfg_file(const std::string& content) {
        std::ofstream ofs(cfg_path_, std::ios::binary | std::ios::trunc);
        ofs << content;
    }

    std::string read_cfg_file() {
        std::ifstream ifs(cfg_path_);
        return std::string(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
    }

    // 排空 reader，返回最后一个 RTN_AUTO_LOGIN 的 payload。
    nlohmann::json read_last_rtn() {
        nlohmann::json last;
        while (auto* frame = reader_->next_frame()) {
            auto view = dztrader::shm::FrameView(frame);
            if (view.type() == DZ_FRAME_RTN_AUTO_LOGIN) {
                const auto* data = reinterpret_cast<const char*>(view.ext_inst_payload());
                last = nlohmann::json::parse(data, data + view.ext_inst_payload_size());
            }
        }
        return last;
    }
};

// ===== is_hh_mm 测试 =====

TEST(AutoLoginIsHhMm, ValidTimes) {
    EXPECT_TRUE(dztrader::platform::AutoLoginConfig::is_hh_mm("00:00"));
    EXPECT_TRUE(dztrader::platform::AutoLoginConfig::is_hh_mm("23:59"));
    EXPECT_TRUE(dztrader::platform::AutoLoginConfig::is_hh_mm("08:45"));
}

TEST(AutoLoginIsHhMm, InvalidTimes) {
    EXPECT_FALSE(dztrader::platform::AutoLoginConfig::is_hh_mm("24:00"));   // hh 越界
    EXPECT_FALSE(dztrader::platform::AutoLoginConfig::is_hh_mm("23:60"));   // mm 越界
    EXPECT_FALSE(dztrader::platform::AutoLoginConfig::is_hh_mm("8:45"));    // 缺前导零
    EXPECT_FALSE(dztrader::platform::AutoLoginConfig::is_hh_mm("08:450"));  // 过长
    EXPECT_FALSE(dztrader::platform::AutoLoginConfig::is_hh_mm(""));        // 空
    EXPECT_FALSE(dztrader::platform::AutoLoginConfig::is_hh_mm("ab:cd"));   // 非数字
}

// ===== validate_schedule 测试 =====

TEST(AutoLoginValidateSchedule, ValidSchedule) {
    nlohmann::json s = {{"login_time", "08:45"}, {"logout_time", "15:30"}};
    EXPECT_FALSE(dztrader::platform::AutoLoginConfig::validate_schedule(s).has_value());
}

TEST(AutoLoginValidateSchedule, CrossMidnightValid) {
    nlohmann::json s = {{"login_time", "23:00"}, {"logout_time", "01:00"}};
    EXPECT_FALSE(dztrader::platform::AutoLoginConfig::validate_schedule(s).has_value());
}

TEST(AutoLoginValidateSchedule, LoginEqualsLogoutRejected) {
    nlohmann::json s = {{"login_time", "08:00"}, {"logout_time", "08:00"}};
    EXPECT_TRUE(dztrader::platform::AutoLoginConfig::validate_schedule(s).has_value());
}

TEST(AutoLoginValidateSchedule, NonObjectRejected) {
    nlohmann::json s = "notanobject";
    EXPECT_TRUE(dztrader::platform::AutoLoginConfig::validate_schedule(s).has_value());
}

TEST(AutoLoginValidateSchedule, BadFormatRejected) {
    nlohmann::json s = {{"login_time", "8:45"}, {"logout_time", "15:30"}};
    EXPECT_TRUE(dztrader::platform::AutoLoginConfig::validate_schedule(s).has_value());
}

// ===== validate（全量）测试 =====

TEST(AutoLoginValidate, FullValid) {
    nlohmann::json cfg = {{"enabled", true},
                          {"schedules", {{{"login_time", "08:45"}, {"logout_time", "15:30"}}}}};
    EXPECT_FALSE(dztrader::platform::AutoLoginConfig::validate(cfg).has_value());
}

TEST(AutoLoginValidate, MissingEnabledRejected) {
    nlohmann::json cfg = {{"schedules", nlohmann::json::array()}};
    EXPECT_TRUE(dztrader::platform::AutoLoginConfig::validate(cfg).has_value());
}

TEST(AutoLoginValidate, MissingSchedulesRejected) {
    nlohmann::json cfg = {{"enabled", true}};
    EXPECT_TRUE(dztrader::platform::AutoLoginConfig::validate(cfg).has_value());
}

TEST(AutoLoginValidate, NullEnabledRejected) {
    nlohmann::json cfg = {{"enabled", nullptr}, {"schedules", nlohmann::json::array()}};
    EXPECT_TRUE(dztrader::platform::AutoLoginConfig::validate(cfg).has_value());
}

TEST(AutoLoginValidate, NonObjectRejected) {
    nlohmann::json cfg = 42;
    EXPECT_TRUE(dztrader::platform::AutoLoginConfig::validate(cfg).has_value());
}

// ===== load() 测试 =====

// 1. 文件不存在：load 后默认值
TEST_F(AutoLoginConfigTest, LoadDefaultsWhenFileMissing) {
    EXPECT_FALSE(std::filesystem::exists(cfg_path_));

    dztrader::platform::AutoLoginConfig cfg("test_instance", cfg_path_, *writer_);
    cfg.load();

    cfg.rtn_auto_login();
    auto rtn = read_last_rtn();
    EXPECT_EQ(rtn["enabled"], true);
    EXPECT_TRUE(rtn["schedules"].is_array() && rtn["schedules"].empty());
}

// 2. 文件有合法 auto_login section
TEST_F(AutoLoginConfigTest, LoadValidConfig) {
    write_cfg_file(R"({"auto_login":{"enabled":false,"schedules":[)"
                   R"({"login_time":"08:45","logout_time":"15:30"}]}})");

    dztrader::platform::AutoLoginConfig cfg("test_instance", cfg_path_, *writer_);
    cfg.load();

    cfg.rtn_auto_login();
    auto rtn = read_last_rtn();
    EXPECT_EQ(rtn["enabled"], false);
    EXPECT_EQ(rtn["schedules"][0]["login_time"], "08:45");
    EXPECT_EQ(rtn["schedules"][0]["logout_time"], "15:30");
}

// 3. 文件 JSON 损坏：自愈为默认值
TEST_F(AutoLoginConfigTest, LoadCorruptFileHeals) {
    write_cfg_file("{not a valid json");

    dztrader::platform::AutoLoginConfig cfg("test_instance", cfg_path_, *writer_);
    cfg.load();

    auto file_json = nlohmann::json::parse(read_cfg_file());
    EXPECT_EQ(file_json["auto_login"]["enabled"], true);
    EXPECT_TRUE(file_json["auto_login"]["schedules"].empty());

    cfg.rtn_auto_login();
    auto rtn = read_last_rtn();
    EXPECT_EQ(rtn["enabled"], true);
}

// 4. 文件 enabled 为非 bool 类型：自愈默认
TEST_F(AutoLoginConfigTest, LoadNonBoolEnabledHeals) {
    write_cfg_file(R"({"auto_login":{"enabled":"yes","schedules":[]}})");

    dztrader::platform::AutoLoginConfig cfg("test_instance", cfg_path_, *writer_);
    cfg.load();

    cfg.rtn_auto_login();
    auto rtn = read_last_rtn();
    EXPECT_EQ(rtn["enabled"], true);  // 回退默认
}

// 5. 文件 schedules 含非法元素（login==logout）：自愈默认
TEST_F(AutoLoginConfigTest, LoadInvalidScheduleHeals) {
    write_cfg_file(R"({"auto_login":{"enabled":true,"schedules":[)"
                   R"({"login_time":"08:00","logout_time":"08:00"}]}})");

    dztrader::platform::AutoLoginConfig cfg("test_instance", cfg_path_, *writer_);
    cfg.load();

    cfg.rtn_auto_login();
    auto rtn = read_last_rtn();
    // 未注入内置默认表（平台层情形）：回退默认空数组；归一见 #392 的注入用例
    EXPECT_TRUE(rtn["schedules"].empty());
}

// 6. 文件缺 schedules 字段：补默认空数组，不降级
TEST_F(AutoLoginConfigTest, LoadMissingSchedulesUsesDefault) {
    write_cfg_file(R"({"auto_login":{"enabled":false}})");

    dztrader::platform::AutoLoginConfig cfg("test_instance", cfg_path_, *writer_);
    cfg.load();

    cfg.rtn_auto_login();
    auto rtn = read_last_rtn();
    EXPECT_EQ(rtn["enabled"], false);       // 文件值保留
    EXPECT_TRUE(rtn["schedules"].empty());  // 默认补齐
}

// ===== set_auto_login() 测试 =====

// 7. patch enabled=false 成功
TEST_F(AutoLoginConfigTest, SetEnabledSucceeds) {
    dztrader::platform::AutoLoginConfig cfg("test_instance", cfg_path_, *writer_);
    cfg.load();

    cfg.set_auto_login({{"enabled", false}});

    cfg.rtn_auto_login();
    auto rtn = read_last_rtn();
    EXPECT_EQ(rtn["enabled"], false);
    EXPECT_TRUE(rtn["schedules"].empty());  // 不变
}

// 8. patch schedules 整体覆盖
TEST_F(AutoLoginConfigTest, SetSchedulesWholesaleReplace) {
    write_cfg_file(R"({"auto_login":{"enabled":true,"schedules":[)"
                   R"({"login_time":"08:45","logout_time":"15:30"}]}})");
    dztrader::platform::AutoLoginConfig cfg("test_instance", cfg_path_, *writer_);
    cfg.load();

    // 整体替换为两条新排程
    cfg.set_auto_login({{"schedules", nlohmann::json::array({
                                          {{"login_time", "20:45"}, {"logout_time", "02:30"}},
                                          {{"login_time", "09:00"}, {"logout_time", "11:30"}},
                                      })}});

    cfg.rtn_auto_login();
    auto rtn = read_last_rtn();
    EXPECT_EQ(rtn["schedules"].size(), 2);
    EXPECT_EQ(rtn["schedules"][0]["login_time"], "20:45");
    EXPECT_EQ(rtn["schedules"][1]["login_time"], "09:00");
}

// 9. patch schedules=[]：平台层无内置默认表时空数组保持为空（网关层归一见 #392）
TEST_F(AutoLoginConfigTest, SetEmptySchedulesKeepsEmptyWithoutInjectedDefault) {
    write_cfg_file(R"({"auto_login":{"enabled":true,"schedules":[)"
                   R"({"login_time":"08:45","logout_time":"15:30"}]}})");
    dztrader::platform::AutoLoginConfig cfg("test_instance", cfg_path_, *writer_);
    cfg.load();

    cfg.set_auto_login({{"schedules", nlohmann::json::array()}});

    cfg.rtn_auto_login();
    auto rtn = read_last_rtn();
    EXPECT_TRUE(rtn["schedules"].empty());
}

// 10. 空 patch {} 无操作
TEST_F(AutoLoginConfigTest, SetEmptyPatchIsNoOp) {
    dztrader::platform::AutoLoginConfig cfg("test_instance", cfg_path_, *writer_);
    cfg.load();

    cfg.set_auto_login(nlohmann::json::object());

    cfg.rtn_auto_login();
    auto rtn = read_last_rtn();
    EXPECT_EQ(rtn["enabled"], true);
    EXPECT_TRUE(rtn["schedules"].empty());
}

// 11. patch enabled=null 抛异常，cfg_ 不变
TEST_F(AutoLoginConfigTest, SetNullEnabledThrows) {
    dztrader::platform::AutoLoginConfig cfg("test_instance", cfg_path_, *writer_);
    cfg.load();

    EXPECT_THROW(cfg.set_auto_login({{"enabled", nullptr}}), std::runtime_error);

    cfg.rtn_auto_login();
    auto rtn = read_last_rtn();
    EXPECT_EQ(rtn["enabled"], true);  // 未变
}

// 12. patch schedules=null 抛异常
TEST_F(AutoLoginConfigTest, SetNullSchedulesThrows) {
    dztrader::platform::AutoLoginConfig cfg("test_instance", cfg_path_, *writer_);
    cfg.load();

    EXPECT_THROW(cfg.set_auto_login({{"schedules", nullptr}}), std::runtime_error);
}

// 13. patch enabled=123(非 bool) 抛异常
TEST_F(AutoLoginConfigTest, SetNonBoolEnabledThrows) {
    dztrader::platform::AutoLoginConfig cfg("test_instance", cfg_path_, *writer_);
    cfg.load();

    EXPECT_THROW(cfg.set_auto_login({{"enabled", 123}}), std::runtime_error);
}

// 14. patch schedules="notarray" 抛异常
TEST_F(AutoLoginConfigTest, SetNonArraySchedulesThrows) {
    dztrader::platform::AutoLoginConfig cfg("test_instance", cfg_path_, *writer_);
    cfg.load();

    EXPECT_THROW(cfg.set_auto_login({{"schedules", "notarray"}}), std::runtime_error);
}

// 15. patch schedules 含 login==logout 抛异常
TEST_F(AutoLoginConfigTest, SetScheduleLoginEqualsLogoutThrows) {
    dztrader::platform::AutoLoginConfig cfg("test_instance", cfg_path_, *writer_);
    cfg.load();

    EXPECT_THROW(
        cfg.set_auto_login({{"schedules", nlohmann::json::array({
                                              {{"login_time", "08:00"}, {"logout_time", "08:00"}},
                                          })}}),
        std::runtime_error);
}

// 15a. patch schedule 缺 login_time 抛异常（契约：缺失=校验失败）
TEST_F(AutoLoginConfigTest, SetScheduleMissingLoginTimeThrows) {
    dztrader::platform::AutoLoginConfig cfg("test_instance", cfg_path_, *writer_);
    cfg.load();

    EXPECT_THROW(cfg.set_auto_login({{"schedules", nlohmann::json::array({
                                                       {{"logout_time", "15:30"}},
                                                   })}}),
                 std::runtime_error);
}

// 15b. patch schedule 缺 logout_time 抛异常（契约：缺失=校验失败）
TEST_F(AutoLoginConfigTest, SetScheduleMissingLogoutTimeThrows) {
    dztrader::platform::AutoLoginConfig cfg("test_instance", cfg_path_, *writer_);
    cfg.load();

    EXPECT_THROW(cfg.set_auto_login({{"schedules", nlohmann::json::array({
                                                       {{"login_time", "08:45"}},
                                                   })}}),
                 std::runtime_error);
}

// 15c. patch schedule 时间非字符串/null 抛异常（契约：非字符串/null=校验失败）
TEST_F(AutoLoginConfigTest, SetScheduleNonStringTimeThrows) {
    dztrader::platform::AutoLoginConfig cfg("test_instance", cfg_path_, *writer_);
    cfg.load();

    EXPECT_THROW(
        cfg.set_auto_login({{"schedules", nlohmann::json::array({
                                              {{"login_time", nullptr}, {"logout_time", "15:30"}},
                                          })}}),
        std::runtime_error);
    EXPECT_THROW(
        cfg.set_auto_login({{"schedules", nlohmann::json::array({
                                              {{"login_time", "08:45"}, {"logout_time", 1530}},
                                          })}}),
        std::runtime_error);
}

// 16. patch 含未知字段：忽略，cfg_ 不含未知字段
TEST_F(AutoLoginConfigTest, SetUnknownFieldIgnored) {
    dztrader::platform::AutoLoginConfig cfg("test_instance", cfg_path_, *writer_);
    cfg.load();

    cfg.set_auto_login({{"enabled", false}, {"unknown", "foo"}});

    cfg.rtn_auto_login();
    auto rtn = read_last_rtn();
    EXPECT_EQ(rtn["enabled"], false);
    EXPECT_FALSE(rtn.contains("unknown"));
}

// 17. patch 非 object 抛异常
TEST_F(AutoLoginConfigTest, SetNonObjectPatchThrows) {
    dztrader::platform::AutoLoginConfig cfg("test_instance", cfg_path_, *writer_);
    cfg.load();

    EXPECT_THROW(cfg.set_auto_login(nlohmann::json(42)), std::runtime_error);
    EXPECT_THROW(cfg.set_auto_login(nlohmann::json("string")), std::runtime_error);
    EXPECT_THROW(cfg.set_auto_login(nlohmann::json::array()), std::runtime_error);
}

// 18. save 失败时 cfg_ 不变（强保证）
TEST_F(AutoLoginConfigTest, SetFailureRollsBackCfg) {
    auto bad_path = shm_dir_ / "no_such_dir" / "config.json";
    dztrader::platform::AutoLoginConfig cfg("test_instance", bad_path, *writer_);
    cfg.load();  // 文件缺失，cfg_ = 默认值

    EXPECT_THROW(cfg.set_auto_login({{"enabled", false}}), std::runtime_error);

    cfg.rtn_auto_login();
    auto rtn = read_last_rtn();
    EXPECT_EQ(rtn["enabled"], true);  // 回滚默认
}

// ===== rtn_auto_login() 测试 =====

// 19. rtn 始终全量，无 error 字段
TEST_F(AutoLoginConfigTest, RtnSendsFullConfigNoError) {
    write_cfg_file(R"({"auto_login":{"enabled":false,"schedules":[)"
                   R"({"login_time":"08:45","logout_time":"15:30"}]}})");
    dztrader::platform::AutoLoginConfig cfg("test_instance", cfg_path_, *writer_);
    cfg.load();

    cfg.rtn_auto_login();
    auto rtn = read_last_rtn();
    EXPECT_EQ(rtn["enabled"], false);
    EXPECT_EQ(rtn["schedules"].size(), 1);
    EXPECT_FALSE(rtn.contains("error"));
}

// 20. set 失败后 rtn 推送旧值
TEST_F(AutoLoginConfigTest, RtnAfterFailedSetSendsOldValue) {
    write_cfg_file(R"({"auto_login":{"enabled":false,"schedules":[)"
                   R"({"login_time":"08:45","logout_time":"15:30"}]}})");
    dztrader::platform::AutoLoginConfig cfg("test_instance", cfg_path_, *writer_);
    cfg.load();

    EXPECT_THROW(cfg.set_auto_login({{"enabled", nullptr}}), std::runtime_error);

    cfg.rtn_auto_login();
    auto rtn = read_last_rtn();
    EXPECT_EQ(rtn["enabled"], false);  // 旧值
    EXPECT_EQ(rtn["schedules"].size(), 1);
}

// 21. set 成功后持久化到文件
TEST_F(AutoLoginConfigTest, SetPersistsToFile) {
    dztrader::platform::AutoLoginConfig cfg("test_instance", cfg_path_, *writer_);
    cfg.load();

    cfg.set_auto_login({{"enabled", false},
                        {"schedules", nlohmann::json::array({
                                          {{"login_time", "20:45"}, {"logout_time", "02:30"}},
                                      })}});

    auto file_json = nlohmann::json::parse(read_cfg_file());
    EXPECT_EQ(file_json["auto_login"]["enabled"], false);
    EXPECT_EQ(file_json["auto_login"]["schedules"][0]["login_time"], "20:45");
}

// ===== 排程空表归一为默认排程（#392）=====

// 网关注入的内置默认排程（非空），空数组回落到它
nlohmann::json make_default_schedules() {
    return nlohmann::json::array({
        {{"login_time", "08:45"}, {"logout_time", "15:30"}},
        {{"login_time", "20:45"}, {"logout_time", "02:30"}},
    });
}

// 22. 文件里 schedules=[]：加载后 RTN 与文件都是默认排程（空数组=未配置，不是清空）
TEST_F(AutoLoginConfigTest, LoadEmptySchedulesNormalizesToDefault) {
    write_cfg_file(R"({"auto_login":{"enabled":false,"schedules":[]}})");
    const auto defaults = make_default_schedules();

    dztrader::platform::AutoLoginConfig cfg("test_instance", cfg_path_, *writer_,
                                            nlohmann::json::json_pointer("/auto_login"), defaults);
    cfg.load();

    cfg.rtn_auto_login();
    auto rtn = read_last_rtn();
    EXPECT_EQ(rtn["schedules"], defaults);
    EXPECT_EQ(rtn["enabled"], false);  // 只归一排程，enabled 保留文件值

    auto file_json = nlohmann::json::parse(read_cfg_file());
    EXPECT_EQ(file_json["auto_login"]["schedules"], defaults);
}

// 23. 文件里没有 auto_login 段：加载后 RTN 与文件都是默认排程，其他段保留
TEST_F(AutoLoginConfigTest, LoadMissingSectionMaterializesDefault) {
    write_cfg_file(R"({"other":{"x":1}})");
    const auto defaults = make_default_schedules();

    dztrader::platform::AutoLoginConfig cfg("test_instance", cfg_path_, *writer_,
                                            nlohmann::json::json_pointer("/auto_login"), defaults);
    cfg.load();

    cfg.rtn_auto_login();
    auto rtn = read_last_rtn();
    EXPECT_EQ(rtn["schedules"], defaults);
    EXPECT_EQ(rtn["enabled"], true);  // 段缺失：enabled 用默认 true

    auto file_json = nlohmann::json::parse(read_cfg_file());
    EXPECT_EQ(file_json["auto_login"]["schedules"], defaults);
    EXPECT_EQ(file_json["other"]["x"], 1);  // 材料化不丢其他段
}

// 24. 文件不存在：加载后 RTN 与文件都是默认排程
TEST_F(AutoLoginConfigTest, LoadMissingFileMaterializesDefault) {
    EXPECT_FALSE(std::filesystem::exists(cfg_path_));
    const auto defaults = make_default_schedules();

    dztrader::platform::AutoLoginConfig cfg("test_instance", cfg_path_, *writer_,
                                            nlohmann::json::json_pointer("/auto_login"), defaults);
    cfg.load();

    EXPECT_TRUE(std::filesystem::exists(cfg_path_));

    cfg.rtn_auto_login();
    auto rtn = read_last_rtn();
    EXPECT_EQ(rtn["schedules"], defaults);

    auto file_json = nlohmann::json::parse(read_cfg_file());
    EXPECT_EQ(file_json["auto_login"]["schedules"], defaults);
}

// 25. 运行期提交空数组：RTN 与文件都回落到默认排程
TEST_F(AutoLoginConfigTest, SetEmptySchedulesFallsBackToDefault) {
    write_cfg_file(R"({"auto_login":{"enabled":true,"schedules":[)"
                   R"({"login_time":"10:00","logout_time":"11:00"}]}})");
    const auto defaults = make_default_schedules();

    dztrader::platform::AutoLoginConfig cfg("test_instance", cfg_path_, *writer_,
                                            nlohmann::json::json_pointer("/auto_login"), defaults);
    cfg.load();

    cfg.set_auto_login({{"schedules", nlohmann::json::array()}});

    cfg.rtn_auto_login();
    auto rtn = read_last_rtn();
    EXPECT_EQ(rtn["schedules"], defaults);

    auto file_json = nlohmann::json::parse(read_cfg_file());
    EXPECT_EQ(file_json["auto_login"]["schedules"], defaults);
}

// 26. 幂等：当前值已等于默认表时再提交空数组：值不变、文件内容不变（不落盘）
TEST_F(AutoLoginConfigTest, SetEmptySchedulesWhenAlreadyDefaultKeepsFile) {
    const auto defaults = make_default_schedules();
    write_cfg_file(
        nlohmann::json({{"auto_login", {{"enabled", true}, {"schedules", defaults}}}}).dump());
    dztrader::platform::AutoLoginConfig cfg("test_instance", cfg_path_, *writer_,
                                            nlohmann::json::json_pointer("/auto_login"), defaults);
    cfg.load();

    const auto file_before = read_cfg_file();

    cfg.set_auto_login({{"schedules", nlohmann::json::array()}});

    EXPECT_EQ(read_cfg_file(), file_before);  // 幂等：无磁盘写入
    cfg.rtn_auto_login();
    auto rtn = read_last_rtn();
    EXPECT_EQ(rtn["schedules"], defaults);
}

// 27. 文件里已有非空合法排程：加载不覆盖文件（用户配置优先）
TEST_F(AutoLoginConfigTest, LoadKeepsUserSchedulesWithoutRewrite) {
    const std::string user_cfg = R"({"auto_login":{"enabled":true,"schedules":[)"
                                 R"({"login_time":"10:00","logout_time":"11:00"}]}})";
    write_cfg_file(user_cfg);
    const auto defaults = make_default_schedules();

    dztrader::platform::AutoLoginConfig cfg("test_instance", cfg_path_, *writer_,
                                            nlohmann::json::json_pointer("/auto_login"), defaults);
    cfg.load();

    EXPECT_EQ(read_cfg_file(), user_cfg);  // 未写回

    cfg.rtn_auto_login();
    auto rtn = read_last_rtn();
    ASSERT_EQ(rtn["schedules"].size(), 1);
    EXPECT_EQ(rtn["schedules"][0]["login_time"], "10:00");
}

// 28. 空默认（不传第 5 参数）：文件不存在时不材料化，空数组仍是空数组
TEST_F(AutoLoginConfigTest, EmptyDefaultSchedulesDoesNotMaterializeFile) {
    EXPECT_FALSE(std::filesystem::exists(cfg_path_));

    dztrader::platform::AutoLoginConfig cfg("test_instance", cfg_path_, *writer_);
    cfg.load();

    EXPECT_FALSE(std::filesystem::exists(cfg_path_));  // 平台层：空默认不写文件

    cfg.rtn_auto_login();
    auto rtn = read_last_rtn();
    EXPECT_TRUE(rtn["schedules"].is_array() && rtn["schedules"].empty());
}

// 29. 空默认：文件里 schedules=[] 保持空数组且不改写文件
TEST_F(AutoLoginConfigTest, EmptyDefaultSchedulesKeepsEmptyInFile) {
    const std::string content = R"({"auto_login":{"enabled":true,"schedules":[]}})";
    write_cfg_file(content);

    dztrader::platform::AutoLoginConfig cfg("test_instance", cfg_path_, *writer_);
    cfg.load();

    EXPECT_EQ(read_cfg_file(), content);  // 归一 no-op：不写回

    cfg.rtn_auto_login();
    auto rtn = read_last_rtn();
    EXPECT_TRUE(rtn["schedules"].is_array() && rtn["schedules"].empty());
}

// 30. 非空默认 + 文件排程非法（login==logout）：降级为默认表并材料化
TEST_F(AutoLoginConfigTest, LoadInvalidScheduleFallsBackToDefaultSchedules) {
    write_cfg_file(R"({"auto_login":{"enabled":true,"schedules":[)"
                   R"({"login_time":"08:00","logout_time":"08:00"}]}})");
    const auto defaults = make_default_schedules();

    dztrader::platform::AutoLoginConfig cfg("test_instance", cfg_path_, *writer_,
                                            nlohmann::json::json_pointer("/auto_login"), defaults);
    cfg.load();

    cfg.rtn_auto_login();
    auto rtn = read_last_rtn();
    EXPECT_EQ(rtn["schedules"], defaults);

    auto file_json = nlohmann::json::parse(read_cfg_file());
    EXPECT_EQ(file_json["auto_login"]["schedules"], defaults);
}

// 31. 非空默认 + 文件 JSON 损坏：降级为默认表并材料化
TEST_F(AutoLoginConfigTest, LoadCorruptFileFallsBackToDefaultSchedules) {
    write_cfg_file("{not a valid json");
    const auto defaults = make_default_schedules();

    dztrader::platform::AutoLoginConfig cfg("test_instance", cfg_path_, *writer_,
                                            nlohmann::json::json_pointer("/auto_login"), defaults);
    cfg.load();

    cfg.rtn_auto_login();
    auto rtn = read_last_rtn();
    EXPECT_EQ(rtn["schedules"], defaults);

    auto file_json = nlohmann::json::parse(read_cfg_file());
    EXPECT_EQ(file_json["auto_login"]["schedules"], defaults);
}

// 32. 非空默认 + enabled 非 bool：enabled 回 true，排程为默认表
TEST_F(AutoLoginConfigTest, LoadNonBoolEnabledKeepsDefaultSchedules) {
    write_cfg_file(R"({"auto_login":{"enabled":"yes","schedules":[]}})");
    const auto defaults = make_default_schedules();

    dztrader::platform::AutoLoginConfig cfg("test_instance", cfg_path_, *writer_,
                                            nlohmann::json::json_pointer("/auto_login"), defaults);
    cfg.load();

    cfg.rtn_auto_login();
    auto rtn = read_last_rtn();
    EXPECT_EQ(rtn["enabled"], true);  // 降级默认
    EXPECT_EQ(rtn["schedules"], defaults);

    auto file_json = nlohmann::json::parse(read_cfg_file());
    EXPECT_EQ(file_json["auto_login"]["enabled"], true);
    EXPECT_EQ(file_json["auto_login"]["schedules"], defaults);
}

// 33. 非空默认 + section 存在但 schedules 键缺失：排程为默认表并材料化，enabled 保留
TEST_F(AutoLoginConfigTest, LoadMissingSchedulesKeyMaterializesDefault) {
    write_cfg_file(R"({"auto_login":{"enabled":false}})");
    const auto defaults = make_default_schedules();

    dztrader::platform::AutoLoginConfig cfg("test_instance", cfg_path_, *writer_,
                                            nlohmann::json::json_pointer("/auto_login"), defaults);
    cfg.load();

    cfg.rtn_auto_login();
    auto rtn = read_last_rtn();
    EXPECT_EQ(rtn["enabled"], false);  // 文件值保留
    EXPECT_EQ(rtn["schedules"], defaults);

    auto file_json = nlohmann::json::parse(read_cfg_file());
    EXPECT_EQ(file_json["auto_login"]["enabled"], false);
    EXPECT_EQ(file_json["auto_login"]["schedules"], defaults);
}

// 34. 注入非法默认表（元素 login==logout）：退化为空数组，不写文件、不产生非法配置
TEST_F(AutoLoginConfigTest, InvalidInjectedDefaultDegradesToEmpty) {
    const nlohmann::json bad_defaults = nlohmann::json::array({
        {{"login_time", "08:00"}, {"logout_time", "08:00"}},
    });

    dztrader::platform::AutoLoginConfig cfg("test_instance", cfg_path_, *writer_,
                                            nlohmann::json::json_pointer("/auto_login"),
                                            bad_defaults);
    cfg.load();

    EXPECT_FALSE(std::filesystem::exists(cfg_path_));  // 退化后归一 no-op：不材料化

    cfg.rtn_auto_login();
    auto rtn = read_last_rtn();
    EXPECT_TRUE(rtn["schedules"].is_array() && rtn["schedules"].empty());
    EXPECT_FALSE(dztrader::platform::AutoLoginConfig::validate(rtn).has_value());
}

// 35. 注入非数组默认表（花括号列表构造出 object）：同样退化为空数组
TEST_F(AutoLoginConfigTest, NonArrayInjectedDefaultDegradesToEmpty) {
    // {{"login_time","08:45"}} 构造出的是 object 而非 array
    const nlohmann::json bad_defaults = {{"login_time", "08:45"}};
    ASSERT_FALSE(bad_defaults.is_array());

    dztrader::platform::AutoLoginConfig cfg("test_instance", cfg_path_, *writer_,
                                            nlohmann::json::json_pointer("/auto_login"),
                                            bad_defaults);
    cfg.load();

    EXPECT_FALSE(std::filesystem::exists(cfg_path_));  // 退化后归一 no-op：不材料化

    cfg.rtn_auto_login();
    auto rtn = read_last_rtn();
    EXPECT_TRUE(rtn["schedules"].is_array() && rtn["schedules"].empty());
}

}  // namespace
