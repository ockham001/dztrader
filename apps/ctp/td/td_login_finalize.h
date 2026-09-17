#ifndef DZTRADER_CTP_TD_LOGIN_FINALIZE_H_
#define DZTRADER_CTP_TD_LOGIN_FINALIZE_H_

// LoginFinalizer: 登录收尾状态机纯逻辑 (spec §4.2 登录完成协议).
// 无 CTP/SHM/DB 依赖, 可完全独立单测.
//
// 驱动: 合约加载完成后发起持仓/资金两查询 (CTP 流控 1 次/秒, 串行),
// 每个查询的 is_last 或失败回调驱动对应 on_*_done/on_*_failed.
// 收尾: 两查询都完成 (含失败降级) 才 can_reach_ready()==true; 查询失败
// 不阻塞 Ready (spec §4.2: 先转 Ready 不阻塞交易, 就绪后由定时补查),
// 收尾序列由 next() 线性推进:
//   kQueryPosition -> kQueryAccount -> kReplay -> kFlush -> kReady -> kDone
// kReady 前必经 kFlush —— 保证 AccountSession 在调 on_instruments_loaded()
// (转 Ready) 之前先执行 persist flush 屏障, 使 Ready 成为 "DB 已稳定" 标记.

#include <cstdint>

namespace dztrader::ctp {

/// 登录收尾阶段 (线性推进).
enum class Phase : uint8_t {
    kQueryPosition,  ///< 持仓查询发起/响应中
    kQueryAccount,   ///< 资金查询发起/响应中 (持仓完成后发起)
    kReplay,         ///< 缓冲回报重放 (经过滤器)
    kFlush,          ///< persist flush 屏障 (排空后才允许 Ready)
    kReady,          ///< 状态机翻转 Ready (on_instruments_loaded)
    kDone,           ///< 收尾完成
};

/// 收尾状态机. 调用方按 next() 推进并执行对应阶段的动作.
class LoginFinalizer {
public:
    LoginFinalizer() = default;

    /// 当前阶段. 初始 kQueryPosition.
    [[nodiscard]] Phase phase() const noexcept { return phase_; }

    /// 持仓查询完成 (is_last 或失败降级, spec §4.2 失败不阻塞).
    void on_position_done() noexcept;
    /// 持仓查询失败 (等价 done).
    void on_position_failed() noexcept { on_position_done(); }

    /// 资金查询完成 (is_last 或失败降级).
    void on_account_done() noexcept;
    /// 资金查询失败 (等价 done).
    void on_account_failed() noexcept { on_account_done(); }

    /// 两查询都完成 (含失败) 才 true —— 查询阶段未结束时不可进入收尾序列.
    [[nodiscard]] bool can_reach_ready() const noexcept;

    /// 线性推进到下一阶段. 未满足前置 (两查询未齐 / 阶段顺序) 时停留当前阶段.
    /// 已到 kDone 后调用返回 kDone (不再回退).
    [[nodiscard]] Phase next() noexcept;

private:
    Phase phase_ = Phase::kQueryPosition;
    bool position_done_ = false;  ///< 持仓查询已完结 (成功或失败)
    bool account_done_ = false;   ///< 资金查询已完结 (成功或失败)
};

}  // namespace dztrader::ctp

#endif  // DZTRADER_CTP_TD_LOGIN_FINALIZE_H_
