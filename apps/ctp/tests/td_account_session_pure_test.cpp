#include "td/td_account_session_pure.h"

#include <cstring>

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
    EXPECT_EQ(m.find_strategy(100), nullptr);
}

// ============================================================================
// strategy_id 映射 (DzOrderId -> 裸策略名, 回报回填用)
// ============================================================================

TEST(OrderRefMapTest, StrategyInsertAndFind) {
    OrderRefMap m;
    m.insert_strategy(100, "stg_a");
    const std::string* s = m.find_strategy(100);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(*s, "stg_a");
}

TEST(OrderRefMapTest, StrategyFindMissingReturnsNullptr) {
    OrderRefMap m;
    EXPECT_EQ(m.find_strategy(999), nullptr);
}

TEST(OrderRefMapTest, StrategyOverwrite) {
    OrderRefMap m;
    m.insert_strategy(100, "stg_a");
    m.insert_strategy(100, "stg_b");
    EXPECT_EQ(*m.find_strategy(100), "stg_b");
}

TEST(OrderRefMapTest, StrategyErase) {
    OrderRefMap m;
    m.insert_strategy(100, "stg_a");
    m.erase_strategy(100);
    EXPECT_EQ(m.find_strategy(100), nullptr);
}

TEST(OrderRefMapTest, StrategyEraseMissingNoOp) {
    OrderRefMap m;
    m.erase_strategy(999);
    EXPECT_EQ(m.find_strategy(999), nullptr);
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

// ============================================================================
// PositionMirror: 持仓绝对态镜像 (2002 写端 diff, spec §4.1)
// ============================================================================

namespace {

DzPositionInfo make_pos(const char* account, const char* instrument, int8_t direction,
                        int64_t volume, int64_t seq) {
    DzPositionInfo p{};
    std::strcpy(p.account_id, account);
    std::strcpy(p.instrument_id, instrument);
    std::strcpy(p.exchange_id, "CFFEX");
    p.direction = direction;
    p.volume = volume;
    p.frozen_volume = 0;
    p.today_volume = volume;
    p.yd_volume = 0;
    p.price = 3900.0;
    p.seq = static_cast<uint64_t>(seq);
    return p;
}

}  // namespace

TEST(PositionMirrorTest, SameValueNotForwarded) {
    PositionMirror m;
    EXPECT_TRUE(m.update_if_changed(make_pos("acc1", "IF2506", DZ_DIRECTION_LONG, 5, 10)));
    EXPECT_FALSE(m.update_if_changed(make_pos("acc1", "IF2506", DZ_DIRECTION_LONG, 5, 11)));
}

TEST(PositionMirrorTest, ChangedValueForwarded) {
    PositionMirror m;
    EXPECT_TRUE(m.update_if_changed(make_pos("acc1", "IF2506", DZ_DIRECTION_LONG, 5, 10)));
    EXPECT_TRUE(m.update_if_changed(make_pos("acc1", "IF2506", DZ_DIRECTION_LONG, 8, 11)));
    EXPECT_FALSE(m.update_if_changed(make_pos("acc1", "IF2506", DZ_DIRECTION_LONG, 8, 12)));
}

TEST(PositionMirrorTest, DirectionDistinguishesKey) {
    PositionMirror m;
    EXPECT_TRUE(m.update_if_changed(make_pos("acc1", "IF2506", DZ_DIRECTION_LONG, 5, 10)));
    // 同 account+instrument 但 direction 不同 = 不同 key, 首次即转发
    EXPECT_TRUE(m.update_if_changed(make_pos("acc1", "IF2506", DZ_DIRECTION_SHORT, 5, 11)));
    EXPECT_FALSE(m.update_if_changed(make_pos("acc1", "IF2506", DZ_DIRECTION_SHORT, 5, 12)));
}

TEST(PositionMirrorTest, InstrumentDistinguishesKey) {
    PositionMirror m;
    EXPECT_TRUE(m.update_if_changed(make_pos("acc1", "IF2506", DZ_DIRECTION_LONG, 5, 10)));
    EXPECT_TRUE(m.update_if_changed(make_pos("acc1", "rb2510", DZ_DIRECTION_LONG, 3, 11)));
}

TEST(PositionMirrorTest, ClearResetsMirror) {
    PositionMirror m;
    EXPECT_TRUE(m.update_if_changed(make_pos("acc1", "IF2506", DZ_DIRECTION_LONG, 5, 10)));
    m.clear();
    EXPECT_EQ(m.size(), 0u);
    // 清空后同值再次出现视为变化 (重连重建基准)
    EXPECT_TRUE(m.update_if_changed(make_pos("acc1", "IF2506", DZ_DIRECTION_LONG, 5, 11)));
}

// 发现 2 (评审 Important): 全量重灌组内未变化行沿用 DB 既有 seq — seq_of 追溯 + update_seq 同步.
TEST(PositionMirrorTest, SeqAccessorsTrackLastForwardedSeq) {
    PositionMirror m;
    // 首次 (变化): seq_of 暂为 0 (调用方尚未分配新 seq).
    EXPECT_TRUE(m.update_if_changed(make_pos("acc1", "IF2506", DZ_DIRECTION_LONG, 5, 0)));
    EXPECT_EQ(m.seq_of("acc1", "IF2506", DZ_DIRECTION_LONG), 0u);
    // 调用方分配新 seq=42 后同步回镜像.
    m.update_seq("acc1", "IF2506", DZ_DIRECTION_LONG, 42);
    EXPECT_EQ(m.seq_of("acc1", "IF2506", DZ_DIRECTION_LONG), 42u);
    // 未变化再次出现: seq_of 追溯 = 42 (重灌组沿用).
    EXPECT_FALSE(m.update_if_changed(make_pos("acc1", "IF2506", DZ_DIRECTION_LONG, 5, 0)));
    EXPECT_EQ(m.seq_of("acc1", "IF2506", DZ_DIRECTION_LONG), 42u);
}

TEST(PositionMirrorTest, SeqAccessorsMissingKeyAndDifferentKey) {
    PositionMirror m;
    // 未见过 key → seq_of 返回 0; update_seq 对不存在 key no-op (不新增).
    EXPECT_EQ(m.seq_of("acc1", "IF2506", DZ_DIRECTION_LONG), 0u);
    m.update_seq("acc1", "IF2506", DZ_DIRECTION_LONG, 99);
    EXPECT_EQ(m.seq_of("acc1", "IF2506", DZ_DIRECTION_LONG), 0u);
    EXPECT_EQ(m.size(), 0u);

    // 不同 direction / instrument 独立.
    EXPECT_TRUE(m.update_if_changed(make_pos("acc1", "IF2506", DZ_DIRECTION_LONG, 5, 0)));
    m.update_seq("acc1", "IF2506", DZ_DIRECTION_LONG, 7);
    EXPECT_EQ(m.seq_of("acc1", "IF2506", DZ_DIRECTION_SHORT), 0u);
    EXPECT_EQ(m.seq_of("acc1", "rb2510", DZ_DIRECTION_LONG), 0u);
}

// 全平幽灵持仓差集 (发现 A): 镜像有而全量组 (查询响应) 无的 key 返回 —
// 调用方据此发 volume=0 清零帧。组内行不受字段值影响, 只按 key 判定。
TEST(PositionMirrorTest, KeysNotInGroupReportsDisappearedKeys) {
    PositionMirror m;
    EXPECT_TRUE(m.update_if_changed(make_pos("acc1", "IF2506", DZ_DIRECTION_LONG, 5, 10)));
    EXPECT_TRUE(m.update_if_changed(make_pos("acc1", "IF2506", DZ_DIRECTION_SHORT, 2, 11)));
    EXPECT_TRUE(m.update_if_changed(make_pos("acc1", "rb2510", DZ_DIRECTION_LONG, 3, 12)));
    m.update_seq("acc1", "IF2506", DZ_DIRECTION_LONG, 10);
    m.update_seq("acc1", "IF2506", DZ_DIRECTION_SHORT, 11);
    m.update_seq("acc1", "rb2510", DZ_DIRECTION_LONG, 12);

    // 全量组含其中两条 (IF2506 多/空 仍持有), rb2510 全平 (不再出现)。
    std::vector<DzPositionInfo> group;
    group.push_back(make_pos("acc1", "IF2506", DZ_DIRECTION_LONG, 5, 10));
    group.push_back(make_pos("acc1", "IF2506", DZ_DIRECTION_SHORT, 2, 11));
    auto missing = m.keys_not_in_group(group);
    ASSERT_EQ(1u, missing.size());
    EXPECT_STREQ(missing[0].account_id, "acc1");
    EXPECT_STREQ(missing[0].instrument_id, "rb2510");
    EXPECT_EQ(DZ_DIRECTION_LONG, missing[0].direction);

    // 组 = 空 (账户全平): 全部 key 返回。
    auto all = m.keys_not_in_group({});
    ASSERT_EQ(3u, all.size());
}

// 差集按 key 而非字段值: 组内行字段任意 (仅 key 参与比较) 不影响判定。
TEST(PositionMirrorTest, KeysNotInGroupIgnoresGroupFieldValues) {
    PositionMirror m;
    EXPECT_TRUE(m.update_if_changed(make_pos("acc1", "IF2506", DZ_DIRECTION_LONG, 5, 10)));
    EXPECT_TRUE(m.update_if_changed(make_pos("acc1", "rb2510", DZ_DIRECTION_LONG, 3, 11)));

    DzPositionInfo g0 = make_pos("acc1", "IF2506", DZ_DIRECTION_LONG, 5, 10);
    g0.volume = 99;  // 字段值不参与 key 判定
    auto missing = m.keys_not_in_group({g0});
    ASSERT_EQ(1u, missing.size());
    EXPECT_STREQ(missing[0].instrument_id, "rb2510");
}
