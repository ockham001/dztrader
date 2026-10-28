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
#include <dztrader/core/env.h>
#include <dztrader/core/path.h>
#include <dztrader/core/this_process.h>
#include <dztrader/shm/channel_meta.h>
#include <dztrader/shm/named_semaphore.h>
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
};

/// @brief 故障注入计划：让 fake B 在指定品种的指定回调里抛异常（异常隔离用例）
struct FaultPlan {
    std::string throw_in_init;       ///< 该品种的 `on_init` 抛异常
    std::string throw_in_bar_open;   ///< 该品种的 `on_bar_open` 抛异常
    std::string throw_in_update;     ///< 该品种的 `on_bar_update` 抛异常
    std::string throw_in_bar_close;  ///< 该品种的 `on_bar_close` 抛异常
    std::string throw_in_destroy;    ///< 该品种的 `on_destroy` 抛异常
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
        saved_views_.clear();
    }

    /// @brief 设置故障注入计划
    void set_fault(FaultPlan plan) {
        const std::lock_guard<std::mutex> lock(mutex_);
        fault_ = std::move(plan);
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

private:
    mutable std::mutex mutex_;
    std::vector<ObservedEvent> events_;
    FaultPlan fault_;
    std::map<std::string, dztrader::SeriesView<double>> saved_views_;
};

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
        record(EventKind::Init, ctx);
        if (recorder().should_throw(ctx.instrument_id(), "init")) {
            throw std::runtime_error("injected on_init failure");
        }
    }

    void on_bar_open(dztrader::InstrumentContext& ctx) {
        record(EventKind::BarOpen, ctx);
        if (recorder().should_throw(ctx.instrument_id(), "bar_open")) {
            throw std::runtime_error("injected on_bar_open failure");
        }
    }

    void on_bar_update(dztrader::InstrumentContext& ctx) {
        record(EventKind::BarUpdate, ctx);
        if (recorder().should_throw(ctx.instrument_id(), "bar_update")) {
            throw std::runtime_error("injected on_bar_update failure");
        }
    }

    void on_bar_close(dztrader::InstrumentContext& ctx) {
        record(EventKind::BarClose, ctx);
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

/// @brief 构造一笔 tick（累计量额按调用方给的值，必须单调不减）
DzTick make_tick(std::string_view instrument,
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
    tick.date = kTradingDay;
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

    /// @brief 写一份合法配置（给定品种、1 分钟周期、0 个挂靠账户）
    void write_config(const std::vector<std::string>& instruments) {
        std::string list;
        for (const std::string& instrument : instruments) {
            if (!list.empty()) {
                list += ", ";
            }
            list += '"' + instrument + '"';
        }
        const std::string text =
            "{\n"
            "  \"schema_version\": 1,\n"
            "  \"period_sec\": 60,\n"
            "  \"instruments\": [" +
            list +
            "],\n"
            "  \"accounts\": [],\n"
            "  \"params\": {}\n"
            "}\n";
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

    std::string home_;
    std::filesystem::path config_path_;
    std::string strategy_id_;
    std::optional<std::string> orig_home_;
    std::optional<std::string> orig_md_source_;
    std::thread strategy_thread_;
    std::int32_t exit_code_ = -1;
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
    write_config({std::string(kInstrumentA)});
    start_strategy();
    ASSERT_TRUE(wait_for_events(1)) << "on_init not observed";

    // 首根 bar：open + update
    ASSERT_TRUE(write_tick(make_tick(kInstrumentA, 64830, 100.0, 10, 1000.0)));
    ASSERT_TRUE(wait_for_count(kInstrumentA, EventKind::BarUpdate, 1));
    // 下一桶：close(0) + open(1) + update
    ASSERT_TRUE(write_tick(make_tick(kInstrumentA, 64890, 101.0, 20, 2010.0)));
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

    // 第二根 bar 打开后，保存的视图读到新 bar 的价与新的窗口长度
    const ObservedEvent* second_open = nullptr;
    for (const ObservedEvent& event : events) {
        if (event.kind == EventKind::BarOpen && event.bar_index == 1) {
            second_open = &event;
        }
    }
    ASSERT_NE(second_open, nullptr) << "second bar open not observed";
    EXPECT_EQ(second_open->saved_view_size, 2);
    EXPECT_DOUBLE_EQ(second_open->saved_view_close, 101.0);
}

// ============================================================
// 回调序列、bar_index、价格值（真实帧通道）
// ============================================================

TEST_F(BarStrategyFrameworkTest, DispatchesCloseOpenUpdateInOrderWithBarIndexAndPrices) {
    write_config({std::string(kInstrumentA), std::string(kInstrumentB)});
    testing::internal::CaptureStderr();
    start_strategy();
    ASSERT_TRUE(wait_for_events(2)) << "on_init not observed for both instruments";

    // A：同桶两笔（tsec 30 / 40，bar begin=0）→ 下一桶（tsec 90，bar begin=60）
    ASSERT_TRUE(write_tick(make_tick(kInstrumentA, 64830, 100.0, 10, 1000.0)));
    ASSERT_TRUE(write_tick(make_tick(kInstrumentA, 64840, 100.5, 20, 2010.0)));
    ASSERT_TRUE(write_tick(make_tick(kInstrumentA, 64890, 101.0, 30, 3030.0)));
    // B：同桶一笔（tsec 31）→ 下一桶（tsec 92）
    ASSERT_TRUE(write_tick(make_tick(kInstrumentB, 64831, 200.0, 5, 1000.0)));
    ASSERT_TRUE(write_tick(make_tick(kInstrumentB, 64892, 200.5, 8, 1600.0)));

    // A: init + open(0) + update×2 + close(0) + open(1) + update = 7
    ASSERT_TRUE(wait_for_count(kInstrumentA, EventKind::BarUpdate, 3));
    // B: init + open(0) + update + close(0) + open(1) + update = 6
    ASSERT_TRUE(wait_for_count(kInstrumentB, EventKind::BarUpdate, 2));
    // 回调语义已稳定；再确认两个品种的第二个 bar 都已打开
    ASSERT_TRUE(wait_until([] {
        return recorder().count(kInstrumentA, EventKind::BarOpen) >= 2 &&
               recorder().count(kInstrumentB, EventKind::BarOpen) >= 2;
    }));

    const auto events_a = recorder().for_instrument(kInstrumentA);
    const auto events_b = recorder().for_instrument(kInstrumentB);
    stop_strategy();
    const std::string stderr_text = testing::internal::GetCapturedStderr();

    EXPECT_EQ(exit_code_, 0);

    // ── A 的事件序列：init → open(0) → update, update → close(0) → open(1) → update ──
    ASSERT_EQ(events_a.size(), 7U) << "instrument A callbacks: " << describe_events(events_a);
    EXPECT_EQ(events_a[0].kind, EventKind::Init);
    EXPECT_EQ(events_a[0].bar_index, -1) << "on_init sees bar_index == -1";
    EXPECT_EQ(events_a[0].window_size, 0) << "on_init sees an empty window";
    EXPECT_EQ(events_a[1].kind, EventKind::BarOpen);
    EXPECT_EQ(events_a[1].bar_index, 0);
    EXPECT_EQ(events_a[2].kind, EventKind::BarUpdate);
    EXPECT_EQ(events_a[2].bar_index, 0);
    EXPECT_DOUBLE_EQ(events_a[2].close, 100.0);
    EXPECT_EQ(events_a[3].kind, EventKind::BarUpdate);
    EXPECT_DOUBLE_EQ(events_a[3].close, 100.5);
    EXPECT_EQ(events_a[4].kind, EventKind::BarClose) << "close precedes the new bar's open";
    EXPECT_EQ(events_a[4].bar_index, 0);
    EXPECT_DOUBLE_EQ(events_a[4].close, 100.5) << "close still sees the previous bar in [0]";
    EXPECT_EQ(events_a[5].kind, EventKind::BarOpen);
    EXPECT_EQ(events_a[5].bar_index, 1);
    EXPECT_DOUBLE_EQ(events_a[5].close, 101.0);
    EXPECT_EQ(events_a[6].kind, EventKind::BarUpdate);
    EXPECT_EQ(events_a[6].bar_index, 1);

    // on_bar_open 时新根 OHLC 已等于本笔 tick 价（组件文档 §5.1）
    for (const ObservedEvent& event : {events_a[1], events_a[5]}) {
        EXPECT_DOUBLE_EQ(event.open, event.close);
        EXPECT_DOUBLE_EQ(event.high, event.close);
        EXPECT_DOUBLE_EQ(event.low, event.close);
    }

    // ── B 的事件序列：init → open(0) → update → close(0) → open(1) → update ──
    ASSERT_EQ(events_b.size(), 6U) << "instrument B callbacks: " << describe_events(events_b);
    EXPECT_EQ(events_b[0].kind, EventKind::Init);
    EXPECT_EQ(events_b[0].bar_index, -1);
    EXPECT_EQ(events_b[1].kind, EventKind::BarOpen);
    EXPECT_EQ(events_b[2].kind, EventKind::BarUpdate);
    EXPECT_EQ(events_b[3].kind, EventKind::BarClose);
    EXPECT_DOUBLE_EQ(events_b[3].close, 200.0);
    EXPECT_EQ(events_b[4].kind, EventKind::BarOpen);
    EXPECT_EQ(events_b[4].bar_index, 1);
    EXPECT_DOUBLE_EQ(events_b[4].close, 200.5);

    // ── bar_index 0 起、单调、只在 open 时前进，bar 内保持不变 ──
    for (const auto* events : {&events_a, &events_b}) {
        std::int64_t previous = -1;
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
    // 地址稳定性的机制在 #157 冻结：`InstrumentContext` 显式不可拷贝/移动（见 context.h 的
    // static_assert），节点存放在 `std::map` 里 `try_emplace` 就地构造，因此任何插入/删除
    // 都不会搬移既有节点——两品种交错回调期间各自地址恒定即是该机制的可观测证据。
    // `instrument add/del` 命令本身属 #162，届时补「增删后既有实例视图仍有效」的用例。
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

    // ── on_destroy 时机：停止时每个存活实例恰好一次（记录器在 stop 后已含 destroy）──
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
// 异常隔离：B 回调抛异常只让该实例失败，其它品种与进程继续
// ============================================================

TEST_F(BarStrategyFrameworkTest, CallbackExceptionFailsOnlyThatInstance) {
    write_config({std::string(kInstrumentA), std::string(kInstrumentB)});
    recorder().set_fault(FaultPlan{.throw_in_init = {},
                                   .throw_in_bar_open = {},
                                   .throw_in_update = std::string(kInstrumentA),
                                   .throw_in_bar_close = {},
                                   .throw_in_destroy = {}});
    start_strategy();
    ASSERT_TRUE(wait_for_events(2));

    // A 的首笔 tick 触发 open → update，update 抛异常 → A 实例失败
    ASSERT_TRUE(write_tick(make_tick(kInstrumentA, 64830, 100.0, 10, 1000.0)));
    ASSERT_TRUE(wait_for_count(kInstrumentA, EventKind::BarUpdate, 1));

    // B 继续正常回调（进程与其它品种不受影响）：
    // tick1 → open(0) + update；tick2（下一桶）→ close(0) + open(1) + update；
    // tick3（再下一桶）→ close(1) + open(2) + update
    ASSERT_TRUE(write_tick(make_tick(kInstrumentB, 64831, 200.0, 5, 1000.0)));
    ASSERT_TRUE(wait_for_count(kInstrumentB, EventKind::BarUpdate, 1));
    ASSERT_TRUE(write_tick(make_tick(kInstrumentB, 64892, 200.5, 8, 1600.0)));
    ASSERT_TRUE(wait_for_count(kInstrumentB, EventKind::BarUpdate, 2));

    // 失败实例的后续 tick 不再产生任何回调；健康实例的 tick 照常
    ASSERT_TRUE(write_tick(make_tick(kInstrumentA, 64890, 101.0, 30, 3030.0)));
    ASSERT_TRUE(write_tick(make_tick(kInstrumentB, 64950, 201.0, 9, 1800.0)));
    ASSERT_TRUE(wait_for_count(kInstrumentB, EventKind::BarUpdate, 3));
    stop_strategy();

    EXPECT_EQ(exit_code_, 0) << "process must survive a strategy callback exception";

    const auto events_a = recorder().for_instrument(kInstrumentA);
    // init → open(0) → update(抛异常) → destroy（失败实例不再接收任何 bar 回调）
    ASSERT_EQ(events_a.size(), 4U)
        << "failed instance must not receive further callbacks: " << describe_events(events_a);
    EXPECT_EQ(events_a[0].kind, EventKind::Init);
    EXPECT_EQ(events_a[1].kind, EventKind::BarOpen);
    EXPECT_EQ(events_a[2].kind, EventKind::BarUpdate);
    EXPECT_EQ(events_a[3].kind, EventKind::Destroy) << "failed instance still gets on_destroy";

    const auto events_b = recorder().for_instrument(kInstrumentB);
    // init → open(0) → update → close(0) → open(1) → update → close(1) → open(2) → update
    //      → destroy = 10
    ASSERT_EQ(events_b.size(), 10U)
        << "healthy instance must keep receiving callbacks: " << describe_events(events_b);
    EXPECT_EQ(events_b.front().kind, EventKind::Init);
    EXPECT_EQ(events_b.back().kind, EventKind::Destroy);
    std::size_t updates_b = 0;
    for (const ObservedEvent& event : events_b) {
        if (event.kind == EventKind::BarUpdate) {
            ++updates_b;
        }
    }
    EXPECT_EQ(updates_b, 3U) << "exactly one update per accepted tick on the healthy instance";
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
    write_config({std::string(kInstrumentA), std::string(kInstrumentB)});
    recorder().set_fault(FaultPlan{.throw_in_init = {},
                                   .throw_in_bar_open = std::string(kInstrumentA),
                                   .throw_in_update = {},
                                   .throw_in_bar_close = {},
                                   .throw_in_destroy = {}});
    start_strategy();
    ASSERT_TRUE(wait_for_events(2));

    // A 首笔 tick 触发 open(0) → 抛异常；本笔 tick 的 update 不应到达 B
    ASSERT_TRUE(write_tick(make_tick(kInstrumentA, 64830, 100.0, 10, 1000.0)));
    ASSERT_TRUE(wait_for_count(kInstrumentA, EventKind::BarOpen, 1));

    // B 健康：首笔 open + update
    ASSERT_TRUE(write_tick(make_tick(kInstrumentB, 64831, 200.0, 5, 1000.0)));
    ASSERT_TRUE(wait_for_count(kInstrumentB, EventKind::BarUpdate, 1));

    // 失败实例的后续 tick 不再回调
    ASSERT_TRUE(write_tick(make_tick(kInstrumentA, 64890, 101.0, 20, 2010.0)));
    ASSERT_TRUE(write_tick(make_tick(kInstrumentB, 64892, 200.5, 8, 1600.0)));
    ASSERT_TRUE(wait_for_count(kInstrumentB, EventKind::BarUpdate, 2));
    stop_strategy();

    EXPECT_EQ(exit_code_, 0);

    const auto events_a = recorder().for_instrument(kInstrumentA);
    // init → open(0)[抛异常] → destroy：失败的 open 本身不算「已完成回调」，其后无 update
    ASSERT_EQ(events_a.size(), 3U) << "callbacks: " << describe_events(events_a);
    EXPECT_EQ(events_a[0].kind, EventKind::Init);
    EXPECT_EQ(events_a[1].kind, EventKind::BarOpen);
    EXPECT_EQ(events_a[2].kind, EventKind::Destroy);
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
    write_config({std::string(kInstrumentA), std::string(kInstrumentB)});
    recorder().set_fault(FaultPlan{.throw_in_init = {},
                                   .throw_in_bar_open = {},
                                   .throw_in_update = {},
                                   .throw_in_bar_close = std::string(kInstrumentA),
                                   .throw_in_destroy = {}});
    start_strategy();
    ASSERT_TRUE(wait_for_events(2));

    // 首笔只开 bar（open + update），第二笔换桶才触发 close(0) → 抛异常
    ASSERT_TRUE(write_tick(make_tick(kInstrumentA, 64830, 100.0, 10, 1000.0)));
    ASSERT_TRUE(wait_for_count(kInstrumentA, EventKind::BarUpdate, 1));
    ASSERT_TRUE(write_tick(make_tick(kInstrumentA, 64890, 101.0, 20, 2010.0)));
    ASSERT_TRUE(wait_for_count(kInstrumentA, EventKind::BarClose, 1));

    // 失败后 A 的后续 tick 不再回调；B 健康
    ASSERT_TRUE(write_tick(make_tick(kInstrumentA, 64950, 102.0, 30, 3030.0)));
    ASSERT_TRUE(write_tick(make_tick(kInstrumentB, 64831, 200.0, 5, 1000.0)));
    ASSERT_TRUE(wait_for_count(kInstrumentB, EventKind::BarUpdate, 1));
    stop_strategy();

    EXPECT_EQ(exit_code_, 0);

    const auto events_a = recorder().for_instrument(kInstrumentA);
    // init → open(0) → update → close(0)[抛异常] → destroy
    ASSERT_EQ(events_a.size(), 5U) << "callbacks: " << describe_events(events_a);
    EXPECT_EQ(events_a[2].kind, EventKind::BarUpdate);
    EXPECT_EQ(events_a[3].kind, EventKind::BarClose);
    EXPECT_EQ(events_a[4].kind, EventKind::Destroy) << "failed instance still gets on_destroy";
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
                                   .throw_in_destroy = std::string(kInstrumentA)});
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
    write_config({std::string(kInstrumentA), std::string(kInstrumentB)});
    recorder().set_fault(FaultPlan{.throw_in_init = std::string(kInstrumentA),
                                   .throw_in_bar_open = {},
                                   .throw_in_update = {},
                                   .throw_in_bar_close = {},
                                   .throw_in_destroy = {}});
    start_strategy();
    ASSERT_TRUE(wait_for_events(2));  // A 的 init（抛异常）+ B 的 init

    ASSERT_TRUE(write_tick(make_tick(kInstrumentA, 64830, 100.0, 10, 1000.0)));
    ASSERT_TRUE(write_tick(make_tick(kInstrumentB, 64831, 200.0, 5, 1000.0)));
    ASSERT_TRUE(wait_for_count(kInstrumentB, EventKind::BarUpdate, 1));
    ASSERT_TRUE(write_tick(make_tick(kInstrumentB, 64892, 200.5, 8, 1600.0)));
    ASSERT_TRUE(wait_for_count(kInstrumentB, EventKind::BarUpdate, 2));
    stop_strategy();

    EXPECT_EQ(exit_code_, 0);

    const auto events_a = recorder().for_instrument(kInstrumentA);
    ASSERT_EQ(events_a.size(), 1U)
        << "instrument whose on_init threw must not be added: " << describe_events(events_a);
    EXPECT_EQ(events_a[0].kind, EventKind::Init);

    const auto events_b = recorder().for_instrument(kInstrumentB);
    // init → open(0) → update → close(0) → open(1) → update → destroy
    ASSERT_EQ(events_b.size(), 7U)
        << "other instruments must keep working: " << describe_events(events_b);
    EXPECT_EQ(events_b[0].kind, EventKind::Init);
    EXPECT_EQ(events_b[1].kind, EventKind::BarOpen);
    EXPECT_EQ(events_b[2].kind, EventKind::BarUpdate);
    EXPECT_EQ(events_b[3].kind, EventKind::BarClose);
    EXPECT_EQ(events_b[4].kind, EventKind::BarOpen);
    EXPECT_EQ(events_b[4].bar_index, 1);
    EXPECT_EQ(events_b[5].kind, EventKind::BarUpdate);
    EXPECT_EQ(events_b[6].kind, EventKind::Destroy);
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
