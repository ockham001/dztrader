/**
 * @file main.cpp
 * @brief dzsysmon 进程入口：事件通道接入、双定时器主循环与优雅退出（契约 sysmon）。
 *
 * 线程模型：
 * - io_context 单线程：全部业务状态（配置镜像、采样基线、定时器）只在它上面访问
 * - 独立信号量监听线程：阻塞在 NamedSemaphore::wait，被 master 唤醒后 post 到
 *   io_context 执行 drain，避免在监听线程中访问业务状态
 *
 * 生命周期：配置自愈 → 日志初始化 → 配置加载 + 平台基线检查 → 打开事件通道 →
 * 首帧上报（配置 + 日志配置 + 快照）→ 计量/推送双定时器 → SHUTDOWN/信号优雅退出。
 */

#include "config.h"
#include "cpu_saturation.h"
#include "cpu_usage.h"
#include "snapshot_builder.h"
#include "timer.h"

#include <dztrader/core/json_section.h>
#include <dztrader/core/path.h>
#include <dztrader/core/this_process.h>
#include <dztrader/log/log.h>
#include <dztrader/platform/frame_codec.h>
#include <dztrader/platform/log_config.h>
#include <dztrader/platform/notify_ui.h>
#include <dztrader/platform/platform_requirements.h>
#include <dztrader/platform/sysmon.h>
#include <dztrader/platform/system_collector.h>
#include <dztrader/shm/channel_meta.h>
#include <dztrader/shm/frame_view.h>
#include <dztrader/shm/reader.h>
#include <dztrader/shm/writer.h>
#include <dztrader/shm/named_semaphore.h>

#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/system/error_code.hpp>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace dztrader::sysmon {
namespace {

/// 墙钟 Unix 毫秒（仅上报口径；定时调度一律用 steady_clock，不受系统时间调整影响）。
int64_t wall_clock_unix_ms() {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
    return static_cast<int64_t>(ms);
}

/// 由每核累计计数器构造快照用的每核条目：本帧使用率未知时为 null，饱和标记由 tracker 写入。
std::vector<platform::SysmonCpuCore> make_core_entries(
    const std::vector<platform::RawCpuCoreTimes>& cores) {
    std::vector<platform::SysmonCpuCore> entries;
    entries.reserve(cores.size());
    for (const auto& core : cores) {
        platform::SysmonCpuCore entry;
        entry.index = core.index;
        entry.usage_ratio = std::nullopt;
        entry.saturated = false;
        entries.push_back(entry);
    }
    return entries;
}

/// dzsysmon 主对象：持有全部运行期状态。非线程安全，只在 io_context 线程访问。
class SysmonApp {
public:
    SysmonApp(std::filesystem::path cfg_path,
              std::string name,
              bool recover,
              nlohmann::json loaded_config)
        : cfg_path_(std::move(cfg_path)),
          name_(std::move(name)),
          recover_(recover),
          signals_(ioc_, SIGINT, SIGTERM),
          measure_timer_(ioc_),
          push_timer_(ioc_),
          config_store_(cfg_path_, std::move(loaded_config)),
          config_(config_store_.current().get<platform::SysmonConfig>()) {
        log_config_ = std::make_unique<platform::LogConfig>(name_, cfg_path_,
                                                            nlohmann::json::json_pointer("/log"));
    }

    SysmonApp(const SysmonApp&) = delete;
    SysmonApp& operator=(const SysmonApp&) = delete;
    SysmonApp(SysmonApp&&) = delete;
    SysmonApp& operator=(SysmonApp&&) = delete;

    ~SysmonApp() { stop_listener(); }

    /// 进程主循环：初始化 → 首帧上报 → 事件循环 → 优雅清理。异常仅可能来自初始化。
    int run() {
        init();
        SPDLOG_INFO("start | name={} cfg={} recover={}", name_, cfg_path_.string(), recover_);

        // 启动初始化完成后立即上报（契约 sysmon §3.2 触发 1 / §4 触发 1）：
        // 全量配置 + 日志配置 + 首帧快照（首帧 cpu.total.usage_ratio 可为 null）。
        push_sysmon_config();
        push_log_config();
        push_snapshot();

        schedule_measure();
        schedule_push();
        arm_signals();
        start_listener();
        SPDLOG_INFO("running | measure_interval_ms={} push_interval_sec={}",
                    config_.sampling.measure_interval_ms, config_.realtime.push_interval_sec);

        // 事件循环异常不退出：监控进程需 7x24 存活，单次回调异常只记日志后继续。
        while (true) {
            try {
                ioc_.run();
                break;
            } catch (const std::exception& e) {
                SPDLOG_CRITICAL("event loop exception, continuing | error=\"{}\"", e.what());
                if (ioc_.stopped()) {
                    break;
                }
            }
        }
        stop_listener();
        SPDLOG_INFO("stop");
        return 0;
    }

private:
    /// 初始化：日志配置自愈、采集器、事件通道（失败抛，由 main 记录 critical 后非 0 退出）。
    void init() {
        log_config_->load();  // 与 spdlog 同步；文件/section 缺失或非法时用默认值并修复

        collector_ = platform::make_system_collector();
        system_info_ = collector_->collect_system_info();
        if (const auto baseline = collector_->collect_cpu_times()) {
            apply_baseline(*baseline);
        } else {
            SPDLOG_WARN("initial cpu sample unavailable | reason=collect_failed");
        }
        refresh_load_average();

        // 事件通道由 master 创建（open_only）；reader/writer 名 = 进程名，
        // 与 master 的 register_subscriber(entry.name) / make_subscriber_name 一致。
        event_meta_ = std::make_shared<shm::ChannelMeta>(
            shm::ChannelMeta::open_only(shm::channel_name(CHANNEL_NAME_EVENT), paths::shm()));
        event_writer_ =
            std::make_unique<shm::MultiWriter>(shm::MultiWriter::create(event_meta_, name_));
        event_reader_ = std::make_unique<shm::Reader>(
            shm::Reader::create(shm::channel_name(CHANNEL_NAME_EVENT), paths::shm(), name_));
        event_sem_ = std::make_unique<shm::NamedSemaphore>(name_);
        notify_ui_ = std::make_unique<platform::NotifyUi>(name_, *event_writer_);
        SPDLOG_INFO("event channel ready | channel={} reader={}", CHANNEL_NAME_EVENT, name_);
    }

    // ===== 定时器 =====

    std::chrono::milliseconds current_measure_interval() const {
        return std::chrono::milliseconds(
            static_cast<int64_t>(config_.sampling.measure_interval_ms));
    }

    std::chrono::milliseconds current_push_interval() const {
        return std::chrono::milliseconds(static_cast<int64_t>(config_.realtime.push_interval_sec) *
                                         1000);
    }

    /// 从当前时刻重新排定计量定时器（配置变更时也走这里，新间隔立即生效）。
    void schedule_measure() {
        measure_deadline_ = std::chrono::steady_clock::now() + current_measure_interval();
        measure_timer_.expires_at(measure_deadline_);
        measure_timer_.async_wait([this](const boost::system::error_code& ec) { on_measure(ec); });
    }

    /// 从当前时刻重新排定推送定时器。
    void schedule_push() {
        push_deadline_ = std::chrono::steady_clock::now() + current_push_interval();
        push_timer_.expires_at(push_deadline_);
        push_timer_.async_wait([this](const boost::system::error_code& ec) { on_push(ec); });
    }

    void restart_timers() {
        schedule_measure();
        schedule_push();
    }

    void on_measure(const boost::system::error_code& ec) {
        if (ec) {
            return;  // 定时器被取消（配置重排/退出）
        }
        try {
            measure_once();
        } catch (const std::exception& e) {
            SPDLOG_ERROR("cpu measure failed | error=\"{}\"", e.what());
        }
        // 按"上个计划 deadline + 间隔"推进：处理耗时与进程停顿时长不累积为频率漂移。
        const auto tick = advance_deadline(measure_deadline_, current_measure_interval(),
                                           std::chrono::steady_clock::now());
        measure_deadline_ = tick.next;
        if (tick.missed > 0) {
            missed_ticks_ += tick.missed;
            SPDLOG_WARN("measure deadline drift | skipped_ticks={} total_missed={}", tick.missed,
                        missed_ticks_);
        }
        measure_timer_.expires_at(measure_deadline_);
        measure_timer_.async_wait(
            [this](const boost::system::error_code& next_ec) { on_measure(next_ec); });
    }

    /// 建立/重建计量基线：记录累计计数器与采样时刻；首帧使用率保持不可用（null），
    /// 每核/速率的可用性能力标记为粘性（一旦确认可用，后续采样失败不改写；契约 sysmon §2.5）。
    void apply_baseline(const platform::RawCpuTimes& times) {
        prev_cpu_times_ = times;
        prev_sample_time_ = std::chrono::steady_clock::now();
        // 可用性（能力）标记是粘性的：一次采样失败不改写「平台是否支持该指标」。
        metrics_.cores_collected = metrics_.cores_collected || !times.cores.empty();
        if (!times.cores.empty()) {
            metrics_.cores = make_core_entries(times.cores);
        }
        metrics_.saturated_cores.clear();
        saturation_tracker_.reset();
        // 可用性键 cpu.context_switches 以系统级上下文切换计数器为准；中断是独立计数器，
        // 不可用时单独输出 null（契约 sysmon §2.3、§2.5）。
        metrics_.rates_collected = metrics_.rates_collected || times.context_switches.has_value();
        metrics_.context_switches_per_sec = std::nullopt;
        metrics_.interrupts_per_sec = std::nullopt;
    }

    /// 本帧未采到任何 CPU 数值：能力标记保持粘性，数值全部置空（不得沿用旧帧冒充本帧，
    /// 契约 sysmon §2.7）。核条目（若有）保留 index，仅把使用率/饱和标记置空。
    void clear_cpu_values() {
        metrics_.total_usage_ratio = std::nullopt;
        for (auto& core : metrics_.cores) {
            core.usage_ratio = std::nullopt;
            core.saturated = false;
        }
        metrics_.saturated_cores.clear();
        metrics_.context_switches_per_sec = std::nullopt;
        metrics_.interrupts_per_sec = std::nullopt;
    }

    /// 采样一次系统负载（Linux 真实 loadavg / Windows 队列长度代理）。
    /// 能力标记粘性；本帧采样失败时数值置 null（不得沿用上一帧冒充本帧，契约 §2.7）。
    void refresh_load_average() {
        const auto load = collector_->collect_load_average();
        if (!load.has_value()) {
            metrics_.load_average = std::nullopt;
            return;
        }
        platform::SysmonLoadAverage average;
        average.one = load->one;
        average.five = load->five;
        average.fifteen = load->fifteen;
        average.semantic = load->semantic;
        metrics_.load_average = average;
        metrics_.load_collected = true;
    }

    /// 采样一次 CPU 计数器并更新使用率/每核/饱和标记/速率/缺口标记。
    void measure_once() {
        const auto now = std::chrono::steady_clock::now();
        auto current = collector_->collect_cpu_times();
        if (!current.has_value()) {
            ++missed_ticks_;     // 采集失败：跳过本次计量并记缺口（契约 sysmon §2.7）
            clear_cpu_values();  // 本帧无值：不得把上一帧数值当本帧发布（能力标记保持粘性）
            SPDLOG_DEBUG("cpu measure skipped | reason=collect_failed");
            refresh_load_average();  // 负载是独立 gauge，CPU 采集失败不影响其采样
            return;
        }
        if (!prev_cpu_times_.has_value()) {
            apply_baseline(*current);  // 首个成功样本只建立基线，不产出速率
            refresh_load_average();
            return;
        }
        const double elapsed_seconds =
            std::chrono::duration<double>(now - prev_sample_time_).count();
        prev_sample_time_ = now;

        // 总使用率与每核使用率（契约 sysmon §2.7：首次/计数器重置不可用，不得用 0 冒充）。
        const auto total_usage = compute_cpu_usage(prev_cpu_times_.value(), current.value());
        if (!current->cores.empty()) {
            const auto core_usages = compute_core_usages(prev_cpu_times_->cores, current->cores);
            metrics_.cores = make_core_entries(current->cores);
            for (std::size_t i = 0; i < metrics_.cores.size(); ++i) {
                metrics_.cores[i].usage_ratio =
                    i < core_usages.size() ? core_usages[i] : std::nullopt;
            }
            metrics_.cores_collected = true;

            // 饱和判定：阈值与连续次数取当前配置（默认 90% / 10 次；运行期修改随配置票）。
            const double threshold_ratio =
                std::clamp(config_.alerts.cpu_sustain_used_pct / 100.0, 0.0, 1.0);
            const auto required_samples = static_cast<uint32_t>(config_.alerts.cpu_sustain_samples);
            saturation_tracker_.update(metrics_.cores, threshold_ratio, required_samples);
            metrics_.saturated_cores = saturation_tracker_.saturated_cores();
        } else {
            // 本帧未采到每核序列：保持能力标记（粘性）与已知核 index，数值/饱和置空，
            // 不得沿用旧帧冒充本帧（契约 sysmon §2.7）。
            for (auto& core : metrics_.cores) {
                core.usage_ratio = std::nullopt;
                core.saturated = false;
            }
            metrics_.saturated_cores.clear();
        }

        // 速率 = 累计计数器差值 / 实际经过秒数（契约 sysmon §2.7）。
        // 能力标记粘性；任一侧计数器不可用/重置时本帧数值为 null，不得沿用旧帧。
        metrics_.context_switches_per_sec = std::nullopt;
        if (current->context_switches.has_value() &&
            prev_cpu_times_->context_switches.has_value()) {
            metrics_.rates_collected = true;
            metrics_.context_switches_per_sec = compute_rate(
                *prev_cpu_times_->context_switches, *current->context_switches, elapsed_seconds);
        }
        metrics_.interrupts_per_sec = std::nullopt;
        if (current->interrupts.has_value() && prev_cpu_times_->interrupts.has_value()) {
            metrics_.interrupts_per_sec =
                compute_rate(*prev_cpu_times_->interrupts, *current->interrupts, elapsed_seconds);
        }

        if (total_usage.has_value()) {
            metrics_.total_usage_ratio = total_usage;
            last_sample_unix_ms_ = wall_clock_unix_ms();
        } else {
            // 计数器重置/Δtotal=0：本次输出不可用（不得沿用旧百分比冒充，契约 sysmon §2.7）。
            metrics_.total_usage_ratio = std::nullopt;
            ++missed_ticks_;
            SPDLOG_DEBUG("cpu measure skipped | reason=counter_unavailable");
        }
        prev_cpu_times_ = std::move(current);  // 无论是否可用都推进基线（重置后重建）
        refresh_load_average();
    }

    void on_push(const boost::system::error_code& ec) {
        if (ec) {
            return;  // 定时器被取消（配置重排/退出）
        }
        push_snapshot();
        event_reader_->release_old_pages();  // 周期推送时顺带回收已读页文件映射
        const auto tick = advance_deadline(push_deadline_, current_push_interval(),
                                           std::chrono::steady_clock::now());
        push_deadline_ = tick.next;
        // 推送漂移只校正频率；sampling.missed_ticks 仅记录计量缺口，不混入推送缺口。
        push_timer_.expires_at(push_deadline_);
        push_timer_.async_wait(
            [this](const boost::system::error_code& next_ec) { on_push(next_ec); });
    }

    // ===== 上报 =====

    /// 推送全量生效配置（RTN_SYSMON_CONFIG，定向 instance_id=进程名）。
    void push_sysmon_config() noexcept {
        try {
            (void)platform::write_ext_inst_json_obj(*event_writer_, DZ_FRAME_RTN_SYSMON_CONFIG,
                                                    name_, config_store_.current());
        } catch (const std::exception& e) {
            SPDLOG_ERROR("push sysmon config failed | error=\"{}\"", e.what());
        }
    }

    /// 推送日志配置（RTN_LOG_CONFIG，定向 instance_id=进程名，契约 log）。
    void push_log_config() noexcept {
        try {
            log_config_->rtn_log_config(*event_writer_);
        } catch (const std::exception& e) {
            SPDLOG_ERROR("push log config failed | error=\"{}\"", e.what());
        }
    }

    /// 推送实时快照（RTN_SYSMON_SNAPSHOT，无 instance_id，仅本次 CPU/系统信息）。
    void push_snapshot() noexcept {
        try {
            const auto snapshot =
                build_snapshot(name_, system_info_, metrics_, config_.sampling.measure_interval_ms,
                               last_sample_unix_ms_, missed_ticks_, wall_clock_unix_ms());
            (void)platform::write_ext_json(*event_writer_, DZ_FRAME_RTN_SYSMON_SNAPSHOT, snapshot);
        } catch (const std::exception& e) {
            SPDLOG_ERROR("push snapshot failed | error=\"{}\"", e.what());
        }
    }

    // ===== 事件通道 =====

    void start_listener() {
        listener_thread_ = std::thread([this]() {
            SPDLOG_INFO("event monitor thread started | mode=semaphore");
            while (!stop_flag_.load(std::memory_order_acquire)) {
                event_sem_->wait();
                if (stop_flag_.load(std::memory_order_acquire)) {
                    break;
                }
                // 投递回 io_context 执行，避免监听线程直接访问业务状态
                boost::asio::post(ioc_, [this]() { drain(); });
            }
            SPDLOG_INFO("event monitor thread exited");
        });
    }

    void stop_listener() {
        stop_flag_.store(true, std::memory_order_release);
        if (event_sem_) {
            event_sem_->notify();  // 唤醒阻塞在 wait 的监听线程检查停止标志
        }
        if (listener_thread_.joinable()) {
            listener_thread_.join();
        }
    }

    /// 排空事件通道已到达的帧。
    void drain() {
        while (!stop_flag_.load(std::memory_order_acquire)) {
            const std::byte* frame = event_reader_->next_frame();
            if (frame == nullptr) {
                break;
            }
            handle_frame(frame);
        }
    }

    void handle_frame(const std::byte* frame) {
        try {
            const shm::FrameView view(frame);
            // 第一层：无 instance_id 的广播帧 / 帧头不足以容纳扩展头的帧。
            switch (view.type()) {
                case DZ_FRAME_QUERY_FULL_SNAPSHOT: {
                    // 契约 sysmon §3.2/§4 完整性收敛：配置 + 日志配置 + 快照一次到位
                    push_sysmon_config();
                    push_log_config();
                    push_snapshot();
                    return;
                }
                case DZ_FRAME_UPDATE_SHM_EVENT_SUBSCRIBER: {
                    // master 注册/注销订阅者后广播：刷新本地 writer 订阅者缓存
                    event_writer_->refresh_subscribers();
                    return;
                }
                default:
                    break;
            }
            // 非扩展帧（basic 帧）不含 instance_id，不可能是定向帧（如 PRELOAD_EVENT_SHM）。
            if (view.frame_size() < sizeof(DzFrameHeader) + sizeof(DzExtInstFrameHeader)) {
                return;
            }
            // 第二层：含 instance_id 帧，只处理发往本进程的（契约 general §5）。
            if (std::string_view(view.ext_inst_id()) != name_) {
                return;
            }
            switch (view.type()) {
                case DZ_FRAME_SHUTDOWN:
                    SPDLOG_INFO("shutdown requested | source=frame");
                    request_stop();
                    return;
                case DZ_FRAME_SET_SYSMON_CONFIG:
                    apply_sysmon_config(view);
                    return;
                case DZ_FRAME_SET_LOG_CONFIG:
                    apply_log_config(view);
                    return;
                case DZ_FRAME_FLUSH_LOG:
                    spdlog::default_logger()->flush();
                    SPDLOG_INFO("log flushed by request");
                    return;
                default:
                    break;
            }
        } catch (const std::exception& e) {
            SPDLOG_ERROR("handle frame failed | error=\"{}\"", e.what());
        } catch (...) {
            SPDLOG_ERROR("handle frame failed | reason=non_std_exception");
        }
    }

    /// SET_SYSMON_CONFIG：应用 patch（校验/持久化失败回滚旧值），成功失败均回推全量配置。
    void apply_sysmon_config(const shm::FrameView& view) {
        try {
            config_store_.set_patch(shm::decode_ext_inst_json<nlohmann::json>(view));
            config_ = config_store_.current().get<platform::SysmonConfig>();
            restart_timers();  // 新间隔立即生效
            SPDLOG_INFO("sysmon config updated | measure_interval_ms={} push_interval_sec={}",
                        config_.sampling.measure_interval_ms, config_.realtime.push_interval_sec);
        } catch (const std::exception& e) {
            // 四件套（契约 sysmon §3.3）：内存回滚由 set_patch 保证 → 日志 → NOTIFY_UI →
            // 回推旧值
            SPDLOG_ERROR("set sysmon config failed | error=\"{}\"", e.what());
            notify_ui_->error(std::format("sysmon config update failed | error=\"{}\"", e.what()));
        }
        push_sysmon_config();
    }

    /// SET_LOG_CONFIG：应用日志 patch（失败回滚），成功失败均回推 RTN_LOG_CONFIG。
    void apply_log_config(const shm::FrameView& view) {
        try {
            log_config_->set_log_config(shm::decode_ext_inst_json<nlohmann::json>(view));
        } catch (const std::exception& e) {
            SPDLOG_ERROR("set log config failed | error=\"{}\"", e.what());
            notify_ui_->error(std::format("log config update failed | error=\"{}\"", e.what()));
        }
        log_config_->rtn_log_config(*event_writer_);
    }

    // ===== 生命周期 =====

    void arm_signals() {
        signals_.async_wait([this](const boost::system::error_code& ec, int sig) {
            if (ec) {
                return;  // 信号集被取消（正常退出路径）
            }
            SPDLOG_INFO("signal received | sig={}", sig);
            request_stop();
        });
    }

    /// 请求优雅退出：置停止标志、取消信号等待并停止 io_context。
    /// 定时器等待无需显式取消：ioc_.stop() 后 pending handler 不再执行，析构时安全释放。
    void request_stop() {
        if (stop_flag_.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        boost::system::error_code ec;
        signals_.cancel(ec);
        ioc_.stop();
        if (event_sem_) {
            event_sem_->notify();  // 唤醒监听线程检查停止标志
        }
    }

    // ===== 成员（声明顺序即构造顺序：ioc_ 必须早于绑定它的定时器/信号集）=====
    std::filesystem::path cfg_path_;
    std::string name_;
    bool recover_ = false;
    boost::asio::io_context ioc_;
    boost::asio::signal_set signals_;
    boost::asio::steady_timer measure_timer_;
    boost::asio::steady_timer push_timer_;
    SysmonConfigStore config_store_;
    platform::SysmonConfig config_;
    std::unique_ptr<platform::LogConfig> log_config_;
    std::unique_ptr<platform::SystemCollector> collector_;
    platform::RawSystemInfo system_info_;
    std::optional<platform::RawCpuTimes> prev_cpu_times_;
    std::chrono::steady_clock::time_point prev_sample_time_{};
    CpuMetrics metrics_;
    CoreSaturationTracker saturation_tracker_;
    int64_t last_sample_unix_ms_ = 0;
    uint64_t missed_ticks_ = 0;
    std::shared_ptr<shm::ChannelMeta> event_meta_;
    std::unique_ptr<shm::MultiWriter> event_writer_;
    std::unique_ptr<shm::Reader> event_reader_;
    std::unique_ptr<shm::NamedSemaphore> event_sem_;
    std::unique_ptr<platform::NotifyUi> notify_ui_;
    std::chrono::steady_clock::time_point measure_deadline_{};
    std::chrono::steady_clock::time_point push_deadline_{};
    std::thread listener_thread_;
    std::atomic<bool> stop_flag_{false};
};

}  // namespace

int run(int argc, char* argv[]) {
    // cmdline: 本票仅接受 --recover（恢复参数由 master 传入；当前只记录日志）。
    bool recover = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--recover") {
            recover = true;
        }
    }
    const std::string name = dztrader::this_process::exe_stem();
    const std::filesystem::path cfg_path = dztrader::paths::configs() / (name + ".json");

    // 1. 配置文件缺失：自愈生成默认值（日志尚未初始化，失败用 stderr）
    if (!std::filesystem::exists(cfg_path)) {
        try {
            generate_default_config(cfg_path);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "dzsysmon: generate default config failed: %s\n", e.what());
            return 1;
        }
    }

    // 2. 用 log section 初始化日志（启动期只需 level/flush_on 字符串）
    nlohmann::json log_json;
    try {
        log_json = dztrader::core::load_json_section<nlohmann::json>(cfg_path, "log");
    } catch (const std::exception& e) {
        std::fprintf(stderr, "dzsysmon: log config load failed: %s\n", e.what());
        return 1;
    }
    std::string log_level = "debug";
    std::string log_flush_on = "info";
    if (log_json.is_object()) {
        log_level = log_json.value("level", log_level);
        log_flush_on = log_json.value("flush_on", log_flush_on);
    } else {
        SPDLOG_WARN("log config missing, using defaults | path={}", cfg_path.string());
    }
    dztrader::log::LoggerSetup log_setup{
        .logger_name = name,
        .log_dir = dztrader::paths::logs(),
        .level =
            dztrader::platform::LogConfig::parse_level(log_level).value_or(spdlog::level::info),
        .flush_level =
            dztrader::platform::LogConfig::parse_level(log_flush_on).value_or(spdlog::level::info),
        .max_files = 30,  // 7x24 长期运行每天一个日志文件，保留 30 天
    };
    dztrader::log::set_default_logger(log_setup);

    // 3. 加载并校验业务配置 + 平台基线检查（失败 fatal）
    nlohmann::json loaded_config;
    try {
        loaded_config = load_config(cfg_path);
    } catch (const std::exception& e) {
        SPDLOG_CRITICAL("config load failed | path={} error=\"{}\"", cfg_path.string(), e.what());
        return 1;
    }
    if (auto baseline_error = dztrader::platform::check_platform_baseline()) {
        SPDLOG_CRITICAL("platform baseline check failed | error=\"{}\"", *baseline_error);
        return 1;
    }

    // 4. 初始化进程对象并运行事件循环（初始化失败 fatal；对象析构在日志关闭前完成）
    int code = 0;
    try {
        {
            SysmonApp app(cfg_path, name, recover, std::move(loaded_config));
            code = app.run();
        }
        spdlog::shutdown();
    } catch (const std::exception& e) {
        SPDLOG_CRITICAL("dzsysmon init failed | error=\"{}\"", e.what());
        spdlog::shutdown();
        return 1;
    }
    return code;
}

}  // namespace dztrader::sysmon

int main(int argc, char* argv[]) { return dztrader::sysmon::run(argc, argv); }
