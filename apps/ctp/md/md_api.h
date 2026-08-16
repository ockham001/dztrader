#ifndef DZTRADER_CTP_MD_API_H_
#define DZTRADER_CTP_MD_API_H_

#include <atomic>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <dztrader/platform/log_config.h>
#include <spdlog/spdlog.h>
#include <magic_enum/magic_enum.hpp>

#include <ThostFtdcMdApi.h>

#include <dztrader/core/core_data_type.h>
#include <dztrader/core/core_struct.h>
#include <dztrader/core/exception.h>
#include <dztrader/shm/reader.h>
#include <dztrader/platform/shm_config.h>
#include <dztrader/platform/auto_login.h>
#include <dztrader/platform/progress.h>
#include <dztrader/platform/notify_ui.h>
#include <dztrader/shm/writer.h>

#include "md/md_spi.h"
#include "md/md_state.h"
#include <dztrader/platform/ctp_md_config.h>
#include "common/ctp_events.h"
#include "common/trading_calendar.h"
#include <dztrader/core/timer_queue.h>

namespace dztrader::ctp {

/// 将合约列表分批为多个批次 (纯函数, 供单元测试)。
/// batch_size <= 0 兜底为 1。
std::deque<std::vector<std::string>> make_batches(std::vector<std::string> instruments,
                                                  int batch_size);

/// 在 brokers 中查找 name 对应的 broker (纯函数, 供单元测试)。
/// name 为空或未找到时返回 nullptr。
const dztrader::platform::CtpBrokerEntry* find_current_broker_in(
    const std::vector<dztrader::platform::CtpBrokerEntry>& brokers,
    const std::string& name);

// on_batch_complete 决策 (BatchCheckAction / decide_batch_check_action) 同为纯函数,
// 声明在独立头文件 md/md_batch_check.h (枚举值无法 forward declare, 测试需 include)。

/// MdApi: CTP 行情网关核心类。
///
/// 职责:
/// - 管理 CTP MdApi 生命周期 (Create/Init/Release)
/// - 主循环: 处理 SHM 帧 + SPI 事件 + 定时器
/// - 订阅批次管理 (subscribe_batch_size 分批)
/// - 配置热更新 (op-based, 副本 + 回滚)
/// - 自动调度 (登录/登出时间表)
/// - SHM 通道维护 (周期预加载 + 清理)
///
/// 线程模型:
/// - 主线程: run() 主循环, 处理帧/事件/定时器
/// - SPI 线程 (CTP 工作线程): 回调通过 event_queue_ 或直接写 SHM (OnRtnDepthMarketData)
/// - 共享数据: event_writer_/reader_ (主线程), spi_.md_writer_ (SPI 线程, SpinLock 保护)
///
/// 生命周期:
/// - 构造: 初始化成员, 不连接 CTP
/// - run(): 初始化 (首次, started_ 标志) + 主循环, 直到 running_=false 或异常
/// - 析构: 释放 api_ (RegisterSpi(nullptr) + Release), 等待 SPI 线程退出, drain 事件队列
class MdApi {
public:
    /// 构造 MdApi。
    /// @param name 网关名 (= exe_stem, 如 "dzmd_ctp")
    /// @param ch_md_shm_dir 行情数据通道 SHM 目录
    /// @param ch_event_meta 事件通道 meta (与 master 共享)
    /// @param event_queue SPI 线程 -> 主线程的事件队列
    /// @param flow_dir CTP 流文件目录
    /// @param reader_name 订阅者名 (与 master make_subscriber_name 一致)
    /// @param cfg_path dzmd_ctp.json 路径 (用于构造 log_config_, 加载 log section)
    MdApi(const std::string& name,
          const std::filesystem::path& ch_md_shm_dir,
          const std::shared_ptr<shm::ChannelMeta>& ch_event_meta,
          const SpscQueuePtr& event_queue,
          const std::filesystem::path& flow_dir,
          const std::string& reader_name,
          const std::filesystem::path& cfg_path);

    MdApi(const MdApi&) = delete;
    MdApi& operator=(const MdApi&) = delete;
    MdApi(MdApi&&) = delete;
    MdApi& operator=(MdApi&&) = delete;

    /// 析构: 释放 CTP API (RegisterSpi(nullptr) + Release), drain 事件队列
    ~MdApi();

    /// 主循环入口。首次调用时执行初始化 (started_ 标志), 之后循环处理帧/事件/定时器,
    /// 直到收到 DZ_FRAME_REQUEST_SHUTDOWN 或异常。阻塞调用。
    void run();

    /// 设置 --recover 标志: 启动时若在会话区间内则立即补登
    void set_recover(bool r) { recover_ = r; }

    /// 设置外部 shutdown 标志指针 (由 md_main.cpp 信号处理器置位)。
    /// 必须在 run() 前调用。
    void set_external_shutdown_flag(std::atomic<bool>* flag) { external_shutdown_flag_ = flag; }

private:
    /// 处理一个 SHM 帧 (外层 try-catch 兜底, 避免异常逃逸到 run() 导致崩溃)
    void handle_frame(const std::byte* frame);
    /// 实际帧分发逻辑 (由 handle_frame 调用, 异常由外层兜底)
    void handle_frame_inner(const std::byte* frame);
    /// 判断定向帧是否发往本实例 (帧头 ext_inst_id == name_)
    bool is_addressed_to_me(const shm::FrameView& view) const {
        return strcmp(view.ext_inst_id(), name_.c_str()) == 0;
    }
    /// 处理 DZ_FRAME_SET_LOG_CONFIG (decode -> set_log_config -> rtn)
    void handle_set_log_config(const shm::FrameView& view);
    /// 处理 DZ_FRAME_SET_MD_SHM_CONFIG (decode -> set_shm_config -> rtn -> 重排维护定时器)
    void handle_set_md_shm_config(const shm::FrameView& view);
    /// 处理 DZ_FRAME_SET_AUTO_LOGIN (decode -> set_auto_login -> rtn, 失败 notify_ui + rtn 旧值)
    void handle_set_auto_login(const shm::FrameView& view);
    /// 处理 DZ_FRAME_SET_MD_CONFIG (decode -> apply_config_change, 失败 catch+上报)
    void handle_set_md_config(const shm::FrameView& view);
    /// 处理 DZ_FRAME_QUERY_MD_SUBSCRIPTIONS (两种模式: unsuccessful / instruments 列表)
    void handle_query_md_subscriptions(const shm::FrameView& view);
    /// 处理 DZ_FRAME_REQUEST_MD_SUBSCRIBE (Subscribe/Unsubscribe/UnsubscribeAll)
    void handle_subscribe_req(const shm::FrameView& view);
    /// 事件通道预加载: 收到 DZ_FRAME_PRELOAD_EVENT_SHM 广播后随机延迟 0-5s 执行三件套
    void schedule_event_shm_preload(const DzShmPreload& params);
    /// 事件通道预加载定时器回调: 对 reader_ + event_writer_ 执行三件套 + touch
    void on_event_shm_timer(const DzShmPreload& params);

    /// 行情数据通道自管理: 排定周期定时器
    void schedule_md_shm_maintenance();
    /// 行情数据通道维护: prefetch + close_old + touch + 广播 PRELOAD_MD_SHM。
    /// on_md_shm_timer 和 on_sched_timer preload_point 共用。
    void maintain_md_shm(uint32_t pages, uint64_t bytes);
    /// 行情数据通道维护定时器回调: 对 spi_ 执行三件套 + touch + 通知
    void on_md_shm_timer();

    /// 广播行情数据通道预加载参数 (通过事件通道发 DZ_FRAME_PRELOAD_MD_SHM 帧, 带本进程 instance_id)
    /// 订阅了本行情源的进程收到后预加载对应 md 通道的 reader
    void broadcast_md_preload(const DzShmPreload& params);
    /// 取消登录超时定时器 (若挂起), 并置 0。幂等。
    void cancel_login_timer();
    /// 处理 DZ_FRAME_REQUEST_SHUTDOWN 帧: 设置 running_=false, 让 run() 退出
    void handle_shutdown();
    /// 推送网关状态 (RTN_MD_STATUS): 契约 md-status 的 6 字段
    /// 内部先同步订阅统计 (set_subscription_stats)，再构造 JSON 发送。
    void report_md_status();
    /// 推送进度 (RTN_PROGRESS): 从状态机 progress_* 字段读取, 经 progress_reporter_ 发送。
    void report_progress();

    /// 行情健康度广播 (DZ_FRAME_NOTIFY_MD_CONNECTED / DZ_FRAME_NOTIFY_MD_DISCONNECTED)
    /// 与 report_progress (DZ_FRAME_RTN_PROGRESS) 是两条独立线:
    ///   - report_progress 上报细粒度状态 (MdState + 进度), 给 UI 展示
    ///   - broadcast_health 上报二元健康度 (Up/Down), 给策略/数据存储决策
    /// 仅在 health 翻转时发送, 避免重复。
    ///   - 进入 LoggedIn -> Up -> DZ_FRAME_NOTIFY_MD_CONNECTED
    ///   - 离开 LoggedIn -> Down -> DZ_FRAME_NOTIFY_MD_DISCONNECTED
    void broadcast_health(MdHealth now);

    /// 连接 CTP 前置 (RegisterFront + Init)。前置条件: MdState::Idle
    void connect();
    /// 断开 CTP 前置 (Release)
    void disconnect();
    /// 发送登录请求 (ReqUserLogin)。前置条件: MdState::Connected
    void req_user_login();
    /// 推送 UI 通知 (转发 state_machine 返回的 json 到 NotifyUi 类)
    void notify_ui(const nlohmann::json& field);
    /// 应用配置变更 (op-based: 验证 -> 持久化 -> 应用 -> 上报), 失败 catch
    void apply_config_change(const dztrader::platform::CtpMdConfigOpReq& req);

    /// 自动调度: 对齐到下个分钟 25 秒排定定时器 (错峰, 避开整分钟 0 秒)
    void schedule_auto_sched_timer();
    /// 自动调度定时器回调: 读 system_clock, 评估动作, 重排定时器
    void on_sched_timer();
    /// 崩溃恢复补登: --recover 启动时若在会话区间内则立即登录
    void try_recover_login();

    /// 上报完整快照: 推 RTN_MD_CONFIG (脱敏) + RTN_LOG_CONFIG
    /// + RTN_MD_SHM_CONFIG + RTN_MD_STATUS + RTN_PROGRESS。
    /// 由 run() 启动时和 DZ_FRAME_QUERY_FULL_SNAPSHOT 帧触发
    void report_full_snapshot();

    /// 上报当前配置到 UI (RTN_MD_CONFIG)。
    /// 契约 md-config: payload 始终为纯脱敏 CtpMdConfigData JSON, 不带 error 字段; 失败原因由 NOTIFY_UI 传达。
    void report_config();

    /// 上报当前日志配置: 推 DZ_FRAME_RTN_LOG_CONFIG
    void report_log_config();
    /// 上报当前 SHM 通道配置: 推 DZ_FRAME_RTN_MD_SHM_CONFIG
    void report_md_shm_config();
    /// 上报当前自动登录/登出排程: 推 DZ_FRAME_RTN_AUTO_LOGIN
    void report_auto_login();

    /// 在 ctp_md_config_.config().brokers 中查找 current_broker_name 对应的 broker。
    /// 未找到或 current_broker_name 为空时返回 nullptr。
    const dztrader::platform::CtpBrokerEntry* find_current_broker() const;

    /// 从事件队列取出一个 Event, 按 EventType dispatch 到对应 on_* 回调
    void dispatch_ctp_event(Event& event);
    /// 模板 dispatch: 类型擦除后调用 handler, 异常不传播 (仅记日志)
    template <typename T>
    void dispatch(Event& event, void (MdApi::*handler)(const T&)) {
        auto* rsp = static_cast<T*>(event.data);
        if (rsp == nullptr) {
            // CTP 回调可能无附加数据 (如 OnRspError 无 RspInfo), push 端允许 nullptr。
            // nullptr 无需释放, 跳过 handler 直接返回。
            return;
        }
        try {
            (this->*handler)(*rsp);
        } catch (const dztrader::Exception& e) {
            SPDLOG_ERROR("dispatch failed | type={} code={} error=\"{}\"",
                         magic_enum::enum_name(event.type), e.code(), e.what());
        } catch (const std::exception& e) {
            SPDLOG_ERROR("dispatch failed | type={} error=\"{}\"",
                         magic_enum::enum_name(event.type), e.what());
        } catch (...) {
            SPDLOG_ERROR("dispatch failed | type={} error=unknown_exception",
                         magic_enum::enum_name(event.type));
        }
        delete rsp;  // NOLINT
    }

    /// 析构时清理队列残留事件, 防止 shutdown 与 SPI 回调竞态导致泄漏。
    /// 不调用 handler (进程即将退出, 无需处理), 仅按 EventType 释放 data。
    void drain_event_queue();

    /// CTP SPI 回调: 前置连接成功
    void on_front_connected(const OnFrontConnectedField& rsp);
    /// CTP SPI 回调: 前置连接断开
    void on_front_disconnected(const OnFrontDisconnectedField& rsp);
    /// CTP SPI 回调: 心跳超时警告
    void on_heartbeat_warning(const OnHeartBeatWarningField& rsp);
    /// CTP SPI 回调: 登录响应
    void on_rsp_user_login(const OnRspUserLoginField& rsp);
    /// CTP SPI 回调: 登出响应
    void on_rsp_user_logout(const OnRspUserLogoutField& rsp);
    /// CTP SPI 回调: 错误响应
    void on_rsp_error(const OnRspErrorField& rsp);
    /// CTP SPI 回调: 订阅行情响应
    void on_rsp_sub_market_data(const OnRspSubMarketDataField& rsp);
    /// CTP SPI 回调: 退订行情响应
    void on_rsp_unsub_market_data(const OnRspUnSubMarketDataField& rsp);

    // --- 订阅批次管理 ---
    /// 将合约列表分批追加到 pending_batches_ 队列 (batch_size 由 ctp_md_config_.config().subscribe_batch_size
    /// 提供)。
    void enqueue_batches(std::vector<std::string> instruments);

    /// 发送下一批 CTP 订阅。队列空时转 on_batch_complete; 否则发批次后用 TimerQueue 延迟发下一批。
    void send_next_batch();

    /// 所有批次发完后的补订检查入口。收集 Pending 合约, 重试或达上限后重置。
    void on_batch_complete();

    /// 分批调用 CTP UnSubscribeMarketData。
    void batch_unsubscribe(const std::vector<std::string>& instruments);

    CThostFtdcMdApi* api_ = nullptr;
    MdSpi spi_;
    SpscQueuePtr event_queue_;
    int request_id_ = 0;
    bool running_ = false;
    /// 外部 shutdown 标志 (由 md_main.cpp 的信号处理器置位)。
    /// run() 主循环检查此标志, 若置位则调用 handle_shutdown() 优雅退出。
    std::atomic<bool>* external_shutdown_flag_ = nullptr;
    bool started_ = false;  ///< run() 是否已完成初始化 (防重入重复广播)
    shm::Reader reader_;
    shm::MultiWriter event_writer_;
    std::string name_;
    std::filesystem::path config_path_;  ///< dzmd_ctp.json 路径, 用于持久化各 section
    // ctp_md_config_ 依赖 event_writer_/name_/config_path_, 必须在其后声明 (C++ 按声明顺序初始化)
    dztrader::platform::CtpMdConfig ctp_md_config_;  ///< md section 配置 (brokers/current_broker/subscribe_params, SET/RTN_MD_CONFIG, 持久化到 md section)
    // notify_ui_ 依赖 event_writer_/name_, 必须在其后声明 (C++ 按声明顺序初始化)
    dztrader::platform::NotifyUi notify_ui_;  ///< UI 通知发送器 (绑定 source+writer)
    // log_config_ 依赖 event_writer_/name_/config_path_, 必须在其后声明 (C++ 按声明顺序初始化)
    dztrader::platform::LogConfig log_config_;  ///< log section 配置 (level/flush_on)
    // md_shm_config_ 依赖 event_writer_/name_/config_path_, 必须在其后声明 (C++ 按声明顺序初始化)
    dztrader::platform::MdShmConfig md_shm_config_;  ///< 行情通道 SHM 配置 (SET_MD_SHM_CONFIG 时更新可变字段, page_size_mb 不可变; RTN 带 instance_id=name_)
    // auto_login_config_ 依赖 event_writer_/name_/config_path_, 必须在其后声明 (C++ 按声明顺序初始化)
    dztrader::platform::AutoLoginConfig auto_login_config_;  ///< 自动登录/登出排程 (SET/RTN_AUTO_LOGIN, 持久化到 auto_login section)
    // progress_reporter_ 依赖 event_writer_/name_, 必须在其后声明 (C++ 按声明顺序初始化)
    dztrader::platform::ProgressReporter progress_reporter_;  ///< 进度推送 (RTN_PROGRESS, 无持久化)
    MdStateMachine state_machine_;
    dztrader::platform::SubscriptionManager sub_manager_;  ///< 订阅管理器（封装帧处理+状态机+查询）
    std::filesystem::path flow_dir_;
    std::deque<std::vector<std::string>> pending_batches_;  ///< 待发送批次队列
    bool sub_check_active_ = false;                         ///< 补订链路是否运行
    int sub_retry_count_ = 0;                               ///< 当前重试次数
    /// 订阅链路代际计数器。断线时自增, 使已挂起的定时器回调失效,
    /// 避免陈旧回调在新会话 LoggedIn 状态下触发重复订阅或早熟重试。
    uint64_t sub_generation_ = 0;
    /// 当前 in-flight 的订阅定时器 id (send_next_batch 或 on_batch_complete 延迟)。
    /// 任意时刻最多 1 个。断线/重新登录时 cancel, 避免僵尸定时器无意义唤醒主循环。
    dztrader::core::TimerQueue::TimerId sub_timer_id_ = 0;
    /// 连接超时定时器 id。connect() 排定 30s 超时, on_front_connected/disconnect 取消。
    dztrader::core::TimerQueue::TimerId connect_timer_id_ = 0;
    /// 登录超时定时器 id。req_user_login() 排定 10s 超时, on_rsp_user_login(成功/失败) 取消。
    /// 防止 OnRspUserLogin 事件丢失 (队列满) 或 CTP 无响应导致永久卡在 LoggingIn。
    dztrader::core::TimerQueue::TimerId login_timer_id_ = 0;
    bool recover_ = false;                    ///< --recover 标志: 崩溃恢复启动, 启动时补登
    dztrader::core::TimerQueue timer_queue_;  ///< 单线程定时器队列, 主循环 tick()
    MdHealth last_health_ = MdHealth::Down;   ///< 上次广播的健康度, 用于翻转去重
};

}  // namespace dztrader::ctp

#endif  // DZTRADER_CTP_MD_API_H_