#include "td/td_account_session_pure.h"

#include <gtest/gtest.h>

using namespace dztrader::ctp;

// ============================================================================
// OrderRefMap
// ============================================================================

TEST(OrderRefMapTest, EmptyReturnsNullptr) {
    OrderRefMap m;
    EXPECT_EQ(m.find_by_order_ref("1"), nullptr);
    EXPECT_EQ(m.find_by_sys_id("12345"), nullptr);
    EXPECT_EQ(m.size(), 0u);
}

TEST(OrderRefMapTest, InsertAndFindByOrderRef) {
    OrderRefMap m;
    m.insert_by_order_ref("1", 100);
    EXPECT_EQ(m.size(), 1u);
    const DzOrderId* p = m.find_by_order_ref("1");
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(*p, 100);
}

TEST(OrderRefMapTest, InsertAndFindBySysId) {
    OrderRefMap m;
    m.insert_by_sys_id("12345", 200);
    const DzOrderId* p = m.find_by_sys_id("12345");
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(*p, 200);
}

TEST(OrderRefMapTest, MultipleEntries) {
    OrderRefMap m;
    m.insert_by_order_ref("1", 100);
    m.insert_by_order_ref("2", 200);
    m.insert_by_order_ref("3", 300);
    EXPECT_EQ(m.size(), 3u);
}

TEST(OrderRefMapTest, RefAndSysIdIndependent) {
    OrderRefMap m;
    m.insert_by_order_ref("1", 100);
    m.insert_by_sys_id("12345", 100);
    EXPECT_EQ(*m.find_by_order_ref("1"), 100);
    EXPECT_EQ(*m.find_by_sys_id("12345"), 100);
}

TEST(OrderRefMapTest, EraseByOrderRef) {
    OrderRefMap m;
    m.insert_by_order_ref("1", 100);
    m.erase_by_order_ref("1");
    EXPECT_EQ(m.find_by_order_ref("1"), nullptr);
    EXPECT_EQ(m.size(), 0u);
}

TEST(OrderRefMapTest, OverwriteByOrderRef) {
    OrderRefMap m;
    m.insert_by_order_ref("1", 100);
    m.insert_by_order_ref("1", 200);  // 覆盖
    EXPECT_EQ(*m.find_by_order_ref("1"), 200);
}

// C1: OrderRefMap 内部归一化到 12 位补零格式 (与 CTP 回传格式一致).
// place_order 用 std::to_string("1") insert, on_rtn_order 用 CTP 回传 "000000000001" find,
// 修复前两者格式不匹配导致本平台订单全被误判为外部订单.
TEST(OrderRefMapTest, NormalizeOrderRefInsertShortFindPadded) {
    OrderRefMap m;
    // place_order 用非补零格式登记
    m.insert_by_order_ref("1", 100);
    EXPECT_EQ(m.size(), 1u);
    // on_rtn_order 用 CTP 12 位补零格式查表, 应能找到
    const DzOrderId* p = m.find_by_order_ref("000000000001");
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(*p, 100);
    // 反向也成立
    EXPECT_EQ(*m.find_by_order_ref("1"), 100);
}

TEST(OrderRefMapTest, NormalizeOrderRefInsertPaddedFindShort) {
    OrderRefMap m;
    // CTP 回传的外部订单回报用 12 位补零格式 insert
    m.insert_by_order_ref("000000000001", 200);
    // place_order 回滚用 std::to_string 格式 erase, 应能匹配
    EXPECT_EQ(*m.find_by_order_ref("1"), 200);
}

TEST(OrderRefMapTest, NormalizeOrderRefEraseByIdentity) {
    OrderRefMap m;
    m.insert_by_order_ref("1", 100);
    // erase 用补零格式, 应能删除非补零格式登记的条目
    m.erase_by_order_ref("000000000001");
    EXPECT_EQ(m.find_by_order_ref("1"), nullptr);
    EXPECT_EQ(m.find_by_order_ref("000000000001"), nullptr);
    EXPECT_EQ(m.size(), 0u);
}

TEST(OrderRefMapTest, NormalizeOrderRefLargeNumber) {
    OrderRefMap m;
    // 大数也归一化 (CTP OrderRef 最大 12 位 9)
    m.insert_by_order_ref("999999999999", 300);
    EXPECT_EQ(*m.find_by_order_ref("999999999999"), 300);
}

TEST(OrderRefMapTest, Clear) {
    OrderRefMap m;
    m.insert_by_order_ref("1", 100);
    m.insert_by_sys_id("12345", 100);
    m.clear();
    EXPECT_EQ(m.size(), 0u);
    EXPECT_EQ(m.find_by_order_ref("1"), nullptr);
    EXPECT_EQ(m.find_by_sys_id("12345"), nullptr);
}

// ============================================================================
// CancelContext (C4: cancel_order 反向查找)
// ============================================================================

TEST(OrderRefMapTest, CancelContextEmptyReturnsNullptr) {
    OrderRefMap m;
    EXPECT_EQ(m.find_cancel_context(100), nullptr);
}

TEST(OrderRefMapTest, InsertAndFindCancelContext) {
    OrderRefMap m;
    CancelContext ctx{.order_ref = "000000000001", .front_id = 0, .session_id = 0};
    m.insert_cancel_context(100, ctx);
    const CancelContext* p = m.find_cancel_context(100);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->order_ref, "000000000001");
    EXPECT_EQ(p->front_id, 0);
    EXPECT_EQ(p->session_id, 0);
}

TEST(OrderRefMapTest, UpdateCancelContextFrontSessionId) {
    OrderRefMap m;
    CancelContext ctx{.order_ref = "000000000001", .front_id = 0, .session_id = 0};
    m.insert_cancel_context(100, ctx);
    // 模拟 on_rtn_order 收到 CTP 回报后更新 front_id/session_id
    m.update_cancel_context(100, 5, 1234);
    const CancelContext* p = m.find_cancel_context(100);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->front_id, 5);
    EXPECT_EQ(p->session_id, 1234);
}

TEST(OrderRefMapTest, UpdateCancelContextZeroIgnored) {
    OrderRefMap m;
    CancelContext ctx{.order_ref = "000000000001", .front_id = 5, .session_id = 1234};
    m.insert_cancel_context(100, ctx);
    // 0 值应被忽略, 不覆盖已有有效值
    m.update_cancel_context(100, 0, 0);
    const CancelContext* p = m.find_cancel_context(100);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->front_id, 5);
    EXPECT_EQ(p->session_id, 1234);
}

TEST(OrderRefMapTest, UpdateCancelContextNotFoundIgnored) {
    OrderRefMap m;
    // 未 insert 的 order_id, update 应 no-op
    m.update_cancel_context(999, 5, 1234);
    EXPECT_EQ(m.find_cancel_context(999), nullptr);
}

TEST(OrderRefMapTest, ClearAlsoClearsCancelContext) {
    OrderRefMap m;
    CancelContext ctx{.order_ref = "1", .front_id = 0, .session_id = 0};
    m.insert_cancel_context(100, ctx);
    m.clear();
    EXPECT_EQ(m.find_cancel_context(100), nullptr);
}

// ============================================================================
// sync_order_ref (设计 §9.4)
// ============================================================================

TEST(SyncOrderRefTest, CtpMaxLarger) {
    EXPECT_EQ(sync_order_ref(5, 100), 101);
}

TEST(SyncOrderRefTest, CurrentLarger) {
    EXPECT_EQ(sync_order_ref(200, 100), 201);
}

TEST(SyncOrderRefTest, Equal) {
    EXPECT_EQ(sync_order_ref(100, 100), 101);
}

TEST(SyncOrderRefTest, BothZero) {
    EXPECT_EQ(sync_order_ref(0, 0), 1);
}

TEST(SyncOrderRefTest, NegativeCtpMax) {
    EXPECT_EQ(sync_order_ref(50, -1), 51);
}

// ============================================================================
// parse_max_order_ref
// ============================================================================

TEST(ParseMaxOrderRefTest, ValidNumber) {
    EXPECT_EQ(parse_max_order_ref("100"), 100);
    EXPECT_EQ(parse_max_order_ref("0"), 0);
    EXPECT_EQ(parse_max_order_ref("999999"), 999999);
}

TEST(ParseMaxOrderRefTest, EmptyReturnsZero) {
    EXPECT_EQ(parse_max_order_ref(""), 0);
    EXPECT_EQ(parse_max_order_ref(nullptr), 0);
}

TEST(ParseMaxOrderRefTest, NonDigitReturnsZero) {
    EXPECT_EQ(parse_max_order_ref("abc"), 0);
    EXPECT_EQ(parse_max_order_ref("12abc"), 0);
}
