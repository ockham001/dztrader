#ifndef DZTRADER_STRATEGY_API_TESTS_BAR_TEST_DATA_H_
#define DZTRADER_STRATEGY_API_TESTS_BAR_TEST_DATA_H_

#include <dztrader/bar/types.h>
#include <dztrader/struct.h>

#include <algorithm>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

namespace dztrader::strategy_api::test {

/// @brief 09:00 的交易日内秒
inline constexpr int32_t TSEC_09_00 = 54000;
/// @brief 15:00 的交易日内秒
inline constexpr int32_t TSEC_15_00 = 75600;
/// @brief 夜盘小节（21:00）的交易日内秒
inline constexpr int32_t TSEC_21_00 = 10800;
/// @brief 夜盘小节结束（次日 02:30）的交易日内秒
inline constexpr int32_t TSEC_02_30 = 30600;
/// @brief 早盘小节开始（09:30）的交易日内秒
inline constexpr int32_t TSEC_09_30 = 55800;
/// @brief 早盘小节结束（11:30）的交易日内秒
inline constexpr int32_t TSEC_11_30 = 63000;
/// @brief 午盘小节开始（13:00）的交易日内秒
inline constexpr int32_t TSEC_13_00 = 68400;
/// @brief 10:00 的交易日内秒
inline constexpr int32_t TSEC_10_00 = 57600;
/// @brief 10:15 的交易日内秒
inline constexpr int32_t TSEC_10_15 = 58500;
/// @brief 10:30 的交易日内秒
inline constexpr int32_t TSEC_10_30 = 59400;
/// @brief 11:00 的交易日内秒
inline constexpr int32_t TSEC_11_00 = 61200;
/// @brief 13:30 的交易日内秒
inline constexpr int32_t TSEC_13_30 = 70200;

/// @brief tick→bar 测试默认交易日
inline constexpr DzDate TRADING_DAY = 20000;

/// @brief 构造一笔 tick→bar 测试用行情快照
inline DzTick make_tick(DzTime clock,
                        double last_price,
                        DzVolume volume,
                        double turnover,
                        DzLargeVolume open_interest,
                        DzDate date = TRADING_DAY) {
    DzTick tick{};
    tick.date = date;
    tick.time = clock;
    tick.last_price = last_price;
    tick.volume = volume;
    tick.turnover = turnover;
    tick.open_interest = open_interest;
    return tick;
}

/// @brief 一次 bar 事件的通知类型
enum class EventKind { Open, Update, Close };

/// @brief sink 收到的一次 bar 事件（携带该时刻的 bar 快照）
struct BarEvent {
    EventKind kind;
    dztrader::bar::Bar bar;
};

/// @brief 记录完整事件序列的手写 fake sink（仓库零 GMock）
class RecordingSink {
public:
    void on_bar_open(const dztrader::bar::Bar& bar) { events.push_back({EventKind::Open, bar}); }
    void on_bar_update(const dztrader::bar::Bar& bar) {
        events.push_back({EventKind::Update, bar});
    }
    void on_bar_close(const dztrader::bar::Bar& bar) { events.push_back({EventKind::Close, bar}); }

    std::vector<BarEvent> events;
};

/// @brief 断言两根 bar 的全部公开字段一致
inline void expect_bar_equal(const dztrader::bar::Bar& actual, const dztrader::bar::Bar& expected) {
    EXPECT_EQ(actual.trading_day, expected.trading_day);
    EXPECT_EQ(actual.begin_sec, expected.begin_sec);
    EXPECT_EQ(actual.last_tick_sec, expected.last_tick_sec);
    EXPECT_EQ(actual.period_sec, expected.period_sec);
    EXPECT_DOUBLE_EQ(actual.open, expected.open);
    EXPECT_DOUBLE_EQ(actual.high, expected.high);
    EXPECT_DOUBLE_EQ(actual.low, expected.low);
    EXPECT_DOUBLE_EQ(actual.close, expected.close);
    EXPECT_EQ(actual.volume, expected.volume);
    EXPECT_DOUBLE_EQ(actual.turnover, expected.turnover);
    EXPECT_EQ(actual.open_interest, expected.open_interest);
    EXPECT_EQ(actual.cum_volume, expected.cum_volume);
    EXPECT_DOUBLE_EQ(actual.cum_turnover, expected.cum_turnover);
    EXPECT_EQ(actual.is_final, expected.is_final);
}

/// @brief 统计某一类 bar 事件的通知次数
inline int count_events(const RecordingSink& sink, EventKind kind) {
    return static_cast<int>(
        std::count_if(sink.events.begin(), sink.events.end(),
                      [kind](const BarEvent& event) { return event.kind == kind; }));
}

/// @brief 最后一类 bar 事件携带的 bar（不存在返回 nullptr）
inline const dztrader::bar::Bar* last_event_bar(const RecordingSink& sink, EventKind kind) {
    for (auto it = sink.events.rbegin(); it != sink.events.rend(); ++it) {
        if (it->kind == kind) {
            return &it->bar;
        }
    }
    return nullptr;
}

/// @brief 单小节当日盘表（09:00–15:00），tick→bar 测试的公共合法表
inline constexpr dztrader::bar::TradingSessionTable day_session_table() {
    dztrader::bar::TradingSessionTable table{};
    table.session_count = 1;
    table.sessions[0] = dztrader::bar::TradingSession{TSEC_09_00, TSEC_15_00};
    return table;
}

/// @brief 日盘三段小节表：09:00–10:15、10:30–11:30、13:30–15:00
///
/// 早盘休息 10:15–10:30 落在 1 小时桶 10:00–11:00 内部，用于验证「跨休息拼根」：
/// 休息前后的 tick 进入同一根 1 小时 bar，不按小节切根。
inline constexpr dztrader::bar::TradingSessionTable day_break_session_table() {
    dztrader::bar::TradingSessionTable table{};
    table.session_count = 3;
    table.sessions[0] = dztrader::bar::TradingSession{TSEC_09_00, TSEC_10_15};
    table.sessions[1] = dztrader::bar::TradingSession{TSEC_10_30, TSEC_11_30};
    table.sessions[2] = dztrader::bar::TradingSession{TSEC_13_30, TSEC_15_00};
    return table;
}

/// @brief 夜盘 + 日盘多小节表：21:00–次日 02:30、09:30–11:30、13:00–15:00
///
/// 夜盘跨午夜且早于日盘（18:00 锚点下夜盘 tsec 更小），两个日盘小节之间存在
/// 11:30–13:00 的休息时段。
inline constexpr dztrader::bar::TradingSessionTable commodity_session_table() {
    dztrader::bar::TradingSessionTable table{};
    table.session_count = 3;
    table.sessions[0] = dztrader::bar::TradingSession{TSEC_21_00, TSEC_02_30};
    table.sessions[1] = dztrader::bar::TradingSession{TSEC_09_30, TSEC_11_30};
    table.sessions[2] = dztrader::bar::TradingSession{TSEC_13_00, TSEC_15_00};
    return table;
}

/// @brief 单小节日盘表 + 开盘竞价窗口 08:59:00–08:59:59（目标小节 0 的首桶 09:00）
inline constexpr dztrader::bar::TradingSessionTable day_auction_session_table() {
    dztrader::bar::TradingSessionTable table = day_session_table();
    table.auction_count = 1;
    table.auctions[0] = dztrader::bar::AuctionWindow{dztrader::bar::AuctionKind::Open, 0,
                                                     TSEC_09_00 - 60, TSEC_09_00 - 1};
    return table;
}

/// @brief 商品多小节表 + 三个竞价窗口，长度不同：
///        夜盘开盘 20:59:30–20:59:59（30 秒，归 21:00 首桶）、
///        早盘开盘 09:28:30–09:29:59（90 秒，归 09:30 首桶）、
///        午盘收盘 14:58:00–15:00:00（120 秒，按普通 tick 处理、含小节末端）
inline constexpr dztrader::bar::TradingSessionTable commodity_auction_session_table() {
    dztrader::bar::TradingSessionTable table = commodity_session_table();
    table.auction_count = 3;
    table.auctions[0] = dztrader::bar::AuctionWindow{dztrader::bar::AuctionKind::Open, 0,
                                                     TSEC_21_00 - 30, TSEC_21_00 - 1};
    table.auctions[1] = dztrader::bar::AuctionWindow{dztrader::bar::AuctionKind::Open, 1,
                                                     TSEC_09_30 - 90, TSEC_09_30 - 1};
    table.auctions[2] = dztrader::bar::AuctionWindow{dztrader::bar::AuctionKind::Close, 2,
                                                     TSEC_15_00 - 120, TSEC_15_00};
    return table;
}

/// @brief 竞价窗口与前一小节重叠的表：11:00–11:29 是午盘（13:00）开盘竞价窗口，
///        同时落在早盘小节 09:00–11:30 内，用于验证「窗口命中优先于 session 判定」
inline constexpr dztrader::bar::TradingSessionTable overlapping_auction_session_table() {
    dztrader::bar::TradingSessionTable table{};
    table.session_count = 2;
    table.sessions[0] = dztrader::bar::TradingSession{TSEC_09_00, TSEC_11_30};
    table.sessions[1] = dztrader::bar::TradingSession{TSEC_13_00, TSEC_15_00};
    table.auction_count = 1;
    table.auctions[0] = dztrader::bar::AuctionWindow{dztrader::bar::AuctionKind::Open, 1,
                                                     TSEC_11_00, TSEC_11_30 - 60};
    return table;
}

}  // namespace dztrader::strategy_api::test

#endif  // DZTRADER_STRATEGY_API_TESTS_BAR_TEST_DATA_H_
