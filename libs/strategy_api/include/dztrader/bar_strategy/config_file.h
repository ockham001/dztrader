/**
 * @file config_file.h
 * @brief BarStrategy 配置文件面：原子写回 / 读取 / 缺失默认生成 / 内存镜像（issue #156）
 *
 * **内部头，不进公开承诺面**：BarStrategy 框架（#157）与命令层（#162）直接包含；
 * 公开入口 `dztrader/bar_strategy.h` 在 #157 里决定暴露哪些类型。策略作者（B 侧）
 * 不直接读写配置——配置由框架在启动时加载、由命令层写回。
 *
 * 职责边界（落盘字段、解析、校验、推导见 `dztrader/bar_strategy/config.h`）：
 *
 *   - `write_json_file_atomic`：同目录 `<path>.tmp` → 写全 → fflush → fsync（POSIX）
 *     / `_commit`（Windows）→ close → `std::filesystem::rename` 原子替换；POSIX 再
 *     best-effort fsync 父目录。失败清理 tmp、旧文件保持原样。**不创建父目录**
 *     （缺失配置的父目录创建由 `load_config_file` 负责）。
 *   - `read_json_file`：读 + JSON 语法 + 根必须是 object（IO/语法错误抛
 *     `std::runtime_error`）。
 *   - `load_config_file`：缺失 → 建父目录 + 原子写最小默认配置 + `GeneratedDefault`
 *     + 提示消息（框架打印该消息并以非 0 退出，等作者编辑后重启）；存在 → 读 + 解析 +
 *     校验，返回已校验 document 与有效配置（供 `ConfigStore` 直接构造，避免双读）。
 *   - `ConfigStore`：内存镜像（`document()` / `current()`）+ `commit()`——**先全量校验、
 *     再原子写盘、写盘成功后才改内存**；失败抛异常且内存与文件都不变（组件文档 §8
 *     「写盘顺序」）。
 *
 * 写回语义（组件文档 §7 / §8）：
 *   - 以**当前 document 为基底**覆盖已知顶层字段 → 未知顶层字段原样保留（前向兼容）。
 *   - 可选字段的**省略态**保留：`max_scale` / `scale_rounding` / `scale_min_one` /
 *     `max_lookback` / `reserve_bars` / `warmup` 在 `BarStrategyConfig` 里是 `nullopt`
 *     时从 document 中 erase，不写成显式默认值（省略 = 用默认值，与显式默认值有别）。
 *   - **运行期不可变字段**（`period_sec`、`max_scale`，ADR 0027 决策 11）：`commit`
 *     在写盘前拒绝修改，消息给出当前值（重启生效，命令层据此返回错误行）。
 *
 * 文件字节格式：`value.dump(2)`（2 空格缩进、**不追加**结尾换行），与
 * `libs/core/include/dztrader/core/json_section.h` / `apps/sysmon/config.cpp` 先例一致。
 * 与平台 `json_section.h` 的原子写**有意不共用**（SDK 公开头不能依赖内部库，且本票要求
 * 文件 `fsync`），取舍见 ADR 0039。
 *
 * 跨平台：仅 Windows x86_64 / MSVC 与 Linux x86_64 / GCC；其它组合 `#error`
 * （AGENTS 跨平台硬规则，禁止静默回退）。Windows 落盘用 `_wfopen` 走宽路径，不包含
 * `<windows.h>`。
 */
#ifndef DZTRADER_BAR_STRATEGY_CONFIG_FILE_H_
#define DZTRADER_BAR_STRATEGY_CONFIG_FILE_H_

#include <dztrader/bar_strategy/config.h>
#include <dztrader/json.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

// 跨平台守卫（docs/agents/build-appendix.md「跨平台编译期检查模板」）：
// 只有 Windows x86_64 / MSVC 与 Linux x86_64 / GCC 两种组合，其它一律 #error，禁止静默回退。
#if defined(_M_IX86) || defined(__i386__) || defined(_M_ARM) || defined(__arm__) || \
    defined(_M_ARM64) || defined(__aarch64__)
#error "unsupported: config_file.h requires 64-bit x86 (Windows MSVC or Linux GCC)"
#endif
#if defined(_WIN32) && defined(_M_X64)
#define DZTRADER_CONFIG_FILE_PLATFORM_WINDOWS 1
#elif defined(__linux__) && defined(__x86_64__)
#define DZTRADER_CONFIG_FILE_PLATFORM_LINUX 1
#else
#error "unsupported: config_file.h requires Windows x86_64 (MSVC) or Linux x86_64 (GCC)"
#endif

#ifdef DZTRADER_CONFIG_FILE_PLATFORM_WINDOWS
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace dztrader {

// ============================================================
// 加载结果（组件文档 §7「缺失：生成最小默认配置 + 非 0 退出」）
// ============================================================

/// @brief 配置加载结果的状态：正常加载 / 缺失后生成默认配置（需用户编辑后重启）
enum class ConfigLoadStatus {
    Loaded,            ///< 文件存在且合法，`effective` 有值
    GeneratedDefault,  ///< 文件缺失，已原子写入最小默认配置；框架应打印 message 并非 0 退出
};

/**
 * @brief 配置加载结果
 *
 * 最小默认配置故意不是可运行配置（`instruments` 为空），`GeneratedDefault` 状态下
 * 无法推导出合法的 {@link EffectiveConfig}，故 `effective` 为 `nullopt`——不给调用方
 * 垃圾值；`document` 带上刚写入/刚读到的文件内容，供 `ConfigStore` 直接构造。
 */
struct ConfigLoadResult {
    ConfigLoadStatus status = ConfigLoadStatus::Loaded;  ///< 加载状态
    std::optional<EffectiveConfig> effective;            ///< 有效配置（仅 `Loaded` 时有值）
    nlohmann::json document;  ///< `Loaded`=已校验文档；`GeneratedDefault`=生成的默认配置
    std::string message;      ///< `GeneratedDefault` 时给用户看的单行提示；`Loaded` 时为空
};

/**
 * @brief 配置启动阶段的进程退出码（#157 接线）
 *
 * `Loaded` = 0；`GeneratedDefault` 等其它状态 = 1（等用户编辑配置后重启，
 * 不静默替用户猜品种）。
 */
[[nodiscard]] constexpr std::int32_t config_startup_exit_code(ConfigLoadStatus status) noexcept {
    return status == ConfigLoadStatus::Loaded ? 0 : 1;
}

/**
 * @brief 文件层加载/写回的可选注入（测试确定性）
 *
 * `warmup.from` 的「不晚于今天」需要当前本地日期。生产路径省略本结构即用本机今天；
 * 测试可显式注入得到完全确定的行为（含 #162 之后写回路径的一致性——`ConfigStore`
 * 记住构造时的注入值，`commit()` 重算校验用同一个 today，不用墙钟）。
 */
struct ConfigLoadOptions {
    std::optional<Date> today;  ///< 省略 = 本机本地日期（`DateTime::local_now().date()`）
};

// ============================================================
// 内部辅助（dztrader::config_file_internal）
// ============================================================

namespace config_file_internal {

/// @brief 注入的「今天」；省略取本机本地日期
[[nodiscard]] inline Date resolve_today(const ConfigLoadOptions& options) {
    return options.today.has_value() ? *options.today : DateTime::local_now().date();
}

/// @brief 原子写的同目录临时文件后缀（`<path>.tmp`）
inline constexpr std::string_view TMP_SUFFIX = ".tmp";

/// @brief 日志/错误消息里的值：含空格 / 等号 / 引号的一律双引号包裹（`k="v"`）
[[nodiscard]] inline std::string quote_value(std::string_view value) {
    std::string quoted_value;
    quoted_value.reserve(value.size() + 2);
    quoted_value += '"';
    quoted_value += value;
    quoted_value += '"';
    return quoted_value;
}

/// @brief 路径值的消息形态：`path="..."`（path 值加双引号）
[[nodiscard]] inline std::string quoted_path(const std::filesystem::path& path) {
    return quote_value(path.string());
}

/// @brief stdio 失败的机器可读原因：` errno=<n>`（不引 strerror，MSVC 下为废弃函数）
[[nodiscard]] inline std::string errno_clause(int error_number) {
    return " errno=" + std::to_string(error_number);
}

/// @brief 错误码的 ` error="..."` 片段
[[nodiscard]] inline std::string error_clause(const std::error_code& error) {
    return " error=" + quote_value(error.message());
}

/// @brief 清理自己创建的临时文件；失败忽略（清理不能掩盖原始写盘错误）
inline void remove_temp_file_best_effort(const std::filesystem::path& tmp_path) noexcept {
    std::error_code ec;
    // 只删自己创建的普通文件 / 符号链接：tmp 位置被目录占住时不得替用户删目录
    if (std::filesystem::is_regular_file(tmp_path, ec) ||
        std::filesystem::is_symlink(tmp_path, ec)) {
        std::filesystem::remove(tmp_path, ec);
    }
}

// ============================================================
// 配置写回：已知顶层字段覆盖（保留未知顶层字段 + 省略态）
// ============================================================

/// @brief 可选字段的落盘：有值写显式值，`nullopt` 从 document 中 erase（保留省略态）
template <typename T>
inline void set_optional_member(nlohmann::json& document,
                                const char* field,
                                const std::optional<T>& value) {
    if (value.has_value()) {
        document[field] = *value;
    } else {
        document.erase(field);
    }
}

/// @brief 挂靠账户数组的落盘形态（`[{"account_id": ..., "scale": ...}]`，按配置顺序）
[[nodiscard]] inline nlohmann::json accounts_document(
    const std::vector<AttachedAccount>& accounts) {
    nlohmann::json array = nlohmann::json::array();
    for (const AttachedAccount& account : accounts) {
        nlohmann::json item = nlohmann::json::object();
        item["account_id"] = account.account_id;
        item["scale"] = account.scale;
        array.push_back(std::move(item));
    }
    return array;
}

/**
 * @brief 把落盘字段覆盖到 document 的已知顶层字段上
 *
 * 未知顶层字段原样保留（命令层写回不能丢掉未来版本或其他工具写入的字段）；
 * `BarStrategyConfig` 里 `nullopt` 的可选字段从 document 中 erase（省略态保留）。
 *
 * @param document 以当前文件 document 为基底的可变 JSON（必须是 object）
 * @param config   新配置（通常来自 `ConfigStore::current().config` 的副本）
 */
inline void apply_config(nlohmann::json& document, const BarStrategyConfig& config) {
    document["schema_version"] = config.schema_version;
    document["period_sec"] = config.period_sec;
    document["instruments"] = config.instruments;
    document["accounts"] = accounts_document(config.accounts);
    document["params"] = config.params;

    set_optional_member(document, "max_scale", config.max_scale);
    if (config.scale_rounding.has_value()) {
        document["scale_rounding"] = std::string(scale_rounding_name(*config.scale_rounding));
    } else {
        document.erase("scale_rounding");
    }
    set_optional_member(document, "scale_min_one", config.scale_min_one);
    set_optional_member(document, "max_lookback", config.max_lookback);
    set_optional_member(document, "reserve_bars", config.reserve_bars);

    if (config.warmup.bars.has_value()) {
        document["warmup"] = nlohmann::json{{"bars", *config.warmup.bars}};
    } else if (config.warmup.from.has_value()) {
        document["warmup"] = nlohmann::json{{"from", *config.warmup.from}};
    } else {
        document.erase("warmup");
    }
}

// ============================================================
// 平台相关的落盘原语（Windows：_wfopen / _commit；Linux：fopen / fsync）
// ============================================================

/// @brief 打开同目录临时文件做二进制写（Windows 宽路径：非 ASCII 目录名不丢）
[[nodiscard]] inline std::FILE* open_write_stream(const std::filesystem::path& path) {
#ifdef DZTRADER_CONFIG_FILE_PLATFORM_WINDOWS
    return ::_wfopen(path.wstring().c_str(), L"wb");
#else
    return std::fopen(path.string().c_str(), "wb");
#endif
}

/// @brief 写全并刷盘：fflush → fsync（POSIX）/ `_commit`（Windows）；失败抛 runtime_error
///
/// 文件 fsync 失败必须抛：rename 之后才发现数据没落盘就晚了（磁盘满 / 配额耗尽）。
inline void flush_and_sync(std::FILE* stream, const std::filesystem::path& tmp_path) {
    const std::string path_clause = " path=" + quoted_path(tmp_path);
    if (std::fflush(stream) != 0) {
        throw std::runtime_error("config write failed |" + path_clause + errno_clause(errno));
    }
#ifdef DZTRADER_CONFIG_FILE_PLATFORM_WINDOWS
    const int file_descriptor = ::_fileno(stream);
    if (file_descriptor < 0) {
        throw std::runtime_error("config sync failed |" + path_clause);
    }
    if (::_commit(file_descriptor) != 0) {
        throw std::runtime_error("config sync failed |" + path_clause + errno_clause(errno));
    }
#else
    if (::fsync(::fileno(stream)) != 0) {
        throw std::runtime_error("config sync failed |" + path_clause + errno_clause(errno));
    }
#endif
}

/// @brief rename 成功后 best-effort 刷父目录（POSIX 持久化目录项）；失败不抛
///
/// 此时文件已成功原子替换，把目录 fsync 失败判成写失败会误导调用方（本票 AC 只要求文件
/// 落盘失败必须报错）；Windows 没有目录 fsync，由文件系统负责 rename 的元数据持久化。
inline void sync_parent_directory_best_effort(const std::filesystem::path& path) noexcept {
#ifdef DZTRADER_CONFIG_FILE_PLATFORM_WINDOWS
    (void)path;  // Windows 无目录 fsync
#else
    std::filesystem::path parent = path.parent_path();
    if (parent.empty()) {
        parent = ".";
    }
    const int directory_fd = ::open(parent.string().c_str(), O_RDONLY | O_DIRECTORY);
    if (directory_fd < 0) {
        return;
    }
    (void)::fsync(directory_fd);
    (void)::close(directory_fd);
#endif
}

}  // namespace config_file_internal

// ============================================================
// 原子写 / 读（组件文档 §7「写回：临时文件 + fsync + 原子替换」）
// ============================================================

/**
 * @brief 原子写 JSON 文件（2 空格缩进、不追加结尾换行）
 *
 * 流程：同目录 `<path>.tmp` → 写全 → fflush → fsync（POSIX）/ `_commit`（Windows）→
 * close → `std::filesystem::rename` 原子替换 → POSIX best-effort fsync 父目录。
 * 失败清理 tmp、旧文件保持原样（不会出现半截文件）。**不创建父目录**（primitive 严格；
 * 缺失配置的父目录创建由 `load_config_file` 负责，strategy_home 通常已存在）。
 *
 * @param path     目标文件路径
 * @param document 要写入的 JSON（通常为整份 document）
 * @throw std::runtime_error 打开 / 写入 / 刷盘 / close / rename 失败
 */
inline void write_json_file_atomic(const std::filesystem::path& path,
                                   const nlohmann::json& document) {
    using namespace config_file_internal;

    std::filesystem::path tmp_path = path;
    tmp_path += std::string(TMP_SUFFIX);

    // 先序列化再落盘：dump 失败（含非法 UTF-8）不会留下半截临时文件
    const std::string text = document.dump(2);

    std::FILE* stream = open_write_stream(tmp_path);
    if (stream == nullptr) {
        const int open_error = errno;
        remove_temp_file_best_effort(tmp_path);
        throw std::runtime_error("config write failed | path=" + quoted_path(tmp_path) +
                                 errno_clause(open_error));
    }
    try {
        const std::size_t written = std::fwrite(text.data(), 1, text.size(), stream);
        if (written != text.size()) {
            throw std::runtime_error("config write failed | path=" + quoted_path(tmp_path) +
                                     errno_clause(errno));
        }
        flush_and_sync(stream, tmp_path);
    } catch (...) {
        (void)std::fclose(stream);
        remove_temp_file_best_effort(tmp_path);
        throw;
    }
    if (std::fclose(stream) != 0) {
        const int close_error = errno;
        remove_temp_file_best_effort(tmp_path);
        throw std::runtime_error("config write failed | path=" + quoted_path(tmp_path) +
                                 errno_clause(close_error));
    }

    std::error_code rename_error;
    std::filesystem::rename(tmp_path, path, rename_error);
    if (rename_error) {
        remove_temp_file_best_effort(tmp_path);
        throw std::runtime_error("config rename failed | from=" + quoted_path(tmp_path) +
                                 " to=" + quoted_path(path) + error_clause(rename_error));
    }
    sync_parent_directory_best_effort(path);
}

/**
 * @brief 读取并解析 JSON object 文件
 *
 * @param path 文件路径
 * @return 根 JSON object
 * @throw std::runtime_error 打开/读取失败、JSON 语法错误、根不是 object
 */
[[nodiscard]] inline nlohmann::json read_json_file(const std::filesystem::path& path) {
    using namespace config_file_internal;

    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("config read failed | path=" + quoted_path(path));
    }
    const std::string text{std::istreambuf_iterator<char>(stream),
                           std::istreambuf_iterator<char>()};
    if (stream.bad()) {
        throw std::runtime_error("config read failed | path=" + quoted_path(path) +
                                 errno_clause(errno));
    }

    nlohmann::json document;
    try {
        document = nlohmann::json::parse(text);
    } catch (const std::exception& error) {
        throw std::runtime_error("config parse failed | path=" + quoted_path(path) +
                                 " error=" + quote_value(error.what()));
    }
    if (!document.is_object()) {
        throw std::runtime_error("config parse failed | path=" + quoted_path(path) +
                                 " reason=root_not_object");
    }
    return document;
}

// ============================================================
// 缺失默认生成 / 加载（组件文档 §7）
// ============================================================

/**
 * @brief 加载配置文件；文件缺失时建父目录、原子写入最小默认配置并返回 `GeneratedDefault`
 *
 * 缺失分支：`create_directories(path.parent_path())`（父目录为空则跳过；与
 * `apps/master/config.cpp` 的 `generate_default_config` 同款）→ 写
 * `make_minimal_default_config()`（五键、不含可选字段）→ 返回 `GeneratedDefault` +
 * 单行提示消息（含 path 与「编辑后重启」提示）；框架打印 message 并用
 * `config_startup_exit_code()` 非 0 退出。
 *
 * 存在分支：读 + JSON 解析 + 结构/语义校验；非法抛异常且**不改文件**（配置文件是用户
 * 手写的，框架不静默修正）。
 *
 * @note `warmup.from` 的「不晚于今天」上界默认取本机本地日期（与 #157 的启动时机一致）；
 *       需要完全确定性的调用方（测试 / 回放）可经 {@link ConfigLoadOptions.today} 注入。
 *
 * @param path    配置文件路径
 * @param options 可选注入（当前只有 `today`；省略 = 本机今天）
 * @return 加载结果（`Loaded` 时含已校验 document 与有效配置；`GeneratedDefault` 时含
 *         生成的默认配置与提示消息）
 * @throw std::runtime_error 读/解析/建目录/默认配置写盘失败
 * @throw std::invalid_argument 配置结构或语义非法
 */
[[nodiscard]] inline ConfigLoadResult load_config_file(const std::filesystem::path& path,
                                                       const ConfigLoadOptions& options = {}) {
    using namespace config_file_internal;

    std::error_code exists_error;
    const bool file_exists = std::filesystem::exists(path, exists_error);
    if (exists_error) {
        throw std::runtime_error("config read failed | path=" + quoted_path(path) +
                                 error_clause(exists_error));
    }

    ConfigLoadResult result;
    if (!file_exists) {
        const std::filesystem::path parent = path.parent_path();
        if (!parent.empty()) {
            std::error_code directory_error;
            std::filesystem::create_directories(parent, directory_error);
            if (directory_error) {
                throw std::runtime_error("config dir create failed | path=" + quoted_path(parent) +
                                         error_clause(directory_error));
            }
        }
        nlohmann::json document = make_minimal_default_config();
        write_json_file_atomic(path, document);
        result.status = ConfigLoadStatus::GeneratedDefault;
        result.document = std::move(document);
        result.message = "config file missing, wrote minimal default | path=" + quoted_path(path) +
                         " action=" + quote_value("edit the file and restart the strategy");
        return result;
    }

    nlohmann::json document = read_json_file(path);
    result.status = ConfigLoadStatus::Loaded;
    result.effective = parse_and_resolve_config(document, resolve_today(options));
    result.document = std::move(document);
    return result;
}

// ============================================================
// 内存镜像 + 写回（组件文档 §8「写盘顺序」）
// ============================================================

/**
 * @brief 配置文件的进程内镜像：加载一次，命令写回时**先写盘成功再改内存**
 *
 * 两种构造：
 *   - `ConfigStore(path, document, current)`：直接用 `load_config_file` 的
 *     `document` + `*effective` 构造（避免框架双读文件 / TOCTOU）；
 *   - `ConfigStore(path)`：便捷构造，读文件 + 解析 + 全量校验（测试与独立工具用）。
 *
 * `commit()` 的顺序是：`resolve_effective_config` 全量校验（非法抛
 * `std::invalid_argument`，不写盘）→ 运行期不可变字段守卫（`period_sec` / `max_scale`
 * 盘中不可变，ADR 0027 决策 11）→ 以当前 document 为基底覆盖已知顶层字段（未知顶层
 * 字段保留、可选字段省略态保留）→ 原子写盘 → **写盘成功后才**替换
 * `document()` / `current()`。写盘失败抛 `std::runtime_error`，内存与文件都不变
 * （命令层据此返回错误行、状态不变）。
 */
class ConfigStore {
public:
    /**
     * @brief 用已加载的结果构造（框架启动路径：`load_config_file` 的 document + effective）
     * @param path     配置文件路径
     * @param document 已校验文件 document
     * @param current  已解析的有效配置
     */
    ConfigStore(std::filesystem::path path,
                nlohmann::json document,
                EffectiveConfig current,
                ConfigLoadOptions options = {})
        : path_(std::move(path)),
          document_(std::move(document)),
          current_(std::move(current)),
          options_(std::move(options)) {}

    /**
     * @brief 从配置文件构造（读文件 + 解析 + 全量校验；文件必须存在且合法）
     * @param path 配置文件路径
     * @throw std::runtime_error 读/解析失败
     * @throw std::invalid_argument 配置结构或语义非法
     */
    explicit ConfigStore(const std::filesystem::path& path, ConfigLoadOptions options = {}) {
        document_ = read_json_file(path);
        current_ =
            parse_and_resolve_config(document_, config_file_internal::resolve_today(options));
        path_ = path;
        options_ = std::move(options);
    }

    /// @brief 当前生效配置（上次成功加载/写回的结果）
    [[nodiscard]] const EffectiveConfig& current() const noexcept { return current_; }

    /// @brief 当前文件 document（未知顶层字段原样保留）
    [[nodiscard]] const nlohmann::json& document() const noexcept { return document_; }

    /**
     * @brief 校验 + 原子写回 + 更新内存镜像
     *
     * @param next 新配置（通常来自 `current().config` 的副本，命令层只改被改动的字段）
     * @return 写回后生效的有效配置
     * @throw std::invalid_argument 校验失败 / 运行期不可变字段被改动（不写盘，内存不变）
     * @throw std::runtime_error 写盘失败（内存不变，旧文件保持原样）
     */
    EffectiveConfig commit(const BarStrategyConfig& next) {
        // 1. 先全量校验：写回后的文件必须仍能通过加载校验（不把配置写坏）；
        //    today 与构造时一致（注入值优先，否则本机今天），避免写回路径与加载路径口径漂移。
        EffectiveConfig effective =
            resolve_effective_config(next, config_file_internal::resolve_today(options_));

        // 2. 运行期不可变字段守卫（命令层收到错误后提示用户重启生效）：
        //    - max_scale：ADR 0027 决策 11「启动加载一次、盘中不可变」；
        //    - period_sec：启动期架构参数（窗口容量/回放深度/全部实例都按它构建，换周期 = 重启）。
        //    浮点直接比较：值来自同一 JSON 解析路径，精确相等可接受。
        if (effective.config.period_sec != current_.config.period_sec) {
            throw std::invalid_argument(
                std::format("invalid config | field=period_sec value={} expected={}",
                            effective.config.period_sec, current_.config.period_sec));
        }
        if (effective.scaling.max_scale != current_.scaling.max_scale) {
            throw std::invalid_argument(
                std::format("invalid config | field=max_scale value={} expected={}",
                            config_internal::format_ratio(effective.scaling.max_scale),
                            config_internal::format_ratio(current_.scaling.max_scale)));
        }

        // 3. 以当前 document 为基底覆盖已知顶层字段（未知顶层字段 / 省略态保留）
        nlohmann::json document = document_;
        config_file_internal::apply_config(document, next);

        // 4. 原子写盘：失败抛异常，内存镜像保持不变
        write_json_file_atomic(path_, document);

        // 5. 写盘成功后才更新内存（命令先写盘再改内存）
        document_ = std::move(document);
        current_ = std::move(effective);
        return current_;
    }

private:
    std::filesystem::path path_;  ///< 配置文件路径
    nlohmann::json document_;     ///< 文件 document 镜像（含未知顶层字段）
    EffectiveConfig current_;     ///< 生效配置镜像
    ConfigLoadOptions options_;   ///< 注入项（today；commit 重算校验沿用）
};

}  // namespace dztrader

#endif  // DZTRADER_BAR_STRATEGY_CONFIG_FILE_H_
