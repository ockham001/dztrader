#ifndef DZTRADER_PLATFORM_AUTO_LOGIN_H_
#define DZTRADER_PLATFORM_AUTO_LOGIN_H_

#include <filesystem>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include <dztrader/platform/frame_codec.h>
#include <dztrader/shm/writer.h>

namespace dztrader::platform {

/// 自动登录/登出排程配置通道类，封装 SET_AUTO_LOGIN / RTN_AUTO_LOGIN 两帧的接收处理与发送。
/// 持有当前配置镜像，与配置文件持久化同步。
///
/// 职责边界（与 LogConfig 一致）：类负责配置的加载/修改/持久化/发送 RTN。
/// rtn 的时机和次数由外部控制——类内部不自己调 rtn_auto_login()，避免重复 rtn。
/// SET 失败的日志 + NOTIFY_UI 也由外部 catch 块处理。
///
/// 严格遵循 docs/frame_contracts/auto-login.md 契约：
///   - SET 纯 RFC 7386 JSON Merge Patch（enabled 出现则覆盖，schedules 出现则整体覆盖）
///   - 任何位置 null 均校验失败（本帧无 map 字段）
///   - login_time / logout_time 为 "HH:MM"（00:00-23:59）本地时间（计算机时钟），契约不做时区预测
///   - login_time == logout_time 非法（会话区间 [login, logout) 必须非空）
class AutoLoginConfig {
public:
    /// @param instance_id  网关进程名，作 RTN_AUTO_LOGIN 的帧头 instance_id
    /// @param cfg_path     持久化路径
    /// @param event_writer 共享内存写入器
    /// @param section      JSON 路径，默认 "/auto_login"；空路径 "" 表示 root
    AutoLoginConfig(std::string instance_id,
                    std::filesystem::path cfg_path,
                    shm::MultiWriter& event_writer,
                    nlohmann::json::json_pointer section = nlohmann::json::json_pointer("/auto_login"));
    // 构造后 cfg_ 初始化为 default_cfg()，即使不调 load() 也有合理默认值

    // 禁拷贝/移动：持有引用成员，绑定后终身不变，不应放入容器或按值传递
    AutoLoginConfig(const AutoLoginConfig&) = delete;
    AutoLoginConfig& operator=(const AutoLoginConfig&) = delete;
    AutoLoginConfig(AutoLoginConfig&&) = delete;
    AutoLoginConfig& operator=(AutoLoginConfig&&) = delete;

    ~AutoLoginConfig() = default;

    /// 启动时加载配置。失败用默认值并修复文件（自愈），不抛、不发 RTN。
    /// 调用者需自行决定是否调 rtn_auto_login() 上报。
    void load();

    /// 接收处理 SET_AUTO_LOGIN（RFC 7386 JSON Merge Patch）。
    /// 成功：合并 patch -> validate -> save -> 更新 cfg_。
    /// 失败：抛 std::runtime_error，内部 cfg_ 不变（强保证）。
    /// 空对象 {} 视为无操作，仍走完整流程（不改变 cfg_）。
    /// 本方法不发 RTN，也不做 NOTIFY_UI——由调用者 catch 后处理。
    void set_auto_login(const nlohmann::json& patch);

    /// 发送 RTN_AUTO_LOGIN 帧。始终全量当前 cfg_（契约要求）。
    /// 非 const：内部写共享内存有副作用。
    /// rtn 的时机和次数完全由外部控制，类内部不自动调用本方法。
    void rtn_auto_login() {
        write_ext_inst_json_obj(event_writer_, DZ_FRAME_RTN_AUTO_LOGIN, instance_id_, cfg_);
    }

    /// 只读访问当前配置镜像（调度器消费用）。
    /// 返回 json：{"enabled": bool, "schedules": [{login_time, logout_time}, ...]}
    const nlohmann::json& config() const noexcept { return cfg_; }

    // ===== 静态工具函数（所有校验的唯一真相源）=====

    /// 判断字符串是否为合法 "HH:MM"（00:00-23:59）。
    static bool is_hh_mm(const std::string& s) noexcept;

    /// 校验单条排程（json 元素）。返回错误消息或 nullopt。
    static std::optional<std::string> validate_schedule(const nlohmann::json& s);

    /// 校验合并后的完整配置 json（含 enabled + schedules）。
    /// 返回错误消息或 nullopt。供外部接收方（如 dzweb）复用同一套校验规则。
    static std::optional<std::string> validate(const nlohmann::json& cfg);

private:
    std::string instance_id_;
    std::filesystem::path cfg_path_;
    shm::MultiWriter& event_writer_;
    nlohmann::json::json_pointer section_;
    nlohmann::json cfg_;  // { "enabled": bool, "schedules": [...] }

    // 默认配置 {"enabled": true, "schedules": []}
    static nlohmann::json default_cfg() {
        return {{"enabled", true}, {"schedules", nlohmann::json::array()}};
    }

    // 按 section_ 从文件读取，文件不存在/section 缺失返回默认值，JSON 解析失败抛。
    // degraded=true 表示文件内容非法/不可读，调用方应修复文件并记日志
    [[nodiscard]] nlohmann::json load_cfg_from_file(bool& degraded) const;

    // 按 section_ 原子写入文件（load-modify-save 保留其他 section），失败抛
    void save_cfg_to_file(const nlohmann::json& cfg) const;
};

}  // namespace dztrader::platform

#endif  // DZTRADER_PLATFORM_AUTO_LOGIN_H_
