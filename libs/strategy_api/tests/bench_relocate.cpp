/**
 * @file bench_relocate.cpp
 * @brief BarStrategy 18:00 回卷成本基准（issue #164；组件文档 §10 基准项 `bench_relocate`）
 *
 * 量的是 `InstrumentContext::relocate()`：把窗口 `[end-size, end)` 搬回 `[0, size)` 的
 * **纯布局变换**成本（7 条价格数组 + 全部用户序列，不改值/`size`/`bar_index`）。组件文档
 * §4.4 把它放在每天 18:00 定时器里，热路径因此永远不承担整窗搬移；§10 要求记录它的成本
 * 与事件循环停顿上限（< 0.2ms）。
 *
 * ## 两个场景
 *
 * - **单品种**（§10 基准参数）：1 个实例、9 条数组（7 价格 + 2 用户序列，双均线）、
 *   `max_lookback=500`、`buffer_bars=3380`；
 * - **50 品种**（§4.4 的预算口径）：50 个实例、每个 11 条数组（7 价格 + 4 用户序列）、
 *   `max_lookback=500`，用于把 §4.4 的「50 品种 × 11 数组 × 1440 根 ≈ 6.3MB → 60–130µs/
 *   天、停顿 < 0.2ms」换算成同量纲：把 500 根折算到预算的 1440 根（搬移量 ∝ 根数）后与
 *   60–130µs 及 0.2ms 上限对照，结论按 report-only 打印 `within_budget` / `above_budget`。
 *
 * ## 测量方法
 *
 * 每个样本：先把窗口推进回 `end == buffer_bars`（回卷前的真实状态，**不计时**），再单独给
 * `relocate()` 计时。这样测的是「稳态回卷」而不是「首次冷 cache 回卷」，也不把推进成本混进
 * 回卷成本。样本之间校验搬移前后窗口逐位等价（值、`size`、`bar_index`）。A 的定时器路径
 * 另有 map 遍历与一行诊断日志，不属布局搬移成本，不在本基准内。
 *
 * 构造走 `context_internal::ContextAccess`（与纯单元测试同一条构造缝），不经引擎/通道/配置。
 *
 * ## 运行
 *
 * ```bash
 * ./scripts/build.sh
 * ./scripts/test.sh --label perf
 * DZTRADER_HOME=$PWD/.dztrader_dev \
 *   ./build/linux/x86_64/Release/tests/bench_relocate
 * ./build/linux/x86_64/Release/tests/bench_relocate --quick
 * ```
 *
 * 只报告数值、不设阈值断言（report-only）。
 */
#include <dztrader/bar_strategy/context.h>
#include <dztrader/bar_strategy/context_internal.h>

#include "bench_support.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

namespace {

using dztrader::InstrumentContext;
using dztrader::InstrumentInit;
using dztrader::InstrumentSpec;
using dztrader::Series;
using dztrader::context_internal::ContextAccess;
using dztrader::strategy_api::bench_support::Clock;
using dztrader::strategy_api::bench_support::expect;
using dztrader::strategy_api::bench_support::make_clock;
using dztrader::strategy_api::bench_support::quick_mode;
using dztrader::strategy_api::bench_support::Statistics;
using dztrader::strategy_api::bench_support::subtract_overhead;
using dztrader::strategy_api::bench_support::summarize;
using dztrader::strategy_api::bench_support::tsc_now;

/// 基准口径：可读深度 500（组件文档 §10）
constexpr std::int32_t kMaxLookback = 500;
/// 1 分钟周期的自动回卷余量（`reserve_bars = max(2×86400/60, R_min) = 2880`）
constexpr std::int32_t kReserveBars = 2880;
/// 物理容量 = max_lookback + reserve_bars
constexpr std::int32_t kBufferBars = kMaxLookback + kReserveBars;
/// 1 分钟周期
constexpr std::int32_t kPeriodSec = 60;
/// 单品种场景的用户序列条数：双均线 2 条 → 7 + 2 = 9 条数组
constexpr std::int32_t kSingleUserSeries = 2;
/// §4.4 预算口径的用户序列条数：示例 4 条 → 7 + 4 = 11 条数组
constexpr std::int32_t kBudgetUserSeries = 4;
/// §4.4 预算口径的品种数
constexpr std::int32_t kBudgetInstruments = 50;
/// §4.4 预算口径的 max_lookback（1 分钟周期自动值 86400/60 = 1440）
constexpr std::int32_t kBudgetLookback = 1440;
/// §4.4 预算口径的物理容量 = 1440 + 2880
constexpr std::int32_t kBudgetBufferBars = kBudgetLookback + kReserveBars;
/// 单品种默认样本数（每个样本 = 2880 次推进 + 1 次回卷）
constexpr std::int64_t kDefaultSamples = 2000;
/// 单品种 `--quick` 样本数
constexpr std::int64_t kQuickSamples = 200;
/// 50 品种默认样本数（每个样本 = 50 × 2880 次推进 + 50 次回卷，约 2ms 推进）
constexpr std::int64_t kDefaultProcessSamples = 100;
/// 50 品种 `--quick` 样本数
constexpr std::int64_t kQuickProcessSamples = 10;

/// @brief 一个品种实例：ctx + 用户序列（数组条数 = 7 + user_series.size()）
struct Instance {
    std::unique_ptr<InstrumentContext> ctx;  ///< 脱离引擎的 ctx
    std::vector<Series<double>> series;      ///< 用户序列（双均线用 2 条，预算口径 4 条）
    std::int64_t next_bar = 0;               ///< 下一次推进的绝对 bar 序号
    std::int32_t max_lookback = 0;           ///< 可读深度
    std::int32_t buffer_bars = 0;            ///< 物理容量

    /// @brief 数组总条数（7 条价格 + 用户序列）
    [[nodiscard]] std::int32_t array_count() const noexcept {
        return 7 + static_cast<std::int32_t>(series.size());
    }
};

/// @brief 造一个实例
/// @param max_lookback 可读深度
/// @param buffer_bars 物理容量
/// @param user_series_count 用户序列条数
/// @return 实例
std::unique_ptr<Instance> make_instance(std::int32_t max_lookback,
                                        std::int32_t buffer_bars,
                                        std::int32_t user_series_count) {
    InstrumentSpec spec;
    spec.exchange_id = "SHFE";
    spec.volume_multiple = 10.0;
    spec.price_tick = 0.01;
    auto instance = std::make_unique<Instance>();
    instance->max_lookback = max_lookback;
    instance->buffer_bars = buffer_bars;
    instance->ctx = ContextAccess::make(InstrumentInit{
        .instrument_id = "rb2601",
        .max_lookback = max_lookback,
        .buffer_bars = buffer_bars,
        .spec = spec,
        .dz = nullptr,        // 纯布局基准：无会话、无账户
        .accounts = nullptr,  // 0 个挂靠账户
        .scaling = {},
    });
    instance->series.reserve(static_cast<std::size_t>(user_series_count));
    for (std::int32_t i = 0; i < user_series_count; ++i) {
        instance->series.push_back(ContextAccess::make_series<double>(*instance->ctx, 0.0));
    }
    return instance;
}

/// @brief 构造第 index 根 bar（交易日按 1440 根/日递增，价格随索引线性变化）
/// @param index 绝对 bar 序号
/// @return 待推进的 bar
dztrader::bar::Bar make_bar(std::int64_t index) {
    constexpr std::int32_t kBarsPerDay = 86400 / kPeriodSec;
    dztrader::bar::Bar bar{};
    bar.trading_day = 20000 + static_cast<DzDate>(index / kBarsPerDay);
    bar.begin_sec = static_cast<std::int32_t>(index % kBarsPerDay) * kPeriodSec;
    bar.last_tick_sec = bar.begin_sec + kPeriodSec - 1;
    bar.period_sec = kPeriodSec;
    const double base = 1000.0 + static_cast<double>(index % 200);
    bar.open = base;
    bar.high = base + 2.0;
    bar.low = base - 1.0;
    bar.close = base + 0.5;
    bar.volume = 10;
    bar.turnover = bar.close * 10.0;
    bar.open_interest = 1000;
    bar.cum_volume = 10LL * (index + 1);
    bar.cum_turnover = bar.turnover * static_cast<double>(index + 1);
    bar.is_final = true;
    return bar;
}

/// @brief 推进一根 bar 并写用户序列当前值（第 k 条写 k×index）
/// @param instance 目标实例
void advance_one(Instance& instance) {
    ContextAccess::advance(*instance.ctx, make_bar(instance.next_bar));
    const auto index = static_cast<double>(instance.next_bar);
    for (std::size_t k = 0; k < instance.series.size(); ++k) {
        instance.series[k][0] = static_cast<double>(k + 1) * index;
    }
    ++instance.next_bar;
}

/// @brief 把一个实例推进到「待回卷」状态（end == buffer_bars）
/// @param instance 目标实例
void fill_to_relocate_ready(Instance& instance) {
    while (instance.next_bar < instance.buffer_bars) {
        advance_one(instance);
    }
}

/// @brief 把一个实例从「刚回卷完」（`end == size == max_lookback`）推进回「待回卷」
///        （`end == buffer_bars`），不计时
/// @param instance 目标实例
void refill_to_relocate_ready(Instance& instance) {
    for (std::int32_t j = 0; j < instance.buffer_bars - instance.max_lookback; ++j) {
        advance_one(instance);
    }
}

/**
 * @brief 窗口逐位校验和（价格 + 用户序列；搬移前后必须完全相同）
 * @param instance 目标实例
 * @return 校验和（值域与顺序敏感）
 */
double checksum(const Instance& instance) {
    double sum = 0.0;
    const InstrumentContext& ctx = *instance.ctx;
    for (std::int32_t n = 0; n < instance.max_lookback; ++n) {
        sum += ctx.close[n] * 0.5;
        sum += ctx.open[n] * 0.25;
        for (const Series<double>& series : instance.series) {
            sum += series[n];
        }
    }
    return sum;
}

/// @brief 一个场景的计时结果
struct ScenarioResult {
    const char* name = "";          ///< 报告名
    Statistics raw;                 ///< 含计时开销的分布（ns/次）
    Statistics net;                 ///< 扣减计时开销后的分布（ns/次）
    double moved_bytes = 0.0;       ///< 每次回卷搬移的字节数
    std::int32_t instruments = 0;   ///< 实例数
    std::int32_t arrays = 0;        ///< 每实例数组条数
    std::int32_t max_lookback = 0;  ///< 可读深度
};

/**
 * @brief 跑一个场景：装好 instruments 个实例、填满窗口，然后每样本「补满窗口（不计时）→
 *        对全部实例 relocate（计时）」，并逐位校验搬移前后等价
 * @param name 报告名
 * @param instruments 实例数
 * @param max_lookback 可读深度
 * @param buffer_bars 物理容量
 * @param user_series 每实例用户序列条数
 * @param samples 样本数
 * @param clock 计时环境
 * @param result 输出：计时结果
 * @return true = 自检通过
 */
bool run_scenario(const char* name,
                  std::int32_t instruments,
                  std::int32_t max_lookback,
                  std::int32_t buffer_bars,
                  std::int32_t user_series,
                  std::int64_t samples,
                  const Clock& clock,
                  ScenarioResult& result) {
    std::vector<std::unique_ptr<Instance>> instances;
    instances.reserve(static_cast<std::size_t>(instruments));
    for (std::int32_t i = 0; i < instruments; ++i) {
        instances.push_back(make_instance(max_lookback, buffer_bars, user_series));
        fill_to_relocate_ready(*instances.back());
    }
    const std::int32_t arrays = 7 + user_series;
    if (!expect(static_cast<std::int32_t>(instances.size()) == instruments, name,
                "check=instrument_count") ||
        !expect(instances.front()->array_count() == arrays, name, "check=array_count") ||
        !expect(instances.front()->ctx->close.size() == max_lookback, name, "check=window_size") ||
        !expect(instances.front()->ctx->bar_index() == buffer_bars - 1, name, "check=bar_index")) {
        std::fprintf(stderr,
                     "actual | instruments=%zu arrays=%d size=%d bar_index=%lld expected | "
                     "instruments=%d arrays=%d size=%d bar_index=%d\n",
                     instances.size(), instances.front()->array_count(),
                     instances.front()->ctx->close.size(),
                     static_cast<long long>(instances.front()->ctx->bar_index()), instruments,
                     arrays, max_lookback, buffer_bars - 1);
        return false;
    }

    // 回卷等价性：搬移前后值/size/bar_index 完全一致
    {
        const std::unique_ptr<Instance>& probe = instances.front();
        const double before = checksum(*probe);
        const std::int32_t size_before = probe->ctx->close.size();
        const std::int64_t index_before = probe->ctx->bar_index();
        ContextAccess::relocate(*probe->ctx);
        if (!expect(before == checksum(*probe), name, "check=relocate_values_equal") ||
            !expect(probe->ctx->close.size() == size_before, name, "check=relocate_size_equal") ||
            !expect(probe->ctx->bar_index() == index_before, name,
                    "check=relocate_bar_index_equal")) {
            std::fprintf(stderr, "actual | checksum_before=%.6f after=%.6f size_before=%d\n",
                         before, checksum(*probe), size_before);
            return false;
        }
        refill_to_relocate_ready(*probe);
    }

    std::vector<std::uint64_t> cycles;
    cycles.reserve(static_cast<std::size_t>(samples));
    for (std::int64_t rep = 0; rep < samples; ++rep) {
        for (std::unique_ptr<Instance>& instance : instances) {
            refill_to_relocate_ready(*instance);
        }
        double reference = 0.0;
        for (const std::unique_ptr<Instance>& instance : instances) {
            reference += checksum(*instance);
        }
        const std::uint64_t begin = tsc_now();
        for (std::unique_ptr<Instance>& instance : instances) {
            ContextAccess::relocate(*instance->ctx);
        }
        const std::uint64_t end = tsc_now();
        cycles.push_back(end - begin);
        double after = 0.0;
        for (const std::unique_ptr<Instance>& instance : instances) {
            after += checksum(*instance);
        }
        if (after != reference) {
            std::fprintf(stderr, "self-check failed | bench=%s rep=%lld check=equivalence\n", name,
                         static_cast<long long>(rep));
            return false;
        }
    }

    result.name = name;
    result.raw = summarize(std::move(cycles), clock.ns_per_cycle);
    result.net = subtract_overhead(result.raw, clock.overhead);
    result.moved_bytes = static_cast<double>(instruments) * static_cast<double>(arrays) *
                         static_cast<double>(max_lookback) * sizeof(double);
    result.instruments = instruments;
    result.arrays = arrays;
    result.max_lookback = max_lookback;
    return true;
}

/**
 * @brief 跑 benchmark
 * @param quick true = 小样本自测档
 * @return 进程退出码
 */
int run(bool quick) {
    constexpr const char* kBench = "bench_relocate";
    const std::int64_t single_samples = quick ? kQuickSamples : kDefaultSamples;
    const std::int64_t process_samples = quick ? kQuickProcessSamples : kDefaultProcessSamples;

    const Clock clock = make_clock(kBench);
    dztrader::strategy_api::bench_support::print_bench_header(
        "dztrader bar strategy benchmark: 18:00 relocate cost (issue #164)",
        "relocate | memmove of window [end-size, end) back to [0, size), pure layout", clock);
    std::printf(
        "scenario | period_sec=%d | single: instruments=1 max_lookback=%d arrays=%d samples=%lld"
        " | process@500: instruments=%d max_lookback=%d arrays=%d samples=%lld"
        " | process@budget: instruments=%d max_lookback=%d arrays=%d samples=%lld\n",
        kPeriodSec, kMaxLookback, 7 + kSingleUserSeries, static_cast<long long>(single_samples),
        kBudgetInstruments, kMaxLookback, 7 + kSingleUserSeries,
        static_cast<long long>(process_samples), kBudgetInstruments, kBudgetLookback,
        7 + kBudgetUserSeries, static_cast<long long>(process_samples));
    if (quick) {
        std::printf("quick mode | reduced sample sizes (--quick)\n");
    }
    std::printf("\n");

    std::vector<ScenarioResult> results(3);
    if (!run_scenario("single instrument", 1, kMaxLookback, kBufferBars, kSingleUserSeries,
                      single_samples, clock, results[0]) ||
        !run_scenario("50 instruments@500", kBudgetInstruments, kMaxLookback, kBufferBars,
                      kSingleUserSeries, process_samples, clock, results[1]) ||
        !run_scenario("50 instruments@1440", kBudgetInstruments, kBudgetLookback, kBudgetBufferBars,
                      kBudgetUserSeries, process_samples, clock, results[2])) {
        return 1;
    }

    // ---- 报告 ----
    std::printf(
        "measure                samples    mean_raw    raw_p50    raw_p99    net_p50    "
        "net_p99    (ns)\n");
    for (const ScenarioResult& result : results) {
        std::printf("%-22s %7lld %11.2f %10.2f %10.2f %10.2f %10.2f\n", result.name,
                    static_cast<long long>(result.raw.count), result.raw.mean, result.raw.p50,
                    result.raw.p99, result.net.p50, result.net.p99);
    }

    const ScenarioResult& process_budget = results[2];
    const double process_p50_us = process_budget.net.p50 / 1000.0;
    const double process_p99_us = process_budget.net.p99 / 1000.0;
    const double throughput_gbs =
        process_budget.moved_bytes / (process_budget.net.p50 > 0.0 ? process_budget.net.p50 : 1.0);
    std::printf(
        "\nderived | budget scenario: moved=%.2fMB instruments=%d arrays=%d max_lookback=%d "
        "per_instrument_p50=%.3fus throughput=%.1fGB/s\n",
        process_budget.moved_bytes / 1.0e6, process_budget.instruments, process_budget.arrays,
        process_budget.max_lookback,
        process_p50_us / static_cast<double>(process_budget.instruments), throughput_gbs);
    std::printf(
        "derived | single instrument@500 per-array=%.2fns; single@1440 equivalent p50=%.3fus "
        "(cost scales with moved elements)\n",
        results[0].net.p50 / static_cast<double>(results[0].arrays),
        results[0].net.p50 / 1000.0 * (static_cast<double>(kBudgetLookback) / kMaxLookback));

    // 组件文档 §4.4/§10：预算 60–130µs/天（50 品种 × 11 数组 × 1440 根 ≈ 6.3MB）、
    // 停顿上限 < 0.2ms。预算场景直接测同口径；report-only。
    const double budget_low_us = 60.0;
    const double budget_high_us = 130.0;
    const double pause_limit_us = 200.0;
    std::printf("\ntarget | pause p99=%.3fus limit=%.0fus %s (component doc §10; report-only)\n",
                process_p99_us, pause_limit_us,
                process_p99_us <= pause_limit_us ? "met" : "missed");
    std::printf(
        "compare | budget_range=%.0f..%.0fus/day (50 instruments, max_lookback=1440, 11 arrays, "
        "≈6.3MB) measured_p99=%.3fus verdict=%s\n",
        budget_low_us, budget_high_us, process_p99_us,
        process_p99_us <= budget_high_us ? "within_budget" : "above_budget");
    std::printf(
        "note | §4.4 estimate implies ~48–105GB/s effective memmove for a 6.3MB working set; "
        "measured throughput above → deviation recorded in the baseline report, no design change "
        "in this issue\n");
    std::printf(
        "note | manager timer path adds map iteration + one diagnostic line per day; not part of "
        "the layout cost measured here\n");
    std::printf(
        "\nrecord: docs/agents/build-appendix.md, section \"performance benchmarks\" "
        "(append machine + date + numbers)\n");
    return 0;
}

}  // namespace

int main(int argc, char** argv) { return run(quick_mode(argc, argv)); }
