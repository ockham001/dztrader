#include "md/md_api.h"

#include <chrono>
#include <cstring>
#include <format>
#include <limits>
#include <optional>
#include <ThostFtdcMdApi.h>

#include <spdlog/spdlog.h>

#include <dztrader/data_type.h>
#include <dztrader/struct.h>
#include <dztrader/core/this_process.h>
#include <dztrader/log/log.h>
#include <dztrader/core/string_util.h>
#include <dztrader/shm/frame_view.h>
#include <dztrader/shm/frame_codec.h>
#include <dztrader/platform/frame_codec.h>

namespace dztrader::ctp {

using namespace dztrader::shm;

MdApi::MdApi(const std::string& name,
             const std::filesystem::path& ch_md_shm_dir,
             const std::shared_ptr<shm::ChannelMeta>& ch_event_meta,
             const SpscQueuePtr& event_queue,
             const std::filesystem::path& flow_dir,
             const std::string& reader_name,
             const std::filesystem::path& cfg_path)
    : spi_{name, ch_md_shm_dir, event_queue},
      event_queue_{event_queue},
      reader_{shm::Reader::create(ch_event_meta, reader_name)},
      event_writer_{shm::MultiWriter::create(ch_event_meta, reader_name)},
      name_{name},
      config_path_{cfg_path},
      ctp_md_config_(name_, config_path_, event_writer_),
      notify_ui_(name_, event_writer_),
      log_config_(name_, config_path_),
      md_shm_config_(name_, config_path_, event_writer_, nlohmann::json::json_pointer("/shm")),
      auto_login_config_(name_, config_path_, event_writer_),
      progress_reporter_(name_, event_writer_),
      sub_manager_(name_, event_writer_, notify_ui_),
      flow_dir_{flow_dir} {
    state_machine_.set_api_version(CThostFtdcMdApi::GetApiVersion());
    // 加载 md 配置 (从 dzmd_ctp.json "md" section, 失败用默认值自愈)
    ctp_md_config_.load();
    // 加载日志配置 (从 dzmd_ctp.json "log" section, 失败用默认值自愈, 与 spdlog 同步)
    log_config_.load();
    // 加载行情通道 SHM 配置 (从 dzmd_ctp.json "shm" section, 失败用默认值自愈)
    md_shm_config_.load();
    // 加载自动登录/登出排程 (从 dzmd_ctp.json "auto_login" section, 失败用默认值自愈)
    auto_login_config_.load();
}

void MdApi::run() {
    running_ = true;
    if (!started_) {
        // 首次进入: 初始化 (报告完整快照 + 广播 service_started + 排定时器)
        // 重入时跳过, 避免重复广播 service_started 和重复排定时器
        // 启动即上报 config + status, 让 dzweb 镜像冷启动时就填充
        report_full_snapshot();

        // 广播服务启动事件: 通知策略/数据存储进程本行情服务已就绪, 可重订阅
        // (空 payload, 靠帧头 instance_id 标识本 dzmd_ctp 实例)
        try {
            platform::write_ext_inst_raw(event_writer_, DZ_FRAME_NOTIFY_MD_STARTED, name_);
            SPDLOG_INFO("service started broadcast | instance={}", name_);
        } catch (const std::exception& e) {
            SPDLOG_ERROR("broadcast service_started failed | error=\"{}\"", e.what());
        }

        // --recover 补登: 崩溃恢复启动时若在会话区间内则立即登录
        if (recover_) {
            try_recover_login();
        }

        // 启动自动调度定时器: 对齐到下个分钟 25 秒 (错峰, 避开繁忙的整分钟 0 秒)
        schedule_auto_sched_timer();

        // 启动行情数据通道 SHM 周期检查定时器
        schedule_md_shm_maintenance();
        started_ = true;
    }

    while (running_) {
        for (;;) {
            int n = 0;
            int m = 0;
            for (; n < 32; ++n) {
                const auto* frame = reader_.next_frame();
                if (!frame) {
                    break;
                }
                handle_frame(frame);
            }
            for (; m < 32; ++m) {
                Event event;
                if (!event_queue_->pop(event)) {
                    break;
                }
                dispatch_ctp_event(event);
            }
            if (n < 32 && m < 32) {
                break;
            }
        }
        if (!running_) {
            break;  // 收到 DZ_FRAME_REQUEST_SHUTDOWN, 不阻塞等待
        }
        // 触发所有已到期的定时器(批次延迟、补订检查等)
        timer_queue_.tick();
        if (!running_) {
            break;
        }
        // 阻塞等待事件: 有定时器时精确超时唤醒, 无定时器时无限等待
        // 唤醒源: SPI 线程 notify / master notify / (有定时器时) 超时
        if (timer_queue_.empty()) {
            event_queue_->wait();  // 无定时器, 无限等待 (CPU=0, 等事件唤醒)
        } else {
            // next_timeout() 返回 native duration (无 uint32_t 截断),
            // NamedSemaphore::wait_for 接受 uint32_t ms, 需 cast + clamp 防溢出
            auto ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(timer_queue_.next_timeout())
                    .count();
            constexpr auto max_ms = static_cast<long long>(std::numeric_limits<uint32_t>::max());
            uint32_t timeout_ms =
                ms >= max_ms ? std::numeric_limits<uint32_t>::max() : static_cast<uint32_t>(ms);
            event_queue_->wait_for(timeout_ms);  // 精确超时唤醒
        }
        // 信号唤醒后检查外部 shutdown 标志
        if (external_shutdown_flag_ != nullptr &&
            external_shutdown_flag_->load(std::memory_order_relaxed)) {
            SPDLOG_INFO("external shutdown requested by signal");
            handle_shutdown();
        }
    }
    SPDLOG_INFO("dzmd_ctp run loop exited");
}

void MdApi::dispatch_ctp_event(Event& event) {
    switch (event.type) {
        case EventType::OnFrontConnected:
            dispatch<OnFrontConnectedField>(event, &MdApi::on_front_connected);
            break;
        case EventType::OnFrontDisconnected:
            dispatch<OnFrontDisconnectedField>(event, &MdApi::on_front_disconnected);
            break;
        case EventType::OnHeartbeatWarning:
            dispatch<OnHeartBeatWarningField>(event, &MdApi::on_heartbeat_warning);
            break;
        case EventType::OnRspUserLogin:
            dispatch<OnRspUserLoginField>(event, &MdApi::on_rsp_user_login);
            break;
        case EventType::OnRspError:
            dispatch<OnRspErrorField>(event, &MdApi::on_rsp_error);
            break;
        case EventType::OnRspSubMarketData:
            dispatch<OnRspSubMarketDataField>(event, &MdApi::on_rsp_sub_market_data);
            break;
        case EventType::OnRspUnsubMarketData:
            dispatch<OnRspUnSubMarketDataField>(event, &MdApi::on_rsp_unsub_market_data);
            break;
        case EventType::OnRspUserLogout:
            dispatch<OnRspUserLogoutField>(event, &MdApi::on_rsp_user_logout);
            break;
        default:
            SPDLOG_ERROR("unknown event | type={}", static_cast<int>(event.type));
            // Unknown 类型无法确定构造类型, 无法安全 delete, 仅记日志 (极罕见)。
            break;
    }
}

MdApi::~MdApi() {
    running_ = false;
    cancel_login_timer();  // 取消可能挂起的登录超时, 避免析构期间回调唤醒
    // 1. 释放 CTP API: RegisterSpi(nullptr) 断开回调, Release() 同步等待内部线程退出
    if (api_ != nullptr) {
        try {
            api_->RegisterSpi(nullptr);
            api_->Release();
        } catch (...) {
            SPDLOG_ERROR("md api destructor: release threw exception");
        }
        api_ = nullptr;
    }
    // 2. 清空事件队列 (此时 SPI 线程已退出, 无新事件)
    drain_event_queue();
}

void MdApi::drain_event_queue() {
    Event event;
    while (event_queue_->pop(event)) {
        // Unknown 类型的 data 无法确定构造类型, delete_data 不释放, 记日志。
        if (event.type == EventType::Unknown) {
            SPDLOG_WARN("leak event data on drain | type=Unknown");
        }
        event.delete_data();
    }
}

void MdApi::handle_frame(const std::byte* frame) {
    try {
        handle_frame_inner(frame);
    } catch (const std::exception& e) {
        // 外层兜底: 防御性, 理论上不应触发 (各 case 内部已处理自身异常)
        // 仅记日志, 避免异常逃逸到 run() 导致进程崩溃
        SPDLOG_ERROR("handle_frame unexpected exception | error=\"{}\"", e.what());
    } catch (...) {
        // 兜底非 std 异常, 仅记日志不崩溃
        SPDLOG_ERROR("handle_frame unexpected non-std exception");
    }
}

void MdApi::handle_set_log_config(const shm::FrameView& view) {
    // 定向: instance_id == name_。payload = JSON patch。
    // 流程: decode -> set_log_config -> rtn
    // 失败: catch -> 日志 + notify_ui -> rtn (旧值)
    try {
        log_config_.set_log_config(decode_ext_inst_json<nlohmann::json>(view));
    } catch (const std::exception& e) {
        SPDLOG_ERROR("set log config failed | error=\"{}\"", e.what());
        notify_ui_.error(std::format("日志配置更新失败: {}", e.what()));
    }
    log_config_.rtn_log_config(event_writer_);
}

void MdApi::handle_set_md_shm_config(const shm::FrameView& view) {
    // 定向: 帧头 instance_id == name_。payload = JSON Merge Patch。
    // 流程: decode -> set_shm_config (内部 validate+merge+save) -> rtn
    // 失败: catch -> 日志 + notify_ui -> rtn (旧值)
    try {
        md_shm_config_.set_shm_config(decode_ext_inst_json<nlohmann::json>(view));
    } catch (const std::exception& e) {
        SPDLOG_ERROR("set md shm config failed | error=\"{}\"", e.what());
        notify_ui_.error(std::format("行情通道配置更新失败: {}", e.what()));
    }
    md_shm_config_.rtn_shm_config();
    schedule_md_shm_maintenance();
}

void MdApi::handle_set_auto_login(const shm::FrameView& view) {
    // 定向: 帧头 instance_id == name_。payload = JSON Merge Patch。
    // 流程: decode -> set_auto_login (内部 validate+merge+save) -> rtn
    // 失败: catch -> 日志 + notify_ui -> rtn (旧值)
    try {
        auto_login_config_.set_auto_login(decode_ext_inst_json<nlohmann::json>(view));
        SPDLOG_INFO("set auto login applied | enabled={} schedules={}",
                    auto_login_config_.config()["enabled"].dump(),
                    auto_login_config_.config()["schedules"].size());
    } catch (const std::exception& e) {
        SPDLOG_ERROR("set auto login failed | error=\"{}\"", e.what());
        notify_ui_.error(std::format("自动登录配置更新失败: {}", e.what()));
    }
    auto_login_config_.rtn_auto_login();
}

void MdApi::handle_set_md_config(const shm::FrameView& view) {
    // catch 范围 std::exception: set_md_config 可能抛 std::runtime_error,
    // 统一回 RTN (旧值, 无 error 字段) + notify_ui 错误级别弹窗
    try {
        auto req = decode_ext_inst_json<dztrader::platform::CtpMdConfigOpReq>(view);
        apply_config_change(req);
    } catch (const std::exception& e) {
        SPDLOG_ERROR("config op failed | error=\"{}\"", e.what());
        // 契约 md-config: 失败原因由 NOTIFY_UI (错误级别弹窗) 传达, RTN 回旧值 (无 error 字段)
        notify_ui_.error("配置处理失败: " + std::string(e.what()));
        report_config();
    }
}

void MdApi::handle_query_md_subscriptions(const shm::FrameView& view) {
    sub_manager_.handle_query_md_subscriptions(view);
}

void MdApi::handle_subscribe_req(const shm::FrameView& view) {
    auto result = sub_manager_.handle_subscribe_req(view);
    // 先退订再订阅: replace=true 时同一合约可能同时出现在两个列表中
    if (api_ != nullptr && !result.to_unsubscribe.empty()) {
        batch_unsubscribe(result.to_unsubscribe);
    }
    if (!result.to_subscribe.empty()) {
        if (api_ != nullptr && state_machine_.state() == MdState::LoggedIn) {
            sub_manager_.mark_pending(result.to_subscribe);
            enqueue_batches(std::move(result.to_subscribe));
            if (!sub_check_active_) {
                sub_check_active_ = true;
                send_next_batch();
            }
        } else {
            SPDLOG_WARN("subscribe deferred | reason=\"not_logged_in\" instance_id={} count={}",
                        result.instance_id, result.to_subscribe.size());
        }
    }
}

void MdApi::handle_frame_inner(const std::byte* frame) {
    FrameView view(frame);

    switch (view.type()) {
        // --- 广播帧: 不检查 instance_id ---
        case DZ_FRAME_REQUEST_SHUTDOWN_ALL:
            // 广播: 所有进程退出
            handle_shutdown();
            break;
        case DZ_FRAME_PRELOAD_EVENT_SHM: {
            // 广播帧: 携带 DzShmPreload payload (pages/bytes), 随机延迟后预加载 event 通道
            const auto& params = view.payload<DzShmPreload>();
            schedule_event_shm_preload(params);
            break;
        }
        case DZ_FRAME_UPDATE_SHM_EVENT_SUBSCRIBER:
            // 广播: 所有进程刷新 event writer 缓存
            event_writer_.refresh_subscribers();
            SPDLOG_INFO("event channel subscribers refreshed (broadcast)");
            break;
        case DZ_FRAME_QUERY_FULL_SNAPSHOT:
            report_full_snapshot();
            break;

        // --- 定向帧: 帧头 ext_inst_id == name_ ---
        case DZ_FRAME_SET_LOG_CONFIG:
            if (is_addressed_to_me(view)) {
                handle_set_log_config(view);
            }
            break;
        case DZ_FRAME_FLUSH_LOG:
            if (is_addressed_to_me(view)) {
                spdlog::default_logger()->flush();
                SPDLOG_INFO("log flushed by request");
            }
            break;
        case DZ_FRAME_SET_MD_SHM_CONFIG:
            if (is_addressed_to_me(view)) {
                handle_set_md_shm_config(view);
            }
            break;
        case DZ_FRAME_SET_AUTO_LOGIN:
            if (is_addressed_to_me(view)) {
                handle_set_auto_login(view);
            }
            break;
        case DZ_FRAME_REQUEST_MD_CONNECT:
            if (is_addressed_to_me(view)) {
                connect();
            }
            break;
        case DZ_FRAME_REQUEST_MD_DISCONNECT:
            if (is_addressed_to_me(view)) {
                disconnect();
            }
            break;
        case DZ_FRAME_QUERY_MD_SUBSCRIPTIONS:
            if (is_addressed_to_me(view)) {
                handle_query_md_subscriptions(view);
            }
            break;
        case DZ_FRAME_SET_MD_CONFIG:
            if (is_addressed_to_me(view)) {
                handle_set_md_config(view);
            }
            break;
        case DZ_FRAME_REQUEST_MD_SUBSCRIBE:
            if (is_addressed_to_me(view)) {
                handle_subscribe_req(view);
            }
            break;
        case DZ_FRAME_REQUEST_SHUTDOWN:
            // 定向: 帧头 instance_id 已由前置网关确保 == name_
            if (is_addressed_to_me(view)) {
                handle_shutdown();
            }
            break;
        case DZ_FRAME_UPDATE_SHM_MD_SUBSCRIBER:
            // 定向: 帧头 instance_id == name_, 刷新行情通道 writer
            // spi_.refresh_subscribers() 内部加自旋锁, 与 SPI 线程的 OnRtnDepthMarketData 互斥
            if (is_addressed_to_me(view)) {
                spi_.refresh_subscribers();
                SPDLOG_DEBUG("md channel subscribers refreshed | source={}", name_);
            }
            break;
        default:
            SPDLOG_DEBUG("unknown frame | type={}", static_cast<int>(view.type()));
            break;
    }
}

void MdApi::handle_shutdown() {
    SPDLOG_INFO("shutdown request received, exiting");
    running_ = false;
    // 若已连接则优雅 disconnect，避免 CTP 资源泄漏
    if (api_ != nullptr) {
        try {
            disconnect();
        } catch (const std::exception& e) {
            SPDLOG_WARN("disconnect during shutdown failed | error=\"{}\"", e.what());
        }
    }
}

void MdApi::report_md_status() {
    state_machine_.set_subscription_stats(sub_manager_.expected_subscribe_count(),
                                          sub_manager_.subscribed_count());
    try {
        auto j = build_md_status_payload(state_machine_.status());
        platform::write_ext_inst_json_obj(event_writer_, DZ_FRAME_RTN_MD_STATUS, name_, j);
    } catch (const Exception& e) {
        SPDLOG_ERROR("report_md_status failed | err_code={} msg=\"{}\"", e.code(), e.what());
    } catch (const std::exception& e) {
        SPDLOG_ERROR("report_md_status failed | error=\"{}\"", e.what());
    }
}

void MdApi::report_progress() {
    try {
        const auto& s = state_machine_.status();
        progress_reporter_.set(s.progress_min, s.progress_max,
                               s.progress_current, s.progress_desc);
        progress_reporter_.rtn();
    } catch (const Exception& e) {
        SPDLOG_ERROR("report_progress failed | err_code={} msg=\"{}\"", e.code(), e.what());
    } catch (const std::exception& e) {
        SPDLOG_ERROR("report_progress failed | error=\"{}\"", e.what());
    }
}

void MdApi::notify_ui(const nlohmann::json& field) {
    notify_ui_.notify(field.value("level", DZ_NOTIFY_INFO), field.value("message", std::string{}),
                      field.value("popup", false));
}

void MdApi::report_log_config() { log_config_.rtn_log_config(event_writer_); }

void MdApi::report_md_shm_config() {
    try {
        md_shm_config_.rtn_shm_config();
    } catch (const std::exception& e) {
        SPDLOG_ERROR("rtn_md_shm_config failed | error=\"{}\"", e.what());
    }
}

void MdApi::report_auto_login() {
    try {
        auto_login_config_.rtn_auto_login();
    } catch (const std::exception& e) {
        SPDLOG_ERROR("rtn_auto_login failed | error=\"{}\"", e.what());
    }
}

void MdApi::report_full_snapshot() {
    report_config();
    report_log_config();
    report_md_shm_config();
    report_auto_login();
    report_md_status();
    report_progress();
    SPDLOG_DEBUG("full snapshot reported | name={}", name_);
}

}  // namespace dztrader::ctp
