/**
 * @file td_stats_test.cpp
 * @brief dzweb 统计计数器纯单元（契约 rest §2.9.5）。
 *
 * 纯逻辑测试：无 DB、无线程。断言**外部可观察行为**——4 个计数器公开值、
 * `td_stats_body` 的 JSON 形状、水位查询；不断言私有容器。
 *
 * 期望值一律按契约口径手算（total = 全部委托含撤单/废单；unfilled = Submitting /
 * NotTraded / PartTraded 的笔数与 `volume - volume_traded`），不用被测实现自我推导。
 */

#include "td_stats.h"

#include <dztrader/data_type.h>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace dztrader::webui {
namespace {

/// 计数器 4 值的完整断言（逐字段，避免「只断言部分值」掩盖偏差）。
void expect_values(const TdStatsCounter& counter,
                   int64_t total_orders,
                   int64_t total_volume,
                   int64_t unfilled_orders,
                   int64_t unfilled_volume) {
    EXPECT_EQ(counter.values().total_orders, total_orders);
    EXPECT_EQ(counter.values().total_volume, total_volume);
    EXPECT_EQ(counter.values().unfilled_orders, unfilled_orders);
    EXPECT_EQ(counter.values().unfilled_volume, unfilled_volume);
}

// ===== 未成交状态判定（契约 rest §2.9.5）=====

TEST(TdStatusUnfilledTest, OnlySubmittingNotTradedPartTradedCountAsUnfilled) {
    EXPECT_TRUE(td_status_unfilled(DZ_ORDER_SUBMITTING));
    EXPECT_TRUE(td_status_unfilled(DZ_ORDER_NOT_TRADED));
    EXPECT_TRUE(td_status_unfilled(DZ_ORDER_PART_TRADED));
    EXPECT_FALSE(td_status_unfilled(DZ_ORDER_ALL_TRADED));
    EXPECT_FALSE(td_status_unfilled(DZ_ORDER_CANCELLED));
    EXPECT_FALSE(td_status_unfilled(DZ_ORDER_REJECTED));
}

/// 判定（`td_status_unfilled`）与枚举（`td_unfilled_statuses`）必须同源：
/// SQL 侧只消费枚举集合绑定 `IN (?,?,?)`，若两者漂移，重算口径与增量口径就会分家。
/// 穷举 DzOrderStatus 全部取值断言「在集合里 ⟺ 判定为真」，并锁定集合内容。
TEST(TdStatusUnfilledTest, EnumeratedSetAndPredicateAgreeOnEveryStatus) {
    constexpr int kAllStatuses[] = {DZ_ORDER_SUBMITTING, DZ_ORDER_NOT_TRADED, DZ_ORDER_PART_TRADED,
                                    DZ_ORDER_ALL_TRADED, DZ_ORDER_CANCELLED,  DZ_ORDER_REJECTED};
    const std::span<const int> set = td_unfilled_statuses();

    for (const int status : kAllStatuses) {
        const bool in_set = std::find(set.begin(), set.end(), status) != set.end();
        EXPECT_EQ(in_set, td_status_unfilled(status))
            << "status=" << status << " in_set=" << in_set;
    }
    // 集合内容明确锁定（三个未成交状态，且不多不少）。
    ASSERT_EQ(set.size(), 3U);
    EXPECT_NE(std::find(set.begin(), set.end(), DZ_ORDER_SUBMITTING), set.end());
    EXPECT_NE(std::find(set.begin(), set.end(), DZ_ORDER_NOT_TRADED), set.end());
    EXPECT_NE(std::find(set.begin(), set.end(), DZ_ORDER_PART_TRADED), set.end());
}

// ===== 4 值口径（契约 rest §2.9.5）=====

TEST(TdStatsCounterTest, FreshCounterIsAllZero) {
    TdStatsCounter counter;
    expect_values(counter, 0, 0, 0, 0);
}

TEST(TdStatsCounterTest, NewUnfilledOrderAddsBothTotalAndUnfilled) {
    TdStatsCounter counter;
    // 一笔 5 手未成交委托：total = 1 笔 / 5 手；unfilled 同（已成交 0）。
    counter.apply("ACC1", DZ_ORDER_NOT_TRADED, /*order_id=*/1, /*volume=*/5, /*volume_traded=*/0);
    expect_values(counter, 1, 5, 1, 5);
}

TEST(TdStatsCounterTest, NewTerminalOrdersCountTowardTotalButNotUnfilled) {
    TdStatsCounter counter;
    // 契约 rest §2.9.5：total 含撤单、废单。全成 / 撤单 / 废单都是终态，不进未成交口径。
    counter.apply("ACC1", DZ_ORDER_ALL_TRADED, 1, 3, 3);
    counter.apply("ACC1", DZ_ORDER_CANCELLED, 2, 4, 0);
    counter.apply("ACC1", DZ_ORDER_REJECTED, 3, 2, 0);
    expect_values(counter, 3, 9, 0, 0);
}

TEST(TdStatsCounterTest, TotalAggregatesAcrossOnlineAccounts) {
    TdStatsCounter counter;
    counter.apply("ACC1", DZ_ORDER_NOT_TRADED, 1, 2, 0);
    counter.apply("ACC2", DZ_ORDER_NOT_TRADED, 2, 3, 0);
    expect_values(counter, 2, 5, 2, 5);
}

// ===== 活动表增量（状态迁移只动未成交口径）=====

TEST(TdStatsCounterTest, PartialFillReducesUnfilledVolumeOnly) {
    TdStatsCounter counter;
    counter.apply("ACC1", DZ_ORDER_NOT_TRADED, 1, 5, 0);
    // 成交 2 手：total 不变，未成交剩余量 5 → 3，笔数仍算一笔未成交。
    counter.apply("ACC1", DZ_ORDER_PART_TRADED, 1, 5, 2);
    expect_values(counter, 1, 5, 1, 3);
}

TEST(TdStatsCounterTest, CancelAfterPartialFillLeavesUnfilledSet) {
    TdStatsCounter counter;
    counter.apply("ACC1", DZ_ORDER_NOT_TRADED, 1, 5, 0);
    counter.apply("ACC1", DZ_ORDER_PART_TRADED, 1, 5, 2);
    // 撤单：未成交这笔退出未成交集合（-1 笔、剩余 3 手清零）；total 不变（含撤单）。
    counter.apply("ACC1", DZ_ORDER_CANCELLED, 1, 5, 2);
    expect_values(counter, 1, 5, 0, 0);
}

TEST(TdStatsCounterTest, AllTradedLeavesUnfilledSet) {
    TdStatsCounter counter;
    counter.apply("ACC1", DZ_ORDER_NOT_TRADED, 1, 2, 0);
    counter.apply("ACC1", DZ_ORDER_PART_TRADED, 1, 2, 1);
    counter.apply("ACC1", DZ_ORDER_ALL_TRADED, 1, 2, 2);
    expect_values(counter, 1, 2, 0, 0);
}

TEST(TdStatsCounterTest, RepeatedSameStateIsIdempotent) {
    TdStatsCounter counter;
    counter.apply("ACC1", DZ_ORDER_NOT_TRADED, 1, 5, 0);
    // 同一状态重复应用（幂等，契约 rest §2.9.5 的设计依据）：delta = 0。
    counter.apply("ACC1", DZ_ORDER_NOT_TRADED, 1, 5, 0);
    counter.apply("ACC1", DZ_ORDER_NOT_TRADED, 1, 5, 0);
    expect_values(counter, 1, 5, 1, 5);
}

TEST(TdStatsCounterTest, RepeatedPartialFillStateIsIdempotent) {
    TdStatsCounter counter;
    counter.apply("ACC1", DZ_ORDER_NOT_TRADED, 1, 5, 0);
    counter.apply("ACC1", DZ_ORDER_PART_TRADED, 1, 5, 3);
    counter.apply("ACC1", DZ_ORDER_PART_TRADED, 1, 5, 3);
    counter.apply("ACC1", DZ_ORDER_PART_TRADED, 1, 5, 3);
    expect_values(counter, 1, 5, 1, 2);
}

TEST(TdStatsCounterTest, TerminalOrderLeavingActiveSetIsNoLongerCountedAsUnfilled) {
    TdStatsCounter counter;
    counter.apply("ACC1", DZ_ORDER_NOT_TRADED, 1, 4, 0);
    counter.apply("ACC1", DZ_ORDER_CANCELLED, 1, 4, 0);
    expect_values(counter, 1, 4, 0, 0);
    // 终态之后同一 order_id 再到一帧：已不在活动表 ⇒ 按契约口径计为新委托。这是设计
    // §5.5 记录的已知精度边界（下一次对账 ≤30s 自动纠正），此处锁定「不会静默回退」。
    counter.apply("ACC1", DZ_ORDER_NOT_TRADED, 1, 4, 0);
    expect_values(counter, 2, 8, 1, 4);
}

TEST(TdStatsCounterTest, ActiveSetIsPerAccountSoSameOrderIdIsNotMerged) {
    TdStatsCounter counter;
    // order_id 只在账户内唯一：两个账户的同号委托是两笔。
    counter.apply("ACC1", DZ_ORDER_NOT_TRADED, 7, 1, 0);
    counter.apply("ACC2", DZ_ORDER_NOT_TRADED, 7, 2, 0);
    expect_values(counter, 2, 3, 2, 3);
    // ACC2 的迁移只影响 ACC2 的那一笔。
    counter.apply("ACC2", DZ_ORDER_PART_TRADED, 7, 2, 1);
    expect_values(counter, 2, 3, 2, 2);
}

// ===== reset / clear / watermark =====

TEST(TdStatsCounterTest, ResetOverwritesValuesAndRebuildsActiveSetFromSnapshot) {
    TdStatsCounter counter;
    counter.apply("ACC1", DZ_ORDER_NOT_TRADED, 1, 2, 0);
    expect_values(counter, 1, 2, 1, 2);

    TdStatsSnapshot snapshot;
    snapshot.values = TdStatsValues{
        .total_orders = 10, .total_volume = 20, .unfilled_orders = 3, .unfilled_volume = 4};
    snapshot.active.push_back(TdStatsActiveOrder{.account_id = "ACC2",
                                                 .order_id = 7,
                                                 .status = DZ_ORDER_NOT_TRADED,
                                                 .volume = 5,
                                                 .volume_traded = 0});
    snapshot.watermarks = {{"ACC2", 99}};
    counter.reset(snapshot);
    expect_values(counter, 10, 20, 3, 4);

    // 快照里的活动委托按「已在活动表」处理：只做剩余量差量，不重复计入 total。
    counter.apply("ACC2", DZ_ORDER_PART_TRADED, 7, 5, 2);
    expect_values(counter, 10, 20, 3, 2);

    // 旧活动表已被覆盖：ACC1/1 不再在活动表 → 视为新委托。
    counter.apply("ACC1", DZ_ORDER_NOT_TRADED, 1, 2, 0);
    expect_values(counter, 11, 22, 4, 4);
}

TEST(TdStatsCounterTest, ResetDropsNonUnfilledSeedsFromActiveSet) {
    TdStatsSnapshot snapshot;
    snapshot.values = TdStatsValues{.total_orders = 1, .total_volume = 5};
    // 查询侧只应产出未成交行；即便喂入终态行也不进活动表（唯一判据 td_status_unfilled）。
    snapshot.active.push_back(TdStatsActiveOrder{.account_id = "ACC1",
                                                 .order_id = 1,
                                                 .status = DZ_ORDER_CANCELLED,
                                                 .volume = 5,
                                                 .volume_traded = 5});
    TdStatsCounter counter;
    counter.reset(snapshot);
    expect_values(counter, 1, 5, 0, 0);
    // 该 order_id 不在活动表 → 视为新委托。
    counter.apply("ACC1", DZ_ORDER_NOT_TRADED, 1, 5, 0);
    expect_values(counter, 2, 10, 1, 5);
}

TEST(TdStatsCounterTest, WatermarkIsZeroWithoutRecordAndOnlyResetWritesIt) {
    TdStatsCounter counter;
    EXPECT_EQ(counter.watermark("ACC1"), 0U);

    TdStatsSnapshot snapshot;
    snapshot.watermarks = {{"ACC1", 7}, {"ACC2", 11}};
    counter.reset(snapshot);
    EXPECT_EQ(counter.watermark("ACC1"), 7U);
    EXPECT_EQ(counter.watermark("ACC2"), 11U);
    EXPECT_EQ(counter.watermark("ACC_UNKNOWN"), 0U);

    // 增量不推进水位（契约 td-data-sync §2 第 4 条：只准用 DB 查得的 W）。
    counter.apply("ACC1", DZ_ORDER_NOT_TRADED, 1, 1, 0);
    EXPECT_EQ(counter.watermark("ACC1"), 7U);
}

// ===== 响应体（契约 rest §2.9.5）=====

TEST(TdStatsBodyTest, ReadyEnvelopeCarriesFourCountersAndScope) {
    const TdStatsValues values{
        .total_orders = 1240, .total_volume = 2480, .unfilled_orders = 12, .unfilled_volume = 21};
    const nlohmann::json body =
        td_stats_body(true, std::string("20260808"),
                      std::vector<std::string>{"account_001", "account_002"}, values);

    EXPECT_TRUE(body["available"].get<bool>());
    EXPECT_EQ(body["trading_day"].get<std::string>(), "20260808");
    ASSERT_EQ(body["online_accounts"].size(), 2U);
    EXPECT_EQ(body["online_accounts"][0].get<std::string>(), "account_001");
    ASSERT_TRUE(body.contains("stats"));
    const auto& stats = body["stats"];
    EXPECT_EQ(stats["total_orders"].get<int64_t>(), 1240);
    EXPECT_EQ(stats["total_volume"].get<int64_t>(), 2480);
    EXPECT_EQ(stats["unfilled_orders"].get<int64_t>(), 12);
    EXPECT_EQ(stats["unfilled_volume"].get<int64_t>(), 21);
    // stats 不是列表（契约 rest §2.9.5：无 items / next_cursor）。
    EXPECT_FALSE(body.contains("items"));
    EXPECT_FALSE(body.contains("next_cursor"));
    EXPECT_FALSE(body.contains("reason"));
}

TEST(TdStatsBodyTest, ReadyEnvelopeWithNoOnlineAccountHasNullTradingDayAndZeroCounters) {
    const nlohmann::json body =
        td_stats_body(true, std::nullopt, std::vector<std::string>{}, TdStatsValues{});
    EXPECT_TRUE(body["available"].get<bool>());
    EXPECT_TRUE(body["trading_day"].is_null());
    EXPECT_TRUE(body["online_accounts"].empty());
    EXPECT_EQ(body["stats"]["total_orders"].get<int64_t>(), 0);
    EXPECT_EQ(body["stats"]["total_volume"].get<int64_t>(), 0);
    EXPECT_EQ(body["stats"]["unfilled_orders"].get<int64_t>(), 0);
    EXPECT_EQ(body["stats"]["unfilled_volume"].get<int64_t>(), 0);
}

TEST(TdStatsBodyTest, NotReadyEnvelopeHasOnlyAvailableAndReason) {
    const nlohmann::json body =
        td_stats_body(false, std::string("20260808"), std::vector<std::string>{"account_001"},
                      TdStatsValues{.total_orders = 5});
    // 契约 rest §2.9.5 + §2.9.8：降级体只有 available 与 reason 两个键（不冒充 0 数据）。
    ASSERT_EQ(body.size(), 2U);
    EXPECT_FALSE(body["available"].get<bool>());
    EXPECT_EQ(body["reason"].get<std::string>(), "not_ready");
    EXPECT_FALSE(body.contains("stats"));
    EXPECT_FALSE(body.contains("trading_day"));
    EXPECT_FALSE(body.contains("online_accounts"));
    EXPECT_FALSE(body.contains("items"));
}

}  // namespace
}  // namespace dztrader::webui
