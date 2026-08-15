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
        channel_name_ =
            "dz_test_auto_login_" + std::to_string(reinterpret_cast<uintptr_t>(this));
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
    EXPECT_FALSE(dztrader::platform::AutoLoginConfig::is_hh_mm("24:00"));  // hh 越界
    EXPECT_FALSE(dztrader::platform::AutoLoginConfig::is_hh_mm("23:60"));  // mm 越界
    EXPECT_FALSE(dztrader::platform::AutoLoginConfig::is_hh_mm("8:45"));   // 缺前导零
    EXPECT_FALSE(dztrader::platform::AutoLoginConfig::is_hh_mm("08:450")); // 过长
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
    EXPECT_TRUE(rtn["schedules"].empty());  // 回退默认空数组
}

// 6. 文件缺 schedules 字段：补默认空数组，不降级
TEST_F(AutoLoginConfigTest, LoadMissingSchedulesUsesDefault) {
    write_cfg_file(R"({"auto_login":{"enabled":false}})");

    dztrader::platform::AutoLoginConfig cfg("test_instance", cfg_path_, *writer_);
    cfg.load();

    cfg.rtn_auto_login();
    auto rtn = read_last_rtn();
    EXPECT_EQ(rtn["enabled"], false);           // 文件值保留
    EXPECT_TRUE(rtn["schedules"].empty());      // 默认补齐
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

// 9. patch schedules=[] 清空排程
TEST_F(AutoLoginConfigTest, SetEmptySchedulesClears) {
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

    EXPECT_THROW(cfg.set_auto_login({{"schedules", nlohmann::json::array({
                                       {{"login_time", "08:00"}, {"logout_time", "08:00"}},
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

    cfg.set_auto_login({{"enabled", false}, {"schedules", nlohmann::json::array({
                                                  {{"login_time", "20:45"}, {"logout_time", "02:30"}},
                                              })}});

    auto file_json = nlohmann::json::parse(read_cfg_file());
    EXPECT_EQ(file_json["auto_login"]["enabled"], false);
    EXPECT_EQ(file_json["auto_login"]["schedules"][0]["login_time"], "20:45");
}

}  // namespace
