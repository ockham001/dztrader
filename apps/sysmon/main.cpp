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

#include "alerts.h"
#include "config.h"
#include "cpu_saturation.h"
#include "cpu_usage.h"
#include "disk_io.h"
#include "history_recorder.h"
#include "network.h"
#include "snapshot_builder.h"
#include "timer.h"

#include <dztrader/sysmonstore/history_store.h>
#include <dztrader/sysmonstore/records.h>
#include <dztrader/sysmonstore/schema.h>

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
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <functional>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

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

/// 某一类速率型指标的采样基线：上一次**成功**采样的原始计数器 + 其单调时钟时刻。
///
/// 磁盘 I/O 与网络两个域同形（都按设备/网卡标识匹配基线、都用单调时钟量窗口），
/// 因此共用一份模板而不是各写一个同构 struct（避免 Data Clumps / Duplicated Code）。
/// `valid = false` 表示尚无基线（首次采样或该域刚被重新开启），此时本帧速率全部不可用。
template <typename RawSample>
struct RateBaseline {
    std::vector<RawSample> samples;                ///< 上次成功采样的原始计数器
    std::chrono::steady_clock::time_point time{};  ///< 上次成功采样的单调时钟时刻
    bool valid = false;                            ///< 是否已有基线
};

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
          config_(config_store_.current().get<platform::SysmonConfig>()),
          history_recorder_(make_record_window_config(config_)) {
        log_config_ = std::make_unique<platform::LogConfig>(name_, cfg_path_,
                                                            nlohmann::json::json_pointer("/log"));
    }

    SysmonApp(const SysmonApp&) = delete;
    SysmonApp& operator=(const SysmonApp&) = delete;
    SysmonApp(SysmonApp&&) = delete;
    SysmonApp& operator=(SysmonApp&&) = delete;

    /// 析构兜底：无论 `run()` 是否走完正常退出路径，都停掉监听线程并 flush 历史。
    /// `run()` 正常结束时会先调 `shutdown_history()`；这里对「run() 中途抛异常」
    /// 的路径补一次（`shutdown_history` 自身幂等：writer 已被 reset 时直接返回）。
    ~SysmonApp() {
        stop_listener();
        shutdown_history();
    }

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
        shutdown_history();
        SPDLOG_INFO("stop");
        return 0;
    }

    /// 优雅退出：把当前未结束的记录窗口落盘，再 flush 队列并关库。
    ///
    /// 顺序是语义的一部分：先产出一批（把最后一个不完整窗口的样本交给落盘线程），
    /// 再让落盘线程冲空队列并关库。不这么做会丢掉「退出前最后不到一个周期的数据」，
    /// 而这段数据恰恰是排查崩溃/退出原因最需要的部分。
    void shutdown_history() noexcept {
        if (history_writer_ == nullptr) {
            return;
        }
        try {
            auto batch = history_recorder_.flush(wall_clock_unix_ms() / 1000);
            if (!batch.empty()) {
                history_writer_->enqueue(std::move(batch));
            }
        } catch (const std::exception& e) {
            SPDLOG_ERROR("history final window flush failed | error=\"{}\"", e.what());
        }
        // 先 flush 再读计数：flush 会冲掉队列里剩余的批次，之前读会少算（日志说谎）。
        const size_t failed = history_writer_->flush();  // 冲空队列 → checkpoint → 关库
        SPDLOG_INFO(
            "history writer stopped | written_records={} dropped_records={} "
            "failed_records={}",
            history_writer_->written_records(), history_writer_->dropped_records(), failed);
        history_writer_.reset();
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
        refresh_sensor_metrics();
        refresh_memory_metrics();
        refresh_disk_metrics();
        refresh_network();

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

        init_history();
        // 首帧就可携带活动告警（启动时资源可能已经越限）：在事件通道与通知器就绪后评估一次。
        evaluate_alerts();
    }

    /// 记录窗口配置（来自 sampling 段；两个字段都在配置校验里钳制过范围）。
    static RecordWindowConfig make_record_window_config(const platform::SysmonConfig& config) {
        RecordWindowConfig window;
        window.interval_sec = config.sampling.record_interval_sec;
        window.phase_sec = config.sampling.phase_offset_sec;
        window.measure_interval_ms = config.sampling.measure_interval_ms;
        return window;
    }

    /// 打开历史库并启动落盘线程（`history.enabled = false` 时完全不碰库）。
    ///
    /// 失败**不**阻止进程启动：历史是观测辅助，实时采集与推送必须照常
    /// （7x24 进程不应因为磁盘满或库版本不符而起不来）。失败记 ERROR 并降级为
    /// 「不写历史」，`history_writer_` 保持空指针，后续所有写入路径都短路。
    void init_history() {
        if (!config_.history.enabled) {
            SPDLOG_INFO("history disabled by config | action=realtime_only");
            return;
        }
        const auto db_path = dztrader::paths::db() / std::string(sysmonstore::kHistoryFileName);
        try {
            sysmonstore::HistoryWriterOptions options;
            options.queue_capacity = 256;
            options.maintenance_interval_sec = 300;
            // 保留策略从**发布出来的快照**读取，而不是直接读 `config_`：
            // 这个 lambda 跑在落盘线程上，而 `config_` 会在 io_context 线程上被
            // `SET_SYSMON_CONFIG` 整体重写（含 std::map 成员）——直接读是数据竞争
            // （UB，配置更新瞬间可能崩）。发布快照在 io 线程写、落盘线程读，用互斥量同步。
            publish_retention_config();
            auto retention = [this]() { return retention_config_snapshot(); };
            history_writer_ = std::make_unique<sysmonstore::HistoryWriter>(db_path, options,
                                                                           std::move(retention));
            SPDLOG_INFO("history writer started | path={} queue_capacity={}", db_path.string(),
                        options.queue_capacity);
            // 恢复参数语义：历史连续性优先——不重置数据文件、不丢已有聚合。
            // 重启窗口标记为「采集中断」，让读取方知道这段没有可信数据（绝不伪造连续）。
            // 与采集期的标记不同：这条必须**立即落盘**，不能等第一个记录窗口结算
            // （默认 60 秒；若进程在窗口结算前再崩一次，重启事实就永久丢了）。
            mark_restart();
            flush_pending_marks();
        } catch (const std::exception& e) {
            SPDLOG_ERROR("history unavailable, continue realtime only | path={} error=\"{}\"",
                         db_path.string(), e.what());
            notify_ui_->error(std::format("sysmon history unavailable | error=\"{}\"", e.what()));
        }
    }

    /// 立即把待落盘标记写成独立批次（不等待记录窗口结算）。
    ///
    /// 用于「启动时的重启标记」这类**必须马上持久化**的事实：如果只塞进
    /// `pending_marks_` 等第一个窗口结算，进程在窗口结束前崩溃就会丢掉这条记录，
    /// 而它恰恰是排查反复重启的关键。
    void flush_pending_marks() {
        if (history_writer_ == nullptr) {
            return;
        }
        auto batch = history_recorder_.drain_marks();
        if (!batch.empty()) {
            history_writer_->enqueue(std::move(batch));
        }
    }

    /// 把当前配置里与保留策略相关的最小数集发布给落盘线程（io_context 线程调用）。
    ///
    /// 只发布 `RetentionPolicy` 需要的字段，而不是共享整个 `config_`：跨线程传递的
    /// 数据面越小，越不容易在将来加字段时重新引入竞争。
    void publish_retention_config() {
        sysmonstore::RetentionPolicy policy;
        policy.raw_retention_sec = static_cast<int64_t>(config_.history.raw_retention_hours) * 3600;
        policy.max_retention_sec = static_cast<int64_t>(config_.history.max_retention_days) * 86400;
        policy.record_interval_sec = config_.sampling.record_interval_sec;
        policy.max_bytes = static_cast<int64_t>(config_.history.max_bytes);
        std::lock_guard lock(retention_config_mutex_);
        retention_config_ = policy;
    }

    /// 落盘线程读取已发布的保留配置；`now_ts` 由采集侧时钟给出（每次维护刷新）。
    sysmonstore::RetentionPolicy retention_config_snapshot() const {
        std::lock_guard lock(retention_config_mutex_);
        auto policy = retention_config_;
        policy.now_ts = wall_clock_unix_ms() / 1000;
        return policy;
    }

    /// 写入「进程重启」标记：恢复参数语义的核心（重启窗口标记为采集中断）。
    ///
    /// 自动重启（`--recover`）与人工重启都记：两者的共同事实是「进程曾经不在」，
    /// 期间的采集数据缺失。带上 `recover` 让读取方能区分是否异常重启。
    void mark_restart() {
        const int64_t now_ts = wall_clock_unix_ms() / 1000;
        sysmonstore::HistoryMark mark;
        mark.ts = now_ts;
        mark.kind = std::string(sysmonstore::kMarkRestart);
        // detail 记录**缺口秒数**（`<restart_kind>/<gap_sec>`），没有历史数据时 gap 为空：
        // 「重启了」这句话本身信息量有限，读取方真正要知道的是「这段有多久没有数据」。
        // 缺口 = 现在 − 库中最后一条记录的时刻；查不到（首次启动/库刚建）就不写数字，
        // 绝不猜一个 0 冒充「没有缺口」。
        std::string detail = recover_ ? "recover" : "manual";
        if (const auto gap = history_gap_seconds(now_ts); gap.has_value()) {
            detail += "/" + std::to_string(*gap);
        }
        mark.detail = std::move(detail);
        history_recorder_.add_mark(std::move(mark));
    }

    /// 查历史库中最后一条记录距 `now_ts` 的秒数（无记录/库不可读时 `nullopt`）。
    ///
    /// 用只读连接查：写入连接归落盘线程独占，不能在 io_context 线程上碰。
    /// 查询失败只是「算不出缺口」，不影响重启标记本身。
    ///
    /// 只在进程初始化阶段（`init_history`）调用一次，因此这里的同步查询不会阻塞
    /// 事件循环；走 `MAX(bucket_ts)` 索引而不读整段历史。
    std::optional<int64_t> history_gap_seconds(int64_t now_ts) const {
        try {
            const auto db_path = dztrader::paths::db() / std::string(sysmonstore::kHistoryFileName);
            auto opened = sysmonstore::HistoryReader::open(db_path);
            if (opened.first != sysmonstore::HistoryAvailability::Ok) {
                return std::nullopt;
            }
            const auto latest = opened.second->latest_bucket_ts(sysmonstore::kLevelRaw);
            if (!latest.has_value()) {
                return std::nullopt;
            }
            // 负值（时钟被回拨）也照实返回：它本身就是要上报的事实，不钳成 0。
            return now_ts - *latest;
        } catch (const std::exception& e) {
            // 用 WARN 而不是 DEBUG：DEBUG 在 Release 下被编译掉，`e` 就成了未引用变量
            // （MSVC C4101 在 /WX 下直接失败），而且「算不出重启缺口」确实是值得留痕的异常。
            SPDLOG_WARN("history gap query failed | error=\"{}\"", e.what());
            return std::nullopt;
        }
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
        // 历史记录：每个计量 tick 观测一次（记录窗口的 min/max 需要全部样本，
        // 只观测推送时刻会系统性漏掉尖峰）。与推送解耦，见 record_history_tick。
        // 跳过的 tick 数也交给记录器：它是「错过 tick」这一事实的直接证据。
        history_recorder_.note_missed_ticks(tick.missed);
        record_history_tick();
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

    /// 配置组开关（契约 sysmon §3.1）：缺少的键按 true 处理（默认全部启用）。
    bool group_enabled(const char* group) const {
        const auto it = config_.sampling.groups.find(group);
        return it == config_.sampling.groups.end() || it->second;
    }

    /// 采样一次温度传感器（#73）。温度为瞬时 gauge，与 CPU 采集成败无关；能力标记粘性，
    /// 本帧读不到时数值置 null，不沿用上一帧；配置关闭时清空数值并标记 disabled（契约 §2.7）。
    void refresh_temperatures() {
        if (!group_enabled(platform::SYSMON_GROUP_TEMPERATURE)) {
            temperature_metrics_.disabled = true;
            temperature_metrics_.sensors = std::nullopt;
            return;
        }
        temperature_metrics_.disabled = false;
        const auto readings = collector_->collect_temperatures();
        if (!readings.has_value()) {
            temperature_metrics_.sensors = std::nullopt;  // 本帧无值；能力标记保持粘性
            return;
        }
        std::vector<platform::SysmonTemperatureInfo> sensors;
        sensors.reserve(readings->size());
        for (const auto& reading : *readings) {
            platform::SysmonTemperatureInfo sensor;
            sensor.sensor = reading.sensor;
            sensor.celsius = reading.celsius;
            sensors.push_back(std::move(sensor));
        }
        temperature_metrics_.sensors = std::move(sensors);
        temperature_metrics_.collected = true;
    }

    /// 采样一次每核频率（#73）；语义同温度（瞬时 gauge、能力粘性、配置门控）。
    void refresh_core_frequencies() {
        if (!group_enabled(platform::SYSMON_GROUP_CPU_FREQUENCY)) {
            metrics_.frequency_disabled = true;
            metrics_.core_frequencies = std::nullopt;
            return;
        }
        metrics_.frequency_disabled = false;
        const auto readings = collector_->collect_core_frequencies();
        if (!readings.has_value()) {
            metrics_.core_frequencies = std::nullopt;
            return;
        }
        std::vector<platform::SysmonCoreFrequency> frequencies;
        frequencies.reserve(readings->size());
        for (const auto& reading : *readings) {
            platform::SysmonCoreFrequency frequency;
            frequency.index = reading.index;
            frequency.mhz = reading.mhz;
            frequencies.push_back(frequency);
        }
        metrics_.core_frequencies = std::move(frequencies);
        metrics_.frequency_collected = true;
    }

    /// 采样温度与每核频率；与 CPU 采集相互独立，每次计量都刷新。
    void refresh_sensor_metrics() {
        refresh_temperatures();
        refresh_core_frequencies();
    }

    /// 采样一次内存与交换（#74；瞬时 gauge，能力标记粘性，配置组可关闭）。
    /// 本帧读不到只清数值、不改写可用性；无交换/无页面文件由 `swap_supported` 区分
    /// （快照可用性映射为 unsupported；契约 sysmon §2.4、§2.5）。
    void refresh_memory_metrics() {
        if (!group_enabled(platform::SYSMON_GROUP_MEMORY)) {
            memory_metrics_.disabled = true;
            memory_metrics_.memory = std::nullopt;
        } else {
            memory_metrics_.disabled = false;
            const auto memory = collector_->collect_memory();
            memory_metrics_.memory = memory;
            if (memory.has_value()) {
                memory_metrics_.collected = true;
            }
        }
        if (!group_enabled(platform::SYSMON_GROUP_SWAP)) {
            memory_metrics_.swap_disabled = true;
            memory_metrics_.swap = std::nullopt;
        } else {
            memory_metrics_.swap_disabled = false;
            const auto swap = collector_->collect_swap();
            memory_metrics_.swap = swap;
            if (swap.has_value()) {
                memory_metrics_.swap_collected = true;
                // 以本帧读数更新「确有交换」事实：运行时移除交换后可用性随之变为
                // unsupported（契约 sysmon §2.5）；本帧读取失败（nullopt）保持上次能力标记。
                memory_metrics_.swap_supported = swap->total_bytes > 0;
            }
        }
    }

    /// 采样一次卷容量、关注路径落卷与共享内存目录占用（#75）。
    ///
    /// 卷/目录能力标记粘性：一次成功后 available 保持 true，本帧失败只把域置 null
    /// （契约 sysmon §2.5）。单个卷不可读不改写整体可用性——采集层把该卷以
    /// "容量字段为 null + reason" 放进数组（契约 sysmon §2.4）。
    /// `watch_paths` 不受 `sampling.groups.volumes` 影响（路径感知告警需要它，见 #80）。
    void refresh_disk_metrics() {
        if (!group_enabled("volumes")) {
            disk_metrics_.volumes_disabled = true;
            disk_metrics_.volumes = std::nullopt;
        } else {
            disk_metrics_.volumes_disabled = false;
            const auto volumes = collector_->collect_volumes();
            disk_metrics_.volumes = volumes;
            if (volumes.has_value()) {
                disk_metrics_.volumes_collected = true;
            }
        }
        // 关注路径：配置非空以其为准，否则用默认的共享内存目录与数据库目录
        // （契约 sysmon §2.4、§3.1）。
        disk_metrics_.watch_paths = reconcile_watch_paths(
            collector_->collect_watch_paths(resolve_watch_paths()), disk_metrics_.volumes);

        const auto shm_dir = collector_->collect_shm_dir(dztrader::paths::shm().string());
        disk_metrics_.shm_dir = shm_dir;
        if (shm_dir.has_value()) {
            disk_metrics_.shm_dir_collected = true;
        }

        refresh_disk_io();
    }

    /// 采样一次每物理盘 I/O（#76）：原始累计计数器 + 单调时钟测得的实际经过时间驱动纯函数。
    ///
    /// 配置关闭优先；本帧采集失败只清数值、能力标记保持粘性；本帧新出现的设备无基线，
    /// 派生字段为 null（契约 sysmon §2.7：不得沿用上一帧、不得用 0 冒充）。
    /// 失败原因由采集层随结果返回（Linux collect_failed / Windows no_permission），
    /// 快照构造直接映射，不按平台名猜（契约 sysmon §2.5）。
    void refresh_disk_io() {
        if (!group_enabled("disk_io")) {
            disk_metrics_.disk_io_disabled = true;
            disk_metrics_.disk_io = std::nullopt;
            // 重新开启时从新基线开始：关闭期间的间隔不能被当成当前速率的计量窗口。
            disk_io_baseline_ = {};
            return;
        }
        disk_metrics_.disk_io_disabled = false;
        const auto now = std::chrono::steady_clock::now();
        const auto raw = collector_->collect_disk_io();
        if (!raw.unavailable_reason.empty()) {
            disk_metrics_.disk_io = std::nullopt;  // 本帧无值；能力标记保持粘性
            disk_metrics_.disk_io_unavailable_reason = raw.unavailable_reason;
            return;
        }
        disk_metrics_.disk_io_collected = true;
        // 速率窗口 = 两次成功采样之间实际经过的秒数（含采集失败时段，口径见契约 §2.7）。
        double elapsed_seconds = 0.0;
        if (disk_io_baseline_.valid) {
            elapsed_seconds = std::chrono::duration<double>(now - disk_io_baseline_.time).count();
        }
        const std::vector<platform::RawDiskIoInfo> no_baseline;
        disk_metrics_.disk_io =
            compute_disk_io(disk_io_baseline_.valid ? disk_io_baseline_.samples : no_baseline,
                            raw.disks, elapsed_seconds);
        disk_io_baseline_.samples = raw.disks;
        disk_io_baseline_.time = now;
        disk_io_baseline_.valid = true;
    }

    /// 采样一次每网卡累计计数器（#77）：原始计数器 + 单调时钟实际经过时间驱动纯函数。
    ///
    /// 配置关闭优先；本帧采集失败只清数值、能力标记保持粘性；本帧新出现的网卡无基线，
    /// 六个速率字段为 null（契约 sysmon §2.7：不得沿用上一帧、不得用 0 冒充）。
    /// 采集层始终返回**全部**网卡（含 `is_virtual`），`hide_virtual` 的过滤在快照构造阶段
    /// 完成——因此改开关不需要重新采集，也不影响这里的速率基线（契约 sysmon §2.4、§2.7）。
    void refresh_network() {
        if (!group_enabled("network")) {
            network_metrics_.network_disabled = true;
            network_metrics_.network = std::nullopt;
            // 重新开启时从新基线开始：关闭期间的间隔不能被当成当前速率的计量窗口。
            network_baseline_ = {};
            return;
        }
        network_metrics_.network_disabled = false;
        const auto now = std::chrono::steady_clock::now();
        const auto raw = collector_->collect_network();
        if (!raw.unavailable_reason.empty()) {
            network_metrics_.network = std::nullopt;  // 本帧无值；能力标记保持粘性
            network_metrics_.network_unavailable_reason = raw.unavailable_reason;
            return;
        }
        network_metrics_.network_collected = true;
        // 速率窗口 = 两次成功采样之间实际经过的秒数（含采集失败时段，口径见契约 §2.7）。
        double elapsed_seconds = 0.0;
        if (network_baseline_.valid) {
            elapsed_seconds = std::chrono::duration<double>(now - network_baseline_.time).count();
        }
        const std::vector<platform::RawNetworkInfo> no_baseline;
        network_metrics_.network =
            compute_network_rates(network_baseline_.valid ? network_baseline_.samples : no_baseline,
                                  raw.interfaces, elapsed_seconds);
        network_baseline_.samples = raw.interfaces;
        network_baseline_.time = now;
        network_baseline_.valid = true;
    }

    /// 把落卷结果与同帧卷列表对齐（契约 sysmon §7：available=true 的 watch_path 必须指向
    /// 同帧 volumes 里存在的挂载点）。
    ///
    /// 卷枚举与落卷解析是两次独立的 /proc 挂载表快照（契约把两者放在不同采集中），
    /// 两者之间发生挂载表变化时落卷可能指向一个本帧未列出的挂载点。若照实上报，
    /// dzweb 的契约校验会拒掉**整帧快照**（连 CPU/内存一起丢），对 7x24 监控代价太大；
    /// 因此这里把对不上的条目降级为"本帧未解析成功"（不伪造卷，也不拖垮整帧）。
    std::optional<std::vector<platform::RawWatchPathInfo>> reconcile_watch_paths(
        std::optional<std::vector<platform::RawWatchPathInfo>> watch_paths,
        const std::optional<std::vector<platform::RawVolumeInfo>>& volumes) {
        // volumes 为 null（组关闭或本帧未采集）时契约不做交叉校验，原样上报。
        if (!watch_paths.has_value() || !volumes.has_value()) {
            return watch_paths;
        }
        std::set<std::string> mounts;
        for (const auto& volume : *volumes) {
            mounts.insert(volume.mount);
        }
        bool reconciled = false;
        for (auto& info : *watch_paths) {
            if (info.mount.has_value() && !mounts.contains(*info.mount)) {
                info.mount = std::nullopt;
                info.reason = "collect_failed";
                reconciled = true;
            }
        }
        if (reconciled) {
            // 挂载表在两次读取之间变化：罕见但真实（umount/mount 竞态），记日志便于诊断。
            SPDLOG_WARN("watch path mount missing from volume list | action=downgraded");
        }
        return watch_paths;
    }

    /// 评估一次阈值告警（契约 sysmon §2.8；#80）。
    ///
    /// 输入只取判定所需的事实：内存/交换使用率、关注卷剩余空间与关注路径落卷；
    /// 阈值与冷却取当前生效配置（本票只读配置值，运行期修改语义随 #81）。
    /// 通知发送（error+popup 触发 / info 恢复）与快照 `alerts[]` 的活动集在此统一产出，
    /// 是告警状态的唯一写入点——采集/判定/发送都在 io_context 线程上串行。
    void evaluate_alerts() {
        // 判定输入（路径感知只把关注卷交给判定）与状态机推进都走纯逻辑模块，便于确定性测试。
        const auto inputs = make_alert_inputs(memory_metrics_, disk_metrics_,
                                              wall_clock_unix_ms() / 1000, steady_clock_ms());
        const auto result = alert_evaluator_.evaluate(inputs, config_.alerts);
        for (const auto& notification : result.notifications) {
            if (notification.recovery) {
                SPDLOG_INFO("threshold alert cleared | kind={} subject={}", notification.kind,
                            notification.subject);
                notify_ui_->info(notification.message);
            } else {
                // 触发必须是 error + popup：契约 notify-ui 只有 error+popup 会打断用户，
                // 阈值告警是"资源即将耗尽、进程可能崩溃"的前置信号（契约 sysmon §2.8）。
                SPDLOG_WARN("threshold alert triggered | kind={} subject={}", notification.kind,
                            notification.subject);
                notify_ui_->error(notification.message);
            }
        }
        active_alerts_.clear();
        active_alerts_.reserve(result.active.size());
        for (const auto& alert : result.active) {
            platform::SysmonAlertInfo info;
            info.kind = alert.kind;
            info.subject = alert.subject;
            info.since = alert.since_unix_sec;
            active_alerts_.push_back(std::move(info));
        }
    }

    /// 关注路径列表：配置 `disk.watch_paths` 非空时以其为准，否则用默认的
    /// 共享内存目录与数据库目录（契约 sysmon §3.1）。
    std::vector<std::string> resolve_watch_paths() const {
        if (!config_.disk.watch_paths.empty()) {
            return config_.disk.watch_paths;
        }
        return {dztrader::paths::shm().string(), dztrader::paths::db().string()};
    }

    /// 把当前快照事实聚合进历史记录窗口（#78）。每次计量 tick 调用一次。
    ///
    /// 与推送解耦：记录间隔（默认 60s）与推送间隔（默认 5s）无关，且**每个计量 tick
    /// 都观测一次**（记录窗口的 min/max 才有意义——只观测推送时刻会让窗口内 11/12 的
    /// 样本丢失，尖峰被系统性漏掉）。
    ///
    /// 时钟跳变检测：采样定时器走单调时钟，墙钟与单调时钟的推进差异超阈值即判定跳变。
    /// 跳变会让墙钟桶边界错乱，必须打标记——否则读取方会把 NTP 步进误读成负载突变
    /// （#41 用户故事 16）。
    void record_history_tick() {
        if (history_writer_ == nullptr) {
            return;  // 历史关闭或打开失败：实时路径完全不受影响
        }
        const int64_t wall_ms = wall_clock_unix_ms();
        const int64_t steady_ms = steady_clock_ms();

        // 检测并标记时钟跳变（首次 tick 没有基线，跳过检测）。
        if (prev_wall_ms_ != 0) {
            if (const auto drift = sysmonstore::detect_clock_jump(
                    prev_wall_ms_, prev_steady_ms_, wall_ms, steady_ms, kClockJumpThresholdMs);
                drift.has_value()) {
                sysmonstore::HistoryMark mark;
                mark.ts = wall_ms / 1000;
                mark.kind = std::string(sysmonstore::kMarkClockJump);
                mark.detail = std::to_string(*drift);
                history_recorder_.add_mark(std::move(mark));
                SPDLOG_WARN("wall clock jump detected | drift_ms={}", *drift);
            }
        }
        prev_wall_ms_ = wall_ms;
        prev_steady_ms_ = steady_ms;

        try {
            const auto snapshot = build_snapshot(
                name_, system_info_, metrics_, temperature_metrics_, memory_metrics_, disk_metrics_,
                network_metrics_, active_alerts_, config_.sampling.measure_interval_ms,
                last_sample_unix_ms_, missed_ticks_, wall_ms);
            auto batch = history_recorder_.observe(snapshot, wall_ms / 1000);
            if (!batch.empty()) {
                enqueue_history(std::move(batch));
            }
        } catch (const std::exception& e) {
            // 历史记录失败不影响采集与推送（7x24 进程不能因观测辅助链路拖垮主职责）。
            SPDLOG_ERROR("history record tick failed | error=\"{}\"", e.what());
        }
    }

    /// 把批次交给落盘线程（入队不阻塞、不抛；队列满丢最旧并计数）。
    void enqueue_history(sysmonstore::HistoryBatch batch) {
        if (history_writer_ == nullptr) {
            return;
        }
        history_writer_->enqueue(std::move(batch));
        const uint64_t dropped = history_writer_->dropped_records();
        if (dropped != last_reported_dropped_) {
            SPDLOG_WARN("history queue overflow, oldest records dropped | dropped_total={}",
                        dropped);
            last_reported_dropped_ = dropped;
        }
        const uint64_t failed = history_writer_->failed_records();
        if (failed != last_reported_failed_) {
            SPDLOG_ERROR("history write failures | failed_total={}", failed);
            last_reported_failed_ = failed;
        }
    }

    /// 单调时钟毫秒（时钟跳变检测的参照；与墙钟同一次 tick 读取）。
    static int64_t steady_clock_ms() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    /// 采样一次 CPU 计数器并更新使用率/每核/饱和标记/速率/缺口标记。
    void measure_once() {
        const auto now = std::chrono::steady_clock::now();
        refresh_sensor_metrics();  // 温度/频率是独立 gauge，CPU 采集失败也照样刷新
        refresh_memory_metrics();  // 内存/交换同为独立 gauge，与 CPU 采集成败无关
        refresh_disk_metrics();    // 卷/落卷/共享内存目录同为独立事实，与 CPU 采集成败无关
        refresh_network();         // 网卡累计计数器同为独立事实，与 CPU 采集成败无关
        // 告警判定与历史记录同频（每个计量 tick）：不跟随推送频率，CPU 采集失败也不跳过。
        evaluate_alerts();
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
            metrics_.context_switches_per_sec =
                compute_rate(*prev_cpu_times_->context_switches, *current->context_switches,
                             elapsed_seconds, kCpuCounterWrapSpan);
        }
        metrics_.interrupts_per_sec = std::nullopt;
        if (current->interrupts.has_value() && prev_cpu_times_->interrupts.has_value()) {
            metrics_.interrupts_per_sec =
                compute_rate(*prev_cpu_times_->interrupts, *current->interrupts, elapsed_seconds,
                             kCpuCounterWrapSpan);
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

    /// 推送实时快照（RTN_SYSMON_SNAPSHOT，无 instance_id，含系统信息/CPU/温度/内存/磁盘/网络）。
    void push_snapshot() noexcept {
        try {
            // hide_virtual 只影响输出条目（采集层始终返回全部网卡），构造快照时读当前配置，
            // 因此 SET_SYSMON_CONFIG 后的这次推送即体现新开关（契约 sysmon §2.7）。
            network_metrics_.hide_virtual = config_.network.hide_virtual;
            const auto snapshot = build_snapshot(
                name_, system_info_, metrics_, temperature_metrics_, memory_metrics_, disk_metrics_,
                network_metrics_, active_alerts_, config_.sampling.measure_interval_ms,
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
    ///
    /// 成功后立即推送一次快照：`network.hide_virtual` 的过滤发生在快照构造阶段，
    /// 立即推送使开关生效体现在下一帧（契约 sysmon §2.7），不必等下一个推送定时器。
    void apply_sysmon_config(const shm::FrameView& view) {
        bool applied = false;
        try {
            config_store_.set_patch(shm::decode_ext_inst_json<nlohmann::json>(view));
            config_ = config_store_.current().get<platform::SysmonConfig>();
            restart_timers();  // 新间隔立即生效
            // 记录窗口也要跟随：record_interval_sec / phase_offset_sec 影响历史落库粒度。
            // 不重新下发的话，运行期改记录间隔会静默不生效（配置声称生效、历史却按旧粒度写）。
            history_recorder_.reconfigure(make_record_window_config(config_));
            publish_retention_config();  // 保留策略参数同样即时生效（下一维护周期）
            applied = true;
            SPDLOG_INFO("sysmon config updated | measure_interval_ms={} push_interval_sec={}",
                        config_.sampling.measure_interval_ms, config_.realtime.push_interval_sec);
        } catch (const std::exception& e) {
            // 四件套（契约 sysmon §3.3）：内存回滚由 set_patch 保证 → 日志 → NOTIFY_UI →
            // 回推旧值
            SPDLOG_ERROR("set sysmon config failed | error=\"{}\"", e.what());
            notify_ui_->error(std::format("sysmon config update failed | error=\"{}\"", e.what()));
        }
        push_sysmon_config();
        if (applied) {
            push_snapshot();  // hide_virtual 等快照侧配置即时生效（契约 sysmon §2.7）
        }
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
    TemperatureMetrics temperature_metrics_;
    MemorySwapMetrics memory_metrics_;
    DiskMetrics disk_metrics_;
    /// 每物理盘 I/O 的采样基线（上一次成功采样与其单调时钟时刻）。
    RateBaseline<platform::RawDiskIoInfo> disk_io_baseline_;
    NetworkMetrics network_metrics_;
    /// 每网卡的采样基线（上一次成功采样与其单调时钟时刻）。
    RateBaseline<platform::RawNetworkInfo> network_baseline_;
    /// 阈值告警状态机（#80）：触发/冷却/恢复状态，只在 io_context 线程访问。
    AlertEvaluator alert_evaluator_;
    /// 当前活动告警（快照 `alerts[]` 的投影；由 `evaluate_alerts` 刷新）。
    std::vector<platform::SysmonAlertInfo> active_alerts_;
    CoreSaturationTracker saturation_tracker_;
    int64_t last_sample_unix_ms_ = 0;
    uint64_t missed_ticks_ = 0;
    /// 历史记录窗口累加器（#78）：只做聚合，不碰 I/O。
    HistoryRecorder history_recorder_;
    // ---- 保留策略的跨线程发布面（**必须声明在 history_writer_ 之前**）----
    // 成员按声明逆序析构：落盘线程在 ~HistoryWriter 里被 join，而它退出前可能仍在
    // 调用保留策略 lambda（→ 读 retention_config_）。若这两个成员声明在 writer 之后，
    // 它们会先被销毁，join 中的工作线程就会读到已析构的互斥量（UB）。
    /// 跨线程发布给落盘线程的保留策略参数（io 线程写、落盘线程读）。
    mutable std::mutex retention_config_mutex_;
    sysmonstore::RetentionPolicy retention_config_;
    /// 历史落盘线程（#78）：有界队列 + 批量提交；nullptr = 历史关闭或打开失败。
    std::unique_ptr<sysmonstore::HistoryWriter> history_writer_;
    /// 历史 tick 的时钟基线（墙钟与单调时钟各一份，用于时钟跳变检测）。
    int64_t prev_wall_ms_ = 0;
    int64_t prev_steady_ms_ = 0;
    /// 上次已上报的丢弃计数（避免队列溢出时每 tick 刷一条日志）。
    uint64_t last_reported_dropped_ = 0;
    /// 上次已上报的落盘失败计数。
    uint64_t last_reported_failed_ = 0;
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
