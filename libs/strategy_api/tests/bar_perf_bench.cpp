/**
 * @file bar_perf_bench.cpp
 * @brief tick→bar 热路径基准：量化 update / 新 bar / 跨日 / 竞价四条路径的 ns/tick 与 p99
 *
 * 对应 issue #93 与 ADR 0031 决策 10：单周期更新目标 p99 ≤ 50ns/tick，新 bar 创建、跨日、
 * 集合竞价路径单独报告。本基准**只报告数值、不做阈值断言**（仓库单测标准 4.3 要求基准与
 * 秒级单测分层；阈值待数值稳定后才升级为 CI 门禁）。
 *
 * ## 测量口径
 *
 * 每个 case 的 tick 块分成三段：`setup`（建立初始 bar，不计时）→ `timed`（全部是被测路径的
 * tick，连续）→ `teardown`（让竞价待补发队列归零等收尾动作，不计时）。块在计时循环**之外**
 * 生成一次，循环里反复用「新鲜 builder + 同一块」重放：这样测的是组件自身开销，不会混入
 * 每次重建 tick 数组的冷内存与分配成本（早期版本把 296 字节 `DzTick` 按值穿过 visitor 逐笔
 * 拷贝、并每轮重建整块，update 路径被量成约 46ns/tick；修正后同一组件约 13ns/tick）。
 *
 * 计时区间只包含 `timed` 段，且 `timed` 段内**全部** tick 都属于被测路径，故分母就是路径
 * 自身的 tick 数，不受首笔建立 bar 或收尾笔污染。
 *
 * 计时用 x86 RDTSC：预算只有 50ns/tick，本机成对读 steady_clock 约 17ns、成对 RDTSC 约
 * 9ns，后者才不至于把计时开销吃进被测区间。TSC 频率启动时用 steady_clock 标定，所有周期数
 * 一律换算成 ns，跨机器/跨构建可比。
 *
 * 两个互补的 pass：
 * - **批量**：每轮 `timed` 段整体读一对时钟，均值除以 tick 数 → 准确的 ns/tick 均值（成对读
 *   时钟的约 8.8ns 分摊到整段 50–200 笔，即 0.04–0.18ns/tick，不到被测值的 1%）。它回答
 *   「这条路径每 tick 平均多少 ns」。
 * - **逐笔**：每笔被测 tick 单独读一对时钟 → 真实逐笔分布与 p99，但必然叠加一次成对读时钟，
 *   故额外标定空区间开销并给出扣减后的 net 分位数。它回答「p99 是多少」。
 * 两者互为交叉校验：批量均值应接近逐笔 net_p50。批量 pass 同时用 steady_clock 量整段墙钟，
 * 给出不依赖 TSC 标定的第三个独立数值。
 *
 * ## 测量边界
 *
 * 测的是**组件自身**的 on_tick 计算：sink 用 NullBarSink（三个回调都是空实现，内联后归零），
 * 故消费方 sink 的自身开销不计入本基准；50ns 目标按 ADR 0031 决策 10 也只针对组件侧计算。
 * 真实 sink（例如把 Bar 写进环形缓冲）会额外增加消费方开销，属消费方预算、不在本基准范围内。
 *
 * ## 自检（防止量错路径）
 *
 * 计时前先用同一份块逐笔断言 builder 的公开状态（交易日 / 桶起点 / last_tick_sec /
 * is_final）与日程独立推导的期望一致，且丢弃计数恒为 0；不符则打印明细并退出 1。
 * 计时结束后把终态（当前 bar + 丢弃计数）折算成指纹打印，并让计时区间产出的样本分布一路
 * 参与报告输出，防止编译器把整段计时循环判为死代码消除；指纹本身还用于判断两次运行是否
 * 真的跑了同一份工作。
 *
 * 运行命令、机器信息与结果记录方式见 docs/agents/build-appendix.md「性能基准」。
 */

#include <dztrader/bar.h>
#include <dztrader/struct.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// 本基准依赖 x86_64 RDTSC。仓库支持 Windows/MSVC 与 Linux/GCC，两者都在 x86_64 上；
// 其余组合显式 #error，不静默回退到精度不足的计时源（AGENTS §四 跨平台）。clang 与 GCC
// 共用同一套 x86intrin 接口，故一并接受（`__GNUC__` 在 clang 下同样定义）。
#if !defined(__x86_64__) && !defined(_M_X64)
#error "bar_perf_bench requires x86_64 (rdtsc)"
#endif

#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(__GNUC__)
#include <cpuid.h>
#include <x86intrin.h>
#else
#error "bar_perf_bench supports MSVC or GCC/Clang only"
#endif

namespace {

using dztrader::bar::AuctionKind;
using dztrader::bar::AuctionWindow;
using dztrader::bar::Bar;
using dztrader::bar::BarBuilderStats;
using dztrader::bar::IntradayBarBuilder;
using dztrader::bar::NullBarSink;
using dztrader::bar::TradingSession;
using dztrader::bar::TradingSessionTable;
using dztrader::bar::TradingTime;

/// @brief 首个交易日（距纪元天数；tick.date 非正会被组件丢弃）
constexpr DzDate FIRST_DAY = 20000;
/// @brief 09:00 的交易日内秒
constexpr int32_t TSEC_09_00 = 54000;
/// @brief 09:30 的交易日内秒（竞价 case 的日盘小节开始）
constexpr int32_t TSEC_09_30 = 55800;
/// @brief 15:00 的交易日内秒
constexpr int32_t TSEC_15_00 = 75600;
/// @brief 日内容周期：1 分钟
constexpr int32_t PERIOD_SEC = 60;
/// @brief 竞价 tick 的目标桶起点（小节首桶，交易日内秒）
constexpr int32_t AUCTION_BUCKET_BEGIN = (TSEC_09_30 / PERIOD_SEC) * PERIOD_SEC;
/// @brief 竞价窗口起点（09:28:30）
constexpr int32_t AUCTION_WINDOW_BEGIN = TSEC_09_30 - 90;

/// @brief 每条路径的目标采样数（1e6 使 p99 由约 1e4 个尾部样本决定，抗单次中断噪声）
constexpr int64_t TARGET_SAMPLES = 1000000;
/// @brief 逐笔 pass 丢弃的预热轮数（分支预测与缓存状态稳定）
constexpr int64_t WARMUP_REPS = 200;
/// @brief 空区间成对读时钟开销的标定样本数
constexpr int64_t OVERHEAD_SAMPLES = 100000;
/// @brief TSC 频率标定窗口（毫秒）
constexpr int TSC_CALIBRATION_MS = 50;

// ============================================================
// 计时与机器信息
// ============================================================

/**
 * @brief 读取 TSC（前置 lfence 阻止前面的被测指令被重排到读时钟之后）
 *
 * lfence + rdtsc 不保证区间末尾的指令全部退役，但对本基准（report-only、相对比较）足够：
 * 绝对误差远小于列间差异。
 *
 * @return TSC 周期数
 */
inline uint64_t tsc_now() noexcept {
    _mm_lfence();
    return __rdtsc();
}

/**
 * @brief 用 steady_clock 标定 TSC 频率
 *
 * TSC 与核心频率解耦（constant_tsc/nonstop_tsc）时，以墙钟时长换算是唯一正确做法。
 *
 * @return TSC 频率（GHz）；标定失败返回 0
 */
double measure_tsc_ghz() {
    const auto wall_begin = std::chrono::steady_clock::now();
    const uint64_t tsc_begin = tsc_now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - wall_begin).count() <
           static_cast<double>(TSC_CALIBRATION_MS) / 1000.0) {
    }
    const uint64_t tsc_end = tsc_now();
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - wall_begin).count();
    if (seconds <= 0.0) {
        return 0.0;
    }
    return static_cast<double>(tsc_end - tsc_begin) / seconds / 1.0e9;
}

/**
 * @brief 读取 cpuid 叶（MSVC 与 GCC 两套接口）
 * @param leaf cpuid 叶号
 * @param regs 输出：EAX/EBX/ECX/EDX
 */
void cpuid_leaf(unsigned leaf, unsigned (&regs)[4]) {
#if defined(_MSC_VER)
    int out[4] = {};
    __cpuid(out, static_cast<int>(leaf));
    for (std::size_t i = 0; i < 4; ++i) {
        regs[i] = static_cast<unsigned>(out[i]);
    }
#else
    __get_cpuid(leaf, &regs[0], &regs[1], &regs[2], &regs[3]);
#endif
}

/// @brief CPU 品牌串（cpuid 扩展叶 0x80000002..0x80000004；不可用时返回 unknown）
std::string cpu_brand() {
    unsigned regs[4] = {};
    cpuid_leaf(0x80000000U, regs);
    if (regs[0] < 0x80000004U) {
        return "unknown";
    }
    std::array<char, 49> brand{};
    for (unsigned i = 0; i < 3; ++i) {
        cpuid_leaf(0x80000002U + i, regs);
        std::memcpy(brand.data() + static_cast<std::size_t>(i) * 16U, regs, 16U);
    }
    const std::string text(brand.data());
    const std::size_t first = text.find_first_not_of(' ');
    return first == std::string::npos ? std::string("unknown") : text.substr(first);
}

/// @brief 编译器标识（区分不同构建产物的数值）
std::string compiler_name() {
#if defined(_MSC_VER)
    return "msvc " + std::to_string(_MSC_VER);
#elif defined(__clang__)
    return "clang " + std::to_string(__clang_major__) + "." + std::to_string(__clang_minor__);
#else
    return "gcc " + std::to_string(__GNUC__) + "." + std::to_string(__GNUC_MINOR__);
#endif
}

/// @brief 平台标识（两个受支持平台在文件头已 gate，故无 else 分支）
std::string platform_name() {
#if defined(_WIN32)
    return "windows x86_64";
#else
    return "linux x86_64";
#endif
}

/// @brief 构建类型（Debug 数值无参考价值，报告显式告警）
std::string build_type_name() {
#ifdef NDEBUG
    return "release";
#else
    return "debug";
#endif
}

// ============================================================
// 统计
// ============================================================

/// @brief 一组计时样本的分布（ns）
struct Statistics {
    int64_t count = 0;  ///< 样本数
    double mean = 0.0;  ///< 均值
    double p50 = 0.0;   ///< 中位数
    double p99 = 0.0;   ///< 99 分位
    double max = 0.0;   ///< 最大值
};

/// @brief 周期数分布 → ns 统计量（会原地排序，故按值接收）
Statistics summarize(std::vector<uint64_t> cycles, double ns_per_cycle) {
    Statistics stats;
    stats.count = static_cast<int64_t>(cycles.size());
    if (cycles.empty()) {
        return stats;
    }
    std::sort(cycles.begin(), cycles.end());
    const auto quantile = [&cycles, ns_per_cycle](double q) {
        const auto index = static_cast<std::size_t>(static_cast<double>(cycles.size()) * q);
        return static_cast<double>(cycles[index]) * ns_per_cycle;
    };
    double total = 0.0;
    for (const uint64_t cycle : cycles) {
        total += static_cast<double>(cycle);
    }
    stats.mean = total / static_cast<double>(cycles.size()) * ns_per_cycle;
    stats.p50 = quantile(0.50);
    stats.p99 = quantile(0.99);
    stats.max = static_cast<double>(cycles.back()) * ns_per_cycle;
    return stats;
}

/// @brief 空区间成对读时钟的开销分布（逐笔计时的扣减基准）
Statistics measure_timer_overhead(double ns_per_cycle) {
    std::vector<uint64_t> cycles;
    cycles.reserve(static_cast<std::size_t>(OVERHEAD_SAMPLES));
    for (int64_t i = 0; i < OVERHEAD_SAMPLES; ++i) {
        const uint64_t begin = tsc_now();
        const uint64_t end = tsc_now();
        cycles.push_back(end - begin);
    }
    return summarize(std::move(cycles), ns_per_cycle);
}

// ============================================================
// 固定 tick 块
// ============================================================

/// @brief 四类被测路径
enum class Path {
    Update,    ///< 同桶 tick：只更新当前形成中 bar
    NewBar,    ///< 跨桶 tick：close 旧 bar + open 新 bar + update
    CrossDay,  ///< 跨交易日 tick：close 旧日 + open 新日 + update
    Auction,   ///< Open 竞价窗口内 tick：静默更新（窗口内不回调 sink）
};

/// @brief 一个基准 case：路径 + 交易小节表 + 周期
struct Case {
    const char* name;           ///< 报告用路径名
    Path path;                  ///< 被测路径
    TradingSessionTable table;  ///< 交易小节与竞价窗口表
    int32_t period_sec;         ///< 周期秒数
};

/// @brief 一笔 tick 由日程独立推出的期望状态（仅自检读，计时区间不读）
struct Expectation {
    DzDate trading_day;  ///< 期望的当前 bar 交易日
    int32_t begin_sec;   ///< 期望的当前 bar 桶起点（交易日内秒）
    int32_t tsec;        ///< 期望的 last_tick_sec
};

/// @brief 一个可重放的 tick 块：setup → timed（全部被测路径）→ teardown
struct Block {
    std::vector<DzTick> setup;                 ///< 建立初始 bar 的 tick（不计时）
    std::vector<Expectation> setup_expect;     ///< 与 setup 一一对应
    std::vector<DzTick> timed;                 ///< 被测路径的 tick（连续，计时区间只含此段）
    std::vector<Expectation> timed_expect;     ///< 与 timed 一一对应
    std::vector<DzTick> teardown;              ///< 收尾 tick（不计时，使状态可重复）
    std::vector<Expectation> teardown_expect;  ///< 与 teardown 一一对应

    /// @brief 被测路径的 tick 数（= 每次重放的采样数）
    [[nodiscard]] int64_t timed_count() const noexcept {
        return static_cast<int64_t>(timed.size());
    }
};

/**
 * @brief 确定性 tick 生成器：单合约；当日累计量额逐笔递增、交易日切换时归零
 *
 * 组件的坏数据守卫要求同交易日内累计量额不倒退，故累计值必须由生成器维护，不能是常数。
 * 价格在 100.00–100.96 之间确定性往返，保证为正且有高低变化，不依赖随机数。
 */
class TickGen {
public:
    /**
     * @brief 生成下一笔 tick
     * @param clock 墙钟秒（距午夜秒数）
     * @param day 交易日
     * @return 行情快照
     */
    DzTick next(DzTime clock, DzDate day) {
        ++index_;
        cum_volume_ += 1;
        cum_turnover_ += last_price_;
        DzTick tick{};
        tick.date = day;
        tick.time = clock;
        tick.last_price = last_price_;
        tick.volume = static_cast<DzVolume>(cum_volume_);
        tick.turnover = cum_turnover_;
        tick.open_interest = static_cast<DzLargeVolume>(1000 + index_ % 50);
        last_price_ = 100.0 + static_cast<double>(index_ % 97) * 0.01;
        return tick;
    }

    /// @brief 进入新交易日（当日累计量额基准归零）
    void new_day() {
        cum_volume_ = 0;
        cum_turnover_ = 0.0;
    }

private:
    int64_t cum_volume_ = 0;     ///< 当日累计成交量
    double cum_turnover_ = 0.0;  ///< 当日累计成交额
    int64_t index_ = 0;          ///< 已生成 tick 数（价格往返与 OI 用）
    double last_price_ = 100.0;  ///< 上一笔价格
};

/// @brief 单小节 09:00–15:00 的表（update / 新 bar / 跨日 case 用）
///
/// @note 与 bar_test_data.h 的 day_session_table() 形状相同是有意重复：基准是独立可执行文件，
///       只链 strategy_api_headers（不链 GTest），故不能复用那些带 GTest 依赖的测试辅助头。
///       两表一旦漂移，本基准的契约自检会立刻失败（它按本表独立推导期望状态）。
constexpr TradingSessionTable perf_day_table() {
    TradingSessionTable table{};
    table.session_count = 1;
    table.sessions[0] = TradingSession{TSEC_09_00, TSEC_15_00};
    return table;
}

/// @brief 09:30–15:00 小节 + 90 秒开盘竞价窗口 09:28:30–09:29:59（竞价 case 用）
constexpr TradingSessionTable perf_auction_table() {
    TradingSessionTable table{};
    table.session_count = 1;
    table.sessions[0] = TradingSession{TSEC_09_30, TSEC_15_00};
    table.auction_count = 1;
    table.auctions[0] =
        AuctionWindow{AuctionKind::Open, 0, AUCTION_WINDOW_BEGIN, AUCTION_WINDOW_BEGIN + 89};
    return table;
}

/// @brief 四个 case 的固定日程（顺序即报告顺序）
std::array<Case, 4> make_cases() {
    return {Case{"update", Path::Update, perf_day_table(), PERIOD_SEC},
            Case{"new-bar", Path::NewBar, perf_day_table(), PERIOD_SEC},
            Case{"cross-day", Path::CrossDay, perf_day_table(), PERIOD_SEC},
            Case{"auction", Path::Auction, perf_auction_table(), PERIOD_SEC}};
}

/**
 * @brief 生成一个 case 的可重放 tick 块
 *
 * 期望状态按组件的公开分桶规则独立推导（`floor(tsec / period) * period`；Open 竞价 tick
 * 归目标小节首桶），与被测实现无关。每块的 tick 数与形状见各 case 分支注释。
 *
 * @param the_case 被测 case
 * @return 已生成的块
 */
Block build_block(const Case& the_case) {
    Block block;
    TickGen gen;
    switch (the_case.path) {
        case Path::Update: {
            // setup：09:00:00 建立首桶 bar。timed：09:00:01–09:00:59 全部同桶 → 纯 update 路径
            // （59 笔）。只放一个桶，保证 timed 段内没有任何换桶/换日 tick 混入。
            constexpr int32_t TIMED_TICKS = PERIOD_SEC - 1;
            block.setup.push_back(gen.next(TradingTime::to_clock(TSEC_09_00), FIRST_DAY));
            block.setup_expect.push_back(
                Expectation{FIRST_DAY, (TSEC_09_00 / PERIOD_SEC) * PERIOD_SEC, TSEC_09_00});
            for (int32_t j = 1; j <= TIMED_TICKS; ++j) {
                const int32_t tsec = TSEC_09_00 + j;
                block.timed.push_back(gen.next(TradingTime::to_clock(tsec), FIRST_DAY));
                block.timed_expect.push_back(
                    Expectation{FIRST_DAY, (tsec / PERIOD_SEC) * PERIOD_SEC, tsec});
            }
            break;
        }
        case Path::NewBar: {
            // setup：09:00:00 建立首桶 bar。timed：每笔跨一个桶（200 笔）→ 纯换桶路径。
            constexpr int32_t TIMED_TICKS = 200;
            block.setup.push_back(gen.next(TradingTime::to_clock(TSEC_09_00), FIRST_DAY));
            block.setup_expect.push_back(
                Expectation{FIRST_DAY, (TSEC_09_00 / PERIOD_SEC) * PERIOD_SEC, TSEC_09_00});
            for (int32_t k = 1; k <= TIMED_TICKS; ++k) {
                const int32_t tsec = TSEC_09_00 + k * PERIOD_SEC;
                block.timed.push_back(gen.next(TradingTime::to_clock(tsec), FIRST_DAY));
                block.timed_expect.push_back(Expectation{FIRST_DAY, tsec, tsec});
            }
            break;
        }
        case Path::CrossDay: {
            // setup：首日 09:00:00 建立 bar。timed：之后每日 09:00:00（50 笔）→ 纯跨日路径。
            constexpr int32_t TIMED_DAYS = 50;
            block.setup.push_back(gen.next(TradingTime::to_clock(TSEC_09_00), FIRST_DAY));
            block.setup_expect.push_back(
                Expectation{FIRST_DAY, (TSEC_09_00 / PERIOD_SEC) * PERIOD_SEC, TSEC_09_00});
            for (int32_t d = 1; d <= TIMED_DAYS; ++d) {
                gen.new_day();
                const DzDate day = FIRST_DAY + d;
                block.timed.push_back(gen.next(TradingTime::to_clock(TSEC_09_00), day));
                block.timed_expect.push_back(
                    Expectation{day, (TSEC_09_00 / PERIOD_SEC) * PERIOD_SEC, TSEC_09_00});
            }
            break;
        }
        case Path::Auction: {
            // setup：窗口首笔建立竞价 bar（不回调）。timed：窗口内其余 89 笔 → 纯竞价静默更新。
            // teardown：窗口后的场内 tick（09:30:00）让待补发队列归零，使块可重复重放且不触发
            // 只有病态流才会走到的队列溢出行；它与竞价 bar 同桶，只做暴露与 update。
            constexpr int32_t TIMED_TICKS = 89;
            block.setup.push_back(gen.next(TradingTime::to_clock(AUCTION_WINDOW_BEGIN), FIRST_DAY));
            block.setup_expect.push_back(
                Expectation{FIRST_DAY, AUCTION_BUCKET_BEGIN, AUCTION_WINDOW_BEGIN});
            for (int32_t w = 1; w <= TIMED_TICKS; ++w) {
                const int32_t tsec = AUCTION_WINDOW_BEGIN + w;
                block.timed.push_back(gen.next(TradingTime::to_clock(tsec), FIRST_DAY));
                block.timed_expect.push_back(Expectation{FIRST_DAY, AUCTION_BUCKET_BEGIN, tsec});
            }
            block.teardown.push_back(gen.next(TradingTime::to_clock(TSEC_09_30), FIRST_DAY));
            block.teardown_expect.push_back(
                Expectation{FIRST_DAY, AUCTION_BUCKET_BEGIN, TSEC_09_30});
            break;
        }
    }
    return block;
}

// ============================================================
// 契约自检
// ============================================================

/**
 * @brief 判断两笔期望是否描述同一个「bar 状态轨迹」（跨日 case 每笔换日，需逐笔比较）
 * @param a 前一笔期望
 * @param b 后一笔期望
 * @return true = 交易日或桶起点发生变化
 */
bool bar_changes(const Expectation& a, const Expectation& b) {
    return a.trading_day != b.trading_day || a.begin_sec != b.begin_sec;
}

/// @brief 丢弃计数是否全为 0（任何丢弃都说明日程与表不匹配，数值不可信）
bool stats_clean(const BarBuilderStats& stats) {
    return stats.out_of_session == 0 && stats.out_of_order == 0 && stats.bad_volume == 0;
}

/// @brief 把丢弃计数打到 stderr（自检失败时定位用）
void print_stats(const char* name, const BarBuilderStats& stats) {
    std::fprintf(stderr,
                 "dropped ticks | case=%s out_of_session=%llu out_of_order=%llu bad_volume=%llu\n",
                 name, static_cast<unsigned long long>(stats.out_of_session),
                 static_cast<unsigned long long>(stats.out_of_order),
                 static_cast<unsigned long long>(stats.bad_volume));
}

/**
 * @brief 喂一段 tick 并逐笔校验 builder 公开状态与期望一致
 *
 * 日程被改坏（例如本应跨桶却落在同桶）会在这里失败，而不是让计时 pass 静默量错路径。
 *
 * @param builder 被测 builder
 * @param ticks 待喂入的 tick 段
 * @param expected 与 ticks 一一对应的期望
 * @param label 报告用的段名
 * @return 不一致笔数（0 = 日程与组件一致）
 */
int64_t check_segment(IntradayBarBuilder<NullBarSink>& builder,
                      const std::vector<DzTick>& ticks,
                      const std::vector<Expectation>& expected,
                      const char* label) {
    int64_t mismatches = 0;
    for (std::size_t i = 0; i < ticks.size(); ++i) {
        builder.on_tick(ticks[i]);
        const Expectation& want = expected[i];
        const char* field = nullptr;
        long long actual = 0;
        long long wanted = 0;
        if (!builder.has_current_bar()) {
            field = "has_current_bar";
        } else {
            const Bar& bar = builder.current_bar();
            if (bar.trading_day != want.trading_day) {
                field = "trading_day";
                actual = bar.trading_day;
                wanted = want.trading_day;
            } else if (bar.begin_sec != want.begin_sec) {
                field = "begin_sec";
                actual = bar.begin_sec;
                wanted = want.begin_sec;
            } else if (bar.last_tick_sec != want.tsec) {
                field = "last_tick_sec";
                actual = bar.last_tick_sec;
                wanted = want.tsec;
            } else if (bar.is_final) {
                field = "is_final";
                actual = 1;
            }
        }
        if (field == nullptr) {
            continue;
        }
        ++mismatches;
        if (mismatches <= 3) {
            std::fprintf(stderr,
                         "schedule mismatch | segment=%s index=%zu field=%s actual=%lld "
                         "expected=%lld\n",
                         label, i, field, actual, wanted);
        }
    }
    return mismatches;
}

/**
 * @brief 统计 sink（只用于路径身份自检，不参与计时）
 *
 * 每条路径必须产生的 sink 事件形态是路径定义的直接体现；用它断言「量到的确实是这条路径」，
 * 而不是只靠 tick 时间表反推。特别是竞价路径：ADR 0031 决策 5 要求窗口内零回调，若某次
 * 改动让窗口内开始回调，路径就不是竞价路径了，数值也不再有意义。
 */
class CountingSink {
public:
    void on_bar_open(const Bar&) { ++opens; }
    void on_bar_update(const Bar&) { ++updates; }
    void on_bar_close(const Bar&) { ++closes; }

    int64_t opens = 0;    ///< on_bar_open 次数
    int64_t updates = 0;  ///< on_bar_update 次数
    int64_t closes = 0;   ///< on_bar_close 次数
};

/// @brief 一条路径在 timed 段内必须产生的 sink 事件数
struct ExpectedEvents {
    int64_t opens;    ///< 期望 on_bar_open 次数
    int64_t updates;  ///< 期望 on_bar_update 次数
    int64_t closes;   ///< 期望 on_bar_close 次数
};

/**
 * @brief 该路径 timed 段的期望事件形态
 *
 * - update：同桶 tick 只 update，零 open/close（首笔建立 bar 在 setup 里）。
 * - new-bar：每笔跨桶 → 先 close 旧根 + open 新根 + update，各 N 次。
 * - cross-day：每笔换日 → 先 close 旧日 + open 新日 + update，各 N 次。
 * - auction：Open 窗口内静默更新，**零回调**（ADR 0031 决策 5）。
 *
 * @param path 被测路径
 * @param timed_ticks timed 段 tick 数
 * @return 期望事件数
 */
ExpectedEvents expected_events(Path path, int64_t timed_ticks) {
    switch (path) {
        case Path::Update:
        case Path::Auction:
            return ExpectedEvents{0, path == Path::Update ? timed_ticks : 0, 0};
        case Path::NewBar:
        case Path::CrossDay:
            return ExpectedEvents{timed_ticks, timed_ticks, timed_ticks};
    }
    // switch 已覆盖全部 Path 取值；走到这里说明新增了路径却没给它定义事件形态。
    // 不用「全零」兜底 —— 那会让新路径静默通过身份自检（AGENTS §四 禁止静默回退）。
    std::fprintf(stderr, "unsupported path in expected_events | path=%d\n", static_cast<int>(path));
    std::abort();
}

/// @brief 把终态（当前 bar 与丢弃计数）折算进指纹（防止计时循环被优化掉，兼作可复现校验）
void mix_fingerprint(uint64_t& fingerprint, const IntradayBarBuilder<NullBarSink>& builder) {
    const auto mix = [&fingerprint](uint64_t value) {
        fingerprint = (fingerprint ^ value) * 1099511628211ULL;
    };
    if (builder.has_current_bar()) {
        const Bar& bar = builder.current_bar();
        uint64_t bits = 0;
        const double prices[4] = {bar.open, bar.high, bar.low, bar.close};
        for (const double price : prices) {
            std::memcpy(&bits, &price, sizeof(bits));
            mix(bits);
        }
        mix(static_cast<uint64_t>(bar.begin_sec));
        mix(static_cast<uint64_t>(bar.cum_volume));
    }
    const BarBuilderStats stats = builder.stats();
    mix(stats.out_of_session);
    mix(stats.out_of_order);
    mix(stats.bad_volume);
}

// ============================================================
// 三个 pass：契约自检 / 批量计时 / 逐笔计时
// ============================================================

/// @brief 一个计时 pass 的累计结果
struct PassTally {
    std::vector<uint64_t> cycles;  ///< 计时区间周期数（批量：每轮一个；逐笔：每 tick 一个）
    uint64_t fingerprint = 1469598103934665603ULL;  ///< 终态指纹（FNV 偏移基，防循环被消除）
    int64_t ticks = 0;                              ///< 累计被测 tick 数
    double wall_ns = 0.0;  ///< 各轮 timed 段墙钟之和（不含 setup/teardown，独立于 TSC 标定）
};

/**
 * @brief 批量计时 pass：每轮对整段 timed 读一对时钟，均值除以 tick 数
 *
 * 成对读时钟的约 8.8ns 分摊到整段 50–200 笔（0.04–0.18ns/tick，不到被测值的 1%），
 * 给出准确的 ns/tick 均值。
 *
 * @param the_case 被测 case
 * @param block 待重放的块
 * @param reps 轮数
 * @param tally 输出累计结果
 */
void run_batch_pass(const Case& the_case, const Block& block, int64_t reps, PassTally& tally) {
    tally.cycles.reserve(static_cast<std::size_t>(reps));
    double wall_ns = 0.0;
    for (int64_t rep = 0; rep < reps; ++rep) {
        NullBarSink sink;
        IntradayBarBuilder<NullBarSink> builder(the_case.table, the_case.period_sec, sink);
        for (const DzTick& tick : block.setup) {
            builder.on_tick(tick);
        }
        // 独立计时源：与 TSC 量同一段、但不依赖 TSC 频率标定。steady_clock 的读取必须落在
        // TSC 计时区间**之外**（否则会把约 17ns/次的读取开销算进被测区间）。代价是墙钟区间
        // 多含两次 rdtsc（约 9ns/次），分摊到 50–200 笔即 0.09–0.36 ns/tick，故 wall_mean
        // 是略偏高的上界，仅作交叉校验，精度以 TSC 的 batch_mean 为准。setup/teardown 不计
        // 入墙钟——分母只数 timed 笔，混入会让 wall_mean 系统性偏低。
        const auto wall_begin = std::chrono::steady_clock::now();
        const uint64_t timed_begin = tsc_now();
        for (const DzTick& tick : block.timed) {
            builder.on_tick(tick);
        }
        const uint64_t timed_end = tsc_now();
        const auto wall_end = std::chrono::steady_clock::now();
        tally.cycles.push_back(timed_end - timed_begin);
        wall_ns += std::chrono::duration<double, std::nano>(wall_end - wall_begin).count();
        tally.ticks += block.timed_count();
        for (const DzTick& tick : block.teardown) {
            builder.on_tick(tick);
        }
        mix_fingerprint(tally.fingerprint, builder);
    }
    tally.wall_ns = wall_ns;
}

/**
 * @brief 逐笔计时 pass：每笔被测 tick 单独读一对时钟，得到真实逐笔分布（含计时开销）
 *
 * @param the_case 被测 case
 * @param block 待重放的块
 * @param reps 轮数
 * @param warmup_reps 丢弃的预热轮数
 * @param tally 输出累计结果
 */
void run_per_tick_pass(
    const Case& the_case, const Block& block, int64_t reps, int64_t warmup_reps, PassTally& tally) {
    tally.cycles.reserve(static_cast<std::size_t>(reps * block.timed_count()));
    for (int64_t rep = 0; rep < reps + warmup_reps; ++rep) {
        const bool warming = rep < warmup_reps;
        NullBarSink sink;
        IntradayBarBuilder<NullBarSink> builder(the_case.table, the_case.period_sec, sink);
        for (const DzTick& tick : block.setup) {
            builder.on_tick(tick);
        }
        for (std::size_t i = 0; i < block.timed.size(); ++i) {
            const uint64_t tick_begin = tsc_now();
            builder.on_tick(block.timed[i]);
            const uint64_t tick_end = tsc_now();
            if (!warming) {
                tally.cycles.push_back(tick_end - tick_begin);
                ++tally.ticks;
            }
        }
        for (const DzTick& tick : block.teardown) {
            builder.on_tick(tick);
        }
        if (!warming) {
            mix_fingerprint(tally.fingerprint, builder);
        }
    }
}

// ============================================================
// 报告
// ============================================================

/// @brief 一个 case 的最终数值
struct CaseResult {
    const char* name = "";      ///< 路径名
    int64_t reps = 0;           ///< 批量轮数
    int64_t ticks_per_rep = 0;  ///< 每轮被测 tick 数
    double batch_mean = 0.0;    ///< 批量计时 ns/tick 均值（无逐笔读时钟开销）
    double wall_mean = 0.0;     ///< 整段墙钟 / 总 tick 数（独立于 TSC 标定）
    double raw_p50 = 0.0;       ///< 逐笔 p50（含成对读时钟开销）
    double raw_p99 = 0.0;       ///< 逐笔 p99（含成对读时钟开销）
    double net_p50 = 0.0;       ///< raw p50 - 空区间开销 p50
    double net_p99 = 0.0;       ///< raw p99 - 空区间开销 p99
    int64_t samples = 0;        ///< 逐笔样本数
    uint64_t fingerprint = 0;   ///< 终态指纹（防优化 + 可复现校验）
};

/// @brief 报告头：机器信息、计时标定与工作集（结果可复现的前提）
void print_header(double tsc_ghz,
                  double ns_per_cycle,
                  const Statistics& overhead,
                  const std::vector<CaseResult>& results,
                  const std::vector<Block>& blocks) {
    std::printf("dztrader tick-to-bar benchmark (issue #93, ADR 0031 decision 10)\n");
    std::printf("build: %s | compiler: %s | platform: %s\n", build_type_name().c_str(),
                compiler_name().c_str(), platform_name().c_str());
    std::printf("cpu: %s | logical_cpus=%u\n", cpu_brand().c_str(),
                std::thread::hardware_concurrency());
    std::printf("tsc: %.3f GHz (%.4f ns/cycle) | calibrated over %d ms\n", tsc_ghz, ns_per_cycle,
                TSC_CALIBRATION_MS);
    std::printf("timer overhead | empty_region samples=%lld p50=%.2fns p99=%.2fns max=%.2fns\n",
                static_cast<long long>(overhead.count), overhead.p50, overhead.p99, overhead.max);
    std::printf("stream: sizeof(DzTick)=%zuB | timed ticks per rep: ", sizeof(DzTick));
    for (std::size_t i = 0; i < results.size(); ++i) {
        std::printf("%s=%lld", results[i].name, static_cast<long long>(blocks[i].timed_count()));
        if (i + 1 != results.size()) {
            std::printf(" ");
        }
    }
    std::printf("\n");
#ifndef NDEBUG
    std::printf("warning: debug build, numbers are not representative of production\n");
#endif
    std::printf("\n");
}

/// @brief 两条结果表 + 目标对照与记录位置提示
void print_results(const std::vector<CaseResult>& results) {
    std::printf("amortized ns/tick | one clock read pair per whole timed segment\n");
    std::printf("%-12s %8s %12s %13s %12s\n", "path", "reps", "ticks/rep", "batch_mean",
                "wall_mean");
    for (const CaseResult& result : results) {
        std::printf(
            "%-12s %8lld %12lld %13.2f %12.2f\n", result.name, static_cast<long long>(result.reps),
            static_cast<long long>(result.ticks_per_rep), result.batch_mean, result.wall_mean);
    }

    std::printf("\nper-tick ns | every timed tick gets its own clock read pair\n");
    std::printf("%-12s %10s %9s %9s %9s %9s\n", "path", "samples", "raw_p50", "raw_p99", "net_p50",
                "net_p99");
    for (const CaseResult& result : results) {
        std::printf("%-12s %10lld %9.2f %9.2f %9.2f %9.2f\n", result.name,
                    static_cast<long long>(result.samples), result.raw_p50, result.raw_p99,
                    result.net_p50, result.net_p99);
    }

    std::printf("\nraw = clock read pair included; net = raw minus empty-region overhead\n");
    std::printf("cross-check: batch_mean / wall_mean / net_p50 should be the same order;\n");
    std::printf("             an order-of-magnitude divergence means suspect the benchmark,\n");
    std::printf("             not the component (see docs/agents/build-appendix.md)\n");
    // ADR 0031 决策 10 的 50ns 目标只覆盖「单周期更新」路径；其余三条路径按同口径报告、
    // 供后续优化与 CI 门禁取基线，不套用该阈值下结论。
    std::printf(
        "target: p99 <= 50 ns/tick applies to the update path only (ADR 0031 decision 10)\n");
    for (const CaseResult& result : results) {
        const bool is_target_path = std::strcmp(result.name, "update") == 0;
        if (is_target_path) {
            std::printf("target check | path=%s net_p99=%.2fns target=50ns %s\n", result.name,
                        result.net_p99, result.net_p99 <= 50.0 ? "met" : "missed");
        } else {
            std::printf("reported | path=%s net_p99=%.2fns (no target for this path)\n",
                        result.name, result.net_p99);
        }
    }
    std::printf("\nfingerprint (reproducibility + anti-dead-code):\n");
    for (const CaseResult& result : results) {
        std::printf("  %-12s %016llx\n", result.name,
                    static_cast<unsigned long long>(result.fingerprint));
    }
    std::printf("\nrecord: docs/agents/build-appendix.md, section \"performance benchmarks\"\n");
    std::printf("        (append machine + date + numbers to the baseline record)\n");
}

/**
 * @brief 跑一个 case 的全部 pass 并返回数值
 *
 * 顺序：契约自检（失败即中止）→ 批量计时 → 逐笔计时。
 *
 * @param the_case 被测 case
 * @param ns_per_cycle 每周期纳秒数
 * @param overhead 空区间成对读时钟开销（用于 net 列）
 * @param result 输出数值
 * @param block 输出块（报告头打印每轮 tick 数）
 * @return true = 自检与计时均通过
 */
bool run_case(const Case& the_case,
              double ns_per_cycle,
              const Statistics& overhead,
              CaseResult& result,
              Block& block) {
    result.name = the_case.name;
    block = build_block(the_case);
    result.ticks_per_rep = block.timed_count();
    if (result.ticks_per_rep <= 0) {
        std::fprintf(stderr, "empty timed segment | case=%s\n", the_case.name);
        return false;
    }
    result.reps = (TARGET_SAMPLES + result.ticks_per_rep - 1) / result.ticks_per_rep;

    {
        NullBarSink sink;
        IntradayBarBuilder<NullBarSink> builder(the_case.table, the_case.period_sec, sink);
        int64_t mismatches = check_segment(builder, block.setup, block.setup_expect, "setup");
        mismatches += check_segment(builder, block.timed, block.timed_expect, "timed");
        mismatches += check_segment(builder, block.teardown, block.teardown_expect, "teardown");
        if (mismatches != 0) {
            std::fprintf(stderr, "schedule self-check failed | case=%s mismatches=%lld\n",
                         the_case.name, static_cast<long long>(mismatches));
            return false;
        }
        if (!stats_clean(builder.stats())) {
            print_stats(the_case.name, builder.stats());
            return false;
        }
        // timed 段必须连续属于同一条路径：前后相邻 tick 的 bar 状态变化形态要一致
        if (the_case.path == Path::Update) {
            for (std::size_t i = 1; i < block.timed_expect.size(); ++i) {
                if (bar_changes(block.timed_expect[i - 1], block.timed_expect[i])) {
                    std::fprintf(stderr, "update path not bucket-homogeneous | index=%zu\n", i);
                    return false;
                }
            }
        }
    }

    // 路径身份自检：用 CountingSink 重放 setup + timed，断言 sink 事件形态与该路径定义一致
    // （特别是竞价路径必须在窗口内零回调）。setup 只建立状态，事件数单独扣掉。
    {
        CountingSink setup_sink;
        IntradayBarBuilder<CountingSink> setup_builder(the_case.table, the_case.period_sec,
                                                       setup_sink);
        for (const DzTick& tick : block.setup) {
            setup_builder.on_tick(tick);
        }
        const CountingSink setup_events = setup_sink;
        for (const DzTick& tick : block.timed) {
            setup_builder.on_tick(tick);
        }
        const int64_t opens = setup_sink.opens - setup_events.opens;
        const int64_t updates = setup_sink.updates - setup_events.updates;
        const int64_t closes = setup_sink.closes - setup_events.closes;
        const ExpectedEvents want = expected_events(the_case.path, result.ticks_per_rep);
        if (opens != want.opens || updates != want.updates || closes != want.closes) {
            std::fprintf(stderr,
                         "path identity mismatch | case=%s opens=%lld/%lld updates=%lld/%lld "
                         "closes=%lld/%lld\n",
                         the_case.name, static_cast<long long>(opens),
                         static_cast<long long>(want.opens), static_cast<long long>(updates),
                         static_cast<long long>(want.updates), static_cast<long long>(closes),
                         static_cast<long long>(want.closes));
            return false;
        }
    }

    {
        PassTally tally;
        run_batch_pass(the_case, block, result.reps, tally);
        const Statistics segments = summarize(std::move(tally.cycles), ns_per_cycle);
        result.batch_mean = segments.mean / static_cast<double>(result.ticks_per_rep);
        result.wall_mean = tally.wall_ns / static_cast<double>(tally.ticks);
        result.fingerprint = tally.fingerprint;
    }

    {
        PassTally tally;
        run_per_tick_pass(the_case, block, result.reps, WARMUP_REPS, tally);
        const Statistics per_tick = summarize(std::move(tally.cycles), ns_per_cycle);
        result.samples = per_tick.count;
        result.raw_p50 = per_tick.p50;
        result.raw_p99 = per_tick.p99;
        result.net_p50 = per_tick.p50 - overhead.p50;
        result.net_p99 = per_tick.p99 - overhead.p99;
        // 两个 pass 的终态相同：用乘法混合保留两个来源，而不是 XOR（相同值会互相抵消为 0）
        result.fingerprint = result.fingerprint * 1099511628211ULL + tally.fingerprint;
    }
    return true;
}

}  // namespace

int main() {
    const double tsc_ghz = measure_tsc_ghz();
    if (tsc_ghz <= 0.0) {
        std::fprintf(stderr, "tsc calibration failed | tsc_ghz=%.6f\n", tsc_ghz);
        return 1;
    }
    const double ns_per_cycle = 1.0 / tsc_ghz;
    const Statistics overhead = measure_timer_overhead(ns_per_cycle);

    const std::array<Case, 4> cases = make_cases();
    std::vector<CaseResult> results;
    std::vector<Block> blocks;
    results.reserve(cases.size());
    blocks.reserve(cases.size());
    for (const Case& the_case : cases) {
        CaseResult result;
        Block block;
        if (!run_case(the_case, ns_per_cycle, overhead, result, block)) {
            return 1;
        }
        results.push_back(result);
        blocks.push_back(std::move(block));
    }
    print_header(tsc_ghz, ns_per_cycle, overhead, results, blocks);
    print_results(results);
    return 0;
}
