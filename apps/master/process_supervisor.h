#ifndef DZTRADER_MASTER_PROCESS_SUPERVISOR_H_
#define DZTRADER_MASTER_PROCESS_SUPERVISOR_H_

/**
 * @file process_supervisor.h
 * @brief 进程编排：并行启动、事件通道关闭、重启调度。
 *
 * 所有 ChildProcess 实例通过 shared_ptr 持有。异步回调
 * 捕获 shared_ptr 防止 use-after-free。已停止的子进程
 * 在退出时从 children_ 中移除，防止无限增长。
 */

#include "child_process.h"
#include "orphan_guard.h"
#include "process_registry.h"
#include "shm_manager.h"

#include <dztrader/platform/notify_ui.h>
#include <dztrader/platform/process.h>

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace dztrader::master {

/// 整体关闭逆序分批 (dztraderd 架构「停止与整体关闭」): 策略 → 交易网关 →
/// 行情进程 → dzweb (UI 最后关闭, 可展示关闭进度), 与启动顺序严格互逆。
/// 输入为关闭时刻运行中子进程的 (名字, 类别) 列表, 输出按批次组织的名字列表
/// (空批次不产生)。纯函数, 供单元测试。
std::vector<std::vector<std::string>> build_shutdown_batches(
    const std::vector<std::pair<std::string, Category>>& running);

class ProcessSupervisor {
public:
    using ShutdownCallback = std::function<void()>;

    ProcessSupervisor(boost::asio::io_context& ioc,
                      ProcessRegistry& registry,
                      ShmManager& shm_mgr,
                      OrphanGuard& orphan_guard,
                      int single_stop_timeout_sec = 3,
                      int md_ready_timeout_sec = 5);

    /// 并行启动所有已注册的进程。
    void start_all();

    /// 关闭所有子进程：逆序分批定向 SHUTDOWN → 批超时强制终止 → 进入下一批，全部退出后触发完成回调。
    /// 幂等：可安全多次调用。
    void shutdown();

    /// 强制终止所有仍在运行的子进程 (二次 Ctrl+C 时调用, 避免孤儿)。
    /// 仅调用 child->terminate(), 不等待退出, 不走 shutdown 流程。
    void force_terminate_all();

    /// 设置关闭完成回调（所有子进程停止后触发）。
    void set_shutdown_callback(ShutdownCallback cb);

    /// 动态启动（迭代 2）。
    /// 返回 true 表示进程成功启动 (child->start 成功); false 表示启动失败
    /// (registry 未找到 / launch_child 失败)。调用方据此决定是否持久化 json。
    bool start_process(std::string_view name);

    /// 动态停止（迭代 2）。
    void stop_process(std::string_view name);

    /// 取消指定进程挂起的自动重启（移除/显式停止时调用，进程不运行路径）。
    /// cancel + erase restart_timers_[name] + 清 restart_counts_；幂等（无挂起定时器为 no-op）。
    /// 整体关闭期间的取消已由 shutdown() 全量执行，本方法面向单进程操作。
    void cancel_pending_restart(const std::string& name);

    /// 查询
    std::shared_ptr<ChildProcess> find_child(std::string_view name);
    const std::vector<std::shared_ptr<ChildProcess>>& children() const;

    /// 返回 registry 中所有条目 (用于 QUERY_ALL 回复进程状态)
    const std::vector<ProcessEntry>& registry_entries() const;

    /// 按 name 查找 registry 条目 (用于 PROCESS_CONTROL start 时获取 exe/args 等
    /// 持久化到 dztraderd.json)。未找到返回 nullptr。
    const ProcessEntry* find_registry_entry(std::string_view name) const;

    /// 实时扫描 App Root 下同名可执行文件（转发 ProcessRegistry::find_exe_by_stem）。
    /// 用于 PROCESS_CONTROL start 未注册目标的动态注册场景（契约 process 修订）。
    /// 返回 thread_local 缓冲指针, 跨调用不可用; 未找到返回 nullptr。
    const ProcessEntry* find_exe_by_stem(std::string_view name) const;

    /// 动态注册网关进程（转发 ProcessRegistry::register_gateway）。
    /// 已存在同名条目抛 Exception(DZ_EC_ALREADY_EXISTS)。
    void register_gateway(ProcessEntry entry);

    /// 更新 registry 中某条目的 display_name (用于 PROCESS_CONTROL start 时
    /// 将 dzweb 透传的 display_name 注入 registry, 供后续 send_process_status 查询)。
    /// 内部调用 ProcessRegistry::update_display_name。未找到条目时无操作。
    void update_display_name(std::string_view name, const std::string& display_name);

    /// 更新 registry 条目的运行配置（store apply 回调用；未找到无操作）。
    /// restart 直接以 platform::RestartPolicy 存储（ProcessEntry.restart 同类型）。
    void update_entry_config(const std::string& name,
                             const std::vector<std::string>& args,
                             const std::unordered_map<std::string, std::string>& env,
                             const platform::RestartPolicy& restart,
                             const std::string& display_name);

    /// 移除 registry 条目（store remove 回调用）
    void unregister_entry(const std::string& name);

    /// 发送 RTN_PROCESS_STATUS 帧 (116, 无 instance_id)。
    /// event 缺省 = 自发状态变化 (契约 process); 操作响应由 handle_process_control 显式传 event。
    /// display_name 从 store 读取 (契约 process, store 为配置真相源)
    void send_process_status(const std::string& name,
                             ChildState state,
                             int pid,
                             const std::string& message = "",
                             std::optional<platform::ProcessEvent> event = std::nullopt);

    /// 标记某个 source 进入 remove 流程（在 PROCESS_CONTROL "remove" 分支调用）
    /// on_child_exit 会通过 consume_remove_pending 检查并触发 remove_gateway_section
    void mark_remove_pending(const std::string& name);

    /// 检查并消费 remove_pending 标记（on_child_exit 中调用）
    /// 返回 true 表示此退出属于 remove 流程，调用方应继续执行 remove_gateway_section
    bool consume_remove_pending(const std::string& name);

    /// 兜底路径: 子进程不在运行时, 由 supervisor 完成 remove 流程
    /// (删配置 + 清订阅者 + 推 stopped + consume_remove_pending)
    /// 当子进程不在运行时, on_child_exit 不会被触发, 但 remove_pending 已挂上,
    /// 需要在 supervisor 层完成剩余步骤 (而非 shm_manager 直接推送 stopped, 绕过层次划分)
    /// 行为契约: 兜底路径, 子进程未运行时使用. 与 on_child_exit remove 分支功能近似等价:
    ///   - 订阅者清理因无 pid 用 entry name 兜底 (remove_reader 幂等)
    ///   - 状态推送 pid=-1, message="removed (process not running)" 区分进程未运行场景
    ///   - 重启相关不适用
    /// category 由调用方在配置删除前取得 (store remove 的 apply 回调删除 registry 条目后
    /// 再 find 恒为 nullptr, 无法在本函数内查询)
    void notify_removed_for_inactive(const std::string& name, Category category);

    /// 是否正在关闭中？
    bool is_shutting_down() const;

    /// md 通道就绪回调 (由 ShmManager::mark_md_channel_ready 在收到
    /// NOTIFY_MD_STARTED 后调用): 启动该源全部 pending 策略并清空 pending,
    /// 取消该源 ready watchdog。
    void on_md_channel_ready(const std::string& source);

    /// 查询策略是否处于 pending (等待行情源 ready)。
    /// 命中时 message 输出 pending 原因 (供 report_full_snapshot 保留 Starting 状态)。
    [[nodiscard]] bool is_pending_strategy(const std::string& name, std::string& message) const;

private:
    /// 启动子进程。返回 true 表示成功启动; false 表示启动失败
    /// (重复启动 / exe 未找到 / child->start 失败)。
    /// exe 为空时调 find_exe_by_stem 实时扫描填充 (扫描结果不回写 registry)。
    /// 失败时: 失败路径 B/D 推送 crashed + notify_ui, 失败路径 B 不安排 restart。
    bool launch_child(const ProcessEntry& entry);

    /// 运行期动态启动入口 (PROCESS_CONTROL start / 晚到 STARTED 补启动):
    /// - Strategy: 绑定源 ready 则 pre-register reader + launch_child;
    ///   源不存在或未 ready 则进 pending (反馈"等待行情源")。
    /// - 其他类别: 直接 launch_child。
    /// 返回 true = 已 spawn (成功); false = 未 spawn (失败/pending/已在运行)。
    bool launch_registered_process(const ProcessEntry& entry);
    void on_child_exit(std::shared_ptr<ChildProcess> child,
                       boost::system::error_code ec, int exit_code);
    void schedule_restart(const std::string& name,
                          const ProcessEntry& entry, int restart_count);
    void remove_child(const std::shared_ptr<ChildProcess>& child);
    bool all_children_stopped() const;
    void check_shutdown_complete();

    /// 单进程停止：发送定向 SHUTDOWN + 启动超时定时器
    void stop_single_child(std::shared_ptr<ChildProcess> child);

    /// 单进程停止超时回调：子进程未退出则强制 terminate
    /// name 由调用方从 lambda 捕获传入 (即 stop_single_child 中捕获的 name 副本),
    /// 用于从 single_stop_timers_ 中 erase 对应 entry, 不依赖 child 仍存活
    void on_single_stop_timeout(std::shared_ptr<ChildProcess> child,
                                 const boost::system::error_code& ec,
                                 const std::string& name);

    /// 向当前批次全部成员定向发送 SHUTDOWN; 批次已全停止则推进下一批
    void send_current_shutdown_batch();

    /// 当前批次超时: 强制终止仍在运行的批次成员 (退出回调随后推进批次)
    /// armed_index = 排定该 timer 时的批次下标, 防过期 timer 在批次推进后误杀新批次
    void force_terminate_batch(size_t armed_index);

    /// 批次推进: 当前批次全部退出后进入下一批 (由 on_child_exit 触发, 事件驱动)
    void advance_shutdown_batch();

    boost::asio::io_context& ioc_;
    ProcessRegistry& registry_;
    ShmManager& shm_mgr_;
    dztrader::platform::NotifyUi notify_ui_;
    OrphanGuard& orphan_guard_;
    std::vector<std::shared_ptr<ChildProcess>> children_;
    bool shutting_down_ = false;

    /// 每个进程的重启尝试计数
    std::unordered_map<std::string, int> restart_counts_;

    /// 退避定时器（重启调度用）
    std::unordered_map<std::string,
        std::unique_ptr<boost::asio::steady_timer>> restart_timers_;

    /// 关闭时当前批次的强制终止定时器 (逐批复用, 批次完成即取消)
    std::unique_ptr<boost::asio::steady_timer> shutdown_timer_;

    /// 整体关闭批次 (shutdown 时冻结) 与当前批次下标
    std::vector<std::vector<std::string>> shutdown_batches_;
    size_t shutdown_batch_index_ = 0;

    /// 单进程停止的强制终止定时器集合 (按 name 索引, 支持多并发 stop)
    /// 旧实现是单值 single_stop_timer_ + single_stop_target_, 快速连续 stop 两个进程时,
    /// 第二次赋值会销毁第一个 timer, 第一个进程若卡死则永远无法 force-kill (变僵尸)。
    /// 改为 map 后, 每个被 stop 的进程都有独立的超时定时器。
    std::unordered_map<std::string,
        std::unique_ptr<boost::asio::steady_timer>> single_stop_timers_;

    /// 进入 remove 流程的 source 集合（区别于 stop：remove 需要 on_child_exit 后删配置）
    /// 由 mark_remove_pending 设置, 由 consume_remove_pending 消费
    /// 与 single_stop_timers_ 一样在 io_context 线程中串行访问, 无需加锁
    std::unordered_set<std::string> remove_pending_;

    /// 未就绪 (md 未 ready) 的策略: source -> 策略名列表。
    /// 启动编排超时后与运行期动态启动 (绑定源未就绪) 时加入;
    /// on_md_channel_ready 收到晚到 STARTED 后启动全部并清空。
    std::unordered_map<std::string, std::vector<std::string>> pending_strategies_;

    /// md ready watchdog 定时器: source -> timer。
    /// 每次 spawn md 进程后启动, 收到对应 NOTIFY_MD_STARTED 时取消 (on_md_channel_ready)。
    /// 超时行为按"是否存在运行中绑定策略"区分 (见 on_md_ready_timeout)。
    std::unordered_map<std::string, std::unique_ptr<boost::asio::steady_timer>> md_ready_timers_;

    /// md ready 超时回调 (watchdog): source 在 md_ready_timeout_sec_ 内未 ready 时触发
    void on_md_ready_timeout(const std::string& source,
                             const boost::system::error_code& ec);

    /// 每次 spawn md 进程后启动 ready watchdog 定时器 (契约 4.5)
    void arm_md_ready_watchdog(const std::string& source);

    /// 启动绑定 source 且未在运行的全部策略 (pre-register reader + spawn)。
    /// 供启动编排与 on_md_channel_ready 共用。
    void start_strategies_for_source(const std::string& source);

    /// 策略 spawn 前预注册 md 读者 (契约 4.3): add_reader(stg.<name>) +
    /// 通知 md 进程刷新订阅者缓存。
    void pre_register_strategy_reader(const std::string& strategy_name,
                                      const std::string& md_source);

    /// start_all 第三趟: 同步有界等待被至少一个策略引用的 md ready,
    /// 每个 ready 源启动其绑定策略 (add_reader + spawn); 超时后未 ready 源的
    /// 绑定策略进 pending (Starting + message)。
    void wait_for_md_ready_and_start_strategies();

    /// 查询绑定某 source 的全部策略名 (来自 registry, 含未启动项)
    std::vector<std::string> strategies_bound_to(const std::string& source) const;

    /// 被强制终止 (force kill) 的子进程名称集合
    /// 在 on_single_stop_timeout 中加入, 在 on_child_exit 中读取后清除
    /// 用于跳过 crash 分支的 notify_ui, 避免与 on_single_stop_timeout 中的
    /// "force killing" notify_ui 形成双弹窗 (且第二个 "crashed" 文案与实际场景不符)
    /// 只在 io_context 线程中访问 (与 remove_pending_ 一样无需加锁)
    std::unordered_set<std::string> force_killed_names_;

    /// 关闭完成回调
    ShutdownCallback shutdown_callback_;

    /// 单进程停止超时秒数 (可配置, 由构造函数从 dztraderd.json [master].single_stop_timeout_sec 注入)
    int single_stop_timeout_sec_ = 3;

    /// md ready 等待超时秒数 (可配置, 由构造函数从 dztraderd.json [master].md_ready_timeout_sec 注入)
    int md_ready_timeout_sec_ = 5;
};

}  // namespace dztrader::master

#endif  // DZTRADER_MASTER_PROCESS_SUPERVISOR_H_
