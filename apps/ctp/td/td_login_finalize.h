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
// (转 Ready) 之前先执行 persist flush 屏障. Ready 的语义是 "登录完成、可交易"
// (不承诺 DB 稳定): 屏障超时 5s 放行 Ready, 由 ERROR 通知表达持久化降级 (ADR 0042 §5).
//
// run_flush_barrier: kFlush 阶段的屏障纯逻辑 (ADR 0042 §5). 超时只发 ERROR 通知
// 并返回 TimedOut, 由调用方照常推进 Ready —— 持久化降级不得阻塞交易.

#include <chrono>
#include <cstdint>
#include <functional>
#include <string_view>

#include "td/td_notify_types.h"  // TdNotifyFn (进程内通知唯一类型)

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

// ============================================================================
// 登录 flush 屏障纯逻辑 (ADR 0042 §5: 5s 超时放行 Ready, 只发 ERROR 通知)
// ============================================================================

/// flush 屏障结果. 两者调用方都照常推进 Ready, 仅供日志/测试区分.
enum class FlushBarrierResult : uint8_t {
    Committed,  ///< 队列已排空且末批已提交
    TimedOut,   ///< 超时放行 (已发 ERROR 通知)
};

/// 等待 persist 排空 (返回 true = 末批已提交). timeout 为本次等待上限.
using FlushWaitFn = std::function<bool(std::chrono::milliseconds timeout)>;

/// 持久化通知回调 (别名到进程内唯一类型 TdNotifyFn).
/// 本头不 include writer 头: 保持登录收尾纯逻辑无 tdstore/CTP/SQLite 依赖, 可独立单测.
using FlushNotifyFn = TdNotifyFn;

/// 登录 flush 屏障: 等待 persist 排空. 超时发 ERROR 通知 (popup=true) 并返回 TimedOut
/// (调用方照常推进 Ready; ADR 0042 §5). 通知文本固定为
/// "persist flush timeout during login, proceeding to ready | account=<id>"; notify 为空仅日志.
[[nodiscard]] FlushBarrierResult run_flush_barrier(const FlushWaitFn& wait_flushed,
                                                   std::chrono::milliseconds timeout,
                                                   const FlushNotifyFn& notify,
                                                   std::string_view account_id);

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
