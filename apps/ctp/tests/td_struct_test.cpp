#include <gtest/gtest.h>

#include <dztrader/struct.h>
#include <dztrader/core/core_struct.h>

// ============================================================================
// TD POD 结构布局测试 (dztd_ctp 用)
// 验证 8 字节对齐 + 大小为 8 倍数 + 关键字段偏移
// ============================================================================

TEST(TdStructTest, DzAccountStatusLayout) {
    static_assert(alignof(DzAccountStatus) == 8);
    static_assert(sizeof(DzAccountStatus) == 104);
    static_assert(sizeof(DzAccountStatus) == sizeof(__dz_internal_packed_DzAccountStatus));
}

TEST(TdStructTest, DzAccountStatusReqLayout) {
    static_assert(alignof(DzAccountStatusReq) == 8);
    static_assert(sizeof(DzAccountStatusReq) == 32);
}
