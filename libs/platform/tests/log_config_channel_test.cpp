#include <dztrader/data_type.h>
#include <dztrader/platform/log_config.h>
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

class LogConfigChannelTest : public ::testing::Test {
protected:
    std::string channel_name_;
    std::filesystem::path shm_dir_;
    std::filesystem::path cfg_path_;
    std::shared_ptr<ChannelMeta> meta_;
    std::optional<MultiWriter> writer_;
    std::optional<Reader> reader_;

    static constexpr uint64_t MB = 1024 * 1024;

    void SetUp() override {
        // 唯一通道名，避免多次运行残留状态互相干扰
        channel_name_ =
            "dz_test_log_cfg_" + std::to_string(reinterpret_cast<uintptr_t>(this));
        shm_dir_ = std::filesystem::temp_directory_path() / channel_name_;
        std::filesystem::remove_all(shm_dir_);
        std::filesystem::create_directories(shm_dir_);

        cfg_path_ = shm_dir_ / "config.json";
        std::filesystem::remove(cfg_path_);

        // 构造 channel + writer + reader（参考 frame_writer_test.cpp SetUp）
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

    // 排空 reader 所有可用帧，返回最后一个 RTN_LOG_CONFIG 的 payload 解析结果。
    // load()/set_log_config() 都不写帧，只有 rtn_log_config() 写一帧 RTN_LOG_CONFIG。
    nlohmann::json read_last_rtn_log_config() {
        nlohmann::json last;
        while (auto* frame = reader_->next_frame()) {
            auto view = dztrader::shm::FrameView(frame);
            if (view.type() == DZ_FRAME_RTN_LOG_CONFIG) {
                const auto* data = reinterpret_cast<const char*>(view.ext_inst_payload());
                last = nlohmann::json::parse(data, data + view.ext_inst_payload_size());
            }
        }
        return last;
    }
};

// ===== load() 测试 =====

// 1. 文件不存在：load 后 cfg_ 为默认值，rtn 推送默认值
TEST_F(LogConfigChannelTest, LoadDefaultsWhenFileMissing) {
    EXPECT_FALSE(std::filesystem::exists(cfg_path_));

    dztrader::platform::LogConfig cfg("test_instance", cfg_path_);
    cfg.load();  // 不抛
    // current() 返回当前生效配置
    EXPECT_EQ(cfg.current().at("level"), "debug");
    EXPECT_EQ(cfg.current().at("flush_on"), "info");
    cfg.rtn_log_config(*writer_);
    auto rtn = read_last_rtn_log_config();
    EXPECT_EQ(rtn["level"], "debug");
    EXPECT_EQ(rtn["flush_on"], "info");
}

// 2. 文件有合法 log section：load 后 rtn 推送文件值（warn 规范化为 warning）
TEST_F(LogConfigChannelTest, LoadValidConfig) {
    write_cfg_file(R"({"log":{"level":"error","flush_on":"warn"}})");

    dztrader::platform::LogConfig cfg("test_instance", cfg_path_);
    cfg.load();

    cfg.rtn_log_config(*writer_);
    auto rtn = read_last_rtn_log_config();
    EXPECT_EQ(rtn["level"], "error");
    EXPECT_EQ(rtn["flush_on"], "warning");  // warn 规范化为 warning
}

// 3. 文件 JSON 损坏：load 不抛，自愈后文件被修复为默认值
TEST_F(LogConfigChannelTest, LoadCorruptFileHeals) {
    write_cfg_file("{not a valid json");
    EXPECT_FALSE(read_cfg_file().empty());

    dztrader::platform::LogConfig cfg("test_instance", cfg_path_);
    cfg.load();  // 不抛

    // 文件被修复为合法 JSON，且 log section 为默认值
    auto file_json = nlohmann::json::parse(read_cfg_file());
    EXPECT_EQ(file_json["log"]["level"], "debug");
    EXPECT_EQ(file_json["log"]["flush_on"], "info");

    cfg.rtn_log_config(*writer_);
    auto rtn = read_last_rtn_log_config();
    EXPECT_EQ(rtn["level"], "debug");
    EXPECT_EQ(rtn["flush_on"], "info");
}

// 4. 文件 level 为非法字符串：load 后用默认值
TEST_F(LogConfigChannelTest, LoadInvalidLevelFallsBackToDefault) {
    write_cfg_file(R"({"log":{"level":"foobar","flush_on":"info"}})");

    dztrader::platform::LogConfig cfg("test_instance", cfg_path_);
    cfg.load();

    cfg.rtn_log_config(*writer_);
    auto rtn = read_last_rtn_log_config();
    EXPECT_EQ(rtn["level"], "debug");
    EXPECT_EQ(rtn["flush_on"], "info");
}

// 5. 文件 level 为 number 类型：load 后用默认值
TEST_F(LogConfigChannelTest, LoadNonStringLevelFallsBackToDefault) {
    write_cfg_file(R"({"log":{"level":123,"flush_on":"info"}})");

    dztrader::platform::LogConfig cfg("test_instance", cfg_path_);
    cfg.load();

    cfg.rtn_log_config(*writer_);
    auto rtn = read_last_rtn_log_config();
    EXPECT_EQ(rtn["level"], "debug");
    EXPECT_EQ(rtn["flush_on"], "info");
}

// ===== set_log_config() 测试 =====

// 6. patch level=error 成功，cfg_ 更新，rtn 推送新值
TEST_F(LogConfigChannelTest, SetLogLevelSucceeds) {
    dztrader::platform::LogConfig cfg("test_instance", cfg_path_);
    cfg.load();

    nlohmann::json patch = {{"level", "error"}};
    cfg.set_log_config(patch);

    cfg.rtn_log_config(*writer_);
    auto rtn = read_last_rtn_log_config();
    EXPECT_EQ(rtn["level"], "error");
    EXPECT_EQ(rtn["flush_on"], "info");  // flush_on 不变
}

// 7. patch flush_on=warn 成功，规范化为 warning
TEST_F(LogConfigChannelTest, SetFlushOnSucceeds) {
    dztrader::platform::LogConfig cfg("test_instance", cfg_path_);
    cfg.load();

    nlohmann::json patch = {{"flush_on", "warn"}};
    cfg.set_log_config(patch);

    cfg.rtn_log_config(*writer_);
    auto rtn = read_last_rtn_log_config();
    EXPECT_EQ(rtn["level"], "debug");  // level 不变
    EXPECT_EQ(rtn["flush_on"], "warning");  // warn 规范化为 warning
}

// 8. 空 patch {} 不改变 cfg_，仍走完整流程
TEST_F(LogConfigChannelTest, SetEmptyPatchIsNoOp) {
    dztrader::platform::LogConfig cfg("test_instance", cfg_path_);
    cfg.load();

    nlohmann::json patch = nlohmann::json::object();
    cfg.set_log_config(patch);  // 不抛

    cfg.rtn_log_config(*writer_);
    auto rtn = read_last_rtn_log_config();
    EXPECT_EQ(rtn["level"], "debug");
    EXPECT_EQ(rtn["flush_on"], "info");
}

// 9. patch level=null 抛 std::runtime_error，cfg_ 不变
TEST_F(LogConfigChannelTest, SetNullFieldThrows) {
    dztrader::platform::LogConfig cfg("test_instance", cfg_path_);
    cfg.load();

    nlohmann::json patch = {{"level", nullptr}};
    EXPECT_THROW(cfg.set_log_config(patch), std::runtime_error);

    cfg.rtn_log_config(*writer_);
    auto rtn = read_last_rtn_log_config();
    EXPECT_EQ(rtn["level"], "debug");  // 仍是默认值
}

// 10. patch level=123(number) 抛 std::runtime_error，cfg_ 不变
TEST_F(LogConfigChannelTest, SetNonStringFieldThrows) {
    dztrader::platform::LogConfig cfg("test_instance", cfg_path_);
    cfg.load();

    nlohmann::json patch = {{"level", 123}};
    EXPECT_THROW(cfg.set_log_config(patch), std::runtime_error);

    cfg.rtn_log_config(*writer_);
    auto rtn = read_last_rtn_log_config();
    EXPECT_EQ(rtn["level"], "debug");
}

// 11. patch level="" 抛 std::runtime_error，cfg_ 不变
TEST_F(LogConfigChannelTest, SetEmptyStringThrows) {
    dztrader::platform::LogConfig cfg("test_instance", cfg_path_);
    cfg.load();

    nlohmann::json patch = {{"level", ""}};
    EXPECT_THROW(cfg.set_log_config(patch), std::runtime_error);

    cfg.rtn_log_config(*writer_);
    auto rtn = read_last_rtn_log_config();
    EXPECT_EQ(rtn["level"], "debug");
}

// 12. patch level="foobar" 抛异常，cfg_ 不变
TEST_F(LogConfigChannelTest, SetInvalidLevelThrows) {
    dztrader::platform::LogConfig cfg("test_instance", cfg_path_);
    cfg.load();

    nlohmann::json patch = {{"level", "foobar"}};
    EXPECT_THROW(cfg.set_log_config(patch), std::runtime_error);

    cfg.rtn_log_config(*writer_);
    auto rtn = read_last_rtn_log_config();
    EXPECT_EQ(rtn["level"], "debug");
}

// 13. patch 含 extra 字段：成功，extra 被忽略，cfg_ 不含 extra
TEST_F(LogConfigChannelTest, SetExtraFieldIgnored) {
    dztrader::platform::LogConfig cfg("test_instance", cfg_path_);
    cfg.load();

    nlohmann::json patch = {{"level", "info"}, {"extra", "foo"}};
    cfg.set_log_config(patch);

    cfg.rtn_log_config(*writer_);
    auto rtn = read_last_rtn_log_config();
    EXPECT_EQ(rtn["level"], "info");
    EXPECT_EQ(rtn["flush_on"], "info");
    EXPECT_FALSE(rtn.contains("extra"));
}

// 14. save 失败时 cfg_ 不变（强保证）：用不存在的父目录使 save 的 ofstream 失败
TEST_F(LogConfigChannelTest, SetFailureRollsBackCfg) {
    // cfg_path 指向不存在子目录，ofstream 必失败 → save 抛 → cfg_ 不变
    auto bad_path = shm_dir_ / "no_such_dir" / "config.json";
    dztrader::platform::LogConfig cfg("test_instance", bad_path);
    cfg.load();  // 文件缺失，不触发 save，cfg_ = 默认值

    nlohmann::json patch = {{"level", "error"}};
    EXPECT_THROW(cfg.set_log_config(patch), std::runtime_error);

    cfg.rtn_log_config(*writer_);
    auto rtn = read_last_rtn_log_config();
    EXPECT_EQ(rtn["level"], "debug");  // save 失败，cfg_ 回滚为默认值
    EXPECT_EQ(rtn["flush_on"], "info");
}

// ===== rtn_log_config() 测试 =====

// 15. rtn 推送的帧包含 level 和 flush_on，无 error 字段
TEST_F(LogConfigChannelTest, RtnSendsFullConfig) {
    write_cfg_file(R"({"log":{"level":"warn","flush_on":"error"}})");

    dztrader::platform::LogConfig cfg("test_instance", cfg_path_);
    cfg.load();

    cfg.rtn_log_config(*writer_);
    auto rtn = read_last_rtn_log_config();
    EXPECT_EQ(rtn["level"], "warning");  // warn 规范化为 warning
    EXPECT_EQ(rtn["flush_on"], "error");
    EXPECT_FALSE(rtn.contains("error"));  // 契约：RTN 无 error 字段
}

// 16. set 失败后 rtn 推送旧值
TEST_F(LogConfigChannelTest, RtnAfterFailedSetSendsOldValue) {
    write_cfg_file(R"({"log":{"level":"warn","flush_on":"error"}})");

    dztrader::platform::LogConfig cfg("test_instance", cfg_path_);
    cfg.load();  // cfg_ = {level:warning, flush_on:error}

    nlohmann::json patch = {{"level", "foobar"}};  // 非法级别
    EXPECT_THROW(cfg.set_log_config(patch), std::runtime_error);

    cfg.rtn_log_config(*writer_);
    auto rtn = read_last_rtn_log_config();
    EXPECT_EQ(rtn["level"], "warning");   // 旧值（规范化后），未被改写
    EXPECT_EQ(rtn["flush_on"], "error");
}

}  // namespace
