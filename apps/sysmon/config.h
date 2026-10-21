#ifndef DZTRADER_SYSMON_CONFIG_H_
#define DZTRADER_SYSMON_CONFIG_H_

/**
 * @file config.h
 * @brief dzsysmon 配置：默认生成、加载合并、运行期 patch 与持久化（契约 sysmon §3）。
 *
 * 配置文件为 `$DZTRADER_HOME/configs/dzsysmon.json`，同时承载两段内容：
 * - `log` section：日志配置（契约 log，由 platform::LogConfig 独立加载/持久化）
 * - 六个业务配置段：sampling/disk/network/history/realtime/alerts（platform::SysmonConfig）
 *
 * 仓库不提交模板文件（AGENTS 硬门禁 6）：进程缺失时自动生成默认值；运行期 SET 走
 * load-modify-save 整文件原子写，**必须保留** `log` 与任何未知 section。
 */

#include <filesystem>

#include <nlohmann/json.hpp>

namespace dztrader::sysmon {

/// 全量默认配置 JSON：`log` section + platform::SysmonConfig 的六个配置段（不写盘）。
/// log 默认值取 `{level:"info", flush_on:"warning"}`（与 LogConfig 的调试默认值区分：
/// 这是生产进程的落盘默认值）。
[[nodiscard]] nlohmann::json default_config_json();

/**
 * @brief 生成默认配置文件。
 *
 * 文件已存在时不覆盖；父目录不存在时自动创建。
 *
 * @param path 配置文件路径
 * @throws std::runtime_error 目录创建/文件写入失败（英文消息、小写开头、无句号）
 */
void generate_default_config(const std::filesystem::path& path);

/**
 * @brief 加载配置。
 *
 * 流程：文件缺失先自愈生成默认文件 → 读文件 → 以默认配置为基底 merge_patch
 * （RFC 7386）→ 仅保留六个业务配置段 → platform::SysmonConfig::validate 校验
 * → 经 SysmonConfig 结构体往返归一化（剔除未知字段）。
 *
 * @param path 配置文件路径
 * @return 校验通过的全量配置 JSON（仅六个业务配置段）
 * @throws std::runtime_error 文件读取/解析/校验失败（英文消息、小写开头、无句号）
 */
[[nodiscard]] nlohmann::json load_config(const std::filesystem::path& path);

/**
 * @brief 运行期配置仓库：内存镜像 + 持久化。
 *
 * 持久化采用 load-modify-save 整文件原子写（tmp + rename）：只替换六个业务配置段，
 * `log` section 与其它未知 section 原样保留；写入失败不留下半截文件。
 *
 * 线程安全：非线程安全。仅在 io_context 线程访问（帧处理与定时器回调同线程）。
 */
class SysmonConfigStore {
public:
    /**
     * @param path    配置文件路径
     * @param current 已校验的全量配置 JSON（通常是 load_config 的结果）
     */
    SysmonConfigStore(std::filesystem::path path, nlohmann::json current);

    SysmonConfigStore(const SysmonConfigStore&) = delete;
    SysmonConfigStore& operator=(const SysmonConfigStore&) = delete;
    SysmonConfigStore(SysmonConfigStore&&) = delete;
    SysmonConfigStore& operator=(SysmonConfigStore&&) = delete;

    ~SysmonConfigStore() = default;

    /// 当前生效的全量配置（仅六个业务配置段）。只读、无副作用。
    [[nodiscard]] const nlohmann::json& current() const noexcept { return current_; }

    /**
     * @brief 应用 RFC 7386 merge patch：校验 → 持久化 → 更新内存镜像。
     *
     * 顺序保证强异常安全：apply_patch 失败或持久化失败时内存镜像不变（回滚旧值）。
     *
     * @param patch 增量 patch（必须为 object；任意位置 null 非法）
     * @throws std::runtime_error 校验/持久化失败（英文消息、小写开头、无句号）
     */
    void set_patch(const nlohmann::json& patch);

private:
    /// load-modify-save 整文件原子写：保留 log 与未知 section。
    /// @throws std::runtime_error 读取/解析/写入/rename 失败
    void save(const nlohmann::json& cfg) const;

    std::filesystem::path path_;
    nlohmann::json current_;
};

}  // namespace dztrader::sysmon

#endif  // DZTRADER_SYSMON_CONFIG_H_
