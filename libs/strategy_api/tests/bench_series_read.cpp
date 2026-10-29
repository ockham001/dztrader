/**
 * @file bench_series_read.cpp
 * @brief BarStrategy Series 读路径基准（issue #164；组件文档 §10 基准项 `bench_series_read`）
 *
 * 量的是策略作者在最热路径上读序列的两种形态：
 *
 * - **`[n]` 回溯读**：`SeriesView::operator[](std::ptrdiff_t)`（`[0]` 当前、`[n]` 往回 n）；
 * - **`span()` 连续扫描**：把整窗喂给指标循环/`std::accumulate` 的零检查快路径。
 *
 * 口径：单品种、`period_sec=60`、`max_lookback=500`（= 组件文档 §10 基准参数）、窗口填满
 * 500 根 + 两条用户序列（双均线）。构造走 `context_internal::ContextAccess`（与纯单元测试
 * 同一条构造缝，不经引擎/通道/配置）。组件文档 §10 没有给序列读的独立预算，本基准只报告
 * 数值作为回归基线。
 *
 * ## 为什么用「一组操作读一对时钟」
 *
 * 单次 `[n]` 约几 ns，低于成对读 RDTSC 的开销（约 8.8ns），逐次计时量不出信号。故每次读一对
 * 时钟覆盖一整组操作（`[n]` 组 = 1000 次读，`span()` 组 = 10 次整窗扫描），组内均值再除以
 * 操作数；`net` 列扣减「空区间开销 / 操作数」。注意：这样得到的 p99 是**组均值的 p99**，
 * 会低估单次操作的尾部，报告里按「聚合成本」读。
 *
 * ## 自检
 *
 * 计时前逐位校验窗口内容（`close[n]` / 用户序列 `[n]` 与写入顺序一致）、`size()` /
 * `max_lookback()`、`span()` 长度与首尾值；任一不符退出 1。
 *
 * ## 运行
 *
 * ```bash
 * ./scripts/build.sh
 * ./scripts/test.sh --label perf
 * DZTRADER_HOME=$PWD/.dztrader_dev \
 *   ./build/linux/x86_64/Release/tests/bench_series_read
 * ./build/linux/x86_64/Release/tests/bench_series_read --quick
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
/// 物理容量 = max_lookback + 自动回卷余量（1 分钟周期：2880），与基准配置推导一致
constexpr std::int32_t kBufferBars = kMaxLookback + 2880;
/// 1 分钟周期（历史 bar 间隔，仅用于构造窗口）
constexpr std::int32_t kPeriodSec = 60;
/// `[n]` 每组的读次数（单次几 ns，需足够多次才高于计时噪声）
constexpr std::int64_t kReadsPerGroup = 1000;
/// `span()` 每组的整窗扫描次数
constexpr std::int64_t kScansPerGroup = 10;
/// 默认每组采样数（组均值分布）
constexpr std::int64_t kDefaultGroups = 100'000;
/// `--quick` 组数
constexpr std::int64_t kQuickGroups = 5'000;

/// @brief 造一份填满 500 根 bar 的 ctx（价格与用户序列按写入顺序可逐位校验）
/// @return 脱离引擎的 ctx
std::unique_ptr<InstrumentContext> make_ctx() {
    InstrumentSpec spec;
    spec.exchange_id = "SHFE";
    spec.volume_multiple = 10.0;
    spec.price_tick = 0.01;
    return ContextAccess::make(InstrumentInit{
        .instrument_id = "rb2601",
        .max_lookback = kMaxLookback,
        .buffer_bars = kBufferBars,
        .spec = spec,
        .dz = nullptr,        // 纯读基准：无会话、无账户
        .accounts = nullptr,  // 0 个挂靠账户
        .scaling = {},
    });
}

/**
 * @brief 填满窗口：第 i 根 bar 的收盘价 = 1000 + i + 0.5，用户序列 double 写 i / 2i
 * @param ctx 目标实例
 * @param fast 5 根均线用户序列（写 i）
 * @param slow 20 根均线用户序列（写 2i）
 */
void fill_window(InstrumentContext& ctx, Series<double>& fast, Series<double>& slow) {
    for (std::int32_t i = 0; i < kMaxLookback; ++i) {
        dztrader::bar::Bar bar{};
        bar.trading_day = 20000;
        bar.begin_sec = i * kPeriodSec;
        bar.last_tick_sec = bar.begin_sec + kPeriodSec - 1;
        bar.period_sec = kPeriodSec;
        bar.open = 1000.0 + static_cast<double>(i);
        bar.high = bar.open + 2.0;
        bar.low = bar.open - 1.0;
        bar.close = bar.open + 0.5;
        bar.volume = 10;
        bar.turnover = bar.close * 10.0;
        bar.open_interest = 1000;
        bar.cum_volume = 10LL * (i + 1);
        bar.cum_turnover = bar.turnover * static_cast<double>(i + 1);
        bar.is_final = true;
        ContextAccess::advance(ctx, bar);
        fast[0] = static_cast<double>(i);
        slow[0] = 2.0 * static_cast<double>(i);
    }
}

/**
 * @brief 逐位校验窗口内容与几何（计时前的路径身份自检）
 * @param ctx 已填满的实例
 * @param fast 用户序列
 * @param slow 用户序列
 * @return true = 全部一致
 */
bool verify_window(InstrumentContext& ctx, const Series<double>& fast, const Series<double>& slow) {
    bool ok = expect(ctx.close.size() == kMaxLookback, "bench_series_read",
                     "check=close_size contract=500");
    ok = expect(ctx.close.max_lookback() == kMaxLookback, "bench_series_read",
                "check=max_lookback contract=500") &&
         ok;
    for (std::int32_t n = 0; n < kMaxLookback; ++n) {
        const double expected_close = 1000.0 + static_cast<double>(kMaxLookback - 1 - n) + 0.5;
        const double expected_fast = static_cast<double>(kMaxLookback - 1 - n);
        const double expected_slow = 2.0 * static_cast<double>(kMaxLookback - 1 - n);
        if (ctx.close[n] != expected_close || fast[n] != expected_fast ||
            slow[n] != expected_slow) {
            std::fprintf(stderr,
                         "self-check failed | bench=bench_series_read check=window_values n=%d "
                         "close=%.2f expected=%.2f fast=%.2f expected=%.2f slow=%.2f "
                         "expected=%.2f\n",
                         n, ctx.close[n], expected_close, fast[n], expected_fast, slow[n],
                         expected_slow);
            return false;
        }
    }
    const std::span<const double> prices = ctx.close.span();
    ok = expect(prices.size() == static_cast<std::size_t>(kMaxLookback), "bench_series_read",
                "check=span_length contract=500") &&
         ok;
    if (prices.size() == static_cast<std::size_t>(kMaxLookback)) {
        ok = expect(prices.front() == 1000.5, "bench_series_read",
                    "check=span_front contract=1000.5") &&
             ok;
        const double expected_back = 1000.0 + static_cast<double>(kMaxLookback - 1) + 0.5;
        ok = expect(prices.back() == expected_back, "bench_series_read", "check=span_back") && ok;
    }
    return ok;
}

/// @brief 单操作口径的统计量（组均值 / 组内操作数；p99 是组均值的 p99，见文件头）
struct OperationStatistics {
    std::int64_t groups = 0;  ///< 组数
    double mean_raw = 0.0;    ///< 含计时开销的单操作均值
    double p50_raw = 0.0;     ///< 含计时开销的组均值 p50
    double p99_raw = 0.0;     ///< 含计时开销的组均值 p99
    double p50_net = 0.0;     ///< 扣减空区间开销后的组均值 p50
    double p99_net = 0.0;     ///< 扣减空区间开销后的组均值 p99
};

/**
 * @brief 组计时 → 单操作 ns 统计（组统计量除以组内操作数并扣减计时开销）
 * @param group_raw 组级分布（含每组的成对读时钟开销）
 * @param overhead 空区间成对读时钟开销分布
 * @param ops_per_group 组内操作数
 * @return 单操作口径的统计量
 */
OperationStatistics per_operation(const Statistics& group_raw,
                                  const Statistics& overhead,
                                  std::int64_t ops_per_group) {
    const Statistics net = subtract_overhead(group_raw, overhead);
    const auto scale = static_cast<double>(ops_per_group);
    OperationStatistics out;
    out.groups = group_raw.count;
    out.mean_raw = group_raw.mean / scale;
    out.p50_raw = group_raw.p50 / scale;
    out.p99_raw = group_raw.p99 / scale;
    out.p50_net = net.p50 / scale;
    out.p99_net = net.p99 / scale;
    return out;
}

/**
 * @brief 跑 benchmark
 * @param quick true = 小样本自测档
 * @return 进程退出码
 */
int run(bool quick) {
    constexpr const char* kBench = "bench_series_read";
    const std::int64_t groups = quick ? kQuickGroups : kDefaultGroups;

    const Clock clock = make_clock(kBench);
    dztrader::strategy_api::bench_support::print_bench_header(
        "dztrader bar strategy benchmark: series read path (issue #164)",
        "series read | [n] backtrack read and span() contiguous scan, window full at max_lookback",
        clock);
    std::printf(
        "scenario | instruments=1 period_sec=%d max_lookback=%d buffer_bars=%d user_series=2 "
        "[n]_reads_per_group=%lld span_scans_per_group=%lld groups=%lld\n",
        kPeriodSec, kMaxLookback, kBufferBars, static_cast<long long>(kReadsPerGroup),
        static_cast<long long>(kScansPerGroup), static_cast<long long>(groups));
    if (quick) {
        std::printf("quick mode | reduced sample sizes (--quick)\n");
    }
    std::printf("\n");

    std::unique_ptr<InstrumentContext> ctx = make_ctx();
    Series<double> fast = ContextAccess::make_series<double>(*ctx, 0.0);
    Series<double> slow = ContextAccess::make_series<double>(*ctx, 0.0);
    fill_window(*ctx, fast, slow);
    if (!verify_window(*ctx, fast, slow)) {
        return 1;
    }

    // ---- [n] 回溯读：组内 1000 次读，偏移按确定性公式轮转 ----
    std::vector<std::uint64_t> read_group_cycles;
    read_group_cycles.reserve(static_cast<std::size_t>(groups));
    double read_sink = 0.0;
    for (std::int64_t group = 0; group < groups; ++group) {
        const std::uint64_t begin = tsc_now();
        for (std::int64_t k = 0; k < kReadsPerGroup; ++k) {
            const auto offset = static_cast<std::ptrdiff_t>((k * 37 + 11) % kMaxLookback);
            read_sink += ctx->close[offset];
        }
        read_group_cycles.push_back(tsc_now() - begin);
    }

    // ---- span() 连续扫描：组内 10 次整窗遍历 ----
    std::vector<std::uint64_t> scan_group_cycles;
    scan_group_cycles.reserve(static_cast<std::size_t>(groups));
    double scan_sink = 0.0;
    for (std::int64_t group = 0; group < groups; ++group) {
        const std::uint64_t begin = tsc_now();
        for (std::int64_t k = 0; k < kScansPerGroup; ++k) {
            for (const double price : ctx->close.span()) {
                scan_sink += price;
            }
        }
        scan_group_cycles.push_back(tsc_now() - begin);
    }

    // 自检：两组 sink 必须非空（防死代码消除）且与期望值一致
    // 窗口收盘价 1000.5..1499.5，均值 1250；偏移模式 1000 次恰好每根取两次 → 组期望 1000×1250
    const double expected_read_sum =
        static_cast<double>(groups) * static_cast<double>(kReadsPerGroup) * 1250.0;
    const double expected_scan_sum =
        static_cast<double>(groups) * static_cast<double>(kScansPerGroup) * 625'000.0;
    const auto close_enough = [](double a, double b) {
        return a == b || (a > b ? a - b : b - a) <= 1e-6 * (b > 0.0 ? b : 1.0);
    };
    if (!expect(close_enough(read_sink, expected_read_sum), "bench_series_read",
                "check=read_sink") ||
        !expect(close_enough(scan_sink, expected_scan_sum), "bench_series_read",
                "check=scan_sink")) {
        std::fprintf(stderr, "actual | read_sink=%.3f expected=%.3f scan_sink=%.3f expected=%.3f\n",
                     read_sink, expected_read_sum, scan_sink, expected_scan_sum);
        return 1;
    }

    // ---- 报告 ----
    const Statistics read_raw = summarize(std::move(read_group_cycles), clock.ns_per_cycle);
    const Statistics scan_raw = summarize(std::move(scan_group_cycles), clock.ns_per_cycle);
    const OperationStatistics read = per_operation(read_raw, clock.overhead, kReadsPerGroup);
    const OperationStatistics scan = per_operation(scan_raw, clock.overhead, kScansPerGroup);

    std::printf(
        "per-operation ns | group timing: one clock read pair per group of operations; net = "
        "minus empty-region overhead / operations\n");
    std::printf("%-12s %10s %11s %10s %10s %10s %10s\n", "path", "groups", "mean_raw", "raw_p50",
                "raw_p99", "net_p50", "net_p99");
    const auto print_row = [](const char* name, const OperationStatistics& stats) {
        std::printf("%-12s %10lld %11.3f %10.3f %10.3f %10.3f %10.3f\n", name,
                    static_cast<long long>(stats.groups), stats.mean_raw, stats.p50_raw,
                    stats.p99_raw, stats.p50_net, stats.p99_net);
    };
    print_row("[n] read", read);
    print_row("span scan", scan);
    std::printf(
        "\nnote | p99 here is p99 of group means, not of single operations (see file header); "
        "read as aggregate cost\n");
    std::printf("fingerprint (anti-dead-code): read_sink=%.3f scan_sink=%.3f\n", read_sink,
                scan_sink);
    std::printf(
        "record: docs/agents/build-appendix.md, section \"performance benchmarks\" "
        "(append machine + date + numbers)\n");
    return 0;
}

}  // namespace

int main(int argc, char** argv) { return run(quick_mode(argc, argv)); }
