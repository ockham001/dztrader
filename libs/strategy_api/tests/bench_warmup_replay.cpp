/**
 * @file bench_warmup_replay.cpp
 * @brief BarStrategy 历史预热回放吞吐基准（issue #164；组件文档 §10 基准项
 *        `bench_warmup_replay`）
 *
 * 量的是**启动期历史回放**的吞吐：`on_start` → 查历史（注入假来源）→ 静默填窗 → 每根归档
 * bar 拆 4 个合成 tick 重放（走与 live 同一条 builder 路径、B 六个月回调照常运行、下单只做
 * 虚拟成交）→ 进入 live。组件文档 §10 的预算口径：「K=10,000、50 品种约 0.5–1s/进程」，
 * 成本估算 = 合成 tick × 0.1µs + 回放期 B 回调 × 0.3µs。
 *
 * ## 口径
 *
 * 单品种、`period_sec=60`、`max_lookback=500`、`warmup.bars=10,000`（§10 基准参数）、双均线
 * 策略（`DualMaStrategy`，回放期 B 照常运行：每根进入回调期的 bar 有 `close → open → 4×update`
 * 共 6 次回调；意图只做虚拟成交、不发单）。
 *
 * 每轮用**全新** `BarStrategyManager` 跑一次完整 `on_start`（配置加载 + 元数据 + 实例装配 +
 * 回放 + 进入 live），用 `steady_clock` 量整段墙钟：
 *
 * - `warm K bars`：带 K 根历史 → 静默填窗 500 根 + 回放 K-500 根；
 * - `cold`：同样配置但历史源不可用（冷启动降级，无回放）→ 作为基线扣掉「与回放无关的
 *   启动固定成本」（配置/元数据/实例装配），得到回放本身的派生成本；
 * - `replay only`（派生）= warm − cold：回放吞吐 = (K − max_lookback) / replay_only。
 *
 * 启动期诊断写 stderr（约 10 行）计入被测区间——那是生产 `on_start` 真实发生的工作，但量级
 * 远小于回放（< 1%，报告里说明）。
 *
 * ## 自检
 *
 * 每轮回放后断言 B 的回调计数与公式完全一致：进入回调期的 bar 数 = `K - max_lookback`，
 * 故 `opens == closes == K - max_lookback`、`updates == 4 × (K - max_lookback)`；回放期
 * `live_ == false`，故**零订单**、`enter_live` 只做 1 次全量上报。任一不符退出 1。
 *
 * ## 运行
 *
 * ```bash
 * ./scripts/build.sh
 * ./scripts/test.sh --label perf
 * DZTRADER_HOME=$PWD/.dztrader_dev \
 *   ./build/linux/x86_64/Release/tests/bench_warmup_replay
 * ./build/linux/x86_64/Release/tests/bench_warmup_replay --quick   # K=1000
 * ```
 *
 * 只报告数值、不设阈值断言（report-only）。
 */
#include "bench_framework_harness.h"
#include "bench_support.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <utility>
#include <vector>

namespace {

using dztrader::strategy_api::bench_framework::BenchConfig;
using dztrader::strategy_api::bench_framework::DualMaStrategy;
using dztrader::strategy_api::bench_framework::g_observer;
using dztrader::strategy_api::bench_framework::g_sdk;
using dztrader::strategy_api::bench_framework::kInstrumentId;
using dztrader::strategy_api::bench_framework::make_history;
using dztrader::strategy_api::bench_framework::ManagerRig;
using dztrader::strategy_api::bench_framework::reset_observer;
using dztrader::strategy_api::bench_framework::reset_sdk_counters;
using dztrader::strategy_api::bench_support::expect;
using dztrader::strategy_api::bench_support::make_clock;
using dztrader::strategy_api::bench_support::quick_mode;
using dztrader::strategy_api::bench_support::Statistics;
using dztrader::strategy_api::bench_support::summarize_values;

/// 基准口径：1 分钟 bar（组件文档 §10）
constexpr std::int32_t kPeriodSec = 60;
/// 基准口径：可读深度 500（组件文档 §10）
constexpr std::int32_t kMaxLookback = 500;
/// §10 基准口径：预热 10,000 根
constexpr std::int64_t kDefaultWarmupBars = 10'000;
/// `--quick` 预热根数（秒级自测）
constexpr std::int64_t kQuickWarmupBars = 1'000;
/// 默认 warm 轮数（每轮一次完整 on_start）
constexpr std::int64_t kDefaultWarmReps = 15;
/// 默认 cold 轮数（基线）
constexpr std::int64_t kDefaultColdReps = 5;
/// `--quick` 轮数
constexpr std::int64_t kQuickReps = 3;
/// §10 的 50 品种预算口径（串行回放，单实例成本 × 品种数）
constexpr std::int64_t kBudgetInstrumentCount = 50;
/// §10 回放成本估算：单 builder tick 0.1µs
constexpr double kEstimateUsPerBuilderTick = 0.1;
/// §10 回放成本估算：单 B 回调 0.3µs
constexpr double kEstimateUsPerCallback = 0.3;
/// 单根 bar 的合成 tick 数与 B 回调数（组件文档 §5.3）
constexpr std::int64_t kBuilderTicksPerBar = 4;
constexpr std::int64_t kCallbacksPerBar = 6;

/**
 * @brief 跑一轮启动并计时
 * @param config 基准配置
 * @param history 共享历史（nullptr = 冷启动）
 * @param expect_replay 是否期望发生回放（true = warm，false = cold）
 * @param elapsed_ns 输出：`on_start` 墙钟（ns）
 * @return true = 自检通过
 */
bool run_once(const BenchConfig& config,
              std::shared_ptr<const std::vector<dztrader::bar::Bar>> history,
              bool expect_replay,
              double& elapsed_ns) {
    reset_sdk_counters();
    reset_observer();
    ManagerRig<DualMaStrategy> rig(config, std::move(history));
    const auto begin = std::chrono::steady_clock::now();
    const bool started = rig.start();
    const auto end = std::chrono::steady_clock::now();
    elapsed_ns = std::chrono::duration<double, std::nano>(end - begin).count();
    if (!started) {
        return false;
    }

    const std::int64_t replayed_bars = static_cast<std::int64_t>(config.warmup_bars) - kMaxLookback;
    const std::int64_t expected_opens = expect_replay ? replayed_bars : 0;
    const std::int64_t expected_closes = expect_replay ? replayed_bars : 0;
    const std::int64_t expected_updates = expect_replay ? replayed_bars * kBuilderTicksPerBar : 0;
    bool ok =
        expect(g_observer.counts.opens == expected_opens, "bench_warmup_replay", "check=opens");
    ok = expect(g_observer.counts.closes == expected_closes, "bench_warmup_replay",
                "check=closes") &&
         ok;
    ok = expect(g_observer.counts.updates == expected_updates, "bench_warmup_replay",
                "check=updates") &&
         ok;
    ok = expect(g_observer.counts.inits == 1, "bench_warmup_replay", "check=on_init contract=1") &&
         ok;
    // 回放期 live_ == false：意图只做虚拟成交，绝不发单/上报；enter_live 只做 1 次全量上报。
    ok = expect(g_sdk.orders == 0, "bench_warmup_replay", "check=zero_orders") && ok;
    ok = expect(g_sdk.position_publishes == 1, "bench_warmup_replay",
                "check=enter_live_report contract=1") &&
         ok;
    if (!ok) {
        std::fprintf(
            stderr,
            "actual | opens=%lld closes=%lld updates=%lld expected_bars=%lld inits=%lld "
            "orders=%lld publishes=%lld\n",
            static_cast<long long>(g_observer.counts.opens),
            static_cast<long long>(g_observer.counts.closes),
            static_cast<long long>(g_observer.counts.updates),
            static_cast<long long>(replayed_bars), static_cast<long long>(g_observer.counts.inits),
            static_cast<long long>(g_sdk.orders), static_cast<long long>(g_sdk.position_publishes));
        return false;
    }
    return true;
}

/// @brief 报告一行启动时长（ms；样本数少时 p99 无意义，故报 p50 与最大值）
void print_startup_row(const char* name, const Statistics& stats, const char* note) {
    std::printf("%-12s %7lld %11.3f %10.3f %10.3f   %s\n", name,
                static_cast<long long>(stats.count), stats.mean / 1.0e6, stats.p50 / 1.0e6,
                stats.max / 1.0e6, note);
}

/**
 * @brief 跑 benchmark
 * @param quick true = 小样本自测档
 * @return 进程退出码
 */
int run(bool quick) {
    constexpr const char* kBench = "bench_warmup_replay";
    const std::int64_t warmup_bars = quick ? kQuickWarmupBars : kDefaultWarmupBars;
    const std::int64_t warm_reps = quick ? kQuickReps : kDefaultWarmReps;
    const std::int64_t cold_reps = quick ? 1 : kDefaultColdReps;

    const auto clock = make_clock(kBench);
    dztrader::strategy_api::bench_support::print_bench_header(
        "dztrader bar strategy benchmark: warmup history replay throughput (issue #164)",
        "warmup replay | on_start history replay: silent fill + 4 synthetic ticks per archived bar "
        "+ B callbacks, virtual fills only",
        clock);
    std::printf(
        "scenario | instruments=1 period_sec=%d max_lookback=%d warmup_bars=%lld "
        "strategy=dual_ma warm_reps=%lld cold_reps=%lld\n",
        kPeriodSec, kMaxLookback, static_cast<long long>(warmup_bars),
        static_cast<long long>(warm_reps), static_cast<long long>(cold_reps));
    if (quick) {
        std::printf("quick mode | reduced warmup depth and reps (--quick)\n");
    }
    std::printf("\n");

    BenchConfig config{
        .strategy_id = kBench,
        .instruments = {std::string(kInstrumentId)},
        .period_sec = kPeriodSec,
        .max_lookback = kMaxLookback,
        .warmup_bars = warmup_bars,
    };
    auto warm_history = std::make_shared<const std::vector<dztrader::bar::Bar>>(
        make_history(static_cast<std::size_t>(warmup_bars), config));
    // 冷启动基线：历史源不可用（nullptr → Unavailable），无回放。
    std::shared_ptr<const std::vector<dztrader::bar::Bar>> cold_history;

    std::vector<double> warm_ns;
    std::vector<double> cold_ns;
    warm_ns.reserve(static_cast<std::size_t>(warm_reps));
    cold_ns.reserve(static_cast<std::size_t>(cold_reps));
    for (std::int64_t rep = 0; rep < warm_reps; ++rep) {
        double elapsed_ns = 0.0;
        if (!run_once(config, warm_history, /*expect_replay=*/true, elapsed_ns)) {
            std::fprintf(stderr, "warm rep failed | rep=%lld\n", static_cast<long long>(rep));
            return 1;
        }
        warm_ns.push_back(elapsed_ns);
    }
    for (std::int64_t rep = 0; rep < cold_reps; ++rep) {
        double elapsed_ns = 0.0;
        if (!run_once(config, cold_history, /*expect_replay=*/false, elapsed_ns)) {
            std::fprintf(stderr, "cold rep failed | rep=%lld\n", static_cast<long long>(rep));
            return 1;
        }
        cold_ns.push_back(elapsed_ns);
    }

    // ---- 报告 ----
    const Statistics warm = summarize_values(warm_ns);
    const Statistics cold = summarize_values(cold_ns);
    const std::int64_t replayed_bars = warmup_bars - kMaxLookback;
    const std::int64_t builder_ticks = replayed_bars * kBuilderTicksPerBar;
    const std::int64_t callbacks = replayed_bars * kCallbacksPerBar;
    const double estimate_us = static_cast<double>(builder_ticks) * kEstimateUsPerBuilderTick +
                               static_cast<double>(callbacks) * kEstimateUsPerCallback;
    // 回放成本（派生）= warm - cold；与回放无关的启动固定成本被扣掉。
    const double replay_only_ns = warm.p50 - cold.p50;
    const double replay_only_us = replay_only_ns / 1000.0;
    const double warm_total_us = warm.p50 / 1000.0;

    std::printf(
        "startup ms | steady_clock around one full on_start (config + metadata + instances + "
        "replay + live)\n");
    std::printf("%-12s %7s %11s %10s %10s   note\n", "phase", "reps", "mean_ms", "p50_ms",
                "max_ms");
    print_startup_row("warm", warm, "with history replay");
    print_startup_row("cold", cold, "history unavailable (baseline)");
    std::printf(
        "\nderived | replay_only_p50=%.3fms warm_total_p50=%.3fms replayed_bars=%lld "
        "builder_ticks=%lld callbacks=%lld\n",
        replay_only_us / 1000.0, warm_total_us / 1000.0, static_cast<long long>(replayed_bars),
        static_cast<long long>(builder_ticks), static_cast<long long>(callbacks));
    if (replay_only_ns > 0.0) {
        const double bars_per_second =
            static_cast<double>(replayed_bars) / (replay_only_ns / 1.0e9);
        const double us_per_bar = replay_only_us / static_cast<double>(replayed_bars);
        std::printf(
            "throughput | replay_only=%.1f bars/s us_per_bar=%.3f "
            "total_startup=%.1f bars/s\n",
            bars_per_second, us_per_bar, static_cast<double>(warmup_bars) / (warm.p50 / 1.0e9));
        std::printf(
            "compare | replay_only=%.3fms estimate=%.3fms (component doc §10: 0.1us/tick + "
            "0.3us/callback) ratio=%.2fx\n",
            replay_only_us / 1000.0, estimate_us / 1000.0,
            replay_only_us / (estimate_us > 0.0 ? estimate_us : 1.0));
        const double projected_50s =
            (replay_only_ns / 1.0e9) * static_cast<double>(kBudgetInstrumentCount);
        const bool within_budget = projected_50s <= 1.0;
        std::printf(
            "projection | 50 instruments serial=%.4fs budget=0.5..1s verdict=%s (component doc "
            "§10; report-only)\n",
            projected_50s,
            within_budget ? (projected_50s >= 0.5 ? "within_budget" : "below_budget_faster")
                          : "above_budget");
    } else {
        std::printf("throughput | replay_only <= 0 (below timer resolution); increase depth\n");
    }
    std::printf(
        "note | cold baseline removes fixed startup cost (config/metadata/instances); watchdog "
        "cancellation is not measured\n");
    std::printf("fingerprint (path identity) | per_warm_rep opens=%lld closes=%lld updates=%lld\n",
                static_cast<long long>(replayed_bars), static_cast<long long>(replayed_bars),
                static_cast<long long>(replayed_bars * kBuilderTicksPerBar));
    std::printf(
        "record: docs/agents/build-appendix.md, section \"performance benchmarks\" "
        "(append machine + date + numbers)\n");
    return 0;
}

}  // namespace

int main(int argc, char** argv) { return run(quick_mode(argc, argv)); }
