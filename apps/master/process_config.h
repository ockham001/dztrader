#ifndef DZTRADER_MASTER_PROCESS_CONFIG_H_
#define DZTRADER_MASTER_PROCESS_CONFIG_H_

#include <dztrader/platform/process.h>
#include <dztrader/shm/writer.h>

#include <functional>
#include <map>
#include <string>

namespace dztrader::master {

/// 进程配置存储类，封装 SET_PROCESS_CONFIG / RTN_PROCESS_CONFIG 两帧的接收处理与发送。
/// 持有全量配置镜像（name → ProcessConfig），与 dztraderd.json 持久化同步（经注入回调）。
///
/// 职责边界（与 LogConfig 一致）：类只负责配置的加载/修改/持久化/发送 RTN。
/// rtn 的时机和次数由外部控制——类内部不自己调 rtn_process_config()，
/// 避免重复 rtn。SET 失败的日志+NOTIFY_UI 也由外部 catch 块处理。
class ProcessConfigStore {
public:
    /// 持久化/应用回调。full 为 null 表示删除该进程条目。
    /// @param name 进程名
    /// @param full 该进程的全量 ProcessConfig JSON（null = 删除）
    using PersistFn = std::function<void(const std::string& name, const nlohmann::json& full)>;

    /// @param event_writer 共享内存写入器（发 RTN_PROCESS_CONFIG）
    /// @param persist_fn   持久化回调（ShmManager 注入：写/删 dztraderd.json 对应段）
    /// @param apply_fn     应用回调（ShmManager 注入：更新 ProcessEntry 内存）
    ProcessConfigStore(shm::MultiWriter& event_writer, PersistFn persist_fn, PersistFn apply_fn);

    // 禁拷贝/移动：持有引用成员, 绑定后终身不变（与 LogConfig/ShmConfigBase 一致）
    ProcessConfigStore(const ProcessConfigStore&) = delete;
    ProcessConfigStore& operator=(const ProcessConfigStore&) = delete;
    ProcessConfigStore(ProcessConfigStore&&) = delete;
    ProcessConfigStore& operator=(ProcessConfigStore&&) = delete;
    ~ProcessConfigStore() = default;

    /// 启动初始化：以全量 map（name → ProcessConfig 全量 JSON）填充镜像。
    /// initial_map 由调用方（master）从 ProcessRegistry 构建（entry → wire 转换属阶段 2 编排）。
    /// 非法条目用 validate_process_config_full 过滤并记日志（跳过），不抛。
    /// 注意：本方法不发送 RTN，调用者自行决定是否调 rtn_process_config()。
    void load(const nlohmann::json& initial_map);

    /// 接收处理 SET_PROCESS_CONFIG 的 config patch（增量角色）。
    /// 流程：target 存在性检查 → validate_process_config_patch
    ///       → apply_process_config_patch（副本）→ persist_fn → apply_fn → 更新镜像。
    /// 失败：target 未注册（契约第 225 行）或校验失败或回调失败 → 抛 std::runtime_error，
    ///       内部镜像不变（强保证）。
    /// 注意：persist_fn 成功而 apply_fn 失败时 dztraderd.json 已写入但镜像未更新
    /// （两个外部副作用的固有窗口），下次启动 load 以文件为准收敛。
    /// 空对象 {} 视为无操作，仍走完整流程（不改变镜像）。
    /// 本方法不发 RTN，也不做 NOTIFY_UI——由调用者 catch 后处理。
    void set_process_config(const std::string& target, const nlohmann::json& patch);

    /// 动态注册新进程条目（PROCESS_CONTROL start 未注册目标扫描命中时, 契约 03 修订）。
    /// 流程: validate_process_config_full → persist_fn → apply_fn → 写入镜像。
    /// 目标已存在或校验失败或回调失败: 抛 std::runtime_error, 镜像不变（强保证）。
    /// 注意: persist_fn 成功而 apply_fn 失败时 dztraderd.json 已写入但镜像未更新
    /// （与 set_process_config 相同的固有窗口）, 下次启动 load 以文件为准收敛。
    /// 本方法不发 RTN。
    void register_process(const std::string& name, const nlohmann::json& full_config);

    /// 删除进程条目（Remove 流程用）：persist_fn(name, null) + apply_fn(name, null) + 镜像删除。
    /// 目标不存在：抛 std::runtime_error（调用方按 RemoveFailed 处理）。
    /// 本方法不发 RTN。
    void remove(const std::string& name);

    /// 发送 RTN_PROCESS_CONFIG 帧。始终全量镜像（契约第 236 行），无 error 字段。
    /// 非 const：内部写共享内存。rtn 时机完全由外部控制。
    void rtn_process_config();

    /// 只读访问某进程的全量配置。未注册返回 nullptr。
    [[nodiscard]] const nlohmann::json* find(const std::string& name) const;

private:
    std::map<std::string, nlohmann::json> cfg_map_;  ///< 全量镜像（始终全量）
    shm::MultiWriter& event_writer_;
    PersistFn persist_fn_;
    PersistFn apply_fn_;
};

}  // namespace dztrader::master

#endif  // DZTRADER_MASTER_PROCESS_CONFIG_H_
