#ifndef DZTRADER_CTP_TD_QUERY_PACER_H_
#define DZTRADER_CTP_TD_QUERY_PACER_H_

#include <chrono>

namespace dztrader::ctp {

/// 账户级 CTP 查询闸门（ADR 0055 决策 1；纯逻辑，不碰 CTP / 线程 / 时钟）。
///
/// CTP 的 `ReqQry*` 在同一 API 实例上共用查询流控：同一时刻只允许 1 笔在途
/// （上一笔未收到 `bIsLast` 就发下一笔 → `Req*` 返回 `-2`），且前置按每秒限制
/// （超限 → `-3` / `OnRspError`「查询未就绪」）。因此登录链三笔查询与运行期资金刷新
/// 必须共用同一份「在途 1 笔 + 最小发送间隔」。
///
/// 本类只维护两个事实：
/// - 是否有查询在途（`note_sent` 置位，`note_done` / `reset` 清除）；
/// - 上次发送时刻 + 最小间隔（`can_issue` / `next_eligible`）。
///
/// 使用方（`AccountSession`）在每个 `ReqQry*` 发送点前调用 `can_issue(now)`，
/// 不满足时用 `TimerQueue` 排到 `next_eligible(now)` 再试；发送返回 0 后 `note_sent`，
/// 响应收口 / 发送失败 / 超时 / 断线时 `note_done` 或 `reset`。
class QueryPacer {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    using Duration = Clock::duration;

    /// 契约 td-config 的默认最小间隔：CTP 每秒限制 + 100ms 余量（1100ms）。
    static constexpr int kDefaultMinGapMs = 1100;

    explicit QueryPacer(Duration min_gap = std::chrono::milliseconds(kDefaultMinGapMs)) noexcept;

    /// 更新最小间隔（配置热更新）。<= 0 时夹到 1ms（校验层已拒绝非法值，这里只防除零语义）。
    void set_min_gap(Duration min_gap) noexcept;
    [[nodiscard]] Duration min_gap() const noexcept { return min_gap_; }

    [[nodiscard]] bool in_flight() const noexcept { return in_flight_; }
    [[nodiscard]] bool has_sent() const noexcept { return has_sent_; }

    /// 现在是否能发下一笔：不在途，且距上次发送 >= 最小间隔（从未发过则为 true）。
    [[nodiscard]] bool can_issue(TimePoint now) const noexcept;

    /// 最早可发时刻。在途时返回 `TimePoint::max()`（等响应收口，调用方不应排定时器）。
    [[nodiscard]] TimePoint next_eligible(TimePoint now) const noexcept;

    /// 记录「已发出」并进入在途。调用方保证 `can_issue(now)` 为 true 且请求已被 CTP 接受。
    void note_sent(TimePoint now) noexcept;

    /// 响应收口（is_last / 失败 / 超时）后退出在途。幂等。
    void note_done() noexcept;

    /// 断线 / 重连 / 登出复位：清空在途与上次发送时刻（下次查询立即允许）。
    void reset() noexcept;

private:
    Duration min_gap_;
    bool has_sent_ = false;
    bool in_flight_ = false;
    TimePoint last_sent_{};
};

}  // namespace dztrader::ctp

#endif  // DZTRADER_CTP_TD_QUERY_PACER_H_
