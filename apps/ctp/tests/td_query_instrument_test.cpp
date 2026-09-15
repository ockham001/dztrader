#include "td/td_ctp_mapping.h"

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
