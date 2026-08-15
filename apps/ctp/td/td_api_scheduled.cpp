#include "td/td_api.h"

#include <chrono>
#include <string>
#include <vector>

#include <magic_enum/magic_enum.hpp>
#include <spdlog/spdlog.h>

#include <dztrader/date_time/date_time.h>

namespace dztrader::ctp {

// ============================================================================
// td_api_scheduled.cpp: 自动调度 (设计 §5.7, 参考 md_api_scheduled.cpp)
// - schedule_auto_sched_timer: 对齐到下个分钟 25 秒排定定时器 (错峰, 避开整分钟 0 秒)
// - on_sched_timer: 读 system_clock, 评估动作 (自动登录/登出), 重排定时器
// - try_recover_login: --recover 启动时若在会话区间内, 对所有 enabled 账户补登
// 与 md_api_scheduled.cpp 的差异: td 多账户, 登录/登出动作作用于所有 enabled 账户
// ============================================================================

void TdApi::schedule_auto_sched_timer() {
    // 计算到下个分钟 25 秒的延迟 (错峰: 避开整分钟 0 秒的繁忙时段)
    // 与 md_api_scheduled.cpp schedule_auto_sched_timer 完全一致
    auto now = DateTime::local_now();
    int current_sec = now.second();
    int delay_sec = 0;
    if (current_sec < 25) {
        delay_sec = 25 - current_sec;  // 当前分钟的 25 秒还没到
    } else {
        delay_sec = (60 - current_sec) + 25;  // 到下个分钟的 25 秒
    }

    timer_queue_.schedule_after_replace("auto_sched", std::chrono::seconds(delay_sec),
                                        [this]() { on_sched_timer(); });
}

void TdApi::on_sched_timer() {
    // 读 system_clock 获取当前本地时间 (用户需确保 OS 时区与交易所时区一致)
    // 评估动作 (每次唤醒全量重评估, 不依赖上次状态), 重排定时器
    try {
        auto now = DateTime::local_now();
        auto hh_mm = now.to_string("%H:%M");
        int weekday = static_cast<int>(now.weekday());  // ISO 8601: 1=周一...7=周日

        // 多账户: is_logged_in 取 "是否存在任意 session" (与 md 单 session 的差异)
        // 评估结果: Login 时连接所有未连接的 enabled 账户, Logout 时断开所有 sessions
        // 排程单一真相源：auto_login_config_（契约 05-auto-login 迁移完成）
        bool is_logged_in = !sessions_.empty();
        auto sched = to_sched_view(auto_login_config_.config());
        auto action = evaluate_sched_action(sched, weekday, hh_mm, is_logged_in);

        switch (action) {
            case AutoSchedAction::Login: {
                SPDLOG_INFO("auto sched login | time={} weekday={}", hh_mm, weekday);
                // 对所有 enabled 且未连接的账户调用 connect_account_by_id
                // (已连接的跳过, 幂等)
                for (const auto& acc : config_.accounts) {
                    if (acc.enabled && find_session(acc.account_id) == nullptr) {
                        connect_account_by_id(acc.account_id);
                    }
                }
                break;
            }
            case AutoSchedAction::Logout: {
                SPDLOG_INFO("auto sched logout | time={} weekday={}", hh_mm, weekday);
                // 收集 account_id 列表, 避免迭代器失效 (disconnect_account_by_id 会 erase)
                std::vector<std::string> ids;
                ids.reserve(sessions_.size());
                for (const auto& [id, _] : sessions_) {
                    ids.push_back(id);
                }
                for (const auto& id : ids) {
                    disconnect_account_by_id(id);
                }
                break;
            }
            case AutoSchedAction::None:
                break;
        }
    } catch (const std::exception& e) {
        SPDLOG_ERROR("sched timer failed | error=\"{}\"", e.what());
    }
    // 重排: 重新对齐到下个分钟 25 秒 (避免回调耗时累积导致漂移)
    try {
        schedule_auto_sched_timer();
    } catch (const std::exception& e) {
        SPDLOG_ERROR("schedule_auto_sched_timer failed | error=\"{}\"", e.what());
    }
}

void TdApi::try_recover_login() {
    // --recover 崩溃恢复: 启动时若在会话区间内, 对所有 enabled 账户补登
    // 参考 md_api_ctp.cpp try_recover_login, 差异: td 多账户, 遍历 config_.accounts
    if (!recover_) {
        return;
    }

    auto now = DateTime::local_now();
    auto hh_mm = now.to_string("%H:%M");
    int weekday = static_cast<int>(now.weekday());

    // 1. 星期过滤 (周一 06:00 ~ 周六 05:59)
    if (!is_trading_window(weekday, hh_mm)) {
        SPDLOG_INFO("recover skipped | reason=\"non_trading_window\" time={} weekday={}", hh_mm,
                    weekday);
        return;
    }

    // 2. 会话区间判断 (任意 schedule 的 [login_time, logout_time) 内)
    //    排程单一真相源：auto_login_config_
    auto sched = to_sched_view(auto_login_config_.config());
    if (!should_be_logged_in(sched, hh_mm)) {
        SPDLOG_INFO("recover skipped | reason=\"not_in_session\" time={}", hh_mm);
        return;
    }

    // 3. 对所有 enabled 账户补登 (已连接的跳过, 幂等)
    SPDLOG_INFO("recover login | time={} weekday={}", hh_mm, weekday);
    for (const auto& acc : config_.accounts) {
        if (acc.enabled && find_session(acc.account_id) == nullptr) {
            connect_account_by_id(acc.account_id);
        }
    }
}

}  // namespace dztrader::ctp
