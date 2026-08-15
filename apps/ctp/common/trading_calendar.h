#ifndef DZTRADER_CTP_TRADING_CALENDAR_H_
#define DZTRADER_CTP_TRADING_CALENDAR_H_

#include <concepts>
#include <string_view>
#include <vector>

#include "common/sched_common.h"

namespace dztrader::ctp {

/// 交易日窗口起始 (周一 06:00)
inline constexpr std::string_view TRADING_WINDOW_START = "06:00";
/// 交易日窗口结束 (周六 06:00, 不含)
inline constexpr std::string_view TRADING_WINDOW_END = "06:00";

/// 自动调度评估结果。
enum class AutoSchedAction {
    None,    ///< 无动作
    Login,   ///< 应登录 (匹配 login_time 且当前未登录)
    Logout,  ///< 应登出 (匹配 logout_time 且当前已登录)
};

/// 判断是否为交易日允许时段 (周一 06:00 ~ 周六 05:59)。
/// @param weekday 星期 (1=周一, ..., 7=周日, ISO 8601 编码, 与 DateTime::weekday() 一致)
/// @param hh_mm 当前 "HH:MM" (交易所时区)
/// @return true 表示在允许时段内
bool is_trading_window(int weekday, std::string_view hh_mm);

/// ConfigT 概念: 须暴露 enable_auto_login_logout (bool) 与 schedules (vector<Schedule>)。
/// 注: 契约 04 迁移后 md/td 的 Config 类不再含这两个字段, 本概念由 AutoLoginSchedView
/// （json 镜像适配视图, 见下）与测试用 FakeTdConfig 满足; 网关调度器经 to_sched_view
/// 转换 auto_login 帧镜像后复用本模块的评估函数。
template <typename ConfigT>
concept AutoSchedConfig = requires(const ConfigT& c) {
    { c.enable_auto_login_logout } -> std::convertible_to<bool>;
    { c.schedules } -> std::convertible_to<const std::vector<Schedule>&>;
};

/// 评估当前是否在任意 schedule 的会话区间内 (用于 --recover 补登)。
/// 会话区间 [login_time, logout_time): 跨午夜时为 [login, 24:00) ∪ [00:00, logout)。
/// @param cfg 配置 (含 schedules + enable_auto_login_logout)
/// @param hh_mm 当前 "HH:MM"
/// @return true 表示当前应处于登录状态
template <AutoSchedConfig ConfigT>
bool should_be_logged_in(const ConfigT& cfg, std::string_view hh_mm);

/// 评估当前分钟应执行的动作。
/// @param cfg 配置
/// @param weekday 当前星期 (1=周一...7=周日, ISO 8601 编码)
/// @param hh_mm 当前 "HH:MM"
/// @param is_logged_in 当前是否已登录 (state == LoggedIn)
/// @return 动作枚举 (None/Login/Logout)
template <AutoSchedConfig ConfigT>
AutoSchedAction evaluate_sched_action(const ConfigT& cfg, int weekday,
                         std::string_view hh_mm, bool is_logged_in);

// 模板实现 (header-only, 避免显式实例化)

template <AutoSchedConfig ConfigT>
bool should_be_logged_in(const ConfigT& cfg, std::string_view hh_mm) {
    if (!cfg.enable_auto_login_logout) {
        return false;
    }
    for (const auto& s : cfg.schedules) {
        if (s.login_time <= s.logout_time) {
            // 同一天: [login, logout)
            if (s.login_time <= hh_mm && hh_mm < s.logout_time) {
                return true;
            }
        } else {
            // 跨午夜: [login, 24:00) ∪ [00:00, logout)
            if (hh_mm >= s.login_time || hh_mm < s.logout_time) {
                return true;
            }
        }
    }
    return false;
}

template <AutoSchedConfig ConfigT>
AutoSchedAction evaluate_sched_action(const ConfigT& cfg, int weekday,
                         std::string_view hh_mm, bool is_logged_in) {
    if (!cfg.enable_auto_login_logout) {
        return AutoSchedAction::None;
    }
    if (!is_trading_window(weekday, hh_mm)) {
        return AutoSchedAction::None;
    }
    // logout 优先于 login (避免同一时刻冲突)
    for (const auto& s : cfg.schedules) {
        if (s.logout_time == hh_mm && is_logged_in) {
            return AutoSchedAction::Logout;
        }
    }
    for (const auto& s : cfg.schedules) {
        if (s.login_time == hh_mm && !is_logged_in) {
            return AutoSchedAction::Login;
        }
    }
    return AutoSchedAction::None;
}

/// AutoLoginConfig json 镜像 -> AutoSchedConfig 兼容视图（调度器消费用）。
/// 字段名映射：json "enabled" -> enable_auto_login_logout（AutoSchedConfig concept 要求）。
/// 每次调用构造临时 vector<Schedule>，调度器每分钟调用一次，开销可忽略。
struct AutoLoginSchedView {
    bool enable_auto_login_logout = true;
    std::vector<Schedule> schedules;
};

inline AutoLoginSchedView to_sched_view(const nlohmann::json& cfg) {
    AutoLoginSchedView v;
    v.enable_auto_login_logout = cfg.value("enabled", true);
    if (cfg.contains("schedules") && cfg["schedules"].is_array()) {
        for (const auto& s : cfg["schedules"]) {
            v.schedules.push_back(Schedule{
                s.value("login_time", ""),
                s.value("logout_time", ""),
            });
        }
    }
    return v;
}

}  // namespace dztrader::ctp

#endif  // DZTRADER_CTP_TRADING_CALENDAR_H_
