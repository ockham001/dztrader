#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <vector>

#include "td/td_login_finalize.h"

using namespace dztrader::ctp;

// ============================================================================
// LoginFinalizer: 登录收尾状态机纯逻辑 (spec §4.2 登录完成协议)
// 持仓查询成功 (零行也算成功) + 资金查询完结 才可进入收尾序列;
// 持仓查询重试耗尽 = 本次登录失败, 停留 kQueryPosition、不再可收尾 (ADR 0049 决策 3);
// 资金查询失败降级仍不阻塞 Ready (资金行只在持仓成功后写入, ADR 0045 决策 4).
// 收尾序列线性推进 kQueryPosition -> kQueryAccount -> kReplay -> kFlush
// -> kReady -> kDone (kReady 前必须经过 kFlush, 保证 flush 屏障先于 Ready).
// 覆盖边界: 本文件测状态机相位 + run_flush_barrier 纯逻辑 (ADR 0042 §5: 超时发
// ERROR popup 一次后仍返回, 成功不通知); AccountSession 级接线 (实际
// enqueue_flush_signal/wait_flush) 属会话层 — 该层无测试基建 (需 CTP/SHM 全套),
// 不在本文件覆盖范围.
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

// 持仓查询重试耗尽 = 本次登录失败 (ADR 0049 决策 3): 不再等价于 done,
// 停住不推进、不可收尾 —— 否则会带着空基准转 Ready, 把「没有持仓」伪装成正常账户.
TEST(LoginFinalizer, PositionFailureStopsFinalize) {
    LoginFinalizer fin;
    fin.on_position_failed();
    EXPECT_TRUE(fin.position_failed());
    EXPECT_FALSE(fin.can_reach_ready());
    EXPECT_EQ(Phase::kQueryPosition, fin.phase());
    EXPECT_EQ(Phase::kQueryPosition, fin.next());  // next() 不越过失败的查询阶段
    // 资金查询本就不会在持仓查询失败后发起; 即便迟到回调, 也不得让收尾从失败中复活.
    fin.on_account_done();
    EXPECT_FALSE(fin.can_reach_ready());
    EXPECT_EQ(Phase::kQueryPosition, fin.phase());
}

// 资金查询失败仍降级收尾 (ADR 0045 决策 4 保留: 资金行只在持仓成功后写入,
// 资金查询失败只是少一行资金, 不阻塞可交易).
TEST(LoginFinalizer, AccountFailureDoesNotBlockFinalize) {
    LoginFinalizer fin;
    fin.on_position_done();
    fin.on_account_failed();
    EXPECT_FALSE(fin.position_failed());
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

// ============================================================================
// run_flush_barrier: 登录 flush 屏障纯逻辑 (ADR 0042 §5 / 契约 td-data-sync §4)
// ============================================================================

namespace {

/// 通知记录器: 断言级别/文本/popup 次数.
struct NotifyRecord {
    DzNotifyLevel level = 0;
    std::string message;
    bool popup = false;
};

class NotifyRecorder {
public:
    FlushNotifyFn callback() {
        return [this](DzNotifyLevel level, const std::string& message, bool popup) {
            records_.push_back(NotifyRecord{level, message, popup});
        };
    }

    const std::vector<NotifyRecord>& records() const { return records_; }

private:
    std::vector<NotifyRecord> records_;
};

}  // namespace

TEST(LoginFlushBarrier, TimeoutReturnsTimedOutAndEmitsErrorOnce) {
    NotifyRecorder recorder;
    const auto result = run_flush_barrier([](std::chrono::milliseconds) { return false; },
                                          std::chrono::seconds(5), recorder.callback(), "acc1");

    EXPECT_EQ(result, FlushBarrierResult::TimedOut);
    ASSERT_EQ(recorder.records().size(), 1u);
    const auto& rec = recorder.records().front();
    EXPECT_EQ(rec.level, DZ_NOTIFY_ERROR);
    EXPECT_TRUE(rec.popup);
    EXPECT_EQ(rec.message,
              "persist flush timeout during login, proceeding to ready | account=acc1");
}

TEST(LoginFlushBarrier, CommittedReturnsCommittedWithoutNotification) {
    NotifyRecorder recorder;
    int wait_calls = 0;
    std::chrono::milliseconds observed_timeout{0};
    const auto result = run_flush_barrier(
        [&wait_calls, &observed_timeout](std::chrono::milliseconds timeout) {
            ++wait_calls;
            observed_timeout = timeout;
            return true;
        },
        std::chrono::milliseconds(500), recorder.callback(), "acc2");

    EXPECT_EQ(result, FlushBarrierResult::Committed);
    EXPECT_EQ(wait_calls, 1);
    EXPECT_EQ(observed_timeout, std::chrono::milliseconds(500));
    EXPECT_TRUE(recorder.records().empty());
}

TEST(LoginFlushBarrier, TimeoutWithEmptyNotifyStillTimesOut) {
    // notify 为空仅日志 (ADR 0042 §5): 不通知 UI, 但结果语义不变.
    const auto result = run_flush_barrier([](std::chrono::milliseconds) { return false; },
                                          std::chrono::seconds(5), {}, "acc3");
    EXPECT_EQ(result, FlushBarrierResult::TimedOut);
}
