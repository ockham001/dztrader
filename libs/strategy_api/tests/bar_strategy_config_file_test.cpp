/**
 * @file bar_strategy_config_file_test.cpp
 * @brief 配置文件面：原子写 / 读取 / 缺失默认生成 / ConfigStore 写回（issue #156，
 *        组件文档 §7 / §8）
 *
 * 断言只落在可观察契约上：磁盘文件内容（重新读回）、返回状态 / message / 异常类型与
 * 消息字段、`ConfigStore::current()`（内存镜像）。IO 失败用例不依赖权限（root 下
 * chmod 无效），统一用「tmp 位置被目录占住」「父目录不存在」制造失败。
 *
 * 确定性：临时目录用 `dztrader::test::unique_temp_dir_tracked()`（进程唯一，ctest -j 下
 * 每个用例是独立进程）；不依赖墙钟——`warmup.from` 用 2020-01-02 这类过去日期，配置层
 * 默认的「今天」必然晚于它。
 */
#include <gtest/gtest.h>

#include <dztrader/bar_strategy/config.h>
#include <dztrader/bar_strategy/config_file.h>
#include <dztrader/json.h>

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "test_temp_dir.h"

namespace {

namespace fs = std::filesystem;

using dztrader::BarStrategyConfig;
using dztrader::ConfigLoadResult;
using dztrader::ConfigLoadStatus;
using dztrader::ConfigStore;
using dztrader::ConfigValueSource;
using dztrader::EffectiveConfig;
using dztrader::ScaleRounding;

// ============================================================
// 通用辅助
// ============================================================

/// 读整个文件为字符串（不经过被测 read_json_file，避免同义反复）
std::string read_text(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    EXPECT_TRUE(stream.is_open()) << "cannot open " << path;
    return std::string{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

/// 直接写文本文件（构造夹具前提 / 伪造坏 JSON，不经过被测实现）
void write_text(const fs::path& path, std::string_view text) {
    std::error_code ec;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(stream.is_open()) << "cannot open " << path;
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    ASSERT_TRUE(stream.good()) << "cannot write " << path;
}

/// 子串断言辅助（异常 / 提示消息里的字段名与 path）
bool contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

/// 原子写的临时文件路径（`<path>.tmp`）
fs::path temp_path_of(const fs::path& path) {
    fs::path temp = path;
    temp += ".tmp";
    return temp;
}

/// 目录里的普通文件列表（断言原子写不留下 tmp 等额外文件）
std::vector<fs::path> regular_files(const fs::path& directory) {
    std::vector<fs::path> files;
    for (const fs::directory_entry& entry : fs::directory_iterator(directory)) {
        if (entry.is_regular_file()) {
            files.push_back(entry.path());
        }
    }
    return files;
}

/// 断言 callable 抛 Exception 并返回 what()；没抛 / 类型不符都记失败
template <typename Exception, typename Callable>
std::string capture_exception(Callable&& callable) {
    std::string message;
    try {
        callable();
    } catch (const Exception& error) {
        message = error.what();
    } catch (const std::exception& error) {
        ADD_FAILURE() << "unexpected exception type: " << error.what();
        message = error.what();
    }
    EXPECT_FALSE(message.empty()) << "expected exception was not thrown";
    return message;
}

// ============================================================
// 配置 document 夹具
// ============================================================

/// 合法且最小的可运行配置：两个品种、两个账户；可选字段全部省略
nlohmann::json valid_document() {
    return nlohmann::json::parse(R"json({
  "schema_version": 1,
  "period_sec": 60,
  "instruments": ["rb2601", "rb2605"],
  "accounts": [
    {"account_id": "CTP001", "scale": 1.0},
    {"account_id": "CTP002", "scale": 0.5}
  ],
  "params": {"fast": 10, "slow": 30}
})json");
}

/// 六个可选字段全部显式给出的合法配置（max_lookback=2000 / reserve_bars=3000 /
/// warmup.bars=50000 互相自洽；accounts 的 scale 不超过 max_scale=2.0）
nlohmann::json document_with_optionals() {
    nlohmann::json document = valid_document();
    document["max_scale"] = 2.0;
    document["scale_rounding"] = "ceil";
    document["scale_min_one"] = true;
    document["max_lookback"] = 2000;
    document["reserve_bars"] = 3000;
    document["warmup"] = {{"bars", 50000}};
    return document;
}

/// `valid_document()` 加上三个可省略的容量/预热字段（max_scale 仍省略）
nlohmann::json document_with_capacity_overrides() {
    nlohmann::json document = valid_document();
    document["max_lookback"] = 2000;
    document["reserve_bars"] = 3000;
    document["warmup"] = {{"bars", 50000}};
    return document;
}

// ============================================================
// 夹具
// ============================================================

class BarStrategyConfigFileTest : public ::testing::Test {
protected:
    void SetUp() override {
        // 进程唯一目录；同一进程内直接跑二进制时各用例复用同名目录，SetUp 清空隔离
        root_ = dztrader::test::unique_temp_dir_tracked("dz_test_bar_config");
        std::filesystem::remove_all(root_);
        std::filesystem::create_directories(root_);
    }

    /// 根目录下的文件路径
    fs::path path_in_root(std::string_view file_name) const {
        return root_ / std::string(file_name);
    }

    fs::path root_;
};

// ============================================================
// 原子写
// ============================================================

// 成功：2 空格缩进、不追加结尾换行、可读回、同目录无 tmp 残留。
TEST_F(BarStrategyConfigFileTest, WriteAtomicCreatesIndentedFileWithoutTempResidue) {
    const fs::path path = path_in_root("strategy.json");
    const nlohmann::json document = valid_document();

    dztrader::write_json_file_atomic(path, document);

    ASSERT_TRUE(fs::exists(path));
    const std::string text = read_text(path);
    ASSERT_FALSE(text.empty());
    EXPECT_EQ(text, document.dump(2));
    EXPECT_NE(text.back(), '\n');
    EXPECT_EQ(dztrader::read_json_file(path), document);
    ASSERT_EQ(regular_files(root_).size(), std::size_t{1});
    EXPECT_EQ(regular_files(root_).front(), path);
    EXPECT_FALSE(fs::exists(temp_path_of(path)));
}

// 成功：覆盖旧内容（旧 key 不残留）。
TEST_F(BarStrategyConfigFileTest, WriteAtomicReplacesExistingFile) {
    const fs::path path = path_in_root("strategy.json");
    const nlohmann::json original = valid_document();
    dztrader::write_json_file_atomic(path, original);

    nlohmann::json updated = valid_document();
    updated["period_sec"] = 180;
    updated["instruments"] = {"ag2601"};
    dztrader::write_json_file_atomic(path, updated);

    const std::string text = read_text(path);
    EXPECT_EQ(text, updated.dump(2));
    EXPECT_FALSE(contains(text, "\"rb2601\""));
    EXPECT_EQ(dztrader::read_json_file(path), updated);
    EXPECT_EQ(regular_files(root_).size(), std::size_t{1});
}

// 失败：tmp 位置被目录占住 → 抛错、旧文件逐字节不变、不留下半截文件。
TEST_F(BarStrategyConfigFileTest, WriteAtomicFailureKeepsOldFileWhenTempPathBlocked) {
    const fs::path path = path_in_root("strategy.json");
    const std::string original = valid_document().dump(2);
    write_text(path, original);
    const fs::path temp = temp_path_of(path);
    ASSERT_TRUE(fs::create_directory(temp));

    const std::string message = capture_exception<std::runtime_error>(
        [&] { dztrader::write_json_file_atomic(path, valid_document()); });

    EXPECT_TRUE(contains(message, "config write failed")) << message;
    EXPECT_TRUE(contains(message, temp.string())) << message;
    EXPECT_EQ(read_text(path), original);
    EXPECT_FALSE(fs::is_regular_file(temp));
    ASSERT_EQ(regular_files(root_).size(), std::size_t{1});
    EXPECT_EQ(regular_files(root_).front(), path);
}

// 失败：父目录不存在（本原语不创建父目录）→ 抛错、不创建任何文件。
TEST_F(BarStrategyConfigFileTest, WriteAtomicFailureWhenParentDirectoryMissing) {
    const fs::path path = root_ / "missing" / "strategy.json";

    const std::string message = capture_exception<std::runtime_error>(
        [&] { dztrader::write_json_file_atomic(path, valid_document()); });

    EXPECT_TRUE(contains(message, "config write failed")) << message;
    EXPECT_FALSE(fs::exists(path));
    EXPECT_FALSE(fs::exists(root_ / "missing"));
}

// ============================================================
// 读取
// ============================================================

// 读：合法 object 文件返回 document（压缩格式不影响解析）。
TEST_F(BarStrategyConfigFileTest, ReadJsonFileReturnsParsedObject) {
    const fs::path path = path_in_root("strategy.json");
    const nlohmann::json document = valid_document();
    write_text(path, document.dump());

    EXPECT_EQ(dztrader::read_json_file(path), document);
}

// 读：文件缺失 → runtime_error（含 path）。
TEST_F(BarStrategyConfigFileTest, ReadJsonFileThrowsWhenFileMissing) {
    const fs::path path = path_in_root("absent.json");

    const std::string message =
        capture_exception<std::runtime_error>([&] { (void)dztrader::read_json_file(path); });

    EXPECT_TRUE(contains(message, "config read failed")) << message;
    EXPECT_TRUE(contains(message, path.string())) << message;
}

// 读：坏 JSON 语法 → runtime_error（含 path 与解析错误）。
TEST_F(BarStrategyConfigFileTest, ReadJsonFileThrowsOnBrokenJson) {
    const fs::path path = path_in_root("broken.json");
    write_text(path, R"({"schema_version": 1, )");

    const std::string message =
        capture_exception<std::runtime_error>([&] { (void)dztrader::read_json_file(path); });

    EXPECT_TRUE(contains(message, "config parse failed")) << message;
    EXPECT_TRUE(contains(message, path.string())) << message;
}

// 读：根不是 object → runtime_error；load_config_file 同样拒绝（不进入配置解析）。
TEST_F(BarStrategyConfigFileTest, ReadJsonFileThrowsOnNonObjectRoot) {
    const fs::path path = path_in_root("array.json");
    write_text(path, "[1, 2, 3]");

    const std::string read_message =
        capture_exception<std::runtime_error>([&] { (void)dztrader::read_json_file(path); });
    EXPECT_TRUE(contains(read_message, "config parse failed")) << read_message;
    EXPECT_TRUE(contains(read_message, "root_not_object")) << read_message;

    const std::string load_message =
        capture_exception<std::runtime_error>([&] { (void)dztrader::load_config_file(path); });
    EXPECT_TRUE(contains(load_message, "config parse failed")) << load_message;
    EXPECT_EQ(read_text(path), "[1, 2, 3]");
}

// ============================================================
// 缺失生成 / 加载
// ============================================================

// 缺失：原子写入最小默认配置（五键、不含可选字段）+ GeneratedDefault + effective 为空
// + document 为生成物 + 单行提示 + 非 0 退出码；再次加载因 instruments 为空而报错
// （生成物故意不是可运行配置）。
TEST_F(BarStrategyConfigFileTest, LoadMissingFileGeneratesMinimalDefault) {
    const fs::path path = path_in_root("generated.json");

    const ConfigLoadResult result = dztrader::load_config_file(path);

    EXPECT_EQ(result.status, ConfigLoadStatus::GeneratedDefault);
    EXPECT_EQ(dztrader::config_startup_exit_code(result.status), 1);
    EXPECT_FALSE(result.effective.has_value());

    ASSERT_TRUE(fs::exists(path));
    const nlohmann::json expected = dztrader::make_minimal_default_config();
    EXPECT_EQ(read_text(path), expected.dump(2));
    EXPECT_EQ(result.document, expected);
    const nlohmann::json disk = dztrader::read_json_file(path);
    EXPECT_EQ(disk, expected);
    EXPECT_EQ(disk.size(), std::size_t{5});
    EXPECT_EQ(disk["period_sec"].get<std::int32_t>(), 60);
    EXPECT_TRUE(disk["instruments"].empty());
    EXPECT_TRUE(disk["accounts"].empty());
    EXPECT_TRUE(disk["params"].empty());
    for (const char* field : {"max_scale", "scale_rounding", "scale_min_one", "max_lookback",
                              "reserve_bars", "warmup"}) {
        EXPECT_FALSE(disk.contains(field)) << "unexpected optional field: " << field;
    }
    EXPECT_EQ(regular_files(root_).size(), std::size_t{1});
    EXPECT_FALSE(fs::exists(temp_path_of(path)));

    ASSERT_FALSE(result.message.empty());
    EXPECT_TRUE(contains(result.message, path.string())) << result.message;
    EXPECT_TRUE(contains(result.message, "edit")) << result.message;
    EXPECT_TRUE(contains(result.message, "restart")) << result.message;
    EXPECT_EQ(result.message.find('\n'), std::string::npos) << result.message;
    EXPECT_NE(result.message.back(), '.') << result.message;
    EXPECT_TRUE(std::islower(static_cast<unsigned char>(result.message.front())) != 0)
        << result.message;

    // 生成的配置故意不是可运行配置：instruments 为空 → 加载报错且不改文件
    const std::string generated_text = read_text(path);
    const std::string message =
        capture_exception<std::invalid_argument>([&] { (void)dztrader::load_config_file(path); });
    EXPECT_TRUE(contains(message, "field=instruments")) << message;
    EXPECT_EQ(read_text(path), generated_text);
}

// 缺失：父目录不存在时由 load_config_file 创建（write_json_file_atomic 本身不建目录）。
TEST_F(BarStrategyConfigFileTest, LoadMissingFileCreatesParentDirectory) {
    const fs::path directory = root_ / "new config dir";
    const fs::path path = directory / "strategy.json";

    const ConfigLoadResult result = dztrader::load_config_file(path);

    EXPECT_EQ(result.status, ConfigLoadStatus::GeneratedDefault);
    ASSERT_TRUE(fs::is_directory(directory));
    ASSERT_TRUE(fs::exists(path));
    EXPECT_EQ(read_text(path), dztrader::make_minimal_default_config().dump(2));
}

// 合法：Loaded + effective 有值（周期 60 自动容量与默认预热）+ document 为已校验文档
// + 退出码 0 + 加载不改文件。
TEST_F(BarStrategyConfigFileTest, LoadValidFileReturnsResolvedConfig) {
    const fs::path path = path_in_root("valid.json");
    const nlohmann::json document = valid_document();
    const std::string text = document.dump(2);
    write_text(path, text);

    const ConfigLoadResult result = dztrader::load_config_file(path);

    EXPECT_EQ(result.status, ConfigLoadStatus::Loaded);
    EXPECT_EQ(dztrader::config_startup_exit_code(result.status), 0);
    EXPECT_TRUE(result.message.empty());
    EXPECT_EQ(result.document, document);
    EXPECT_EQ(read_text(path), text);

    ASSERT_TRUE(result.effective.has_value());
    const EffectiveConfig& effective = *result.effective;
    EXPECT_EQ(effective.max_lookback, 1440);
    EXPECT_EQ(effective.reserve_bars, 2880);
    EXPECT_EQ(effective.buffer_bars, 4320);
    EXPECT_EQ(effective.mem_per_series_bytes, 34560);
    EXPECT_EQ(effective.max_lookback_source, ConfigValueSource::Auto);
    EXPECT_EQ(effective.reserve_bars_source, ConfigValueSource::Auto);
    EXPECT_EQ(effective.scaling.max_scale, 1.0);
    EXPECT_EQ(effective.scaling.rounding, ScaleRounding::Nearest);
    EXPECT_FALSE(effective.scaling.min_one);
    ASSERT_TRUE(effective.warmup_bars.has_value());
    EXPECT_EQ(*effective.warmup_bars, 10000);
    EXPECT_EQ(effective.warmup_source, ConfigValueSource::Default);
    EXPECT_EQ(effective.config.instruments.size(), std::size_t{2});
    EXPECT_EQ(effective.config.accounts.size(), std::size_t{2});
}

// 非法：抛 invalid_argument（复用 config.h 的语义校验）且文件逐字节不变。
TEST_F(BarStrategyConfigFileTest, LoadInvalidFileThrowsAndKeepsFileUnchanged) {
    const fs::path path = path_in_root("invalid.json");
    const std::string text = dztrader::make_minimal_default_config().dump(2);
    write_text(path, text);

    const std::string message =
        capture_exception<std::invalid_argument>([&] { (void)dztrader::load_config_file(path); });

    EXPECT_TRUE(contains(message, "field=instruments")) << message;
    EXPECT_EQ(read_text(path), text);
    EXPECT_EQ(regular_files(root_).size(), std::size_t{1});
}

// 目录 / 文件名含空格（Windows 宽路径 _wfopen 覆盖；Linux 同样走通）。
TEST_F(BarStrategyConfigFileTest, HandlesPathsWithSpaces) {
    const fs::path directory = root_ / "config dir with spaces";
    const fs::path path = directory / "my strategy.json";

    const ConfigLoadResult generated = dztrader::load_config_file(path);
    EXPECT_EQ(generated.status, ConfigLoadStatus::GeneratedDefault);
    EXPECT_TRUE(contains(generated.message, path.string())) << generated.message;
    ASSERT_TRUE(fs::exists(path));

    const nlohmann::json document = valid_document();
    dztrader::write_json_file_atomic(path, document);
    EXPECT_EQ(dztrader::read_json_file(path), document);

    const ConfigLoadResult loaded = dztrader::load_config_file(path);
    EXPECT_EQ(loaded.status, ConfigLoadStatus::Loaded);
    ASSERT_TRUE(loaded.effective.has_value());
    EXPECT_EQ(loaded.effective->max_lookback, 1440);
}

// 退出码映射：Loaded=0，GeneratedDefault=1。
TEST_F(BarStrategyConfigFileTest, StartupExitCodeMapsStatuses) {
    EXPECT_EQ(dztrader::config_startup_exit_code(ConfigLoadStatus::Loaded), 0);
    EXPECT_EQ(dztrader::config_startup_exit_code(ConfigLoadStatus::GeneratedDefault), 1);
}

// ============================================================
// ConfigStore：加载镜像
// ============================================================

// 便捷构造：读 document + 全量校验；current() 暴露显式来源与推导值；未知顶层字段保留。
TEST_F(BarStrategyConfigFileTest, ConfigStoreLoadsDocumentAndEffectiveConfig) {
    const fs::path path = path_in_root("store.json");
    nlohmann::json document = document_with_optionals();
    document["future_field"] = {{"mode", "v2"}};
    write_text(path, document.dump(2));

    const ConfigStore store(path);

    EXPECT_EQ(store.document(), document);
    const EffectiveConfig& current = store.current();
    EXPECT_EQ(current.scaling.max_scale, 2.0);
    EXPECT_EQ(current.scaling.rounding, ScaleRounding::Ceil);
    EXPECT_TRUE(current.scaling.min_one);
    EXPECT_EQ(current.max_lookback, 2000);
    EXPECT_EQ(current.max_lookback_source, ConfigValueSource::Explicit);
    EXPECT_EQ(current.reserve_bars, 3000);
    EXPECT_EQ(current.reserve_bars_source, ConfigValueSource::Explicit);
    EXPECT_EQ(current.buffer_bars, 5000);
    EXPECT_EQ(current.mem_per_series_bytes, 40000);
    ASSERT_TRUE(current.warmup_bars.has_value());
    EXPECT_EQ(*current.warmup_bars, 50000);
    EXPECT_EQ(current.warmup_source, ConfigValueSource::Explicit);
}

// 快照构造（load_config_file 的 document + *effective）：不重新读文件（避免 TOCTOU），
// 之后 commit 仍能正常写回。
TEST_F(BarStrategyConfigFileTest, ConfigStoreConstructedFromLoadResultUsesSnapshot) {
    const fs::path path = path_in_root("store.json");
    write_text(path, valid_document().dump(2));
    const ConfigLoadResult loaded = dztrader::load_config_file(path);
    ASSERT_EQ(loaded.status, ConfigLoadStatus::Loaded);
    ASSERT_TRUE(loaded.effective.has_value());

    // 模拟加载后文件被外部改写：从快照构造不得重新读文件（否则会抛 parse 错误）
    write_text(path, "{ not json");
    ConfigStore store(path, loaded.document, *loaded.effective);
    EXPECT_EQ(store.document(), loaded.document);
    EXPECT_EQ(store.current().max_lookback, 1440);

    BarStrategyConfig next = store.current().config;
    next.instruments = {"ag2601"};
    (void)store.commit(next);
    const nlohmann::json disk = dztrader::read_json_file(path);
    EXPECT_EQ(disk["instruments"], nlohmann::json::array({"ag2601"}));
}

// 便捷构造：文件缺失 → runtime_error；文件非法 → invalid_argument。
TEST_F(BarStrategyConfigFileTest, ConfigStoreConstructorRejectsMissingAndInvalidFiles) {
    const fs::path missing = path_in_root("absent.json");
    const std::string missing_message =
        capture_exception<std::runtime_error>([&] { (void)ConfigStore(missing); });
    EXPECT_TRUE(contains(missing_message, "config read failed")) << missing_message;
    EXPECT_TRUE(contains(missing_message, missing.string())) << missing_message;

    const fs::path invalid = path_in_root("invalid.json");
    const std::string invalid_text = dztrader::make_minimal_default_config().dump(2);
    write_text(invalid, invalid_text);
    const std::string invalid_message =
        capture_exception<std::invalid_argument>([&] { (void)ConfigStore(invalid); });
    EXPECT_TRUE(contains(invalid_message, "field=instruments")) << invalid_message;
    EXPECT_EQ(read_text(invalid), invalid_text);
}

// ============================================================
// ConfigStore::commit：写盘成功才改内存
// ============================================================

// 合法 commit：文件更新（未知顶层字段保留、可选字段省略态保留）+ 内存更新 + 可重新加载。
TEST_F(BarStrategyConfigFileTest, ConfigStoreCommitRewritesFilePreservingUnknownAndOmission) {
    const fs::path path = path_in_root("store.json");
    nlohmann::json document = valid_document();
    document["future_field"] = {{"mode", "v2"}};
    write_text(path, document.dump(2));
    ConfigStore store(path);

    BarStrategyConfig next = store.current().config;
    next.instruments = {"ag2601"};
    const EffectiveConfig committed = store.commit(next);

    EXPECT_EQ(committed.config.instruments, std::vector<std::string>{"ag2601"});
    EXPECT_EQ(store.current().config.instruments, std::vector<std::string>{"ag2601"});
    const nlohmann::json disk = dztrader::read_json_file(path);
    EXPECT_EQ(disk, store.document());
    EXPECT_EQ(disk["instruments"], nlohmann::json::array({"ag2601"}));
    ASSERT_TRUE(disk.contains("future_field"));
    EXPECT_EQ(disk["future_field"]["mode"].get<std::string>(), "v2");
    for (const char* field : {"max_scale", "scale_rounding", "scale_min_one", "max_lookback",
                              "reserve_bars", "warmup"}) {
        EXPECT_FALSE(disk.contains(field)) << "unexpected optional field written: " << field;
    }
    EXPECT_EQ(store.current().max_lookback, 1440);
    EXPECT_EQ(store.current().max_lookback_source, ConfigValueSource::Auto);

    const ConfigStore reloaded(path);
    EXPECT_EQ(reloaded.current().config.instruments, std::vector<std::string>{"ag2601"});
    EXPECT_EQ(reloaded.current().max_lookback, 1440);
}

// 合法 commit：可变的显式字段按配置写回（rounding 写字符串、warmup 写 from 模式），
// 不可变的 max_scale 保持当前值不变。
TEST_F(BarStrategyConfigFileTest, ConfigStoreCommitWritesExplicitOptionalFields) {
    const fs::path path = path_in_root("store.json");
    write_text(path, document_with_optionals().dump(2));
    ConfigStore store(path);

    BarStrategyConfig next = store.current().config;
    next.scale_rounding = ScaleRounding::Floor;
    next.scale_min_one = false;
    next.max_lookback = 4000;
    next.reserve_bars = 5000;
    next.warmup = {};
    next.warmup.from = "2020-01-02";
    const EffectiveConfig committed = store.commit(next);

    const nlohmann::json disk = dztrader::read_json_file(path);
    EXPECT_EQ(disk, store.document());
    EXPECT_EQ(disk["max_scale"].get<double>(), 2.0);  // 未改动，仍保留显式值
    EXPECT_EQ(disk["scale_rounding"].get<std::string>(), "floor");
    EXPECT_EQ(disk["scale_min_one"].get<bool>(), false);
    EXPECT_EQ(disk["max_lookback"].get<std::int32_t>(), 4000);
    EXPECT_EQ(disk["reserve_bars"].get<std::int32_t>(), 5000);
    ASSERT_TRUE(disk.contains("warmup"));
    EXPECT_FALSE(disk["warmup"].contains("bars"));
    EXPECT_EQ(disk["warmup"]["from"].get<std::string>(), "2020-01-02");
    EXPECT_EQ(disk["accounts"], valid_document()["accounts"]);

    EXPECT_EQ(committed.scaling.max_scale, 2.0);
    EXPECT_EQ(committed.scaling.rounding, ScaleRounding::Floor);
    EXPECT_FALSE(committed.scaling.min_one);
    EXPECT_EQ(committed.max_lookback, 4000);
    EXPECT_EQ(committed.max_lookback_source, ConfigValueSource::Explicit);
    EXPECT_EQ(committed.reserve_bars, 5000);
    EXPECT_EQ(committed.reserve_bars_source, ConfigValueSource::Explicit);
    EXPECT_EQ(committed.buffer_bars, 9000);
    EXPECT_EQ(committed.mem_per_series_bytes, 72000);
    ASSERT_TRUE(committed.warmup_from.has_value());
    EXPECT_EQ(*committed.warmup_from, "2020-01-02");
    EXPECT_FALSE(committed.warmup_bars.has_value());
    EXPECT_EQ(committed.warmup_source, ConfigValueSource::Explicit);
}

// 合法 commit：可选字段变成省略态时从文件 erase（回到自动值 / 默认值）。
TEST_F(BarStrategyConfigFileTest, ConfigStoreCommitErasesFieldsThatBecomeOmitted) {
    const fs::path path = path_in_root("store.json");
    write_text(path, document_with_capacity_overrides().dump(2));
    ConfigStore store(path);

    BarStrategyConfig next = store.current().config;
    next.max_lookback.reset();
    next.reserve_bars.reset();
    next.warmup = {};
    const EffectiveConfig committed = store.commit(next);

    const nlohmann::json disk = dztrader::read_json_file(path);
    EXPECT_EQ(disk, store.document());
    for (const char* field : {"max_scale", "scale_rounding", "scale_min_one", "max_lookback",
                              "reserve_bars", "warmup"}) {
        EXPECT_FALSE(disk.contains(field)) << "optional field not erased: " << field;
    }
    EXPECT_EQ(disk["instruments"], valid_document()["instruments"]);
    EXPECT_EQ(disk["accounts"], valid_document()["accounts"]);
    EXPECT_EQ(disk["params"], valid_document()["params"]);

    EXPECT_EQ(committed.scaling.max_scale, 1.0);
    EXPECT_EQ(committed.scaling.rounding, ScaleRounding::Nearest);
    EXPECT_FALSE(committed.scaling.min_one);
    EXPECT_EQ(committed.max_lookback, 1440);
    EXPECT_EQ(committed.max_lookback_source, ConfigValueSource::Auto);
    EXPECT_EQ(committed.reserve_bars, 2880);
    EXPECT_EQ(committed.reserve_bars_source, ConfigValueSource::Auto);
    ASSERT_TRUE(committed.warmup_bars.has_value());
    EXPECT_EQ(*committed.warmup_bars, 10000);
    EXPECT_EQ(committed.warmup_source, ConfigValueSource::Default);
}

// 非法 commit：复用 resolve 全量校验（空 instruments 拒绝）→ 抛错、内存与文件均不变。
TEST_F(BarStrategyConfigFileTest, ConfigStoreCommitRejectsInvalidConfigAndKeepsState) {
    const fs::path path = path_in_root("store.json");
    const std::string text = valid_document().dump(2);
    write_text(path, text);
    ConfigStore store(path);
    const nlohmann::json before_document = store.document();
    const std::string before_text = read_text(path);

    BarStrategyConfig next = store.current().config;
    next.instruments.clear();
    const std::string message =
        capture_exception<std::invalid_argument>([&] { (void)store.commit(next); });

    EXPECT_TRUE(contains(message, "field=instruments")) << message;
    EXPECT_EQ(store.document(), before_document);
    EXPECT_EQ(store.current().config.instruments.size(), std::size_t{2});
    EXPECT_EQ(read_text(path), before_text);
    EXPECT_FALSE(fs::exists(temp_path_of(path)));
}

// 运行期不可变：period_sec 盘中不可改（ADR 0027 决策 11）→ 抛错、内存与文件均不变。
TEST_F(BarStrategyConfigFileTest, ConfigStoreCommitRejectsPeriodChange) {
    const fs::path path = path_in_root("store.json");
    const std::string text = valid_document().dump(2);
    write_text(path, text);
    ConfigStore store(path);
    const nlohmann::json before_document = store.document();

    BarStrategyConfig next = store.current().config;
    next.period_sec = 180;
    const std::string message =
        capture_exception<std::invalid_argument>([&] { (void)store.commit(next); });

    EXPECT_TRUE(contains(message, "field=period_sec")) << message;
    EXPECT_TRUE(contains(message, "value=180")) << message;
    EXPECT_TRUE(contains(message, "expected=60")) << message;
    EXPECT_EQ(store.document(), before_document);
    EXPECT_EQ(store.current().config.period_sec, 60);
    EXPECT_EQ(store.current().max_lookback, 1440);
    EXPECT_EQ(read_text(path), text);
}

// 运行期不可变：max_scale 盘中不可改 → 抛错、内存与文件均不变。
TEST_F(BarStrategyConfigFileTest, ConfigStoreCommitRejectsMaxScaleChange) {
    const fs::path path = path_in_root("store.json");
    const std::string text = document_with_optionals().dump(2);
    write_text(path, text);
    ConfigStore store(path);
    const nlohmann::json before_document = store.document();

    BarStrategyConfig next = store.current().config;
    next.max_scale = 1.5;
    const std::string message =
        capture_exception<std::invalid_argument>([&] { (void)store.commit(next); });

    EXPECT_TRUE(contains(message, "field=max_scale")) << message;
    EXPECT_TRUE(contains(message, "value=1.5")) << message;
    EXPECT_TRUE(contains(message, "expected=2.0")) << message;
    EXPECT_EQ(store.document(), before_document);
    EXPECT_EQ(store.current().scaling.max_scale, 2.0);
    EXPECT_EQ(read_text(path), text);
}

// 写盘失败：抛 runtime_error → 内存与文件均不变（写盘成功才改内存）。
TEST_F(BarStrategyConfigFileTest, ConfigStoreCommitWriteFailureKeepsState) {
    const fs::path path = path_in_root("store.json");
    const std::string text = valid_document().dump(2);
    write_text(path, text);
    ConfigStore store(path);
    const nlohmann::json before_document = store.document();
    const std::string before_text = read_text(path);
    const fs::path temp = temp_path_of(path);
    ASSERT_TRUE(fs::create_directory(temp));

    BarStrategyConfig next = store.current().config;
    next.instruments = {"hc2601"};
    const std::string message =
        capture_exception<std::runtime_error>([&] { (void)store.commit(next); });

    EXPECT_TRUE(contains(message, "config write failed")) << message;
    EXPECT_TRUE(contains(message, temp.string())) << message;
    EXPECT_EQ(store.document(), before_document);
    const std::vector<std::string> original_instruments{"rb2601", "rb2605"};
    EXPECT_EQ(store.current().config.instruments, original_instruments);
    EXPECT_EQ(read_text(path), before_text);
    EXPECT_FALSE(fs::is_regular_file(temp));
}

}  // namespace
