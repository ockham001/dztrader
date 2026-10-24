#include "history_recorder.h"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace dztrader::sysmon {

namespace {

/// 上下文：把「当前窗口的标量与维度累加状态」和「指标键 → 槽位」的解析放在一起，
/// 使 observe_values 的每个指标只写一行、不需要手写下标字面量。
///
/// 手写下标（`scalars_[9]`）会在往 schema 注册表插指标时静默错位——这是本类最容易
/// 出错的地方，所以改成按键查表：查不到就跳过并让写入方在落库时告警。
class WindowAccumulator {
public:
    WindowAccumulator(
        std::array<sysmonstore::MetricAggregate, sysmonstore::kScalarMetrics.size()>& scalars,
        std::vector<HistoryRecorder::DimensionSlot>& dimensions)
        : scalars_(scalars),
          dimensions_(dimensions) {}

    /// 观测一个可空标量；`nullopt` = 本 tick 不可用 → 不观测（不用 0 冒充）。
    void scalar(std::string_view key, const std::optional<double>& value) {
        if (value.has_value()) {
            scalar(key, *value);
        }
    }

    /// 观测一个可空整数标量（字节数/计数）。
    void scalar(std::string_view key, const std::optional<uint64_t>& value) {
        if (value.has_value()) {
            scalar(key, static_cast<double>(*value));
        }
    }

    /// 观测一个可空的有符号整数标量（`int64_t` 与 `uint64_t`/`double` 两组重载都会
    /// 参与重载决议，故显式提供）。
    void scalar(std::string_view key, const std::optional<int64_t>& value) {
        if (value.has_value()) {
            scalar(key, static_cast<double>(*value));
        }
    }

    /// 观测一个必有整数值。
    void scalar(std::string_view key, uint64_t value) { scalar(key, static_cast<double>(value)); }

    void scalar(std::string_view key, double value) {
        const auto* spec = sysmonstore::find_scalar_metric(key);
        if (spec == nullptr) {
            return;  // 未注册的键：schema 是唯一真相源，写入方落库时会告警
        }
        const auto index = static_cast<size_t>(spec - sysmonstore::kScalarMetrics.data());
        scalars_[index].observe(value);
    }

    void dimension(std::string_view domain,
                   std::string_view dimension,
                   std::string_view metric,
                   double value) {
        // 线性查找当前窗口已有的 (域, 维度, 指标) 槽位；新出现的维度（热插拔的网卡、
        // 新挂载的卷）追加槽位。维度集合帧间可变，不能按下标假设稳定。
        for (auto& slot : dimensions_) {
            if (slot.domain == domain && slot.dimension == dimension && slot.metric == metric) {
                slot.aggregate.observe(value);
                return;
            }
        }
        HistoryRecorder::DimensionSlot slot;
        slot.domain = std::string(domain);
        slot.dimension = std::string(dimension);
        slot.metric = std::string(metric);
        slot.aggregate.observe(value);
        dimensions_.push_back(std::move(slot));
    }

private:
    std::array<sysmonstore::MetricAggregate, sysmonstore::kScalarMetrics.size()>& scalars_;
    std::vector<HistoryRecorder::DimensionSlot>& dimensions_;
};

}  // namespace

HistoryRecorder::HistoryRecorder(RecordWindowConfig config)
    : config_(config) {
    if (config_.interval_sec <= 0) {
        config_.interval_sec = 60;  // 防御：配置校验已保证 [1, 86400]
    }
}

int64_t HistoryRecorder::expected_samples_per_window() const noexcept {
    if (config_.measure_interval_ms <= 0) {
        return 1;
    }
    return std::max<int64_t>(1, config_.interval_sec * 1000 / config_.measure_interval_ms);
}

sysmonstore::HistoryBatch HistoryRecorder::observe(const platform::SysmonSnapshot& snapshot,
                                                   int64_t now_ts) {
    const int64_t bucket =
        sysmonstore::bucket_start(now_ts, config_.interval_sec, config_.phase_sec);

    sysmonstore::HistoryBatch batch;
    if (!bucket_ts_.has_value()) {
        bucket_ts_ = bucket;
    } else if (*bucket_ts_ != bucket) {
        // 窗口切换：先产出上一窗口的批次。
        // 缺口判定必须在 `take_batch` 重置累加状态**之前**取样本数——重置后读到的是 0。
        const int64_t previous = *bucket_ts_;
        const int64_t samples = samples_in_window_;
        batch = take_batch(previous);
        // 上一窗口样本不齐（停顿/被强杀/时钟跳变）⇒ 记缺口，绝不补成连续数据。
        mark_if_incomplete(batch, previous, samples);
        bucket_ts_ = bucket;
    }

    observe_values(snapshot);
    ++samples_in_window_;
    return batch;
}

sysmonstore::HistoryBatch HistoryRecorder::flush(int64_t now_ts) {
    if (!bucket_ts_.has_value() || samples_in_window_ <= 0) {
        // 从未观测到任何样本：无数据可落盘（不产出空窗口冒充「有数据」）。
        pending_marks_.clear();
        return {};
    }
    const int64_t previous = *bucket_ts_;
    const int64_t samples = samples_in_window_;
    auto batch = take_batch(previous);
    mark_if_incomplete(batch, previous, samples);
    // 待落盘标记（采集中断/重启/时钟跳变）也随这一批走：标记与数据同事务提交。
    pending_marks_.clear();
    (void)now_ts;
    bucket_ts_ = std::nullopt;
    return batch;
}

void HistoryRecorder::add_mark(sysmonstore::HistoryMark mark) {
    pending_marks_.push_back(std::move(mark));
}

void HistoryRecorder::mark_if_incomplete(sysmonstore::HistoryBatch& batch,
                                         int64_t bucket_ts,
                                         int64_t samples) const {
    const int64_t expected = expected_samples_per_window();
    // 允许 ±1 的调度抖动：定时器按「上一 deadline + 间隔」推进，落在窗口边界的样本
    // 归属可能相差一个 tick，严格相等会造成大量假缺口告警。
    if (samples + 1 >= expected) {
        return;
    }
    sysmonstore::HistoryMark gap;
    gap.ts = bucket_ts;
    gap.kind = std::string(sysmonstore::kMarkSampleGap);
    gap.detail = std::to_string(samples);
    batch.marks.push_back(std::move(gap));
}

void HistoryRecorder::observe_values(const platform::SysmonSnapshot& snapshot) {
    WindowAccumulator acc(scalars_, dimensions_);

    // ---- 宽表：固定标量 ----
    // 可能为 null 的（首次采样/计数器重置/本帧未采集）交给可空重载：不可用即不观测。
    acc.scalar("cpu.usage_ratio", snapshot.cpu.total.usage_ratio);
    if (snapshot.cpu.load_average.has_value()) {
        acc.scalar("cpu.load_average.one", snapshot.cpu.load_average->one);
        acc.scalar("cpu.load_average.five", snapshot.cpu.load_average->five);
        acc.scalar("cpu.load_average.fifteen", snapshot.cpu.load_average->fifteen);
    }
    acc.scalar("cpu.context_switches_per_sec", snapshot.cpu.context_switches_per_sec);
    acc.scalar("cpu.interrupts_per_sec", snapshot.cpu.interrupts_per_sec);

    if (snapshot.memory.has_value()) {
        const auto& memory = *snapshot.memory;
        acc.scalar("memory.total_bytes", memory.total_bytes);
        acc.scalar("memory.available_bytes", memory.available_bytes);
        acc.scalar("memory.used_bytes", memory.used_bytes);
        acc.scalar("memory.used_ratio", memory.used_ratio);
        acc.scalar("memory.cached_bytes", memory.cached_bytes);
        acc.scalar("memory.buffers_bytes", memory.buffers_bytes);
    }
    if (snapshot.swap.has_value()) {
        const auto& swap = *snapshot.swap;
        acc.scalar("swap.total_bytes", swap.total_bytes);
        acc.scalar("swap.used_bytes", swap.used_bytes);
        acc.scalar("swap.used_ratio", swap.used_ratio);
    }
    if (snapshot.shm_dir.has_value()) {
        const auto& shm = *snapshot.shm_dir;
        acc.scalar("shm_dir.total_bytes", shm.total_bytes);
        acc.scalar("shm_dir.page_files", shm.page_files);
        acc.scalar("shm_dir.oldest_file_age_sec", shm.oldest_file_age_sec);
    }

    // ---- 长表：按维度展开 ----
    // CPU 核：`saturated` 以 0/1 记录（读取方按 max>0 判定该窗口内是否饱和过）。
    for (const auto& core : snapshot.cpu.cores) {
        const std::string index = std::to_string(core.index);
        if (core.usage_ratio.has_value()) {
            acc.dimension(sysmonstore::kCpuCoreDomain, index, "usage_ratio", *core.usage_ratio);
        }
        acc.dimension(sysmonstore::kCpuCoreDomain, index, "saturated", core.saturated ? 1.0 : 0.0);
    }
    for (const auto& frequency :
         snapshot.cpu.core_frequencies.value_or(std::vector<platform::SysmonCoreFrequency>{})) {
        acc.dimension(sysmonstore::kCpuCoreDomain, std::to_string(frequency.index), "mhz",
                      static_cast<double>(frequency.mhz));
    }

    // 各域可能为 null（本帧未采集/能力不可用）：null 表示「没有事实」，
    // 不是「值为 0」，因此整个域跳过而不是用空值补齐。
    for (const auto& volume :
         snapshot.volumes.value_or(std::vector<platform::SysmonVolumeInfo>{})) {
        if (!volume.available) {
            continue;  // 卷存在但读不到：不观测（避免把「读不到」记成 0）
        }
        if (volume.total_bytes.has_value()) {
            acc.dimension(sysmonstore::kVolumeDomain, volume.mount, "total_bytes",
                          static_cast<double>(*volume.total_bytes));
        }
        if (volume.used_bytes.has_value()) {
            acc.dimension(sysmonstore::kVolumeDomain, volume.mount, "used_bytes",
                          static_cast<double>(*volume.used_bytes));
        }
        if (volume.used_ratio.has_value()) {
            acc.dimension(sysmonstore::kVolumeDomain, volume.mount, "used_ratio",
                          *volume.used_ratio);
        }
        acc.dimension(sysmonstore::kVolumeDomain, volume.mount, "watched",
                      volume.watched ? 1.0 : 0.0);
    }

    for (const auto& disk : snapshot.disk_io.value_or(std::vector<platform::SysmonDiskIoInfo>{})) {
        const auto& device = disk.device;
        // 派生字段为 null（首次采样/计数器重置）时不观测，避免把「不可用」记成 0。
        if (disk.read_bytes_per_sec.has_value()) {
            acc.dimension(sysmonstore::kDiskIoDomain, device, "read_bytes_per_sec",
                          *disk.read_bytes_per_sec);
        }
        if (disk.write_bytes_per_sec.has_value()) {
            acc.dimension(sysmonstore::kDiskIoDomain, device, "write_bytes_per_sec",
                          *disk.write_bytes_per_sec);
        }
        if (disk.read_iops.has_value()) {
            acc.dimension(sysmonstore::kDiskIoDomain, device, "read_iops", *disk.read_iops);
        }
        if (disk.write_iops.has_value()) {
            acc.dimension(sysmonstore::kDiskIoDomain, device, "write_iops", *disk.write_iops);
        }
        if (disk.latency_ms.has_value()) {
            acc.dimension(sysmonstore::kDiskIoDomain, device, "latency_ms", *disk.latency_ms);
        }
        if (disk.queue_depth.has_value()) {
            acc.dimension(sysmonstore::kDiskIoDomain, device, "queue_depth", *disk.queue_depth);
        }
        if (disk.utilization.has_value()) {
            acc.dimension(sysmonstore::kDiskIoDomain, device, "utilization", *disk.utilization);
        }
    }

    for (const auto& network :
         snapshot.network.value_or(std::vector<platform::SysmonNetworkInfo>{})) {
        const std::string& name = network.interface_name;
        // 累计计数器：min/max 在窗口内没有信息量，但必须存 last（读取方可据此算跨窗口速率，
        // 也可回答「某一时刻的累计收发量」）；记录 min/max 也不额外花空间。
        acc.dimension(sysmonstore::kNetworkDomain, name, "rx_bytes_total",
                      static_cast<double>(network.rx_bytes_total));
        acc.dimension(sysmonstore::kNetworkDomain, name, "tx_bytes_total",
                      static_cast<double>(network.tx_bytes_total));
        acc.dimension(sysmonstore::kNetworkDomain, name, "rx_packets_total",
                      static_cast<double>(network.rx_packets_total));
        acc.dimension(sysmonstore::kNetworkDomain, name, "tx_packets_total",
                      static_cast<double>(network.tx_packets_total));
        acc.dimension(sysmonstore::kNetworkDomain, name, "rx_errors_total",
                      static_cast<double>(network.rx_errors_total));
        acc.dimension(sysmonstore::kNetworkDomain, name, "tx_errors_total",
                      static_cast<double>(network.tx_errors_total));
        acc.dimension(sysmonstore::kNetworkDomain, name, "rx_drops_total",
                      static_cast<double>(network.rx_drops_total));
        acc.dimension(sysmonstore::kNetworkDomain, name, "tx_drops_total",
                      static_cast<double>(network.tx_drops_total));
        if (network.rx_bytes_per_sec.has_value()) {
            acc.dimension(sysmonstore::kNetworkDomain, name, "rx_bytes_per_sec",
                          *network.rx_bytes_per_sec);
        }
        if (network.tx_bytes_per_sec.has_value()) {
            acc.dimension(sysmonstore::kNetworkDomain, name, "tx_bytes_per_sec",
                          *network.tx_bytes_per_sec);
        }
        if (network.rx_errors_per_sec.has_value()) {
            acc.dimension(sysmonstore::kNetworkDomain, name, "rx_errors_per_sec",
                          *network.rx_errors_per_sec);
        }
        if (network.tx_errors_per_sec.has_value()) {
            acc.dimension(sysmonstore::kNetworkDomain, name, "tx_errors_per_sec",
                          *network.tx_errors_per_sec);
        }
        if (network.rx_drops_per_sec.has_value()) {
            acc.dimension(sysmonstore::kNetworkDomain, name, "rx_drops_per_sec",
                          *network.rx_drops_per_sec);
        }
        if (network.tx_drops_per_sec.has_value()) {
            acc.dimension(sysmonstore::kNetworkDomain, name, "tx_drops_per_sec",
                          *network.tx_drops_per_sec);
        }
        acc.dimension(sysmonstore::kNetworkDomain, name, "is_virtual",
                      network.is_virtual ? 1.0 : 0.0);
    }

    for (const auto& sensor :
         snapshot.temperature.value_or(std::vector<platform::SysmonTemperatureInfo>{})) {
        acc.dimension(sysmonstore::kTemperatureDomain, sensor.sensor, "celsius", sensor.celsius);
    }
}

sysmonstore::HistoryBatch HistoryRecorder::take_batch(int64_t bucket_ts) {
    sysmonstore::HistoryBatch batch;
    sysmonstore::ScalarRecord record;
    record.level = sysmonstore::kLevelRaw;
    record.bucket_ts = bucket_ts;
    record.interval_sec = config_.interval_sec;
    record.metrics = scalars_;
    batch.scalars.push_back(std::move(record));

    for (auto& slot : dimensions_) {
        sysmonstore::DimensionRecord dimension;
        dimension.level = sysmonstore::kLevelRaw;
        dimension.bucket_ts = bucket_ts;
        dimension.domain = std::move(slot.domain);
        dimension.dimension = std::move(slot.dimension);
        dimension.metric = std::move(slot.metric);
        dimension.aggregate = slot.aggregate;
        batch.dimensions.push_back(std::move(dimension));
    }

    batch.marks = std::move(pending_marks_);
    pending_marks_.clear();

    // 重置累加状态：新窗口从零开始，绝不沿用上一窗口的数据。
    scalars_ = {};
    dimensions_.clear();
    samples_in_window_ = 0;
    return batch;
}

}  // namespace dztrader::sysmon
