/**
 * @file bench_bar_close_dual_ma.cpp
 * @brief BarStrategy 框架 bar close 热路径基准（issue #164；组件文档 §10 第二段）
 *
 * 量的是**bar close 框架路径**：「`on_bar_close` 分发 + 双均线指标 + 下单意图 fanout +
 * 逻辑持仓上报」，口径与组件文档 §10 的「bar close 框架路径 p99 ≤ 1us」一致。**不含** shm
 * 写（SDK 边界打桩，见 bench_framework_harness.h）——那一段是「决策 → 订单帧落 shm」的
 * 端到端成本，组件文档 §10 把它单列，本基准不覆盖。
 *
 * ## 参数（与 §10 预算口径一致）
 *
 * Release、单品种、单挂靠账户（scale 1.0）、`period_sec=60`、`max_lookback=500`、双均线基准
 * 策略（`DualMaStrategy`：`on_bar_close` 里用 `close[0..4]` / `close[0..19]` 算 5/20 均线，
 * 再按基准的最坏情况每根反手一次，保证每根 bar 的 close 都走满 fanout）。live 流每 tick 前进
 * 一根 bar（60 交易日内秒），故每笔被测 tick 恰好触发一次 `close → open → update`。
 *
 * ## 两个数值
 *
 * - `close tick`：整个 `on_tick`（close 分发 + B 回调体含指标/fanout/上报 + open/update
 *   分发）的逐笔 p50/p99 —— 对应 §10 的「bar close 框架路径」预算；
 * - `b callback body`：B 在 `on_bar_close` 内用 RDTSC 圈出的自身开销（指标 + 意图 + fanout +
 *   上报），用于把框架分发成本与 B 自身成本分开看；两者相减不要当精确分解（各自含一次
 *   RDTSC 读时钟开销，见 net 列）。
 *
 * ## 自检
 *
 * 每 tick 必须恰好 +1 `close` / +1 `open` / +1 `update`、恰好 1 张订单（每根反手一次，差量
 * 非 0）与 1 次逻辑持仓上报；启动期初始上报 1 次（`enter_live` 全量对账）。任一不符即退出 1。
 *
 * ## 运行
 *
 * ```bash
 * ./scripts/build.sh
 * ./scripts/test.sh --label perf
 * DZTRADER_HOME=$PWD/.dztrader_dev \
 *   ./build/linux/x86_64/Release/tests/bench_bar_close_dual_ma
 * ./build/linux/x86_64/Release/tests/bench_bar_close_dual_ma --quick
 * ```
 *
 * 只报告数值、不设阈值断言（report-only）。
 */
#include "bench_framework_harness.h"
#include "bench_support.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

namespace {

using dztrader::strategy_api::bench_framework::BenchConfig;
using dztrader::strategy_api::bench_framework::check_wiring;
using dztrader::strategy_api::bench_framework::DualMaStrategy;
using dztrader::strategy_api::bench_framework::g_observer;
using dztrader::strategy_api::bench_framework::g_sdk;
using dztrader::strategy_api::bench_framework::kInstrumentId;
using dztrader::strategy_api::bench_framework::make_history;
using dztrader::strategy_api::bench_framework::ManagerRig;
using dztrader::strategy_api::bench_framework::reset_observer;
using dztrader::strategy_api::bench_framework::reset_sdk_counters;
using dztrader::strategy_api::bench_framework::TickStream;
using dztrader::strategy_api::bench_support::Clock;
using dztrader::strategy_api::bench_support::expect;
using dztrader::strategy_api::bench_support::Fingerprint;
using dztrader::strategy_api::bench_support::make_clock;
using dztrader::strategy_api::bench_support::quick_mode;
using dztrader::strategy_api::bench_support::Statistics;
using dztrader::strategy_api::bench_support::subtract_overhead;
using dztrader::strategy_api::bench_support::summarize;
using dztrader::strategy_api::bench_support::tsc_now;

/// 基准口径：1 分钟 bar（组件文档 §10）
constexpr std::int32_t kPeriodSec = 60;
/// 基准口径：可读深度 500（组件文档 §10）
constexpr std::int32_t kMaxLookback = 500;
/// 默认采样 bar 数（20 万根 close；p99 由 2000 个尾部样本决定）
constexpr std::int64_t kDefaultBars = 200'000;
/// `--quick` 采样 bar 数（秒级内跑完的自测档）
constexpr std::int64_t kQuickBars = 20'000;

/// @brief 终态指纹（防死代码消除 + 跨机器/跨 commit 可复现校验）
/// @return FNV 混入后的 64 位指纹（B 最后收盘价 + close 次数 + 订单数）
std::uint64_t fingerprint() {
    Fingerprint fingerprint;
    fingerprint.mix_double(g_observer.last_close);
    fingerprint.mix(static_cast<std::uint64_t>(g_observer.counts.closes));
    fingerprint.mix(static_cast<std::uint64_t>(g_sdk.orders));
    return fingerprint.value();
}

/**
 * @brief 跑 benchmark
 * @param quick true = 小样本自测档
 * @return 进程退出码
 */
int run(bool quick) {
    constexpr const char* kBench = "bench_bar_close_dual_ma";
    const std::int64_t bar_count = quick ? kQuickBars : kDefaultBars;

    const Clock clock = make_clock(kBench);
    dztrader::strategy_api::bench_support::print_bench_header(
        "dztrader bar strategy benchmark: bar close path with dual moving average (issue #164)",
        "framework bar close path | on_bar_close dispatch + dual MA + intent fanout + position "
        "report, no shm writes",
        clock);
    std::printf(
        "scenario | instruments=1 accounts=1 period_sec=%d max_lookback=%d history_bars=%d "
        "bars=%lld strategy=dual_ma (5/20, intent every close)\n",
        kPeriodSec, kMaxLookback, kMaxLookback, static_cast<long long>(bar_count));
    if (quick) {
        std::printf("quick mode | reduced sample sizes (--quick)\n");
    }
    std::printf("\n");

    BenchConfig config{
        .strategy_id = kBench,
        .instruments = {std::string(kInstrumentId)},
        .period_sec = kPeriodSec,
        .max_lookback = kMaxLookback,
        .warmup_bars = kMaxLookback,
    };
    std::vector<dztrader::bar::Bar> history = dztrader::strategy_api::bench_framework::make_history(
        static_cast<std::size_t>(kMaxLookback), config);
    const dztrader::bar::Bar last_history_bar = history.back();

    reset_sdk_counters();
    reset_observer();
    ManagerRig<DualMaStrategy> rig(config, std::move(history));
    if (!rig.start()) {
        return 1;
    }
    if (!check_wiring(kBench)) {
        return 1;
    }
    // 启动后：500 根静默填窗恰好在回调门禁前结束（bar_index < max_lookback 不回调），故
    // B 在回放期零 close；`enter_live` 的全量对账已上报 1 次（0 持仓）。
    if (!expect(g_observer.counts.closes == 0, kBench, "check=replay_closes contract=0") ||
        !expect(g_observer.counts.inits == 1, kBench, "check=on_init contract=1") ||
        !expect(g_sdk.position_publishes == 1, kBench, "check=enter_live_report contract=1")) {
        std::fprintf(stderr, "actual | closes=%lld inits=%lld publishes=%lld\n",
                     static_cast<long long>(g_observer.counts.closes),
                     static_cast<long long>(g_observer.counts.inits),
                     static_cast<long long>(g_sdk.position_publishes));
        return 1;
    }
    g_observer.record_close_cycles = true;
    g_observer.close_cycles.reserve(static_cast<std::size_t>(bar_count) + 1);

    // live 流：每 tick 前进一根 bar（60 交易日内秒）。首笔落在历史末根之后 60 秒，
    // 当日累计量额从历史末根续接（同一交易日内不得倒退）。
    TickStream stream(last_history_bar.trading_day, last_history_bar.last_tick_sec,
                      last_history_bar.cum_volume, last_history_bar.cum_turnover);
    std::vector<std::uint64_t> close_tick_cycles;
    close_tick_cycles.reserve(static_cast<std::size_t>(bar_count) + 1);
    for (std::int64_t i = 0; i < bar_count; ++i) {
        const DzTick tick = stream.next(kPeriodSec, i);
        const std::uint64_t begin = tsc_now();
        rig.manager().on_tick(tick);
        const std::uint64_t end = tsc_now();
        close_tick_cycles.push_back(end - begin);
    }

    // ---- 路径身份自检 ----
    const std::int64_t observed_closes = g_observer.counts.closes;
    const std::int64_t observed_opens = g_observer.counts.opens;
    const std::int64_t observed_updates = g_observer.counts.updates;
    const std::int64_t observed_orders = g_sdk.orders;
    const std::int64_t observed_publishes = g_sdk.position_publishes;
    const std::int64_t observed_callbacks =
        static_cast<std::int64_t>(g_observer.close_cycles.size());
    bool ok = expect(observed_closes == bar_count, kBench, "check=closes");
    ok = expect(observed_opens == bar_count, kBench, "check=opens") && ok;
    ok = expect(observed_updates == bar_count, kBench, "check=updates") && ok;
    ok = expect(observed_callbacks == bar_count, kBench, "check=close_callbacks") && ok;
    ok = expect(observed_orders == bar_count, kBench, "check=orders") && ok;
    ok = expect(observed_publishes == 1 + bar_count, kBench, "check=publishes") && ok;
    if (!ok) {
        std::fprintf(
            stderr,
            "actual | closes=%lld opens=%lld updates=%lld close_callbacks=%lld "
            "orders=%lld publishes=%lld expected_bars=%lld\n",
            static_cast<long long>(observed_closes), static_cast<long long>(observed_opens),
            static_cast<long long>(observed_updates), static_cast<long long>(observed_callbacks),
            static_cast<long long>(observed_orders), static_cast<long long>(observed_publishes),
            static_cast<long long>(bar_count));
        return 1;
    }

    // ---- 报告 ----
    const Statistics close_tick_raw = summarize(std::move(close_tick_cycles), clock.ns_per_cycle);
    const Statistics close_tick = subtract_overhead(close_tick_raw, clock.overhead);
    std::vector<std::uint64_t> callback_cycles = g_observer.close_cycles;
    const Statistics callback_raw = summarize(std::move(callback_cycles), clock.ns_per_cycle);
    const Statistics callback = subtract_overhead(callback_raw, clock.overhead);

    std::printf(
        "per close ns | raw = timer included, net = minus empty-region overhead; B callback body "
        "times itself inside on_bar_close\n");
    std::printf("%-16s %10s %11s %10s %10s %10s %10s\n", "measure", "samples", "mean_raw",
                "raw_p50", "raw_p99", "net_p50", "net_p99");
    const auto print_row = [](const char* name, const Statistics& raw, const Statistics& net) {
        std::printf("%-16s %10lld %11.2f %10.2f %10.2f %10.2f %10.2f\n", name,
                    static_cast<long long>(raw.count), raw.mean, raw.p50, raw.p99, net.p50,
                    net.p99);
    };
    print_row("close tick", close_tick_raw, close_tick);
    print_row("b callback body", callback_raw, callback);
    std::printf(
        "\nnote | close tick = framework close dispatch + B body (indicator + intent + fanout + "
        "report) + open/update dispatch\n");
    std::printf(
        "note | orders=%lld publishes=%lld over %lld closes (1 order + 1 report per "
        "close expected)\n",
        static_cast<long long>(g_sdk.orders), static_cast<long long>(g_sdk.position_publishes),
        static_cast<long long>(g_observer.counts.closes));

    // 组件文档 §10：bar close 框架路径 p99 <= 1us。report-only：只对照不阻断。
    const double target_ns = 1000.0;
    std::printf(
        "\ntarget | path=close_tick net_p99=%.2fns target=%.0fns %s (component doc §10; "
        "report-only)\n",
        close_tick.p99, target_ns, close_tick.p99 <= target_ns ? "met" : "missed");
    std::printf("\nfingerprint (reproducibility + anti-dead-code): %016llx\n",
                static_cast<unsigned long long>(fingerprint()));
    std::printf(
        "record: docs/agents/build-appendix.md, section \"performance benchmarks\" "
        "(append machine + date + numbers)\n");
    return 0;
}

}  // namespace

int main(int argc, char** argv) { return run(quick_mode(argc, argv)); }
