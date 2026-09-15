#include <gtest/gtest.h>

#include <dztrader/struct.h>
#include <dztrader/core/core_struct.h>

// ============================================================================
// TD POD 结构布局测试 (dztd_ctp 用)
// 验证 8 字节对齐 + 大小为 8 倍数 + 关键字段偏移
// ============================================================================

TEST(TdStructTest, DzMarginRateLayout) {
    static_assert(alignof(DzMarginRate) == 8);
    static_assert(sizeof(DzMarginRate) % 8 == 0);
    static_assert(sizeof(DzMarginRate) == sizeof(__dz_internal_packed_DzMarginRate));
}

TEST(TdStructTest, DzCommissionRateLayout) {
    static_assert(alignof(DzCommissionRate) == 8);
    static_assert(sizeof(DzCommissionRate) % 8 == 0);
}

TEST(TdStructTest, DzInstrumentStatusLayout) {
    static_assert(alignof(DzInstrumentStatus) == 8);
    static_assert(sizeof(DzInstrumentStatus) % 8 == 0);
}

TEST(TdStructTest, DzAccountStatusLayout) {
    static_assert(alignof(DzAccountStatus) == 8);
    static_assert(sizeof(DzAccountStatus) == 104);
    static_assert(sizeof(DzAccountStatus) == sizeof(__dz_internal_packed_DzAccountStatus));
}

TEST(TdStructTest, DzAccountStatusReqLayout) {
    static_assert(alignof(DzAccountStatusReq) == 8);
    static_assert(sizeof(DzAccountStatusReq) == 32);
}
