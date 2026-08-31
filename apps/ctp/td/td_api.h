#ifndef DZTRADER_CTP_TD_API_H_
#define DZTRADER_CTP_TD_API_H_

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <magic_enum/magic_enum.hpp>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <dztrader/core/exception.h>
#include <dztrader/core/timer_queue.h>
#include <dztrader/platform/log_config.h>
#include <dztrader/platform/auto_login.h>
#include <dztrader/platform/notify_ui.h>
#include <dztrader/shm/channel_meta.h>
#include <dztrader/shm/frame_view.h>
#include <dztrader/shm/order_id_meta.h>
#include <dztrader/shm/reader.h>
#include <dztrader/shm/writer.h>
#include <dztrader/struct.h>

#include "common/ctp_events.h"
#include "common/trading_calendar.h"
#include "td/td_account_session.h"
#include "td/td_config.h"
#include "td/td_persist_writer.h"
#include "td/td_state.h"

namespace dztrader::ctp {

// ============================================================================
// 纯函数声明 (实现在 td_api_pure.cpp, 供单元测试直接编译, 不依赖 TdApi 类和 CTP 头)
// 与 md_api.h 声明 apply_config_op / find_current_broker_in 的模式一致
// ============================================================================

/// 纯粹的 op 应用: switch(req.op) 修改 cfg, 无副作用无 I/O。
/// @throws std::invalid_argument 若 op 非法或参数越界 (如重复 AddAccount / UpdateAccount 不存在)
void apply_config_op(TdConfig& cfg, const TdConfigOpReq& req);

/// 在 accounts 中查找 account_id 对应的账户 (纯函数)。
/// account_id 为空或未找到时返回 nullptr。
const AccountConfig* find_current_account_in(const std::vector<AccountConfig>& accounts,
                                              const std::string& account_id);

/// 在 cfg.accounts 中查找 account_id 对应的账户 (纯函数, 便捷重载)。
/// account_id 为空或未找到时返回 nullptr。委托给 find_current_account_in(cfg.accounts, ...)。
const AccountConfig* find_account_in(const TdConfig& cfg, const std::string& account_id);

/// 2115 查询路由: queried 为空串 = 查全部 (true); 否则命中配置才应答 (不在配置不回, master 兜底)
bool account_query_matches(const std::vector<std::string>& configured,
                           const std::string& queried);

/// TdApi: CTP 交易网关进程级编排器 (设计 §1.1 多账户路由, §6.3 主循环)。
///
/// 职责:
/// - 管理多个 AccountSession (每账户一个, sessions_ map)
/// - 主循环: 处理 SHM 帧 + SPI 事件 (MPMC) + 定时器 + 信号量 wait
/// - 帧路由: basic 广播帧 (TD_ORDER_REQ 等) + 含 instance_id 帧 (TD_CONNECT 等)
/// - 事件 dispatch: 按 Field.account_id 路由到对应 AccountSession::on_*
/// - 配置热更新 (op-based, 副本 + 回滚): td/log 两 section
/// - 健康度广播 (per-account TdHealth 翻转检测)
/// - 自动调度 (登录/登出时间表, 复用 trading_calendar)
/// - 事件通道预加载 (被动, 响应 master 的 PRELOAD_EVENT_SHM 广播, 与 md 端一致)
/// - 优雅退出 (信号 / REQUEST_SHUTDOWN 帧)
///
/// 线程模型 (设计 §1.2):
/// - 主线程: run() 主循环, 处理帧/事件/定时器
/// - SPI 线程 (每账户 1 个 CTP 工作线程): 仅 push 事件到 event_queue_ (MPMC)
/// - PersistWriter 线程: 独立 SQLite 写入线程 (由 persist_writer_ 拥有)
/// - 共享数据: event_writer_/reader_ (主线程), sessions_ 中各 AccountSession 的 writer 引用主线程的 event_writer_
///
/// 生命周期:
/// - 构造: 初始化 SHM reader/writer + OrderIdMeta + PersistWriter, 不连接 CTP
/// - run(): 首次调用时初始化 (started_ 标志) + 主循环, 直到 running_=false 或异常
/// - 析构: disconnect 所有 AccountSession + stop PersistWriter + drain 事件队列
///
/// 与 MdApi 的关键差异:
/// - 多账户: sessions_ map 持有多个 AccountSession (MdApi 单账户, 直接持有 api_/spi_)
/// - MPMC 事件队列: 多账户 SPI 线程共享 event_queue_ (MdApi 用 SPSC)
/// - 持有 PersistWriter + OrderIdMeta (MdApi 无持久化/订单 ID)
/// - dispatch 路由到 AccountSession::on_* (MdApi dispatch 到自身 on_*)
class TdApi {
public:
    /// 构造 TdApi。
    /// @param name 网关名 (= exe_stem, 如 "dztd_ctp")
    /// @param shm_dir 事件通道 SHM 目录 (master 已创建, open_only)
    /// @param event_meta 事件通道 meta (与 master 共享)
    /// @param event_queue SPI 线程 -> 主线程的事件队列 (MPMC, 多账户共享)
    /// @param flow_dir CTP 流文件目录根 (每账户子目录 flow_dir/<account_id>/)
    /// @param reader_name 订阅者名 (与 master make_subscriber_name 一致)
    /// @param cfg_path dztd_ctp.json 路径 (用于构造 log_config_, 加载 log section)
    TdApi(std::string name,
          std::filesystem::path shm_dir,
          std::shared_ptr<shm::ChannelMeta> event_meta,
          MpmcQueuePtr event_queue,
          std::filesystem::path flow_dir,
          std::string reader_name,
          std::filesystem::path cfg_path);

    TdApi(const TdApi&) = delete;
    TdApi& operator=(const TdApi&) = delete;
    TdApi(TdApi&&) = delete;
    TdApi& operator=(TdApi&&) = delete;

    /// 析构: disconnect 所有 AccountSession + stop PersistWriter + drain 事件队列。
    /// AccountSession 析构内部处理 RegisterSpi(nullptr) + Release。
    ~TdApi();

    /// 主循环入口。首次调用时执行初始化 (started_ 标志), 之后循环处理帧/事件/定时器,
    /// 直到收到 DZ_FRAME_REQUEST_SHUTDOWN / 外部信号 / 异常。阻塞调用。
    void run();

    /// 设置 td 配置, 必须在 run() 前调用
    /// (log_config_/auto_login_config_ 在构造函数中已加载, 不再由 set_configs 传入)
    void set_configs(TdConfig td_cfg);

    /// 设置 --recover 标志: 启动时若在会话区间内则对所有 enabled 账户补登
    void set_recover(bool r) { recover_ = r; }

    /// 设置外部 shutdown 标志指针 (由 td_main.cpp 信号处理器置位)。
    /// 必须在 run() 前调用。
    void set_external_shutdown_flag(std::atomic<bool>* flag) { external_shutdown_flag_ = flag; }

private:
    /// 启动自检: 保证 order_id 原子计数器 > 库内最大 order_id (设计 §13 step 8, 开放问题 7)。
    /// 跨机导入数据库后, 原子计数器可能落后于库内最大值, 此处 fetch_max 上调, 防止重号。
    /// 构造函数内调用, 时机在 PersistWriter::open() 后 start_writer() 前 (主线程独占 db)。
    void verify_order_id_against_db();
    // === 主循环 ===
    /// 处理一个 SHM 帧 (分两层: basic 帧 (广播控制帧 + TD_ORDER_REQ/CANCEL_REQ 按 payload account_id 路由) /
    /// 含 instance_id 帧检查 instance_id == name_ 或 name_: 前缀)
    void handle_frame(const std::byte* frame);
    /// 实际帧分发逻辑 (由 handle_frame 调用, 异常由外层兜底)
    void handle_frame_inner(const std::byte* frame);
    /// 从事件队列取出一个 Event, 按 EventType dispatch 到对应 AccountSession on_* 方法
    void process_event(Event& event);
    /// 模板 dispatch: 类型擦除后按 rsp->account_id 路由到 AccountSession handler, 异常不传播 (仅记日志)。
    /// handler 是 AccountSession 的 on_*(const T&) 方法。rsp 为 nullptr 时跳过。
    template <typename T>
    void dispatch(Event& event, void (AccountSession::*handler)(const T&));
    /// 处理 OnFrontConnected / OnFrontDisconnected 事件 (不能用 dispatch<T> 模板,
    /// 因 AccountSession::on_front_connected / on_front_disconnected 签名非 const T&).
    /// 从 td 版本 Field 提取 account_id, find_session 后调用对应 on_front_* 方法。
    /// session 为 null / 异常时记日志 + 释放 data (不传播异常)。
    void handle_front_event(Event& event);
    /// 析构时清理队列残留事件, 防止 shutdown 与 SPI 回调竞态导致泄漏。
    /// 不调用 handler (进程即将退出), 仅按 EventType 释放 data (td 类型走 AccountSession::delete_event)。
    void drain_event_queue();
    /// 优雅退出: running_=false + disconnect 所有 AccountSession + stop PersistWriter + 保存配置
    void handle_shutdown();

    // === 账户管理 ===
    /// 处理 TD_CONNECT 帧: 解析 account_id, 查 config_.accounts 找配置, 构造 AccountSession 并 open()
    void connect_account(const shm::FrameView& view);
    /// 处理 TD_DISCONNECT 帧: 解析 account_id, 调用 session->disconnect()
    void disconnect_account(const shm::FrameView& view);
    /// 按 account_id 直接连接 (TD_CONNECT 帧路径 + 自动调度 / try_recover_login 共用)。
    /// 查 config_.accounts -> 构造 AccountSession -> open()。异常不传播 (仅记日志 + 通知 UI)。
    void connect_account_by_id(const std::string& account_id);
    /// 按 account_id 直接断开 (TD_DISCONNECT 帧路径 + 自动调度共用)。
    /// find_session -> disconnect() -> erase。无 session 时 no-op。
    void disconnect_account_by_id(const std::string& account_id);
    /// 处理 TD_ORDER_REQ 帧 (契约 td-order: basic 广播帧): 解析 payload DzOrderReq,
    /// 按 account_id 归属过滤后 find_session + place_order
    void on_order_req(const shm::FrameView& view);
    /// 处理 TD_ORDER_CANCEL_REQ 帧 (契约 td-order: basic 广播帧): 解析 payload DzOrderCancelReq
    /// (order_id + account_id), 按 account_id 归属过滤后 find_session + cancel_order
    void on_cancel_req(const shm::FrameView& view);
    /// 在 sessions_ 中查找 account_id 对应的 AccountSession。未找到返回 nullptr。
    /// 与纯函数 find_account_in (查 config_.accounts 配置) 语义不同: 本方法查运行中的会话。
    AccountSession* find_session(const std::string& account_id);

    // === 状态/健康度 ===
    /// 推送指定账户的当前状态到 SHM (DZ_FRAME_RTN_TD_STATUS, payload 为 TdStatusRecord)
    void update_status(const std::string& account_id);
    /// 账户状态统一写出口 (spec §3.1): 2018 三态去重推送。
    /// state 与该账户上次推送相同则跳过; trading_day 为空串或 state!=READY 时帧内 trading_day=0。
    void write_account_status(const std::string& account_id, DzAccountState state,
                              const std::string& trading_day);
    /// 对 config_.accounts 全量重推 (无 session=Offline; spec §3.1 配置加载即推)
    void report_account_status_all();
    /// 处理 TD_QUERY_ACCOUNT_STATUS(2115) basic 广播帧: 空=全量应答, 指定=命中配置才应答
    void handle_query_account_status(const std::byte* frame);
    /// per-account 健康度翻转检测 + 广播 (NOTIFY_TD_CONNECTED / NOTIFY_TD_DISCONNECTED)。
    /// instance_id 格式 name_:account_id。仅在 health 翻转时发送, 避免重复。
    void broadcast_health(const std::string& account_id, TdHealth now);
    /// 对所有 sessions_ 调用 update_status (用于定时上报 / QUERY_ALL 响应)
    void report_state();
    /// 上报完整快照: 推 RTN_TD_CONFIG (脱敏) + RTN_LOG_CONFIG + 每个 session 的 RTN_TD_STATUS。
    /// 注意: tdctp 不上报 SHM 配置 (事件通道由 master 管理)。
    /// 由 run() 启动时和 DZ_FRAME_QUERY_FULL_SNAPSHOT 帧触发
    void report_full_snapshot();
    /// 上报当前 td 配置 (脱敏): 推 RTN_TD_CONFIG
    void report_config();
    /// 上报当前日志配置: 推 RTN_LOG_CONFIG
    void report_log_config();
    /// 上报当前自动登录/登出排程: 推 RTN_AUTO_LOGIN
    void report_auto_login();

    // === 配置热更新 (实现在 td_api_config.cpp) ===
    /// 应用 td 配置变更 (TD_REQ_MODIFY_CONFIG): 解码 TdConfigOpReq -> 副本 -> apply_config_op 纯函数
    /// -> validate -> 持久化到 config_path_ 的 td section -> 应用 -> 上报 RTN_TD_CONFIG。失败回滚。
    void apply_config_change(const TdConfigOpReq& req);

    // === 自动调度 (实现在 td_api_scheduled.cpp) ===
    /// 对齐到下个分钟 25 秒排定定时器 (错峰, 避开整分钟 0 秒)。复用 trading_calendar。
    void schedule_auto_sched_timer();
    /// 自动调度定时器回调: 读 system_clock, 评估动作 (自动登录/登出), 重排定时器
    void on_sched_timer();
    /// 崩溃恢复补登 (--recover): 启动时若在会话区间内, 对所有 enabled 账户调用 connect_account
    void try_recover_login();

    // === 事件通道预加载 (实现在 td_api_scheduled.cpp, 与 md_api_scheduled.cpp 组织一致) ===
    /// 事件通道预加载: 收到 DZ_FRAME_PRELOAD_EVENT_SHM 广播后随机延迟 0-5s 执行三件套
    void schedule_event_shm_preload(const DzShmPreload& params);
    /// 事件通道预加载定时器回调: 对 reader_ + event_writer_ 执行三件套
    void on_event_shm_timer(const DzShmPreload& params);

    // === 成员变量 ===
    std::string name_;                                   ///< 网关名 (= exe_stem)
    std::filesystem::path shm_dir_;                      ///< 事件通道 SHM 目录
    std::filesystem::path flow_dir_;                     ///< CTP 流文件目录根
    std::string reader_name_;                            ///< SHM 订阅者名
    std::shared_ptr<shm::ChannelMeta> event_meta_;       ///< 事件通道 meta (与 master 共享)
    MpmcQueuePtr event_queue_;                           ///< SPI -> 主线程事件队列 (MPMC)
    shm::Reader reader_;                                 ///< 事件通道 reader (主线程)
    shm::MultiWriter event_writer_;                      ///< 事件通道 writer (主线程)
    dztrader::core::TimerQueue timer_queue_;             ///< 单线程定时器队列, 主循环 tick()

    std::unique_ptr<PersistWriter> persist_writer_;      ///< SQLite 持久化 writer (进程级单例, TdApi 拥有)
    std::unique_ptr<shm::OrderIdMeta> order_id_meta_;    ///< 跨进程 order_id 计数器 (进程级单例, TdApi 拥有)

    TdConfig config_;                                    ///< td section 配置
    std::filesystem::path config_path_;                  ///< dztd_ctp.json 路径, 用于持久化各 section
    // notify_ui_ 依赖 name_/event_writer_, 必须在其后声明 (C++ 按声明顺序初始化)
    dztrader::platform::NotifyUi notify_ui_;             ///< UI 通知发送器 (绑定 name_ + event_writer_)
    // log_config_ 依赖 name_/config_path_, 必须在其后声明 (C++ 按声明顺序初始化)
    dztrader::platform::LogConfig log_config_;                ///< log section 配置
    // auto_login_config_ 依赖 name_/config_path_/event_writer_, 必须在其后声明 (C++ 按声明顺序初始化)
    dztrader::platform::AutoLoginConfig auto_login_config_;   ///< 自动登录/登出排程 (SET/RTN_AUTO_LOGIN, 持久化到 auto_login section)

    std::unordered_map<std::string, std::unique_ptr<AccountSession>> sessions_;  ///< account_id -> 会话
    std::unordered_map<std::string, TdHealth> account_health_;                   ///< account_id -> 上次广播健康度
    /// 账户上次推送的三态 (去重缓存; 键=account_id)
    std::unordered_map<std::string, DzAccountState> account_last_state_;

    bool running_ = false;                               ///< 主循环运行中
    bool started_ = false;                               ///< run() 是否已完成初始化 (防重入重复广播)
    bool recover_ = false;                               ///< --recover 标志: 崩溃恢复启动, 补登
    /// 外部 shutdown 标志 (由 td_main.cpp 的信号处理器置位)。
    /// run() 主循环检查此标志, 若置位则调用 handle_shutdown() 优雅退出。
    std::atomic<bool>* external_shutdown_flag_ = nullptr;
};

// ============================================================================
// dispatch 模板实现 (头文件内, 参考 md_api.h dispatch 模式)
// 与 MdApi::dispatch 的差异: handler 是 AccountSession 方法, 需先 find_session(rsp->account_id)
// 路由 (依赖各 TD Field 结构含 account_id 字段)。
// ============================================================================

template <typename T>
void TdApi::dispatch(Event& event, void (AccountSession::*handler)(const T&)) {
    auto* rsp = static_cast<T*>(event.data);
    if (rsp == nullptr) {
        // CTP 回调可能无附加数据, push 端允许 nullptr。nullptr 无需释放, 跳过 handler。
        return;
    }
    auto* session = find_session(rsp->account_id);
    if (session == nullptr) {
        SPDLOG_WARN("event for unknown account | type={} account={}",
                    magic_enum::enum_name(event.type), rsp->account_id);
        delete rsp;  // NOLINT
        return;
    }
    // 捕获 handler 前状态, 用于 Ready 翻转检测 (设计 §5.8 健康度广播)
    TdState before = session->state();
    try {
        (session->*handler)(*rsp);
    } catch (const dztrader::Exception& e) {
        SPDLOG_ERROR("dispatch failed | type={} account={} code={} error=\"{}\"",
                     magic_enum::enum_name(event.type), rsp->account_id, e.code(), e.what());
    } catch (const std::exception& e) {
        SPDLOG_ERROR("dispatch failed | type={} account={} error=\"{}\"",
                     magic_enum::enum_name(event.type), rsp->account_id, e.what());
    } catch (...) {
        SPDLOG_ERROR("dispatch failed | type={} account={} error=unknown_exception",
                     magic_enum::enum_name(event.type), rsp->account_id);
    }
    // handler 后状态: 进入/离开 Ready 时广播健康度 (设计 §5.8 per-account TdHealth 翻转)
    TdState after = session->state();
    if (before != TdState::Ready && after == TdState::Ready) {
        broadcast_health(rsp->account_id, TdHealth::Up);
    } else if (before == TdState::Ready && after != TdState::Ready) {
        broadcast_health(rsp->account_id, TdHealth::Down);
    }
    delete rsp;  // NOLINT
}

}  // namespace dztrader::ctp

#endif  // DZTRADER_CTP_TD_API_H_
