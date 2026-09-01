#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "td/td_report_filter.h"
#include "td/td_schema.h"

using namespace dztrader::ctp;

using Verdict = ReportFilter::Verdict;

namespace {

/// 辅助: 构造一个基准 OrderRecord (status=已撤销'5', vol_traded=10, upd=1000).
OrderRecord make_order(int64_t order_id, int8_t status, int32_t vol_traded,
                       int32_t vol_canceled, int64_t update_time) {
    OrderRecord r{};
    r.base.order_id = order_id;
    r.base.status = status;
    r.base.volume_traded = vol_traded;
    r.volume_canceled = vol_canceled;
    r.update_time = update_time;
    std::strcpy(r.trading_day, "20260901");
    std::strcpy(r.base.account_id, "acc1");
    return r;
}

/// 辅助: 构造一个基准 TradeRecord (trading_day/trade_id).
TradeRecord make_trade(const char* trading_day, const char* trade_id) {
    TradeRecord r{};
    std::strcpy(r.trading_day, trading_day);
    std::strcpy(r.base.trade_id, trade_id);
    std::strcpy(r.base.account_id, "acc1");
    r.base.seq = 1;
    return r;
}

/// 基准含 1 条 status='5'(已撤销), vol_traded=10, upd=1000 的记录.
ReportFilter make_filter_with_order() {
    return ReportFilter::load({make_order(1, DZ_ORDER_CANCELLED, 10, 0, 1000)}, {});
}

// ============================================================================
// check_order: 委托去重对比集 {update_time, volume_traded, volume_canceled, status}
// ============================================================================

TEST(ReportFilterOrder, IdenticalRecordSkipped) {
    auto f = make_filter_with_order();
    auto rec = make_order(1, DZ_ORDER_CANCELLED, 10, 0, 1000);  // 完全相等
    bool warn = false;
    EXPECT_EQ(Verdict::kSkip, f.check_order(rec, &warn));
    EXPECT_FALSE(warn);
}

TEST(ReportFilterOrder, StatusFlipSameSecondForwards) {
    // spec §4.1 修正后的关键用例: 同秒 status 翻转 (volume/update_time 均不变) 必须转发
    auto f = make_filter_with_order();                       // status='3'(部分成交), vol=5, upd=1000
    auto rec = make_order(1, DZ_ORDER_PART_TRADED, 5, 0, 1000);
    f.accept_order(rec);                                     // 更新基准为 part-traded
    auto flipped = make_order(1, DZ_ORDER_CANCELLED, 5, 0, 1000);  // 已撤销, vol=5, upd=1000
    EXPECT_EQ(Verdict::kForward, f.check_order(flipped));
}

TEST(ReportFilterOrder, OutdatedUpdateTimeDefensivelySkipped) {
    auto f = make_filter_with_order();                       // upd=1000
    auto rec = make_order(1, DZ_ORDER_ALL_TRADED, 10, 0, 500);  // 其余字段不等, 但时间回退
    bool warn = false;
    EXPECT_EQ(Verdict::kSkip, f.check_order(rec, &warn));
    EXPECT_TRUE(warn);
}

TEST(ReportFilterOrder, OutdatedSameTimeNotSkipped) {
    // update_time 相等时不判回退 (同秒不等即转发)
    auto f = make_filter_with_order();                       // upd=1000
    auto rec = make_order(1, DZ_ORDER_ALL_TRADED, 10, 0, 1000);  // 字段不等但 upd 相同
    bool warn = false;
    EXPECT_EQ(Verdict::kForward, f.check_order(rec, &warn));
    EXPECT_FALSE(warn);
}

TEST(ReportFilterOrder, NewOrderForwards) {
    auto f = make_filter_with_order();  // 基准含 order_id=1
    auto rec = make_order(2, DZ_ORDER_NOT_TRADED, 0, 0, 2000);  // order_id 不在基准
    EXPECT_EQ(Verdict::kForward, f.check_order(rec));
}

TEST(ReportFilterOrder, CheckDoesNotUpdateBaseline) {
    auto f = make_filter_with_order();
    auto rec = make_order(1, DZ_ORDER_ALL_TRADED, 10, 0, 2000);
    EXPECT_EQ(Verdict::kForward, f.check_order(rec));
    // check 不更新基准: 再次 check 同一 rec 仍是 kForward (基准未变)
    EXPECT_EQ(Verdict::kForward, f.check_order(rec));
}

TEST(ReportFilterOrder, AcceptUpdatesBaseline) {
    auto f = make_filter_with_order();
    auto rec = make_order(1, DZ_ORDER_ALL_TRADED, 10, 0, 2000);
    EXPECT_EQ(Verdict::kForward, f.check_order(rec));
    f.accept_order(rec);
    // accept 后基准已更新: 再次 check 同一 rec 应为 kSkip
    EXPECT_EQ(Verdict::kSkip, f.check_order(rec));
}

// ============================================================================
// check_trade: 成交键 (trading_day, trade_id) 存在性
// ============================================================================

TEST(ReportFilterTrade, SameDaySameTradeIdSkipped) {
    auto f = ReportFilter::load({}, {make_trade("20260901", "T001")});
    auto rec = make_trade("20260901", "T001");  // 键命中
    EXPECT_EQ(Verdict::kSkip, f.check_trade(rec));
}

TEST(ReportFilterTrade, CrossDaySameTradeIdForwards) {
    auto f = ReportFilter::load({}, {make_trade("20260901", "T001")});
    auto rec = make_trade("20260902", "T001");  // 同 trade_id 不同 day
    EXPECT_EQ(Verdict::kForward, f.check_trade(rec));
}

TEST(ReportFilterTrade, SameDayDifferentTradeIdForwards) {
    auto f = ReportFilter::load({}, {make_trade("20260901", "T001")});
    auto rec = make_trade("20260901", "T002");  // 同 day 不同 trade_id
    EXPECT_EQ(Verdict::kForward, f.check_trade(rec));
}

TEST(ReportFilterTrade, AcceptUpdatesTradeKeys) {
    auto f = ReportFilter::load({}, {});
    auto rec = make_trade("20260901", "T001");
    EXPECT_EQ(Verdict::kForward, f.check_trade(rec));
    f.accept_trade(rec);
    EXPECT_EQ(Verdict::kSkip, f.check_trade(rec));  // accept 后键命中
}

// ============================================================================
// load: 从 DB 全量装载构造基准, 与逐条 accept 后行为一致
// ============================================================================

TEST(ReportFilterLoad, BuildsBaselineFromDbRows) {
    auto f = ReportFilter::load({make_order(1, DZ_ORDER_ALL_TRADED, 5, 0, 2000)},
                                {make_trade("20260901", "T001")});
    // 与逐条 accept 后行为一致: 重放同一批全 kSkip
    EXPECT_EQ(Verdict::kSkip, f.check_order(make_order(1, DZ_ORDER_ALL_TRADED, 5, 0, 2000)));
    EXPECT_EQ(Verdict::kSkip, f.check_trade(make_trade("20260901", "T001")));
}

// ============================================================================
// find_latest_order / find_latest_trade: 最近 accept 的那条
// ============================================================================

TEST(ReportFilterLatest, EmptyBaselineReturnsNull) {
    auto f = ReportFilter::load({}, {});
    EXPECT_EQ(nullptr, f.find_latest_order());
    EXPECT_EQ(nullptr, f.find_latest_trade());
}

TEST(ReportFilterLatest, FindLatestReturnsLastAccepted) {
    auto f = ReportFilter::load({}, {});
    EXPECT_EQ(nullptr, f.find_latest_order());
    EXPECT_EQ(nullptr, f.find_latest_trade());

    auto o1 = make_order(1, DZ_ORDER_NOT_TRADED, 0, 0, 1000);
    auto o2 = make_order(2, DZ_ORDER_ALL_TRADED, 5, 0, 2000);
    f.accept_order(o1);
    f.accept_order(o2);
    ASSERT_NE(nullptr, f.find_latest_order());
    EXPECT_EQ(2, f.find_latest_order()->order_id);

    auto t1 = make_trade("20260901", "T001");
    auto t2 = make_trade("20260901", "T002");
    f.accept_trade(t1);
    f.accept_trade(t2);
    ASSERT_NE(nullptr, f.find_latest_trade());
    EXPECT_STREQ("T002", f.find_latest_trade()->trade_id);
}

TEST(ReportFilterLatest, LoadBaselineSetsLatest) {
    auto f = ReportFilter::load({make_order(1, DZ_ORDER_ALL_TRADED, 5, 0, 2000)},
                                {make_trade("20260901", "T001")});
    ASSERT_NE(nullptr, f.find_latest_order());
    EXPECT_EQ(1, f.find_latest_order()->order_id);
    ASSERT_NE(nullptr, f.find_latest_trade());
    EXPECT_STREQ("T001", f.find_latest_trade()->trade_id);
}

}  // namespace
