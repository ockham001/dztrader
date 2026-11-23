#ifndef DZTRADER_CTP_TD_QUERY_LANE_H_
#define DZTRADER_CTP_TD_QUERY_LANE_H_

#include <array>
#include <cstdint>
#include <optional>

#include "td/td_query_pacer.h"

namespace dztrader::ctp {

/// 运行期按需查询种类（ADR 0055）。当前只有资金刷新；新增种类时在此追加并同步
/// `kQueryKindCount` 与调用侧 switch（编译期数组大小保证漏改会编译失败）。
enum class QueryKind : uint8_t {
    TradingAccount = 0,  ///< 资金快照（ReqQryTradingAccount）
};

/// QueryKind 的个数（数组维度）。
constexpr std::size_t kQueryKindCount = 1;

/// 把 QueryKind 转成数组下标（越界返回 0，仅供日志/防御；合法值恒在范围内）。
[[nodiscard]] constexpr std::size_t query_kind_index(QueryKind kind) noexcept {
    const auto idx = static_cast<std::size_t>(kind);
    return idx < kQueryKindCount ? idx : 0;
}

/// 运行期查询调度器（ADR 0055 决策 2；纯逻辑，不碰 CTP / 线程 / 时钟）。
///
/// 语义：
/// - **置脏只置位**：`mark_dirty` 递增该种类的脏序号；一次查询覆盖「发送时刻之前」的全部
///   置脏（`covered` = 发送时的脏序号）；发送之后到达的置脏在响应收口后仍待查（不丢）。
/// - **在途 1 笔 + 最小间隔**：由共用 `QueryPacer` 保证；`take_ready` 只在 pacer 允许且
///   退避已到期时返回可发种类。
/// - **失败有界退避**：`note_failure` 递增尝试次数，第 n 次等待 `min_gap * 2^(n-1)`（封顶 5s）；
///   超过最大重试次数后丢弃本次待查（`exhausted` 一次性标记，调用方负责通知），
///   下一个事件 / 手动刷新可再次置脏。
/// - **无周期查询**：本类不产生时间源，调用方只在事件与 `next_wake` 时刻驱动。
class QueryLane {
public:
    using Clock = QueryPacer::Clock;
    using TimePoint = QueryPacer::TimePoint;
    using Duration = QueryPacer::Duration;

    /// 失败退避封顶（ADR 0055 决策 6）。
    static constexpr int kDefaultMaxRetries = 3;
    static constexpr Duration kBackoffCap = std::chrono::seconds(5);

    explicit QueryLane(QueryPacer& pacer, int max_retries = kDefaultMaxRetries) noexcept;

    /// 置脏：该种类需要一个新查询。只置位，不清除未覆盖的变化。
    void mark_dirty(QueryKind kind) noexcept;

    /// 该种类是否有「发送后尚未覆盖」的置脏。
    [[nodiscard]] bool pending(QueryKind kind) const noexcept;

    /// 该种类是否因失败耗尽而放弃本次待查（一次性；调用方消费后须 `clear_exhausted`）。
    [[nodiscard]] bool exhausted(QueryKind kind) const noexcept;

    /// 消费「失败耗尽」标记（清 attempts；保持 pending 已清空的状态）。
    void clear_exhausted(QueryKind kind) noexcept;

    /// 已有多少个种类可立即发送：返回第一个可发种类（调用方发送成功后必须 `note_sent`；
    /// 发送失败则 `note_failure`，成功收口则 `note_response`）。
    [[nodiscard]] std::optional<QueryKind> take_ready(TimePoint now) noexcept;

    /// 下一次需要唤醒的时刻（< = now 表示可立即发送；nullopt = 无待查 / 有在途）。
    [[nodiscard]] std::optional<TimePoint> next_wake(TimePoint now) const noexcept;

    /// 记录已发出：pacer 进入在途，本次覆盖当前全部置脏。
    void note_sent(TimePoint now, QueryKind kind) noexcept;

    /// 响应成功收口：退出在途、清尝试次数；若发送后又置脏则继续待查（受 pacer 间隔约束）。
    void note_response(TimePoint now, QueryKind kind) noexcept;

    /// 失败收口（发送失败 / OnRspError / 业务错误 / 超时）：退出在途并进入退避。
    void note_failure(TimePoint now, QueryKind kind) noexcept;

    /// 断线 / 离开 Ready：清空脏、在途、退避与耗尽标记，并复位 pacer。
    void reset() noexcept;

    /// 该种类的尝试次数（测试与日志用）。
    [[nodiscard]] int attempts(QueryKind kind) const noexcept;

    [[nodiscard]] bool in_flight() const noexcept { return in_flight_.has_value(); }
    [[nodiscard]] std::optional<QueryKind> in_flight_kind() const noexcept { return in_flight_; }

private:
    [[nodiscard]] Duration backoff_for(int attempts) const noexcept;
    [[nodiscard]] bool retry_ready(QueryKind kind, TimePoint now) const noexcept;

    QueryPacer& pacer_;
    int max_retries_;
    std::array<uint64_t, kQueryKindCount> dirty_seq_{};
    std::array<uint64_t, kQueryKindCount> covered_seq_{};
    std::array<int, kQueryKindCount> attempts_{};
    std::array<bool, kQueryKindCount> exhausted_{};
    std::array<bool, kQueryKindCount> retry_pending_{};
    std::array<bool, kQueryKindCount> has_retry_at_{};
    std::array<TimePoint, kQueryKindCount> retry_at_{};
    std::optional<QueryKind> in_flight_;
};

}  // namespace dztrader::ctp

#endif  // DZTRADER_CTP_TD_QUERY_LANE_H_
