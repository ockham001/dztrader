#ifndef DZTRADER_PLATFORM_LOG_CONFIG_H_
#define DZTRADER_PLATFORM_LOG_CONFIG_H_

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <dztrader/platform/frame_codec.h>
#include <dztrader/shm/writer.h>

namespace dztrader::platform {

/// 日志配置通道类，封装 SET_LOG_CONFIG / RTN_LOG_CONFIG 两帧的接收处理与发送。
/// 持有当前配置镜像，与 spdlog 实时同步，与配置文件持久化同步。
///
/// 职责边界：类只负责配置的加载/修改/持久化/应用/发送 RTN。
/// rtn 的时机和次数由外部控制--类内部不自己调 rtn_log_config()，
/// 避免重复 rtn。SET 失败的日志+NOTIFY_UI 也由外部 catch 块处理。
///
/// 线程安全：非线程安全。实例方法（load/set_log_config/rtn_log_config）
/// 不可并发调用，调用方需保证同实例仅在同一线程访问。
class LogConfig {
public:
    /// @param instance_id  进程名，作 RTN_LOG_CONFIG 的帧头 instance_id
    /// @param cfg_path     持久化路径
    /// @param section      JSON 路径,默认 "/log";空路径 "" 表示 root
    LogConfig(std::string instance_id,
              std::filesystem::path cfg_path,
              nlohmann::json::json_pointer section = nlohmann::json::json_pointer("/log"));
    // 构造后 cfg_ 初始化为 default_cfg()，即使不调 load() 也有合理默认值

    // 禁拷贝/移动：持有引用成员, 绑定后终身不变, 不应放入容器或按值传递
    LogConfig(const LogConfig&) = delete;
    LogConfig& operator=(const LogConfig&) = delete;
    LogConfig(LogConfig&&) = delete;
    LogConfig& operator=(LogConfig&&) = delete;

    ~LogConfig() = default;

    /// 启动时加载配置。失败用默认值并修复文件（自愈），与 spdlog 实时同步。不抛。
    /// 注意：本方法不发送 RTN，调用者需自行决定是否调 rtn_log_config() 上报。
    void load();

    /// 接收处理 SET_LOG_CONFIG。
    /// 手动合并 patch 的 level/flush_on 字段（忽略其他字段，兼容性优先）。
    /// 成功：检查 patch → 合并 → validate → save → apply → 更新 cfg_，与 spdlog 同步。
    /// 失败：抛 std::runtime_error，内部 cfg_ 不变（强保证）。
    /// 空对象 {} 视为无操作，仍走完整流程（不改变 cfg_）。
    /// 注意：本方法不发送 RTN，也不做 NOTIFY_UI——由调用者 catch 后处理。
    void set_log_config(const nlohmann::json& patch);

    /// 发送 RTN_LOG_CONFIG 帧。始终全量当前 cfg_，无 error 字段（契约要求）。
    /// 非 const：内部写共享内存（event_writer 有副作用）。
    /// 注意：rtn 的时机和次数完全由外部控制，类内部不自动调用本方法。
    void rtn_log_config(shm::MultiWriter& event_writer);

    /// 返回当前生效配置（{ "level": "...", "flush_on": "..." }）。只读，无副作用。
    const nlohmann::json& current() const noexcept { return cfg_; }

    // ===== 静态工具函数（所有 level 校验/解析的唯一真相源）=====
    // 契约 log: 有效值为 spdlog 规范全称（全小写，大小写敏感）：
    // trace/debug/info/warning/error/critical/off。
    // warn/err 为 spdlog from_str 内置快捷方式，作为输入别名接受但存储/RTN 时规范化为 warning/error。
    // from_str 对未知串返回 off，与合法 "off" 歧义，需显式区分。空串视为非法。

    /// 解析 level 字符串为 spdlog 级别。非法（空串或未知串）返回 std::nullopt。
    static std::optional<spdlog::level::level_enum> parse_level(std::string_view s) noexcept;

    /// 校验 level 字符串是否合法。等价于 parse_level(s).has_value()。
    static bool is_valid_level(std::string_view s) noexcept { return parse_level(s).has_value(); }

    /// 将 level 字符串规范化为 spdlog 全称（warn->warning, err->error）。
    /// 调用前应已通过 is_valid_level 校验；未校验时原样返回。
    static std::string canonicalize_level(std::string_view s) noexcept;

    /// 规范化 cfg 中的 level/flush_on 为 spdlog 全称（原地修改）。
    static void canonicalize(nlohmann::json& cfg) noexcept;

    /// 校验合并后的完整配置 json (含 level + flush_on) 是否合法。
    /// 返回错误消息或 nullopt。供外部接收方 (如 webui) 复用同一套校验规则。
    static std::optional<std::string> validate(const nlohmann::json& cfg);

    /// 应用配置到 spdlog (set_level + flush_on)。noexcept, 供外部接收方 (如 webui) 复用。
    static void apply_to_spdlog(const nlohmann::json& cfg) noexcept;

private:
    std::string instance_id_;
    std::filesystem::path cfg_path_;
    nlohmann::json::json_pointer section_;
    nlohmann::json cfg_;  // { "level": "...", "flush_on": "..." }

    // 默认配置 {"level":"debug","flush_on":"info"}
    static nlohmann::json default_cfg();

    // 按 section_ 从文件读取，文件不存在/section 缺失返回默认值，JSON 解析失败抛。
    // degraded=true 表示文件内容非法/不可读，调用方应修复文件并记日志
    [[nodiscard]] nlohmann::json load_cfg_from_file(bool& degraded) const;

    // 按 section_ 原子写入文件（load-modify-save 保留其他 section），失败抛
    void save_cfg_to_file(const nlohmann::json& cfg) const;
};

// ===== 内联实现 =====

inline nlohmann::json LogConfig::default_cfg() {
    return {{"level", "debug"}, {"flush_on", "info"}};
}

inline void LogConfig::rtn_log_config(shm::MultiWriter& event_writer) {
    // 始终全量当前 cfg_，无 error 字段（契约 log：RTN 始终全量、无 error 字段）
    // 非 const：写共享内存有副作用
    write_ext_inst_json_obj(event_writer, DZ_FRAME_RTN_LOG_CONFIG, instance_id_, cfg_);
}

}  // namespace dztrader::platform

#endif  // DZTRADER_PLATFORM_LOG_CONFIG_H_
