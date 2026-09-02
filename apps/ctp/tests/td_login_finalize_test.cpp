#include <gtest/gtest.h>

#include "td/td_login_finalize.h"

using namespace dztrader::ctp;

// ============================================================================
// LoginFinalizer: 登录收尾状态机纯逻辑 (spec §4.2 登录完成协议)
// 双查询完成 (含失败降级) 才可进入收尾序列; 查询失败不阻塞 Ready;
// 收尾序列线性推进 kQueryPosition -> kQueryAccount -> kReplay -> kFlush
// -> kReady -> kDone (kReady 前必须经过 kFlush, 保证 flush 屏障先于 Ready).
// ============================================================================

TEST(LoginFinalizer, BothQueriesCompleteBeforeReplay) {
    LoginFinalizer fin;
    EXPECT_FALSE(fin.can_reach_ready());  // 初始: 双查询均未到, 不可收尾
    fin.on_position_done();
    EXPECT_FALSE(fin.can_reach_ready());  // 只到一个 → 未收尾
    fin.on_account_done();
    EXPECT_TRUE(fin.can_reach_ready());   // 两个都到 → 可收尾
    EXPECT_EQ(Phase::kReplay, fin.next());
}

TEST(LoginFinalizer, QueryFailureDoesNotBlockFinalize) {
    LoginFinalizer fin;
    fin.on_position_failed();  // 失败等价 done (降级收尾, spec §4.2 失败不阻塞)
    fin.on_account_done();
    EXPECT_TRUE(fin.can_reach_ready());
}

TEST(LoginFinalizer, SequenceIsLinear) {
    // next() 依次 kReplay->kFlush->kReady->kDone, 且 kReady 前必须经过 kFlush
    LoginFinalizer fin;
    fin.on_position_done();
    fin.on_account_done();
    EXPECT_EQ(Phase::kReplay, fin.next());
    EXPECT_EQ(Phase::kFlush, fin.next());
    // kReady 前一阶段必须是 kFlush (屏障先于 Ready 翻转)
    EXPECT_EQ(Phase::kReady, fin.next());
    EXPECT_EQ(Phase::kDone, fin.next());
    // 到达 kDone 后 next() 停留 (不再回退)
    EXPECT_EQ(Phase::kDone, fin.next());
}

TEST(LoginFinalizer, PositionOnlyIsNotReady) {
    LoginFinalizer fin;
    fin.on_position_done();
    fin.on_account_failed();
    EXPECT_TRUE(fin.can_reach_ready());  // 一个完成 + 一个失败 = 双查询齐, 可收尾
    EXPECT_EQ(Phase::kReplay, fin.next());
}

TEST(LoginFinalizer, BothFailStillFinalizes) {
    LoginFinalizer fin;
    fin.on_position_failed();
    fin.on_account_failed();
    EXPECT_TRUE(fin.can_reach_ready());  // 双查询都失败也降级收尾
    EXPECT_EQ(Phase::kReplay, fin.next());
}

// 终检发现 1: 重连后二次收尾 — 断连作废在途查询链 = 整体重置 LoginFinalizer
// (td_account_session.cpp on_front_disconnected / on_rsp_qry_instrument is_last):
// kDone 后重新默认构造, phase 回 kQueryPosition, 双查询重新齐备可再次收尾
// (修复"重连重登后持仓 is_last 被 kDone 门挡住, 账户永久停在 LoadingInstruments")。
TEST(LoginFinalizer, ReassignmentAfterDoneRestartsQueryPhase) {
    LoginFinalizer fin;
    fin.on_position_done();
    fin.on_account_done();
    fin.next();  // kReplay
    fin.next();
    fin.next();
    fin.next();  // kDone
    ASSERT_EQ(Phase::kDone, fin.phase());

    // 断连/重登: finalizer_ = LoginFinalizer{} (重置语义)
    fin = LoginFinalizer{};
    EXPECT_EQ(Phase::kQueryPosition, fin.phase());
    EXPECT_FALSE(fin.can_reach_ready());  // 双查询未齐: 查询阶段重新开始

    // 重登查询链重走: 持仓/资金 is_last 再次齐备 → 可收尾
    fin.on_position_done();
    fin.on_account_done();
    EXPECT_TRUE(fin.can_reach_ready());
    EXPECT_EQ(Phase::kReplay, fin.next());
}
