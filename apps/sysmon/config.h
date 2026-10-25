#ifndef DZTRADER_SYSMON_CONFIG_H_
#define DZTRADER_SYSMON_CONFIG_H_

/**
 * @file config.h
 * @brief dzsysmon 配置：默认生成、加载合并、运行期 patch 与持久化（契约 sysmon §3）。
 *
 * 配置文件为 `$DZTRADER_HOME/configs/dzsysmon.json`，同时承载两段内容：
 * - `log` section：日志配置（契约 log，由 platform::LogConfig 独立加载/持久化）
 * - 六个业务配置段：sampling/disk/network/history/realtime/thresholds（platform::SysmonConfig）
 *
 * 仓库不提交模板文件（AGENTS 硬门禁 6）：进程缺失时自动生成默认值；运行期 SET 走
 * load-modify-save 整文件原子写，**必须保留** `log` 与任何未知 section。
 *
 * **旧配置迁移**（契约 sysmon §3.1）：旧版本把阈值存在 `alerts` 段。读到该段时把可映射的
 * 键搬到 `thresholds`、丢弃不可映射的 `volume_free_bytes` 与已删除的 `cooldown_sec`、
 * 写回文件并记一条日志——**不报错、不阻塞启动**（现网配置文件直接报错会让进程起不来）。
 */

#include <filesystem>
#include <string>
#include <vector>

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
 * @brief 旧配置段迁移结果：是否需要改写文件 + 迁移了哪些键。
 *
 * 只报告**外部可观察**的事实（哪些键搬走了、哪些键丢弃了），供调用方记一条日志；
 * 不暴露迁移的内部实现细节。
 */
struct ConfigMigration {
    /// 是否存在旧 `alerts` 段（true 表示文件需要改写为新的 `thresholds` 形状）。
    bool migrated = false;
    /// 成功映射到 `thresholds` 的键（按 `thresholds` 段的键序）。
    std::vector<std::string> mapped_keys;
    /// 因不再支持而丢弃的键（如 `volume_free_bytes` / `cooldown_sec`），按原文件顺序。
    std::vector<std::string> dropped_keys;
};

/**
 * @brief 把整文件 JSON 里的旧 `alerts` 段迁移为 `thresholds` 段（原地修改）。
 *
 * 语义（契约 sysmon §3.1）：
 * - 只搬**可映射**的键（`cpu_sustain_used_pct` / `cpu_sustain_samples` / `memory_used_pct` /
 *   `swap_used_pct` / `volume_free_pct`）；已存在的新 `thresholds` 段里的同名键优先，
 *   不被旧值覆盖（用户可能已经手工写好了新形状）；
 * - `volume_free_bytes`（绝对容量形态已删除）与 `cooldown_sec`（通知冷却已删除）**丢弃**；
 * - 旧段里的未知键一并丢弃，不搬到新段（未知字段忽略是总则 §8 的通用约定）；
 * - 无论迁移成功与否都删除旧段（保留它会让下次启动重复迁移）。
 *
 * @param full 整文件 JSON（含 log 与未知 section；原地修改）
 * @return 迁移结果；文件里没有旧段时 `migrated = false` 且不做任何修改
 */
[[nodiscard]] ConfigMigration migrate_legacy_alerts(nlohmann::json& full);

/**
 * @brief 加载配置。
 *
 * 流程：文件缺失先自愈生成默认文件 → 读文件 → **旧 `alerts` 段迁移**（命中时写回文件
 * 并记一条日志）→ 以默认配置为基底 merge_patch（RFC 7386）→ 仅保留六个业务配置段
 * → platform::SysmonConfig::validate 校验 → 经 SysmonConfig 结构体往返归一化
 * （剔除未知字段）。
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
