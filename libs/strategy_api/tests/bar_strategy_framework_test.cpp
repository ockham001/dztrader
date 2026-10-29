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

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
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

#include "bar_test_data.h"
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

/// `DzVolume` 上限（手数越界拒单的用例：两次 `buy` 这个值，第二笔差量必然越界）
constexpr dztrader::Volume kVolumeMax = std::numeric_limits<dztrader::Volume>::max();
/// 两笔 `kVolumeMax` 的意图后应有的逻辑持仓（拒单不回流，逻辑账照常推进）
constexpr dztrader::LargeVolume kTwiceVolumeMax =
    2 * static_cast<dztrader::LargeVolume>(kVolumeMax);

/// 注入式假历史的第一个 60s 桶起点（tsec 21600 = 墙钟 00:00；落在退化表 0–86399 内）
constexpr std::int32_t kHistoryFirstBeginSec = 21600;

// ============================================================
// fake B 的事件记录器（进程内全局；B 由框架默认构造，拿不到测试注入的指针）
// ============================================================

/// @brief B 的六类观测事件（与六个回调一一对应；`on_params_update` 属 #162）
enum class EventKind : std::uint8_t { Init, BarOpen, BarUpdate, BarClose, ParamsUpdate, Destroy };

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
        case EventKind::ParamsUpdate:
            return "params_update";
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
    bool is_auction = false;            ///< `ctx.is_auction()`（#163：触发本次回调的 tick 相位）
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
    // #162：每次回调读一次参数（「`param set` 之后 B 重读到新值」的唯一可观测证据）
    double param_fast = -1;  ///< `ctx.param<double>("fast", -1)`
    std::string param_mode;  ///< `ctx.param<std::string>("mode", "<unset>")`
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

/// @brief 取 stderr 中包含 marker 的第一行（未找到返回空串）
///
/// `silent_bars=` 同时出现在回放计划（估算）与完成汇总（实际）两行里，只做全串 find 无法
/// 区分二者——必须按行取用。
std::string stderr_line_with(const std::string& stderr_text, std::string_view marker) {
    const std::size_t at = stderr_text.find(marker);
    if (at == std::string::npos) {
        return {};
    }
    const std::size_t begin = stderr_text.rfind('\n', at);
    const std::size_t line_begin = begin == std::string::npos ? 0 : begin + 1;
    const std::size_t end = stderr_text.find('\n', at);
    return stderr_text.substr(line_begin,
                              end == std::string::npos ? std::string::npos : end - line_begin);
}

/// @brief 读当前 ctx 状态并记录（B 的六个回调共用）
void record(EventKind kind, dztrader::InstrumentContext& ctx) {
    ObservedEvent event;
    event.kind = kind;
    event.instrument = ctx.instrument_id();
    event.ctx_address = &ctx;
    event.bar_index = ctx.bar_index();
    event.window_size = ctx.close.size();
    event.is_auction = ctx.is_auction();
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
    event.param_fast = ctx.param<double>("fast", -1.0);
    event.param_mode = ctx.param<std::string>("mode", "<unset>");
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

    /**
     * @brief 参数热更新通知（#162）：记录并重读参数
     *
     * `record()` 里的 `param_fast` 字段带着本次回调读到的参数值——这才让「通知之后 B 读到
     * 新值」成为可观测事实，而不是只断言「回调被调了一次」。
     */
    void on_params_update(dztrader::InstrumentContext& ctx) {
        record(EventKind::ParamsUpdate, ctx);
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
// E2E 验收用策略：双均线 + 固定止损（#165 AC1；只经公开 API）
// ============================================================
//
// 这条策略是「整条链路」的验收载体：它不碰框架私有状态，只用 `InstrumentContext` 的公开面
// （价格序列 / `make_series` / 四个交易意图 / Tier 1 查询），因此它同时是「公开 API 够用」
// 的正面证据。策略线程只做确定性计算与记录；gtest 断言一律留在主线程。

/// @brief 快线（5 根收盘均线）周期
constexpr std::int32_t kDualMaFastPeriod = 5;
/// @brief 慢线（20 根收盘均线）周期
constexpr std::int32_t kDualMaSlowPeriod = 20;
/// @brief 固定止损距离（tick 数）：多仓 `close <= 均价 − 5 × price_tick`，空仓镜像
constexpr std::int32_t kDualMaStopTicks = 5;
/// @brief 测试注入的合约最小变动价位（与 Seam B 的 `InstrumentSpec.price_tick` 一致）
///
/// B 的公开面没有「读合约元数据」的接口（元数据只供框架做价格对齐与 Tier 1 换算），
/// 因此策略按配置常量工作：验收用例注入 `price_tick = 1.0`，这里也取 1.0。
constexpr double kDualMaPriceTick = 1.0;
/// @brief 每次开仓/加仓的策略手数（验收场景的 2 手）
constexpr dztrader::Volume kDualMaVolume = 2;

/// @brief 一次意图前后各抄一份的 Tier 1 快照（#165 AC2 的精确数值断言用）
struct Tier1Snapshot {
    std::int32_t market_position = 0;             ///< `ctx.market_position()`
    dztrader::LargeVolume current_contracts = 0;  ///< `ctx.current_contracts()`
    double avg_entry_price = 0;                   ///< `ctx.avg_entry_price()`
    std::int32_t current_entries = 0;             ///< `ctx.current_entries()`
    dztrader::LargeVolume max_contracts = 0;      ///< `ctx.max_contracts()`
    std::int32_t max_entries = 0;                 ///< `ctx.max_entries()`
    double entry_price = 0;                       ///< `ctx.entry_price()`
    double exit_price = 0;                        ///< `ctx.exit_price()`
    std::int64_t bars_since_entry = -1;           ///< `ctx.bars_since_entry()`
    std::int64_t bars_since_last_entry = -1;      ///< `ctx.bars_since_last_entry()`
    std::int64_t bars_since_exit = -1;            ///< `ctx.bars_since_exit()`
    double position_profit = 0;                   ///< `ctx.position_profit()`
    double contract_profit = 0;                   ///< `ctx.contract_profit()`
    double close = 0;                             ///< 意图执行时的 `ctx.close[0]`（计价现价）
};

/// @brief 抄一份当前 Tier 1（在策略线程调用；只读公开查询面）
[[nodiscard]] Tier1Snapshot capture_tier1(dztrader::InstrumentContext& ctx) {
    Tier1Snapshot snapshot;
    snapshot.market_position = ctx.market_position();
    snapshot.current_contracts = ctx.current_contracts();
    snapshot.avg_entry_price = ctx.avg_entry_price();
    snapshot.current_entries = ctx.current_entries();
    snapshot.max_contracts = ctx.max_contracts();
    snapshot.max_entries = ctx.max_entries();
    snapshot.entry_price = ctx.entry_price();
    snapshot.exit_price = ctx.exit_price();
    snapshot.bars_since_entry = ctx.bars_since_entry();
    snapshot.bars_since_last_entry = ctx.bars_since_last_entry();
    snapshot.bars_since_exit = ctx.bars_since_exit();
    snapshot.position_profit = ctx.position_profit();
    snapshot.contract_profit = ctx.contract_profit();
    if (ctx.bar_index() >= 0 && ctx.close.size() > 0) {
        snapshot.close = ctx.close[0];
    }
    return snapshot;
}

/// @brief 一次测试策略意图的观测（标签 + 意图价 + 两条均线 + 前后 Tier 1）
struct IntentObservation {
    std::string label;            ///< `buy` / `sell_short` / `buy_to_cover` / `sell`
    std::int64_t bar_index = -1;  ///< `ctx.bar_index()`（触发意图的 bar）
    double price = 0;             ///< 意图传入的限价（= 当根 bar 收盘价）
    double fast = 0;              ///< 本次回调算出的 5 根均线
    double slow = 0;              ///< 本次回调算出的 20 根均线
    Tier1Snapshot before;         ///< 意图执行前
    Tier1Snapshot after;          ///< 意图执行后（虚拟成交即时生效）
};

/// @brief 进程内意图观测记录器（策略线程写、测试线程读；与 {@link Recorder} 同模式）
class IntentRecorder {
public:
    /// @brief 记录一次意图观测
    void push(IntentObservation observation) {
        const std::lock_guard<std::mutex> lock(mutex_);
        observations_.push_back(std::move(observation));
    }

    /// @brief 当前记录数（等待循环用）
    [[nodiscard]] std::size_t size() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return observations_.size();
    }

    /// @brief 观测快照拷贝
    [[nodiscard]] std::vector<IntentObservation> snapshot() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return observations_;
    }

    /// @brief 清空（每个用例开始前）
    void reset() {
        const std::lock_guard<std::mutex> lock(mutex_);
        observations_.clear();
    }

private:
    mutable std::mutex mutex_;
    std::vector<IntentObservation> observations_;
};

/// @brief 进程内唯一意图记录器（策略与测试共用）
IntentRecorder& intent_recorder() {
    static IntentRecorder instance;
    return instance;
}

/// @brief 当前 bar 的收盘均价（`[0]` 最新）；调用方保证窗口已满
[[nodiscard]] double mean_close(dztrader::InstrumentContext& ctx, std::int32_t period) {
    double sum = 0.0;
    for (std::int32_t n = 0; n < period; ++n) {
        sum += ctx.close[static_cast<std::ptrdiff_t>(n)];
    }
    return sum / static_cast<double>(period);
}

/// @brief 执行一次意图并记录前后 Tier 1（标签 + 意图价 + 均线 + 快照）
template <class Action>
void record_intent(dztrader::InstrumentContext& ctx,
                   std::string_view label,
                   double fast_value,
                   double slow_value,
                   Action&& action) {
    IntentObservation observation;
    observation.label = std::string(label);
    observation.bar_index = ctx.bar_index();
    observation.fast = fast_value;
    observation.slow = slow_value;
    observation.before = capture_tier1(ctx);
    observation.price = observation.before.close;
    action();
    observation.after = capture_tier1(ctx);
    intent_recorder().push(std::move(observation));
}

/**
 * @brief 双均线 + 固定止损测试策略（#165 AC1）
 *
 * 状态机（确定性、逐 bar 只跑一次，全部在 `on_bar_close`）：
 *
 *   1. **固定止损优先**：多仓且 `close <= avg_entry_price − 5 × price_tick` → `sell(0)` 全平；
 *      空仓且 `close >= avg_entry_price + 5 × price_tick` → `buy_to_cover(0)` 全平。命中即记
 *      一条意图标签并返回（同一根 bar 不再做均线动作）。
 *   2. **均线关系**：快线 > 慢线且当前不是多头 → `buy(2)`；快线 < 慢线且当前不是空头 →
 *      `sell_short(2)`；相等不动。反向时框架把「平旧仓 + 开新仓」记成一次反手，发单侧仍是
 *      一张差量单。
 *
 * 六回调全部实现：`on_init` 注册两条用户序列（5/20 均线），其余回调只记录观测（事件序断言
 * 用），`on_bar_close` 才是决策点。
 */
struct DualMaStopStrategy {
    dztrader::Series<double> fast;  ///< 5 根收盘均线（当前 bar 写在 `[0]`）
    dztrader::Series<double> slow;  ///< 20 根收盘均线

    void on_init(dztrader::InstrumentContext& ctx) {
        fast = ctx.make_series<double>(0.0);
        slow = ctx.make_series<double>(0.0);
        record(EventKind::Init, ctx);
    }

    void on_bar_open(dztrader::InstrumentContext& ctx) { record(EventKind::BarOpen, ctx); }

    void on_bar_update(dztrader::InstrumentContext& ctx) { record(EventKind::BarUpdate, ctx); }

    void on_bar_close(dztrader::InstrumentContext& ctx) {
        record(EventKind::BarClose, ctx);
        if (ctx.close.size() < kDualMaSlowPeriod) {
            return;  // 均线窗口未就绪（本验收脚本不会走到这里：回调期窗口恒为满窗）
        }
        const double fast_value = mean_close(ctx, kDualMaFastPeriod);
        const double slow_value = mean_close(ctx, kDualMaSlowPeriod);
        fast[0] = fast_value;
        slow[0] = slow_value;

        const double close_price = ctx.close[0];
        const std::int32_t position = ctx.market_position();
        const double stop_distance = static_cast<double>(kDualMaStopTicks) * kDualMaPriceTick;
        // 1. 固定止损优先：命中即全平同类方向持仓并结束本 bar 的决策。
        if (position > 0 && close_price <= ctx.avg_entry_price() - stop_distance) {
            record_intent(ctx, "sell", fast_value, slow_value,
                          [&ctx, close_price] { ctx.sell(0, close_price); });
            return;
        }
        if (position < 0 && close_price >= ctx.avg_entry_price() + stop_distance) {
            record_intent(ctx, "buy_to_cover", fast_value, slow_value,
                          [&ctx, close_price] { ctx.buy_to_cover(0, close_price); });
            return;
        }
        // 2. 均线关系决定持仓方向；反向时框架把「平旧仓 + 开新仓」记成一次反手。
        if (fast_value > slow_value && position <= 0) {
            record_intent(ctx, "buy", fast_value, slow_value,
                          [&ctx, close_price] { ctx.buy(kDualMaVolume, close_price); });
        } else if (fast_value < slow_value && position >= 0) {
            record_intent(ctx, "sell_short", fast_value, slow_value,
                          [&ctx, close_price] { ctx.sell_short(kDualMaVolume, close_price); });
        }
    }

    void on_params_update(dztrader::InstrumentContext& ctx) {
        record(EventKind::ParamsUpdate, ctx);
    }

    void on_destroy(dztrader::InstrumentContext& ctx) { record(EventKind::Destroy, ctx); }
};

static_assert(dztrader::BarStrategy<DualMaStopStrategy>);

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
 * @brief 构造一段可回放的归档历史（#161 Seam B 的假历史 bar 源数据）
 *
 * 每根 bar 占一个 60 秒桶（`first_begin_sec` 起连续），价格按索引线性变化，涨跌交替（奇数
 * 根 `close < open`、偶数根 `close > open`），量额/持仓量逐根推进，**当日累计**从 0 起算
 * ——这正是 builder 的累计基准口径，回放才可能逐字段还原。
 *
 * @param bar_count      根数
 * @param first_begin_sec 第一根的桶起点（交易日内秒，须为 60 的整数倍）
 * @param first_price    第一根的开盘价（每根 +1）
 * @param last_forming   true = 最后一根按 forming 给出（验证续接规则的另一半）
 * @return 升序归档 bar
 */
std::vector<dztrader::bar::Bar> make_archive_history(
    std::size_t bar_count,
    std::int32_t first_begin_sec = kHistoryFirstBeginSec,
    double first_price = 200.0,
    bool last_forming = false) {
    std::vector<dztrader::bar::Bar> bars;
    bars.reserve(bar_count);
    std::int64_t cum_volume = 0;
    double cum_turnover = 0.0;
    for (std::size_t i = 0; i < bar_count; ++i) {
        dztrader::bar::Bar bar{};
        bar.trading_day = kTradingDay;
        bar.begin_sec = first_begin_sec + static_cast<std::int32_t>(i) * 60;
        bar.last_tick_sec = bar.begin_sec + 59;
        bar.period_sec = 60;
        bar.open = first_price + static_cast<double>(i);
        bar.high = bar.open + 2.0;
        bar.low = bar.open - 1.0;
        bar.close = (i % 2 == 0) ? bar.open + 1.0 : bar.open - 0.5;
        bar.volume = 10;
        bar.turnover = bar.close * 10.0;
        bar.open_interest = 1000 + static_cast<DzLargeVolume>(i);
        cum_volume += bar.volume;
        cum_turnover += bar.turnover;
        bar.cum_volume = cum_volume;
        bar.cum_turnover = cum_turnover;
        bar.is_final = !(last_forming && i + 1 == bar_count);
        bars.push_back(bar);
    }
    return bars;
}

/// @brief 一根归档 bar 的 4 个合成 tick 的收盘价顺序（组件文档 §5.3 的涨/跌/平口径）
std::array<double, 4> replay_tick_prices(const dztrader::bar::Bar& bar) {
    if (bar.close < bar.open) {
        return {bar.open, bar.high, bar.low, bar.close};
    }
    return {bar.open, bar.low, bar.high, bar.close};
}

// 复用 bar 层测试的共享夹具（单一真相源：含 Open 竞价窗口的小节表与 09:00/15:00 交易日内秒
// 常量只在 `bar_test_data.h` 定义一次；replay / diff / builder 测试同样 include 该头）。
using dztrader::strategy_api::test::day_auction_session_table;
using dztrader::strategy_api::test::TSEC_09_00;

/// @brief 08:59:00（墙钟）的交易日内秒：Open 竞价窗口起点（tsec = 53940）
constexpr std::int32_t kAuctionBeginSec = TSEC_09_00 - 60;

/// @brief 日盘静默填窗的起始墙钟秒（09:00；`warm_up` 按墙钟秒步进）
constexpr std::int32_t kDaySessionWarmupClock = dztrader::bar::TradingTime::to_clock(TSEC_09_00);

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
        intent_recorder().reset();
        reset_observations();
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
    /// @param warmup_json   预热字段片段（`"warmup": {"bars": 40},` 等，含行尾逗号；#161）
    void write_config(const std::vector<std::string>& instruments,
                      std::optional<std::int32_t> max_lookback = std::nullopt,
                      std::optional<std::int32_t> reserve_bars = std::nullopt,
                      std::int32_t period_sec = 60,
                      const std::string& accounts_json = "[]",
                      const std::string& scaling_json = "",
                      const std::string& warmup_json = "",
                      const std::string& params_json = "{}") {
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
        text += warmup_json;
        text += "  \"params\": " + params_json + "\n}\n";
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

    /// @brief 写一条定向本策略的 UI_INPUT 帧（#162；命令文本，无 NUL 结尾也合法）
    ///
    /// 与 SHUTDOWN 同一条 event 通道、同一套 `ext_inst` 布局；`DZ_FRAME_UI_INPUT` 的 payload
    /// 是 UTF-8 文本、**不以 NUL 结尾**（长度由 `data_size` 给出），因此这里按字节数写、
    /// 不依赖 `strlen`。
    bool write_ui_input(std::string_view command) {
        try {
            dztrader::shm::MultiWriter writer =
                dztrader::shm::MultiWriter::create(open_event_meta(), "bar_framework_test_writer");
            const bool written =
                writer.write_ext_inst_frame(DZ_FRAME_UI_INPUT, strategy_id_.c_str(),
                                            reinterpret_cast<const std::byte*>(command.data()),
                                            static_cast<std::uint32_t>(command.size()));
            writer.notify_subscribers();
            return written;
        } catch (...) {
            return false;
        }
    }

    /// @brief 写一条 UI_INPUT 并等它的响应出现在 OUTPUT_UI 里（超时返回 false）
    ///
    /// 命令是异步的（帧 → 事件泵 → `on_ui_input` → 写 OUTPUT_UI），因此必须「等结果出现」
    /// 而不是「发完就断言」。`expected_index` 是本用例里第几条响应（从 0 起）——同一用例
    /// 连发多条命令时用它区分先后。
    bool run_command(std::string_view command, std::size_t expected_index = 0) {
        if (!write_ui_input(command)) {
            return false;
        }
        return wait_until([this, expected_index] {
            drain_probe();  // 响应是异步写出的：每轮都排空，才能观察到第 N 条
            return ui_outputs_.size() > expected_index;
        });
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

    /// @brief 在线程里启动 `run_bar_strategy<Strategy>`（内部自行 dz_init / dz_release）
    template <class Strategy>
    void start_strategy_as() {
        strategy_thread_ =
            std::thread([this] { exit_code_ = dztrader::run_bar_strategy<Strategy>(); });
    }

    /// @brief 用既有 fake B 启动（行为不变；#165 起转调模板版本）
    void start_strategy() { start_strategy_as<FakeStrategy>(); }

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

    /// @brief 写出某个交易日内秒的一笔 tick（`trading_day = kTradingDay + day_offset`）
    bool write_tick_at_tsec(std::string_view instrument,
                            std::int32_t day_offset,
                            std::int32_t tsec,
                            double price,
                            DzVolume volume) {
        return write_tick(make_tick_on(instrument, kTradingDay + day_offset,
                                       dztrader::bar::TradingTime::to_clock(tsec), price, volume,
                                       price * static_cast<double>(volume)));
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
     * @brief 排空事件通道里**剩余**的订单帧与上报帧（须在策略停止后调用）
     *
     * 用**独立 probe reader** 而不是 `dz_next_event`：平台内部帧（TD_ORDER_REQ /
     * SET_LOGICAL_POSITION）在 SDK 侧被拦截，不会交给策略回调（见 `trade_api_test` 的同款
     * 做法）。probe 从创建时刻起顺序读通道，因此必须在启动策略**之前**创建它，否则会漏掉
     * 启动阶段的帧。
     *
     * **不清空观测容器**：`run_command()` 需要在命令进行中反复 `drain_probe()`（OUTPUT_UI 是
     * 异步写出的），那些帧已经被消费掉了——这里再清一次等于丢掉它们。清空发生在每个用例
     * 开始前的 `reset_observations()`（`SetUp` 调用）。
     */
    void collect_order_and_report_frames() { drain_probe(); }

    /// @brief 清空全部观测容器（`SetUp` 调用；probe 尚未创建，无帧可丢）
    void reset_observations() {
        orders_.clear();
        reports_.clear();
        ui_outputs_.clear();
        ui_notifications_.clear();
    }

    /**
     * @brief 把 probe 里**新增**的帧追加到观测容器（不清空）
     *
     * `run_command()` 需要在命令进行中反复排空：命令响应是异步写出的，`wait_until` 的判定
     * 依据就是「`ui_outputs_` 里出现了第 N 条」。stop 之后那一轮 `collect_order_and_report_frames`
     * 会清空重扫，因此两处不冲突。
     */
    void drain_probe() {
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
            } else if (view.type() == DZ_FRAME_OUTPUT_UI) {
                // #162：OUTPUT_UI 是变长 ext_inst 帧（instance_id = 策略名，payload = UTF-8 文本）
                const auto size = static_cast<std::size_t>(view.ext_inst_payload_size());
                ui_outputs_.emplace_back(reinterpret_cast<const char*>(view.ext_inst_payload()),
                                         size);
            } else if (view.type() == DZ_FRAME_NOTIFY_UI) {
                // #162：dz_notify_ui 写 JSON payload（source/level/message/timestamp/popup）
                const auto size = static_cast<std::size_t>(view.ext_payload_size());
                ui_notifications_.emplace_back(reinterpret_cast<const char*>(view.ext_payload()),
                                               size);
            }
        }
    }

    /**
     * @brief 只取「意图引起的」上报帧，跳过进入 live 时的基线批量上报
     *
     * `enter_live()` 在 `on_start` 末尾对**全部**挂靠账户报一次当前逻辑持仓（§5.3 的一次性
     * 上报；`L_i == 0` 也报，用于清掉上次运行留下的 UI 镜像）。因此有 N 个账户时，`reports_`
     * 的前 N 条是基线，之后才是策略意图产生的。断言「某笔意图是否上报」必须排除基线，
     * 否则基线会把计数抬高、让 `L_i == 0` 该跳过的断言失效。
     */
    [[nodiscard]] std::vector<ObservedPositionReport> intent_reports(
        std::size_t account_count) const {
        if (reports_.size() <= account_count) {
            return {};
        }
        return {reports_.begin() + static_cast<std::ptrdiff_t>(account_count), reports_.end()};
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
    /// #162：命令的 OUTPUT_UI 响应（文本）与 NOTIFY_UI 通知（JSON）
    std::vector<std::string> ui_outputs_;
    std::vector<std::string> ui_notifications_;

    // ---- Seam B：注入式假历史 bar 源 + 假合约元数据源（#161）----

    /// @brief 一次历史查询的观测记录（加载器在策略线程写；主线程 join 后读）
    struct ObservedHistoryQuery {
        std::string instrument_id;
        std::int32_t period_sec = 0;
        std::optional<std::int32_t> warmup_bars;
        std::optional<DzDate> warmup_from;
    };

    /// @brief 注入的交易小节/竞价窗口表（默认退化表；#163 的竞价用例换成含 Open 窗口的表）
    dztrader::bar::TradingSessionTable injected_session_table_ =
        dztrader::degraded_trading_session_table();

    /// @brief 假历史 bar 源：instrument → 归档 bar（升序）；缺省 = Unavailable
    std::map<std::string, std::vector<dztrader::bar::Bar>, std::less<>> fake_history_;
    bool fake_history_available_ = true;                 ///< false = 数据源不可用（冷启动降级用例）
    std::vector<ObservedHistoryQuery> history_queries_;  ///< 查询参数（断言 warmup 两形态）

    /// @brief 假合约元数据源：instrument → 合约属性；缺省 = Missing
    std::map<std::string, dztrader::InstrumentSpec, std::less<>> fake_metadata_;
    bool fake_metadata_available_ = true;  ///< false = 数据源不可用（部署级降级用例）

    /**
     * @brief 「回放即将开始」同步点（缓冲 tick 续接用例）
     *
     * 行情 reader 在 `dz_init` 时从当前写位置开始，**测试在启动前写入的 tick 会被跳过**；
     * 要复现「回放期间到达的缓冲 tick」，必须让回放晚于测试写帧。做法是让假历史加载器
     * （在策略线程、回放之前调用）发出「回放即将开始」信号并等待测试写完缓冲 tick：
     * 这既确定性地覆盖 `drain_buffered_ticks` 路径，又不依赖 sleep/时序。
     */
    std::mutex history_barrier_mutex_;
    std::condition_variable history_barrier_cv_;
    bool history_barrier_enabled_ = false;
    bool history_loader_reached_ = false;  ///< 加载器已到达（回放即将开始）
    bool buffered_ticks_written_ = false;  ///< 测试已写完缓冲 tick

    /// @brief 等假历史加载器到达同步点（超时返回 false）
    bool wait_for_history_loader(std::chrono::milliseconds timeout = 15s) {
        std::unique_lock<std::mutex> lock(history_barrier_mutex_);
        return history_barrier_cv_.wait_for(lock, timeout,
                                            [this] { return history_loader_reached_; });
    }

    /// @brief 放行被同步点挂住的假历史加载器（测试写完缓冲 tick 后调用）
    void release_history_loader() {
        {
            const std::lock_guard<std::mutex> lock(history_barrier_mutex_);
            buffered_ticks_written_ = true;
        }
        history_barrier_cv_.notify_all();
    }

    /// @brief 组装注入 fake 历史/元数据/小节表的依赖（默认退化表 + 默认回卷时刻）
    dztrader::bar_strategy_internal::FrameworkDependencies make_injected_dependencies() {
        dztrader::bar_strategy_internal::FrameworkDependencies dependencies;
        dependencies.session_table = injected_session_table_;
        dependencies.history = [this](const dztrader::bar_strategy_internal::HistoryQuery& query) {
            history_queries_.push_back(ObservedHistoryQuery{
                .instrument_id = std::string(query.instrument_id),
                .period_sec = query.period_sec,
                .warmup_bars = query.warmup_bars,
                .warmup_from = query.warmup_from,
            });
            if (history_barrier_enabled_) {
                std::unique_lock<std::mutex> lock(history_barrier_mutex_);
                history_loader_reached_ = true;
                history_barrier_cv_.notify_all();
                history_barrier_cv_.wait(lock, [this] { return buffered_ticks_written_; });
            }
            dztrader::bar_strategy_internal::HistoryQueryResult result;
            if (!fake_history_available_) {
                result.status = dztrader::bar_strategy_internal::HistoryStatus::Unavailable;
                result.message = "fake history source unavailable";
                return result;
            }
            const auto found = fake_history_.find(query.instrument_id);
            if (found == fake_history_.end()) {
                result.status = dztrader::bar_strategy_internal::HistoryStatus::Unavailable;
                result.message = "fake history source has no data";
                return result;
            }
            result.status = dztrader::bar_strategy_internal::HistoryStatus::Loaded;
            result.bars = found->second;
            return result;
        };
        dependencies.metadata = [this](std::string_view instrument_id) {
            dztrader::bar_strategy_internal::MetadataResult result;
            if (!fake_metadata_available_) {
                result.status = dztrader::bar_strategy_internal::MetadataStatus::SourceUnavailable;
                result.message = "fake metadata source unavailable";
                return result;
            }
            const auto found = fake_metadata_.find(instrument_id);
            if (found == fake_metadata_.end()) {
                result.status = dztrader::bar_strategy_internal::MetadataStatus::Missing;
                result.message = "fake metadata missing | instrument=" + std::string(instrument_id);
                return result;
            }
            result.status = dztrader::bar_strategy_internal::MetadataStatus::Loaded;
            result.spec = found->second;
            return result;
        };
        return dependencies;
    }

    /// @brief 在策略线程用注入依赖启动 `run_bar_strategy_with<Strategy>`（Seam B）
    template <class Strategy>
    void start_strategy_as_with_injected_dependencies() {
        dztrader::bar_strategy_internal::FrameworkDependencies dependencies =
            make_injected_dependencies();
        strategy_thread_ = std::thread([this, dependencies] {
            exit_code_ =
                dztrader::bar_strategy_internal::run_bar_strategy_with<Strategy>(dependencies);
        });
    }

    /// @brief 用既有 fake B 走 Seam B 启动（行为不变；#165 起转调模板版本）
    void start_strategy_with_injected_dependencies() {
        start_strategy_as_with_injected_dependencies<FakeStrategy>();
    }

    /**
     * @brief 起一次「`instrument_count` 个品种 × 预热上限」的启动并返回 stderr（成本 WARN
     * 用例共用）
     *
     * 成本估算只吃配置（`warmup.bars` × 品种数），回放本身喂很小的假历史；两个阈值用例只有
     * 品种数不同，共用这段准备/启动/停止序列。
     */
    std::string run_warmup_cost_case(int instrument_count) {
        std::vector<std::string> instruments;
        for (int index = 0; index < instrument_count; ++index) {
            instruments.push_back("rb" + std::to_string(2601 + index));
        }
        write_config(instruments, kTestMaxLookback, std::nullopt, 60, "[]", "",
                     "  \"warmup\": {\"bars\": 10000000},\n");
        for (const std::string& instrument : instruments) {
            fake_history_[instrument] =
                make_archive_history(static_cast<std::size_t>(kTestMaxLookback));
        }
        fake_metadata_available_ = false;

        testing::internal::CaptureStderr();
        start_strategy_with_injected_dependencies();
        EXPECT_TRUE(wait_for_events(static_cast<std::size_t>(instruments.size())));
        stop_strategy();
        return testing::internal::GetCapturedStderr();
    }
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
    // Tier 1 盈亏的乘数来源缺失必须**说出来**（静默 0 会让止损逻辑永不触发）。
    // 本用例的临时 home 里没有 td 库：元数据接缝（#161）走「部署级不可用」降级，
    // 而不是逐品种判失败——所以这里断言的是 `reason=platform_db_unavailable`。
    EXPECT_NE(stderr_text.find("instrument metadata unavailable |"), std::string::npos)
        << "the missing metadata source must be announced: " << stderr_text;
    EXPECT_NE(stderr_text.find("reason=platform_db_unavailable"), std::string::npos) << stderr_text;
    // 归档域未实现（dz_db_query_bar stub）必须按「无历史」降级冷启动，而不是把启动打断
    EXPECT_NE(stderr_text.find("historical bars unavailable |"), std::string::npos) << stderr_text;
    EXPECT_NE(stderr_text.find("fallback=cold_start"), std::string::npos) << stderr_text;
    EXPECT_NE(stderr_text.find("warmup replay complete |"), std::string::npos) << stderr_text;

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
    // 基线 2 条（两个账户在进入 live 时各报一次 L=0）+ 两笔意图各 2 条 = 6
    ASSERT_EQ(reports_.size(), 6U) << "2 accounts x (1 baseline + 1 long + 1 reversal) reports";
    const auto mirror_reports = intent_reports(2);
    ASSERT_EQ(mirror_reports.size(), 4U) << "one report per changed L_i per intent";
    EXPECT_EQ(mirror_reports[0].account_id, "CTP001");
    EXPECT_EQ(mirror_reports[0].instrument_id, kInstrumentA);
    EXPECT_EQ(mirror_reports[0].net_volume, 3);
    EXPECT_EQ(mirror_reports[1].account_id, "CTP002");
    EXPECT_EQ(mirror_reports[1].net_volume, 2);
    EXPECT_EQ(mirror_reports[2].account_id, "CTP001");
    EXPECT_EQ(mirror_reports[2].net_volume, -3);
    EXPECT_EQ(mirror_reports[3].account_id, "CTP002");
    EXPECT_EQ(mirror_reports[3].net_volume, -2);
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

    // 基线批量上报（进入 live 时 L=0）之后，两笔意图各报一条
    const auto scaling_reports = intent_reports(1);
    ASSERT_EQ(scaling_reports.size(), 2U) << "one report per intent that changed L";
    EXPECT_EQ(scaling_reports[0].net_volume, 1);
    EXPECT_EQ(scaling_reports[1].net_volume, 2);
    // 基线本身报的是冷启动值 0（全量对账口径，见 publish_all_positions）
    ASSERT_FALSE(reports_.empty());
    EXPECT_EQ(reports_[0].net_volume, 0) << "the live-entry baseline must publish the cold value";
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
    const auto min_one_reports = intent_reports(1);
    ASSERT_EQ(min_one_reports.size(), 1U);
    EXPECT_EQ(min_one_reports[0].net_volume, 1);
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
    // 基线 1 条（进入 live 时 L=0）+ 只有第三笔的 1 条 = 2；前两笔的 L 未变得不到上报
    const auto tiny_reports = intent_reports(1);
    ASSERT_EQ(tiny_reports.size(), 1U) << "unchanged L_i must not produce a report";
    EXPECT_EQ(tiny_reports[0].net_volume, 1);
}

/**
 * **进入 live 的全量对账上报携带本品种代码**（`publish_position` 的 `instrument_id` 参数化）。
 *
 * `publish_position` 把合约代码做成参数而不是硬编码，是为了让「账户 + 空品种」通配清零那条
 * 契约路径真的可达（删除账户归 #162 调用）。这条用例锁住的是**另一侧**：正常上报确实把
 * 本品种代码写进帧，而不是写空串——把通配语义误用到普通上报时，帧里的 `instrument_id` 会
 * 变成空串，dzweb 侧的镜像就会挂到一个不存在的合约上。
 */
TEST_F(BarStrategyFrameworkTest, BatchReportCarriesTheInstrumentId) {
    write_config({std::string(kInstrumentA), std::string(kInstrumentB)}, kTestMaxLookback,
                 /*reserve_bars=*/std::nullopt, /*period_sec=*/60,
                 /*accounts_json=*/R"([{"account_id": "CTP001", "scale": 1.0}])");
    open_probe();
    start_strategy();
    ASSERT_TRUE(wait_for_events(2));
    ASSERT_EQ(warm_up(kInstrumentA), 1U);
    ASSERT_EQ(warm_up(kInstrumentB, 64831), 1U);
    stop_strategy();
    EXPECT_EQ(exit_code_, 0);
    collect_order_and_report_frames();

    // 每个品种 1 个账户 → 各 1 条基线上报（`L_i == 0` 也报，用于清掉旧镜像）
    ASSERT_EQ(reports_.size(), 2U) << "one reconciliation report per instrument";
    for (const ObservedPositionReport& report : reports_) {
        EXPECT_EQ(report.account_id, "CTP001");
        EXPECT_FALSE(report.instrument_id.empty())
            << "a per-instrument report must never carry an empty instrument id "
               "(that is the wildcard-clear shape, not a position report)";
        EXPECT_TRUE(report.instrument_id == kInstrumentA || report.instrument_id == kInstrumentB)
            << "unexpected instrument id: " << report.instrument_id;
        EXPECT_EQ(report.net_volume, 0) << "cold start reports 0 for every account";
    }
    // 两个品种各报一条（不是同一品种报两条）
    EXPECT_NE(reports_[0].instrument_id, reports_[1].instrument_id);
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

// ============================================================
// #161 历史预热回放 + 缓冲续接（Seam B：注入假历史 bar 源 / 假合约元数据源）
// ============================================================

/**
 * 注入 K = max_lookback + 8 根归档历史：前 32 根静默填窗（B 零回调），之后 8 根逐根回放。
 *
 * 断言三件事，任何一件错了都说明「回放没走 live 那条 tick→bar 实现」：
 *   1. 事件序与 live 同构：`close(上一根) → open(新根) → update(新根) × 4`，一根不差；
 *   2. 回调里交出的 bar 值与归档一致（O/H/L/C、量/额/OI、合成 tick 的价格顺序）；
 *   3. 窗口历史就是归档历史（`close[n] == archive[bar_index - n].close`）——窗口被完整恢复。
 */
TEST_F(BarStrategyFrameworkTest, WarmupReplayDrivesLiveIsomorphicCallbacks) {
    const std::int32_t replayed = 8;
    const std::size_t total =
        static_cast<std::size_t>(kTestMaxLookback) + static_cast<std::size_t>(replayed);
    write_config({std::string(kInstrumentA)}, kTestMaxLookback);
    const std::vector<dztrader::bar::Bar> history = make_archive_history(total);
    fake_history_[std::string(kInstrumentA)] = history;
    fake_metadata_available_ = false;  // 元数据缺失不影响回放事件序

    testing::internal::CaptureStderr();
    start_strategy_with_injected_dependencies();
    ASSERT_TRUE(
        wait_for_count(kInstrumentA, EventKind::BarClose, static_cast<std::size_t>(replayed)))
        << "replay did not produce the expected callbacks";
    stop_strategy();
    const std::string stderr_text = testing::internal::GetCapturedStderr();
    ASSERT_EQ(exit_code_, 0);

    // 计划行是**按配置估算**（默认 warmup.bars=10000 → replay_bars≈9968），完成行才是
    // **实际回放**（K−max_lookback = 8）：两者必须分开断言，否则永远测不出估算是否被误用。
    const std::string plan_line = stderr_line_with(stderr_text, "warmup replay plan |");
    const std::string complete_line = stderr_line_with(stderr_text, "warmup replay complete |");
    EXPECT_NE(plan_line.find("warmup_bars=10000 silent_bars=32"), std::string::npos) << stderr_text;
    EXPECT_NE(complete_line.find("silent_bars=32 replayed_bars=8"), std::string::npos)
        << stderr_text;

    // 去掉 init/destroy：只剩回放期 B 可见的回调（close → open → update × 4）
    const std::vector<ObservedEvent> events = callback_stage_events(kInstrumentA);
    ASSERT_EQ(events.size(), static_cast<std::size_t>(replayed) * 6U);
    std::size_t cursor = 0;
    for (std::int32_t bar = kTestMaxLookback; bar < kTestMaxLookback + replayed; ++bar) {
        const std::int32_t previous = bar - 1;
        const ObservedEvent& close_event = events[cursor++];
        EXPECT_EQ(close_event.kind, EventKind::BarClose);
        EXPECT_EQ(close_event.bar_index, previous);
        // 被关闭那根的值 = 归档值（最后一根由下一笔更晚 tick 关闭，回放未额外 flush）
        EXPECT_EQ(close_event.open, history[static_cast<std::size_t>(previous)].open);
        EXPECT_EQ(close_event.high, history[static_cast<std::size_t>(previous)].high);
        EXPECT_EQ(close_event.low, history[static_cast<std::size_t>(previous)].low);
        EXPECT_EQ(close_event.close, history[static_cast<std::size_t>(previous)].close);

        const ObservedEvent& open_event = events[cursor++];
        EXPECT_EQ(open_event.kind, EventKind::BarOpen);
        EXPECT_EQ(open_event.bar_index, bar);
        EXPECT_EQ(open_event.close, history[static_cast<std::size_t>(bar)].open);

        const dztrader::bar::Bar& archive = history[static_cast<std::size_t>(bar)];
        const std::array<double, 4> expected_closes = replay_tick_prices(archive);
        double last_close = 0;
        for (std::size_t tick = 0; tick < expected_closes.size(); ++tick) {
            const ObservedEvent& update = events[cursor++];
            EXPECT_EQ(update.kind, EventKind::BarUpdate);
            EXPECT_EQ(update.bar_index, bar);
            EXPECT_DOUBLE_EQ(update.close, expected_closes[tick])
                << "synthetic tick price order must be O->L->H->C / O->H->L->C";
            last_close = update.close;
        }
        // 第 4 个 tick 之后 bar 的终值 = 归档值（量/额/OI 与 OHLC 同批校验）
        const ObservedEvent& last_update = events[cursor - 1];
        EXPECT_DOUBLE_EQ(last_update.close, archive.close);
        EXPECT_DOUBLE_EQ(last_update.high, archive.high);
        EXPECT_DOUBLE_EQ(last_update.low, archive.low);
        EXPECT_DOUBLE_EQ(last_update.volume_history[0], static_cast<double>(archive.volume));
        EXPECT_DOUBLE_EQ(last_update.turnover_history[0], archive.turnover);
        EXPECT_DOUBLE_EQ(last_update.open_interest_history[0],
                         static_cast<double>(archive.open_interest));
        EXPECT_DOUBLE_EQ(last_close, archive.close);
    }

    // 窗口历史就是归档历史：最后一笔 update 的 close[n] == archive[最后 bar - n].close
    const ObservedEvent& final_update = events.back();
    const std::int32_t final_bar = kTestMaxLookback + replayed - 1;
    ASSERT_EQ(final_update.history.size(), static_cast<std::size_t>(kTestMaxLookback));
    for (std::size_t n = 0; n < final_update.history.size(); ++n) {
        EXPECT_DOUBLE_EQ(final_update.history[n],
                         history[static_cast<std::size_t>(final_bar) - n].close)
            << "window slot close[" << n << "] must equal archive history";
    }
}

/**
 * 回放期的交易意图只做虚拟成交：不落单、不逐笔上报；回放结束后**一次性**上报恢复后的逻辑
 * 持仓（引用）：开平仓记录的持仓是 P=2，`enter_live` 的批量上报必须把它带出去，否则策略在
 * UI 上显示空仓、并被实盘 tick 的第一笔意图覆盖。
 */
TEST_F(BarStrategyFrameworkTest, WarmupReplayKeepsIntentsVirtualAndPublishesRecoveredPositionOnce) {
    const std::size_t total = static_cast<std::size_t>(kTestMaxLookback) + 5;
    write_config({std::string(kInstrumentA)}, kTestMaxLookback, std::nullopt, 60,
                 R"([{"account_id":"CTP001","scale":1.0}])");
    const std::vector<dztrader::bar::Bar> history = make_archive_history(total);
    fake_history_[std::string(kInstrumentA)] = history;
    fake_metadata_[std::string(kInstrumentA)] =
        dztrader::InstrumentSpec{.exchange_id = "SHFE", .volume_multiple = 10, .price_tick = 0.5};
    // 第一根回放 bar 上建仓 2 手（回放期 live_ == false → 只记账）
    recorder().add_script(ScriptedIntent{
        .instrument = std::string(kInstrumentA),
        .bar_index = kTestMaxLookback,
        .when = EventKind::BarUpdate,
        .apply = [](dztrader::InstrumentContext& ctx) { ctx.buy(2, ctx.close[0]); },
    });

    open_probe();
    testing::internal::CaptureStderr();
    start_strategy_with_injected_dependencies();
    ASSERT_TRUE(wait_for_count(kInstrumentA, EventKind::BarUpdate, 5 * 4));
    stop_strategy();
    const std::string stderr_text = testing::internal::GetCapturedStderr();
    ASSERT_EQ(exit_code_, 0);
    collect_order_and_report_frames();

    EXPECT_TRUE(orders_.empty()) << "replay intents must be virtual only (no order frames)";
    ASSERT_EQ(reports_.size(), 1U) << "logical positions must be published exactly once at startup";
    EXPECT_EQ(reports_[0].account_id, "CTP001");
    EXPECT_EQ(reports_[0].instrument_id, std::string(kInstrumentA));
    EXPECT_EQ(reports_[0].net_volume, 2) << "recovered position must be published, not zero";
    EXPECT_NE(stderr_text.find("account fanout enabled |"), std::string::npos) << stderr_text;
    EXPECT_NE(stderr_text.find("warmup=replayed"), std::string::npos) << stderr_text;
}

/**
 * 最后一根归档 bar 已收（final）：回放期间缓冲的 tick 中，目标桶 `<= B` 的必须丢弃（归档已
 * 覆盖，再喂一次会重复计一根），更晚桶的保留并触发 `close(B) → open(B+1)`。
 *
 * 用「假历史加载器同步点」保证缓冲 tick 在回放开始**之前**已写入 md 通道（行情 reader 在
 * `dz_init` 时从当前写位置开始，启动前写入的 tick 会被跳过）。
 */
TEST_F(BarStrategyFrameworkTest, BufferedTicksAreDroppedAgainstFinalTail) {
    const std::size_t total = static_cast<std::size_t>(kTestMaxLookback) + 4;
    write_config({std::string(kInstrumentA)}, kTestMaxLookback);
    const std::vector<dztrader::bar::Bar> history = make_archive_history(total);
    fake_history_[std::string(kInstrumentA)] = history;
    fake_metadata_available_ = false;
    history_barrier_enabled_ = true;

    const dztrader::bar::Bar& anchor = history.back();
    const int32_t last_tick_sec = anchor.begin_sec + 30;
    const auto cum_volume_after = static_cast<DzVolume>(anchor.cum_volume + 1);

    testing::internal::CaptureStderr();
    start_strategy_with_injected_dependencies();
    ASSERT_TRUE(wait_for_history_loader()) << "the fake history loader was never reached";
    // 1) 更早的桶（属于已收的上一根）→ 丢；2) 同桶（final）→ 丢；3) 下一桶 → 留
    ASSERT_TRUE(write_tick(
        make_tick(kInstrumentA, dztrader::bar::TradingTime::to_clock(anchor.begin_sec - 60 + 30),
                  anchor.close, cum_volume_after, anchor.cum_turnover + 1.0)));
    ASSERT_TRUE(write_tick(make_tick(
        kInstrumentA, dztrader::bar::TradingTime::to_clock(last_tick_sec), anchor.close + 0.5,
        static_cast<DzVolume>(cum_volume_after + 1), anchor.cum_turnover + 2.0)));
    ASSERT_TRUE(write_tick(
        make_tick(kInstrumentA, dztrader::bar::TradingTime::to_clock(anchor.begin_sec + 60 + 30),
                  anchor.close + 1.0, static_cast<DzVolume>(cum_volume_after + 2),
                  anchor.cum_turnover + 3.0)));
    release_history_loader();

    ASSERT_TRUE(wait_for_count(kInstrumentA, EventKind::BarClose, 5));
    stop_strategy();
    const std::string stderr_text = testing::internal::GetCapturedStderr();
    ASSERT_EQ(exit_code_, 0);

    EXPECT_NE(stderr_text.find("warmup continuation | drained=3 retained=1 dropped=2"),
              std::string::npos)
        << stderr_text;

    const std::vector<ObservedEvent> events = callback_stage_events(kInstrumentA);
    // final 尾部：同桶缓冲 tick 不产生第 5 个 update
    std::size_t updates_of_anchor = 0;
    for (const ObservedEvent& event : events) {
        if (event.kind == EventKind::BarUpdate &&
            event.bar_index == static_cast<std::int64_t>(total) - 1) {
            ++updates_of_anchor;
        }
    }
    EXPECT_EQ(updates_of_anchor, 4U)
        << "a buffered same-bucket tick must be dropped for final anchor";
    // 保留的下一桶 tick 关闭尾部 bar 并开新根
    ASSERT_GE(events.size(), 3U);
    EXPECT_EQ(events[events.size() - 3].kind, EventKind::BarClose);
    EXPECT_EQ(events[events.size() - 3].bar_index, static_cast<std::int64_t>(total) - 1);
    EXPECT_EQ(events[events.size() - 2].kind, EventKind::BarOpen);
    EXPECT_EQ(events[events.size() - 2].bar_index, static_cast<std::int64_t>(total));
    EXPECT_EQ(events[events.size() - 1].kind, EventKind::BarUpdate);
    EXPECT_EQ(events[events.size() - 1].bar_index, static_cast<std::int64_t>(total));
}

/**
 * 最后一根归档 bar 未完成（forming）：同桶的缓冲 tick 保留并**原地续接** forming bar——不产生
 * 重复的 close/open（同一根 bar 只能 open 一次、close 一次），多出的 update 直接并入。
 */
TEST_F(BarStrategyFrameworkTest, BufferedTicksContinueFormingTailWithoutDuplicateOpenClose) {
    const std::size_t total = static_cast<std::size_t>(kTestMaxLookback) + 4;
    write_config({std::string(kInstrumentA)}, kTestMaxLookback);
    std::vector<dztrader::bar::Bar> history =
        make_archive_history(total, kHistoryFirstBeginSec, 200.0, true);
    // forming bar 的 last_tick_sec 是「断线前最后一笔」：续接 tick 必须晚于它才不会被 builder
    // 当乱序丢掉。归档里默认填桶末（begin+59），这里改成断线时刻 begin+10。
    history.back().last_tick_sec = history.back().begin_sec + 10;
    fake_history_[std::string(kInstrumentA)] = history;
    fake_metadata_available_ = false;
    history_barrier_enabled_ = true;

    const dztrader::bar::Bar& anchor = history.back();
    const auto cum_volume_after = static_cast<DzVolume>(anchor.cum_volume + 1);

    testing::internal::CaptureStderr();
    start_strategy_with_injected_dependencies();
    ASSERT_TRUE(wait_for_history_loader()) << "the fake history loader was never reached";
    // 更早桶 → 丢；同桶 → 留（续接 forming bar）；下一桶 → 留（收 forming bar 并开新根）
    ASSERT_TRUE(write_tick(
        make_tick(kInstrumentA, dztrader::bar::TradingTime::to_clock(anchor.begin_sec - 60 + 30),
                  anchor.close, cum_volume_after, anchor.cum_turnover + 1.0)));
    ASSERT_TRUE(write_tick(
        make_tick(kInstrumentA, dztrader::bar::TradingTime::to_clock(anchor.begin_sec + 40),
                  anchor.close + 2.0, static_cast<DzVolume>(cum_volume_after + 1),
                  anchor.cum_turnover + 2.0)));
    ASSERT_TRUE(write_tick(
        make_tick(kInstrumentA, dztrader::bar::TradingTime::to_clock(anchor.begin_sec + 60 + 30),
                  anchor.close + 1.0, static_cast<DzVolume>(cum_volume_after + 2),
                  anchor.cum_turnover + 3.0)));
    release_history_loader();

    ASSERT_TRUE(wait_for_count(kInstrumentA, EventKind::BarClose, 5));
    stop_strategy();
    const std::string stderr_text = testing::internal::GetCapturedStderr();
    ASSERT_EQ(exit_code_, 0);

    EXPECT_NE(stderr_text.find("warmup continuation | drained=3 retained=2 dropped=1"),
              std::string::npos)
        << stderr_text;

    const auto anchor_index = static_cast<std::int64_t>(total) - 1;
    const std::vector<ObservedEvent> events = callback_stage_events(kInstrumentA);
    std::size_t anchor_updates = 0;
    std::size_t anchor_opens = 0;
    std::size_t anchor_closes = 0;
    double last_anchor_update_close = 0;
    for (const ObservedEvent& event : events) {
        if (event.kind == EventKind::BarUpdate && event.bar_index == anchor_index) {
            ++anchor_updates;
            last_anchor_update_close = event.close;
        } else if (event.kind == EventKind::BarOpen && event.bar_index == anchor_index) {
            ++anchor_opens;
        } else if (event.kind == EventKind::BarClose && event.bar_index == anchor_index) {
            ++anchor_closes;
        }
    }
    EXPECT_EQ(anchor_updates, 5U) << "the same-bucket buffered tick must continue the forming bar";
    EXPECT_EQ(anchor_opens, 1U) << "a forming bar must not be opened twice";
    EXPECT_EQ(anchor_closes, 1U)
        << "a forming bar must be closed exactly once (by the next bucket)";
    EXPECT_DOUBLE_EQ(last_anchor_update_close, anchor.close + 2.0);
}

/// 冷启动（归档不可用）：打印降级 WARN、不阻断启动，tick 照常走门禁（静默填窗 → 回调）
TEST_F(BarStrategyFrameworkTest, ColdStartDegradesToLiveWithoutHistory) {
    write_config({std::string(kInstrumentA)}, kTestMaxLookback);
    fake_history_available_ = false;
    fake_metadata_available_ = false;

    testing::internal::CaptureStderr();
    start_strategy_with_injected_dependencies();
    ASSERT_TRUE(wait_for_events(1));  // on_init 已执行 → 说明启动没被历史缺失打断
    stop_strategy();
    const std::string stderr_text = testing::internal::GetCapturedStderr();
    ASSERT_EQ(exit_code_, 0);

    EXPECT_NE(stderr_text.find("historical bars unavailable |"), std::string::npos) << stderr_text;
    EXPECT_NE(stderr_text.find("fallback=cold_start"), std::string::npos) << stderr_text;
    EXPECT_NE(stderr_text.find("warmup replay complete |"), std::string::npos) << stderr_text;
    EXPECT_NE(stderr_text.find("replayed_instruments=0"), std::string::npos) << stderr_text;
    EXPECT_TRUE(recorder().count(kInstrumentA, EventKind::BarOpen) == 0U)
        << "no history = no replay callbacks";
}

/// `warmup.bars` 模式：按**尾部**裁剪到最近 N 根（多给的历史被裁掉，续接基准仍是归档尾部）
TEST_F(BarStrategyFrameworkTest, WarmupBarsModeCropsToTailOfHistory) {
    write_config({std::string(kInstrumentA)}, kTestMaxLookback, std::nullopt, 60, "[]", "",
                 "  \"warmup\": {\"bars\": 40},\n");
    std::vector<dztrader::bar::Bar> history = make_archive_history(50);
    fake_history_[std::string(kInstrumentA)] = history;
    fake_metadata_available_ = false;

    testing::internal::CaptureStderr();
    start_strategy_with_injected_dependencies();
    // 40 根里 32 根静默 → 8 根回放，共 48 个回调
    ASSERT_TRUE(wait_for_count(kInstrumentA, EventKind::BarClose, 8));
    stop_strategy();
    const std::string stderr_text = testing::internal::GetCapturedStderr();
    ASSERT_EQ(exit_code_, 0);

    ASSERT_EQ(history_queries_.size(), 1U);
    EXPECT_EQ(history_queries_[0].instrument_id, std::string(kInstrumentA));
    EXPECT_EQ(history_queries_[0].period_sec, 60);
    ASSERT_TRUE(history_queries_[0].warmup_bars.has_value());
    EXPECT_EQ(*history_queries_[0].warmup_bars, 40);
    EXPECT_FALSE(history_queries_[0].warmup_from.has_value());
    const std::string plan_line = stderr_line_with(stderr_text, "warmup replay plan |");
    const std::string complete_line = stderr_line_with(stderr_text, "warmup replay complete |");
    EXPECT_NE(plan_line.find("silent_bars=32 replay_bars=8"), std::string::npos) << stderr_text;
    EXPECT_NE(complete_line.find("replayed_bars=8"), std::string::npos) << stderr_text;

    // 裁剪取尾部：首个 close 事件对应原归档下标 10+31 = 41
    const std::vector<ObservedEvent> events = recorder().for_instrument(kInstrumentA);
    const std::int64_t first_close_index = static_cast<std::int64_t>(kTestMaxLookback) - 1;
    ASSERT_GE(events.size(), 2U);
    EXPECT_EQ(events[1].kind, EventKind::BarClose);
    EXPECT_EQ(events[1].bar_index, first_close_index);
    EXPECT_DOUBLE_EQ(events[1].close, history[static_cast<std::size_t>(10 + 31)].close);
}

/// `warmup.from` 模式：把起始交易日（距纪元天数）原样交给数据源（裁剪语义由数据源负责）
TEST_F(BarStrategyFrameworkTest, WarmupFromModeForwardsStartDateToSource) {
    write_config({std::string(kInstrumentA)}, kTestMaxLookback, std::nullopt, 60, "[]", "",
                 "  \"warmup\": {\"from\": \"2026-01-05\"},\n");
    fake_history_[std::string(kInstrumentA)] = make_archive_history(40);
    fake_metadata_available_ = false;

    testing::internal::CaptureStderr();
    start_strategy_with_injected_dependencies();
    ASSERT_TRUE(wait_for_count(kInstrumentA, EventKind::BarClose, 8));
    stop_strategy();
    const std::string stderr_text = testing::internal::GetCapturedStderr();
    ASSERT_EQ(exit_code_, 0);

    ASSERT_EQ(history_queries_.size(), 1U);
    EXPECT_FALSE(history_queries_[0].warmup_bars.has_value());
    ASSERT_TRUE(history_queries_[0].warmup_from.has_value());
    EXPECT_EQ(*history_queries_[0].warmup_from,
              static_cast<DzDate>(dztrader::Date::from_string("2026-01-05").days_since_epoch()));
    EXPECT_NE(stderr_text.find("replayed_bars=8"), std::string::npos) << stderr_text;
    // `from` 模式的回放计划也要按日期跨度估算（起始日在过去 → 根数 > 0），否则成本 WARN 失效
    EXPECT_EQ(stderr_text.find("warmup_bars=0"), std::string::npos) << stderr_text;
    EXPECT_EQ(stderr_text.find("estimated_seconds=0.000"), std::string::npos) << stderr_text;
}

/**
 * 锚点按实例独立：两个品种的历史起点/深度不同，各自的回放从自己的最早一根开始，窗口内容
 * 来自各自的归档历史，不跨实例拼接（组件文档 §5.3 AC）。
 *
 * 同时锁「`begin_sec` 跳跃只记 INFO」：B 的历史刻意缺一根（无成交的桶不生成 bar），
 * 回放照常继续、只打印 gap 计数。
 */
TEST_F(BarStrategyFrameworkTest, WarmupAnchorsAreIndependentPerInstrument) {
    const auto total_a = static_cast<std::size_t>(kTestMaxLookback) + 8;
    const auto total_b = static_cast<std::size_t>(kTestMaxLookback) + 5;
    const std::int32_t b_first_begin = kHistoryFirstBeginSec + 20 * 60;
    std::vector<dztrader::bar::Bar> history_a = make_archive_history(total_a);
    std::vector<dztrader::bar::Bar> history_b = make_archive_history(total_b, b_first_begin, 900.0);
    // B 的历史缺第 5 根（begin_sec 跳跃）：无成交桶，回放照常、只记 INFO
    history_b.erase(history_b.begin() + 4);
    write_config({std::string(kInstrumentA), std::string(kInstrumentB)}, kTestMaxLookback);
    fake_history_[std::string(kInstrumentA)] = history_a;
    fake_history_[std::string(kInstrumentB)] = history_b;
    fake_metadata_available_ = false;

    testing::internal::CaptureStderr();
    start_strategy_with_injected_dependencies();
    ASSERT_TRUE(wait_for_count(kInstrumentA, EventKind::BarClose, 8));
    ASSERT_TRUE(wait_for_count(kInstrumentB, EventKind::BarClose, 4));
    stop_strategy();
    const std::string stderr_text = testing::internal::GetCapturedStderr();
    ASSERT_EQ(exit_code_, 0);

    EXPECT_NE(stderr_text.find("warmup history gaps | instrument=rb2605 gap_count=1"),
              std::string::npos)
        << stderr_text;
    // 每个实例的 bar_index 从自己的窗口推进，互不干扰
    const auto last_update_a = recorder().count(kInstrumentA, EventKind::BarUpdate);
    const auto last_update_b = recorder().count(kInstrumentB, EventKind::BarUpdate);
    EXPECT_EQ(last_update_a, 8U * 4U);
    EXPECT_EQ(last_update_b, 4U * 4U);
    const std::vector<ObservedEvent> events_a = recorder().for_instrument(kInstrumentA);
    const std::vector<ObservedEvent> events_b = recorder().for_instrument(kInstrumentB);
    ASSERT_FALSE(events_a.empty());
    ASSERT_FALSE(events_b.empty());
    // 两个实例的最终窗口分别等于各自归档的尾部
    EXPECT_DOUBLE_EQ(events_a.back().close, history_a.back().close);
    EXPECT_DOUBLE_EQ(events_b.back().close, history_b.back().close);
    EXPECT_NE(events_a.back().close, events_b.back().close) << "windows must not be cross-stitched";
}

/// 历史深度不足可读深度：先填可用部分（静默），余下由 live tick 补足（INFO + 行为一致）
TEST_F(BarStrategyFrameworkTest, InsufficientHistoryFillsRemainderFromLive) {
    const std::size_t available = 10;
    write_config({std::string(kInstrumentA)}, kTestMaxLookback);
    fake_history_[std::string(kInstrumentA)] = make_archive_history(available);
    fake_metadata_available_ = false;

    testing::internal::CaptureStderr();
    start_strategy_with_injected_dependencies();
    ASSERT_TRUE(wait_for_events(1));
    // live 补足剩余 22 根静默 bar：每根一根 tick（时钟顺序推进，价格单调；累计量额接着
    // 归档尾部，否则 builder 按「量额倒退」丢弃）
    const std::vector<dztrader::bar::Bar> history = make_archive_history(available);
    const std::int32_t first_begin =
        kHistoryFirstBeginSec + static_cast<std::int32_t>(available) * 60;
    double price = 300.0;
    DzVolume cum_volume = static_cast<DzVolume>(history.back().cum_volume);
    double cum_turnover = history.back().cum_turnover;
    // 补 22 根静默 bar 后窗口才装满，再补 1 根新桶的 tick 才会触发 close(31)
    for (std::int32_t index = 0; index <= kTestMaxLookback - static_cast<std::int32_t>(available);
         ++index) {
        cum_volume = static_cast<DzVolume>(cum_volume + 10);
        cum_turnover += price * 10.0;
        ASSERT_TRUE(write_tick(make_tick(
            kInstrumentA, dztrader::bar::TradingTime::to_clock(first_begin + index * 60 + 30),
            price, cum_volume, cum_turnover)));
        ++price;
    }
    // 第 33 根（bar_index == max_lookback）才产生第一个回调
    ASSERT_TRUE(wait_for_count(kInstrumentA, EventKind::BarClose, 1));
    stop_strategy();
    const std::string stderr_text = testing::internal::GetCapturedStderr();
    ASSERT_EQ(exit_code_, 0);

    EXPECT_NE(stderr_text.find("warmup history insufficient | instrument=rb2601"),
              std::string::npos)
        << stderr_text;
    EXPECT_NE(stderr_text.find("fallback=live_fill"), std::string::npos) << stderr_text;
    const std::vector<ObservedEvent> events = recorder().for_instrument(kInstrumentA);
    ASSERT_GE(events.size(), 3U);
    EXPECT_EQ(events[1].kind, EventKind::BarClose);
    EXPECT_EQ(events[1].bar_index, static_cast<std::int64_t>(kTestMaxLookback) - 1);
}

/**
 * 合约元数据源接上之后：Tier 1 盈亏按合约乘数换算、下单价格按最小变动价位对齐。
 *
 * 回放期建仓（虚拟）、回放结束后收到 live tick 触发平仓意图：归一化后的委托价必须是
 * `4500.0`（`price_tick=0.5` 向下对齐），且盈亏按 `×10` 乘数计算（此前为零的已知限制消失）。
 */
TEST_F(BarStrategyFrameworkTest, InjectedMetadataRecoversTier1AndPriceAlignment) {
    const std::size_t total = static_cast<std::size_t>(kTestMaxLookback) + 4;
    write_config({std::string(kInstrumentA)}, kTestMaxLookback, std::nullopt, 60,
                 R"([{"account_id":"CTP001","scale":1.0}])");
    const std::vector<dztrader::bar::Bar> history = make_archive_history(total);
    fake_history_[std::string(kInstrumentA)] = history;
    fake_metadata_[std::string(kInstrumentA)] =
        dztrader::InstrumentSpec{.exchange_id = "SHFE", .volume_multiple = 10, .price_tick = 0.5};

    // 回放第一根建仓 2 手（虚拟），下一根 close 回调里校验按 ×10 的浮动盈亏
    const double entry_price = history[static_cast<std::size_t>(kTestMaxLookback)].open;
    recorder().add_script(ScriptedIntent{
        .instrument = std::string(kInstrumentA),
        .bar_index = kTestMaxLookback,
        .when = EventKind::BarUpdate,
        .apply = [](dztrader::InstrumentContext& ctx) { ctx.buy(2, ctx.close[0]); },
    });
    recorder().add_script(ScriptedIntent{
        .instrument = std::string(kInstrumentA),
        .bar_index = kTestMaxLookback + 1,
        .when = EventKind::BarUpdate,
        .apply =
            [entry_price](dztrader::InstrumentContext& ctx) {
                EXPECT_DOUBLE_EQ(ctx.position_profit(), (ctx.close[0] - entry_price) * 2 * 10.0)
                    << "Tier 1 profit must use the injected volume_multiple";
            },
    });
    // live 阶段加仓：价格 4500.3 不在 tick 网格上 → 买单委托价必须向下对齐到 4500.0
    const std::int64_t live_bar = static_cast<std::int64_t>(total);
    recorder().add_script(ScriptedIntent{
        .instrument = std::string(kInstrumentA),
        .bar_index = live_bar,
        .when = EventKind::BarUpdate,
        .apply = [](dztrader::InstrumentContext& ctx) { ctx.buy(1, 4500.3); },
    });

    open_probe();
    testing::internal::CaptureStderr();
    start_strategy_with_injected_dependencies();
    // 触发 live bar：喂一根下一桶的 tick（量额继续累计）
    const dztrader::bar::Bar& anchor = history.back();
    ASSERT_TRUE(wait_for_count(kInstrumentA, EventKind::BarUpdate, 4 * 4));
    ASSERT_TRUE(write_tick(make_tick(
        kInstrumentA, dztrader::bar::TradingTime::to_clock(anchor.begin_sec + 60 + 30), 4500.3,
        static_cast<DzVolume>(anchor.cum_volume + 10), anchor.cum_turnover + 45003.0)));
    ASSERT_TRUE(wait_for_count(kInstrumentA, EventKind::BarUpdate, 4 * 4 + 1));
    stop_strategy();
    const std::string stderr_text = testing::internal::GetCapturedStderr();
    ASSERT_EQ(exit_code_, 0);
    collect_order_and_report_frames();

    EXPECT_NE(stderr_text.find("instrument metadata loaded |"), std::string::npos) << stderr_text;
    EXPECT_EQ(stderr_text.find("instrument metadata unavailable |"), std::string::npos)
        << stderr_text;
    ASSERT_EQ(orders_.size(), 1U);
    EXPECT_EQ(orders_[0].direction, DZ_DIRECTION_LONG);
    EXPECT_DOUBLE_EQ(orders_[0].price, 4500.0) << "order price must be aligned to the price tick";
    // 回放期 2 手 + live 加 1 手：差量成交 1 手
    EXPECT_EQ(orders_[0].volume, 1);
}

/// 合约元数据缺失只让**该**实例创建失败：打印诊断、进程继续、其它品种照常回放
TEST_F(BarStrategyFrameworkTest, MissingMetadataSkipsOnlyThatInstrument) {
    write_config({std::string(kInstrumentA), std::string(kInstrumentB)}, kTestMaxLookback);
    const auto total = static_cast<std::size_t>(kTestMaxLookback) + 2;
    fake_history_[std::string(kInstrumentA)] = make_archive_history(total);
    fake_history_[std::string(kInstrumentB)] = make_archive_history(total);
    // 只给 A 元数据：B 走 Missing → 实例创建失败
    fake_metadata_[std::string(kInstrumentA)] =
        dztrader::InstrumentSpec{.exchange_id = "SHFE", .volume_multiple = 10, .price_tick = 0.5};

    testing::internal::CaptureStderr();
    start_strategy_with_injected_dependencies();
    ASSERT_TRUE(wait_for_count(kInstrumentA, EventKind::BarClose, 2));
    stop_strategy();
    const std::string stderr_text = testing::internal::GetCapturedStderr();
    ASSERT_EQ(exit_code_, 0) << "a single instrument metadata failure must not stop the process";

    EXPECT_NE(stderr_text.find("instance creation skipped | instrument=rb2605 "
                               "stage=instrument_metadata status=missing"),
              std::string::npos)
        << stderr_text;
    EXPECT_EQ(recorder().for_instrument(kInstrumentB).size(), 0U)
        << "the skipped instrument must not receive any callback";
    EXPECT_FALSE(recorder().for_instrument(kInstrumentA).empty())
        << "the healthy instrument must still replay";
}

/**
 * 回放成本估算的**第一级 WARN**（> 60s）：`warmup.bars` 取配置上限 × 10 个品种时，估算约
 * 220 秒。回放阻塞在启动阶段，运维必须先被告知「这次启动会卡多久」。
 */
TEST_F(BarStrategyFrameworkTest, WarmupPlanWarnsAtSixtySeconds) {
    // 10 品种 × 预热上限：估算约 220s（> 60 但 < 300）
    const std::string stderr_text = run_warmup_cost_case(10);
    ASSERT_EQ(exit_code_, 0);

    EXPECT_NE(stderr_text.find("warmup replay plan |"), std::string::npos) << stderr_text;
    EXPECT_NE(stderr_text.find("warmup replay slow |"), std::string::npos) << stderr_text;
    EXPECT_NE(stderr_text.find(std::format(
                  "threshold={}", dztrader::bar_strategy_internal::REPLAY_SLOW_WARN_SECONDS)),
              std::string::npos)
        << stderr_text;
    EXPECT_EQ(stderr_text.find(std::format(
                  "threshold={}", dztrader::bar_strategy_internal::REPLAY_HARD_WARN_SECONDS)),
              std::string::npos)
        << stderr_text;
}

/**
 * 回放成本估算的**第二级 WARN**（> 300s）：20 个品种时估算约 440 秒，必须升级到 300 阈值
 * （只打一条，不重复刷两级告警）。
 */
TEST_F(BarStrategyFrameworkTest, WarmupPlanWarnsAtThreeHundredSeconds) {
    // 20 品种 × 预热上限：估算约 440s（> 300），且只打一条升级告警
    const std::string stderr_text = run_warmup_cost_case(20);
    ASSERT_EQ(exit_code_, 0);

    EXPECT_NE(stderr_text.find(std::format(
                  "threshold={}", dztrader::bar_strategy_internal::REPLAY_HARD_WARN_SECONDS)),
              std::string::npos)
        << stderr_text;
    EXPECT_EQ(stderr_text.find(std::format(
                  "threshold={}", dztrader::bar_strategy_internal::REPLAY_SLOW_WARN_SECONDS)),
              std::string::npos)
        << stderr_text;
}

/**
 * 失败实例的逻辑台账也要在启动时上报一次（§6 全量对账口径）。
 *
 * B 在回放期抛异常后实例失败（§4.6 不清仓），台账里可能已有 `on_init`/回放期建立的虚拟持仓；
 * `enter_live` 若跳过失败实例，UI 镜像会停留在旧值，与「全部品种处理完 → 一次性上报各账户
 * 逻辑持仓」冲突。执行门禁对它保持关闭（失败实例本来就拒单），但台账要报。
 */
TEST_F(BarStrategyFrameworkTest, FailedInstanceStillPublishesItsLedgerAtStartup) {
    write_config({std::string(kInstrumentA), std::string(kInstrumentB)}, kTestMaxLookback,
                 std::nullopt, 60, R"([{"account_id":"CTP001","scale":1.0}])");
    fake_history_[std::string(kInstrumentA)] =
        make_archive_history(static_cast<std::size_t>(kTestMaxLookback) + 4);
    fake_history_[std::string(kInstrumentB)] =
        make_archive_history(static_cast<std::size_t>(kTestMaxLookback) + 4);
    fake_metadata_available_ = false;
    recorder().set_fault(FaultPlan{.throw_in_init = {},
                                   .throw_in_bar_open = {},
                                   .throw_in_update = std::string(kInstrumentA),
                                   .throw_in_bar_close = {},
                                   .throw_in_destroy = {},
                                   .make_series_in_update = {}});
    // A 在 on_init 建立 2 手虚拟持仓（合法用法），随后首个回放 update 抛异常 → 实例失败
    recorder().add_script(ScriptedIntent{
        .instrument = std::string(kInstrumentA),
        .bar_index = -1,
        .when = EventKind::Init,
        .apply = [](dztrader::InstrumentContext& ctx) { ctx.buy(2, 100.0); },
    });

    open_probe();
    testing::internal::CaptureStderr();
    start_strategy_with_injected_dependencies();
    ASSERT_TRUE(wait_for_count(kInstrumentB, EventKind::BarClose, 4))
        << "the healthy instrument must keep replaying after the other one failed";
    stop_strategy();
    const std::string stderr_text = testing::internal::GetCapturedStderr();
    ASSERT_EQ(exit_code_, 0);
    collect_order_and_report_frames();

    EXPECT_NE(stderr_text.find("instance failed | instrument=rb2601"), std::string::npos)
        << stderr_text;
    bool saw_failed_ledger = false;
    bool saw_healthy_zero = false;
    for (const ObservedPositionReport& report : reports_) {
        if (report.instrument_id == std::string(kInstrumentA)) {
            EXPECT_EQ(report.net_volume, 2)
                << "a failed instance's ledger must still be published at startup";
            saw_failed_ledger = true;
        } else if (report.instrument_id == std::string(kInstrumentB)) {
            EXPECT_EQ(report.net_volume, 0);
            saw_healthy_zero = true;
        }
    }
    EXPECT_TRUE(saw_failed_ledger) << "no position report for the failed instrument";
    EXPECT_TRUE(saw_healthy_zero) << "no position report for the healthy instrument";
}

// ============================================================
// #162：动态管理命令与参数热更新（Seam A：UI_INPUT → OUTPUT_UI / 订单帧 / 上报帧）
// ============================================================
//
// 这一组覆盖组件文档 §7「UI_INPUT 宿主命令」与 §8「动态管理」的命令面。断言只落在**外部
// 可观察行为**上：OUTPUT_UI 的文本行、订单帧、上报帧、退出码，以及 B 观察到的回调序列。
// 命令的**解析**（文法/参数个数/取值）由 `bar_strategy_command_test.cpp` 纯单元覆盖，这里
// 只验证「解析结果真的驱动了 A 的状态与帧」。
//
// 每个用例都先静默填窗、再发第一笔回调期 tick：命令要求 `live_`（执行门禁）打开才发单，
// 而 `live_` 只在 `enter_live()` 之后为 true——与真实运维时序一致（进程起来了才有命令）。

/**
 * **命令面完整 + 解析失败不改内存**（AC「命令面完整 / 文法错误返回单行错误且内存不变」）。
 *
 * 一次跑通全部 12 条命令的「成功路径」，再验证 3 条错误命令各回一行错误、且随后的 `status`
 * 显示状态完全没变——「内存不变」用 status 的计数与退出码间接断言（B 的 ctx 不在命令层
 * 可见，但品种数/账户数/参数数就是内存状态的摘要）。
 */
TEST_F(BarStrategyFrameworkTest, CommandSurfaceCoversAllTwelveCommands) {
    write_config({std::string(kInstrumentA)}, kTestMaxLookback, std::nullopt, 60,
                 R"([{"account_id": "CTP001", "scale": 1.0}])");
    fake_metadata_[std::string(kInstrumentA)] =
        dztrader::InstrumentSpec{.exchange_id = "SHFE", .volume_multiple = 10, .price_tick = 0.5};
    // rb2605 也需要元数据，否则 instrument add 会走「元数据缺失 → 命令失败」分支
    fake_metadata_[std::string(kInstrumentB)] =
        dztrader::InstrumentSpec{.exchange_id = "SHFE", .volume_multiple = 10, .price_tick = 0.5};
    // 注入历史让 instrument add 能走「静默填窗 + 回放」而不是仅有冷启动
    fake_history_[std::string(kInstrumentB)] = make_archive_history(4);

    open_probe();
    start_strategy_with_injected_dependencies();
    ASSERT_TRUE(wait_for_events(1));
    ASSERT_EQ(warm_up(kInstrumentA), 1U);
    ASSERT_TRUE(
        write_tick(make_tick(kInstrumentA, warmed_up_clock(0), warmed_up_price(0),
                             warmed_up_volume(0), warmed_up_price(0) * warmed_up_volume(0))));
    ASSERT_TRUE(wait_for_count(kInstrumentA, EventKind::BarUpdate, 1));

    // ── 1. status ──
    ASSERT_TRUE(run_command("status", 0)) << "status did not respond";
    EXPECT_NE(ui_outputs_[0].find("status | instrument_count=1 account_count=1"), std::string::npos)
        << ui_outputs_[0];

    // ── 2. instrument list ──
    ASSERT_TRUE(run_command("instrument list", 1));
    EXPECT_NE(ui_outputs_[1].find("instrument list | count=1"), std::string::npos)
        << ui_outputs_[1];
    EXPECT_NE(ui_outputs_[1].find(std::string(kInstrumentA)), std::string::npos);

    // ── 3. account list（含 scale 与各品种 L_i）──
    ASSERT_TRUE(run_command("account list", 2));
    EXPECT_NE(ui_outputs_[2].find("account list | count=1"), std::string::npos) << ui_outputs_[2];
    EXPECT_NE(ui_outputs_[2].find("CTP001 scale=1.0"), std::string::npos) << ui_outputs_[2];

    // ── 4. param list / get / set ──
    ASSERT_TRUE(run_command("param list", 3));
    EXPECT_NE(ui_outputs_[3].find("param list | -"), std::string::npos)
        << "empty params must render as a single dash line; got " << ui_outputs_[3];

    ASSERT_TRUE(run_command("param set fast 10", 4));
    EXPECT_NE(ui_outputs_[4].find("param set ok | name=fast value=10"), std::string::npos)
        << ui_outputs_[4];

    ASSERT_TRUE(run_command("param get fast", 5));
    EXPECT_NE(ui_outputs_[5].find("param fast = 10"), std::string::npos) << ui_outputs_[5];

    // 非数值 token 按字符串存；`param get` 原样回显（不加引号）
    ASSERT_TRUE(run_command("param set mode live", 6));
    ASSERT_TRUE(run_command("param get mode", 7));
    EXPECT_NE(ui_outputs_[7].find("param mode = live"), std::string::npos) << ui_outputs_[7];

    // ── 5. instrument add（查元数据 → 订阅 → on_init → 回放 → 进入实盘）──
    //
    // 地址稳定性（AC「实例增删后的地址稳定性」）：先记下 rb2601 当前回调里的 ctx 地址，
    // 增删品种之后它必须**逐位不变**——B 在 `on_init` 保存的价格视图只绑一次，节点若搬移
    // 视图就会悬空。`instruments_` 用 `std::map` 正是为此（节点地址稳定）。
    const void* const address_a_before_add =
        recorder().for_instrument(kInstrumentA).back().ctx_address;
    ASSERT_TRUE(run_command(std::format("instrument add {}", kInstrumentB), 8));
    EXPECT_NE(ui_outputs_[8].find("instrument add ok"), std::string::npos) << ui_outputs_[8];

    ASSERT_TRUE(run_command("instrument list", 9));
    EXPECT_NE(ui_outputs_[9].find("instrument list | count=2"), std::string::npos)
        << ui_outputs_[9];

    // 让 rb2601 再走一次回调：新回调里的 ctx 地址必须与 add 之前相同
    ASSERT_TRUE(
        write_tick(make_tick(kInstrumentA, warmed_up_clock(1), warmed_up_price(1),
                             warmed_up_volume(1), warmed_up_price(1) * warmed_up_volume(1))));
    ASSERT_TRUE(wait_for_count(kInstrumentA, EventKind::BarUpdate, 2));
    EXPECT_EQ(recorder().for_instrument(kInstrumentA).back().ctx_address, address_a_before_add)
        << "instrument rb2601's ctx address must survive instrument add";

    // ── 6. account add / set / del ──
    ASSERT_TRUE(run_command("account add CTP002 0.5", 10));
    EXPECT_NE(ui_outputs_[10].find("account add ok | account=CTP002 scale=0.5"), std::string::npos)
        << ui_outputs_[10];

    ASSERT_TRUE(run_command("account set CTP002 scale 0.25", 11));
    EXPECT_NE(ui_outputs_[11].find("account set ok | account=CTP002 scale=0.25"), std::string::npos)
        << ui_outputs_[11];

    ASSERT_TRUE(run_command("account del CTP002", 12));
    EXPECT_NE(ui_outputs_[12].find("account del ok | account=CTP002"), std::string::npos)
        << ui_outputs_[12];

    // ── 7. instrument del ──
    ASSERT_TRUE(run_command(std::format("instrument del {}", kInstrumentB), 13));
    EXPECT_NE(ui_outputs_[13].find("instrument del ok"), std::string::npos) << ui_outputs_[13];

    // 删除另一个品种同样不能搬移 rb2601 的节点
    ASSERT_TRUE(
        write_tick(make_tick(kInstrumentA, warmed_up_clock(2), warmed_up_price(2),
                             warmed_up_volume(2), warmed_up_price(2) * warmed_up_volume(2))));
    ASSERT_TRUE(wait_for_count(kInstrumentA, EventKind::BarUpdate, 3));
    EXPECT_EQ(recorder().for_instrument(kInstrumentA).back().ctx_address, address_a_before_add)
        << "instrument rb2601's ctx address must survive instrument del of rb2605";

    // ── 8. 解析失败三条：都只回一行错误，且状态不变 ──
    const std::size_t status_before = ui_outputs_.size();
    ASSERT_TRUE(run_command("instrument", status_before));
    ASSERT_TRUE(run_command("bogus command", status_before + 1));
    ASSERT_TRUE(run_command("account add CTP003 0", status_before + 2));
    EXPECT_NE(ui_outputs_[status_before].find("invalid command"), std::string::npos)
        << ui_outputs_[status_before];
    EXPECT_NE(ui_outputs_[status_before + 1].find("invalid command"), std::string::npos)
        << ui_outputs_[status_before + 1];
    // 语法错误（0 scale 被解析层拒绝）→ 单行错误，账户表不变
    EXPECT_NE(ui_outputs_[status_before + 2].find("invalid command"), std::string::npos)
        << ui_outputs_[status_before + 2];

    ASSERT_TRUE(run_command("status", status_before + 3));
    EXPECT_NE(ui_outputs_[status_before + 3].find("instrument_count=1 account_count=1"),
              std::string::npos)
        << "a rejected command must not change memory; got " << ui_outputs_[status_before + 3];

    // ── 9. quit：正常码 0 ──
    ASSERT_TRUE(run_command("quit", status_before + 4));
    EXPECT_NE(ui_outputs_[status_before + 4].find("quit ok"), std::string::npos)
        << ui_outputs_[status_before + 4];
    if (strategy_thread_.joinable()) {
        strategy_thread_.join();
    }
    EXPECT_EQ(exit_code_, 0) << "quit must take the self-exit path with a normal exit code";
}

/**
 * **写盘失败不发单且逻辑不变**（AC「写盘失败不发单且逻辑不变」）。
 *
 * 让配置文件所在目录在运行期变得不可写不现实（同一进程里已打开过），因此这里用**另一条
 * 不可达路径**制造写盘失败：`instrument del` 删掉唯一的品种会让写回后的配置 `instruments`
 * 为空 → `ConfigStore::commit` 的全量校验失败（§7「写回后可重新加载」）→ 命令失败、内存与
 * 磁盘都不变。这与「磁盘满」是同一类结局（写回被拒），而可确定性触发。
 */
TEST_F(BarStrategyFrameworkTest, RejectedWritebackLeavesMemoryAndDiskUnchanged) {
    write_config({std::string(kInstrumentA)}, kTestMaxLookback, std::nullopt, 60,
                 R"([{"account_id": "CTP001", "scale": 1.0}])");
    fake_metadata_[std::string(kInstrumentA)] =
        dztrader::InstrumentSpec{.exchange_id = "SHFE", .volume_multiple = 10, .price_tick = 0.5};
    // 先把逻辑持仓做起来（3 手多），这样若命令错误地「先改内存再写盘」，就会留下可观测的
    // 差异：L_i 被清成 0、上报帧多一条。
    recorder().add_script(ScriptedIntent{
        .instrument = std::string(kInstrumentA),
        .bar_index = kTestMaxLookback,
        .when = EventKind::BarUpdate,
        .apply = [](dztrader::InstrumentContext& ctx) { ctx.buy(3, 5000.0); },
    });

    open_probe();
    start_strategy_with_injected_dependencies();
    ASSERT_TRUE(wait_for_events(1));
    ASSERT_EQ(warm_up(kInstrumentA), 1U);
    ASSERT_TRUE(
        write_tick(make_tick(kInstrumentA, warmed_up_clock(0), warmed_up_price(0),
                             warmed_up_volume(0), warmed_up_price(0) * warmed_up_volume(0))));
    ASSERT_TRUE(wait_for_count(kInstrumentA, EventKind::BarUpdate, 1));

    // 删掉唯一的品种：写回后 instruments 为空 → 校验失败
    ASSERT_TRUE(run_command(std::format("instrument del {}", kInstrumentA), 0));
    EXPECT_NE(ui_outputs_[0].find("instrument del failed"), std::string::npos) << ui_outputs_[0];
    EXPECT_NE(ui_outputs_[0].find("stage=config_write"), std::string::npos) << ui_outputs_[0];

    // 内存不变：品种仍在、bar 序号仍在（若实例被销毁，这个品种之后再发 tick 不会产生回调）
    ASSERT_TRUE(run_command("status", 1));
    EXPECT_NE(ui_outputs_[1].find("instrument_count=1"), std::string::npos) << ui_outputs_[1];

    stop_strategy();
    EXPECT_EQ(exit_code_, 0);
    collect_order_and_report_frames();
    // 只有建仓那笔意图自己的单（3 手买）；**没有**「删除」引起的平仓单——若命令先改内存再
    // 写盘失败，会多一张 `-3` 的平仓单；若它先发单再写盘，同样会多一张。
    ASSERT_EQ(orders_.size(), 1U) << "a rejected command must not place orders; got "
                                  << orders_.size();
    EXPECT_EQ(orders_[0].direction, DZ_DIRECTION_LONG);
    EXPECT_EQ(orders_[0].volume, 3);
    // 上报同样只有建仓那一条（净 3），没有清零上报
    const auto intent_only = intent_reports(1);
    ASSERT_EQ(intent_only.size(), 1U);
    EXPECT_EQ(intent_only[0].net_volume, 3);
}

/**
 * **`instrument del` 的完整顺序**（AC：「可选 sync 发 `−L_i` 差额单 → 退订 → `on_destroy` →
 * 逐账户清零上报 → 移除实例」，以及「删除立即移除并清零上报」）。
 *
 * 断言三件事，顺序即 AC 的顺序：
 *   1. `sync` 发一张 `−L_i` 的**平仓单**（方向取差量符号）；
 *   2. `on_destroy` 被调用，且它是该品种的**最后一个** B 回调（之后再无 bar 事件）；
 *   3. 清零上报出现在上报帧里（`net_volume == 0` 且 instrument 是该品种）。
 */
TEST_F(BarStrategyFrameworkTest, InstrumentDelSyncSubmitsClosingOrderThenDestroys) {
    // 两个品种：`instrument del` 删掉最后一个品种会被配置校验拒掉（`instruments` 非空），
    // 那是 `RejectedWritebackLeavesMemoryAndDiskUnchanged` 覆盖的另一条路径。这里要测的是
    // **成功删除**的完整顺序，因此留一个品种撑住配置。
    write_config({std::string(kInstrumentA), std::string(kInstrumentB)}, kTestMaxLookback,
                 std::nullopt, 60, R"([{"account_id": "CTP001", "scale": 1.0}])");
    fake_metadata_[std::string(kInstrumentA)] =
        dztrader::InstrumentSpec{.exchange_id = "SHFE", .volume_multiple = 10, .price_tick = 0.5};
    fake_metadata_[std::string(kInstrumentB)] =
        dztrader::InstrumentSpec{.exchange_id = "SHFE", .volume_multiple = 10, .price_tick = 0.5};
    recorder().add_script(ScriptedIntent{
        .instrument = std::string(kInstrumentA),
        .bar_index = kTestMaxLookback,
        .when = EventKind::BarUpdate,
        .apply = [](dztrader::InstrumentContext& ctx) { ctx.buy(4, 5000.0); },
    });

    open_probe();
    start_strategy_with_injected_dependencies();
    ASSERT_TRUE(wait_for_events(1));
    ASSERT_EQ(warm_up(kInstrumentA), 1U);
    // 先喂一笔 tick 让 tick 快照有效（同步单要取对手价/最新价）。给一个明确的盘口。
    DzTick quoting = make_tick(kInstrumentA, warmed_up_clock(0), warmed_up_price(0),
                               warmed_up_volume(0), warmed_up_price(0) * warmed_up_volume(0));
    quoting.bid_price[0] = 4999.5;
    quoting.ask_price[0] = 5000.5;
    quoting.last_price = 5000.0;
    ASSERT_TRUE(write_tick(quoting));
    ASSERT_TRUE(wait_for_count(kInstrumentA, EventKind::BarUpdate, 1));
    const std::size_t callbacks_before_del = recorder().for_instrument(kInstrumentA).size();

    ASSERT_TRUE(run_command(std::format("instrument del {} sync", kInstrumentA), 0));
    EXPECT_NE(ui_outputs_[0].find("instrument del ok"), std::string::npos) << ui_outputs_[0];

    // 删除之后该品种再有 tick 也不应产生回调（实例已从 map 移除）
    ASSERT_TRUE(
        write_tick(make_tick(kInstrumentA, warmed_up_clock(1), warmed_up_price(1),
                             warmed_up_volume(1), warmed_up_price(1) * warmed_up_volume(1))));
    ASSERT_TRUE(wait_until(
        [&] { return recorder().for_instrument(kInstrumentA).size() > callbacks_before_del; }));
    // 唯一新增的回调应当是 on_destroy
    const auto events = recorder().for_instrument(kInstrumentA);
    ASSERT_GT(events.size(), callbacks_before_del);
    EXPECT_EQ(events.back().kind, EventKind::Destroy)
        << "on_destroy must be the last user callback for a deleted instrument";

    stop_strategy();
    EXPECT_EQ(exit_code_, 0);
    collect_order_and_report_frames();

    // 1) sync 平仓单：卖方、4 手（L = 1.0 × 4）、对手价 = 买一（卖单吃买一）
    ASSERT_EQ(orders_.size(), 2U) << "1 entry order + 1 sync closing order; got " << orders_.size();
    const ObservedOrder& close_order = orders_[1];
    EXPECT_EQ(close_order.account_id, "CTP001");
    EXPECT_EQ(close_order.instrument_id, kInstrumentA);
    EXPECT_EQ(close_order.direction, DZ_DIRECTION_SHORT);
    EXPECT_EQ(close_order.volume, 4);
    EXPECT_DOUBLE_EQ(close_order.price, 4999.5)
        << "a sell sync order must take bid1 (the opponent price)";
    EXPECT_EQ(close_order.effect, DZ_POSITION_EFFECT_AUTO);

    // 2) 清零上报
    bool saw_zero = false;
    for (const ObservedPositionReport& report : reports_) {
        if (report.account_id == "CTP001" && report.instrument_id == std::string(kInstrumentA) &&
            report.net_volume == 0) {
            saw_zero = true;
        }
    }
    EXPECT_TRUE(saw_zero) << "instrument del must publish a zero logical position per account";
}

/**
 * **`account del` 立刻移除并通配清零上报**（AC「删除立即移除并清零上报」/ §8）。
 *
 * 「通配」是契约里 `instrument_id == ""` 的语义。这里断言上报帧的 `instrument_id` 为空串——
 * 若实现改成逐个品种上报，这条断言会失败（那意味着「删账户只清了启动时认识的品种」，
 * 运行期新增的品种会被漏掉）。
 */
TEST_F(BarStrategyFrameworkTest, AccountDelRemovesImmediatelyAndPublishesWildcardZero) {
    write_config({std::string(kInstrumentA)}, kTestMaxLookback, std::nullopt, 60,
                 R"([{"account_id": "CTP001", "scale": 1.0}])");
    recorder().add_script(ScriptedIntent{
        .instrument = std::string(kInstrumentA),
        .bar_index = kTestMaxLookback,
        .when = EventKind::BarUpdate,
        .apply = [](dztrader::InstrumentContext& ctx) { ctx.buy(2, 5000.0); },
    });

    open_probe();
    start_strategy();
    ASSERT_TRUE(wait_for_events(1));
    ASSERT_EQ(warm_up(kInstrumentA), 1U);
    ASSERT_TRUE(
        write_tick(make_tick(kInstrumentA, warmed_up_clock(0), warmed_up_price(0),
                             warmed_up_volume(0), warmed_up_price(0) * warmed_up_volume(0))));
    ASSERT_TRUE(wait_for_count(kInstrumentA, EventKind::BarUpdate, 1));

    ASSERT_TRUE(run_command("account del CTP001", 0));
    EXPECT_NE(ui_outputs_[0].find("account del ok | account=CTP001"), std::string::npos)
        << ui_outputs_[0];

    ASSERT_TRUE(run_command("account list", 1));
    EXPECT_NE(ui_outputs_[1].find("account list | -"), std::string::npos)
        << "the account table must be empty right after deletion; got " << ui_outputs_[1];

    stop_strategy();
    EXPECT_EQ(exit_code_, 0);
    collect_order_and_report_frames();

    bool saw_wildcard = false;
    for (const ObservedPositionReport& report : reports_) {
        if (report.account_id == "CTP001" && report.instrument_id.empty() &&
            report.net_volume == 0) {
            saw_wildcard = true;
        }
    }
    EXPECT_TRUE(saw_wildcard)
        << "account del must publish a wildcard (empty instrument_id) zero-position report";
}

/**
 * **账户命令的 `sync` 语义**（AC「`sync` 按命令引起的逻辑持仓变化量先发差额单」/ §6.2）。
 *
 * 三条命令各一格，差额口径不同：
 *   - `account add`：`qty = new_l`（新账户 `old_l = 0`）：P = 4、scale 0.5 → `L = 2`；
 *   - `account set`：`qty = new_l − old_l`：scale 0.5 → 1.0，`L` 从 2 变 4 → 差量 2；
 *   - `account del`：`qty = −old_l`：`L = 4` → 平 4 手。
 *
 * 同时覆盖「默认 nosync 不发单」：不带关键字的 `account add` 只改表、不发单。
 */
TEST_F(BarStrategyFrameworkTest, AccountCommandsSubmitOnlyCommandInducedDeltas) {
    write_config({std::string(kInstrumentA)}, kTestMaxLookback, std::nullopt, 60, "[]");
    recorder().add_script(ScriptedIntent{
        .instrument = std::string(kInstrumentA),
        .bar_index = kTestMaxLookback,
        .when = EventKind::BarUpdate,
        .apply = [](dztrader::InstrumentContext& ctx) { ctx.buy(4, 5000.0); },
    });

    open_probe();
    start_strategy();
    ASSERT_TRUE(wait_for_events(1));
    ASSERT_EQ(warm_up(kInstrumentA), 1U);
    // 有盘口才能取对手价
    DzTick quoting = make_tick(kInstrumentA, warmed_up_clock(0), warmed_up_price(0),
                               warmed_up_volume(0), warmed_up_price(0) * warmed_up_volume(0));
    quoting.bid_price[0] = 4999.5;
    quoting.ask_price[0] = 5000.5;
    quoting.last_price = 5000.0;
    ASSERT_TRUE(write_tick(quoting));
    ASSERT_TRUE(wait_for_count(kInstrumentA, EventKind::BarUpdate, 1));

    // 默认 nosync：只改表、不发单（P = 4，新账户目标 L = 2，但命令没带 sync）
    ASSERT_TRUE(run_command("account add CTP002 0.5", 0));
    ASSERT_TRUE(run_command("account list", 1));
    EXPECT_NE(ui_outputs_[1].find("CTP002 scale=0.5"), std::string::npos) << ui_outputs_[1];
    EXPECT_NE(ui_outputs_[1].find(std::string(kInstrumentA) + "=2"), std::string::npos)
        << "L = 0.5 x 4 = 2 must be visible in account list; got " << ui_outputs_[1];

    // 带 sync 的 set：0.5 → 1.0，L 2 → 4，差量买 2 手（对手价 = 卖一 5000.5）
    ASSERT_TRUE(run_command("account set CTP002 scale 1.0 sync", 2));
    // 带 sync 的 del：L = 4 → 平 4 手（对手价 = 买一 4999.5）
    ASSERT_TRUE(run_command("account del CTP002 sync", 3));

    stop_strategy();
    EXPECT_EQ(exit_code_, 0);
    collect_order_and_report_frames();

    // 只有两笔意图之外的订单帧：set 的差量 2 手买、del 的平仓 4 手卖
    ASSERT_EQ(orders_.size(), 2U) << "nosync add must not order; got " << orders_.size();
    EXPECT_EQ(orders_[0].account_id, "CTP002");
    EXPECT_EQ(orders_[0].direction, DZ_DIRECTION_LONG);
    EXPECT_EQ(orders_[0].volume, 2) << "set: L(1.0) - L(0.5) = 4 - 2";
    EXPECT_DOUBLE_EQ(orders_[0].price, 5000.5) << "a buy sync order must take ask1";

    EXPECT_EQ(orders_[1].account_id, "CTP002");
    EXPECT_EQ(orders_[1].direction, DZ_DIRECTION_SHORT);
    EXPECT_EQ(orders_[1].volume, 4) << "del: -L = -4";
    EXPECT_DOUBLE_EQ(orders_[1].price, 4999.5) << "a sell sync order must take bid1";
}

/**
 * **`market` 只影响同步单价格方式**（AC「`market` 只影响同步单价格方式」）。
 *
 * 带 `market` 时走市价单（`price_type == DZ_PRICE_MARKET`、价格 0），而不是对手价限价单。
 * 同时验证 `market` 对 `sync` 之外的路径没有副作用：不带 `sync` 的 `market` 不发单。
 */
TEST_F(BarStrategyFrameworkTest, MarketKeywordSwitchesSyncOrderToMarketPriceType) {
    write_config({std::string(kInstrumentA)}, kTestMaxLookback, std::nullopt, 60,
                 R"([{"account_id": "CTP001", "scale": 1.0}])");
    recorder().add_script(ScriptedIntent{
        .instrument = std::string(kInstrumentA),
        .bar_index = kTestMaxLookback,
        .when = EventKind::BarUpdate,
        .apply = [](dztrader::InstrumentContext& ctx) { ctx.buy(3, 5000.0); },
    });

    open_probe();
    start_strategy();
    ASSERT_TRUE(wait_for_events(1));
    ASSERT_EQ(warm_up(kInstrumentA), 1U);
    ASSERT_TRUE(
        write_tick(make_tick(kInstrumentA, warmed_up_clock(0), warmed_up_price(0),
                             warmed_up_volume(0), warmed_up_price(0) * warmed_up_volume(0))));
    ASSERT_TRUE(wait_for_count(kInstrumentA, EventKind::BarUpdate, 1));

    // nosync + market：market 无单可影响（语法合法、语义无操作）
    ASSERT_TRUE(run_command("account add CTP002 0.5 nosync market", 0));
    // sync + market：走市价单
    ASSERT_TRUE(run_command("account add CTP003 1.0 sync market", 1));

    stop_strategy();
    EXPECT_EQ(exit_code_, 0);
    collect_order_and_report_frames();

    // 两笔：建仓意图自己发的一笔（CTP001 限价）+ sync+market 命令的一笔（CTP003 市价）。
    // `nosync market` 那笔不该有单——这是「market 只对同步单生效」的正面证据。
    ASSERT_EQ(orders_.size(), 2U) << "entry order + one sync+market order; got " << orders_.size();
    EXPECT_EQ(orders_[0].account_id, "CTP001")
        << "the entry intent still goes to the attached account";
    EXPECT_EQ(orders_[0].price_type, DZ_PRICE_LIMIT);

    const ObservedOrder& market_order = orders_[1];
    EXPECT_EQ(market_order.account_id, "CTP003");
    EXPECT_EQ(market_order.price_type, DZ_PRICE_MARKET);
    EXPECT_DOUBLE_EQ(market_order.price, 0.0) << "a market order carries no limit price";
    EXPECT_EQ(market_order.volume, 3) << "P = 3, scale 1.0 -> L = 3";
}

/**
 * **`param set` 通知已进入回调阶段的实例、未进入的挂起并在首根回调后合并投递**
 * （AC「`param set`：写盘成功 → 更新 → 通知已进入回调的实例；未进入的挂起并在首根回调后
 * 合并投递」）。
 *
 * 场景：A 早已进入回调期（收到过 bar 回调），B 新加进来且刚回放完毕——它也会进入回调阶段。
 * 「挂起 + 合并投递」的正面证据在 `ParamUpdateIsDeferredUntilFirstCallback` 里（那是唯一能
 * 真正制造「未进入回调阶段」窗口的路径）；本用例锁定「已进入 → 立刻通知，且 B 读到新值」。
 */
TEST_F(BarStrategyFrameworkTest, ParamSetNotifiesLiveInstancesAndTheyReread) {
    write_config({std::string(kInstrumentA)}, kTestMaxLookback, std::nullopt, 60, "[]");
    open_probe();
    start_strategy();
    ASSERT_TRUE(wait_for_events(1));
    ASSERT_EQ(warm_up(kInstrumentA), 1U);
    ASSERT_TRUE(
        write_tick(make_tick(kInstrumentA, warmed_up_clock(0), warmed_up_price(0),
                             warmed_up_volume(0), warmed_up_price(0) * warmed_up_volume(0))));
    ASSERT_TRUE(wait_for_count(kInstrumentA, EventKind::BarUpdate, 1));

    const std::size_t updates_before = recorder().count(kInstrumentA, EventKind::ParamsUpdate);
    ASSERT_TRUE(run_command("param set fast 42", 0));
    ASSERT_TRUE(wait_for_count(kInstrumentA, EventKind::ParamsUpdate, updates_before + 1));

    const auto events = recorder().for_instrument(kInstrumentA);
    const auto update = std::ranges::find_if(
        events, [](const ObservedEvent& event) { return event.kind == EventKind::ParamsUpdate; });
    ASSERT_NE(update, events.end());
    EXPECT_DOUBLE_EQ(update->param_fast, 42.0)
        << "the notification must let B reread the new parameter value";

    stop_strategy();
    EXPECT_EQ(exit_code_, 0);
}

/**
 * **未进入回调阶段的实例挂起参数更新、首根回调后合并投递一次**
 * （AC「未进入的挂起并在首根回调后合并投递」）。
 *
 * 制造窗口的办法：`instrument add` 的实例在**回放期**收不到 B 回调（静默填窗），回放结束
 * 后才可能收到回调。但 `instrument add` 是同步完成的，测试无法在它「回放中」发命令。
 * 因此改用**启动期**这条路：静默填窗还没喂满时发 `param set`——此时实例已 `on_init` 但
 * 从未收到回调（`callback_stage_entered_ == false`），通知必须挂起；随后喂满窗口并触发首根
 * 回调，挂起的通知应该在那之后**恰好一次**。
 *
 * 两次 `param set` 都在挂起窗口内：合并投递的证据是「只有一次 `on_params_update`」，而
 * 不是两次。
 */
TEST_F(BarStrategyFrameworkTest, ParamUpdateIsDeferredUntilFirstCallbackAndCoalesced) {
    write_config({std::string(kInstrumentA)}, kTestMaxLookback, std::nullopt, 60, "[]");
    open_probe();
    start_strategy();
    ASSERT_TRUE(wait_for_events(1));  // on_init 已发生

    // 静默填窗期：实例未进入回调阶段
    ASSERT_EQ(recorder().count(kInstrumentA, EventKind::ParamsUpdate), 0U);
    ASSERT_TRUE(run_command("param set fast 7", 0));
    ASSERT_TRUE(run_command("param set fast 8", 1));
    EXPECT_EQ(recorder().count(kInstrumentA, EventKind::ParamsUpdate), 0U)
        << "updates must be deferred while the instance has not entered the callback stage";

    // 填满窗口 + 第一笔回调期 tick → 首根回调之后合并投递一次
    ASSERT_EQ(warm_up(kInstrumentA), 1U);
    ASSERT_TRUE(
        write_tick(make_tick(kInstrumentA, warmed_up_clock(0), warmed_up_price(0),
                             warmed_up_volume(0), warmed_up_price(0) * warmed_up_volume(0))));
    ASSERT_TRUE(wait_for_count(kInstrumentA, EventKind::BarUpdate, 1));
    ASSERT_TRUE(wait_for_count(kInstrumentA, EventKind::ParamsUpdate, 1));

    // 再喂一根：不应再有挂起的投递（合并只投一次）
    ASSERT_TRUE(
        write_tick(make_tick(kInstrumentA, warmed_up_clock(1), warmed_up_price(1),
                             warmed_up_volume(1), warmed_up_price(1) * warmed_up_volume(1))));
    ASSERT_TRUE(wait_for_count(kInstrumentA, EventKind::BarUpdate, 2));
    EXPECT_EQ(recorder().count(kInstrumentA, EventKind::ParamsUpdate), 1U)
        << "two pending updates must coalesce into a single delivery";

    // 合并投递读到的是**最后一次**的值
    const auto events = recorder().for_instrument(kInstrumentA);
    const auto update = std::ranges::find_if(
        events, [](const ObservedEvent& event) { return event.kind == EventKind::ParamsUpdate; });
    ASSERT_NE(update, events.end());
    EXPECT_DOUBLE_EQ(update->param_fast, 8.0) << "the coalesced delivery must see the latest value";

    stop_strategy();
    EXPECT_EQ(exit_code_, 0);
}

/**
 * **`quit` 走引擎自主退出路径（正常码 0），`on_stop` 是最后一个用户回调**
 * （AC「`quit` 走引擎自主退出路径（正常码），`on_stop` 是最后一个用户回调」）。
 *
 * curl 断言：
 *   - 进程退出码 0（不是启动失败的 1，也不是异常路径的负码）；
 *   - 退出之后 `on_destroy` 已经跑过（`on_stop` → 逐品种 `on_destroy` 是引擎的自退出路径）；
 *   - 退出之后不再有任何 bar 回调（写 tick 也不该有反应）。
 */
TEST_F(BarStrategyFrameworkTest, QuitRoutesThroughSelfExitWithNormalCode) {
    write_config({std::string(kInstrumentA)}, kTestMaxLookback, std::nullopt, 60, "[]");
    open_probe();
    start_strategy();
    ASSERT_TRUE(wait_for_events(1));
    ASSERT_EQ(warm_up(kInstrumentA), 1U);
    ASSERT_TRUE(
        write_tick(make_tick(kInstrumentA, warmed_up_clock(0), warmed_up_price(0),
                             warmed_up_volume(0), warmed_up_price(0) * warmed_up_volume(0))));
    ASSERT_TRUE(wait_for_count(kInstrumentA, EventKind::BarUpdate, 1));

    // `run_command` 会在等待响应时反复唤醒策略（测试进程无 master，订阅者列表为空）
    ASSERT_TRUE(run_command("quit", 0));
    EXPECT_NE(ui_outputs_[0].find("quit ok"), std::string::npos) << ui_outputs_[0];
    if (strategy_thread_.joinable()) {
        strategy_thread_.join();
    }
    EXPECT_EQ(exit_code_, 0) << "quit is a normal exit, not a startup failure";

    const auto events = recorder().for_instrument(kInstrumentA);
    ASSERT_FALSE(events.empty());
    EXPECT_EQ(events.back().kind, EventKind::Destroy)
        << "on_destroy is dispatched by the self-exit path (on_stop)";

    // 退出之后进程已结束：再写 tick 不会产生任何回调（`on_destroy` 真的是最后一个）
    const std::size_t after_quit = recorder().for_instrument(kInstrumentA).size();
    EXPECT_TRUE(
        write_tick(make_tick(kInstrumentA, warmed_up_clock(1), warmed_up_price(1),
                             warmed_up_volume(1), warmed_up_price(1) * warmed_up_volume(1))));
    wake_strategy();
    std::this_thread::sleep_for(50ms);
    EXPECT_EQ(recorder().for_instrument(kInstrumentA).size(), after_quit)
        << "no user callback may run after the self-exit path finished";
}

/**
 * **`instrument add` 的元数据缺失让命令失败、进程继续**（AC「查元数据（必需字段缺失则命令
 * 失败、进程继续）」）。
 *
 * 与启动期的 `MissingMetadataSkipsOnlyThatInstrument` 是同一口径的运行期版本：命令回一行
 * 错误（并推 UI 通知），已存在的品种照常工作，进程退出码仍是 0。
 */
TEST_F(BarStrategyFrameworkTest, InstrumentAddFailsOnMissingMetadataAndProcessContinues) {
    write_config({std::string(kInstrumentA)}, kTestMaxLookback, std::nullopt, 60, "[]");
    // 只给 A 元数据：add B 时 Missing → 命令失败
    fake_metadata_[std::string(kInstrumentA)] =
        dztrader::InstrumentSpec{.exchange_id = "SHFE", .volume_multiple = 10, .price_tick = 0.5};
    fake_history_[std::string(kInstrumentB)] = make_archive_history(4);

    open_probe();
    start_strategy_with_injected_dependencies();
    ASSERT_TRUE(wait_for_events(1));
    ASSERT_EQ(warm_up(kInstrumentA), 1U);
    ASSERT_TRUE(
        write_tick(make_tick(kInstrumentA, warmed_up_clock(0), warmed_up_price(0),
                             warmed_up_volume(0), warmed_up_price(0) * warmed_up_volume(0))));
    ASSERT_TRUE(wait_for_count(kInstrumentA, EventKind::BarUpdate, 1));

    ASSERT_TRUE(run_command(std::format("instrument add {}", kInstrumentB), 0));
    EXPECT_NE(ui_outputs_[0].find("instrument add failed"), std::string::npos) << ui_outputs_[0];
    EXPECT_NE(ui_outputs_[0].find("notify=instrument_metadata_unavailable"), std::string::npos)
        << ui_outputs_[0];

    // 进程继续：原品种照常收 tick
    const std::size_t updates_before = recorder().count(kInstrumentA, EventKind::BarUpdate);
    ASSERT_TRUE(
        write_tick(make_tick(kInstrumentA, warmed_up_clock(1), warmed_up_price(1),
                             warmed_up_volume(1), warmed_up_price(1) * warmed_up_volume(1))));
    ASSERT_TRUE(wait_for_count(kInstrumentA, EventKind::BarUpdate, updates_before + 1));
    // 新账户的命令同样能正常工作（证明命令层没有卡住）
    ASSERT_TRUE(run_command("status", 1));
    EXPECT_NE(ui_outputs_[1].find("instrument_count=1"), std::string::npos) << ui_outputs_[1];

    stop_strategy();
    EXPECT_EQ(exit_code_, 0);

    // 元数据缺失是关键错误：额外推了 UI 通知
    collect_order_and_report_frames();
    bool saw_notification = false;
    for (const std::string& notification : ui_notifications_) {
        if (notification.find("instrument_metadata_unavailable") != std::string::npos) {
            saw_notification = true;
        }
    }
    EXPECT_TRUE(saw_notification)
        << "a failed instrument add must also push a UI notification; notifications="
        << static_cast<std::size_t>(ui_notifications_.size());

    // 关键错误用 ERROR 级（「实例建不起来」是操作员必须立刻处理的），不是 WARN
    bool saw_error_level = false;
    for (const std::string& notification : ui_notifications_) {
        if (notification.find("instrument_metadata_unavailable") != std::string::npos &&
            notification.find("\"level\":\"error\"") != std::string::npos) {
            saw_error_level = true;
        }
    }
    EXPECT_TRUE(saw_error_level)
        << "an instance that cannot be created must notify at error level; payload sample="
        << (ui_notifications_.empty() ? std::string("<none>") : ui_notifications_.front());
}

/**
 * **超长命令结果按 SDK 页上限截断**（AC「命令结果写 OUTPUT_UI；超长按 SDK 页上限截断」）。
 *
 * `param list` 是唯一能稳定造出超长结果的命令：塞进足够多/足够长的参数后，响应超过单页
 * payload 上限。SDK 的截断在 `dz_output_ui` 内部完成（`output_limit.h`），因此这里断言的是
 * 「截断后仍写出了一帧、且长度等于页上限」——不是「命令失败」。
 */
TEST_F(BarStrategyFrameworkTest, OversizedCommandOutputIsTruncatedToPageLimit) {
    // 512 个参数 × 每行约 200 字节 ≈ 100KB；测试通道页 1MB，因此还不到上限。
    // 用 2000 个参数（每行约 200B ≈ 400KB）仍然小于 1MB —— 改成把参数名拉长：每行 ~5KB × 400
    // ≈ 2MB，必然超过 1MB 页上限。
    std::string params_json = "{";
    for (int index = 0; index < 400; ++index) {
        if (index > 0) {
            params_json += ", ";
        }
        params_json += std::format("\"p{}\": \"{}\"", index, std::string(4000, 'x'));
    }
    params_json += "}";
    write_config({std::string(kInstrumentA)}, kTestMaxLookback, std::nullopt, 60, "[]", "", "",
                 params_json);

    open_probe();
    start_strategy();
    ASSERT_TRUE(wait_for_events(1));

    ASSERT_TRUE(run_command("param list", 0));
    // 单页 payload 上限（测试通道 1MB 页）：输出被 SDK 截断到该上限
    EXPECT_LT(ui_outputs_[0].size(), 2U * 1024U * 1024U)
        << "the response must be capped by the page limit, not accumulated unbounded";
    EXPECT_GT(ui_outputs_[0].size(), 1024U * 1024U - 4096U)
        << "the truncated response should fill the page; got " << ui_outputs_[0].size();

    stop_strategy();
    EXPECT_EQ(exit_code_, 0);
}

/**
 * **`account set` 在 scale 未变时幂等**（组件文档 §8「实现期确认」：`changed=0`，不发单、不上报）。
 *
 * 报错会让脚本化运维很难写——运维脚本重放同一批命令是常态。这条路径没有任何帧可观测（不发单、
 * 不上报），唯一的证据是命令结果里的 `changed=0`。
 */
TEST_F(BarStrategyFrameworkTest, AccountSetWithUnchangedScaleIsIdempotent) {
    write_config({std::string(kInstrumentA)}, kTestMaxLookback, std::nullopt, 60,
                 R"([{"account_id": "CTP001", "scale": 0.5}])");
    recorder().add_script(ScriptedIntent{
        .instrument = std::string(kInstrumentA),
        .bar_index = kTestMaxLookback,
        .when = EventKind::BarUpdate,
        .apply = [](dztrader::InstrumentContext& ctx) { ctx.buy(4, 5000.0); },
    });

    open_probe();
    start_strategy();
    ASSERT_TRUE(wait_for_events(1));
    ASSERT_EQ(warm_up(kInstrumentA), 1U);
    ASSERT_TRUE(
        write_tick(make_tick(kInstrumentA, warmed_up_clock(0), warmed_up_price(0),
                             warmed_up_volume(0), warmed_up_price(0) * warmed_up_volume(0))));
    ASSERT_TRUE(wait_for_count(kInstrumentA, EventKind::BarUpdate, 1));

    // 带 sync 但 scale 没变：不发单、不上报，只回 changed=0
    ASSERT_TRUE(run_command("account set CTP001 scale 0.5 sync", 0));
    EXPECT_NE(ui_outputs_[0].find("changed=0"), std::string::npos) << ui_outputs_[0];

    stop_strategy();
    EXPECT_EQ(exit_code_, 0);
    collect_order_and_report_frames();

    // 唯一的单来自建仓意图（1 张）；幂等 set 不该有同步单
    ASSERT_EQ(orders_.size(), 1U) << "an unchanged scale must not place a sync order; got "
                                  << orders_.size();

    // 上报只有「进入 live 的基线 1 条 + 建仓 1 条」；幂等 set 不该有重报
    // （基线报 L=0，建仓后报 L=2；若 set 重报了会多一条 L=2）
    ASSERT_EQ(reports_.size(), 2U)
        << "an unchanged scale must not republish; got " << reports_.size();
    EXPECT_EQ(reports_[0].net_volume, 0);
    EXPECT_EQ(reports_[1].net_volume, 2);
}

/**
 * **`account set` / `account add` 的 `scale` 上界是运行期的 `max_scale`**（ADR 0027 决策 11：
 * `max_scale` 启动加载一次、盘中不可变）。
 *
 * 超出上限时命令失败、**不顺带放宽上限**——否则「盘中不可变」就成了一句空话（操作员只要先
 * `account add` 一个大比率，之后就能一直用它）。
 */
TEST_F(BarStrategyFrameworkTest, AccountScaleAboveMaxIsRejectedWithoutChangingMaxScale) {
    write_config({std::string(kInstrumentA)}, kTestMaxLookback, std::nullopt, 60, "[]",
                 "  \"max_scale\": 2.0,\n");

    open_probe();
    start_strategy();
    ASSERT_TRUE(wait_for_events(1));

    ASSERT_TRUE(run_command("account add CTP001 3.0", 0));
    EXPECT_NE(ui_outputs_[0].find("reason=scale_exceeds_max_scale"), std::string::npos)
        << ui_outputs_[0];

    // 账户表不变：add 失败后表仍是空的
    ASSERT_TRUE(run_command("account list", 1));
    EXPECT_NE(ui_outputs_[1].find("account list | -"), std::string::npos) << ui_outputs_[1];

    // 上界本身没有被动过：1.5（<= 2.0）应当成功
    ASSERT_TRUE(run_command("account add CTP002 1.5", 2));
    EXPECT_NE(ui_outputs_[2].find("account add ok"), std::string::npos) << ui_outputs_[2];

    stop_strategy();
    EXPECT_EQ(exit_code_, 0);
}

/**
 * **下单失败（手数越界被拒）仍更新逻辑持仓**（AC「下单失败或未成交仍更新逻辑持仓」）。
 *
 * 这是 AC 里唯一没有其它用例覆盖的一半：写帧彻底失败（shm 页满）在测试里无法确定性触发，但
 * 「单没发出去而逻辑账照常推进」这条**性质**有一条可复现的入口——账户差量手数超出
 * `DzVolume` 上限时 `order_volume()` 返回 `nullopt`，单被拒（`order rejected | …
 * reason=volume_out_of_range`），而 `PositionState` 已经按 B 的意图推进。
 *
 * 构造：B 两次 `buy(INT32_MAX)`。第一次差量恰好等于上限、发单成功；第二次差量约为上限的
 * 两倍，被拒。断言逻辑持仓 = 2×INT32_MAX（不受拒单影响），而订单帧只有第一张。
 */
TEST_F(BarStrategyFrameworkTest, RejectedOrderStillAdvancesTheLogicalLedger) {
    // 挂靠比率 2.0 + `max_scale` 2.0：一笔 `buy(INT32_MAX)` 的目标 L = 2×INT32_MAX，差量因此
    // **超过** `DzVolume` 上限 → `order_volume()` 返回 nullopt → 单被拒。这是「单没发出去」
    // 在测试里唯一可确定性复现的入口（写帧失败依赖 shm 页满，无法构造）。
    write_config({std::string(kInstrumentA)}, kTestMaxLookback, std::nullopt, 60,
                 R"([{"account_id": "CTP001", "scale": 2.0}])", "  \"max_scale\": 2.0,\n");
    recorder().add_script(ScriptedIntent{
        .instrument = std::string(kInstrumentA),
        .bar_index = kTestMaxLookback,
        .when = EventKind::BarUpdate,
        .apply =
            [](dztrader::InstrumentContext& ctx) {
                ctx.buy(kVolumeMax, 5000.0);
                // 即时记账：拒单不回流，逻辑持仓仍是本笔意图的结果
                EXPECT_EQ(ctx.current_contracts(), static_cast<dztrader::LargeVolume>(kVolumeMax))
                    << "a rejected order must not roll back the logical ledger";
            },
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
    ASSERT_EQ(exit_code_, 0);
    collect_order_and_report_frames();

    // 单被拒（差量越界），stderr 有诊断
    EXPECT_NE(stderr_text.find("order rejected"), std::string::npos) << stderr_text;
    EXPECT_NE(stderr_text.find("reason=volume_out_of_range"), std::string::npos) << stderr_text;

    // 一张单都没发出去；上报仍按**命令后的**逻辑持仓走（L = 2×INT32_MAX）
    EXPECT_TRUE(orders_.empty()) << "the oversized delta must not be submitted";
    bool saw_report = false;
    for (const ObservedPositionReport& report : reports_) {
        if (report.account_id == "CTP001" && report.instrument_id == std::string(kInstrumentA) &&
            report.net_volume == kTwiceVolumeMax) {
            saw_report = true;
        }
    }
    EXPECT_TRUE(saw_report)
        << "the logical position must be published even though no order went out";

    // 逻辑台账未被拒单回滚：下一次回调里读到的持仓仍是本笔意图的结果
    const auto events = recorder().for_instrument(kInstrumentA);
    const auto last = std::ranges::find_if(
        events.rbegin(), events.rend(),
        [](const ObservedEvent& event) { return event.kind == EventKind::Destroy; });
    ASSERT_NE(last, events.rend());
    EXPECT_EQ(last->current_contracts, static_cast<dztrader::LargeVolume>(kVolumeMax))
        << "the ledger must keep the intent even though the order was rejected";
}

/**
 * **`instrument add` 补报该品种各账户逻辑持仓**（AC「instrument add … → 补报持仓 → 进入实盘」）。
 *
 * 现有 `CommandSurfaceCoversAllTwelveCommands` 里新加品种的逻辑持仓恰好是 0，看不出「补报
 * 有没有发生」。这里让新加品种在**加入后的回调期**建仓，再让账户命令触发一次重报——但更直接
 * 的证据是：`instrument add` 成功之后立刻断言该品种在上报帧里出现过。
 *
 * 用冷启动（无历史）的品种：它加进来时逻辑持仓为 0，补报会报一条 `net_volume == 0`——
 * 「0 也报」正是全量对账的口径（`publish_all_positions` 的注释）。这足以证明补报发生了。
 */
TEST_F(BarStrategyFrameworkTest, InstrumentAddPublishesTheNewInstrumentPosition) {
    write_config({std::string(kInstrumentA)}, kTestMaxLookback, std::nullopt, 60,
                 R"([{"account_id": "CTP001", "scale": 1.0}])");
    fake_metadata_[std::string(kInstrumentA)] =
        dztrader::InstrumentSpec{.exchange_id = "SHFE", .volume_multiple = 10, .price_tick = 0.5};
    fake_metadata_[std::string(kInstrumentB)] =
        dztrader::InstrumentSpec{.exchange_id = "SHFE", .volume_multiple = 10, .price_tick = 0.5};
    fake_history_[std::string(kInstrumentB)] = make_archive_history(4);

    open_probe();
    start_strategy_with_injected_dependencies();
    ASSERT_TRUE(wait_for_events(1));
    ASSERT_EQ(warm_up(kInstrumentA), 1U);
    ASSERT_TRUE(
        write_tick(make_tick(kInstrumentA, warmed_up_clock(0), warmed_up_price(0),
                             warmed_up_volume(0), warmed_up_price(0) * warmed_up_volume(0))));
    ASSERT_TRUE(wait_for_count(kInstrumentA, EventKind::BarUpdate, 1));

    ASSERT_TRUE(run_command(std::format("instrument add {}", kInstrumentB), 0));
    EXPECT_NE(ui_outputs_[0].find("instrument add ok"), std::string::npos) << ui_outputs_[0];

    stop_strategy();
    EXPECT_EQ(exit_code_, 0);
    collect_order_and_report_frames();

    // 补报：新加品种的上报帧出现在通道里（此处逻辑持仓为 0，正好验证「0 也报」）
    bool saw_added_report = false;
    for (const ObservedPositionReport& report : reports_) {
        if (report.instrument_id == std::string(kInstrumentB)) {
            saw_added_report = true;
            EXPECT_EQ(report.net_volume, 0);
        }
    }
    EXPECT_TRUE(saw_added_report)
        << "instrument add must publish the new instrument's logical position per account";
}

/**
 * **`instrument del <id> sync` 在无行情时报出「同步单没发全」**（组件文档 §6.2「无近期行情的
 * 品种跳过并输出错误行；逻辑持仓仍按命令更新」）。
 *
 * 这是第 1 轮 Spec 审查发现的一个真实缺口：早先实现只在 stderr 打诊断，命令结果仍回 `ok`，
 * 操作员会以为平仓单已经发出去了。现在结果文本追加一行 `sync orders incomplete | …`。
 *
 * 构造：品种 A 有持仓但**从未收到过任何 tick**（快照 invalid → `resolve_sync_price` 返回
 * Unavailable）→ `instrument del A sync` 成功但那张平仓单发不出去。
 */
TEST_F(BarStrategyFrameworkTest, InstrumentDelSyncReportsIncompleteWhenNoMarketData) {
    // 两个品种撑住配置（删最后一个会被写回校验拒掉）
    write_config({std::string(kInstrumentA), std::string(kInstrumentB)}, kTestMaxLookback,
                 std::nullopt, 60, R"([{"account_id": "CTP001", "scale": 1.0}])");
    fake_metadata_[std::string(kInstrumentA)] =
        dztrader::InstrumentSpec{.exchange_id = "SHFE", .volume_multiple = 10, .price_tick = 0.5};
    fake_metadata_[std::string(kInstrumentB)] =
        dztrader::InstrumentSpec{.exchange_id = "SHFE", .volume_multiple = 10, .price_tick = 0.5};
    // A 在 on_init 建仓（虚拟成交，进入 live 后补报），但**一笔 tick 都不喂** → 无行情快照
    recorder().add_script(ScriptedIntent{
        .instrument = std::string(kInstrumentA),
        .bar_index = -1,
        .when = EventKind::Init,
        .apply = [](dztrader::InstrumentContext& ctx) { ctx.buy(3, 5000.0); },
    });

    open_probe();
    start_strategy_with_injected_dependencies();
    ASSERT_TRUE(wait_for_events(2));

    ASSERT_TRUE(run_command(std::format("instrument del {} sync", kInstrumentA), 0));
    // 命令成功（写盘与台账都已推进）
    EXPECT_NE(ui_outputs_[0].find("instrument del ok"), std::string::npos) << ui_outputs_[0];
    // 但同步单没发出去，结果里必须说出来
    EXPECT_NE(ui_outputs_[0].find("sync orders incomplete"), std::string::npos) << ui_outputs_[0];
    EXPECT_NE(ui_outputs_[0].find("skipped_no_market_data=1"), std::string::npos) << ui_outputs_[0];

    stop_strategy();
    EXPECT_EQ(exit_code_, 0);
    collect_order_and_report_frames();
    // 一张单都没发：无行情时不能凭空发一张价格 0 的单
    EXPECT_TRUE(orders_.empty()) << "no order may be placed without market data";
}

/**
 * **启动期创建失败的品种可以在运行期用 `instrument add` 补救**（第 1 轮 Spec 审查的 finding 3）。
 *
 * 场景：配置里写了 rb2605，但启动时元数据缺失 → 实例创建失败（进程继续）。该 id **仍在配置
 * 文件里**（框架不悄悄删用户的配置）。之后元数据源恢复（fake 里补上），`instrument add`
 * 必须能把它建起来——若写回无条件 `push_back`，会撞上配置层的 `duplicate` 校验并回一句与
 * 真实原因无关的错误。
 */
TEST_F(BarStrategyFrameworkTest, InstrumentAddRecoversAnInstrumentThatFailedAtStartup) {
    write_config({std::string(kInstrumentA), std::string(kInstrumentB)}, kTestMaxLookback);
    fake_metadata_[std::string(kInstrumentA)] =
        dztrader::InstrumentSpec{.exchange_id = "SHFE", .volume_multiple = 10, .price_tick = 0.5};
    // rb2605 启动期**没有**元数据 → 实例创建失败，但 id 留在配置里
    // （fake_metadata_ 里暂时不放 rb2605）

    open_probe();
    testing::internal::CaptureStderr();
    start_strategy_with_injected_dependencies();
    ASSERT_TRUE(wait_for_events(1));  // 只有 rb2601 的 on_init
    const std::string startup_stderr = testing::internal::GetCapturedStderr();
    // 前置条件：启动期 rb2605 确实因元数据缺失被跳过（该 id 仍在配置里）
    ASSERT_NE(startup_stderr.find("instance creation skipped | instrument=rb2605"),
              std::string::npos)
        << startup_stderr;

    // 元数据源恢复：现在 add 应当成功（写回不得因 duplicate 失败）
    fake_metadata_[std::string(kInstrumentB)] =
        dztrader::InstrumentSpec{.exchange_id = "SHFE", .volume_multiple = 10, .price_tick = 0.5};
    fake_history_[std::string(kInstrumentB)] = make_archive_history(4);

    ASSERT_TRUE(run_command(std::format("instrument add {}", kInstrumentB), 0));
    EXPECT_NE(ui_outputs_[0].find("instrument add ok"), std::string::npos)
        << "recovering a startup-failed instrument must not hit a duplicate-config error; got "
        << ui_outputs_[0];

    stop_strategy();
    EXPECT_EQ(exit_code_, 0);
}

// ============================================================
// #163 竞价相位：ctx.is_auction()
// ============================================================

/**
 * **含 Open 竞价窗口的小节表（Seam B 注入）**：窗口内事件序与相位写入（#163 AC1/AC2/AC3）。
 *
 * 用 `day_auction_session_table()`（日盘 09:00–15:00 + 08:59:00–08:59:59 Open 窗口）驱动：
 *
 *   1. 静默填窗 32 根（09:00–09:31，B 零回调）；
 *   2. 次日 08:59:15 竞价首笔：`close(上一根普通 bar) → open(新首根) → update`，三者相位均
 *      为 true——**close 也是 true**，因为相位属于触发 tick、不属于被交出的 bar（ADR 0032）；
 *   3. 后续竞价 tick 只 update、相位 true；
 *   4. 09:00:00 连续交易首笔：同一根 bar 继续、只 update、相位转 false；
 *   5. 09:01:00 下一根：close/open/update 相位 false。
 *
 * 窗口内脚本注入的 `buy` 与连续交易里的 `buy` 各产生一张形状相同的差量单：框架不因相位
 * 拒单、缓存或延后发单（#163 AC3）。竞价窗口内的单在窗口结束**之前**就已落地（`wait_until`
 * 在写入 09:00 连续 tick 之前断言 `orders_` 非空）。
 */
TEST_F(BarStrategyFrameworkTest, AuctionWindowDrivesEventOrderAndPhaseWrites) {
    injected_session_table_ = day_auction_session_table();
    write_config({std::string(kInstrumentA)}, kTestMaxLookback, std::nullopt, 60,
                 R"([{"account_id":"CTP001","scale":1.0}])");
    fake_metadata_[std::string(kInstrumentA)] =
        dztrader::InstrumentSpec{.exchange_id = "SHFE", .volume_multiple = 10, .price_tick = 0.5};
    // 两笔意图：bar 32 的 update（竞价窗口内）与 bar 33 的 update（连续交易）
    recorder().add_script(ScriptedIntent{
        .instrument = std::string(kInstrumentA),
        .bar_index = kTestMaxLookback,
        .when = EventKind::BarUpdate,
        .apply = [](dztrader::InstrumentContext& ctx) { ctx.buy(2, ctx.close[0]); },
    });
    recorder().add_script(ScriptedIntent{
        .instrument = std::string(kInstrumentA),
        .bar_index = kTestMaxLookback + 1,
        .when = EventKind::BarUpdate,
        .apply = [](dztrader::InstrumentContext& ctx) { ctx.buy(2, ctx.close[0]); },
    });

    open_probe();
    start_strategy_with_injected_dependencies();
    ASSERT_TRUE(wait_for_events(1)) << "on_init not observed";
    ASSERT_EQ(warm_up(kInstrumentA, kDaySessionWarmupClock), 1U)
        << "no callback may fire before the window is full";

    // 次日 08:59:15 / 08:59:30 / 08:59:45：竞价首笔 + 两笔后续竞价 tick
    ASSERT_TRUE(write_tick_at_tsec(kInstrumentA, 1, kAuctionBeginSec + 15, 200.0, 1));
    ASSERT_TRUE(write_tick_at_tsec(kInstrumentA, 1, kAuctionBeginSec + 30, 201.0, 2));
    ASSERT_TRUE(write_tick_at_tsec(kInstrumentA, 1, kAuctionBeginSec + 45, 202.0, 3));
    ASSERT_TRUE(wait_for_count(kInstrumentA, EventKind::BarUpdate, 3));
    // 窗口内下单立即落地（若框架按相位缓存/延后，这里会等不到单子）
    ASSERT_TRUE(wait_until([this] {
        collect_order_and_report_frames();
        return orders_.size() >= 1;
    })) << "an auction-window intent must be placed immediately, not cached until the session "
           "opens";

    // 09:00:00 连续交易首笔 → 同桶只 update、相位 false；09:01:00 下一根 → close/open/update
    ASSERT_TRUE(write_tick_at_tsec(kInstrumentA, 1, TSEC_09_00, 203.0, 4));
    ASSERT_TRUE(write_tick_at_tsec(kInstrumentA, 1, TSEC_09_00 + 60, 204.0, 5));
    ASSERT_TRUE(wait_for_count(kInstrumentA, EventKind::BarUpdate, 5));
    ASSERT_TRUE(wait_for_count(kInstrumentA, EventKind::BarClose, 2));
    ASSERT_TRUE(wait_until([this] {
        collect_order_and_report_frames();
        return orders_.size() >= 2;
    }));
    stop_strategy();
    ASSERT_EQ(exit_code_, 0);
    collect_order_and_report_frames();

    const std::vector<ObservedEvent> events = recorder().for_instrument(kInstrumentA);
    // init + close(31) + open(32) + update×4 + close(32) + open(33) + update + destroy = 11
    ASSERT_EQ(events.size(), 11U) << "callbacks: " << describe_events(events);
    EXPECT_EQ(events[0].kind, EventKind::Init);
    EXPECT_FALSE(events[0].is_auction) << "on_init must never be in auction phase";
    EXPECT_EQ(events[0].bar_index, -1);

    const auto first = kTestMaxLookback;
    // ── AC1/AC2：竞价首笔触发 close(上一根) → open(新首根) → update，三者均为 true ──
    EXPECT_EQ(events[1].kind, EventKind::BarClose);
    EXPECT_EQ(events[1].bar_index, first - 1) << "the closed bar is the previous normal bar";
    EXPECT_TRUE(events[1].is_auction)
        << "the close triggered by the first auction tick must carry auction phase";
    EXPECT_EQ(events[2].kind, EventKind::BarOpen);
    EXPECT_EQ(events[2].bar_index, first);
    EXPECT_TRUE(events[2].is_auction);
    EXPECT_EQ(events[3].kind, EventKind::BarUpdate);
    EXPECT_EQ(events[3].bar_index, first);
    EXPECT_TRUE(events[3].is_auction);
    // ── 窗口内后续每笔只 update、相位 true ──
    EXPECT_EQ(events[4].kind, EventKind::BarUpdate);
    EXPECT_TRUE(events[4].is_auction);
    EXPECT_EQ(events[5].kind, EventKind::BarUpdate);
    EXPECT_TRUE(events[5].is_auction);
    // ── AC2：离开窗口第一笔连续 tick 只 update、相位转 false ──
    EXPECT_EQ(events[6].kind, EventKind::BarUpdate);
    EXPECT_EQ(events[6].bar_index, first) << "the first continuous tick continues the same bar";
    EXPECT_FALSE(events[6].is_auction)
        << "the first continuous-trading tick must turn the auction phase false";
    // ── 下一根 bar 的三事件相位均 false ──
    EXPECT_EQ(events[7].kind, EventKind::BarClose);
    EXPECT_EQ(events[7].bar_index, first);
    EXPECT_FALSE(events[7].is_auction);
    EXPECT_EQ(events[8].kind, EventKind::BarOpen);
    EXPECT_EQ(events[8].bar_index, first + 1);
    EXPECT_FALSE(events[8].is_auction);
    EXPECT_EQ(events[9].kind, EventKind::BarUpdate);
    EXPECT_EQ(events[9].bar_index, first + 1);
    EXPECT_FALSE(events[9].is_auction);
    EXPECT_EQ(events[10].kind, EventKind::Destroy);
    EXPECT_FALSE(events[10].is_auction) << "on_destroy is not a bar event";

    // ── AC3：窗口内下单与其它时刻同路径（各一张形状相同的差量单）──
    ASSERT_EQ(orders_.size(), 2U)
        << "both intents must produce an order frame (auction phase must not block or drop)";
    for (const ObservedOrder& order : orders_) {
        EXPECT_EQ(order.account_id, "CTP001");
        EXPECT_EQ(order.instrument_id, std::string(kInstrumentA));
        EXPECT_EQ(order.direction, DZ_DIRECTION_LONG);
        EXPECT_EQ(order.price_type, DZ_PRICE_LIMIT);
        EXPECT_EQ(order.effect, DZ_POSITION_EFFECT_AUTO);
        EXPECT_EQ(order.volume, 2);
    }
}

/**
 * **历史回放期相位恒 false**（#163 AC4）。
 *
 * 回放复用 live 的 tick→bar 实现：竞价归档 bar 的合成 tick 会被 builder 标记
 * `is_auction=true`（本用例先在本地用 builder 做控制；builder 层另有
 * `bar_strategy_replay_test.cpp` 的 `AuctionOnlyBarIsRebuiltWithArchivedLastTickSec`）。但
 * 回放不是实时竞价——A 在写 ctx 时必须抑制，B 在回放期观察到的相位一律 false。若去掉抑制，
 * 下面第一条 close/open/update（由竞价归档 bar 的合成 tick 触发）会带 true，本用例立即失败。
 */
TEST_F(BarStrategyFrameworkTest, AuctionPhaseIsSuppressedDuringReplay) {
    injected_session_table_ = day_auction_session_table();
    write_config({std::string(kInstrumentA)}, kTestMaxLookback);

    // 前 32 根静默填窗（day D 09:00–09:31）；day D+1 首根是竞价 bar：begin = 09:00 桶、
    // last_tick = 08:59:59（合成 tick 全部落在 Open 窗口内，builder 相位为 true）。
    std::vector<dztrader::bar::Bar> history = make_archive_history(kTestMaxLookback, TSEC_09_00);
    dztrader::bar::Bar auction{};
    auction.trading_day = kTradingDay + 1;
    auction.begin_sec = TSEC_09_00;
    auction.last_tick_sec = TSEC_09_00 - 1;
    auction.period_sec = 60;
    auction.open = 100.0;
    auction.high = 100.0;
    auction.low = 100.0;
    auction.close = 100.0;
    auction.volume = 8;
    auction.turnover = 800.0;
    auction.open_interest = 500;
    auction.cum_volume = 8;
    auction.cum_turnover = 800.0;
    auction.is_final = true;
    history.push_back(auction);
    // 竞价 bar 之后的普通 bar（关闭竞价 bar，并覆盖「回放里普通 bar 也是 false」）
    dztrader::bar::Bar after_auction{};
    after_auction.trading_day = kTradingDay + 1;
    after_auction.begin_sec = TSEC_09_00 + 60;
    after_auction.last_tick_sec = TSEC_09_00 + 119;
    after_auction.period_sec = 60;
    after_auction.open = 101.0;
    after_auction.high = 103.0;
    after_auction.low = 100.0;
    after_auction.close = 102.0;
    after_auction.volume = 5;
    after_auction.turnover = 510.0;
    after_auction.open_interest = 500;
    after_auction.cum_volume = 13;
    after_auction.cum_turnover = 1310.0;
    after_auction.is_final = true;
    history.push_back(after_auction);
    fake_history_[std::string(kInstrumentA)] = history;
    fake_metadata_available_ = false;

    // 本地控制（防止断言恒真）：同一根竞价归档 bar 的合成 tick 经 builder 必须标记竞价相位，
    // 且实例首笔只触发 `open → update`（ADR 0032 的「窗口内启动」形态：没有上一根可 close）。
    // 若合成 tick 不再命中 Open 窗口，本控制先失败，下面「B 看到 false」才不是恒真。
    {
        dztrader::strategy_api::test::RecordingSink control_sink;
        dztrader::bar::IntradayBarBuilder<dztrader::strategy_api::test::RecordingSink>
            control_builder(day_auction_session_table(), 60, control_sink);
        for (const DzTick& tick : dztrader::bar_strategy_internal::make_replay_ticks(auction)) {
            control_builder.on_tick(tick);
        }
        ASSERT_FALSE(control_sink.events.empty());
        EXPECT_EQ(control_sink.events.front().kind, dztrader::strategy_api::test::EventKind::Open)
            << "the first auction tick must open the auction bar (no previous bar to close)";
        EXPECT_EQ(std::ranges::count_if(control_sink.events,
                                        [](const auto& event) {
                                            return event.kind ==
                                                   dztrader::strategy_api::test::EventKind::Close;
                                        }),
                  0)
            << "an instance starting inside the auction window has no bar to close";
        for (const auto& event : control_sink.events) {
            EXPECT_TRUE(event.is_auction)
                << "control: builder must mark replay ticks of an auction bar as auction phase";
        }
    }

    testing::internal::CaptureStderr();
    start_strategy_with_injected_dependencies();
    ASSERT_TRUE(wait_for_count(kInstrumentA, EventKind::BarUpdate, 8))
        << "replay did not produce the expected callbacks";
    ASSERT_TRUE(wait_for_count(kInstrumentA, EventKind::BarClose, 2));
    stop_strategy();
    ASSERT_EQ(exit_code_, 0);
    const std::string stderr_text = testing::internal::GetCapturedStderr();
    EXPECT_NE(stderr_text.find("warmup replay complete |"), std::string::npos) << stderr_text;
    EXPECT_NE(stderr_text.find("replayed_bars=2"), std::string::npos)
        << "the two archived bars (including the auction bar) must actually be replayed: "
        << stderr_text;

    const std::vector<ObservedEvent> events = callback_stage_events(kInstrumentA);
    ASSERT_EQ(events.size(), 12U) << "callbacks: " << describe_events(events);
    // 结构断言：竞价归档 bar（bar 32）确实是 B 可见的第一根回放 bar（O=H=L=C=100）
    EXPECT_EQ(events[0].kind, EventKind::BarClose);
    EXPECT_EQ(events[0].bar_index, kTestMaxLookback - 1);
    EXPECT_EQ(events[1].kind, EventKind::BarOpen);
    EXPECT_EQ(events[1].bar_index, kTestMaxLookback);
    EXPECT_DOUBLE_EQ(events[1].close, 100.0) << "the first replayed bar must be the auction bar";
    EXPECT_EQ(events[6].kind, EventKind::BarClose);
    EXPECT_EQ(events[6].bar_index, kTestMaxLookback);
    EXPECT_EQ(events[7].kind, EventKind::BarOpen);
    EXPECT_EQ(events[7].bar_index, kTestMaxLookback + 1);

    for (const ObservedEvent& event : events) {
        EXPECT_FALSE(event.is_auction)
            << "replay must not expose auction phase | kind=" << event_kind_name(event.kind)
            << " bar_index=" << event.bar_index;
    }
}

/**
 * **非 bar 回调的相位恒 false**（#163 AC4）：`on_params_update` 没有触发 tick。
 *
 * 覆盖最刁钻的投递时机：`param set` 发生在首根回调之前（挂起），竞价首笔 tick 触发首个
 * 回调序列 `close(上一根) → open → update`，挂起的参数更新在 `close` 之后补投——此刻 ctx 里
 * 还留着这次 close 的竞价相位 true。若不显式清零，B 会在 `on_params_update` 里读到 true，
 * 而它根本不是 bar 事件。
 */
TEST_F(BarStrategyFrameworkTest, DeferredParamsUpdateSeesPhaseFalseAfterFirstAuctionCallback) {
    injected_session_table_ = day_auction_session_table();
    write_config({std::string(kInstrumentA)}, kTestMaxLookback, std::nullopt, 60, "[]", "", "",
                 R"({"fast": 1.0})");
    fake_metadata_available_ = false;
    open_probe();
    start_strategy_with_injected_dependencies();
    ASSERT_TRUE(wait_for_events(1));
    ASSERT_EQ(warm_up(kInstrumentA, kDaySessionWarmupClock), 1U)
        << "no callback may fire before the window is full";

    // 首根回调之前 `param set`：实例还没进入回调阶段 → 挂起，首根回调之后补投。
    ASSERT_TRUE(run_command("param set fast 5.0", 0)) << "param set response not observed";

    // 次日竞价首笔：close(31)（true）→ 补投挂起的 on_params_update → open(32) → update
    ASSERT_TRUE(write_tick_at_tsec(kInstrumentA, 1, kAuctionBeginSec + 15, 200.0, 1));
    ASSERT_TRUE(wait_for_count(kInstrumentA, EventKind::ParamsUpdate, 1));
    ASSERT_TRUE(wait_for_count(kInstrumentA, EventKind::BarUpdate, 1));
    stop_strategy();
    ASSERT_EQ(exit_code_, 0);

    const std::vector<ObservedEvent> events = recorder().for_instrument(kInstrumentA);
    const auto params_event = std::ranges::find_if(
        events, [](const ObservedEvent& event) { return event.kind == EventKind::ParamsUpdate; });
    ASSERT_NE(params_event, events.end()) << "callbacks: " << describe_events(events);
    EXPECT_EQ(params_event->bar_index, kTestMaxLookback - 1)
        << "the deferred update is delivered after the first close callback";
    EXPECT_FALSE(params_event->is_auction)
        << "on_params_update is not a bar event: it must never carry the auction phase";
    EXPECT_DOUBLE_EQ(params_event->param_fast, 5.0) << "B must reread the new value";
    // 前置条件：同一次竞价首笔触发的 close 确实带 true（证明上面不是恒 false 的假绿）
    EXPECT_TRUE(events[1].is_auction) << "the triggering close must carry auction phase";
}

/**
 * **退化小节表下相位恒 false**（#163 AC5；组件文档 §5.5）。
 *
 * 生产入口 v1 用内置退化表（单小节 `[0, 86399]`、无竞价窗口）：即使 tick 落在生产口径的
 * 竞价时段，也没有竞价窗口可命中，`ctx.is_auction()` 必须始终为 false。用 Seam A 默认入口
 * （`run_bar_strategy` = 退化表 + 默认归档实现）跑静默填窗，再喂 08:59:00 / 08:59:30 两笔
 * **生产口径 Open 竞价窗口内**的 tick 与 09:00:00 连续交易首笔，覆盖全部 B 回调。
 */
TEST_F(BarStrategyFrameworkTest, DegradedSessionTableKeepsAuctionPhaseFalse) {
    write_config({std::string(kInstrumentA)}, kTestMaxLookback);
    start_strategy();
    ASSERT_TRUE(wait_for_events(1));
    ASSERT_EQ(warm_up(kInstrumentA), 1U) << "no callback may fire before the window is full";

    // 价格/累计量延续 warm_up 的序列（每笔 +1 / +10；warm_up 最后一笔是 131/320）。
    // 首笔 08:59:00 换桶：close(静默填窗最后一根) → open(08:59 bar) → update；第二笔同桶只
    // update；第三笔 09:00:00 再换桶 → close(08:59 bar) → open(09:00 bar) → update。
    ASSERT_TRUE(write_tick_at_tsec(kInstrumentA, 0, kAuctionBeginSec, 132.0, 330));
    ASSERT_TRUE(write_tick_at_tsec(kInstrumentA, 0, kAuctionBeginSec + 30, 133.0, 340));
    ASSERT_TRUE(write_tick_at_tsec(kInstrumentA, 0, kAuctionBeginSec + 60, 134.0, 350));
    ASSERT_TRUE(wait_for_count(kInstrumentA, EventKind::BarUpdate, 3));
    ASSERT_TRUE(wait_for_count(kInstrumentA, EventKind::BarClose, 1));
    stop_strategy();
    ASSERT_EQ(exit_code_, 0);

    const std::vector<ObservedEvent> events = recorder().for_instrument(kInstrumentA);
    ASSERT_GE(events.size(), 7U) << "callbacks: " << describe_events(events);
    for (const ObservedEvent& event : events) {
        EXPECT_FALSE(event.is_auction)
            << "the degraded session table has no auction windows | kind="
            << event_kind_name(event.kind) << " bar_index=" << event.bar_index;
    }
}

// ============================================================
// #165 E2E 验收（Seam A）：双均线 + 固定止损策略跑通整条链路
// ============================================================
//
// 两条用例共用同一个测试策略 fixture：
//   - `E2eAcceptanceDualMaStop`：1 逻辑账户 + 2 挂靠账户（1.0 / 0.5），断言事件序、指标驱动
//     的建仓/反手/止损平仓、Tier 1 数值、订单帧、上报帧与退出码（AC2）；
//   - `E2eAccountCommandSyncSemantics`：`account add/set/del` 的 sync/nosync 语义（AC3）。

/**
 * **双均线 + 固定止损策略的 Seam A 端到端验收**（#165 AC2）。
 *
 * tick 脚本（静默填窗 32 根之后）：
 *   1. 静默填窗最后一根（bar 31）的桶内再补一笔 tick，把它的收盘价从 131 压到 108——
 *      同一桶内的 tick 不启动新 bar（B 仍零回调），但会改写 bar 31 的 close；
 *   2. 回调期三笔 tick（各占一个新桶）：bar 32 close=104、bar 33 close=115、bar 34 close=120。
 *
 * 由此驱动三条路径：
 *   - close(bar 31)：快线 124.4 > 慢线 120.35 → `buy(2)` 建多（均价 108）；
 *   - close(bar 32)：快线 119.8 < 慢线 119.95 → `sell_short(2)` 反手（P +2 → −2，一张差量单）；
 *   - close(bar 33)：空头均价 104，115 ≥ 104 + 5×1.0 → `buy_to_cover(0)` 固定止损全平。
 *
 * 断言口径全部落在外部可观察行为上：B 的回调事件序、Tier 1 查询值、订单帧、上报帧、退出码。
 */
TEST_F(BarStrategyFrameworkTest, E2eAcceptanceDualMaStop) {
    // 验收场景：1 个逻辑账户 + 2 个挂靠账户（scale 1.0 / 0.5）。
    write_config(
        {std::string(kInstrumentA)}, kTestMaxLookback,
        /*reserve_bars=*/std::nullopt, /*period_sec=*/60,
        /*accounts_json=*/
        R"([{"account_id": "CTP001", "scale": 1.0}, {"account_id": "CTP002", "scale": 0.5}])");
    // Seam B 注入假合约元数据：Tier 1 盈亏按 ×10 乘数、委托价按 1.0 的最小变动价位对齐。
    // 历史留空（`fake_history_` 不填）→ 冷启动 live，静默填窗由 `warm_up` 的 tick 补足。
    fake_metadata_[std::string(kInstrumentA)] = dztrader::InstrumentSpec{
        .exchange_id = "SHFE", .volume_multiple = 10.0, .price_tick = kDualMaPriceTick};

    open_probe();
    start_strategy_as_with_injected_dependencies<DualMaStopStrategy>();
    ASSERT_TRUE(wait_for_events(1)) << "on_init not observed";
    ASSERT_EQ(warm_up(kInstrumentA), 1U) << "no callback may fire before the window is full";
    EXPECT_EQ(recorder().count(kInstrumentA, EventKind::BarClose), 0U)
        << "the silent fill window must not deliver callbacks";

    constexpr double kEntryPrice = 108.0;
    constexpr double kReversalPrice = 104.0;
    constexpr double kStopClosePrice = 115.0;
    constexpr std::int32_t kCallbackTicks = 3;
    constexpr double kCallbackPrices[kCallbackTicks] = {kReversalPrice, kStopClosePrice, 120.0};

    // 累计量额承接 `warm_up` 的最后一笔（价格 131 / 累计量 320 / 累计额 131×320）；
    // 之后每笔按「新增手数 × 本笔价」推进，满足 builder 的累计量额单调判据。
    std::int32_t cumulative_volume = warmed_up_volume(-1);
    double cumulative_turnover = warmed_up_price(-1) * static_cast<double>(warmed_up_volume(-1));

    // bar 31 桶内补笔（tsec 1900，与 warm_up 最后一笔的 tsec 1890 同桶）
    cumulative_volume += 5;
    cumulative_turnover += kEntryPrice * 5.0;
    ASSERT_TRUE(write_tick(make_tick(kInstrumentA, warmed_up_clock(-1) + 10, kEntryPrice,
                                     cumulative_volume, cumulative_turnover)));

    for (std::int32_t index = 0; index < kCallbackTicks; ++index) {
        const std::int32_t volume = warmed_up_volume(index);
        cumulative_turnover +=
            kCallbackPrices[index] * static_cast<double>(volume - cumulative_volume);
        cumulative_volume = volume;
        ASSERT_TRUE(
            write_tick(make_tick(kInstrumentA, warmed_up_clock(index), kCallbackPrices[index],
                                 cumulative_volume, cumulative_turnover)));
    }
    ASSERT_TRUE(wait_for_count(kInstrumentA, EventKind::BarUpdate, kCallbackTicks));
    stop_strategy();
    ASSERT_EQ(exit_code_, 0) << "the strategy process must exit normally";
    collect_order_and_report_frames();

    // ── AC2-a：事件序（每笔回调期 tick 一次 close(上一根) → open(新根) → update）──
    const std::vector<ObservedEvent> events = recorder().for_instrument(kInstrumentA);
    ASSERT_EQ(events.size(), 1U + 3U * static_cast<std::size_t>(kCallbackTicks) + 1U)
        << "callbacks: " << describe_events(events);
    EXPECT_EQ(events.front().kind, EventKind::Init);
    EXPECT_EQ(events.front().bar_index, -1);
    EXPECT_EQ(events.back().kind, EventKind::Destroy);
    const auto first_callback_bar = static_cast<std::int64_t>(kTestMaxLookback);  // 32
    for (std::int32_t index = 0; index < kCallbackTicks; ++index) {
        const std::size_t base = 1U + static_cast<std::size_t>(index) * 3U;
        SCOPED_TRACE(::testing::Message() << "callback group index=" << index);
        EXPECT_EQ(events[base].kind, EventKind::BarClose);
        EXPECT_EQ(events[base].bar_index, first_callback_bar - 1 + index)
            << "each new-bar tick first closes the previous bar";
        EXPECT_EQ(events[base + 1].kind, EventKind::BarOpen);
        EXPECT_EQ(events[base + 1].bar_index, first_callback_bar + index);
        EXPECT_EQ(events[base + 2].kind, EventKind::BarUpdate);
        EXPECT_EQ(events[base + 2].bar_index, first_callback_bar + index);
        for (std::size_t offset = 0; offset < 3U; ++offset) {
            EXPECT_EQ(events[base + offset].window_size, kTestMaxLookback)
                << "callbacks only start once the window is full";
            EXPECT_EQ(events[base + offset].instrument, std::string(kInstrumentA));
        }
    }
    // 静默填窗的 bar 0–30 从来没有 close：回调期 close 数 = tick 数，首次 close 是 bar 31。
    EXPECT_EQ(recorder().count(kInstrumentA, EventKind::BarClose),
              static_cast<std::size_t>(kCallbackTicks));
    std::int64_t previous_bar = -1;
    for (const ObservedEvent& event : events) {
        if (event.kind == EventKind::Init || event.kind == EventKind::Destroy) {
            continue;
        }
        EXPECT_GE(event.bar_index, previous_bar) << "bar_index must be monotonic";
        previous_bar = event.bar_index;
    }

    // ── AC2-b：意图序列与每次意图前后的 Tier 1 精确数值（乘数 10、2 手）──
    const std::vector<IntentObservation> intents = intent_recorder().snapshot();
    ASSERT_EQ(intents.size(), 3U) << "expected buy → sell_short(reversal) → buy_to_cover(stop)";
    // 1) 建多 2 手 @108：均价 108、1 段、最大持仓 2 手、bars_since_entry = 0
    EXPECT_EQ(intents[0].label, "buy");
    EXPECT_EQ(intents[0].bar_index, first_callback_bar - 1);
    EXPECT_DOUBLE_EQ(intents[0].price, kEntryPrice);
    EXPECT_DOUBLE_EQ(intents[0].fast, 124.4);
    EXPECT_DOUBLE_EQ(intents[0].slow, 120.35) << "fast > slow is what opens the long";
    EXPECT_EQ(intents[0].before.market_position, 0);
    EXPECT_EQ(intents[0].before.current_contracts, 0);
    EXPECT_EQ(intents[0].before.max_contracts, 0);
    EXPECT_EQ(intents[0].before.bars_since_entry, -1);
    EXPECT_EQ(intents[0].after.market_position, 1);
    EXPECT_EQ(intents[0].after.current_contracts, 2);
    EXPECT_DOUBLE_EQ(intents[0].after.avg_entry_price, kEntryPrice);
    EXPECT_EQ(intents[0].after.current_entries, 1);
    EXPECT_EQ(intents[0].after.max_contracts, 2);
    EXPECT_EQ(intents[0].after.max_entries, 1);
    EXPECT_DOUBLE_EQ(intents[0].after.entry_price, kEntryPrice);
    EXPECT_EQ(intents[0].after.bars_since_entry, 0);
    EXPECT_EQ(intents[0].after.bars_since_exit, -1) << "no exit has happened yet";
    EXPECT_DOUBLE_EQ(intents[0].after.position_profit, 0.0)
        << "(108 − 108) × 2 × 10 = 0 at the entry price";
    EXPECT_DOUBLE_EQ(intents[0].after.contract_profit, 0.0);

    // 2) 反手做空 2 手 @104：P +2 → −2（一张 sell_short 意图），均价重置为 104
    EXPECT_EQ(intents[1].label, "sell_short");
    EXPECT_EQ(intents[1].bar_index, first_callback_bar);
    EXPECT_DOUBLE_EQ(intents[1].price, kReversalPrice);
    EXPECT_DOUBLE_EQ(intents[1].fast, 119.8);
    EXPECT_DOUBLE_EQ(intents[1].slow, 119.95) << "fast < slow is what flips the position";
    // 意图前：多 2 手均价 108，现价 104 → 总浮亏 (104−108)×2×10 = −80、每手 −40
    EXPECT_EQ(intents[1].before.market_position, 1);
    EXPECT_EQ(intents[1].before.current_contracts, 2);
    EXPECT_DOUBLE_EQ(intents[1].before.avg_entry_price, kEntryPrice);
    EXPECT_EQ(intents[1].before.bars_since_entry, 1);
    EXPECT_DOUBLE_EQ(intents[1].before.position_profit, -80.0);
    EXPECT_DOUBLE_EQ(intents[1].before.contract_profit, -40.0);
    // 意图后：空 2 手均价 104，反手重置均价/段数，exit_price = 104、同 bar 内 bars_since = 0
    EXPECT_EQ(intents[1].after.market_position, -1);
    EXPECT_EQ(intents[1].after.current_contracts, 2);
    EXPECT_DOUBLE_EQ(intents[1].after.avg_entry_price, kReversalPrice);
    EXPECT_EQ(intents[1].after.current_entries, 1);
    EXPECT_EQ(intents[1].after.max_contracts, 2);
    EXPECT_EQ(intents[1].after.max_entries, 1);
    EXPECT_DOUBLE_EQ(intents[1].after.entry_price, kReversalPrice);
    EXPECT_DOUBLE_EQ(intents[1].after.exit_price, kReversalPrice);
    EXPECT_EQ(intents[1].after.bars_since_entry, 0);
    EXPECT_EQ(intents[1].after.bars_since_exit, 0);
    EXPECT_DOUBLE_EQ(intents[1].after.position_profit, 0.0) << "the reversal resets the average";

    // 3) 空头固定止损：close(bar 33)=115 ≥ 104 + 5×1.0 → buy_to_cover(0) 全平
    EXPECT_EQ(intents[2].label, "buy_to_cover");
    EXPECT_EQ(intents[2].bar_index, first_callback_bar + 1);
    EXPECT_DOUBLE_EQ(intents[2].price, kStopClosePrice);
    EXPECT_DOUBLE_EQ(intents[2].fast, 117.2);
    EXPECT_DOUBLE_EQ(intents[2].slow, 120.05);
    // 意图前：空 2 手均价 104、现价 115 → 浮亏 (115−104)×(−2)×10 = −220、每手 −110
    EXPECT_EQ(intents[2].before.market_position, -1);
    EXPECT_EQ(intents[2].before.current_contracts, 2);
    EXPECT_DOUBLE_EQ(intents[2].before.avg_entry_price, kReversalPrice);
    EXPECT_EQ(intents[2].before.bars_since_entry, 1);
    EXPECT_DOUBLE_EQ(intents[2].before.position_profit, -220.0);
    EXPECT_DOUBLE_EQ(intents[2].before.contract_profit, -110.0);
    // 意图后：空仓（均价/段数/浮盈亏归零）；entry/exit 价格保留最近值、max_* 不回落
    EXPECT_EQ(intents[2].after.market_position, 0);
    EXPECT_EQ(intents[2].after.current_contracts, 0);
    EXPECT_DOUBLE_EQ(intents[2].after.avg_entry_price, 0.0);
    EXPECT_EQ(intents[2].after.current_entries, 0);
    EXPECT_EQ(intents[2].after.max_contracts, 2) << "the historical max never falls back";
    EXPECT_EQ(intents[2].after.max_entries, 1);
    EXPECT_DOUBLE_EQ(intents[2].after.entry_price, kReversalPrice)
        << "a flat ledger keeps the last entry price";
    EXPECT_DOUBLE_EQ(intents[2].after.exit_price, kStopClosePrice);
    EXPECT_EQ(intents[2].after.bars_since_entry, -1) << "no position, no reference bar";
    EXPECT_EQ(intents[2].after.bars_since_exit, 0);
    EXPECT_DOUBLE_EQ(intents[2].after.position_profit, 0.0);

    // ── AC2-c：订单帧（每账户每笔意图一张差量单；反手是一张 |ΔP| + |P_old| 的单）──
    ASSERT_EQ(orders_.size(), 6U) << "2 accounts × 3 intents; got " << orders_.size();
    struct ExpectedOrder {
        const char* account;
        DzDirection direction;
        dztrader::Volume volume;
        double price;
    };
    const ExpectedOrder expected_orders[] = {
        {"CTP001", DZ_DIRECTION_LONG, 2, kEntryPrice},
        {"CTP002", DZ_DIRECTION_LONG, 1, kEntryPrice},
        // 反手：CTP001 qty = −2 −(+2) = −4；CTP002 qty = −1 −(+1) = −2——**各一张**卖单
        {"CTP001", DZ_DIRECTION_SHORT, 4, kReversalPrice},
        {"CTP002", DZ_DIRECTION_SHORT, 2, kReversalPrice},
        // 止损全平：CTP001 买回 2 手；CTP002 买回 1 手
        {"CTP001", DZ_DIRECTION_LONG, 2, kStopClosePrice},
        {"CTP002", DZ_DIRECTION_LONG, 1, kStopClosePrice},
    };
    for (std::size_t index = 0; index < orders_.size(); ++index) {
        SCOPED_TRACE(::testing::Message() << "order index=" << index);
        const ObservedOrder& order = orders_[index];
        EXPECT_EQ(order.account_id, expected_orders[index].account);
        EXPECT_EQ(order.instrument_id, std::string(kInstrumentA));
        EXPECT_EQ(order.direction, expected_orders[index].direction);
        EXPECT_EQ(order.volume, expected_orders[index].volume);
        EXPECT_DOUBLE_EQ(order.price, expected_orders[index].price)
            << "the order price is the aligned intent limit price";
        EXPECT_EQ(order.price_type, DZ_PRICE_LIMIT);
        EXPECT_EQ(order.effect, DZ_POSITION_EFFECT_AUTO) << "every strategy intent is AUTO";
    }

    // ── AC2-d：上报帧（基线 2 条 + 每笔意图每账户一条，net_volume 精确等于 L_i）──
    ASSERT_EQ(reports_.size(), 8U)
        << "2 baseline (live entry, L=0) + 6 intent reports; got " << reports_.size();
    for (std::size_t index = 0; index < 2U; ++index) {
        EXPECT_EQ(reports_[index].instrument_id, std::string(kInstrumentA))
            << "the live-entry baseline carries the instrument id";
        EXPECT_EQ(reports_[index].net_volume, 0) << "cold start publishes 0 for every account";
    }
    EXPECT_EQ(reports_[0].account_id, "CTP001");
    EXPECT_EQ(reports_[1].account_id, "CTP002");
    const std::vector<ObservedPositionReport> mirror_reports = intent_reports(2);
    ASSERT_EQ(mirror_reports.size(), 6U);
    struct ExpectedReport {
        const char* account;
        dztrader::LargeVolume net_volume;
    };
    const ExpectedReport expected_reports[] = {
        {"CTP001", 2}, {"CTP002", 1}, {"CTP001", -2}, {"CTP002", -1}, {"CTP001", 0}, {"CTP002", 0},
    };
    for (std::size_t index = 0; index < mirror_reports.size(); ++index) {
        SCOPED_TRACE(::testing::Message() << "report index=" << index);
        EXPECT_EQ(mirror_reports[index].account_id, expected_reports[index].account);
        EXPECT_EQ(mirror_reports[index].instrument_id, std::string(kInstrumentA));
        EXPECT_EQ(mirror_reports[index].net_volume, expected_reports[index].net_volume);
    }

    // ── AC2-e：正常退出，on_destroy 恰好一次 ──
    EXPECT_EQ(recorder().count(kInstrumentA, EventKind::Destroy), 1U);
}

/**
 * **账户命令的 sync / nosync 语义**（#165 AC3；含 add / set / del 三条命令）。
 *
 * 启动时 0 个挂靠账户：双均线策略先建多 2 手（只推进逻辑台账，零订单帧），随后每条命令
 * 单独验证「改账户表 → 上报 → 可选同步单」的完整效果：
 *
 *   1. `account add CTP003 0.5 nosync`：L = 1，**不发单**，上报 L=1；
 *   2. `account add CTP004 1.0 sync`：L = 2，发买 2 手（假设该账户实际持仓为 0），上报 2；
 *   3. `account set CTP003 scale 1.0 sync`：L 1 → 2，发差量买 1 手，上报 2；
 *   4. `account set CTP004 scale 0.5 nosync`：L 2 → 1，**不发单**，上报 1；
 *   5. `account del CTP003 sync`：发卖 2 手（qty = −L），随后「账户 + 空品种」通配清零上报；
 *   6. `account del CTP004 nosync`：**不发单**，只做通配清零上报。
 *
 * 同步单价格取对手价（买 = 卖一 105、卖 = 买一 103，均已在 1.0 的价格网格上）。
 */
TEST_F(BarStrategyFrameworkTest, E2eAccountCommandSyncSemantics) {
    // 启动时 0 个挂靠账户：建仓只推进逻辑台账。
    write_config({std::string(kInstrumentA)}, kTestMaxLookback);
    fake_metadata_[std::string(kInstrumentA)] = dztrader::InstrumentSpec{
        .exchange_id = "SHFE", .volume_multiple = 10.0, .price_tick = kDualMaPriceTick};

    open_probe();
    start_strategy_as_with_injected_dependencies<DualMaStopStrategy>();
    ASSERT_TRUE(wait_for_events(1)) << "on_init not observed";
    ASSERT_EQ(warm_up(kInstrumentA), 1U) << "no callback may fire before the window is full";

    constexpr double kEntryPrice = 108.0;
    constexpr double kBid1 = 103.0;
    constexpr double kAsk1 = 105.0;

    std::int32_t cumulative_volume = warmed_up_volume(-1);
    double cumulative_turnover = warmed_up_price(-1) * static_cast<double>(warmed_up_volume(-1));
    cumulative_volume += 5;
    cumulative_turnover += kEntryPrice * 5.0;
    ASSERT_TRUE(write_tick(make_tick(kInstrumentA, warmed_up_clock(-1) + 10, kEntryPrice,
                                     cumulative_volume, cumulative_turnover)));

    // 回调期第一笔 tick：close(bar 31) 建多 2 手；同一笔 tick 带盘口，供后续同步单取对手价。
    std::int32_t previous_volume = cumulative_volume;
    cumulative_volume = warmed_up_volume(0);
    cumulative_turnover += 104.0 * static_cast<double>(cumulative_volume - previous_volume);
    DzTick quoting =
        make_tick(kInstrumentA, warmed_up_clock(0), 104.0, cumulative_volume, cumulative_turnover);
    quoting.bid_price[0] = kBid1;
    quoting.ask_price[0] = kAsk1;
    ASSERT_TRUE(write_tick(quoting));
    ASSERT_TRUE(wait_for_count(kInstrumentA, EventKind::BarUpdate, 1));

    // P = 2（策略已建多），但没有挂靠账户 → 意图不产生任何帧。
    collect_order_and_report_frames();
    EXPECT_TRUE(orders_.empty()) << "an intent with no attached accounts must not trade";
    EXPECT_TRUE(reports_.empty()) << "an intent with no attached accounts must not report";
    ASSERT_EQ(intent_recorder().size(), 1U) << "the dual-MA entry must have fired exactly once";

    // ── 1. nosync：只改表 + 上报 L=1，不发单 ──
    ASSERT_TRUE(run_command("account add CTP003 0.5 nosync", 0));
    EXPECT_NE(ui_outputs_[0].find("account add ok | account=CTP003 scale=0.5"), std::string::npos)
        << ui_outputs_[0];
    collect_order_and_report_frames();
    EXPECT_EQ(orders_.size(), 0U) << "a nosync add must not place an order";
    ASSERT_EQ(reports_.size(), 1U) << "a nosync add still publishes the account's L";
    EXPECT_EQ(reports_[0].account_id, "CTP003");
    EXPECT_EQ(reports_[0].instrument_id, std::string(kInstrumentA));
    EXPECT_EQ(reports_[0].net_volume, 1) << "L = nearest(0.5 x 2) = 1";

    ASSERT_TRUE(run_command("account list", 1));
    EXPECT_NE(ui_outputs_[1].find("CTP003 scale=0.5 rb2601=1"), std::string::npos)
        << ui_outputs_[1];

    // ── 2. sync：新账户按 L=2 发买 2 手，上报 2 ──
    ASSERT_TRUE(run_command("account add CTP004 1.0 sync", 2));
    EXPECT_NE(ui_outputs_[2].find("account add ok | account=CTP004 scale=1.0"), std::string::npos)
        << ui_outputs_[2];
    collect_order_and_report_frames();
    ASSERT_EQ(orders_.size(), 1U) << "a sync add places the new account's delta order";
    ASSERT_EQ(reports_.size(), 2U);
    EXPECT_EQ(reports_[1].account_id, "CTP004");
    EXPECT_EQ(reports_[1].net_volume, 2) << "L = 1.0 x 2 = 2";

    ASSERT_TRUE(run_command("account list", 3));
    EXPECT_NE(ui_outputs_[3].find("CTP003 scale=0.5 rb2601=1"), std::string::npos)
        << ui_outputs_[3];
    EXPECT_NE(ui_outputs_[3].find("CTP004 scale=1.0 rb2601=2"), std::string::npos)
        << ui_outputs_[3];

    // ── 3. set + sync：L 1 → 2，差量买 1 手，上报 2 ──
    ASSERT_TRUE(run_command("account set CTP003 scale 1.0 sync", 4));
    EXPECT_NE(ui_outputs_[4].find("account set ok | account=CTP003 scale=1.0 changed=1"),
              std::string::npos)
        << ui_outputs_[4];
    collect_order_and_report_frames();
    ASSERT_EQ(orders_.size(), 2U);
    ASSERT_EQ(reports_.size(), 3U);
    EXPECT_EQ(reports_[2].account_id, "CTP003");
    EXPECT_EQ(reports_[2].net_volume, 2) << "the rescaled account now targets L = 2";

    // ── 4. set + nosync：L 2 → 1，不发单，上报 1 ──
    ASSERT_TRUE(run_command("account set CTP004 scale 0.5 nosync", 5));
    collect_order_and_report_frames();
    EXPECT_EQ(orders_.size(), 2U) << "a nosync set must not place an order";
    ASSERT_EQ(reports_.size(), 4U) << "a nosync set still republishes the new L";
    EXPECT_EQ(reports_[3].account_id, "CTP004");
    EXPECT_EQ(reports_[3].net_volume, 1) << "L = nearest(0.5 x 2) = 1";

    ASSERT_TRUE(run_command("account list", 6));
    EXPECT_NE(ui_outputs_[6].find("CTP003 scale=1.0 rb2601=2"), std::string::npos)
        << ui_outputs_[6];
    EXPECT_NE(ui_outputs_[6].find("CTP004 scale=0.5 rb2601=1"), std::string::npos)
        << ui_outputs_[6];

    // ── 5. del + sync：发卖 2 手（qty = −L），随后通配清零上报 ──
    ASSERT_TRUE(run_command("account del CTP003 sync", 7));
    EXPECT_NE(ui_outputs_[7].find("account del ok | account=CTP003"), std::string::npos)
        << ui_outputs_[7];
    collect_order_and_report_frames();
    ASSERT_EQ(orders_.size(), 3U) << "a sync del closes the account's L with one order";
    ASSERT_EQ(reports_.size(), 5U);
    EXPECT_EQ(reports_[4].account_id, "CTP003");
    EXPECT_TRUE(reports_[4].instrument_id.empty())
        << "account del publishes a wildcard (empty instrument id) zero-position report";
    EXPECT_EQ(reports_[4].net_volume, 0);

    ASSERT_TRUE(run_command("account list", 8));
    EXPECT_NE(ui_outputs_[8].find("account list | count=1"), std::string::npos) << ui_outputs_[8];
    EXPECT_EQ(ui_outputs_[8].find("CTP003"), std::string::npos)
        << "a deleted account must disappear immediately";
    EXPECT_NE(ui_outputs_[8].find("CTP004 scale=0.5 rb2601=1"), std::string::npos)
        << ui_outputs_[8];

    // ── 6. del + nosync：不发单，只做通配清零上报 ──
    ASSERT_TRUE(run_command("account del CTP004 nosync", 9));
    collect_order_and_report_frames();
    EXPECT_EQ(orders_.size(), 3U) << "a nosync del must not place an order";
    ASSERT_EQ(reports_.size(), 6U);
    EXPECT_EQ(reports_[5].account_id, "CTP004");
    EXPECT_TRUE(reports_[5].instrument_id.empty());
    EXPECT_EQ(reports_[5].net_volume, 0);

    ASSERT_TRUE(run_command("account list", 10));
    EXPECT_NE(ui_outputs_[10].find("account list | -"), std::string::npos) << ui_outputs_[10];

    stop_strategy();
    ASSERT_EQ(exit_code_, 0) << "the strategy process must exit normally";
    collect_order_and_report_frames();

    // 订单帧：只有 sync 命令引起的三张单（nosync 两条命令各零张）。
    ASSERT_EQ(orders_.size(), 3U) << "only sync commands may place orders; got " << orders_.size();
    struct ExpectedSyncOrder {
        const char* account;
        DzDirection direction;
        dztrader::Volume volume;
        double price;
    };
    const ExpectedSyncOrder expected_orders[] = {
        // add CTP004 1.0 sync：买 2 手 @ 卖一
        {"CTP004", DZ_DIRECTION_LONG, 2, kAsk1},
        // set CTP003 scale 1.0 sync：L 1 → 2，差量买 1 手 @ 卖一
        {"CTP003", DZ_DIRECTION_LONG, 1, kAsk1},
        // del CTP003 sync：qty = −L = −2，卖 2 手 @ 买一
        {"CTP003", DZ_DIRECTION_SHORT, 2, kBid1},
    };
    for (std::size_t index = 0; index < orders_.size(); ++index) {
        SCOPED_TRACE(::testing::Message() << "sync order index=" << index);
        const ObservedOrder& order = orders_[index];
        EXPECT_EQ(order.account_id, expected_orders[index].account);
        EXPECT_EQ(order.instrument_id, std::string(kInstrumentA));
        EXPECT_EQ(order.direction, expected_orders[index].direction);
        EXPECT_EQ(order.volume, expected_orders[index].volume);
        EXPECT_DOUBLE_EQ(order.price, expected_orders[index].price)
            << "a sync order prices at the opponent side (buy = ask1, sell = bid1)";
        EXPECT_EQ(order.price_type, DZ_PRICE_LIMIT);
        EXPECT_EQ(order.effect, DZ_POSITION_EFFECT_AUTO);
    }
}

}  // namespace
