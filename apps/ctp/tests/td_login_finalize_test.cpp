#include <gtest/gtest.h>

#include "td/td_login_finalize.h"

using namespace dztrader::ctp;

// ============================================================================
// LoginFinalizer: 登录收尾状态机纯逻辑 (spec §4.2 登录完成协议)
// 两查询 (持仓/资金) 完成 (含失败降级) 才可进入收尾序列; 查询失败不阻塞 Ready;
// 收尾序列线性推进 kQueryPosition -> kQueryAccount -> kReplay -> kFlush
// -> kReady -> kDone (kReady 前必须经过 kFlush, 保证 flush 屏障先于 Ready).
// 覆盖边界: 本文件只测状态机相位; kFlush 的真实屏障行为 (drive_finalizer 实际
// enqueue_flush_signal/wait_flush 后才转 Ready, 超时仍继续) 属 AccountSession 级 —
// 该层无测试基建 (需 CTP/SHM 全套), 不在本文件覆盖范围.
// ============================================================================

TEST(LoginFinalizer, TwoQueriesCompleteBeforeReplay) {
    LoginFinalizer fin;
    EXPECT_FALSE(fin.can_reach_ready());
    fin.on_position_done();
    EXPECT_FALSE(fin.can_reach_ready());
    EXPECT_EQ(Phase::kQueryAccount, fin.phase());
    fin.on_account_done();
    EXPECT_TRUE(fin.can_reach_ready());
    EXPECT_EQ(Phase::kReplay, fin.phase());  // 账毕即入收尾序列
    EXPECT_EQ(Phase::kFlush, fin.next());
    EXPECT_EQ(Phase::kReady, fin.next());
    EXPECT_EQ(Phase::kDone, fin.next());
    EXPECT_EQ(Phase::kDone, fin.next());  // 终态停留
}

TEST(LoginFinalizer, QueryFailureDoesNotBlockFinalize) {
    LoginFinalizer fin;
    fin.on_position_failed();  // 失败等价 done (降级收尾, spec §4.2)
    fin.on_account_failed();
    EXPECT_TRUE(fin.can_reach_ready());
    EXPECT_EQ(Phase::kReplay, fin.phase());
}

TEST(LoginFinalizer, StaysInQueryPhaseUntilCallback) {
    LoginFinalizer fin;
    // 两查询未回调前 next() 停留 (drive_finalizer 不越过查询阶段)
    EXPECT_EQ(Phase::kQueryPosition, fin.next());
    fin.on_position_done();
    EXPECT_EQ(Phase::kQueryAccount, fin.next());
}

TEST(LoginFinalizer, AccountDoneTwiceIsIdempotent) {
    LoginFinalizer fin;
    fin.on_position_done();
    fin.on_account_done();
    EXPECT_EQ(Phase::kReplay, fin.phase());
    fin.on_account_done();  // 迟到/重复 is_last 不破坏相位
    EXPECT_EQ(Phase::kReplay, fin.phase());
    EXPECT_TRUE(fin.can_reach_ready());
}

// §2.2 缺失: 重复/迟到的持仓 is_last — on_rsp_qry_investor_position 的
// phase_ != kQueryPosition 忽略分支 (含 kReplay 后再次到达) 不得把相位打回查询阶段.
TEST(LoginFinalizer, PositionDoneTwiceIsIdempotent) {
    LoginFinalizer fin;
    fin.on_position_done();
    EXPECT_EQ(Phase::kQueryAccount, fin.phase());
    fin.on_position_done();  // 迟到/重复 is_last: 相位不回退
    EXPECT_EQ(Phase::kQueryAccount, fin.phase());

    fin.on_account_done();
    ASSERT_EQ(Phase::kReplay, fin.phase());
    fin.on_position_done();  // kReplay 后再次到达: 不破坏收尾相位
    EXPECT_EQ(Phase::kReplay, fin.phase());
    EXPECT_TRUE(fin.can_reach_ready());
}

// 终检发现 1: 重连后二次收尾 — 断连作废在途查询链 = 整体重置 LoginFinalizer
// (td_account_session.cpp on_front_disconnected / on_rsp_qry_instrument is_last):
// kDone 后重新默认构造, phase 回 kQueryPosition, 两查询重新齐备可再次收尾。
TEST(LoginFinalizer, ReassignmentAfterDoneRestartsQueryPhase) {
    LoginFinalizer fin;
    fin.on_position_done();
    fin.on_account_done();
    (void)fin.next();  // kFlush
    (void)fin.next();  // kReady
    (void)fin.next();  // kDone
    ASSERT_EQ(Phase::kDone, fin.phase());

    fin = LoginFinalizer{};
    EXPECT_EQ(Phase::kQueryPosition, fin.phase());
    EXPECT_FALSE(fin.can_reach_ready());

    fin.on_position_done();
    fin.on_account_done();
    EXPECT_TRUE(fin.can_reach_ready());
    EXPECT_EQ(Phase::kReplay, fin.phase());
}
