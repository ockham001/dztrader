#ifndef DZTRADER_MASTER_SHM_MANAGER_H_
#define DZTRADER_MASTER_SHM_MANAGER_H_

/**
 * @file shm_manager.h
 * @brief 共享内存元数据管理。
 *
 * 创建 ChannelMeta 文件，管理订阅者列表（仅 master 写入），
 * 处理旧页面文件的定期清理，读写 event channel 控制帧。
 */

#include "config.h"
#include "process_config.h"

#include <dztrader/platform/log_config.h>
#include <dztrader/platform/notify_ui.h>
#include <dztrader/platform/process.h>
#include <dztrader/platform/shm_config.h>
#include <dztrader/shm/channel_meta.h>
#include <dztrader/shm/frame_view.h>
#include <dztrader/shm/named_semaphore.h>
#include <dztrader/shm/page_cleaner.h>
#include <dztrader/shm/reader.h>
#include <dztrader/shm/writer.h>

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace dztrader::master {

class ProcessSupervisor;  // 前向声明, 避免 .h 循环 include

/// md 通道运行态: 元数据句柄 + 就绪标志。
/// meta 为 null 表示通道已关闭 (行情进程停止的后果): 数据文件/读取位置/page_size
/// 保留待重启复用 (create_md_channel 重建时 page_size 不一致自动重置)。
/// ready = 行情进程已广播 NOTIFY_MD_STARTED (通道就绪, 可接受读者接入)。
struct MdChannelState {
    std::shared_ptr<shm::ChannelMeta> meta;
    bool ready = false;
};

class ShmManager {
public:
    // ===== 生命周期与通道创建 =====
    /// 显式构造: 创建事件通道 + writer/reader/cleaner + 信号量, 失败抛异常 (RAII)
    /// log_config_ 在构造函数体内从 cfg_path 的 "log" section 加载 (失败用默认值自愈)
    /// shm_global: SHM 全局参数 (meta_file_size,启动后不变)
    /// cfg_path: dztraderd.json 路径 (用于 SET_LOG_CONFIG/SET_EVENT_SHM_CONFIG 持久化)
    /// cleanup_policy: 旧页清理策略 (event + 全部 md 通道共用, 双 0 不清理;
    /// 默认与旧硬编码行为一致 200/24)
    /// name: 进程名 (与 exe stem 一致, 默认 "dztraderd")
    ShmManager(const ShmGlobalConfig& shm_global,
               const std::filesystem::path& cfg_path,
               shm::CleanupPolicy cleanup_policy =
                   shm::CleanupPolicy{.max_page_count = 200, .max_page_age_hours = 24},
               std::string name = "dztraderd");

    ~ShmManager() = default;

    ShmManager(const ShmManager&) = delete;
    ShmManager& operator=(const ShmManager&) = delete;
    ShmManager(ShmManager&&) = delete;
    ShmManager& operator=(ShmManager&&) = delete;

    /// 创建 md 通道元数据 (启动每个 md 进程前调用)。
    /// page_size 按优先级: read_md_page_size(source_name) -> kDefaultMdPageSize
    /// (新协议不再有 UI 透传的 page_size_override)
    /// 创建后立即 clear_readers 清空订阅者列表 (行情通道订阅者由策略/数据进程
    /// 自行注册,master 不订阅行情通道)。launch_child 之后应调用
    /// notify_md_channel_subscriber_update 通知行情进程刷新 spi_.writer_ 缓存。
    void create_md_channel(std::string_view source_name);

    /// 关闭 md 通道 (行情进程停止的后果, dztraderd 架构): 清空读者列表 +
    /// 释放元数据句柄 + 复位就绪标志。不触碰数据文件/读取位置/page_size
    /// (保留待重启复用); 条目保留在 md_channels_ 表示通道已配置。
    /// 由 ProcessSupervisor::on_child_exit 在 md 进程退出时调用。
    void close_md_channel(std::string_view source_name);

    /// 标记 md 通道就绪 (收到行情进程 NOTIFY_MD_STARTED 广播时调用)。
    /// 未知通道或已关闭通道忽略。
    void mark_md_channel_ready(std::string_view source_name);

    /// 注入 ProcessSupervisor 引用 (用于 PROCESS_CONTROL 帧调用启停)
    void set_supervisor(ProcessSupervisor* supervisor);

    /// 释放所有映射（master 关闭时调用）。
    void release_all();

    // ===== 周期任务与事件监听 =====
    /// 启动周期任务：60s PageCleaner 清理 + event channel 信号量监听线程。
    /// 必须在 io_context::run() 之前调用。
    void start_periodic_tasks(boost::asio::io_context& ioc);

    /// 启动 event 通道维护周期任务
    /// 周期: event_shm_config_.check_interval_min 分钟
    /// 每次执行:
    ///   1. event_writer_.prefetch_pages(check_pages) - master 自己写的页预热
    ///   2. event_writer_.prefetch_for_bytes(check_bytes) - 按字节预热
    ///   3. cleaner_->cleanup() + md 通道 cleanup - 清理旧页
    ///   4. event_writer_.touch_write_position() - 触页防 swap
    ///   5. 广播 DZ_FRAME_PRELOAD_EVENT_SHM (通知子进程预加载 event 通道)
    void start_event_shm_maintenance(boost::asio::io_context& ioc);

    /// 重新调度 event 通道维护任务 (check_interval_min 变化时调用)
    /// 不使用 cancel+wait 模式 (recycle 的 if(ec)return 会终止链), 手动重启 recycle lambda
    void reschedule_event_shm_maintenance();

    /// preload_points 时间点检查 (每分钟触发, 匹配 HH:MM 时执行额外预加载 + 广播)
    void on_event_preload_points_tick();

    /// event 通道维护 tick 实际执行体 (供 start/reschedule 共用, 避免 DRY 违规)
    void run_event_maintenance_tick();

    /// 停止 event channel 信号量监听线程（master 退出前调用）。
    void stop_event_thread();

    /// 定期清理旧页面文件（由 cleanup_timer_ 60s 触发，也可手动调）。
    void cleanup_old_pages();

    // ===== SHM 帧处理 =====
    /// 排空 event channel：批量读取已到达的帧并逐个处理（由信号量唤醒，非轮询）。
    void drain_event_channel();

    /// 处理一个 SHM 帧 (dispatch 到对应的 case)。
    /// 与 dzmd_ctp 的 MdApi::handle_frame 签名一致。
    void handle_frame(const std::byte* frame);

    /// 读取指定行情源的 page_size (从 <source_name>.json 的 shm.page_size_mb 字段)
    /// 若文件不存在或字段缺失, 返回 kDefaultMdPageSize (1024MB, 与 MdShmConfig 默认值一致)
    /// 若文件存在但解析失败 (malformed json), 返回 nullopt 表示失败
    /// (失败路径 C: 调用方应据此发 notify_ui 并阻止 spawn)
    std::optional<uint64_t> read_md_page_size(const std::string& source_name);

    // ===== 配置上报与进程状态 =====
    /// 推送当前日志配置 (RTN_LOG_CONFIG)
    void report_log_config();

    /// 推送当前 SHM 配置 (RTN_EVENT_SHM_CONFIG, 推送完整 EventShmConfig)
    void report_shm_config();

    /// 上报完整快照: 推送 master 自己的配置 (RTN_LOG_CONFIG + RTN_EVENT_SHM_CONFIG)
    /// + 遍历 supervisor_ 推送所有子进程的 ProcessStatus。
    /// 由 main.cpp 启动时和 DZ_FRAME_QUERY_FULL_SNAPSHOT 帧触发。
    /// 与 mdctp 的 report_full_snapshot() 形成命名一致性。
    void report_full_snapshot();

    /// 写 PROCESS_STATUS 扩展帧 (116, 无 instance_id) 到 event channel + notify subscribers。
    /// master 在子进程状态变化时调用 (launch_child/on_child_exit/stop_single_child)。
    void write_process_status(const platform::ProcessStatus& status);

    /// 进程配置存储（只读访问：display_name 真相源，契约 process）
    [[nodiscard]] const ProcessConfigStore& process_config_store() const {
        return *process_config_store_;
    }
    /// 某进程当前配置中的 display_name（缺省空串；未注册返回空串）
    [[nodiscard]] std::string display_name_of(const std::string& name) const;

    /// 向 event channel 写 REQUEST_SHUTDOWN 帧（instance_id=target，仅匹配的进程执行）。
    void send_shutdown(std::string_view target);

    // ===== 订阅者管理 =====
    /// 通过事件通道发 UPDATE_SHM_MD_SUBSCRIBER 帧 instance_id=source_name，
    /// 通知行情进程（dzmd_ctp）刷新行情通道 writer 缓存的订阅者列表。
    /// 行情进程收到 instance_id=name_ 的帧后，调用 spi_.refresh_subscribers()（内部加自旋锁）。
    void notify_md_channel_subscriber_update(std::string_view source_name);

    /// 更新订阅者列表（仅 master 写入）。
    void update_subscribers(std::string_view channel_name,
                            const std::vector<std::string>& subscribers);

    /// 通知子进程更新订阅者列表（通过事件通道写 UPDATE_SHM_EVENT_SUBSCRIBER 帧 + notify）。
    void notify_subscriber_update();

    /// 注册子进程订阅者：add_reader + 写 UPDATE_SHM_EVENT_SUBSCRIBER 帧 + notify。
    /// name 为已构造好的订阅者名（如 "dzweb"、"stg.my_strat"，无 pid 后缀，
    /// 与子进程自身的 reader/信号量名一致，见 make_subscriber_name）。
    void register_subscriber(const std::string& name, uint64_t pid);

    /// 注销子进程订阅者：remove_reader + 写 UPDATE_SHM_EVENT_SUBSCRIBER 帧 + notify。
    /// name 为已构造好的订阅者名（如 "dzweb"、"stg.my_strat"，无 pid 后缀）。
    void unregister_subscriber(const std::string& name);

    /// 兜底清理订阅者：仅调 event_meta_->remove_reader + try/catch，
    /// **不通知子进程**（子进程已不运行，无需通知）。
    /// 与 unregister_subscriber 的差异：unregister_subscriber 是正常注销路径，
    /// 会通知子进程刷新订阅者缓存；remove_reader 是兜底清理路径。
    /// 由 ProcessSupervisor::notify_removed_for_inactive 调用。
    void remove_reader(std::string_view name);

    // ===== 行情通道读者注册（帧 1013/1014, 任意已注册进程发起, master 持权） =====
    /// 处理 DZ_FRAME_REQUEST_MD_READER_REGISTER：校验 subscriber 为已注册进程
    /// （任意类别，不限策略，契约 shm）与通道三条件（已配置 / 行情进程运行 /
    /// 已就绪），通过后 add_reader + 广播 UPDATE + 回 RTN_MD_READER_REGISTER(1015)
    /// 成功；失败回 RTN ok=false 且 message 必填。
    void handle_md_reader_register(const shm::FrameView& view);

    /// 处理 DZ_FRAME_REQUEST_MD_READER_UNREGISTER：同身份校验，remove_reader
    /// （幂等）+ 广播 UPDATE + 回 RTN_MD_READER_UNREGISTER(1016)；
    /// 通道不存在/已关闭时幂等成功。
    void handle_md_reader_unregister(const shm::FrameView& view);

    /// 从全部 md 通道注销指定读者并通知对应 md 进程刷新缓存。
    /// 子进程退出时由 ProcessSupervisor::on_child_exit 调用（幂等，key 不存在为 no-op）。
    void remove_reader_from_all_md_channels(const std::string& sub_name);

    /// 重置订阅者列表：clear_readers + add_reader(master 自己) + 写 UPDATE_SHM_EVENT_SUBSCRIBER 帧。
    /// 在 master 启动创建事件通道后调用。
    void reset_subscribers();

    // ===== 访问器 =====
    /// 返回 dztraderd.json 路径 (供 ProcessSupervisor 在 on_child_exit 中
    /// 调用 remove_gateway_section 使用, 避免重复传递)
    const std::filesystem::path& config_path() const { return config_path_; }

    /// 返回 event channel writer 引用 (供 ProcessSupervisor 直接调用 frame_writer 函数)
    shm::MultiWriter& event_writer() { return event_writer_; }

    /// 返回进程名 (供 ProcessSupervisor 构造 NotifyUi 时作为 source)
    const std::string& name() const { return name_; }

    /// md 通道默认 page size (子进程配置缺失时使用, 与 MdShmConfig 默认值一致)
    static constexpr uint64_t kDefaultMdPageSize = 1024 * 1024 * 1024;

private:
    // ===== 帧分发私有方法 (handle_frame 拆分) =====
    void handle_process_control(const shm::FrameView& view);
    void handle_process_start(const platform::ProcessControlReq& req);
    void handle_process_stop(const platform::ProcessControlReq& req);
    void handle_process_remove(const platform::ProcessControlReq& req);
    void handle_set_process_config(const shm::FrameView& view);

    // ===== 配置状态 (声明在最前, 构造函数初始化列表先执行) =====
    /// 全局 meta 文件大小 (所有通道共用,启动后不变)
    uint64_t meta_file_size_ = 1 * 1024 * 1024;

    /// 旧页清理策略 (全局统一, event + 全部 md 通道共用; 双 0 不清理)
    shm::CleanupPolicy cleanup_policy_{};

    /// 不可变: event 通道 page size (字节,从 event_shm_config_.page_size_mb 换算,启动后不变)
    uint64_t event_page_size_ = 32 * 1024 * 1024;

    /// ProcessSupervisor 指针 (set_supervisor 注入, 可空)
    ProcessSupervisor* supervisor_ = nullptr;

    /// 进程名 (与 exe stem 一致, 替代硬编码 "dztraderd")
    std::string name_;

    /// dztraderd.json 路径 (init 时保存, 用于 PROCESS_CONTROL start/stop 时
    /// 调 write_gateway_section / remove_gateway_section 持久化网关声明)
    std::filesystem::path config_path_;

    // ===== SHM 基础设施 =====
    // 声明顺序 = 析构逆序 = 依赖顺序 (writer/reader/cleaner 依赖 event_meta_)
    std::shared_ptr<shm::ChannelMeta> event_meta_;
    std::unordered_map<std::string, MdChannelState> md_channels_;

    // event channel 读写器 (RAII: 构造函数初始化列表创建, 析构自动释放)
    shm::MultiWriter event_writer_;
    shm::Reader event_reader_;
    std::unique_ptr<shm::PageCleaner> cleaner_;

    // ===== 日志配置 (依赖 event_writer_, 必须在其后声明; C++ 按声明顺序初始化) =====
    /// master 自己的日志配置 (SET_LOG_CONFIG 时更新 + 应用, RTN_LOG_CONFIG 时上报)
    dztrader::platform::LogConfig log_config_;

    /// UI 通知发送器 (依赖 name_ + event_writer_, 必须在其后声明)
    dztrader::platform::NotifyUi notify_ui_;

    // ===== event 通道 SHM 配置 (依赖 event_writer_/config_path_, 必须在其后声明) =====
    /// event 通道 SHM 配置 (SET_EVENT_SHM_CONFIG 时更新可变字段,page_size_mb 不可变)
    /// RTN 无 instance_id (契约 shm：事件通道帧头无 instance_id)
    dztrader::platform::EventShmConfig event_shm_config_;

    // ===== 进程配置存储 (依赖 event_writer_/config_path_/supervisor_, 必须在其后声明) =====
    /// 进程配置存储 (SET_PROCESS_CONFIG/RTN 处理 + dztraderd.json 持久化, 构造体内 emplace)
    std::optional<ProcessConfigStore> process_config_store_;

    /// 进程配置持久化回调：full 为 null 删除条目；否则按 category 写 dztraderd.json 段
    void persist_process_config(const std::string& name, const nlohmann::json& full);
    /// 进程配置应用回调：full 为 null 移除 registry 条目；否则更新 ProcessEntry 内存
    void apply_process_config(const std::string& name, const nlohmann::json& full);
    /// 从 registry entries 构建全量配置 map（{name: {args,env,restart,display_name?}}）
    [[nodiscard]] nlohmann::json build_initial_config_map() const;
    /// 动态注册网关进程（PROCESS_CONTROL start 未注册目标扫描命中, 契约 process 修订）:
    /// registry 注册（persist 依赖 find_registry_entry 取 category）→ store 注册
    /// （persist 写 dztraderd.json + apply 更新 registry + 写镜像）。失败抛异常,
    /// 调用方按 StartFailed 处理。本方法不发 RTN。
    void register_dynamic_gateway(const ProcessEntry& scanned);

    // ===== 定时器与调度 =====
    std::unique_ptr<boost::asio::steady_timer> cleanup_timer_;
    boost::asio::io_context* ioc_ = nullptr;

    /// event 通道维护定时器 (周期: event_shm_config_.check_interval_min 分钟)
    std::unique_ptr<boost::asio::steady_timer> event_maintenance_timer_;

    /// preload_points 检查定时器 (每分钟触发, 检查是否匹配 HH:MM)
    std::unique_ptr<boost::asio::steady_timer> preload_points_timer_;

    // ===== 事件监听线程 =====
    // event channel 信号量监听（事件驱动，非轮询）
    // event_sem_ 名 = "dztraderd.<pid>"，与 reset_subscribers 中注册的订阅者名一致，
    // writer notify_subscribers 时被唤醒
    shm::NamedSemaphore event_sem_;
    std::thread event_thread_;
    std::atomic<bool> stop_flag_{false};
};

}  // namespace dztrader::master

#endif  // DZTRADER_MASTER_SHM_MANAGER_H_
