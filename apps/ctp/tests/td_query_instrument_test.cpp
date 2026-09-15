#include "td/td_ctp_mapping.h"
#include "td/td_instrument_query_pending.h"

#include <string>

#include <gtest/gtest.h>

using namespace dztrader::ctp;

// to_qry_instrument_field: 单合约查询字段构造

TEST(ToQryInstrumentFieldTest, SymbolFillsInstrumentIdOnly) {
    auto f = to_qry_instrument_field("MA601");

    EXPECT_STREQ(f.InstrumentID, "MA601");
    // 其余字段零初始化 (CTP 查询无需 ExchangeID/ProductID 等)
    EXPECT_EQ(f.ExchangeID[0], '\0');
    EXPECT_EQ(f.ExchangeInstID[0], '\0');
    EXPECT_EQ(f.ProductID[0], '\0');
}

// InstrumentQueryPending: CZCE 刷新"场所码 -> 平台 instrument_id"待回写映射

TEST(InstrumentQueryPendingTest, AddThenTakeErases) {
    InstrumentQueryPending pending;
    pending.add("MA601", "MA1601");
    EXPECT_EQ(pending.size(), 1u);

    EXPECT_EQ(pending.take("MA601"), "MA1601");
    EXPECT_EQ(pending.size(), 0u);
    EXPECT_EQ(pending.take("MA601"), "");  // 命中即擦除, 二次未命中
}

TEST(InstrumentQueryPendingTest, TakeMissingReturnsEmpty) {
    InstrumentQueryPending pending;
    pending.add("MA601", "MA1601");
    EXPECT_EQ(pending.take("rb2601"), "");
    EXPECT_EQ(pending.size(), 1u);  // 未命中不影响既有登记
}

TEST(InstrumentQueryPendingTest, AddOverwritesSameSymbol) {
    InstrumentQueryPending pending;
    pending.add("MA601", "MA1601");
    pending.add("MA601", "MA1602");
    EXPECT_EQ(pending.size(), 1u);
    EXPECT_EQ(pending.take("MA601"), "MA1602");
}

TEST(InstrumentQueryPendingTest, OverflowClearsAndKeepsLatest) {
    InstrumentQueryPending pending(2);
    pending.add("A", "a");
    pending.add("B", "b");
    pending.add("C", "c");  // 超限: 清空旧登记 + 保留本条 (在途请求仍可回写)
    EXPECT_EQ(pending.size(), 1u);
    EXPECT_EQ(pending.take("A"), "");
    EXPECT_EQ(pending.take("C"), "c");
}

TEST(InstrumentQueryPendingTest, OverwriteAtCapacityDoesNotClear) {
    InstrumentQueryPending pending(2);
    pending.add("A", "a");
    pending.add("B", "b");
    pending.add("A", "a2");  // 覆盖写不占新槽, 不得触发清空
    EXPECT_EQ(pending.size(), 2u);
    EXPECT_EQ(pending.take("A"), "a2");
    EXPECT_EQ(pending.take("B"), "b");
}
