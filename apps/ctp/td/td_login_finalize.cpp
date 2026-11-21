#include "td/td_login_finalize.h"

#include <format>
#include <string>

#include <spdlog/spdlog.h>

namespace dztrader::ctp {

FlushBarrierResult run_flush_barrier(const FlushWaitFn& wait_flushed,
                                     std::chrono::milliseconds timeout,
                                     const FlushNotifyFn& notify,
                                     std::string_view account_id) {
    if (wait_flushed(timeout)) {
        return FlushBarrierResult::Committed;
    }
    // 超时放行 Ready (ADR 0042 §5): 持久化降级不得阻塞交易, 只发分级 ERROR 通知.
    const std::string message = std::format(
        "persist flush timeout during login, proceeding to ready | account={}", account_id);
    SPDLOG_ERROR("{}", message);
    if (notify) {
        notify(DZ_NOTIFY_ERROR, message, true);
    }
    return FlushBarrierResult::TimedOut;
}

void LoginFinalizer::on_position_done() noexcept {
    if (phase_ != Phase::kQueryPosition) {
        // 防重复/乱序回调: 非查询阶段到达时忽略 (不破坏线性推进).
        return;
    }
    position_done_ = true;
    // 持仓完成后进入资金查询阶段 (CTP 流控 1 次/秒, 串行发起).
    phase_ = Phase::kQueryAccount;
}

void LoginFinalizer::on_position_failed() noexcept {
    if (phase_ != Phase::kQueryPosition) {
        // 防重复/乱序回调: 非查询阶段到达时忽略 (不破坏线性推进).
        return;
    }
    // 重试预算已由调用方在调本方法前用尽 (td_account_session_pure.h 的
    // kPositionQueryMaxRetries). 停在 kQueryPosition: next() 不推进,
    // can_reach_ready() 恒 false —— 本次登录终止, 等用户处理.
    position_failed_ = true;
}

void LoginFinalizer::on_account_done() noexcept {
    if (phase_ != Phase::kQueryAccount) {
        // 防重复/乱序回调: 迟到/重复 is_last 到达时忽略, 不破坏相位 (幂等).
        return;
    }
    account_done_ = true;
    phase_ = Phase::kReplay;  // 两查询齐, 进入收尾序列
}

bool LoginFinalizer::can_reach_ready() const noexcept {
    // 持仓查询必须成功 (它建的是持仓基准, ADR 0049 决策 3);
    // 资金查询失败降级不阻塞 Ready (资金行只在持仓成功后写入, ADR 0045 决策 4).
    return position_done_ && !position_failed_ && account_done_;
}

Phase LoginFinalizer::next() noexcept {
    switch (phase_) {
        case Phase::kQueryPosition:
        case Phase::kQueryAccount:
            // 查询未完成: 停留 (等 SPI 回调推进).
            break;
        case Phase::kReplay:
            // 缓冲重放后进入 flush 屏障阶段 (kReady 前必经 kFlush).
            phase_ = Phase::kFlush;
            break;
        case Phase::kFlush:
            // flush 屏障完成后才转 Ready.
            phase_ = Phase::kReady;
            break;
        case Phase::kReady:
            // Ready 翻转后收尾完成.
            phase_ = Phase::kDone;
            break;
        case Phase::kDone:
            // 终态: 停留, 不再回退.
            break;
    }
    return phase_;
}

}  // namespace dztrader::ctp
