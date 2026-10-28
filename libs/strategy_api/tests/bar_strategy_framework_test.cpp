/**
 * @file bar_strategy_framework_test.cpp
 * @brief BarStrategy 框架端到端集成测试（Seam A：真实帧通道，#157 验收项）
 *
 * 只经外部可观察行为验证框架骨架：临时 `DZTRADER_HOME` + 真实事件/行情通道，测试线程内跑
 * `run_bar_strategy<FakeStrategy>()`，再用独立 writer 注入 TICK / SHUTDOWN 帧；断言落在
 * fake B 记录到的回调序列、价格值、`bar_index` 单调性与进程退出码上。
 *
 * fake B 经进程内全局记录器收集事件（B 由框架默认构造，测试拿不到实例指针），记录器用
 * 互斥量保护；等待循环读记录器并在主线程断言（gtest 断言宏不得在非主线程使用）。
 *
 * 配置文件路径由框架自己决定（`$dz_strategy_home()/<strategy_id>.json`，即本测试可执行文件
 * 同目录），测试按同一规则写入/清理——这正是真实部署的布局（配置与 exe 同目录）。ctest 把
 * 每个 TEST 注册成独立进程但它们共用同一配置路径，故本目标必须 RUN_SERIAL。
 */
#include <gtest/gtest.h>

#include <dztrader/api.h>
#include <dztrader/bar_strategy.h>
#include <dztrader/core/core_data_type.h>
#include <dztrader/core/core_struct.h>
#include <dztrader/core/env.h>
#include <dztrader/core/path.h>
#include <dztrader/core/this_process.h>
#include <dztrader/shm/channel_meta.h>
#include <dztrader/shm/frame_view.h>
#include <dztrader/shm/named_semaphore.h>
#include <dztrader/shm/reader.h>
#include <dztrader/shm/writer.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "test_temp_dir.h"

namespace {

using namespace std::chrono_literals;

constexpr uint64_t kMB = 1024 * 1024;

/// 本测试统一使用的两个合约代码（配置里的 `instruments`）
constexpr std::string_view kInstrumentA = "rb2601";
constexpr std::string_view kInstrumentB = "rb2605";

/// 交易日（任意正数；builder 只要求 > 0 且不倒退）
constexpr DzDate kTradingDay = 20000;

/// 用户序列的声明初值（#158：静默填窗期框架逐根写入，回调期首根回调时历史位应全是它）
constexpr double kUserSeriesInitD = 7.5;
constexpr std::int64_t kUserSeriesInitI = 11;

/// 门禁用例的显式可读深度（= `MAX_LOOKBACK_MIN`）：静态填窗只要 32 根 bar
constexpr std::int32_t kTestMaxLookback = dztrader::series_internal::MAX_LOOKBACK_MIN;

// ============================================================
// fake B 的事件记录器（进程内全局；B 由框架默认构造，拿不到测试注入的指针）
// ============================================================

/// @brief B 的五类观测事件（与六个回调一一对应，`on_params_update` 属 #162）
enum class EventKind : std::uint8_t { Init, BarOpen, BarUpdate, BarClose, Destroy };

/// @brief 回调名（断言失败时打印）
const char* event_kind_name(EventKind kind) noexcept {
    switch (kind) {
        case EventKind::Init:
            return "init";
        case EventKind::BarOpen:
            return "bar_open";
        case EventKind::BarUpdate:
            return "bar_update";
        case EventKind::BarClose:
            return "bar_close";
        case EventKind::Destroy:
            return "destroy";
    }
    return "unknown";
}

/// @brief 一次回调的观测快照（B 在回调体内读到的 ctx 状态）
struct ObservedEvent {
    EventKind kind = EventKind::Init;
    std::string instrument;             ///< `ctx.instrument_id()`
    const void* ctx_address = nullptr;  ///< `&ctx`（断言节点地址稳定）
    std::int64_t bar_index = -1;        ///< `ctx.bar_index()`
    std::int32_t window_size = -1;      ///< `ctx.close.size()`（已填充 bar 数）
    // 本品种在 on_init 里保存的视图副本：跨回调仍有效且跟随窗口（#157 AC2 视图有效性）
    bool saved_view_bound = false;      ///< 是否已保存过视图副本（on_init 之后为 true）
    std::int32_t saved_view_size = -1;  ///< 保存的视图副本此刻的 `size()`（= 当前窗口）
    double saved_view_close = 0;        ///< 保存的视图副本此刻的 `[0]`（= 当前 bar 收盘）
    double open = 0;                    ///< `ctx.open[0]`（无 bar 时为 0）
    double high = 0;                    ///< `ctx.high[0]`
    double low = 0;                     ///< `ctx.low[0]`
    double close = 0;                   ///< `ctx.close[0]`
    // #158：回调期「读 [n] 全部成功」与「回卷前后值不变」的可观测证据——每根回调里把
    // **全部 7 条价格序列**与两条用户序列的整个窗口抄下来（窗口只有 max_lookback 根）。
    // 抄满 7 条而不是只抄 close：回卷若漏搬 open/high/low/volume/turnover/open_interest
    // 中的任一条，只有对应数组的历史能暴露它——只比 close 会让 5 条数组失去覆盖。
    std::vector<double> history;                ///< `close[0] … close[size-1]`
    std::vector<double> open_history;           ///< `open[0] … open[size-1]`
    std::vector<double> high_history;           ///< `high[0] … high[size-1]`
    std::vector<double> low_history;            ///< `low[0] … low[size-1]`
    std::vector<double> volume_history;         ///< `volume[0] … volume[size-1]`
    std::vector<double> turnover_history;       ///< `turnover[0] … turnover[size-1]`
    std::vector<double> open_interest_history;  ///< `open_interest[0] … [size-1]`
    std::vector<double> user_d_history;         ///< 用户 double 序列 `[0] … [size-1]`
    std::vector<std::int64_t> user_i_history;   ///< 用户 int64 序列 `[0] … [size-1]`
    // #159：Tier 1 查询快照。每次回调抄一份，让「意图 → 台账」的接线可以在真实帧通道上
    // 被观测——包括「同一回调里 buy() 之后立刻读」与「close 时极值已含刚收 bar」。
    std::int32_t market_position = 0;             ///< `ctx.market_position()`
    dztrader::LargeVolume current_contracts = 0;  ///< `ctx.current_contracts()`
    double avg_entry_price = 0;                   ///< `ctx.avg_entry_price()`
    std::int32_t current_entries = 0;             ///< `ctx.current_entries()`
    dztrader::LargeVolume max_contracts = 0;      ///< `ctx.max_contracts()`
    std::int32_t max_entries = 0;                 ///< `ctx.max_entries()`
    double entry_price = 0;                       ///< `ctx.entry_price()`
    DzDate entry_date = DZ_DATE_NA;               ///< `ctx.entry_date()`
    DzTime entry_time = 0;                        ///< `ctx.entry_time()`
    double last_entry_price = 0;                  ///< `ctx.last_entry_price()`
    double exit_price = 0;                        ///< `ctx.exit_price()`
    std::int64_t bars_since_entry = -1;           ///< `ctx.bars_since_entry()`
    std::int64_t bars_since_last_entry = -1;      ///< `ctx.bars_since_last_entry()`
    std::int64_t bars_since_exit = -1;            ///< `ctx.bars_since_exit()`
    double position_profit = 0;                   ///< `ctx.position_profit()`
    double contract_profit = 0;                   ///< `ctx.contract_profit()`
    double max_position_profit = 0;               ///< `ctx.max_position_profit()`
    double max_position_loss = 0;                 ///< `ctx.max_position_loss()`
};

/**
 * @brief 一笔由测试脚本注入的交易意图（#159）
 *
 * `apply` 用 `std::function` 而不是 `{intent, volume, price}` 三元组：脚本要能在调用点
 * **前后**各读一次 Tier 1（即时记账的可观测证据），也能在同一根 bar 里连做多个意图。
 * 三元组表达不了这种「读 → 写 → 再读」的形状。
 */
struct ScriptedIntent {
    std::string instrument;                 ///< 空 = 任意品种（单品种脚本可省略）
    std::int64_t bar_index = -1;            ///< 在第几根 bar 上执行（`ctx.bar_index()`）
    EventKind when = EventKind::BarUpdate;  ///< 在哪个回调里执行
    std::function<void(dztrader::InstrumentContext&)> apply;  ///< 脚本动作
};

/// @brief 故障注入计划：让 fake B 在指定品种的指定回调里抛异常（异常隔离用例）
struct FaultPlan {
    std::string throw_in_init;       ///< 该品种的 `on_init` 抛异常
    std::string throw_in_bar_open;   ///< 该品种的 `on_bar_open` 抛异常
    std::string throw_in_update;     ///< 该品种的 `on_bar_update` 抛异常
    std::string throw_in_bar_close;  ///< 该品种的 `on_bar_close` 抛异常
    std::string throw_in_destroy;    ///< 该品种的 `on_destroy` 抛异常
    /// 该品种的 `on_bar_update` 里调 `ctx.make_series`（越界时机，应响亮失败；#158 AC5）
    std::string make_series_in_update;
};

/// @brief 进程内事件记录器 + 故障注入点（策略线程写、测试线程读）
class Recorder {
public:
    /// @brief 记录一次回调观测
    void push(ObservedEvent event) {
        const std::lock_guard<std::mutex> lock(mutex_);
        events_.push_back(std::move(event));
    }

    /// @brief 当前事件数（等待循环用）
    [[nodiscard]] std::size_t size() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return events_.size();
    }

    /// @brief 事件快照拷贝
    [[nodiscard]] std::vector<ObservedEvent> snapshot() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return events_;
    }

    /// @brief 清空事件与故障计划（每个用例开始前）
    void reset() {
        const std::lock_guard<std::mutex> lock(mutex_);
        events_.clear();
        fault_ = FaultPlan{};
        scripts_.clear();
        saved_views_.clear();
        user_series_.clear();
    }

    /// @brief 设置故障注入计划
    void set_fault(FaultPlan plan) {
        const std::lock_guard<std::mutex> lock(mutex_);
        fault_ = std::move(plan);
    }

    /// @brief 该品种是否应在 `on_bar_update` 里尝试 `make_series`（越界时机用例）
    [[nodiscard]] bool should_make_series(std::string_view instrument) const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return fault_.make_series_in_update == std::string(instrument);
    }

    /// @brief 该品种在该回调是否应抛异常
    [[nodiscard]] bool should_throw(std::string_view instrument, std::string_view callback) const {
        const std::lock_guard<std::mutex> lock(mutex_);
        const std::string id(instrument);
        if (callback == "init") {
            return fault_.throw_in_init == id;
        }
        if (callback == "bar_open") {
            return fault_.throw_in_bar_open == id;
        }
        if (callback == "bar_close") {
            return fault_.throw_in_bar_close == id;
        }
        if (callback == "destroy") {
            return fault_.throw_in_destroy == id;
        }
        return fault_.throw_in_update == id;
    }

    /**
     * @brief 注册一个交易意图脚本（#159；在指定 bar 的指定回调里执行）
     *
     * 脚本在 `record()` **之后**执行（`FakeStrategy` 的顺序），因此：
     *   - 回调内那一格 `ObservedEvent` 的 Tier 1 快照是「意图执行**前**」的状态；
     *   - 紧随其后的下一次回调快照是「意图执行**后**」的状态。
     * 这两格之差就是「意图即时生效」的可观测证据，测试按 `bar_index` + `kind` 取用。
     *
     * 脚本自己在 `apply` 里也可以读 ctx（例如同一回调内 `buy()` 之后立刻
     * `market_position()`），用于断言「同一个回调里立刻可见」这条 AC。
     */
    void add_script(ScriptedIntent script) {
        const std::lock_guard<std::mutex> lock(mutex_);
        scripts_.push_back(std::move(script));
    }

    /// @brief 取出并移除本品种本次回调应执行的脚本（无匹配时返回空）
    std::vector<ScriptedIntent> take_scripts(std::string_view instrument,
                                             EventKind when,
                                             std::int64_t bar_index) {
        const std::lock_guard<std::mutex> lock(mutex_);
        std::vector<ScriptedIntent> matched;
        auto it = scripts_.begin();
        while (it != scripts_.end()) {
            if (it->when == when && it->bar_index == bar_index &&
                (it->instrument.empty() || it->instrument == instrument)) {
                matched.push_back(std::move(*it));
                it = scripts_.erase(it);
                continue;
            }
            ++it;
        }
        return matched;
    }

    /// @brief 取某品种的事件子序列（保持发生顺序）
    [[nodiscard]] std::vector<ObservedEvent> for_instrument(std::string_view instrument) const {
        std::vector<ObservedEvent> filtered;
        for (const ObservedEvent& event : snapshot()) {
            if (event.instrument == instrument) {
                filtered.push_back(event);
            }
        }
        return filtered;
    }

    /// @brief 某品种某类事件的次数
    [[nodiscard]] std::size_t count(std::string_view instrument, EventKind kind) const {
        std::size_t total = 0;
        for (const ObservedEvent& event : for_instrument(instrument)) {
            if (event.kind == kind) {
                ++total;
            }
        }
        return total;
    }

    /// @brief 记录本品种在 `on_init` 保存的视图副本（B 用它验证视图跨回调有效）
    void save_view(std::string_view instrument, dztrader::SeriesView<double> view) {
        const std::lock_guard<std::mutex> lock(mutex_);
        saved_views_.insert_or_assign(std::string(instrument), view);
    }

    /// @brief 取本品种已保存的视图副本（未保存时返回空 optional）
    [[nodiscard]] std::optional<dztrader::SeriesView<double>> saved_view(
        std::string_view instrument) const {
        const std::lock_guard<std::mutex> lock(mutex_);
        const auto found = saved_views_.find(std::string(instrument));
        if (found == saved_views_.end()) {
            return std::nullopt;
        }
        return found->second;
    }

    /// @brief 清空保存的视图副本
    void clear_saved_views() {
        const std::lock_guard<std::mutex> lock(mutex_);
        saved_views_.clear();
    }

    /// @brief 记录本品种在 `on_init` 经 `make_series` 取得的两条用户序列（B 的成员）
    void save_user_series(std::string_view instrument,
                          dztrader::Series<double> user_d,
                          dztrader::Series<std::int64_t> user_i) {
        const std::lock_guard<std::mutex> lock(mutex_);
        user_series_.insert_or_assign(std::string(instrument), std::pair{user_d, user_i});
    }

    /// @brief 取本品种的用户序列对（未注册时返回空 optional）
    [[nodiscard]] std::optional<std::pair<dztrader::Series<double>, dztrader::Series<std::int64_t>>>
    user_series(std::string_view instrument) const {
        const std::lock_guard<std::mutex> lock(mutex_);
        const auto found = user_series_.find(std::string(instrument));
        if (found == user_series_.end()) {
            return std::nullopt;
        }
        return found->second;
    }

    /**
     * @brief 写本品种用户序列的 `[0]`（B 的回调里调用）
     *
     * 单独开一个方法而不是让调用方拿序列副本再写：`Series` 是可平凡拷贝的值类型，副本上的
     * `operator[]` 依然指向同一块数组，但 `optional<pair<...>>` 的返回值是 **const** 的
     * 临时副本，其 `operator[]` 退化成只读 `SeriesView` 版本——赋不进去。这里直接从映射里
     * 取可写引用。
     *
     * @param instrument 品种
     * @param value      写进 `[0]` 的值（三条序列同值，便于断言）
     * @return true = 该品种已注册用户序列
     */
    bool write_user_series_slot(std::string_view instrument, std::int64_t value) {
        const std::lock_guard<std::mutex> lock(mutex_);
        const auto found = user_series_.find(std::string(instrument));
        if (found == user_series_.end()) {
            return false;
        }
        found->second.first[0] = static_cast<double>(value);
        found->second.second[0] = value;
        return true;
    }

private:
    mutable std::mutex mutex_;
    std::vector<ObservedEvent> events_;
    FaultPlan fault_;
    std::vector<ScriptedIntent> scripts_;
    std::map<std::string, dztrader::SeriesView<double>> saved_views_;
    std::map<std::string, std::pair<dztrader::Series<double>, dztrader::Series<std::int64_t>>>
        user_series_;
};

/**
 * @brief 校验 `history`（`[0]` 最新 → `[size-1]` 最旧）是严格的「每往回一根小 1」
 *
 * 门禁用例按每根 bar +1 的价格喂 tick，因此正确的窗口必须恰好是 `p, p-1, p-2, …`。这条断言
 * 同时锁住两件事：`[n]` 的语义方向（往回数）与窗口内容的连续性（没有跳根、没有残留旧值）。
 */
void expect_strictly_decreasing_history(const ObservedEvent& event) {
    for (std::size_t n = 1; n < event.history.size(); ++n) {
        EXPECT_DOUBLE_EQ(event.history[n], event.history[n - 1] - 1.0)
            << "history must walk back one bar per index, n=" << n
            << " bar_index=" << event.bar_index;
    }
}

/**
 * @brief 一根 bar 的 int64 序列终值：静默填窗 = 声明初值，回调期 = `索引 * 1000`
 *
 * `FakeStrategy::write_user_series` 在 `open` 与 `update` 都把 `bar_index * 1000` 写进 `[0]`，
 * 因此任何进入回调期的 bar 在它第一次 `open` 之后终值就已确定；索引 `0 … max_lookback-1`
 * 那批 bar 没有回调，永远停在框架写的声明初值。
 *
 * 两条序列的声明初值不同（`kUserSeriesInitD` = 7.5 / `kUserSeriesInitI` = 11），所以值对
 * 初值的比较按类型分开——这正是「按类型分别注册」的可观测证据。
 */
[[nodiscard]] std::int64_t user_series_final_value_i(std::int64_t bar_index) {
    return bar_index < kTestMaxLookback ? kUserSeriesInitI : bar_index * 1000;
}

/// @brief 一根 bar 的 double 序列终值（与 {@link user_series_final_value_i} 同口径）
[[nodiscard]] double user_series_final_value_d(std::int64_t bar_index) {
    return bar_index < kTestMaxLookback ? kUserSeriesInitD : static_cast<double>(bar_index * 1000);
}

/**
 * @brief 校验用户序列窗口（与 `FakeStrategy::write_user_series` 的写入口径配套）
 *
 * 两种格子的期望值不同，正是 carry 语义的两个面：
 *
 * - **`n >= 1`（历史位）**：那根 bar 早已走完它的 open/update，值就是终值
 *   （`user_series_final_value_i` / `_d`）。若回卷漏搬用户序列，历史会整体错位一根，立刻不等。
 * - **`n == 0`（当前 bar）**：期望值**由事件类型唯一确定**（不是"carried 或 own 都算过"——
 *   那种析取等于没断言 carry）：`on_bar_close` 时窗口尚未推进，`[0]` 仍是被关闭的那根，
 *   期望本根终值；`on_bar_open` 时窗口刚推进、B 的写入在 `record()` 之后，期望**上一根**终值
 *   （这是 carry 唯一的正面证据）；`on_bar_update` 时 B 已写过，期望本根终值。
 *
 * 静默填窗那批格子的期望值 = 声明初值，这同时证明「静默期逐根写初值」确实发生过。
 */
void expect_user_series_window(const ObservedEvent& event) {
    for (std::size_t n = 0; n < event.user_i_history.size(); ++n) {
        const std::int64_t bar = event.bar_index - static_cast<std::int64_t>(n);
        if (n == 0) {
            // `[0]` 的期望值**由事件类型唯一决定**——不能只接受"carried 或 own 之一"，
            // 那样的析取等于没断言 carry：
            //   - close：窗口尚未推进，`[0]` 仍是被关闭的那根（其 bar_index 就是它自己），
            //     故期望 = 本根 own 值；
            //   - open：窗口刚推进、新槽位由框架 carry 出来，而 B 的写入在本函数之后执行，
            //     故期望 = **上一根**终值——这是 carry 唯一的正面证据；
            //   - update：B 在本根 open 或前一次 update 已写过，故期望 = 本根 own 值。
            const std::int64_t expected_i = event.kind == EventKind::BarOpen
                                                ? user_series_final_value_i(bar - 1)
                                                : user_series_final_value_i(bar);
            const double expected_d = event.kind == EventKind::BarOpen
                                          ? user_series_final_value_d(bar - 1)
                                          : user_series_final_value_d(bar);
            EXPECT_EQ(event.user_i_history[0], expected_i)
                << "user int64 series [0] mismatch | kind=" << event_kind_name(event.kind)
                << " bar_index=" << event.bar_index;
            EXPECT_DOUBLE_EQ(event.user_d_history[0], expected_d)
                << "user double series [0] mismatch | kind=" << event_kind_name(event.kind)
                << " bar_index=" << event.bar_index;
            continue;
        }
        EXPECT_EQ(event.user_i_history[n], user_series_final_value_i(bar))
            << "user int64 history mismatch | n=" << n << " bar_index=" << event.bar_index;
        EXPECT_DOUBLE_EQ(event.user_d_history[n], user_series_final_value_d(bar))
            << "user double history mismatch | n=" << n << " bar_index=" << event.bar_index;
    }
}

/// @brief 进程内唯一记录器（B 与测试共用）
Recorder& recorder() {
    static Recorder instance;
    return instance;
}

/// @brief 读当前 ctx 状态并记录（B 的六个回调共用）
void record(EventKind kind, dztrader::InstrumentContext& ctx) {
    ObservedEvent event;
    event.kind = kind;
    event.instrument = ctx.instrument_id();
    event.ctx_address = &ctx;
    event.bar_index = ctx.bar_index();
    event.window_size = ctx.close.size();
    // #159：Tier 1 快照（on_init 也抄——初始空仓是「无参考 + 盈亏 0」的基线）
    event.market_position = ctx.market_position();
    event.current_contracts = ctx.current_contracts();
    event.avg_entry_price = ctx.avg_entry_price();
    event.current_entries = ctx.current_entries();
    event.max_contracts = ctx.max_contracts();
    event.max_entries = ctx.max_entries();
    event.entry_price = ctx.entry_price();
    event.entry_date = ctx.entry_date();
    event.entry_time = ctx.entry_time();
    event.last_entry_price = ctx.last_entry_price();
    event.exit_price = ctx.exit_price();
    event.bars_since_entry = ctx.bars_since_entry();
    event.bars_since_last_entry = ctx.bars_since_last_entry();
    event.bars_since_exit = ctx.bars_since_exit();
    event.position_profit = ctx.position_profit();
    event.contract_profit = ctx.contract_profit();
    event.max_position_profit = ctx.max_position_profit();
    event.max_position_loss = ctx.max_position_loss();
    // on_init 里已保存的视图副本此刻是否仍与窗口一致（视图只绑一次、推进后自动看到新 bar）
    if (const auto saved = recorder().saved_view(event.instrument); saved.has_value()) {
        event.saved_view_bound = true;
        event.saved_view_size = saved->size();
        if (saved->size() > 0) {
            event.saved_view_close = (*saved)[0];
        }
    }
    if (ctx.bar_index() >= 0) {
        event.open = ctx.open[0];
        event.high = ctx.high[0];
        event.low = ctx.low[0];
        event.close = ctx.close[0];
        // 回调门禁的正面证据：回调期每个 [n]（n < size）都能读到（越界会抛异常打成失败实例）
        for (std::ptrdiff_t n = 0; n < ctx.close.size(); ++n) {
            event.history.push_back(ctx.close[n]);
            event.open_history.push_back(ctx.open[n]);
            event.high_history.push_back(ctx.high[n]);
            event.low_history.push_back(ctx.low[n]);
            event.volume_history.push_back(ctx.volume[n]);
            event.turnover_history.push_back(ctx.turnover[n]);
            event.open_interest_history.push_back(ctx.open_interest[n]);
        }
        if (const auto series = recorder().user_series(event.instrument); series.has_value()) {
            for (std::ptrdiff_t n = 0; n < series->first.size(); ++n) {
                event.user_d_history.push_back(series->first[n]);
                event.user_i_history.push_back(series->second[n]);
            }
        }
    }
    recorder().push(std::move(event));
}

/**
 * @brief fake B：六个可选回调全部实现，每次回调都记录 ctx 观测值
 *
 * 通过全局记录器的故障计划在指定品种的指定回调抛异常，用于验证异常隔离边界。
 */
struct FakeStrategy {
    void on_init(dztrader::InstrumentContext& ctx) {
        // B 侧保存只读视图副本（文档 §3「价格视图可复制到局部变量或传给指标函数」）：
        // 之后每根 bar 都不重新取视图，用它验证视图跨回调、跨推进仍有效。
        // 先保存再记录：Init 事件本身也要带上「此刻视图为空」这条观测。
        recorder().save_view(ctx.instrument_id(), ctx.close);
        // #158：用户序列在 on_init 注册（两条：double + int64），声明初值 7.5 / 11
        recorder().save_user_series(ctx.instrument_id(), ctx.make_series<double>(kUserSeriesInitD),
                                    ctx.make_series<std::int64_t>(kUserSeriesInitI));
        record(EventKind::Init, ctx);
        // #160：`on_init` 期的意图是合法用法（先建立初始逻辑持仓），但此时实例还没进入 live，
        // 因此只做虚拟成交、不发单不上报。脚本在 record 之后执行，与其它回调同序。
        run_scripts(ctx, EventKind::Init);
        if (recorder().should_throw(ctx.instrument_id(), "init")) {
            throw std::runtime_error("injected on_init failure");
        }
    }

    /**
     * @brief 执行测试脚本注入到本次回调的交易意图（#159）
     *
     * 位置在 `record()` 之后：那一格快照是「意图执行前」的 Tier 1 状态，紧随其后的回调快照
     * 是「意图执行后」的状态；两者之差即「意图即时生效」的证据。脚本也可在 `apply` 内自行
     * 读 ctx，断言同一回调内立刻可见。
     */
    static void run_scripts(dztrader::InstrumentContext& ctx, EventKind kind) {
        for (ScriptedIntent& script :
             recorder().take_scripts(ctx.instrument_id(), kind, ctx.bar_index())) {
            script.apply(ctx);
        }
    }

    void on_bar_open(dztrader::InstrumentContext& ctx) {
        record(EventKind::BarOpen, ctx);
        write_user_series(ctx);
        run_scripts(ctx, EventKind::BarOpen);
        if (recorder().should_throw(ctx.instrument_id(), "bar_open")) {
            throw std::runtime_error("injected on_bar_open failure");
        }
    }

    /**
     * @brief 把当前 bar 的序号写进用户序列 `[0]`：值 = `bar_index * 1000`
     *
     * **这是 carry 与回卷两条语义的可观测证据**：`on_init` 里框架把整条历史填成声明初值
     * `kUserSeriesInit*`。若 B 从不写、或框架不 carry，序列会永远是同一个数——那种情况下
     * 「回卷漏搬用户序列」与「搬对了」完全无法区分。写入随 `bar_index` 变化的值之后，历史位
     * 的每一格都有确定的期望值（见 `user_series_final_value_i` / `_d`），漏搬或漏 carry
     * 都会被抓到。
     *
     * 只在 `on_bar_update` 写，且值只依赖 `bar_index`：这样历史位满足一个干净的不变式——
     * 第 `k` 根 bar 的值 = `k * 1000`（静默填窗的 bar 没有回调、保持声明初值）。
     */
    static void write_user_series(dztrader::InstrumentContext& ctx) {
        (void)recorder().write_user_series_slot(ctx.instrument_id(), ctx.bar_index() * 1000);
    }

    void on_bar_update(dztrader::InstrumentContext& ctx) {
        record(EventKind::BarUpdate, ctx);
        write_user_series(ctx);
        run_scripts(ctx, EventKind::BarUpdate);
        if (recorder().should_make_series(ctx.instrument_id())) {
            // 非 on_init 注册必须响亮失败（组件文档 §4.1「创建」）。让异常逃出回调，
            // 框架会把该实例标记失败——这本身就是「被发现」的证据。
            (void)ctx.make_series<double>(0.0);
        }
        if (recorder().should_throw(ctx.instrument_id(), "bar_update")) {
            throw std::runtime_error("injected on_bar_update failure");
        }
    }

    void on_bar_close(dztrader::InstrumentContext& ctx) {
        record(EventKind::BarClose, ctx);
        run_scripts(ctx, EventKind::BarClose);
        if (recorder().should_throw(ctx.instrument_id(), "bar_close")) {
            throw std::runtime_error("injected on_bar_close failure");
        }
    }

    void on_destroy(dztrader::InstrumentContext& ctx) {
        record(EventKind::Destroy, ctx);
        if (recorder().should_throw(ctx.instrument_id(), "destroy")) {
            throw std::runtime_error("injected on_destroy failure");
        }
    }
};

static_assert(dztrader::BarStrategy<FakeStrategy>);

// ============================================================
// 通道 helper
// ============================================================

void create_event_channel(const std::filesystem::path& shm_dir) {
    dztrader::shm::ChannelConfig cfg{
        .channel_name = dztrader::shm::channel_name(dztrader::CHANNEL_NAME_EVENT),
        .shm_dir = shm_dir,
        .meta_file_size = 1 * kMB,
        .page_size = 1 * kMB,
        .lock_memory = false,
        .prefetch_memory = false,
    };
    (void)dztrader::shm::ChannelMeta::open_or_create(cfg);
}

void create_md_channel(const std::filesystem::path& shm_dir) {
    dztrader::shm::ChannelConfig cfg{
        // NOLINT(temp-dir-unique): test_md 是 SDK 约定的行情通道名，其 shm_dir 每用例唯一
        .channel_name = "test_md", .shm_dir = shm_dir,   .meta_file_size = 1 * kMB,
        .page_size = 1 * kMB,      .lock_memory = false, .prefetch_memory = false,
    };
    (void)dztrader::shm::ChannelMeta::open_or_create(cfg);
}

/// @brief 把事件序列渲染成 "init,bar_open,bar_update,..."（断言失败时便于定位）
std::string describe_events(const std::vector<ObservedEvent>& events) {
    std::string text;
    for (const ObservedEvent& event : events) {
        if (!text.empty()) {
            text += ',';
        }
        text += event_kind_name(event.kind);
    }
    return text;
}

DzTick make_tick_on(std::string_view instrument,
                    DzDate trading_day,
                    int32_t clock_sec,
                    double price,
                    int32_t cum_volume,
                    double cum_turnover);

/// @brief 构造一笔 tick（累计量额按调用方给的值，必须单调不减）
DzTick make_tick(std::string_view instrument,
                 int32_t clock_sec,
                 double price,
                 int32_t cum_volume,
                 double cum_turnover) {
    return make_tick_on(instrument, kTradingDay, clock_sec, price, cum_volume, cum_turnover);
}

/// @brief 构造一笔指定交易日的 tick（跨日脚本用）
DzTick make_tick_on(std::string_view instrument,
                    DzDate trading_day,
                    int32_t clock_sec,
                    double price,
                    int32_t cum_volume,
                    double cum_turnover) {
    DzTick tick{};
    const std::size_t copy_len = instrument.size() < sizeof(tick.instrument_id) - 1
                                     ? instrument.size()
                                     : sizeof(tick.instrument_id) - 1;
    std::memcpy(tick.instrument_id, instrument.data(), copy_len);
    tick.instrument_id[copy_len] = '\0';
    tick.date = trading_day;
    tick.time = clock_sec;
    tick.last_price = price;
    tick.volume = cum_volume;
    tick.turnover = cum_turnover;
    tick.open_interest = 1000;
    return tick;
}

/**
 * @brief 框架集成测试夹具：临时 home + 真实通道 + 确定性配置
 *
 * `run_bar_strategy` 内部自己 `dz_init` / `dz_release`，夹具只准备环境、写配置、注入帧。
 * 策略线程用定向信号量唤醒（测试进程不是 master，事件通道订阅者列表为空，
 * `writer.notify_subscribers()` 唤不醒 `dz_wait`）。
 */
class BarStrategyFrameworkTest : public ::testing::Test {
protected:
    void SetUp() override {
        recorder().reset();
        home_ = dztrader::test::unique_temp_dir("dz_test_bar_framework").string();
        std::filesystem::remove_all(home_);
        std::filesystem::create_directories(home_ + "/shm");
        orig_home_ = dztrader::env::get("DZTRADER_HOME");
        orig_md_source_ = dztrader::env::get("DZTRADER_MD_SOURCE");
        dztrader::env::set("DZTRADER_HOME", home_);
        dztrader::env::set("DZTRADER_MD_SOURCE", "test_md");
        dztrader::paths::reset_for_test();
        create_event_channel(home_ + "/shm");
        create_md_channel(home_ + "/shm");
        // 策略身份 = 测试 exe 名；配置路径与框架内部推导规则一致（exe 目录 + `<stem>.json`）
        strategy_id_ = dztrader::this_process::exe_stem();
        config_path_ = dztrader::this_process::exe_dir() / (strategy_id_ + ".json");
        std::error_code ec;
        std::filesystem::remove(config_path_, ec);  // 上一轮残留（崩溃时可能留下）
    }

    void TearDown() override {
        if (strategy_thread_.joinable()) {
            // 兜底：用例断言提前失败时策略仍在跑，定向 SHUTDOWN 结束它
            write_shutdown();
            wake_strategy();
            strategy_thread_.join();
        }
        std::error_code ec;
        std::filesystem::remove(config_path_, ec);  // 缺失分支会让框架生成默认配置
        std::filesystem::remove_all(home_, ec);
        if (orig_home_.has_value()) {
            dztrader::env::set("DZTRADER_HOME", *orig_home_);
        } else {
            dztrader::env::unset("DZTRADER_HOME");
        }
        if (orig_md_source_.has_value()) {
            dztrader::env::set("DZTRADER_MD_SOURCE", *orig_md_source_);
        } else {
            dztrader::env::unset("DZTRADER_MD_SOURCE");
        }
        dztrader::paths::reset_for_test();
    }

    // ---- 配置 ----

    /// @brief 写一份合法配置（默认 0 个挂靠账户）
    /// @param max_lookback 可选显式可读深度（门禁用例用它把静默填窗压到几秒内完成）
    /// @param reserve_bars 可选显式回卷余量（兜底路径用例用它把两次回卷压得很近）
    /// @param period_sec   bar 周期秒数（默认 60；回卷等价性用例用 900 换取更小的 buffer_bars）
    /// @param accounts_json `accounts` 数组字面量（#160 的挂靠用例传 1–2 个账户）
    /// @param scaling_json  缩放字段片段（`"scale_rounding": ...` 等，含行尾逗号）
    void write_config(const std::vector<std::string>& instruments,
                      std::optional<std::int32_t> max_lookback = std::nullopt,
                      std::optional<std::int32_t> reserve_bars = std::nullopt,
                      std::int32_t period_sec = 60,
                      const std::string& accounts_json = "[]",
                      const std::string& scaling_json = "") {
        std::string list;
        for (const std::string& instrument : instruments) {
            if (!list.empty()) {
                list += ", ";
            }
            list += '"' + instrument + '"';
        }
        std::string text =
            "{\n  \"schema_version\": 1,\n  \"period_sec\": " + std::to_string(period_sec) +
            ",\n  \"instruments\": [" + list + "],\n  \"accounts\": " + accounts_json + ",\n";
        if (max_lookback.has_value()) {
            text += "  \"max_lookback\": " + std::to_string(*max_lookback) + ",\n";
        }
        if (reserve_bars.has_value()) {
            text += "  \"reserve_bars\": " + std::to_string(*reserve_bars) + ",\n";
        }
        text += scaling_json;
        text += "  \"params\": {}\n}\n";
        std::ofstream stream(config_path_);
        ASSERT_TRUE(stream.good()) << "write config failed: " << config_path_.string();
        stream << text;
    }

    // ---- 帧通道 ----

    std::shared_ptr<dztrader::shm::ChannelMeta> open_event_meta() {
        return std::make_shared<dztrader::shm::ChannelMeta>(dztrader::shm::ChannelMeta::open_only(
            dztrader::shm::channel_name(dztrader::CHANNEL_NAME_EVENT), home_ + "/shm"));
    }

    std::shared_ptr<dztrader::shm::ChannelMeta> open_md_meta() {
        return std::make_shared<dztrader::shm::ChannelMeta>(
            dztrader::shm::ChannelMeta::open_only("test_md", home_ + "/shm"));
    }

    /// @brief 写一帧 tick（无 gtest 断言：可在任意线程调用）
    bool write_tick(const DzTick& tick) {
        try {
            dztrader::shm::MultiWriter writer =
                dztrader::shm::MultiWriter::create(open_md_meta(), "bar_framework_test_md_writer");
            return writer.write_frame(DZ_FRAME_TICK, tick);
        } catch (...) {
            return false;
        }
    }

    /// @brief 写定向本策略的 SHUTDOWN 帧（无 gtest 断言：可在任意线程调用）
    void write_shutdown() {
        try {
            dztrader::shm::MultiWriter writer =
                dztrader::shm::MultiWriter::create(open_event_meta(), "bar_framework_test_writer");
            (void)writer.write_ext_inst_frame(DZ_FRAME_SHUTDOWN, strategy_id_.c_str(), nullptr, 0);
            writer.notify_subscribers();
        } catch (...) {
        }
    }

    /// @brief 直连策略信号量唤醒 `dz_wait`（测试进程无 master，订阅者列表为空）
    void wake_strategy() {
        try {
            dztrader::shm::NamedSemaphore sem("stg." + strategy_id_);
            sem.notify();
        } catch (...) {
        }
    }

    // ---- 策略线程 ----

    /// @brief 在线程里启动 `run_bar_strategy`（内部自行 dz_init / dz_release）
    void start_strategy() {
        strategy_thread_ =
            std::thread([this] { exit_code_ = dztrader::run_bar_strategy<FakeStrategy>(); });
    }

    /**
     * @brief 在线程里启动策略，并把 18:00 回卷时刻换到「若干秒后」（#158 定时器路径用例）
     *
     * 18:00 真实定时器要等一天，Seam A 集成测试因此经内部接缝注入一个近未来时刻：定时器
     * 仍走完整的 `dz_schedule_daily` → `DZ_FRAME_SCHEDULE` → `on_schedule` 链路，只是触发
     * 时刻可控。B 观察到的值必须与推进前兜底路径完全一致。
     *
     * @param seconds_from_now 相对「现在」的秒数（1–3 秒，留足启动与断言时间）
     */
    void start_strategy_with_rollback_clock(std::int32_t seconds_from_now) {
        const auto now = dztrader::DateTime::local_now();
        const std::int32_t clock_ms =
            (now.millisecs_since_midnight() + seconds_from_now * 1000) % 86'400'000;
        strategy_thread_ = std::thread([this, clock_ms] {
            exit_code_ = dztrader::bar_strategy_internal::run_bar_strategy_with<FakeStrategy>(
                dztrader::degraded_trading_session_table(), clock_ms);
        });
    }

    /**
     * @brief 轮询直到 pred 成立（每次醒一次策略并小睡；超时返回 false）
     * @param pred    主线程断言前的就绪条件
     * @param timeout 超时
     */
    bool wait_until(const std::function<bool()>& pred, std::chrono::milliseconds timeout = 15s) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            if (pred()) {
                return true;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            wake_strategy();
            std::this_thread::sleep_for(5ms);
        }
    }

    /// @brief 等某品种某类事件达到 count
    bool wait_for_count(std::string_view instrument, EventKind kind, std::size_t count) {
        return wait_until(
            [instrument, kind, count] { return recorder().count(instrument, kind) >= count; });
    }

    /// @brief 等事件总数达到 count（启动阶段等 on_init 用）
    bool wait_for_events(std::size_t count) {
        return wait_until([count] { return recorder().size() >= count; });
    }

    /// @brief 写 SHUTDOWN、唤醒、等线程结束（之后记录器里含 on_destroy 事件）
    void stop_strategy() {
        write_shutdown();
        wake_strategy();
        if (strategy_thread_.joinable()) {
            strategy_thread_.join();
        }
    }

    // ---- 静默填窗 ----

    /**
     * @brief 喂满静默填窗：`kTestMaxLookback` 根 bar（索引 `0 … max_lookback-1`），B 零回调
     *
     * 时间从 `64830`（tsec 30，bar begin=0）起每根 60 秒一格，bucket 与 clock 都单调递增，
     * 满足 builder 的乱序丢弃判据。写完这些 tick 后窗口恰好装满（`size == max_lookback`），
     * 但 B 一个回调都没收到——这正是门禁要求的口径。
     *
     * 时钟跨午夜不回绕：`kTestMaxLookback`（32）× 60s = 1920s，从 64830 起最远到 66750，
     * 远在 86400 之内。
     *
     * @param instrument   品种
     * @param start_clock  首笔 tick 的墙钟秒（默认 64830 = tsec 30，落进 begin=0 那根 bar）
     * @param start_price  首根 bar 的价格（每根 +1，便于在断言里反推 bar 序号）
     * @param start_volume 首根 bar 的累计成交量（每根 +10）
     * @return 此刻记录到的本品种事件总数（静默期只含 `on_init`，调用方断言 `== 1`）
     */
    std::size_t warm_up(std::string_view instrument,
                        int32_t start_clock = 64830,
                        double start_price = 100.0,
                        int32_t start_volume = 10) {
        for (std::int32_t bar = 0; bar < kTestMaxLookback; ++bar) {
            const int32_t clock = (start_clock + bar * 60) % 86400;
            const double price = start_price + static_cast<double>(bar);
            const auto volume = start_volume + 10 * bar;
            const double turnover = price * static_cast<double>(volume);
            EXPECT_TRUE(write_tick(make_tick(instrument, clock, price, volume, turnover)));
        }
        return recorder().for_instrument(instrument).size();
    }

    /**
     * @brief 静默填窗之后第 k 笔 tick 的墙钟秒（与 {@link warm_up} 的时钟连续）
     * @param bars_after_warmup 距静默填窗结束再往后几根 bar（0 = 首根回调 bar）
     */
    static int32_t warmed_up_clock(std::int32_t bars_after_warmup, int32_t start_clock = 64830) {
        return (start_clock + (kTestMaxLookback + bars_after_warmup) * 60) % 86400;
    }

    /// @brief 静默填窗之后第 k 笔 tick 的价格（与 {@link warm_up} 的价格连续）
    static double warmed_up_price(std::int32_t bars_after_warmup, double start_price = 100.0) {
        return start_price + static_cast<double>(kTestMaxLookback + bars_after_warmup);
    }

    /// @brief 静默填窗之后第 k 笔 tick 的累计成交量（与 {@link warm_up} 的量连续）
    static int32_t warmed_up_volume(std::int32_t bars_after_warmup, int32_t start_volume = 10) {
        return start_volume + 10 * (kTestMaxLookback + bars_after_warmup);
    }

    /**
     * @brief 900 秒周期脚本的 tick 游标：每根 bar 一笔 tick，按**交易日内秒**步进
     *
     * 900 秒 × (32 静默 + 120 回调) = 136,800 秒 > 一天，脚本必然跨交易日。**交易日按 18:00
     * 锚点翻，不是午夜**（`TradingTime::from_clock`：tsec 在墙钟 18:00 归零），所以游标按
     * tsec 步进、`tsec >= 86400` 时交易日 +1 并回绕——按墙钟午夜翻会让 tsec 在同一交易日内
     * 从 85500 跳回 0，被 builder 当乱序丢掉。
     *
     * 每根 tick 落在桶起点（`tsec` 起点是 900 的整数倍且步长恰为 900），保证一根 tick 一根 bar。
     * 累计量额**不跨日重置**：跨日后 builder 以 0 为基准比较，继续单调递增天然合法。
     */
    struct BarScriptCursor {
        DzDate trading_day = kTradingDay;  ///< 当前交易日
        std::int32_t tsec = 21600;         ///< 当前交易日内秒（21600 = 墙钟 0:00）
        std::int32_t cum_volume = 10;      ///< 累计成交量（单调不减）
        double price = 100.0;              ///< 本根 bar 的价格（每根 +1）
        std::int32_t bars_emitted = 0;     ///< 已产出 bar 数
    };

    /// @brief 按 900 秒步进推进一步游标并写出该 bar 的 tick
    void emit_900_second_bar(std::string_view instrument, BarScriptCursor& cursor) {
        const double turnover = cursor.price * static_cast<double>(cursor.cum_volume);
        const int32_t clock_sec = dztrader::bar::TradingTime::to_clock(cursor.tsec);
        EXPECT_TRUE(write_tick(make_tick_on(instrument, cursor.trading_day, clock_sec, cursor.price,
                                            cursor.cum_volume, turnover)));
        ++cursor.bars_emitted;
        cursor.price += 1.0;
        cursor.cum_volume += 10;
        cursor.tsec += 900;
        if (cursor.tsec >= 86400) {
            cursor.tsec -= 86400;
            ++cursor.trading_day;
        }
    }

    /**
     * @brief 900 秒周期的静默填窗：`kTestMaxLookback` 根 bar，B 零回调
     *
     * 每根 bar 的**起点**时刻落一个 tick（`clock_sec` 始终是桶起点，避免落进相邻桶）。
     *
     * @return 静默期观察到的 B 回调数（必须为 1，只有 on_init）
     */
    std::size_t warm_up_900(std::string_view instrument, BarScriptCursor& cursor) {
        for (std::int32_t bar = 0; bar < kTestMaxLookback; ++bar) {
            emit_900_second_bar(instrument, cursor);
        }
        return recorder().for_instrument(instrument).size();
    }

    /// @brief 900 秒周期的回调期：`bars` 根 bar，每根一笔 tick
    void feed_900_second_callback_stage(std::string_view instrument,
                                        BarScriptCursor& cursor,
                                        std::int32_t bars) {
        for (std::int32_t bar = 0; bar < bars; ++bar) {
            emit_900_second_bar(instrument, cursor);
        }
    }

    /// @brief 与 B 可见面无关的事件（init/destroy 不进回卷等价性比较）
    std::vector<ObservedEvent> callback_stage_events(std::string_view instrument) {
        std::vector<ObservedEvent> events = recorder().for_instrument(instrument);
        std::erase_if(events, [](const ObservedEvent& event) {
            return event.kind == EventKind::Init || event.kind == EventKind::Destroy;
        });
        return events;
    }

    // ---- 订单帧 / 上报帧 probe（#160：SDK 写出的帧在通道里，用独立 reader 观察）----

    /// @brief 本用例观测到的订单帧（`DZ_FRAME_TD_ORDER_REQ` 的 payload）
    struct ObservedOrder {
        std::string account_id;                             ///< 挂靠账户
        std::string instrument_id;                          ///< 合约代码
        DzDirection direction = DZ_DIRECTION_NET;           ///< 方向
        DzPriceType price_type = DZ_PRICE_LIMIT;            ///< 价格类型
        DzPositionEffect effect = DZ_POSITION_EFFECT_AUTO;  ///< 开平
        double price = 0;                                   ///< 委托价
        dztrader::Volume volume = 0;                        ///< 委托手数
    };

    /// @brief 本用例观测到的上报帧（`DZ_FRAME_SET_LOGICAL_POSITION` 的 payload）
    struct ObservedPositionReport {
        std::string account_id;                ///< 挂靠账户
        std::string instrument_id;             ///< 合约代码
        dztrader::LargeVolume net_volume = 0;  ///< 上报的逻辑持仓
    };

    /**
     * @brief 排空事件通道里由策略写出的订单帧与上报帧（须在策略停止后调用）
     *
     * 用**独立 probe reader** 而不是 `dz_next_event`：平台内部帧（TD_ORDER_REQ /
     * SET_LOGICAL_POSITION）在 SDK 侧被拦截，不会交给策略回调（见 `trade_api_test` 的同款
     * 做法）。probe 从创建时刻起顺序读通道，因此必须在启动策略**之前**创建它，否则会漏掉
     * 启动阶段的帧——本文件只在停止后读一次，创建点放在 `start_strategy()` 之前。
     *
     * 只保留本策略写的两类帧：同一个 shm 目录里的 md/event 通道可能也有测试自身写入的帧。
     */
    void collect_order_and_report_frames() {
        orders_.clear();
        reports_.clear();
        if (!probe_.has_value()) {
            return;
        }
        while (const std::byte* frame = probe_->next_frame()) {
            const dztrader::shm::FrameView view(frame);
            if (view.type() == DZ_FRAME_TD_ORDER_REQ) {
                const auto& req = view.payload<DzOrderReq>();
                ObservedOrder order;
                order.account_id = req.account_id;
                order.instrument_id = req.instrument_id;
                order.direction = req.direction;
                order.price_type = req.price_type;
                order.effect = req.position_effect;
                order.price = req.price;
                order.volume = req.volume;
                orders_.push_back(std::move(order));
            } else if (view.type() == DZ_FRAME_SET_LOGICAL_POSITION) {
                const auto& pos = view.payload<DzLogicalPosition>();
                ObservedPositionReport report;
                report.account_id = pos.account_id;
                report.instrument_id = pos.instrument_id;
                report.net_volume = pos.net_volume;
                reports_.push_back(std::move(report));
            }
        }
    }

    /// @brief 打开 probe reader（须在 `start_strategy()` 之前调用，否则漏启动帧）
    void open_probe() {
        probe_.emplace(
            dztrader::shm::Reader::create(dztrader::shm::channel_name(dztrader::CHANNEL_NAME_EVENT),
                                          home_ + "/shm", "bar_framework_test_probe"));
    }

    std::string home_;
    std::filesystem::path config_path_;
    std::string strategy_id_;
    std::optional<std::string> orig_home_;
    std::optional<std::string> orig_md_source_;
    std::thread strategy_thread_;
    std::int32_t exit_code_ = -1;
    std::optional<dztrader::shm::Reader> probe_;
    std::vector<ObservedOrder> orders_;
    std::vector<ObservedPositionReport> reports_;
};

// ============================================================
// 退化交易小节表（组件文档 §5.5）
// ============================================================

TEST(BarStrategyDegradedSessionTable, CoversWholeTradingDayWithoutAuctionWindows) {
    const dztrader::bar::TradingSessionTable table = dztrader::degraded_trading_session_table();
    ASSERT_TRUE(table.valid());
    ASSERT_EQ(table.session_count, 1);
    EXPECT_EQ(table.sessions[0].begin_sec, 0);
    EXPECT_EQ(table.sessions[0].end_sec, 86399);
    EXPECT_EQ(table.auction_count, 0);
}

/// 退化表下 builder 接受交易日内首秒与末秒的 tick（确实覆盖全交易日内秒）
TEST(BarStrategyDegradedSessionTable, BuilderAcceptsDayStartAndDayEndTicks) {
    dztrader::bar::NullBarSink sink;
    dztrader::bar::IntradayBarBuilder<dztrader::bar::NullBarSink> builder(
        dztrader::degraded_trading_session_table(), 60, sink);

    builder.on_tick(make_tick(kInstrumentA, 64800, 100.0, 1, 100.0));  // tsec 0
    ASSERT_TRUE(builder.has_current_bar());
    EXPECT_EQ(builder.current_bar().begin_sec, 0);

    builder.on_tick(make_tick(kInstrumentA, 64799, 101.0, 2, 201.0));  // tsec 86399
    EXPECT_EQ(builder.stats().out_of_session, 0U);
    EXPECT_EQ(builder.current_bar().begin_sec, 86340);
}

// ============================================================
// 视图有效性：B 在 on_init 保存的视图副本跨 bar 推进仍有效（#157 AC2）
// ============================================================

/**
 * B 在 `on_init` 里保存 `ctx.close` 的副本（`SeriesView` 只绑一次数组与窗口指针），之后每根
 * bar 都不重新取视图，仍能读到当前 bar 的价——「视图构造一次后永不重绑，窗口一推进全部视图
 * 同时看到新 bar」（组件文档 §3）的可观测证据，也是「其它实例增删不搬移本节点」的前提。
 */
TEST_F(BarStrategyFrameworkTest, SavedViewStaysValidAcrossBarAdvances) {
    // 门禁（#158）之后 B 第一次回调在 bar_index == max_lookback，因此这里用最小的显式
    // max_lookback 把静默填窗压到 32 根，用例仍是秒级。
    write_config({std::string(kInstrumentA)}, kTestMaxLookback);
    start_strategy();
    ASSERT_TRUE(wait_for_events(1)) << "on_init not observed";

    // 静默填窗：前 32 根 bar 零回调
    EXPECT_EQ(warm_up(kInstrumentA), 1U) << "only on_init must be observed before the gate opens";

    // 回调期第一根（bar_index == max_lookback）：close(31) + open(32) + update
    ASSERT_TRUE(
        write_tick(make_tick(kInstrumentA, warmed_up_clock(0), warmed_up_price(0),
                             warmed_up_volume(0), warmed_up_price(0) * warmed_up_volume(0))));
    ASSERT_TRUE(wait_for_count(kInstrumentA, EventKind::BarUpdate, 1));
    // 下一桶：close(32) + open(33) + update
    ASSERT_TRUE(
        write_tick(make_tick(kInstrumentA, warmed_up_clock(1), warmed_up_price(1),
                             warmed_up_volume(1), warmed_up_price(1) * warmed_up_volume(1))));
    ASSERT_TRUE(wait_for_count(kInstrumentA, EventKind::BarUpdate, 2));
    stop_strategy();

    EXPECT_EQ(exit_code_, 0);

    const auto events = recorder().for_instrument(kInstrumentA);
    ASSERT_GE(events.size(), 6U) << "callbacks: " << describe_events(events);

    // on_init 时视图为空（窗口未填）
    EXPECT_EQ(events[0].kind, EventKind::Init);
    EXPECT_EQ(events[0].saved_view_size, 0);

    // on_init 之后每次回调里，保存的视图副本都等于当前窗口（无需重取视图）
    for (const ObservedEvent& event : events) {
        if (event.kind == EventKind::Init) {
            continue;
        }
        EXPECT_TRUE(event.saved_view_bound);
        EXPECT_EQ(event.saved_view_size, event.window_size)
            << "saved view must track the live window at bar_index=" << event.bar_index;
        if (event.saved_view_size > 0) {
            EXPECT_DOUBLE_EQ(event.saved_view_close, event.close)
                << "saved view [0] must equal ctx.close[0]";
        }
    }

    // 回调期第二根 bar 打开后，保存的视图读到新 bar 的价与封顶的窗口长度
    const std::int64_t second_callback_bar = kTestMaxLookback + 1;
    const ObservedEvent* second_open = nullptr;
    for (const ObservedEvent& event : events) {
        if (event.kind == EventKind::BarOpen && event.bar_index == second_callback_bar) {
            second_open = &event;
        }
    }
    ASSERT_NE(second_open, nullptr) << "second callback-stage bar open not observed";
    EXPECT_EQ(second_open->saved_view_size, kTestMaxLookback)
        << "window is full from the first callback on";
    EXPECT_DOUBLE_EQ(second_open->saved_view_close, warmed_up_price(1));
}

// ============================================================
// 回调序列、bar_index、价格值（真实帧通道）
// ============================================================

TEST_F(BarStrategyFrameworkTest, DispatchesCloseOpenUpdateInOrderWithBarIndexAndPrices) {
    write_config({std::string(kInstrumentA), std::string(kInstrumentB)}, kTestMaxLookback);
    testing::internal::CaptureStderr();
    start_strategy();
    ASSERT_TRUE(wait_for_events(2)) << "on_init not observed for both instruments";

    // 静默填窗（两品种各 max_lookback 根 bar，B 零回调）
    EXPECT_EQ(warm_up(kInstrumentA, 64830), 1U);
    EXPECT_EQ(warm_up(kInstrumentB, 64831), 1U);

    // A：回调期首根 bar 内两笔（同桶）→ 下一桶
    const int32_t a_clock = warmed_up_clock(0, 64830);
    ASSERT_TRUE(write_tick(make_tick(kInstrumentA, a_clock, 300.0, 500, 150000.0)));
    ASSERT_TRUE(write_tick(make_tick(kInstrumentA, a_clock + 10, 300.5, 510, 153255.0)));
    ASSERT_TRUE(
        write_tick(make_tick(kInstrumentA, warmed_up_clock(1, 64830), 301.0, 520, 156520.0)));
    // B：回调期首根 bar 一笔 → 下一桶
    const int32_t b_clock = warmed_up_clock(0, 64831);
    ASSERT_TRUE(write_tick(make_tick(kInstrumentB, b_clock, 400.0, 600, 240000.0)));
    ASSERT_TRUE(
        write_tick(make_tick(kInstrumentB, warmed_up_clock(1, 64831), 400.5, 610, 244305.0)));

    // A: close(31) + open(32) + update×2 + close(32) + open(33) + update = 7
    ASSERT_TRUE(wait_for_count(kInstrumentA, EventKind::BarUpdate, 3));
    // B: close(31) + open(32) + update + close(32) + open(33) + update = 6
    ASSERT_TRUE(wait_for_count(kInstrumentB, EventKind::BarUpdate, 2));
    ASSERT_TRUE(wait_until([] {
        return recorder().count(kInstrumentA, EventKind::BarOpen) >= 2 &&
               recorder().count(kInstrumentB, EventKind::BarOpen) >= 2;
    }));

    const auto events_a = recorder().for_instrument(kInstrumentA);
    const auto events_b = recorder().for_instrument(kInstrumentB);
    stop_strategy();
    const std::string stderr_text = testing::internal::GetCapturedStderr();

    EXPECT_EQ(exit_code_, 0);

    // ── A：init → close(31) → open(32) → update,update → close(32) → open(33) → update ──
    const auto first = kTestMaxLookback;
    ASSERT_EQ(events_a.size(), 8U) << "instrument A callbacks: " << describe_events(events_a);
    EXPECT_EQ(events_a[0].kind, EventKind::Init);
    EXPECT_EQ(events_a[0].bar_index, -1) << "on_init sees bar_index == -1";
    EXPECT_EQ(events_a[0].window_size, 0) << "on_init sees an empty window";
    EXPECT_EQ(events_a[1].kind, EventKind::BarClose) << "first callback is the silent-fill close";
    EXPECT_EQ(events_a[1].bar_index, first - 1);
    EXPECT_EQ(events_a[1].window_size, first) << "the closing bar's window is already full";
    EXPECT_EQ(events_a[2].kind, EventKind::BarOpen);
    EXPECT_EQ(events_a[2].bar_index, first);
    EXPECT_EQ(events_a[3].kind, EventKind::BarUpdate);
    EXPECT_EQ(events_a[3].bar_index, first);
    EXPECT_DOUBLE_EQ(events_a[3].close, 300.0);
    EXPECT_EQ(events_a[4].kind, EventKind::BarUpdate);
    EXPECT_DOUBLE_EQ(events_a[4].close, 300.5);
    EXPECT_EQ(events_a[5].kind, EventKind::BarClose) << "close precedes the new bar's open";
    EXPECT_EQ(events_a[5].bar_index, first);
    EXPECT_DOUBLE_EQ(events_a[5].close, 300.5) << "close still sees the previous bar in [0]";
    EXPECT_EQ(events_a[6].kind, EventKind::BarOpen);
    EXPECT_EQ(events_a[6].bar_index, first + 1);
    EXPECT_DOUBLE_EQ(events_a[6].close, 301.0);
    EXPECT_EQ(events_a[7].kind, EventKind::BarUpdate);
    EXPECT_EQ(events_a[7].bar_index, first + 1);

    // on_bar_open 时新根 OHLC 已等于本笔 tick 价（组件文档 §5.1）
    for (const ObservedEvent& event : {events_a[2], events_a[6]}) {
        EXPECT_DOUBLE_EQ(event.open, event.close);
        EXPECT_DOUBLE_EQ(event.high, event.close);
        EXPECT_DOUBLE_EQ(event.low, event.close);
    }

    // ── B：init → close(31) → open(32) → update → close(32) → open(33) → update ──
    ASSERT_EQ(events_b.size(), 7U) << "instrument B callbacks: " << describe_events(events_b);
    EXPECT_EQ(events_b[0].kind, EventKind::Init);
    EXPECT_EQ(events_b[0].bar_index, -1);
    EXPECT_EQ(events_b[1].kind, EventKind::BarClose);
    EXPECT_EQ(events_b[1].bar_index, first - 1);
    EXPECT_EQ(events_b[2].kind, EventKind::BarOpen);
    EXPECT_EQ(events_b[2].bar_index, first);
    EXPECT_EQ(events_b[3].kind, EventKind::BarUpdate);
    EXPECT_EQ(events_b[4].kind, EventKind::BarClose);
    EXPECT_DOUBLE_EQ(events_b[4].close, 400.0);
    EXPECT_EQ(events_b[5].kind, EventKind::BarOpen);
    EXPECT_EQ(events_b[5].bar_index, first + 1);
    EXPECT_DOUBLE_EQ(events_b[5].close, 400.5);

    // ── bar_index 单调、只在 open 时前进，bar 内保持不变 ──
    for (const auto* events : {&events_a, &events_b}) {
        std::int64_t previous = kTestMaxLookback - 1;  // 首个可见事件 = 静默填窗最后一根的 close
        for (const ObservedEvent& event : *events) {
            if (event.kind == EventKind::Init) {
                continue;
            }
            EXPECT_GE(event.bar_index, previous) << "bar_index must be monotonic";
            if (event.kind == EventKind::BarOpen) {
                EXPECT_GT(event.bar_index, previous) << "bar_index advances on open";
            } else {
                EXPECT_EQ(event.bar_index, previous) << "bar_index stable within a bar";
            }
            previous = event.bar_index;
        }
    }

    // ── 实例 = 品种：两品种各有独立 ctx，节点地址在回调间稳定 ──
    EXPECT_NE(events_a[1].ctx_address, events_b[1].ctx_address);
    for (const ObservedEvent& event : events_a) {
        EXPECT_EQ(event.ctx_address, events_a[1].ctx_address)
            << "instrument A node address must stay stable";
    }
    for (const ObservedEvent& event : events_b) {
        EXPECT_EQ(event.ctx_address, events_b[1].ctx_address)
            << "instrument B node address must stay stable";
    }

    // ── 启动诊断：两行有效配置 + 退化小节表声明（组件文档 §5.5/§7）──
    EXPECT_NE(stderr_text.find("config effective |"), std::string::npos) << stderr_text;
    EXPECT_NE(stderr_text.find("accounts effective | -"), std::string::npos) << stderr_text;
    EXPECT_NE(stderr_text.find("trading session table degraded |"), std::string::npos)
        << "degraded session table must be announced: " << stderr_text;
    EXPECT_NE(stderr_text.find("reason=platform_db_absent"), std::string::npos) << stderr_text;

    // ── on_destroy 时机：停止时每个存活实例恰好一次 ──
    const auto final_events = recorder().snapshot();
    std::size_t destroy_a = 0;
    std::size_t destroy_b = 0;
    for (const ObservedEvent& event : final_events) {
        if (event.kind != EventKind::Destroy) {
            continue;
        }
        if (event.instrument == kInstrumentA) {
            ++destroy_a;
        }
        if (event.instrument == kInstrumentB) {
            ++destroy_b;
        }
    }
    EXPECT_EQ(destroy_a, 1U) << "on_destroy called once per live instrument";
    EXPECT_EQ(destroy_b, 1U);
}

// ============================================================
// 回调门禁 + 静默填窗 + 用户序列（#158 AC1/AC2/AC5）
// ============================================================

/**
 * **回调门禁**：窗口填满前零回调（组件文档 §5.2）。
 *
 * 静默填窗期间框架照常聚合 bar、推进窗口、写用户序列初值——只是不调 B。因此这段之后 B 一条
 * 事件都没有（只有 `on_init`）。
 */
TEST_F(BarStrategyFrameworkTest, CallbackGateSuppressesEverythingUntilWindowIsFull) {
    write_config({std::string(kInstrumentA)}, kTestMaxLookback);
    start_strategy();
    ASSERT_TRUE(wait_for_events(1)) << "on_init not observed";

    // 静默填窗：正好 max_lookback 根 bar（索引 0 … max_lookback-1）
    EXPECT_EQ(warm_up(kInstrumentA), 1U) << "no callback may fire before the price window is full";

    stop_strategy();
    EXPECT_EQ(exit_code_, 0);
    EXPECT_EQ(recorder().count(kInstrumentA, EventKind::BarOpen), 0U);
    EXPECT_EQ(recorder().count(kInstrumentA, EventKind::BarUpdate), 0U);
    EXPECT_EQ(recorder().count(kInstrumentA, EventKind::BarClose), 0U);
}

/**
 * **首根回调序列 + 回调期恒有 `size == max_lookback` + 用户序列初值/carry**（AC1/AC2/AC5）。
 */
TEST_F(BarStrategyFrameworkTest, FirstCallbackSequenceAndFullWindowReads) {
    write_config({std::string(kInstrumentA)}, kTestMaxLookback);
    start_strategy();
    ASSERT_TRUE(wait_for_events(1));
    ASSERT_EQ(warm_up(kInstrumentA), 1U);

    // 第一根回调 bar：close(31) → open(32) → update
    ASSERT_TRUE(
        write_tick(make_tick(kInstrumentA, warmed_up_clock(0), warmed_up_price(0),
                             warmed_up_volume(0), warmed_up_price(0) * warmed_up_volume(0))));
    ASSERT_TRUE(wait_for_count(kInstrumentA, EventKind::BarUpdate, 1));
    // 第二根回调 bar：close(32) → open(33) → update
    ASSERT_TRUE(
        write_tick(make_tick(kInstrumentA, warmed_up_clock(1), warmed_up_price(1),
                             warmed_up_volume(1), warmed_up_price(1) * warmed_up_volume(1))));
    ASSERT_TRUE(wait_for_count(kInstrumentA, EventKind::BarUpdate, 2));
    stop_strategy();
    EXPECT_EQ(exit_code_, 0) << "no out-of-range read may fail the instance";

    const auto events = recorder().for_instrument(kInstrumentA);
    ASSERT_EQ(events.size(), 8U) << "callbacks: " << describe_events(events);
    EXPECT_EQ(events[0].kind, EventKind::Init);
    // ── AC1：首个回调序列 close → open → update ──
    EXPECT_EQ(events[1].kind, EventKind::BarClose)
        << "first callback must be the fill-window close";
    EXPECT_EQ(events[1].bar_index, kTestMaxLookback - 1);
    EXPECT_EQ(events[2].kind, EventKind::BarOpen);
    EXPECT_EQ(events[2].bar_index, kTestMaxLookback);
    EXPECT_EQ(events[3].kind, EventKind::BarUpdate);
    EXPECT_EQ(events[3].bar_index, kTestMaxLookback);

    // ── AC2：回调期恒有 size == max_lookback，且每个 [n] 都读成功 ──
    for (const ObservedEvent& event : events) {
        if (event.kind == EventKind::Init) {
            continue;
        }
        EXPECT_EQ(event.window_size, kTestMaxLookback)
            << "window must stay full during callbacks, bar_index=" << event.bar_index;
        EXPECT_EQ(event.history.size(), static_cast<std::size_t>(kTestMaxLookback))
            << "every [n] for n < max_lookback must be readable";
        expect_strictly_decreasing_history(event);
    }

    // ── AC5：静默期逐根写声明初值 + 回调期 carry 上一根当前值 ──
    //
    // B 只在 `on_bar_update` 写序列，值 = `bar_index * 1000`；静默填窗的 bar 没有回调、保持
    // 声明初值。于是窗口里的期望值就是一个确定函数（见 expect_user_series_window）：
    //   - 值 == 初值 → 这根属静默填窗（B 没写过）
    //   - 值 == 索引 * 1000 → B 写过这根（回调期）
    // 漏 carry 会让某个回调期槽位停在初值；漏搬（回卷只搬价格）会让历史错位一根——
    // 两者都会在这里被抓到。
    for (const ObservedEvent& event : events) {
        if (event.kind == EventKind::Init) {
            continue;
        }
        ASSERT_EQ(event.user_d_history.size(), static_cast<std::size_t>(kTestMaxLookback))
            << "user series must share the price window";
        ASSERT_EQ(event.user_i_history.size(), static_cast<std::size_t>(kTestMaxLookback));
        expect_user_series_window(event);
    }
}

/**
 * **首根 bar 没有 close**（AC3）：实例的第一笔有效 tick 只触发 `open → update`。
 *
 * 这条 AC 在门禁之下不能靠"看首根 bar 的回调"验证——门禁让前 `max_lookback` 根零回调，首根
 * 的回调根本不存在。因此本用例分两层验证：
 *
 * 1. **框架层（本用例）**：`max_lookback = 32`（配置下限），静默填窗 32 根之后，回调期的
 *    第一根 bar 索引是 32；把整段回调序列里**每一条 close 的 bar_index** 收集起来，断言
 *    **没有任何一条 close 属于 bar 0**——即首根 bar 从未被"收盘"，与"首根没有上一根可关闭"
 *    一致。同时断言回调序列的第一条事件是 `close(31)`（静默填窗最后一根），而不是 `close(0)`。
 * 2. **builder 层（`intraday_bar_lifecycle_test.cpp::FirstTickEmitsOpenThenUpdate`，已有）**：
 *    直接对 `IntradayBarBuilder` 喂首笔 tick，断言恰好 `Open → Update` 两个事件、无 `Close`。
 *    bar 生命周期由 builder 决定（ADR 0031），那才是这条 AC 的语义归属地；框架层只能观察到
 *    它的推论（"bar 0 从未被报告为收盘"）。
 *
 * 两层合起来覆盖 AC3 的完整语义；单看框架层会因门禁而变弱，单看 builder 层则漏掉"框架确实
 * 把首根的回调压住了、没有提前合成一条 close"这条推论。
 */
TEST_F(BarStrategyFrameworkTest, FirstBarIsNeverReportedAsClosed) {
    write_config({std::string(kInstrumentA)}, kTestMaxLookback);
    start_strategy();
    ASSERT_TRUE(wait_for_events(1));
    ASSERT_EQ(warm_up(kInstrumentA), 1U);

    // 回调期前两根 bar
    ASSERT_TRUE(
        write_tick(make_tick(kInstrumentA, warmed_up_clock(0), warmed_up_price(0),
                             warmed_up_volume(0), warmed_up_price(0) * warmed_up_volume(0))));
    ASSERT_TRUE(wait_for_count(kInstrumentA, EventKind::BarUpdate, 1));
    ASSERT_TRUE(
        write_tick(make_tick(kInstrumentA, warmed_up_clock(1), warmed_up_price(1),
                             warmed_up_volume(1), warmed_up_price(1) * warmed_up_volume(1))));
    ASSERT_TRUE(wait_for_count(kInstrumentA, EventKind::BarUpdate, 2));
    stop_strategy();
    EXPECT_EQ(exit_code_, 0);

    const auto events = recorder().for_instrument(kInstrumentA);
    ASSERT_GE(events.size(), 7U) << "callbacks: " << describe_events(events);

    // 首根 bar（索引 0）从未被报告为收盘
    for (const ObservedEvent& event : events) {
        if (event.kind == EventKind::BarClose) {
            EXPECT_GT(event.bar_index, 0)
                << "bar index 0 has no previous bar and must never be closed";
        }
    }
    // 回调序列的第一条 bar 事件是 close(静默填窗最后一根)，其 bar_index 是 max_lookback-1，
    // 不是 0——证明框架没有在首根就合成一条 close
    EXPECT_EQ(events[1].kind, EventKind::BarClose);
    EXPECT_EQ(events[1].bar_index, kTestMaxLookback - 1);
    EXPECT_EQ(events[2].kind, EventKind::BarOpen);
    EXPECT_EQ(events[2].bar_index, kTestMaxLookback);
    // open 之前不存在"属于新根的 close"：close 的 bar_index 恒比随后的 open 小 1
    for (std::size_t i = 0; i + 1 < events.size(); ++i) {
        if (events[i].kind == EventKind::BarClose && events[i + 1].kind == EventKind::BarOpen) {
            EXPECT_EQ(events[i].bar_index + 1, events[i + 1].bar_index)
                << "close must belong to the bar immediately before the open, index " << i;
        }
    }
}

/**
 * **最后一根 bar 不主动收**（AC4）：没有 flush、没有定时器定义 bar 边界——只有下一笔更晚
 * tick 到达时才 close。
 */
TEST_F(BarStrategyFrameworkTest, LastBarIsNotClosedWithoutALaterTick) {
    write_config({std::string(kInstrumentA)}, kTestMaxLookback);
    start_strategy();
    ASSERT_TRUE(wait_for_events(1));
    ASSERT_EQ(warm_up(kInstrumentA), 1U);

    // 回调期第一根：close(31) → open(32) → update；此后这根本身不被关闭
    ASSERT_TRUE(
        write_tick(make_tick(kInstrumentA, warmed_up_clock(0), warmed_up_price(0),
                             warmed_up_volume(0), warmed_up_price(0) * warmed_up_volume(0))));
    ASSERT_TRUE(wait_for_count(kInstrumentA, EventKind::BarUpdate, 1));
    const std::size_t closes_before = recorder().count(kInstrumentA, EventKind::BarClose);
    EXPECT_EQ(closes_before, 1U) << "only the fill-window close so far";

    // 静置：不喂 tick，close 数必须不变（无 flush、无定时器收 bar）。
    //
    // 等待方式是**轮询可观察条件**（close 数是否增长）而不是裸 sleep：唤醒策略线程让它有机会
    // 拿到任何「本该由框架主动产生」的收 bar 事件，一旦真的多出 close 就立刻退出——比固定
    // 睡满一段更能暴露「有事件但来得稍晚」的情形，也不会为了不确定的时序付固定等待。
    const auto quiet_deadline = std::chrono::steady_clock::now() + 200ms;
    while (std::chrono::steady_clock::now() < quiet_deadline &&
           recorder().count(kInstrumentA, EventKind::BarClose) == closes_before) {
        wake_strategy();
        std::this_thread::sleep_for(5ms);
    }
    EXPECT_EQ(recorder().count(kInstrumentA, EventKind::BarClose), closes_before)
        << "a forming bar must never be closed without a later tick";

    // 补一笔更晚 tick：这才产生 close(32) → open(33) → update
    ASSERT_TRUE(
        write_tick(make_tick(kInstrumentA, warmed_up_clock(1), warmed_up_price(1),
                             warmed_up_volume(1), warmed_up_price(1) * warmed_up_volume(1))));
    ASSERT_TRUE(wait_for_count(kInstrumentA, EventKind::BarClose, closes_before + 1));
    ASSERT_TRUE(wait_for_count(kInstrumentA, EventKind::BarUpdate, 2));
    stop_strategy();
    EXPECT_EQ(exit_code_, 0);

    const auto events = recorder().for_instrument(kInstrumentA);
    // 0=init, 1=close(31), 2=open(32), 3=update(32), 4=close(32), 5=open(33), 6=update(33)
    ASSERT_EQ(events.size(), 8U) << "callbacks: " << describe_events(events);
    EXPECT_EQ(events[4].kind, EventKind::BarClose) << "close only after the later tick arrives";
    EXPECT_EQ(events[4].bar_index, kTestMaxLookback);
}

// ============================================================
// 回卷等价性（#158 AC6/AC7）
// ============================================================

/**
 * **回卷两种路径对 B 完全等价**（AC6/AC7）。
 *
 * 同一份 tick 脚本（`kTestMaxLookback` 根静默填窗 + 120 根回调期）跑两轮，两轮的差异
 * **只有回卷来源**：
 *
 * - **轮 1（兜底）**：`period_sec=900 reserve_bars=106`（即 `R_min`）→ `buffer_bars=138`，
 *   回调期推进 120 根必然把 `end` 推满，`advance_bar` 内联搬移。
 * - **轮 2（定时器）**：同一份 tick 脚本 + **显式大余量 192**（`buffer_bars=224`，152 根内
 *   绝不触发兜底）+ 把回卷时刻换到 2 秒后。本轮每次回卷都只能来自
 *   `DZ_FRAME_SCHEDULE` → `on_schedule` → `relocate_live_instruments`。
 *
 * 两种触发的**布局变换是同一个 `InstrumentContext::relocate()`**；本用例断言 B 观察到的
 * 回调序列、`bar_index`、窗口长度、以及与 `price` 序列相关的每根历史值、两条用户序列的
 * 每根历史值**逐字段相同**。回卷只改布局不改值，任何「漏搬某条数组」（典型：只搬价格、
 * 忘了用户序列）都会让两轮在某根上错位。
 */
TEST_F(BarStrategyFrameworkTest, RelocationPathsAreInvisibleToStrategy) {
    const std::int32_t kCallbackBars = 120;  // > R_min(900) = 106，兜底路径必然触发
    const auto tight_reserve = dztrader::series_internal::min_reserve_bars(900);
    // 定时器轮：显式给大余量 32 + 192 = 224 > 32 + 120 = 152，兜底永不触发
    constexpr std::int32_t timer_reserve = 192;
    // 定时器布置在「启动 + 2s」，留 1s 余量用于启动开销与一次 dz_next_event 消费
    constexpr std::chrono::milliseconds kRollbackWait{3000};

    // ── 轮 1：推进前兜底（buffer_bars = 32 + 106 = 138，回调期推进 120 根必然撞上）──
    recorder().reset();
    write_config({std::string(kInstrumentA)}, kTestMaxLookback, tight_reserve, 900);
    start_strategy();
    ASSERT_TRUE(wait_for_events(1));
    BarScriptCursor fallback_cursor;
    ASSERT_EQ(warm_up_900(kInstrumentA, fallback_cursor), 1U);
    feed_900_second_callback_stage(kInstrumentA, fallback_cursor, kCallbackBars);
    ASSERT_TRUE(wait_for_count(kInstrumentA, EventKind::BarUpdate, kCallbackBars));
    stop_strategy();
    ASSERT_EQ(exit_code_, 0);
    const auto fallback_events = callback_stage_events(kInstrumentA);

    // ── 轮 2：18:00 定时器（同一份 tick 脚本；显式给大余量，让兜底路径在 152 根内绝不
    //          触发——本轮的每一次回卷都只能来自定时器，两轮差异就纯粹是回卷来源）──
    recorder().reset();
    write_config({std::string(kInstrumentA)}, kTestMaxLookback, timer_reserve, 900);
    testing::internal::CaptureStderr();
    start_strategy_with_rollback_clock(2);
    ASSERT_TRUE(wait_for_events(1));
    BarScriptCursor timer_cursor;
    ASSERT_EQ(warm_up_900(kInstrumentA, timer_cursor), 1U);
    // 前半段喂完，等定时器真的触发（策略线程必须醒着走 dz_next_event 才消费定时器帧）。
    //
    // 这里是全文件**唯一**必须按流逝时间等待的地方：回卷对 B 完全不可见（纯布局），
    // 没有可轮询的观测量，而触发源就是墙钟定时器。因此按注入的触发时刻算绝对截止时间
    // （`start + 2s` 布置，留 1s 余量），而不是睡满一个拍脑袋的固定时长；期间持续唤醒
    // 策略线程，让它尽早消费 DZ_FRAME_SCHEDULE。
    feed_900_second_callback_stage(kInstrumentA, timer_cursor, kCallbackBars / 2);
    const auto timer_deadline = std::chrono::steady_clock::now() + kRollbackWait;
    while (std::chrono::steady_clock::now() < timer_deadline) {
        wake_strategy();
        std::this_thread::sleep_for(20ms);
    }
    feed_900_second_callback_stage(kInstrumentA, timer_cursor, kCallbackBars - kCallbackBars / 2);
    ASSERT_TRUE(wait_for_count(kInstrumentA, EventKind::BarUpdate, kCallbackBars));
    stop_strategy();
    ASSERT_EQ(exit_code_, 0);
    const std::string stderr_text = testing::internal::GetCapturedStderr();
    // 定时器注册失败会打这行诊断；有它说明本轮只走了兜底路径，等价性比较就退化了
    EXPECT_EQ(stderr_text.find("daily rollback timer unavailable"), std::string::npos)
        << "the rollback timer must register in-process: " << stderr_text;
    // 回卷诊断行是「定时器路径真的执行过」的唯一证据（回卷对 B 不可见）。
    // 本轮 buffer_bars = 32 + 192 = 224 > 32 + 120 = 152，兜底路径不可能触发，
    // 所以这一行只可能来自 on_schedule。
    EXPECT_NE(stderr_text.find("daily rollback |"), std::string::npos)
        << "the 18:00 timer path must actually fire: " << stderr_text;
    const auto timer_events = callback_stage_events(kInstrumentA);

    // ── 逐字段比较：两条路径对 B 的可见面必须完全相同 ──
    ASSERT_EQ(timer_events.size(), fallback_events.size())
        << "relocation must not change the callback sequence";
    for (std::size_t i = 0; i < timer_events.size(); ++i) {
        const ObservedEvent& expected = fallback_events[i];
        const ObservedEvent& actual = timer_events[i];
        EXPECT_EQ(actual.kind, expected.kind) << "event index " << i;
        EXPECT_EQ(actual.bar_index, expected.bar_index) << "event index " << i;
        EXPECT_EQ(actual.window_size, expected.window_size) << "event index " << i;
        EXPECT_DOUBLE_EQ(actual.close, expected.close) << "event index " << i;
        // 全部 7 条价格序列 + 全部用户序列：回卷漏搬任何一条都会在这里不等
        EXPECT_EQ(actual.history, expected.history) << "close history, event index " << i;
        EXPECT_EQ(actual.open_history, expected.open_history) << "open history, event index " << i;
        EXPECT_EQ(actual.high_history, expected.high_history) << "high history, event index " << i;
        EXPECT_EQ(actual.low_history, expected.low_history) << "low history, event index " << i;
        EXPECT_EQ(actual.volume_history, expected.volume_history)
            << "volume history, event index " << i;
        EXPECT_EQ(actual.turnover_history, expected.turnover_history)
            << "turnover history, event index " << i;
        EXPECT_EQ(actual.open_interest_history, expected.open_interest_history)
            << "open_interest history, event index " << i;
        EXPECT_EQ(actual.user_d_history, expected.user_d_history)
            << "user double series, event index " << i;
        EXPECT_EQ(actual.user_i_history, expected.user_i_history)
            << "user int64 series, event index " << i;
    }
}

// ============================================================
// 用户序列的注册时机（#158 AC5：非 on_init 调用必须清晰失败）
// ============================================================

/**
 * **`make_series` 只能在 `on_init` 调用**（组件文档 §4.1「创建」）。
 *
 * 静默填窗已按当时注册的集合跑过，中途新增的序列无法补写历史初值——只会得到一条「前 N 根
 * 是未初始化内存」的序列。框架选择**响亮失败**而不是默默返回残缺序列：`make_series` 抛
 * `std::logic_error`，异常经回调边界被捕获、该实例标记失败、其它品种继续。
 */
TEST_F(BarStrategyFrameworkTest, MakeSeriesOutsideOnInitFailsClearly) {
    write_config({std::string(kInstrumentA), std::string(kInstrumentB)}, kTestMaxLookback);
    recorder().set_fault(FaultPlan{.throw_in_init = {},
                                   .throw_in_bar_open = {},
                                   .throw_in_update = {},
                                   .throw_in_bar_close = {},
                                   .throw_in_destroy = {},
                                   .make_series_in_update = std::string(kInstrumentA)});
    testing::internal::CaptureStderr();
    start_strategy();
    ASSERT_TRUE(wait_for_events(2));

    EXPECT_EQ(warm_up(kInstrumentA), 1U);
    EXPECT_EQ(warm_up(kInstrumentB, 64831), 1U);

    // A 的回调期首根 update 里调 make_series → 抛异常 → 实例失败
    ASSERT_TRUE(
        write_tick(make_tick(kInstrumentA, warmed_up_clock(0), warmed_up_price(0),
                             warmed_up_volume(0), warmed_up_price(0) * warmed_up_volume(0))));
    ASSERT_TRUE(wait_for_count(kInstrumentA, EventKind::BarUpdate, 1));
    // B 健康
    ASSERT_TRUE(
        write_tick(make_tick(kInstrumentB, warmed_up_clock(0, 64831), 400.0, 600, 240000.0)));
    ASSERT_TRUE(wait_for_count(kInstrumentB, EventKind::BarUpdate, 1));
    stop_strategy();
    const std::string stderr_text = testing::internal::GetCapturedStderr();
    EXPECT_EQ(exit_code_, 0) << "a misuse exception must not kill the process";

    // 失败证据：诊断行里带 make_series 的消息与回调名
    EXPECT_NE(stderr_text.find("instance failed |"), std::string::npos) << stderr_text;
    EXPECT_NE(stderr_text.find("callback=on_bar_update"), std::string::npos) << stderr_text;
    EXPECT_NE(stderr_text.find("make_series must be called from on_init"), std::string::npos)
        << "the misuse must name itself in the diagnostic: " << stderr_text;

    // A 彻底静默（失败后不再回调），B 照常
    const auto events_a = recorder().for_instrument(kInstrumentA);
    EXPECT_EQ(events_a.back().kind, EventKind::Destroy) << describe_events(events_a);
    EXPECT_EQ(recorder().count(kInstrumentA, EventKind::BarUpdate), 1U);
    EXPECT_EQ(recorder().count(kInstrumentB, EventKind::BarUpdate), 1U);
}

// ============================================================
// 异常隔离：B 回调抛异常只让该实例失败，其它品种与进程继续
// ============================================================

TEST_F(BarStrategyFrameworkTest, CallbackExceptionFailsOnlyThatInstance) {
    write_config({std::string(kInstrumentA), std::string(kInstrumentB)}, kTestMaxLookback);
    recorder().set_fault(FaultPlan{.throw_in_init = {},
                                   .throw_in_bar_open = {},
                                   .throw_in_update = std::string(kInstrumentA),
                                   .throw_in_bar_close = {},
                                   .throw_in_destroy = {},
                                   .make_series_in_update = {}});
    start_strategy();
    ASSERT_TRUE(wait_for_events(2));

    // 静默填窗（门禁：这段不产生任何回调，故障注入也不会被触发）
    EXPECT_EQ(warm_up(kInstrumentA), 1U);
    EXPECT_EQ(warm_up(kInstrumentB, 64831), 1U);

    // A 的首个回调是 close(31)（成功）→ open(32)（成功）→ update（抛异常）→ A 实例失败
    ASSERT_TRUE(
        write_tick(make_tick(kInstrumentA, warmed_up_clock(0), warmed_up_price(0),
                             warmed_up_volume(0), warmed_up_price(0) * warmed_up_volume(0))));
    ASSERT_TRUE(wait_for_count(kInstrumentA, EventKind::BarUpdate, 1));

    // B 继续正常回调（进程与其它品种不受影响）
    ASSERT_TRUE(
        write_tick(make_tick(kInstrumentB, warmed_up_clock(0, 64831), 400.0, 600, 240000.0)));
    ASSERT_TRUE(wait_for_count(kInstrumentB, EventKind::BarUpdate, 1));
    ASSERT_TRUE(
        write_tick(make_tick(kInstrumentB, warmed_up_clock(1, 64831), 400.5, 610, 244305.0)));
    ASSERT_TRUE(wait_for_count(kInstrumentB, EventKind::BarUpdate, 2));

    // 失败实例的后续 tick 不再产生任何回调；健康实例的 tick 照常
    ASSERT_TRUE(write_tick(make_tick(kInstrumentA, warmed_up_clock(1), 501.0, 700, 350700.0)));
    ASSERT_TRUE(
        write_tick(make_tick(kInstrumentB, warmed_up_clock(2, 64831), 401.0, 620, 248620.0)));
    ASSERT_TRUE(wait_for_count(kInstrumentB, EventKind::BarUpdate, 3));
    stop_strategy();

    EXPECT_EQ(exit_code_, 0) << "process must survive a strategy callback exception";

    const auto events_a = recorder().for_instrument(kInstrumentA);
    // init → close[成功] → open[成功] → update[抛异常] → destroy
    ASSERT_EQ(events_a.size(), 5U)
        << "failed instance must not receive further callbacks: " << describe_events(events_a);
    EXPECT_EQ(events_a[0].kind, EventKind::Init);
    EXPECT_EQ(events_a[1].kind, EventKind::BarClose);
    EXPECT_EQ(events_a[2].kind, EventKind::BarOpen);
    EXPECT_EQ(events_a[3].kind, EventKind::BarUpdate);
    EXPECT_EQ(events_a[4].kind, EventKind::Destroy) << "failed instance still gets on_destroy";

    const auto events_b = recorder().for_instrument(kInstrumentB);
    // init → close(31) → open(32) → update → close(32) → open(33) → update
    //      → close(33) → open(34) → update → destroy = 11
    EXPECT_EQ(events_b.front().kind, EventKind::Init);
    EXPECT_EQ(events_b.back().kind, EventKind::Destroy);
    std::size_t updates_b = 0;
    std::size_t closes_b = 0;
    for (const ObservedEvent& event : events_b) {
        if (event.kind == EventKind::BarUpdate) {
            ++updates_b;
        }
        if (event.kind == EventKind::BarClose) {
            ++closes_b;
        }
    }
    EXPECT_EQ(updates_b, 3U) << "exactly one update per accepted tick on the healthy instance";
    EXPECT_EQ(closes_b, 3U) << "each new callback-stage bar closes the previous one";
}

// ============================================================
// 异常隔离（续）：on_bar_close / on_destroy 抛异常
// ============================================================

/**
 * `on_bar_open` 抛异常同样只失败该实例。
 *
 * 这一路径要单独覆盖：`on_bar_open` 是**窗口已经推进之后**的回调，失败时该实例的
 * `bar_index` 已经前进，且本笔 tick 的 `on_bar_update` 不该再到达 B——与 `on_bar_update`
 * 失败（窗口已写、后续 tick 不通）和 `on_bar_close` 失败（窗口未推进）都不同。
 */
TEST_F(BarStrategyFrameworkTest, BarOpenExceptionFailsOnlyThatInstance) {
    write_config({std::string(kInstrumentA), std::string(kInstrumentB)}, kTestMaxLookback);
    recorder().set_fault(FaultPlan{.throw_in_init = {},
                                   .throw_in_bar_open = std::string(kInstrumentA),
                                   .throw_in_update = {},
                                   .throw_in_bar_close = {},
                                   .throw_in_destroy = {},
                                   .make_series_in_update = {}});
    start_strategy();
    ASSERT_TRUE(wait_for_events(2));

    EXPECT_EQ(warm_up(kInstrumentA), 1U);
    EXPECT_EQ(warm_up(kInstrumentB, 64831), 1U);

    // A 回调期首根 open(32) → 抛异常；本笔 tick 的 update 不应到达 B
    ASSERT_TRUE(
        write_tick(make_tick(kInstrumentA, warmed_up_clock(0), warmed_up_price(0),
                             warmed_up_volume(0), warmed_up_price(0) * warmed_up_volume(0))));
    ASSERT_TRUE(wait_for_count(kInstrumentA, EventKind::BarOpen, 1));

    // B 健康：首根回调 close + open + update
    ASSERT_TRUE(
        write_tick(make_tick(kInstrumentB, warmed_up_clock(0, 64831), 400.0, 600, 240000.0)));
    ASSERT_TRUE(wait_for_count(kInstrumentB, EventKind::BarUpdate, 1));

    // 失败实例的后续 tick 不再回调
    ASSERT_TRUE(write_tick(make_tick(kInstrumentA, warmed_up_clock(1), 501.0, 700, 350700.0)));
    ASSERT_TRUE(
        write_tick(make_tick(kInstrumentB, warmed_up_clock(1, 64831), 400.5, 610, 244305.0)));
    ASSERT_TRUE(wait_for_count(kInstrumentB, EventKind::BarUpdate, 2));
    stop_strategy();

    EXPECT_EQ(exit_code_, 0);

    const auto events_a = recorder().for_instrument(kInstrumentA);
    // init → close(31)[成功] → open(32)[抛异常] → destroy
    ASSERT_EQ(events_a.size(), 4U) << "callbacks: " << describe_events(events_a);
    EXPECT_EQ(events_a[0].kind, EventKind::Init);
    EXPECT_EQ(events_a[1].kind, EventKind::BarClose);
    EXPECT_EQ(events_a[2].kind, EventKind::BarOpen);
    EXPECT_EQ(events_a[3].kind, EventKind::Destroy);
    EXPECT_EQ(recorder().count(kInstrumentA, EventKind::BarUpdate), 0U)
        << "no update after a failed on_bar_open";

    // 健康实例不受影响
    EXPECT_EQ(recorder().count(kInstrumentB, EventKind::BarUpdate), 2U);
}

/**
 * `on_bar_close` 抛异常同样只失败该实例：被关闭的那根 bar 已经走完 open/update，close 只是
 * 收尾通知；失败后该品种不再回调，其它品种照常。
 */
TEST_F(BarStrategyFrameworkTest, BarCloseExceptionFailsOnlyThatInstance) {
    write_config({std::string(kInstrumentA), std::string(kInstrumentB)}, kTestMaxLookback);
    recorder().set_fault(FaultPlan{.throw_in_init = {},
                                   .throw_in_bar_open = {},
                                   .throw_in_update = {},
                                   .throw_in_bar_close = std::string(kInstrumentA),
                                   .throw_in_destroy = {},
                                   .make_series_in_update = {}});
    start_strategy();
    ASSERT_TRUE(wait_for_events(2));

    EXPECT_EQ(warm_up(kInstrumentA), 1U);
    EXPECT_EQ(warm_up(kInstrumentB, 64831), 1U);

    // 回调期首根：close(31)[抛异常]。静默填窗期的 close 不进 B，因此故障点只能在这里触发。
    ASSERT_TRUE(
        write_tick(make_tick(kInstrumentA, warmed_up_clock(0), warmed_up_price(0),
                             warmed_up_volume(0), warmed_up_price(0) * warmed_up_volume(0))));
    ASSERT_TRUE(wait_for_count(kInstrumentA, EventKind::BarClose, 1));

    // 失败后 A 的后续 tick 不再回调；B 健康
    ASSERT_TRUE(write_tick(make_tick(kInstrumentA, warmed_up_clock(1), 501.0, 700, 350700.0)));
    ASSERT_TRUE(
        write_tick(make_tick(kInstrumentB, warmed_up_clock(0, 64831), 400.0, 600, 240000.0)));
    ASSERT_TRUE(wait_for_count(kInstrumentB, EventKind::BarUpdate, 1));
    stop_strategy();

    EXPECT_EQ(exit_code_, 0);

    const auto events_a = recorder().for_instrument(kInstrumentA);
    // init → close(31)[抛异常] → destroy：close 在 open 之前，失败的实例拿不到本根 open
    ASSERT_EQ(events_a.size(), 3U) << "callbacks: " << describe_events(events_a);
    EXPECT_EQ(events_a[1].kind, EventKind::BarClose);
    EXPECT_EQ(events_a[2].kind, EventKind::Destroy) << "failed instance still gets on_destroy";
    // close 抛异常后该实例彻底静默：失败之后只有停止时的 destroy，没有 open/update
    bool failed_seen = false;
    for (const ObservedEvent& event : events_a) {
        if (event.kind == EventKind::BarClose) {
            failed_seen = true;
            continue;
        }
        if (!failed_seen) {
            continue;
        }
        EXPECT_EQ(event.kind, EventKind::Destroy)
            << "no bar callback after the failed on_bar_close";
    }

    const auto events_b = recorder().for_instrument(kInstrumentB);
    EXPECT_EQ(events_b.front().kind, EventKind::Init);
    EXPECT_EQ(events_b.back().kind, EventKind::Destroy);
}

/**
 * `on_destroy` 抛异常不阻止其它实例销毁、也不改变退出码（销毁期不再标记失败）。
 */
TEST_F(BarStrategyFrameworkTest, DestroyExceptionDoesNotStopOtherInstruments) {
    write_config({std::string(kInstrumentA), std::string(kInstrumentB)});
    recorder().set_fault(FaultPlan{.throw_in_init = {},
                                   .throw_in_bar_open = {},
                                   .throw_in_update = {},
                                   .throw_in_bar_close = {},
                                   .throw_in_destroy = std::string(kInstrumentA),
                                   .make_series_in_update = {}});
    start_strategy();
    ASSERT_TRUE(wait_for_events(2));
    stop_strategy();

    EXPECT_EQ(exit_code_, 0) << "on_destroy exception must not change the exit code";

    // 两个品种都收到了 on_destroy：一个抛异常不影响另一个
    EXPECT_EQ(recorder().count(kInstrumentA, EventKind::Destroy), 1U);
    EXPECT_EQ(recorder().count(kInstrumentB, EventKind::Destroy), 1U);
}

// ============================================================
// on_init 抛异常：该品种不加入，其它品种继续
// ============================================================

TEST_F(BarStrategyFrameworkTest, OnInitExceptionSkipsOnlyThatInstrument) {
    write_config({std::string(kInstrumentA), std::string(kInstrumentB)}, kTestMaxLookback);
    recorder().set_fault(FaultPlan{.throw_in_init = std::string(kInstrumentA),
                                   .throw_in_bar_open = {},
                                   .throw_in_update = {},
                                   .throw_in_bar_close = {},
                                   .throw_in_destroy = {},
                                   .make_series_in_update = {}});
    start_strategy();
    ASSERT_TRUE(wait_for_events(2));  // A 的 init（抛异常）+ B 的 init

    EXPECT_EQ(warm_up(kInstrumentA), 1U) << "dropped instrument has no live instance";
    EXPECT_EQ(warm_up(kInstrumentB, 64831), 1U);

    ASSERT_TRUE(
        write_tick(make_tick(kInstrumentB, warmed_up_clock(0, 64831), 400.0, 600, 240000.0)));
    ASSERT_TRUE(wait_for_count(kInstrumentB, EventKind::BarUpdate, 1));
    ASSERT_TRUE(
        write_tick(make_tick(kInstrumentB, warmed_up_clock(1, 64831), 400.5, 610, 244305.0)));
    ASSERT_TRUE(wait_for_count(kInstrumentB, EventKind::BarUpdate, 2));
    stop_strategy();

    EXPECT_EQ(exit_code_, 0);

    const auto events_a = recorder().for_instrument(kInstrumentA);
    ASSERT_EQ(events_a.size(), 1U)
        << "instrument whose on_init threw must not be added: " << describe_events(events_a);
    EXPECT_EQ(events_a[0].kind, EventKind::Init);

    const auto events_b = recorder().for_instrument(kInstrumentB);
    // init → close(31) → open(32) → update → close(32) → open(33) → update → destroy
    ASSERT_EQ(events_b.size(), 8U)
        << "other instruments must keep working: " << describe_events(events_b);
    EXPECT_EQ(events_b[0].kind, EventKind::Init);
    EXPECT_EQ(events_b[1].kind, EventKind::BarClose);
    EXPECT_EQ(events_b[2].kind, EventKind::BarOpen);
    EXPECT_EQ(events_b[2].bar_index, kTestMaxLookback);
    EXPECT_EQ(events_b[3].kind, EventKind::BarUpdate);
    EXPECT_EQ(events_b[4].kind, EventKind::BarClose);
    EXPECT_EQ(events_b[5].kind, EventKind::BarOpen);
    EXPECT_EQ(events_b[5].bar_index, kTestMaxLookback + 1);
    EXPECT_EQ(events_b[6].kind, EventKind::BarUpdate);
    EXPECT_EQ(events_b[7].kind, EventKind::Destroy);
}

// ============================================================
// 交易意图与 Tier 1 的端到端接线（#159；Seam A）
// ============================================================

/**
 * **意图在同一回调内立刻可见，并由 A 的 close 分发驱动 Tier 1 极值更新**（#159 AC）。
 *
 * 单测（`bar_strategy_position_test.cpp`）覆盖台账的逐格口径；这里补的是**接线**：真实帧
 * 通道 + 真实回调顺序下，
 *   1. `on_bar_close` 里做的意图在紧接着的 `on_bar_open` 快照里已经可见（没有被推到下一根）；
 *   2. `on_bar_close` 之前 A 已经用被关闭 bar 的收盘价更新了浮盈极值（组件文档 §6.1
 *      「极值在 bar 收盘前更新」）——B 在 close 里读到的极值包含刚收这根 bar；
 *   3. `bars_since_*` 在三个回调里同 bar = 0、跨 bar 线性增长、无仓 = -1。
 *
 * 脚本按 `bar_index` 精确投放，因此断言不依赖 tick 时序（每个 tick 落到哪根 bar 由
 * `warmed_up_clock` 唯一确定）。
 */
TEST_F(BarStrategyFrameworkTest, IntentLedgerIsWiredIntoRealCallbacks) {
    write_config({std::string(kInstrumentA)}, kTestMaxLookback);
    // 静默填窗最后一根（索引 max_lookback-1）；它的 close 是 B 在回调期的第一次回调，
    // 由回调期第一笔 tick 触发。bar 事件里的索引就是被关闭/打开的那根 bar 自己。
    const auto fill_bar = static_cast<std::int64_t>(kTestMaxLookback - 1);
    const auto bar0 = static_cast<std::int64_t>(kTestMaxLookback);
    const auto bar1 = bar0 + 1;
    const auto bar2 = bar0 + 2;

    const double price_fill = warmed_up_price(-1);  // 静默填窗最后一根的收盘价

    // ① 静默填窗最后一根的 close 里买 2 手，并在同一回调内立刻读（AC 的即时性要求）
    recorder().add_script(ScriptedIntent{
        .instrument = std::string(kInstrumentA),
        .bar_index = fill_bar,
        .when = EventKind::BarClose,
        .apply =
            [price_fill](dztrader::InstrumentContext& ctx) {
                EXPECT_EQ(ctx.current_contracts(), 0) << "flat before the first intent";
                EXPECT_EQ(ctx.bars_since_entry(), -1) << "no entry reference while flat";
                ctx.buy(2, price_fill);
                // 同一回调内立刻可见（不依赖任何回报）
                EXPECT_EQ(ctx.market_position(), 1);
                EXPECT_EQ(ctx.current_contracts(), 2);
                EXPECT_DOUBLE_EQ(ctx.avg_entry_price(), price_fill);
                EXPECT_EQ(ctx.current_entries(), 1);
                EXPECT_EQ(ctx.bars_since_entry(), 0) << "entry on this bar = 0";
                EXPECT_DOUBLE_EQ(ctx.entry_price(), price_fill);
            },
    });
    // ② bar0 的 update 里部分平仓 1 手（均价与段数都不变）
    recorder().add_script(ScriptedIntent{
        .instrument = std::string(kInstrumentA),
        .bar_index = bar0,
        .when = EventKind::BarUpdate,
        .apply =
            [](dztrader::InstrumentContext& ctx) {
                EXPECT_EQ(ctx.current_contracts(), 2) << "the intent from the previous close";
                EXPECT_EQ(ctx.bars_since_entry(), 1) << "one bar after the entry";
                const double close = ctx.close[0];
                ctx.sell(1, close);  // 部分平仓
                EXPECT_EQ(ctx.current_contracts(), 1);
                EXPECT_EQ(ctx.current_entries(), 1) << "partial close keeps the segment count";
                EXPECT_EQ(ctx.bars_since_exit(), 0) << "closed on this bar = 0";
                EXPECT_DOUBLE_EQ(ctx.exit_price(), close);
            },
    });
    // ③ bar1 的 close 里全平（volume = 0 是模式值）
    recorder().add_script(ScriptedIntent{
        .instrument = std::string(kInstrumentA),
        .bar_index = bar1,
        .when = EventKind::BarClose,
        .apply =
            [](dztrader::InstrumentContext& ctx) {
                EXPECT_EQ(ctx.current_contracts(), 1);
                ctx.sell(0, ctx.close[0]);
                EXPECT_EQ(ctx.market_position(), 0);
                EXPECT_EQ(ctx.current_contracts(), 0);
                EXPECT_EQ(ctx.current_entries(), 0) << "flat position has no entry segments";
                EXPECT_EQ(ctx.bars_since_entry(), -1) << "no entry reference once flat";
                EXPECT_EQ(ctx.bars_since_exit(), 0) << "the exit reference survives going flat";
                EXPECT_GE(ctx.max_contracts(), 2) << "historical peak survives going flat";
            },
    });

    testing::internal::CaptureStderr();
    start_strategy();
    ASSERT_TRUE(wait_for_events(1));
    ASSERT_EQ(warm_up(kInstrumentA), 1U);

    // 四笔 tick → 四根回调期 bar 打开；第 4 笔才关闭 bar2（纯 tick 驱动，无 flush）
    for (std::int32_t i = 0; i <= 3; ++i) {
        ASSERT_TRUE(
            write_tick(make_tick(kInstrumentA, warmed_up_clock(i), warmed_up_price(i),
                                 warmed_up_volume(i), warmed_up_price(i) * warmed_up_volume(i))));
    }
    ASSERT_TRUE(wait_for_count(kInstrumentA, EventKind::BarUpdate, 4));
    stop_strategy();
    ASSERT_EQ(exit_code_, 0);
    const std::string stderr_text = testing::internal::GetCapturedStderr();
    // Tier 1 盈亏的乘数来源缺失必须**说出来**（静默 0 会让止损逻辑永不触发）
    EXPECT_NE(stderr_text.find("instrument metadata unavailable |"), std::string::npos)
        << "the missing metadata source must be announced: " << stderr_text;
    EXPECT_NE(stderr_text.find("ticket=161"), std::string::npos) << stderr_text;

    const auto events = recorder().for_instrument(kInstrumentA);
    ASSERT_GE(events.size(), 10U) << "callbacks: " << describe_events(events);

    // 按 (bar_index, kind) 取快照。close 事件的 bar_index 就是被关闭的那根 bar。
    auto find_event = [&events](EventKind kind, std::int64_t bar_index) -> const ObservedEvent* {
        for (const ObservedEvent& event : events) {
            if (event.kind == kind && event.bar_index == bar_index) {
                return &event;
            }
        }
        return nullptr;
    };

    const ObservedEvent* close_fill = find_event(EventKind::BarClose, fill_bar);
    // 打开的新根是 bar0：close(fill_bar) 的 bar_index 是**被关**的那根，open 的是**新**那根
    const ObservedEvent* open_fill = find_event(EventKind::BarOpen, bar0);
    const ObservedEvent* update0 = find_event(EventKind::BarUpdate, bar0);
    const ObservedEvent* close1 = find_event(EventKind::BarClose, bar1);
    const ObservedEvent* close2 = find_event(EventKind::BarClose, bar2);
    ASSERT_NE(close_fill, nullptr) << "fill-window close missing";
    ASSERT_NE(open_fill, nullptr) << "fill-window open missing";
    ASSERT_NE(update0, nullptr) << "bar0 update missing";
    ASSERT_NE(close1, nullptr) << "bar1 close missing";
    ASSERT_NE(close2, nullptr) << "bar2 close missing";

    // ① 即时性：close_fill 的快照在脚本之前（空仓），紧随的 open_fill 已是多头 2 手
    EXPECT_EQ(close_fill->current_contracts, 0) << "recorded before the script ran";
    EXPECT_EQ(open_fill->market_position, 1) << "the intent is visible in the very next callback";
    EXPECT_EQ(open_fill->current_contracts, 2);
    EXPECT_DOUBLE_EQ(open_fill->avg_entry_price, price_fill);
    EXPECT_EQ(open_fill->current_entries, 1);
    // 意图发生在 fill_bar 的 close 里（那一格快照的 bars_since_entry 已在脚本内断言为 0）；
    // 到新根 open 时 bar_index 已 +1，所以这里是 1——两格之差正是「推进发生过」的证据。
    EXPECT_EQ(open_fill->bars_since_entry, 1) << "one bar after the entry bar";
    // 进场时间 = **该 bar 的标准结束时间**（begin_sec + period_sec），不是 tick 时间。
    // tick 脚本把 tick 落在桶起点（clock = 起点），所以若实现误取 tick 时间，这里会差 60s。
    const std::int32_t fill_tsec = dztrader::bar::TradingTime::from_clock(warmed_up_clock(-1));
    const std::int32_t fill_begin = fill_tsec - fill_tsec % 60;
    EXPECT_EQ(open_fill->entry_date, kTradingDay);
    EXPECT_EQ(open_fill->entry_time, dztrader::bar::TradingTime::to_clock(fill_begin + 60))
        << "entry time is the bar's standard end time, not the tick time";

    // ② 极值/盈亏在框架路径上**目前恒为 0**：`volume_multiple` 来自合约元数据源，而那是
    //    #161 的内部依赖缝（v1 尚未落地）。这里把这个已知限制**正面锁死**，而不是留一个
    //    「反正都是 0」的空断言：一旦 #161 接上元数据源，本断言会失败并强制改为按乘数核对
    //    （同时启动诊断里那行 `instrument metadata unavailable` 也该消失）。
    //    极值「在 bar 收盘前更新」的**公式与更新时机**由单元测试
    //    `bar_strategy_position_test.cpp` 用注入的 `InstrumentSpec` 覆盖。
    EXPECT_DOUBLE_EQ(close1->max_position_profit, 0.0)
        << "without a metadata source the multiplier is 0, so profit stays 0 (known gap, #161)";
    // 部分平仓后均价不变（只有 exit_* 更新）
    EXPECT_DOUBLE_EQ(close1->avg_entry_price, price_fill);

    // ③ 平仓后 Tier 1 归零 / 无参考：全平脚本在 bar1 的 close 里执行，因此 bar2 的 close
    //    快照（以及其后的一切）已经是空仓态
    EXPECT_EQ(close2->current_contracts, 0) << "flat since the bar1 close";
    EXPECT_EQ(close2->current_entries, 0);
    EXPECT_EQ(close2->bars_since_entry, -1);
    EXPECT_EQ(close2->max_position_profit, 0.0) << "extremes reset when flat";
    EXPECT_EQ(close2->max_position_loss, 0.0);
    const ObservedEvent* final_update = find_event(EventKind::BarUpdate, bar2 + 1);
    ASSERT_NE(final_update, nullptr) << "the bar after the flat intent is missing";
    EXPECT_EQ(final_update->current_contracts, 0);
    EXPECT_EQ(final_update->bars_since_entry, -1);
    EXPECT_EQ(final_update->max_position_profit, 0.0) << "extremes reset when flat";
    EXPECT_EQ(final_update->max_position_loss, 0.0);
}

/**
 * **失败实例拒单且不影响其它品种**（#159 AC「失败实例不再受理下单意图」）。
 *
 * 与上一条同一接线，但让 A 在 `on_bar_update` 抛异常（实例失败）后继续投放意图脚本：
 * 脚本里的 `current_contracts()` 必须停在被保留的逻辑持仓上——失败实例的逻辑持仓保留、
 * 但不再受理任何意图（组件文档 §4.6）。
 */
TEST_F(BarStrategyFrameworkTest, FailedInstanceStopsAcceptingIntents) {
    write_config({std::string(kInstrumentA), std::string(kInstrumentB)}, kTestMaxLookback);
    const auto fill_bar = static_cast<std::int64_t>(kTestMaxLookback - 1);

    // 静默填窗最后一根的 close（B 的第一个回调，先于同笔 tick 的 open/update）里建多 2 手；
    // 紧接着的 update 注入异常 → 实例失败。因此「失败前的意图已生效」与「失败后不再收
    // 回调」两条都能在同一笔 tick 上观察。
    recorder().add_script(ScriptedIntent{
        .instrument = std::string(kInstrumentA),
        .bar_index = fill_bar,
        .when = EventKind::BarClose,
        .apply = [](dztrader::InstrumentContext& ctx) { ctx.buy(2, ctx.close[0]); },
    });

    recorder().set_fault(FaultPlan{.throw_in_init = {},
                                   .throw_in_bar_open = {},
                                   .throw_in_update = std::string(kInstrumentA),
                                   .throw_in_bar_close = {},
                                   .throw_in_destroy = {},
                                   .make_series_in_update = {}});
    start_strategy();
    ASSERT_TRUE(wait_for_events(2));
    EXPECT_EQ(warm_up(kInstrumentA), 1U);
    EXPECT_EQ(warm_up(kInstrumentB, 64831), 1U);

    // 同一笔 tick：close（脚本建多）→ open → update（抛异常 → 实例失败）
    ASSERT_TRUE(
        write_tick(make_tick(kInstrumentA, warmed_up_clock(0), warmed_up_price(0),
                             warmed_up_volume(0), warmed_up_price(0) * warmed_up_volume(0))));
    ASSERT_TRUE(wait_for_count(kInstrumentA, EventKind::BarUpdate, 1));
    // B 照常运行（失败隔离）
    ASSERT_TRUE(
        write_tick(make_tick(kInstrumentB, warmed_up_clock(0, 64831), 400.0, 600, 240000.0)));
    ASSERT_TRUE(wait_for_count(kInstrumentB, EventKind::BarUpdate, 1));
    stop_strategy();
    EXPECT_EQ(exit_code_, 0) << "process must survive a failed instance";

    const auto events_a = recorder().for_instrument(kInstrumentA);
    // 失败实例：init → close（脚本建多）→ open → update（抛异常）→ destroy。
    // 失败后不再收任何回调，所以后续 tick 既不产生事件也不执行脚本。
    EXPECT_EQ(events_a.size(), 5U) << "callbacks: " << describe_events(events_a);
    ASSERT_EQ(events_a.size(), 5U);
    EXPECT_EQ(events_a[0].kind, EventKind::Init);
    EXPECT_EQ(events_a[1].kind, EventKind::BarClose) << "the fill-window close carries the intent";
    EXPECT_EQ(events_a[1].current_contracts, 0) << "recorded before the script ran";
    EXPECT_EQ(events_a[2].kind, EventKind::BarOpen);
    EXPECT_EQ(events_a[2].current_contracts, 2) << "the successful intent before the failure";
    EXPECT_EQ(events_a[2].market_position, 1);
    EXPECT_EQ(events_a[3].kind, EventKind::BarUpdate) << "this callback throws";
    EXPECT_EQ(events_a[4].kind, EventKind::Destroy);
    // 框架不自动平仓：失败实例的逻辑持仓保留（组件文档 §4.6）
    EXPECT_EQ(events_a[4].current_contracts, 2) << "a failed instance keeps its position";
}

// ============================================================
// 挂靠缩放 / 差量单 / 逻辑持仓上报（#160 Seam A）
// ============================================================

/**
 * **两个挂靠账户按比率镜像，反手只发一张差量单**（#160 的核心 AC）。
 *
 * 配置 scale 1.0（CTP001）与 0.5（CTP002），策略买 3 手：`L` 分别是 3 与 2（0.5 × 3 = 1.5 →
 * nearest 2），因此两张买单 3 手 / 2 手，上报 3 / 2。
 *
 * 随后反手卖 3 手（`sell_short(3)`，P 从 +3 直接到 -3）：
 *   - `qty₁ = -3 - 3 = -6`、`qty₂ = -2 - 2 = -4`——每个账户**只有一张**卖单（框架不拆平/开）；
 *   - effect 恒 `AUTO`、价格就是策略传入的限价；
 *   - 上报跟着变到 -3 / -2。
 *
 * 这也同时锁住「反手是一张差量单」与「上报只在 `L` 变化时」两条：若框架按「先平后开」拆单，
 * 账户会看到 4 张单而不是 2 张。
 */
TEST_F(BarStrategyFrameworkTest, AttachedAccountsMirrorAndReverseWithSingleDeltaOrder) {
    write_config(
        {std::string(kInstrumentA)}, kTestMaxLookback,
        /*reserve_bars=*/std::nullopt, /*period_sec=*/60,
        /*accounts_json=*/
        R"([{"account_id": "CTP001", "scale": 1.0}, {"account_id": "CTP002", "scale": 0.5}])");
    const auto fill_bar = static_cast<std::int64_t>(kTestMaxLookback - 1);
    const auto bar0 = static_cast<std::int64_t>(kTestMaxLookback);
    const double fill_price = 5000.0;
    const double reverse_price = 5010.0;

    // 静默填窗最后一根的 close 里买 3 手；bar0 的 update 里反手卖 3 手。
    // 两笔都用显式限价（不是 tick 价），这样订单帧的价格断言能区分「策略传入价」与「行情价」。
    recorder().add_script(ScriptedIntent{
        .instrument = std::string(kInstrumentA),
        .bar_index = fill_bar,
        .when = EventKind::BarClose,
        .apply = [fill_price](dztrader::InstrumentContext& ctx) { ctx.buy(3, fill_price); },
    });
    recorder().add_script(ScriptedIntent{
        .instrument = std::string(kInstrumentA),
        .bar_index = bar0,
        .when = EventKind::BarUpdate,
        .apply =
            [reverse_price](dztrader::InstrumentContext& ctx) {
                EXPECT_EQ(ctx.market_position(), 1) << "still long before the reversal";
                ctx.sell_short(3, reverse_price);
                EXPECT_EQ(ctx.market_position(), -1)
                    << "the reversal is immediate in the logical ledger";
            },
    });

    open_probe();
    start_strategy();
    ASSERT_TRUE(wait_for_events(1));
    ASSERT_EQ(warm_up(kInstrumentA), 1U);

    // 回调期第一根 bar：close(fill) → open(bar0) → update(bar0，反手)
    ASSERT_TRUE(
        write_tick(make_tick(kInstrumentA, warmed_up_clock(0), warmed_up_price(0),
                             warmed_up_volume(0), warmed_up_price(0) * warmed_up_volume(0))));
    ASSERT_TRUE(wait_for_count(kInstrumentA, EventKind::BarUpdate, 1));
    stop_strategy();
    EXPECT_EQ(exit_code_, 0);
    collect_order_and_report_frames();

    // ── 订单帧：2 张买单 + 2 张卖单（每账户每笔意图恰好一张）──
    ASSERT_EQ(orders_.size(), 4U) << "one delta order per account per intent; got "
                                  << orders_.size();
    const ObservedOrder& buy1 = orders_[0];
    const ObservedOrder& buy2 = orders_[1];
    const ObservedOrder& sell1 = orders_[2];
    const ObservedOrder& sell2 = orders_[3];

    // 建多：账户手数 = L_i(P=+3)，scale 1.0 → 3、scale 0.5 → 2（1.5 四舍五入）
    EXPECT_EQ(buy1.account_id, "CTP001");
    EXPECT_EQ(buy1.instrument_id, kInstrumentA);
    EXPECT_EQ(buy1.direction, DZ_DIRECTION_LONG);
    EXPECT_EQ(buy1.volume, 3);
    EXPECT_DOUBLE_EQ(buy1.price, fill_price);
    EXPECT_EQ(buy1.effect, DZ_POSITION_EFFECT_AUTO) << "effect is always AUTO";

    EXPECT_EQ(buy2.account_id, "CTP002");
    EXPECT_EQ(buy2.direction, DZ_DIRECTION_LONG);
    EXPECT_EQ(buy2.volume, 2) << "L = 0.5 * 3 = 1.5 rounds to 2";
    EXPECT_DOUBLE_EQ(buy2.price, fill_price);

    // 反手：每账户**一张**卖单，手数 = L(-3) - L(+3) 的幅值（6 与 4），不是「先平 3 再开 3」
    EXPECT_EQ(sell1.account_id, "CTP001");
    EXPECT_EQ(sell1.direction, DZ_DIRECTION_SHORT);
    EXPECT_EQ(sell1.volume, 6) << "a reversal is a single delta order (-3 - 3 = -6)";
    EXPECT_DOUBLE_EQ(sell1.price, reverse_price);
    EXPECT_EQ(sell1.effect, DZ_POSITION_EFFECT_AUTO);

    EXPECT_EQ(sell2.account_id, "CTP002");
    EXPECT_EQ(sell2.direction, DZ_DIRECTION_SHORT);
    EXPECT_EQ(sell2.volume, 4) << "L(-3) - L(+3) = -2 - 2 = -4";
    EXPECT_DOUBLE_EQ(sell2.price, reverse_price);

    // ── 上报帧：4 条，账户与净手数按 L 一一对应 ──
    ASSERT_EQ(reports_.size(), 4U) << "one report per changed L_i per intent";
    EXPECT_EQ(reports_[0].account_id, "CTP001");
    EXPECT_EQ(reports_[0].instrument_id, kInstrumentA);
    EXPECT_EQ(reports_[0].net_volume, 3);
    EXPECT_EQ(reports_[1].account_id, "CTP002");
    EXPECT_EQ(reports_[1].net_volume, 2);
    EXPECT_EQ(reports_[2].account_id, "CTP001");
    EXPECT_EQ(reports_[2].net_volume, -3);
    EXPECT_EQ(reports_[3].account_id, "CTP002");
    EXPECT_EQ(reports_[3].net_volume, -2);
}

/**
 * **配置里的取整策略与 `scale_min_one` 真的到达执行路径**（AC「三种取整策略正确，
 * `scale_min_one` 生效」的接线面）。
 *
 * 单元测试断言的是 `logical_position()` 的三种策略本身；但它们可能正确、而框架仍然用默认
 * `nearest` 跑（配置解析 → `EffectiveConfig::scaling` → `InstrumentInit::scaling` → ctx 这条
 * 链上有四个可以断开的地方，每一处断开都不会编译失败）。这里用 `floor` + `scale_min_one`
 * 走完整链路：`P = 3`、scale 0.5 时
 *   - `nearest` → `L = 2`（四舍五入，也是默认值——用默认值断不出接线错误）；
 *   - `floor`   → `L = 1`（向少）；
 *   - `floor + scale_min_one` 且 `0.5 × 1 = 0.5 → floor 0` → 抬到 **1**。
 *
 * 三者里只有 `floor` 会给出 1，所以「订单手数是 1」同时证明取整策略不是默认的 `nearest`；
 * 再用一笔 `P = 1`（`0.5 → floor 0 → 抬 1`）证明 `scale_min_one` 也接上了——若 `min_one`
 * 没传下去，该笔的 `L` 会是 0、完全不发单。
 */
TEST_F(BarStrategyFrameworkTest, ConfiguredRoundingAndMinOneReachExecution) {
    write_config({std::string(kInstrumentA)}, kTestMaxLookback,
                 /*reserve_bars=*/std::nullopt, /*period_sec=*/60,
                 /*accounts_json=*/R"([{"account_id": "CTP001", "scale": 0.5}])",
                 /*scaling_json=*/
                 "  \"scale_rounding\": \"floor\",\n  \"scale_min_one\": true,\n");
    const auto fill_bar = static_cast<std::int64_t>(kTestMaxLookback - 1);
    const auto bar0 = static_cast<std::int64_t>(kTestMaxLookback);

    // P: 0 → 3（floor(0.5 × 3) = 1，nearest 会给 2）
    recorder().add_script(ScriptedIntent{
        .instrument = std::string(kInstrumentA),
        .bar_index = fill_bar,
        .when = EventKind::BarClose,
        .apply = [](dztrader::InstrumentContext& ctx) { ctx.buy(3, 4500.0); },
    });
    // P: 3 → 4（floor(0.5 × 4) = 2，所以差量 1 手）
    recorder().add_script(ScriptedIntent{
        .instrument = std::string(kInstrumentA),
        .bar_index = bar0,
        .when = EventKind::BarUpdate,
        .apply = [](dztrader::InstrumentContext& ctx) { ctx.buy(1, 4501.0); },
    });

    open_probe();
    testing::internal::CaptureStderr();
    start_strategy();
    ASSERT_TRUE(wait_for_events(1));
    ASSERT_EQ(warm_up(kInstrumentA), 1U);
    ASSERT_TRUE(
        write_tick(make_tick(kInstrumentA, warmed_up_clock(0), warmed_up_price(0),
                             warmed_up_volume(0), warmed_up_price(0) * warmed_up_volume(0))));
    ASSERT_TRUE(wait_for_count(kInstrumentA, EventKind::BarUpdate, 1));
    stop_strategy();
    const std::string stderr_text = testing::internal::GetCapturedStderr();
    EXPECT_EQ(exit_code_, 0);
    collect_order_and_report_frames();

    // 启动诊断必须把生效的取整策略与 min_one 说出来（运维唯一的证据来源）
    EXPECT_NE(stderr_text.find("account fanout |"), std::string::npos) << stderr_text;
    EXPECT_NE(stderr_text.find("scale_rounding=floor"), std::string::npos) << stderr_text;
    EXPECT_NE(stderr_text.find("scale_min_one=1"), std::string::npos) << stderr_text;

    // 两笔意图各一张单：3 手 → L=1、4 手 → L=2（差量 1）
    ASSERT_EQ(orders_.size(), 2U) << "floor(0.5 * 3) = 1 and floor(0.5 * 4) = 2";
    EXPECT_EQ(orders_[0].volume, 1) << "nearest would have sent 2 lots (0.5 * 3 = 1.5)";
    EXPECT_EQ(orders_[1].volume, 1) << "the delta from L=1 to L=2 is one lot";

    ASSERT_EQ(reports_.size(), 2U);
    EXPECT_EQ(reports_[0].net_volume, 1);
    EXPECT_EQ(reports_[1].net_volume, 2);
}

/**
 * **`scale_min_one` 单独生效**：`floor` + `min_one`、`P = 1`、scale **0.1** 时
 * `floor(0.1) = 0` → 抬到 1。若 `min_one` 没传下去，该账户的 `L` 恒为 0、**一帧都不发**。
 *
 * scale 取 0.1 而不是 0.5：`nearest(0.5) = 1` 恰好与 min_one 的抬升结果相同，那样的输入
 * 在「min_one 没接线」的变异下仍然通过，断不出接线错误（本用例首版即踩了这一格）。
 */
TEST_F(BarStrategyFrameworkTest, ScaleMinOneIsWiredThroughConfig) {
    write_config({std::string(kInstrumentA)}, kTestMaxLookback,
                 /*reserve_bars=*/std::nullopt, /*period_sec=*/60,
                 /*accounts_json=*/R"([{"account_id": "CTP001", "scale": 0.1}])",
                 /*scaling_json=*/
                 "  \"scale_rounding\": \"floor\",\n  \"scale_min_one\": true,\n");
    const auto fill_bar = static_cast<std::int64_t>(kTestMaxLookback - 1);
    recorder().add_script(ScriptedIntent{
        .instrument = std::string(kInstrumentA),
        .bar_index = fill_bar,
        .when = EventKind::BarClose,
        // 1 手：floor(0.1 × 1) = 0，只有 min_one 能把它抬到 1
        .apply = [](dztrader::InstrumentContext& ctx) { ctx.buy(1, 4500.0); },
    });

    open_probe();
    start_strategy();
    ASSERT_TRUE(wait_for_events(1));
    ASSERT_EQ(warm_up(kInstrumentA), 1U);
    ASSERT_TRUE(
        write_tick(make_tick(kInstrumentA, warmed_up_clock(0), warmed_up_price(0),
                             warmed_up_volume(0), warmed_up_price(0) * warmed_up_volume(0))));
    ASSERT_TRUE(wait_for_count(kInstrumentA, EventKind::BarUpdate, 1));
    stop_strategy();
    EXPECT_EQ(exit_code_, 0);
    collect_order_and_report_frames();

    ASSERT_EQ(orders_.size(), 1U)
        << "without min_one this account would stay at 0 and send nothing";
    EXPECT_EQ(orders_[0].volume, 1);
    ASSERT_EQ(reports_.size(), 1U);
    EXPECT_EQ(reports_[0].net_volume, 1);
}

/**
 * **`qty_i == 0` 的账户既不収单也不上报**（AC「`qty_i == 0` 时跳过发单与上报」）。
 *
 * scale 0.1 + nearest：策略 3 手 → `L = 0.3 → 0`；4 手 → `0.4 → 0`；5 手 → `0.5 → 1`。
 * 因此前两笔意图对该账户的 `L` 都是 0（差量 0）→ 零帧；第三笔才出现一张 1 手单。
 *
 * 这条正是「取整落在目标持仓上」的可观测后果：若框架按单量取整，每笔 3/4/5 手都会算出 0
 * 手单（最终持仓 0，与 `L(5) = 1` 不符）。
 */
TEST_F(BarStrategyFrameworkTest, AccountsWithUnchangedLogicalPositionAreSkipped) {
    write_config({std::string(kInstrumentA)}, kTestMaxLookback,
                 /*reserve_bars=*/std::nullopt, /*period_sec=*/60,
                 /*accounts_json=*/R"([{"account_id": "TINY", "scale": 0.1}])");
    const auto fill_bar = static_cast<std::int64_t>(kTestMaxLookback - 1);
    const auto bar0 = static_cast<std::int64_t>(kTestMaxLookback);
    const auto bar1 = bar0 + 1;

    recorder().add_script(ScriptedIntent{
        .instrument = std::string(kInstrumentA),
        .bar_index = fill_bar,
        .when = EventKind::BarClose,
        .apply = [](dztrader::InstrumentContext& ctx) { ctx.buy(3, 4000.0); },  // L = 0
    });
    recorder().add_script(ScriptedIntent{
        .instrument = std::string(kInstrumentA),
        .bar_index = bar0,
        .when = EventKind::BarUpdate,
        .apply = [](dztrader::InstrumentContext& ctx) { ctx.buy(1, 4001.0); },  // +1 → 4 手, L = 0
    });
    recorder().add_script(ScriptedIntent{
        .instrument = std::string(kInstrumentA),
        .bar_index = bar1,
        .when = EventKind::BarUpdate,
        .apply = [](dztrader::InstrumentContext& ctx) { ctx.buy(1, 4002.0); },  // 5 手, L = 1
    });

    open_probe();
    start_strategy();
    ASSERT_TRUE(wait_for_events(1));
    ASSERT_EQ(warm_up(kInstrumentA), 1U);
    for (std::int32_t i = 0; i <= 1; ++i) {
        ASSERT_TRUE(
            write_tick(make_tick(kInstrumentA, warmed_up_clock(i), warmed_up_price(i),
                                 warmed_up_volume(i), warmed_up_price(i) * warmed_up_volume(i))));
    }
    ASSERT_TRUE(wait_for_count(kInstrumentA, EventKind::BarUpdate, 2));
    stop_strategy();
    EXPECT_EQ(exit_code_, 0);
    collect_order_and_report_frames();

    // 只有第三笔（P 到 5，L 到 1）产生帧
    ASSERT_EQ(orders_.size(), 1U) << "unchanged L_i must not produce an order";
    EXPECT_EQ(orders_[0].account_id, "TINY");
    EXPECT_EQ(orders_[0].volume, 1) << "L(5) = 1 while L(4) = L(3) = 0, so the delta is 1 lot";
    ASSERT_EQ(reports_.size(), 1U) << "unchanged L_i must not produce a report";
    EXPECT_EQ(reports_[0].net_volume, 1);
}

/**
 * **0 个挂靠账户：只记账、不发单、不上报**（AC「0 个挂靠账户路径仍正确」）。
 *
 * 这条路径在本票之前就存在（ctx 内没有账户表），现在账户表由 A 注入，因此「空表」必须仍然
 * 完全不产生帧——一个 `accounts_.empty()` 判断写漏就会变成「遍历 0 次」的正确行为，但
 * 「指针非空、表空」与「指针为空」是两条不同的分支，值得正面锁一次。
 */
TEST_F(BarStrategyFrameworkTest, ZeroAttachedAccountsProduceNoFrames) {
    write_config({std::string(kInstrumentA)}, kTestMaxLookback);  // accounts: []
    const auto fill_bar = static_cast<std::int64_t>(kTestMaxLookback - 1);
    recorder().add_script(ScriptedIntent{
        .instrument = std::string(kInstrumentA),
        .bar_index = fill_bar,
        .when = EventKind::BarClose,
        .apply = [](dztrader::InstrumentContext& ctx) { ctx.buy(5, 3000.0); },
    });

    open_probe();
    start_strategy();
    ASSERT_TRUE(wait_for_events(1));
    ASSERT_EQ(warm_up(kInstrumentA), 1U);
    ASSERT_TRUE(
        write_tick(make_tick(kInstrumentA, warmed_up_clock(0), warmed_up_price(0),
                             warmed_up_volume(0), warmed_up_price(0) * warmed_up_volume(0))));
    ASSERT_TRUE(wait_for_count(kInstrumentA, EventKind::BarUpdate, 1));
    stop_strategy();
    EXPECT_EQ(exit_code_, 0);
    collect_order_and_report_frames();

    EXPECT_TRUE(orders_.empty()) << "a strategy with no attached accounts must not trade";
    EXPECT_TRUE(reports_.empty()) << "nothing to report without attached accounts";

    // 逻辑台账照常推进（「先纸上跑通策略」的口径）
    const auto events = recorder().for_instrument(kInstrumentA);
    const ObservedEvent* open_event = nullptr;
    for (const ObservedEvent& event : events) {
        if (event.kind == EventKind::BarOpen && event.bar_index == kTestMaxLookback) {
            open_event = &event;
        }
    }
    ASSERT_NE(open_event, nullptr);
    EXPECT_EQ(open_event->current_contracts, 5) << "the virtual ledger still advances";
}

/**
 * **`on_init` 期的意图只做虚拟成交、不发单不上报**（AC「下单只有单一出口」的 live 门禁面）。
 *
 * `on_init` 里下单是合法用法（先按参数建立初始逻辑持仓），但那时实例还没进入 live：不能发单，
 * 否则启动阶段就会向账户送出策略尚未准备完的单子。这里在 `on_init` 买 2 手，断言零帧；随后
 * 回调期的意图才产出帧——两条合起来才是「门禁真的在按阶段切换」的正面证据（只断言零帧的话，
 * 「门禁恒关」也满足）。
 */
TEST_F(BarStrategyFrameworkTest, OnInitIntentIsVirtualOnlyUntilLive) {
    write_config({std::string(kInstrumentA)}, kTestMaxLookback,
                 /*reserve_bars=*/std::nullopt, /*period_sec=*/60,
                 /*accounts_json=*/R"([{"account_id": "CTP001", "scale": 1.0}])");

    // on_init 里建多 2 手：bar_index == -1 是 on_init 的唯一判据
    recorder().add_script(ScriptedIntent{
        .instrument = std::string(kInstrumentA),
        .bar_index = -1,
        .when = EventKind::Init,
        .apply = [](dztrader::InstrumentContext& ctx) { ctx.buy(2, 2000.0); },
    });

    open_probe();
    start_strategy();
    ASSERT_TRUE(wait_for_events(1));

    // 静默填窗：B 零回调，因此没有任何新的意图 → 帧里只可能有 on_init 那笔（且它必须没发）
    ASSERT_EQ(warm_up(kInstrumentA), 1U);
    stop_strategy();
    EXPECT_EQ(exit_code_, 0);
    collect_order_and_report_frames();

    EXPECT_TRUE(orders_.empty()) << "an on_init intent must not reach the accounts";
    // 上报则**必须**发生：进入 live 时对台账当前值做一次批量上报（先上报再放开）。`on_init`
    // 已建 2 手，因此这里报的是 2——硬编码「启动时逻辑持仓为 0」会让这种策略在 UI 上显示空仓。
    ASSERT_EQ(reports_.size(), 1U) << "the live entry must publish the current ledger position";
    EXPECT_EQ(reports_[0].account_id, "CTP001");
    EXPECT_EQ(reports_[0].instrument_id, kInstrumentA);
    EXPECT_EQ(reports_[0].net_volume, 2) << "the 2 lots built in on_init must be reported";
    // 逻辑台账照样推进（「先纸上跑通策略」）
    EXPECT_EQ(recorder().count(kInstrumentA, EventKind::BarUpdate), 0U)
        << "no callbacks yet: the window is not full";
}

/**
 * **价格被调整时记诊断**（AC「价格处理：tick 对齐 / 涨跌停夹取，调整记诊断」）。
 *
 * v1 没有元数据来源（`price_tick == 0`），因此 tick 对齐被跳过；但涨跌停来自 tick 本身，
 * 是真实可用的输入。这里喂一笔带涨跌停的行情，在策略里以高于涨停的限价买入，断言：
 *   - 订单帧的价被夹到涨停价（**挂单而不是拒单**）；
 *   - stderr 出现 `order price adjusted` 与 `reason=limit_up`。
 */
TEST_F(BarStrategyFrameworkTest, OrderPriceIsClampedToLimitAndDiagnosed) {
    write_config({std::string(kInstrumentA)}, kTestMaxLookback,
                 /*reserve_bars=*/std::nullopt, /*period_sec=*/60,
                 /*accounts_json=*/R"([{"account_id": "CTP001", "scale": 1.0}])");
    const auto fill_bar = static_cast<std::int64_t>(kTestMaxLookback - 1);
    constexpr double kUpperLimit = 110.0;

    recorder().add_script(ScriptedIntent{
        .instrument = std::string(kInstrumentA),
        .bar_index = fill_bar,
        .when = EventKind::BarClose,
        // 远高于涨停的限价：必须被夹到 110，而不是拒单
        .apply = [](dztrader::InstrumentContext& ctx) { ctx.buy(1, 500.0); },
    });

    open_probe();
    testing::internal::CaptureStderr();
    start_strategy();
    ASSERT_TRUE(wait_for_events(1));
    ASSERT_EQ(warm_up(kInstrumentA), 1U);

    // 回调期第一笔 tick 带涨跌停：price 落在涨停价以下的正常价位，涨跌停分别是 110/90
    DzTick tick = make_tick(kInstrumentA, warmed_up_clock(0), warmed_up_price(0),
                            warmed_up_volume(0), warmed_up_price(0) * warmed_up_volume(0));
    tick.upper_limit_price = kUpperLimit;
    tick.lower_limit_price = 90.0;
    ASSERT_TRUE(write_tick(tick));
    ASSERT_TRUE(wait_for_count(kInstrumentA, EventKind::BarUpdate, 1));
    stop_strategy();
    const std::string stderr_text = testing::internal::GetCapturedStderr();
    EXPECT_EQ(exit_code_, 0);
    collect_order_and_report_frames();

    ASSERT_EQ(orders_.size(), 1U);
    EXPECT_DOUBLE_EQ(orders_[0].price, kUpperLimit)
        << "a limit-up order must be placed at the limit price, not rejected";
    EXPECT_EQ(orders_[0].volume, 1);

    EXPECT_NE(stderr_text.find("order price adjusted |"), std::string::npos)
        << "the clamp must be diagnosed: " << stderr_text;
    EXPECT_NE(stderr_text.find("reason=limit_up"), std::string::npos) << stderr_text;
    // 决策性断言：这条路径**不是**拒单
    EXPECT_EQ(stderr_text.find("order rejected |"), std::string::npos) << stderr_text;
}

// ============================================================
// 启动失败路径：缺失配置 / 非法配置（组件文档 §7）
// ============================================================

TEST_F(BarStrategyFrameworkTest, MissingConfigGeneratesDefaultAndExitsNonZero) {
    // 不写配置：框架应生成最小默认配置、打印提示并自主退出（不进入事件循环）
    testing::internal::CaptureStderr();
    start_strategy();
    if (strategy_thread_.joinable()) {
        strategy_thread_.join();
    }
    const std::string stderr_text = testing::internal::GetCapturedStderr();

    EXPECT_EQ(exit_code_, 1) << "missing config must exit non-zero";
    EXPECT_TRUE(std::filesystem::exists(config_path_))
        << "framework must write the minimal default config";
    EXPECT_NE(stderr_text.find("config file missing"), std::string::npos) << stderr_text;
    EXPECT_NE(stderr_text.find("edit the file and restart"), std::string::npos) << stderr_text;
    EXPECT_TRUE(recorder().snapshot().empty()) << "no strategy callback before config is valid";
}

/// 非法配置（schema_version 不受支持）：打印错误并以非 0 退出，不静默继续
TEST_F(BarStrategyFrameworkTest, InvalidConfigExitsNonZero) {
    {
        std::ofstream stream(config_path_);
        ASSERT_TRUE(stream.good());
        stream << "{\n  \"schema_version\": 99,\n  \"period_sec\": 60,\n"
                  "  \"instruments\": [\"rb2601\"]\n}\n";
    }
    testing::internal::CaptureStderr();
    start_strategy();
    if (strategy_thread_.joinable()) {
        strategy_thread_.join();
    }
    const std::string stderr_text = testing::internal::GetCapturedStderr();

    EXPECT_EQ(exit_code_, 1) << "invalid config must exit non-zero";
    EXPECT_NE(stderr_text.find("strategy startup failed |"), std::string::npos) << stderr_text;
    EXPECT_TRUE(recorder().snapshot().empty());
}

}  // namespace
