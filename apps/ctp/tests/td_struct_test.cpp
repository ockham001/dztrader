#include <gtest/gtest.h>

#include <dztrader/struct.h>
#include <dztrader/core/core_struct.h>

// ============================================================================
// TD POD 结构布局测试 (dztd_ctp 用)
// 验证 8 字节对齐 + 大小为 8 倍数 + 关键字段偏移
// ============================================================================

TEST(TdStructTest, DzPositionDetailLayout) {
    static_assert(alignof(DzPositionDetail) == 8);
    static_assert(sizeof(DzPositionDetail) % 8 == 0);
    static_assert(sizeof(DzPositionDetail) == sizeof(__dz_internal_packed_DzPositionDetail));
}

TEST(TdStructTest, DzMarginRateLayout) {
    static_assert(alignof(DzMarginRate) == 8);
    static_assert(sizeof(DzMarginRate) % 8 == 0);
    static_assert(sizeof(DzMarginRate) == sizeof(__dz_internal_packed_DzMarginRate));
}

TEST(TdStructTest, DzCommissionRateLayout) {
    static_assert(alignof(DzCommissionRate) == 8);
    static_assert(sizeof(DzCommissionRate) % 8 == 0);
}

TEST(TdStructTest, DzContractLayout) {
    static_assert(alignof(DzContract) == 8);
    static_assert(sizeof(DzContract) % 8 == 0);
}

TEST(TdStructTest, DzInstrumentStatusLayout) {
    static_assert(alignof(DzInstrumentStatus) == 8);
    static_assert(sizeof(DzInstrumentStatus) % 8 == 0);
}

TEST(TdStructTest, DzTransferReqLayout) {
    static_assert(alignof(DzTransferReq) == 8);
    static_assert(sizeof(DzTransferReq) % 8 == 0);
}

TEST(TdStructTest, DzTransferRspLayout) {
    static_assert(alignof(DzTransferRsp) == 8);
    static_assert(sizeof(DzTransferRsp) % 8 == 0);
}

TEST(TdStructTest, DzPasswordUpdateReqLayout) {
    static_assert(alignof(DzPasswordUpdateReq) == 8);
    static_assert(sizeof(DzPasswordUpdateReq) % 8 == 0);
}

TEST(TdStructTest, DzPasswordUpdateRspLayout) {
    static_assert(alignof(DzPasswordUpdateRsp) == 8);
    static_assert(sizeof(DzPasswordUpdateRsp) % 8 == 0);
}

TEST(TdStructTest, DzRiskRejectLayout) {
    static_assert(alignof(DzRiskReject) == 8);
    static_assert(sizeof(DzRiskReject) % 8 == 0);
}

TEST(TdStructTest, DzContractFields) {
    DzContract c{};
    c.volume_multiple = 10;
    c.price_tick = 0.5;
    c.option_type = 1;  // CALL
    EXPECT_EQ(c.volume_multiple, 10);
    EXPECT_DOUBLE_EQ(c.price_tick, 0.5);
    EXPECT_EQ(c.option_type, 1);
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
