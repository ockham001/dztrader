#ifndef DZTRADER_CTP_TD_TD_STATUS_PUBLISH_H_
#define DZTRADER_CTP_TD_TD_STATUS_PUBLISH_H_

#include <string>
#include <unordered_map>
#include <vector>

#include "td/td_state.h"

namespace dztrader::ctp {

/// 状态重推判据的键（契约 td-status §3 触发场景 1）：
/// 状态机翻转、date / login_time 填充或清空、进度变化都需要重推
/// RTN_TD_STATUS + RTN_PROGRESS；ext 里的未知枚举计数等不参与判定
/// （它们不是状态字段，随下一次推送带出即可，避免每笔未知回报都触发状态帧）。
struct TdStatusPublishKey {
    TdState state = TdState::Idle;
    int progress_current = 0;
    std::string date;
    std::string login_time;

    bool operator==(const TdStatusPublishKey&) const = default;
};

/// 从 TdStatus 提取重推键（纯函数）。
inline TdStatusPublishKey td_status_publish_key(const TdStatus& status) {
    return {.state = status.state,
            .progress_current = status.progress_current,
            .date = status.date,
            .login_time = status.login_time};
}

/// 主循环 sweep 的输入：一个会话的 account_id + 当前重推键。
struct TdSessionStatusSnapshot {
    std::string account_id;
    TdStatusPublishKey key;
};

/// 计算本轮需要重推 RTN_TD_STATUS/RTN_PROGRESS 的账户（纯函数，无 I/O）：
/// 账户首次出现（新会话）或键变化（状态/进度/date/login_time 变化）→ 需要推；
/// 键未变化 → 不重复推（契约要求「状态变化时推送」，不是定时灌帧）。
///
/// 为什么用它而不是在每次状态机调用处手动推送：td 的状态转移点分散在 SPI 回调、
/// 定时器（登录/连接超时）与跨日登出路径，逐个补推送点必漏；主循环每轮比较一次
/// 键，任何来源的变化都会在下一轮被捕获（#409 落地时发现的既有缺口：
/// update_status 此前只由快照/配置变更调用，登录状态转移不推帧）。
inline std::vector<std::string> publish_changed_accounts(
    const std::vector<TdSessionStatusSnapshot>& sessions,
    const std::unordered_map<std::string, TdStatusPublishKey>& published) {
    std::vector<std::string> changed;
    changed.reserve(sessions.size());
    for (const auto& session : sessions) {
        const auto it = published.find(session.account_id);
        if (it == published.end() || !(it->second == session.key)) {
            changed.push_back(session.account_id);
        }
    }
    return changed;
}

}  // namespace dztrader::ctp

#endif  // DZTRADER_CTP_TD_TD_STATUS_PUBLISH_H_
