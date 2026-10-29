/**
 * @file bench_framework_harness.h
 * @brief BarStrategy 框架基准的共享夹具：假 SDK 边界 + 假依赖 + 配置 home（issue #164）
 *
 * ## 为什么不走真实引擎/通道
 *
 * `bench_tick_update` / `bench_bar_close_dual_ma` / `bench_warmup_replay` 量的是**框架自身**
 * 的计算路径（组件文档 §10 的 1µs 预算不含 SDK 帧解析、shm 写与网络）。若用 Seam A（真实
 * `dz_init` + 帧通道）驱动，每笔 tick 都要经过通道读写，被测值会被 shm 往返淹没；回放基准也
 * 会被真实归档库阻塞。因此本夹具在 **SDK/C API 边界**打桩：
 *
 *   - 定义基准进程内需要的 `dz_*` 入口（`dz_place_order` / `dz_set_logical_position` /
 *     `dz_subscribe` / `dz_schedule_daily` / `dz_next_md` / 归档库查询等）为可观测桩：只记
 *     计数并返回成功，不写任何帧、不碰 shm；
 *   - 用**非空假会话指针**驱动 `BarStrategyManager`：框架的 `dz_ == nullptr` 分支会跳过
 *     fanout（`fanout_positions` 的虚拟台账早退），而本组的 bar close 基准要量的正是「逻辑
 *     持仓差量 → 每账户手数 → 价格对齐 → 下单/上报」这段 fanout 计算。桩保证指针永不解引用；
 *   - 历史 bar 与合约元数据经 `FrameworkDependencies` 注入（与 Seam B 测试同一条内部缝）：
 *     回放、静默填窗、元数据全走生产代码，只是数据来自确定性生成器。
 *
 * 由此启动序列（`on_start` → 配置加载 → 元数据 → 实例装配 → 回放 → 进入 live）与生产完全
 * 一致，被替换的只有「帧写到哪」。
 *
 * ## 桩的边界与代价
 *
 * 桩函数本身是一次普通调用 + 一个计数器自增（几 ns），**不含**真实帧序列化与 shm 写。
 * 这正是组件文档 §10 的口径（「不含共享内存写」）；「决策 → 订单帧落 shm」的端到端成本
 * 不在本组基准内（需要真实通道，属另一次测量）。
 *
 * 桩是**编译期替代**（本可执行文件不链接 `dztrading` 的 api.cpp）：只要框架头文件新增一个
 * 被实例化的 SDK 调用而本文件没给桩，**链接会失败**——不会静默少测一段路径。
 *
 * ## 配置 home
 *
 * 框架从 `$dz_strategy_home()/<strategy_id>.json` 读配置，故桩把 home 指到
 * `$DZTRADER_HOME/bench/<strategy_id>`（AGENTS §一.2：测试/调试期 `DZTRADER_HOME` 必须
 * 指向项目内目录）。未设置 `DZTRADER_HOME` 时**直接报错退出**，不退化到系统临时目录。
 *
 * ## B 的观测值为什么走进程级全局
 *
 * B 由框架默认构造、基准拿不到实例指针；计数器与回调内计时放进程级 {@link g_observer}
 * （与 `bar_strategy_framework_test.cpp` 的记录器同一条理由）。基准恒单品种单实例，全局
 * 观测不会混淆归属。
 */
#ifndef DZTRADER_STRATEGY_API_TESTS_BENCH_FRAMEWORK_HARNESS_H_
#define DZTRADER_STRATEGY_API_TESTS_BENCH_FRAMEWORK_HARNESS_H_

#include <dztrader/bar_strategy.h>
#include <dztrader/bar_strategy/dependencies.h>
#include <dztrader/data_type.h>
#include <dztrader/struct.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "bench_support.h"

namespace dztrader::strategy_api::bench_framework {

using bar_strategy_internal::FrameworkDependencies;
using bar_strategy_internal::HistoryQuery;
using bar_strategy_internal::HistoryQueryResult;
using bar_strategy_internal::HistoryStatus;
using bar_strategy_internal::MetadataResult;
using bar_strategy_internal::MetadataStatus;

/// @brief 基准统一使用的合约代码（单品种口径，组件文档 §10）
inline constexpr std::string_view kInstrumentId = "rb2601";
/// @brief 基准统一使用的挂靠账户（单挂靠账户口径）
inline constexpr std::string_view kAccountId = "sim";
/// @brief 假回卷定时器 ID（`dz_schedule_daily` 桩返回它，基准据此驱动 `on_schedule`）
inline constexpr DzTimerId kRollbackTimerId = 42;
/// @brief 基准历史与 live 流共用的首交易日（任意正值；builder 只要求 > 0 且不倒退）
inline constexpr DzDate kFirstTradingDay = 20000;
/// @brief 合约乘数（Tier 1 盈亏换算；与真实 rb 相同量级）
inline constexpr double kVolumeMultiple = 10.0;
/// @brief 最小变动价位（下单价格对齐；非 0 才会走对齐分支）
inline constexpr double kPriceTick = 0.01;

// ============================================================
// 假 SDK 边界
// ============================================================

/// @brief 假会话句柄的存储（所有 SDK 桩都不解引用它，只为让 `dz_ != nullptr`）
inline int g_fake_session_storage = 0;

/**
 * @brief 假会话指针（非空 → 框架走实盘 fanout 分支；桩保证不解引用）
 * @return 稳定的非空句柄
 */
[[nodiscard]] inline DzContext* fake_session() noexcept {
    return reinterpret_cast<DzContext*>(&g_fake_session_storage);
}

/// @brief SDK 桩的调用计数（兼作路径身份自检：订阅/回卷/订单/上报必须真的发生过）
struct SdkCounters {
    std::int64_t subscribe_calls = 0;     ///< `dz_subscribe` 次数
    std::int64_t schedule_calls = 0;      ///< `dz_schedule_daily` 次数
    std::int64_t orders = 0;              ///< `dz_place_order` 次数
    std::int64_t position_publishes = 0;  ///< `dz_set_logical_position` 次数
};

/// @brief 进程级 SDK 桩计数
inline SdkCounters g_sdk;

/// @brief 清零 SDK 桩计数（多次 rig 的场景每个 rig 启动前调用）
inline void reset_sdk_counters() noexcept { g_sdk = SdkCounters{}; }

/// @brief 假 home 路径（由 {@link BenchHome} 设置；`dz_strategy_home` 桩返回它）
inline std::string g_strategy_home;
/// @brief 假策略 ID（由 {@link BenchHome} 设置；决定配置文件名）
inline std::string g_strategy_id;

}  // namespace dztrader::strategy_api::bench_framework

// 桩用 C 链接定义在全局命名空间：它们替代的正是 C ABI 入口（api.h 的声明）。
// inline 保证本头若被同一可执行文件包含多次也不会重复定义。
extern "C" {

inline const char* dz_strategy_home(DzContext*) {
    return dztrader::strategy_api::bench_framework::g_strategy_home.c_str();
}

inline const char* dz_strategy_id(DzContext*) {
    return dztrader::strategy_api::bench_framework::g_strategy_id.c_str();
}

inline bool dz_subscribe(DzContext*, const char* const[], uint32_t, bool) {
    ++dztrader::strategy_api::bench_framework::g_sdk.subscribe_calls;
    return true;
}

inline bool dz_unsubscribe(DzContext*, const char* const[], uint32_t) { return true; }

inline DzTimerId dz_schedule_daily(DzContext*, int32_t) {
    ++dztrader::strategy_api::bench_framework::g_sdk.schedule_calls;
    return dztrader::strategy_api::bench_framework::kRollbackTimerId;
}

inline bool dz_schedule_cancel(DzContext*, DzTimerId) { return true; }

inline const void* dz_next_md(DzContext*) {
    return nullptr;  // 无缓冲 tick：drain 循环立刻结束
}

inline DzOrderId dz_place_order(DzContext*,
                                const char*,
                                const char*,
                                DzDirection,
                                DzPriceType,
                                double,
                                DzVolume,
                                DzPositionEffect) {
    ++dztrader::strategy_api::bench_framework::g_sdk.orders;
    return static_cast<DzOrderId>(dztrader::strategy_api::bench_framework::g_sdk.orders);
}

inline bool dz_set_logical_position(DzContext*, const char*, const char*, DzLargeVolume) {
    ++dztrader::strategy_api::bench_framework::g_sdk.position_publishes;
    return true;
}

inline bool dz_output_ui(DzContext*, const char*) { return true; }

inline bool dz_notify_ui(DzContext*, DzNotifyLevel, const char*, bool) { return true; }

inline const char* dz_errmsg(void) { return ""; }

inline int32_t dz_errcode(void) { return 0; }

inline DzDatabase* dz_db_open_default(void) {
    return nullptr;  // 归档库不可用：历史/元数据由注入的假来源提供
}

inline bool dz_db_close(DzDatabase*) { return true; }

inline DzResultSet* dz_db_query_bar(
    DzDatabase*, const char*, int32_t, DzAdjustType, DzDate, DzDate) {
    return nullptr;
}

inline DzResultSet* dz_db_query_instruments(DzDatabase*, const char*, const char*) {
    return nullptr;
}

inline bool dz_resultset_next(DzResultSet*) { return false; }

inline DzColumnType dz_resultset_column_type(DzResultSet*, uint32_t) { return DZ_COL_TYPE_INT64; }

inline bool dz_resultset_is_null(DzResultSet*, uint32_t) { return true; }

inline int64_t dz_resultset_get_int64(DzResultSet*, uint32_t) { return 0; }

inline double dz_resultset_get_float64(DzResultSet*, uint32_t) { return 0.0; }

inline const char* dz_resultset_get_string(DzResultSet*, uint32_t) { return ""; }

inline bool dz_resultset_get_bool(DzResultSet*, uint32_t) { return false; }

inline void dz_resultset_close(DzResultSet*) {}

inline int32_t dz_resultset_status(DzResultSet*) { return 0; }

inline uint32_t dz_resultset_column_count(DzResultSet*) { return 0; }

inline const char* dz_resultset_column_name(DzResultSet*, uint32_t) { return ""; }

}  // extern "C"

namespace dztrader::strategy_api::bench_framework {

// ============================================================
// 配置 home
// ============================================================

/// @brief 一份基准配置（字段即组件文档 §7 的落盘字段；其余走默认/自动值）
struct BenchConfig {
    std::string strategy_id;               ///< 策略 ID = 配置文件名（各基准唯一）
    std::vector<std::string> instruments;  ///< 品种表（基准恒单品种）
    std::int32_t period_sec = 60;          ///< bar 周期（§10 基准口径 1 分钟）
    std::int32_t max_lookback = 500;       ///< 可读深度（§10 基准口径 500）
    std::int64_t warmup_bars = 500;        ///< 预热根数（静默填窗 + 回放）
};

/// @brief 把 {@link BenchConfig} 渲染成配置 JSON 文本（写盘用）
/// @param config 基准配置
/// @return JSON 文本
[[nodiscard]] inline std::string render_config(const BenchConfig& config) {
    std::string text = "{\n  \"schema_version\": 1,\n";
    text += "  \"period_sec\": " + std::to_string(config.period_sec) + ",\n";
    text += "  \"instruments\": [";
    for (std::size_t i = 0; i < config.instruments.size(); ++i) {
        if (i != 0) {
            text += ", ";
        }
        text += '"' + config.instruments[i] + '"';
    }
    text += "],\n";
    text += "  \"accounts\": [{\"account_id\": \"" + std::string(kAccountId) +
            "\", \"scale\": 1.0}],\n";
    text += "  \"max_lookback\": " + std::to_string(config.max_lookback) + ",\n";
    text += "  \"warmup\": {\"bars\": " + std::to_string(config.warmup_bars) + "},\n";
    text += "  \"params\": {}\n}\n";
    return text;
}

/**
 * @brief 基准的配置 home：创建目录、写配置、让 `dz_strategy_home`/`dz_strategy_id` 指向它
 *
 * 目录 = `$DZTRADER_HOME/bench/<strategy_id>`，进程启动前先删除（避免上一轮残留），析构时
 * 再删除。**未设置 `DZTRADER_HOME` 直接退出**：禁止让基准把配置落到真实 home（AGENTS §一.2）。
 */
class BenchHome {
public:
    /**
     * @brief 建 home 并写配置
     * @param config 基准配置（`strategy_id` 必须非空）
     */
    explicit BenchHome(const BenchConfig& config) {
        const char* home_env = std::getenv("DZTRADER_HOME");
        if (home_env == nullptr || home_env[0] == '\0') {
            std::fprintf(stderr,
                         "bench home unavailable | reason=strategy_home_env_unset effect=abort "
                         "hint=\"set DZTRADER_HOME to a directory inside the project\"\n");
            std::exit(1);
        }
        home_ = std::filesystem::path(home_env) / "bench" / config.strategy_id;
        std::filesystem::remove_all(home_);
        std::filesystem::create_directories(home_);
        g_strategy_home = home_.string() + "/";
        g_strategy_id = config.strategy_id;
        const std::filesystem::path config_path = home_ / (config.strategy_id + ".json");
        std::ofstream stream(config_path);
        stream << render_config(config);
        stream.close();
        if (!stream.good()) {
            std::fprintf(stderr, "bench config write failed | path=\"%s\"\n",
                         config_path.string().c_str());
            std::exit(1);
        }
    }

    ~BenchHome() {
        std::error_code ignored;
        std::filesystem::remove_all(home_, ignored);
    }

    BenchHome(const BenchHome&) = delete;
    BenchHome& operator=(const BenchHome&) = delete;
    BenchHome(BenchHome&&) = delete;
    BenchHome& operator=(BenchHome&&) = delete;

private:
    std::filesystem::path home_;  ///< `$DZTRADER_HOME/bench/<strategy_id>`
};

// ============================================================
// 假历史 bar 与假依赖
// ============================================================

/**
 * @brief 生成确定性归档历史（多交易日、连续桶、量额当日累计从 0 起算）
 *
 * 口径必须满足归档域数据契约（组件文档 §5.3）：`begin_sec` 是周期整数倍、`last_tick_sec`
 * 落在本桶、`cum_*` 是当日累计且 >= bar 内值。回放（`make_replay_ticks`）会据此还原 bar 并
 * 喂回真实 builder——历史不合法时回放产出的 bar 就不等于归档 bar，基准量到的就不是生产路径。
 *
 * 交易日跨度为 `86400 / period_sec` 根/日（1 分钟 = 1440 根/日）；价格确定性锯齿，量额按每根
 * 10 手线性累计。
 *
 * @note 不复用 `bar_strategy_framework_test.cpp` 的 `make_archive_history` / Seam-B 夹具：
 *         那些 helper 定义在含 `#include <gtest/gtest.h>` 的测试头里，而基准**不链 GTest**
 *         （组件文档 §10 的 1µs 预算不允许把测试框架拉进进程）。两处形状一致是刻意取舍，
 *         基准自检（回调计数公式 + 回放后窗口逐位校验）会在生成器漂移时立即失败。
 *
 * @param bar_count 根数
 * @param config 基准配置（用其周期）
 * @return 升序归档 bar
 */
[[nodiscard]] inline std::vector<bar::Bar> make_history(std::size_t bar_count,
                                                        const BenchConfig& config) {
    std::vector<bar::Bar> bars;
    bars.reserve(bar_count);
    const std::int32_t bar_sec = config.period_sec;
    const std::int32_t bars_per_day = 86400 / bar_sec;
    std::int64_t cum_volume = 0;
    double cum_turnover = 0.0;
    for (std::size_t i = 0; i < bar_count; ++i) {
        const auto index = static_cast<std::int32_t>(i % static_cast<std::size_t>(bars_per_day));
        if (index == 0) {
            cum_volume = 0;
            cum_turnover = 0.0;
        }
        bar::Bar one{};
        one.trading_day = kFirstTradingDay + static_cast<DzDate>(i / bars_per_day);
        one.begin_sec = index * bar_sec;
        one.last_tick_sec = one.begin_sec + bar_sec - 1;
        one.period_sec = bar_sec;
        // 确定性锯齿价格（200 根一个周期），高低价包住开收盘
        const double phase = static_cast<double>(i % 200U) * 0.5;
        one.open = 2000.0 + phase;
        one.close = one.open + ((i % 2U) == 0U ? 0.25 : -0.25);
        one.high = std::max(one.open, one.close) + 0.5;
        one.low = std::min(one.open, one.close) - 0.5;
        one.volume = 10;
        one.turnover = one.close * 10.0;
        one.open_interest = 1000 + static_cast<DzLargeVolume>(i);
        cum_volume += one.volume;
        cum_turnover += one.turnover;
        one.cum_volume = cum_volume;
        one.cum_turnover = cum_turnover;
        one.is_final = true;
        bars.push_back(one);
    }
    return bars;
}

/**
 * @brief 组装注入假历史/元数据的依赖（小节表用内置退化表，回卷时刻用默认 18:00）
 * @param history 归档历史（回放输入；nullptr = 历史源不可用）
 * @return 框架依赖
 */
[[nodiscard]] inline FrameworkDependencies make_dependencies(
    std::shared_ptr<const std::vector<bar::Bar>> history) {
    FrameworkDependencies dependencies;
    dependencies.history = [history](const HistoryQuery&) {
        HistoryQueryResult result;
        if (history == nullptr) {
            result.status = HistoryStatus::Unavailable;
            result.message = "bench fake history unavailable";
            return result;
        }
        result.status = HistoryStatus::Loaded;
        result.bars = *history;
        return result;
    };
    dependencies.metadata = [](std::string_view) {
        MetadataResult result;
        result.status = MetadataStatus::Loaded;
        result.spec.exchange_id = "SHFE";
        result.spec.volume_multiple = kVolumeMultiple;
        result.spec.price_tick = kPriceTick;
        return result;
    };
    return dependencies;
}

// ============================================================
// B 的进程级观测
// ============================================================

/// @brief B 的 bar 回调观测计数（路径身份自检用）
struct CallbackCounts {
    std::int64_t inits = 0;    ///< `on_init` 次数
    std::int64_t opens = 0;    ///< `on_bar_open` 次数
    std::int64_t updates = 0;  ///< `on_bar_update` 次数
    std::int64_t closes = 0;   ///< `on_bar_close` 次数
};

/// @brief B 的观测值（B 由框架默认构造，基准只能经进程级全局读它）
struct Observer {
    CallbackCounts counts;                    ///< 回调计数
    std::int64_t bar_index = -1;              ///< 最近一次 bar 回调观测到的 bar 序号
    std::int64_t close_bar_index = -1;        ///< 最近一次 `on_bar_close` 观测到的 bar 序号
    double last_close = 0.0;                  ///< 最近一次回调读到的收盘价（防死代码消除）
    bool record_close_cycles = false;         ///< 是否在 `on_bar_close` 内记录自身周期数
    std::vector<std::uint64_t> close_cycles;  ///< 每次 close 回调体的 RDTSC 周期数
};

/// @brief 进程级观测器
inline Observer g_observer;

/// @brief 清零观测器（多次 rig 的场景每个 rig 启动前调用；保留 `record_close_cycles` 由调用方重设）
inline void reset_observer() noexcept { g_observer = Observer{}; }

// ============================================================
// 基准策略
// ============================================================

/**
 * @brief 双均线基准策略（组件文档 §10 的基准 B）
 *
 * - `on_init` 建两条用户序列（`double`），让回卷/推进都包含用户序列；
 * - `on_bar_close` 用 `close[0..4]` / `close[0..19]` 算 5/20 均线（真实指标成本），再按
 *   **基准的最坏情况**每根 bar 反手一次：偶数 `bar_index` 目标 +1、奇数 -1。这样每根 bar
 *   的 close 都恰好产生一次逻辑持仓变化 → 每根 close 都走满「差量 → 每账户手数 → 下单 +
 *   上报」fanout；若按均线交叉下单，合成行情下成交意图会稀疏到量不出 fanout。
 * - `g_observer.record_close_cycles` 为 true 时在回调体内用 RDTSC 圈出「指标 + 意图 +
 *   fanout」的自身开销（`bench_bar_close_dual_ma` 用它把「框架分发」与「B 回调体」分开
 *   报告；回放基准不开启）。
 */
struct DualMaStrategy {
    Series<double> fast;  ///< 5 根均线用户序列（`fast[0]` = 本根，`fast[1]` = 上一根）
    Series<double> slow;  ///< 20 根均线用户序列

    /// @brief 建两条用户序列（静默填窗期框架会逐根写入初值）
    void on_init(InstrumentContext& ctx) {
        fast = ctx.make_series<double>(0.0);
        slow = ctx.make_series<double>(0.0);
        ++g_observer.counts.inits;
    }

    /// @brief bar 收盘：算 5/20 均线，并按最坏情况反手（每根一次意图）
    void on_bar_close(InstrumentContext& ctx) {
        ++g_observer.counts.closes;
        const std::uint64_t begin = g_observer.record_close_cycles ? bench_support::tsc_now() : 0;
        double fast_sum = 0.0;
        for (std::int32_t n = 0; n < 5; ++n) {
            fast_sum += ctx.close[n];
        }
        double slow_sum = 0.0;
        for (std::int32_t n = 0; n < 20; ++n) {
            slow_sum += ctx.close[n];
        }
        fast[0] = fast_sum / 5.0;
        slow[0] = slow_sum / 20.0;
        const double price = ctx.close[0];
        g_observer.last_close = price;
        if (ctx.bar_index() % 2 == 0) {
            ctx.buy(1, price);
        } else {
            ctx.sell_short(1, price);
        }
        if (g_observer.record_close_cycles) {
            g_observer.close_cycles.push_back(bench_support::tsc_now() - begin);
        }
    }

    /// @brief 形成中 bar 更新：记收盘价（分类与防消除用）
    void on_bar_update(InstrumentContext& ctx) {
        ++g_observer.counts.updates;
        g_observer.bar_index = ctx.bar_index();
        g_observer.last_close = ctx.close[0];
    }

    /// @brief 新 bar 打开：只计数（路径身份自检用）
    void on_bar_open(InstrumentContext& ctx) {
        ++g_observer.counts.opens;
        g_observer.bar_index = ctx.bar_index();
        (void)ctx;
    }
};

// ============================================================
// Manager rig
// ============================================================

/**
 * @brief 把「配置 home + 假历史 + 依赖 + 管理器」装配成一个可启动的 rig
 *
 * 成员声明顺序即构造/析构顺序：home_ → history_ → dependencies_ → manager_；析构反向，
 * manager_ 先销毁（`~ManagerRig` 里显式 `on_stop()`），home_ 最后删除目录。
 *
 * @tparam Strategy 基准策略类型（六回调任意子集）
 */
template <class Strategy>
class ManagerRig {
public:
    /**
     * @brief 装配 rig（不启动；启动是 {@link start}，便于基准把启动时间单独计时）
     * @param config 基准配置
     * @param history 归档历史（回放输入）
     */
    ManagerRig(const BenchConfig& config, std::vector<bar::Bar> history)
        : ManagerRig(config, std::make_shared<const std::vector<bar::Bar>>(std::move(history))) {}

    /**
     * @brief 装配 rig（共享历史；多轮启动的基准复用同一份历史，避免每轮再拷一次）
     * @param config 基准配置
     * @param history 归档历史（nullptr = 历史源不可用 → 冷启动降级）
     */
    ManagerRig(const BenchConfig& config, std::shared_ptr<const std::vector<bar::Bar>> history)
        : home_(config),
          history_(std::move(history)),
          dependencies_(make_dependencies(history_)),
          manager_(dependencies_) {}

    ~ManagerRig() { manager_.on_stop(); }

    ManagerRig(const ManagerRig&) = delete;
    ManagerRig& operator=(const ManagerRig&) = delete;
    ManagerRig(ManagerRig&&) = delete;
    ManagerRig& operator=(ManagerRig&&) = delete;

    /**
     * @brief 启动框架（`on_start`：配置 → 元数据 → 装配 → 回放 → live）
     * @return true = 启动成功（非自主退出）；失败时打印诊断并返回 false
     */
    [[nodiscard]] bool start() {
        manager_.on_start(fake_session());
        if (manager_.should_exit()) {
            std::fprintf(stderr, "bench startup failed | exit_code=%d\n", manager_.exit_code());
            return false;
        }
        return true;
    }

    /// @brief 框架管理器（喂 tick / 驱动定时器）
    [[nodiscard]] BarStrategyManager<Strategy>& manager() noexcept { return manager_; }

private:
    BenchHome home_;                                        ///< 配置 home（析构时清理）
    std::shared_ptr<const std::vector<bar::Bar>> history_;  ///< 归档历史（回放输入）
    FrameworkDependencies dependencies_;                    ///< 注入的假依赖
    BarStrategyManager<Strategy> manager_;                  ///< 框架管理器
};

// ============================================================
// 行情流与自检
// ============================================================

/// @brief 构造一笔基准 tick（合约固定 `kInstrumentId`）
/// @param day 交易日
/// @param clock_sec 墙钟秒（0..86399）
/// @param price 最新价
/// @param cum_volume 当日累计成交量
/// @param cum_turnover 当日累计成交额
/// @return 行情快照
[[nodiscard]] inline DzTick make_tick(
    DzDate day, DzTime clock_sec, double price, std::int64_t cum_volume, double cum_turnover) {
    DzTick tick{};
    std::memcpy(tick.instrument_id, kInstrumentId.data(), kInstrumentId.size());
    tick.instrument_id[kInstrumentId.size()] = '\0';
    tick.date = day;
    tick.time = clock_sec;
    tick.last_price = price;
    tick.volume = static_cast<DzVolume>(cum_volume);
    tick.turnover = cum_turnover;
    tick.open_interest = 1000;
    return tick;
}

/**
 * @brief 确定性的 live 行情流（按**交易日内秒**推进，跨交易日自动换日）
 *
 * 交易日内秒以 18:00 为锚点（`bar::TradingTime::from_clock`：墙钟 18:00 = tsec 0），因此
 * **交易日切换发生在 tsec 从 86399 回绕到 0 时**，而 `tick.time` 必须写回墙钟秒
 * （`TradingTime::to_clock`）。基准早期版本直接按墙钟秒推进、在午夜换日，结果 tsec 在墙钟
 * 18:00 处从 86399 回绕到 0 而交易日没变 → builder 按乱序丢弃整个夜盘（每交易日 21600 笔），
 * 自检立刻抓到。这里按 tsec 推进、回绕时换日，并让当日累计量额在换日时归零。
 *
 * `first_cum_volume` / `first_cum_turnover` 用于**跨历史续接**：live 第一笔 tick 的当日累计
 * 必须从历史末根的累计值继续（同一交易日内累计量额不得倒退），否则整个当日 tick 会被
 * builder 的量额守卫丢弃。
 */
class TickStream {
public:
    /// @brief 从指定交易日/交易日内秒开始
    /// @param first_day 起始交易日
    /// @param first_tsec 起始交易日内秒（下一笔 tick 前会先加步长）
    /// @param first_cum_volume 起始当日累计成交量（同交易日内续接历史末根）
    /// @param first_cum_turnover 起始当日累计成交额
    TickStream(DzDate first_day,
               std::int32_t first_tsec,
               std::int64_t first_cum_volume = 0,
               double first_cum_turnover = 0.0)
        : day_(first_day),
          tsec_(first_tsec),
          cum_volume_(first_cum_volume),
          cum_turnover_(first_cum_turnover) {}

    /**
     * @brief 生成下一笔 tick（每次前进 `step_sec` 个交易日内秒）
     * @param step_sec 与本笔 tick 的间隔（1 = 下一秒；60 = 下一根 1 分钟 bar）
     * @param price_phase 价格相位（调用方决定价格轨迹；映射到 2000..2050 附近的锯齿）
     * @return 行情快照
     */
    [[nodiscard]] DzTick next(std::int32_t step_sec, std::int64_t price_phase) {
        tsec_ += step_sec;
        while (tsec_ >= 86400) {
            tsec_ -= 86400;
            ++day_;
            cum_volume_ = 0;
            cum_turnover_ = 0.0;
        }
        ++cum_volume_;
        const double price = 2000.0 + static_cast<double>(price_phase % 100) * 0.5;
        cum_turnover_ += price;
        return make_tick(day_, bar::TradingTime::to_clock(tsec_), price, cum_volume_,
                         cum_turnover_);
    }

private:
    DzDate day_ = kFirstTradingDay;  ///< 当前交易日
    std::int32_t tsec_ = 0;          ///< 当前交易日内秒（18:00 锚点）
    std::int64_t cum_volume_ = 0;    ///< 当日累计成交量
    double cum_turnover_ = 0.0;      ///< 当日累计成交额
};

/**
 * @brief 校验 SDK 桩的接线（订阅/定时器/初始全量上报都必须真的发生过）
 * @param bench 基准名（诊断用）
 * @return true = 接线符合预期
 */
[[nodiscard]] inline bool check_wiring(const char* bench) {
    const SdkCounters& counters = g_sdk;
    const bool ok = counters.subscribe_calls == 1 && counters.schedule_calls == 1 &&
                    counters.position_publishes >= 1;
    if (!ok) {
        std::fprintf(stderr,
                     "self-check failed | bench=%s check=sdk_wiring subscribe=%lld schedule=%lld "
                     "publishes=%lld\n",
                     bench, static_cast<long long>(counters.subscribe_calls),
                     static_cast<long long>(counters.schedule_calls),
                     static_cast<long long>(counters.position_publishes));
        return false;
    }
    return true;
}

}  // namespace dztrader::strategy_api::bench_framework

#endif  // DZTRADER_STRATEGY_API_TESTS_BENCH_FRAMEWORK_HARNESS_H_
