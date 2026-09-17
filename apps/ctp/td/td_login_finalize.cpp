#include "td/td_login_finalize.h"

namespace dztrader::ctp {

void LoginFinalizer::on_position_done() noexcept {
    if (phase_ != Phase::kQueryPosition) {
        // 防重复/乱序回调: 非查询阶段到达时忽略 (不破坏线性推进).
        return;
    }
    position_done_ = true;
    // 持仓完成后进入资金查询阶段 (CTP 流控 1 次/秒, 串行发起).
    phase_ = Phase::kQueryAccount;
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
    // 两查询都完成 (含失败降级) 才 true. 查询失败不阻塞 Ready (spec §4.2).
    return position_done_ && account_done_;
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
