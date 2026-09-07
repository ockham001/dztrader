#include "td/td_login_finalize.h"

#include <spdlog/spdlog.h>

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
        return;
    }
    account_done_ = true;
    // 资金完成后进入保证金率查询阶段 (CTP 流控 1 次/秒, 串行发起).
    phase_ = Phase::kQueryMarginRate;
}

void LoginFinalizer::on_margin_rate_done() noexcept {
    if (phase_ != Phase::kQueryMarginRate) {
        return;
    }
    margin_rate_done_ = true;
    // 保证金率完成后进入手续费率查询阶段 (CTP 流控 1 次/秒, 串行发起).
    phase_ = Phase::kQueryCommissionRate;
}

void LoginFinalizer::on_commission_rate_done() noexcept {
    if (phase_ != Phase::kQueryCommissionRate) {
        return;
    }
    commission_rate_done_ = true;
    // 不在此推进: 四查询齐后由 next() 返回首个收尾阶段 (kReplay).
}

bool LoginFinalizer::can_reach_ready() const noexcept {
    // 四查询都完成 (含失败降级) 才 true. 查询失败不阻塞 Ready (spec §4.2).
    return position_done_ && account_done_ && margin_rate_done_ && commission_rate_done_;
}

Phase LoginFinalizer::next() noexcept {
    switch (phase_) {
        case Phase::kQueryPosition:
            // 持仓未完成: 停留 (等 on_position_done).
            break;
        case Phase::kQueryAccount:
            // 资金未完成: 停留. 完成后由 on_account_done 推进阶段.
            break;
        case Phase::kQueryMarginRate:
            // 保证金率未完成: 停留. 完成后由 on_margin_rate_done 推进阶段.
            break;
        case Phase::kQueryCommissionRate:
            // 手续费率未完成: 停留. 四查询齐才推进到 kReplay (首个收尾阶段).
            if (can_reach_ready()) {
                phase_ = Phase::kReplay;
            }
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
