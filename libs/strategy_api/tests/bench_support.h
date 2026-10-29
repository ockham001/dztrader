/**
 * @file bench_support.h
 * @brief 基准共享设施：RDTSC 计时、TSC 标定、机器信息、分布统计与报告头（issue #164）
 *
 * 只服务 `perf` 档的无阈值基准可执行文件，**不进 SDK 公开面、不链 GTest**。本文件是仓内
 * 唯一一份计时设施：`bar_perf_bench.cpp`（#93）与新基准（#164）共用同一套 RDTSC 口径，
 * 避免同一个「怎么量」在两个地方各写一遍后悄悄漂移。
 *
 * ## 为什么用 RDTSC 而不是 steady_clock
 *
 * 本机成对读 `steady_clock` 约 17ns、成对读 RDTSC 约 9ns；而 `bench_series_read` 的单项
 * 只有几 ns、`bench_tick_update` 的预算只有 1µs 量级，只有 RDTSC 的读时钟开销不至于把被测
 * 区间吃掉。TSC 频率启动时用 `steady_clock` 标定（TSC 与核心频率解耦时这是唯一正确的换算），
 * 所有周期数一律换算成 ns，跨机器/跨构建可比。
 *
 * ## 报告口径（与 docs/agents/build-appendix.md「性能基准」一致）
 *
 * - `batch_mean`：每轮对**整段**操作读一对时钟、均值再除以操作数 → 计时开销摊薄，是最准的
 *   水平值（基准的实现各自决定「一轮」包含多少操作）。
 * - 逐样本分布：每样本单独读一对时钟 → 得到真实 p50/p99，但必然叠加一次成对读时钟，故用
 *   {@link subtract_overhead} 扣减空区间开销后给出 net 分位数（偏保守的上界）。
 * - `raw = 含读时钟开销`、`net = 扣减后`；两者与 batch_mean 应同量级，数量级分歧先怀疑基准。
 *
 * 计时前必须做**路径身份自检**（各基准自己实现：事件数 / 指纹 / 几何检查），否则量到的可能
 * 不是被测路径，数值再稳定也没有意义。
 *
 * x86_64 + Windows/Linux 之外显式 `#error`（AGENTS §四 跨平台：禁止静默回退到精度不足的
 * 计时源）。
 */
#ifndef DZTRADER_STRATEGY_API_TESTS_BENCH_SUPPORT_H_
#define DZTRADER_STRATEGY_API_TESTS_BENCH_SUPPORT_H_

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

#if !defined(__x86_64__) && !defined(_M_X64)
#error "bench_support requires x86_64 (rdtsc)"
#endif

#if !defined(_WIN32) && !defined(__linux__)
#error "bench_support supports only windows x86_64 and linux x86_64"
#endif

#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(__GNUC__)
#include <cpuid.h>
#include <x86intrin.h>
#else
#error "bench_support supports MSVC or GCC/Clang only"
#endif

namespace dztrader::strategy_api::bench_support_internal {

/**
 * @brief 读取 cpuid 叶（MSVC 与 GCC 两套接口）
 * @param leaf cpuid 叶号
 * @param regs 输出：EAX/EBX/ECX/EDX
 */
inline void cpuid_leaf(unsigned leaf, unsigned (&regs)[4]) {
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

}  // namespace dztrader::strategy_api::bench_support_internal

namespace dztrader::strategy_api::bench_support {

/// @brief 空区间成对读时钟开销的标定样本数（逐样本计时的扣减基准）
inline constexpr std::int64_t OVERHEAD_SAMPLES = 100000;
/// @brief TSC 频率标定窗口（毫秒）
inline constexpr int TSC_CALIBRATION_MS = 50;

// ============================================================
// TSC 与机器信息
// ============================================================

/**
 * @brief 读取 TSC（前置 lfence 阻止前面的被测指令被重排到读时钟之后）
 *
 * lfence + rdtsc 不保证区间末尾的指令全部退役，但对 report-only、相对比较的基准足够：
 * 绝对误差远小于列间差异。
 *
 * @return TSC 周期数
 */
inline std::uint64_t tsc_now() noexcept {
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
inline double measure_tsc_ghz() {
    const auto wall_begin = std::chrono::steady_clock::now();
    const std::uint64_t tsc_begin = tsc_now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - wall_begin).count() <
           static_cast<double>(TSC_CALIBRATION_MS) / 1000.0) {
    }
    const std::uint64_t tsc_end = tsc_now();
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - wall_begin).count();
    if (seconds <= 0.0) {
        return 0.0;
    }
    return static_cast<double>(tsc_end - tsc_begin) / seconds / 1.0e9;
}

/// @brief CPU 品牌串（cpuid 扩展叶 0x80000002..0x80000004；不可用时返回 unknown）
inline std::string cpu_brand() {
    unsigned regs[4] = {};
    bench_support_internal::cpuid_leaf(0x80000000U, regs);
    if (regs[0] < 0x80000004U) {
        return "unknown";
    }
    std::array<char, 49> brand{};
    for (unsigned i = 0; i < 3; ++i) {
        bench_support_internal::cpuid_leaf(0x80000002U + i, regs);
        std::memcpy(brand.data() + static_cast<std::size_t>(i) * 16U, regs, 16U);
    }
    const std::string text(brand.data());
    const std::size_t first = text.find_first_not_of(' ');
    return first == std::string::npos ? std::string("unknown") : text.substr(first);
}

/// @brief 编译器标识（区分不同构建产物的数值）
inline std::string compiler_name() {
#if defined(_MSC_VER)
    return "msvc " + std::to_string(_MSC_VER);
#elif defined(__clang__)
    return "clang " + std::to_string(__clang_major__) + "." + std::to_string(__clang_minor__);
#else
    return "gcc " + std::to_string(__GNUC__) + "." + std::to_string(__GNUC_MINOR__);
#endif
}

/// @brief 平台标识（两个受支持平台在文件头已 gate，故无 else 分支）
inline std::string platform_name() {
#if defined(_WIN32)
    return "windows x86_64";
#else
    return "linux x86_64";
#endif
}

/// @brief 构建类型（Debug 数值无参考价值，报告头显式告警）
inline std::string build_type_name() {
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
    std::int64_t count = 0;  ///< 样本数
    double mean = 0.0;       ///< 均值
    double p50 = 0.0;        ///< 中位数
    double p99 = 0.0;        ///< 99 分位
    double max = 0.0;        ///< 最大值
};

/**
 * @brief 周期数分布 → ns 统计量（会原地排序，故按值接收）
 * @param cycles 样本（TSC 周期数）
 * @param ns_per_cycle 每周期纳秒数
 * @return 分布统计（`count == 0` 时其余字段为 0）
 */
inline Statistics summarize(std::vector<std::uint64_t> cycles, double ns_per_cycle) {
    Statistics stats;
    stats.count = static_cast<std::int64_t>(cycles.size());
    if (cycles.empty()) {
        return stats;
    }
    std::sort(cycles.begin(), cycles.end());
    const auto quantile = [&cycles, ns_per_cycle](double q) {
        const auto index = static_cast<std::size_t>(static_cast<double>(cycles.size()) * q);
        return static_cast<double>(cycles[index]) * ns_per_cycle;
    };
    double total = 0.0;
    for (const std::uint64_t cycle : cycles) {
        total += static_cast<double>(cycle);
    }
    stats.mean = total / static_cast<double>(cycles.size()) * ns_per_cycle;
    stats.p50 = quantile(0.50);
    stats.p99 = quantile(0.99);
    stats.max = static_cast<double>(cycles.back()) * ns_per_cycle;
    return stats;
}

/**
 * @brief 一组**直接以 ns 为单位**的样本 → 分布统计（墙钟计时用，不需要 TSC 换算）
 *
 * `summarize` 服务于 TSC 周期样本；用 `steady_clock` 量的整段耗时（例如启动回放）本身就是
 * ns，直接走本函数。会原地排序，故按值接收。
 *
 * @param values_ns 样本（ns）
 * @return 分布统计（`count == 0` 时其余字段为 0）
 */
inline Statistics summarize_values(std::vector<double> values_ns) {
    Statistics stats;
    stats.count = static_cast<std::int64_t>(values_ns.size());
    if (values_ns.empty()) {
        return stats;
    }
    std::sort(values_ns.begin(), values_ns.end());
    const auto quantile = [&values_ns](double q) {
        const auto index = static_cast<std::size_t>(static_cast<double>(values_ns.size()) * q);
        return values_ns[index];
    };
    double total = 0.0;
    for (const double value : values_ns) {
        total += value;
    }
    stats.mean = total / static_cast<double>(values_ns.size());
    stats.p50 = quantile(0.50);
    stats.p99 = quantile(0.99);
    stats.max = values_ns.back();
    return stats;
}

/**
 * @brief 空区间成对读时钟的开销分布（逐样本计时的扣减基准）
 * @param ns_per_cycle 每周期纳秒数
 * @return 开销分布
 */
inline Statistics measure_timer_overhead(double ns_per_cycle) {
    std::vector<std::uint64_t> cycles;
    cycles.reserve(static_cast<std::size_t>(OVERHEAD_SAMPLES));
    for (std::int64_t i = 0; i < OVERHEAD_SAMPLES; ++i) {
        const std::uint64_t begin = tsc_now();
        const std::uint64_t end = tsc_now();
        cycles.push_back(end - begin);
    }
    return summarize(std::move(cycles), ns_per_cycle);
}

/**
 * @brief 扣减成对读时钟开销后的分布（net 列；`mean/p50/p99/max` 逐项相减）
 *
 * 是**近似**而非精确补偿（逐样本计时扰动管线），故 net 分位数应视为偏保守的上界。相减后
 * 可能为负（被测操作低于计时噪声）——不钳到 0，如实暴露「这一项已经量不准」。
 *
 * @param raw 含计时开销的分布
 * @param overhead 空区间开销分布
 * @return 扣减后的分布（`count` 沿用 raw）
 */
inline Statistics subtract_overhead(const Statistics& raw, const Statistics& overhead) {
    Statistics net;
    net.count = raw.count;
    net.mean = raw.mean - overhead.mean;
    net.p50 = raw.p50 - overhead.p50;
    net.p99 = raw.p99 - overhead.p99;
    net.max = raw.max - overhead.max;
    return net;
}

// ============================================================
// 自检与指纹
// ============================================================

/**
 * @brief 自检失败的第一行诊断（各基准统一文案；失败明细由 `key=value` 片段给出）
 * @param bench 基准名
 * @param detail 失败明细（`k=v` 片段，可为空）
 */
inline void report_self_check_failure(const char* bench, const char* detail) {
    std::fprintf(stderr, "self-check failed | bench=%s %s\n", bench, detail);
}

/**
 * @brief 自检断言：失败时打印统一诊断并返回 false（基准进程据此 exit 1）
 * @param condition 断言条件
 * @param bench 基准名
 * @param detail 失败明细（`k=v` 片段）
 * @return `condition` 原样返回
 */
[[nodiscard]] inline bool expect(bool condition, const char* bench, const char* detail) {
    if (!condition) {
        report_self_check_failure(bench, detail);
    }
    return condition;
}

/**
 * @brief FNV-1a 64 位指纹混入器（基准终态指纹：防死代码消除 + 跨机器/跨 commit 可复现校验）
 *
 * 所有数一律按位混入：浮点用 `memcpy` 取位模式（不经浮点格式转换），整数直接混。相同的
 * 工作量得到相同指纹，值变化即说明「两次运行没跑同一份工作」。
 */
class Fingerprint {
public:
    /// @brief 混入一个 64 位值
    /// @param value 待混入值
    void mix(std::uint64_t value) noexcept { state_ = (state_ ^ value) * 1099511628211ULL; }

    /// @brief 按位混入一个 double（不经浮点格式化，-0.0 与 +0.0 因此可区分）
    /// @param value 待混入的浮点值
    void mix_double(double value) noexcept {
        std::uint64_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        mix(bits);
    }

    /// @brief 当前指纹值
    /// @return 64 位指纹
    [[nodiscard]] std::uint64_t value() const noexcept { return state_; }

private:
    std::uint64_t state_ = 1469598103934665603ULL;  ///< FNV-1a 偏移基
};

// ============================================================
// 报告
// ============================================================

/// @brief 一次基准运行的计时环境（TSC 标定 + 空区间开销）
struct Clock {
    double tsc_ghz = 0.0;       ///< TSC 频率（GHz）
    double ns_per_cycle = 0.0;  ///< 每周期纳秒数
    Statistics overhead;        ///< 空区间成对读时钟开销
};

/**
 * @brief 标定 TSC 并测空区间开销（失败即退出 1：没有可信时钟就不该输出数值）
 * @param bench_name 基准名（失败诊断用）
 * @return 计时环境
 */
[[nodiscard]] inline Clock make_clock(const char* bench_name) {
    Clock clock;
    clock.tsc_ghz = measure_tsc_ghz();
    if (clock.tsc_ghz <= 0.0) {
        std::fprintf(stderr, "tsc calibration failed | bench=%s tsc_ghz=%.6f\n", bench_name,
                     clock.tsc_ghz);
        std::exit(1);
    }
    clock.ns_per_cycle = 1.0 / clock.tsc_ghz;
    clock.overhead = measure_timer_overhead(clock.ns_per_cycle);
    return clock;
}

/**
 * @brief 报告头：被测对象 + 机器/构建/计时标定（结果可复现的前提）
 * @param title 基准名（含 issue 号）
 * @param scope 本基准量的是哪条路径（一句话）
 * @param clock 计时环境
 */
inline void print_bench_header(const char* title, const char* scope, const Clock& clock) {
    std::printf("%s\n", title);
    std::printf("scope: %s\n", scope);
    std::printf("build: %s | compiler: %s | platform: %s\n", build_type_name().c_str(),
                compiler_name().c_str(), platform_name().c_str());
    std::printf("cpu: %s | logical_cpus=%u\n", cpu_brand().c_str(),
                std::thread::hardware_concurrency());
    std::printf("tsc: %.3f GHz (%.4f ns/cycle) | calibrated over %d ms\n", clock.tsc_ghz,
                clock.ns_per_cycle, TSC_CALIBRATION_MS);
    std::printf("timer overhead | empty_region samples=%lld p50=%.2fns p99=%.2fns max=%.2fns\n",
                static_cast<long long>(clock.overhead.count), clock.overhead.p50,
                clock.overhead.p99, clock.overhead.max);
#ifndef NDEBUG
    std::printf("warning: debug build, numbers are not representative of production\n");
#endif
    std::printf("\n");
}

/**
 * @brief 是否快速自测档（`--quick`：各基准用小样本在秒级内跑完）
 *
 * 会话式地统一各基准的第一个参数：`--quick` 之外的参数一律忽略（基准没有其它可调参数，
 * 小样本只用于「改完基准是否还能跑通」的自测，不是正式对比口径）。
 *
 * @param argc 参数个数
 * @param argv 参数数组
 * @return true = 快速自测档
 */
[[nodiscard]] inline bool quick_mode(int argc, char** argv) {
    return argc > 1 && std::strcmp(argv[1], "--quick") == 0;
}

}  // namespace dztrader::strategy_api::bench_support

#endif  // DZTRADER_STRATEGY_API_TESTS_BENCH_SUPPORT_H_
