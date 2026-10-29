/**
 * @file bench_tick_update.cpp
 * @brief BarStrategy 框架 tick 热路径基准（issue #164；组件文档 §10 第一段）
 *
 * 量的是**框架 tick 路径**：`BarStrategyManager::on_tick`（实例表查找 → tick→bar 聚合 →
 * `on_bar_update` 回调分发），口径与组件文档 §10 的「A 框架计算路径（tick 聚合 + 回调分发 +
 * 基准 B 代码）p99 ≤ 1us」一致。**不含** shm 写（SDK 边界打桩，见
 * bench_framework_harness.h）。
 *
 * ## 参数（与 §10 预算口径一致）
 *
 * Release、单品种、单挂靠账户、`period_sec=60`、`max_lookback=500`、双均线基准策略
 * （`DualMaStrategy`，与 bar close 基准同一个 B）。历史回放 500 根静默填窗后进入 live，
 * live tick 流按「每秒一笔、每 60 笔换一根 bar」构造：每桶 59 笔纯 update + 1 笔换 bar，
 * 两条路径分开报告。换 bar 的那笔 tick 先触发 `on_bar_close`（双均线 + 每根反手的意图 +
 * fanout/上报），因此 new-bar 行含该成本，update 行才是纯 tick 聚合 + `on_bar_update`
 * 分发——§10 的 1µs「A 框架计算路径」预算只对照 update 行。
 *
 * ## 测量与自检
 *
 * - 每笔 tick 单独读一对 RDTSC → 逐笔分布与 p99；再扣减空区间读时钟开销给 net 分位（偏保守
 *   上界）。均值列是含读时钟开销的 raw 值（1µs 预算下开销 < 1%）。
 * - 分类由 B 观测的 `bar_index` 变化决定，**不看时间表反推**：框架若把 tick 丢了/没回调，
 *   `updates` 计数与分类样本数就对不上，自检失败退出 1。
 * - 自检（计时前/后）：启动后静默填窗期满 500 根、B 零回调（回调门禁）；live 期
 *   `updates == ticks`、换 bar 次数 == `ceil(ticks / 60)`、每根换 bar 恰 1 次 open/close/订单
 *   与 1 次逻辑持仓上报；任一不符即失败。
 * - 计时后打印终态指纹，用于跨机器/跨 commit 判断「是否真的跑了同一份工作」。
 *
 * ## 运行
 *
 * ```bash
 * ./scripts/build.sh
 * ./scripts/test.sh --label perf                       # 全部 perf 基准（Release）
 * DZTRADER_HOME=$PWD/.dztrader_dev \
 *   ./build/linux/x86_64/Release/tests/bench_tick_update   # 直接跑（完整输出）
 * ./build/linux/x86_64/Release/tests/bench_tick_update --quick  # 小样本自测
 * ```
 *
 * 只报告数值、不设阈值断言（report-only，见 docs/agents/build-appendix.md「性能基准」）。
 */
#include "bench_framework_harness.h"
#include "bench_support.h"

#include <chrono>
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
using dztrader::strategy_api::bench_framework::kFirstTradingDay;
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
/// 默认采样 tick 数（1e6 使 p99 由约 1e4 个尾部样本决定，抗单次中断噪声）
constexpr std::int64_t kDefaultTicks = 1'000'000;
/// `--quick` 采样 tick 数（秒级内跑完的自测档）
constexpr std::int64_t kQuickTicks = 20'000;
/// 静默填窗期 B 的 update 次数：`bar_index < max_lookback` 的根不回调（组件文档 §5.2），
/// 回放 500 根恰好在门禁前结束，故启动后应为 0。
constexpr std::int64_t kReplayUpdates = 0;

/**
 * @brief 终态指纹（防死代码消除 + 跨机器/跨 commit 可复现校验）
 * @return FNV 混入后的 64 位指纹（B 最后收盘价 + 终态 bar 序号 + 订单数）
 */
std::uint64_t fingerprint() {
    Fingerprint fingerprint;
    fingerprint.mix_double(g_observer.last_close);
    fingerprint.mix(static_cast<std::uint64_t>(g_observer.bar_index));
    fingerprint.mix(static_cast<std::uint64_t>(g_sdk.orders));
    return fingerprint.value();
}

/**
 * @brief 跑 benchmark
 * @param quick true = 小样本自测档
 * @return 进程退出码（0 = 自检通过并输出报告）
 */
int run(bool quick) {
    constexpr const char* kBench = "bench_tick_update";
    const std::int64_t tick_count = quick ? kQuickTicks : kDefaultTicks;

    const Clock clock = make_clock(kBench);
    dztrader::strategy_api::bench_support::print_bench_header(
        "dztrader bar strategy benchmark: tick update path (issue #164)",
        "framework tick path | tick-to-bar aggregation + B callback dispatch, no shm writes",
        clock);
    std::printf(
        "scenario | instruments=1 accounts=1 period_sec=%d max_lookback=%d history_bars=%d "
        "ticks=%lld strategy=dual_ma\n",
        kPeriodSec, kMaxLookback, kMaxLookback, static_cast<long long>(tick_count));
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
    // live 第一笔 tick 必须从历史末根的当日累计量额继续（同一交易日内不得倒退）
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
    // 启动后：静默填窗已跑完 500 根，回调门禁要求 `bar_index >= max_lookback`（组件文档
    // §5.2），故 B 在回放期零回调；第一条回调发生在 live 首个换 bar tick。
    const std::int64_t updates_after_start = g_observer.counts.updates;
    const std::int64_t bar_index_after_start = g_observer.bar_index;
    if (!expect(updates_after_start == kReplayUpdates, kBench, "check=replay_updates contract=0") ||
        !expect(bar_index_after_start == -1, kBench, "check=replay_bar_index contract=-1")) {
        std::fprintf(stderr, "actual | updates=%lld bar_index=%lld\n",
                     static_cast<long long>(updates_after_start),
                     static_cast<long long>(bar_index_after_start));
        return 1;
    }

    // live 流：从历史末根 bar 的 last_tick_sec（tsec 29999）之后 1 秒起，每 tick 前进 1 秒；
    // 当日累计量额从历史末根续接（同一交易日内不得倒退）。
    TickStream stream(last_history_bar.trading_day, last_history_bar.last_tick_sec,
                      last_history_bar.cum_volume, last_history_bar.cum_turnover);
    std::vector<std::uint64_t> update_cycles;
    std::vector<std::uint64_t> new_bar_cycles;
    update_cycles.reserve(static_cast<std::size_t>(tick_count));
    new_bar_cycles.reserve(static_cast<std::size_t>(tick_count / kPeriodSec + 1));
    std::int64_t previous_bar_index = bar_index_after_start;
    const auto wall_begin = std::chrono::steady_clock::now();
    for (std::int64_t i = 0; i < tick_count; ++i) {
        const DzTick tick = stream.next(1, i);
        const std::uint64_t begin = tsc_now();
        rig.manager().on_tick(tick);
        const std::uint64_t end = tsc_now();
        if (g_observer.bar_index > previous_bar_index) {
            new_bar_cycles.push_back(end - begin);
            previous_bar_index = g_observer.bar_index;
        } else {
            update_cycles.push_back(end - begin);
        }
    }
    const auto wall_end = std::chrono::steady_clock::now();
    const double wall_ns = std::chrono::duration<double, std::nano>(wall_end - wall_begin).count();

    // ---- 路径身份自检 ----
    // 流从桶边界开始：每 60 笔 tick 换一根 bar（首笔即换 bar）。
    const std::int64_t expected_new_bar_ticks = (tick_count + kPeriodSec - 1) / kPeriodSec;
    const std::int64_t expected_updates = tick_count + kReplayUpdates;
    const std::int64_t expected_final_bar_index = (kMaxLookback - 1) + expected_new_bar_ticks;
    const std::int64_t observed_updates = g_observer.counts.updates;
    const std::int64_t observed_opens = g_observer.counts.opens;
    const std::int64_t observed_closes = g_observer.counts.closes;
    const std::int64_t observed_new_bar_ticks = static_cast<std::int64_t>(new_bar_cycles.size());
    const std::int64_t observed_final_bar_index = g_observer.bar_index;
    bool ok = expect(observed_updates == expected_updates, kBench, "check=updates");
    ok = expect(observed_new_bar_ticks == expected_new_bar_ticks, kBench, "check=new_bar_ticks") &&
         ok;
    ok = expect(observed_opens == observed_new_bar_ticks, kBench, "check=opens") && ok;
    ok = expect(observed_closes == observed_new_bar_ticks, kBench, "check=closes") && ok;
    ok = expect(observed_final_bar_index == expected_final_bar_index, kBench,
                "check=final_bar_index") &&
         ok;
    // 每根 close 恰好一次反手意图 → 1 张差量单 + 1 次逻辑持仓上报；启动期另有 1 次全量上报。
    ok = expect(g_sdk.orders == expected_new_bar_ticks, kBench, "check=orders") && ok;
    ok =
        expect(g_sdk.position_publishes == 1 + expected_new_bar_ticks, kBench, "check=publishes") &&
        ok;
    if (!ok) {
        std::fprintf(
            stderr,
            "actual | updates=%lld expected=%lld new_bar_ticks=%lld expected=%lld "
            "opens=%lld closes=%lld final_bar_index=%lld expected=%lld orders=%lld "
            "publishes=%lld\n",
            static_cast<long long>(observed_updates), static_cast<long long>(expected_updates),
            static_cast<long long>(observed_new_bar_ticks),
            static_cast<long long>(expected_new_bar_ticks), static_cast<long long>(observed_opens),
            static_cast<long long>(observed_closes),
            static_cast<long long>(observed_final_bar_index),
            static_cast<long long>(expected_final_bar_index), static_cast<long long>(g_sdk.orders),
            static_cast<long long>(g_sdk.position_publishes));
        return 1;
    }

    // ---- 报告 ----
    const Statistics update_raw = summarize(std::move(update_cycles), clock.ns_per_cycle);
    const Statistics new_bar_raw = summarize(std::move(new_bar_cycles), clock.ns_per_cycle);
    const Statistics update = subtract_overhead(update_raw, clock.overhead);
    const Statistics new_bar = subtract_overhead(new_bar_raw, clock.overhead);

    std::printf(
        "per-tick ns | one clock read pair per tick (raw = timer included, net = minus "
        "empty-region overhead)\n");
    std::printf("%-10s %10s %11s %10s %10s %10s %10s\n", "path", "samples", "mean_raw", "raw_p50",
                "raw_p99", "net_p50", "net_p99");
    const auto print_row = [](const char* name, const Statistics& raw, const Statistics& net) {
        std::printf("%-10s %10lld %11.2f %10.2f %10.2f %10.2f %10.2f\n", name,
                    static_cast<long long>(raw.count), raw.mean, raw.p50, raw.p99, net.p50,
                    net.p99);
    };
    print_row("update", update_raw, update);
    print_row("new-bar", new_bar_raw, new_bar);
    std::printf(
        "\nstream cross-check: wall=%.3fms wall_ns/tick=%.2f (upper bound: includes tick "
        "generation and loop overhead)\n",
        wall_ns / 1.0e6, wall_ns / static_cast<double>(tick_count));

    // 组件文档 §10：A 框架计算路径 p99 <= 1us。report-only：只对照不阻断。
    const double target_ns = 1000.0;
    std::printf(
        "\ntarget | path=update net_p99=%.2fns target=%.0fns %s (component doc §10; "
        "report-only)\n",
        update.p99, target_ns, update.p99 <= target_ns ? "met" : "missed");
    std::printf(
        "reported | path=new-bar net_p99=%.2fns (no target; includes one on_bar_close with dual "
        "MA + intent + fanout)\n",
        new_bar.p99);
    std::printf("\nfingerprint (reproducibility + anti-dead-code): %016llx\n",
                static_cast<unsigned long long>(fingerprint()));
    std::printf(
        "record: docs/agents/build-appendix.md, section \"performance benchmarks\" "
        "(append machine + date + numbers)\n");
    return 0;
}

}  // namespace

int main(int argc, char** argv) { return run(quick_mode(argc, argv)); }
