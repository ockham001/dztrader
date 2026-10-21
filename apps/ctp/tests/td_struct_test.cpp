#include <gtest/gtest.h>

#include <cstddef>

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
    // 关键字段偏移 (跨进程 SHM payload 布局): account_id[32] + gateway_name[64] + day + state
    static_assert(offsetof(DzAccountStatus, account_id) == 0);
    static_assert(offsetof(DzAccountStatus, gateway_name) == 32);
    static_assert(offsetof(DzAccountStatus, trading_day) == 96);
    static_assert(offsetof(DzAccountStatus, state) == 100);
}

TEST(TdStructTest, DzAccountStatusReqLayout) {
    static_assert(alignof(DzAccountStatusReq) == 8);
    static_assert(sizeof(DzAccountStatusReq) == 32);
}
