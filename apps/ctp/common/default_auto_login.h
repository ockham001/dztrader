#ifndef DZTRADER_CTP_COMMON_DEFAULT_AUTO_LOGIN_H_
#define DZTRADER_CTP_COMMON_DEFAULT_AUTO_LOGIN_H_

#include <nlohmann/json.hpp>

namespace dztrader::ctp {

/// 交易网关 (dztd_ctp) 内置默认排程：日盘 08:45~15:45，夜盘 20:45~02:33。
///
/// 这是「网关内置默认排程」，只在配置文件缺失 / 排程为空数组时经
/// platform::AutoLoginConfig 归一后生效；运维一旦写入自定义排程即固化（配置优先），
/// 不会回落到本默认表。
///
/// 形式说明：返回新构造的 json 值而非静态常量，避开静态初始化顺序问题，
/// 且调用点语义清晰（每次取用为独立副本，调用方修改不影响下一次取值）。
///
/// 刻意不做段数 / 区间重叠校验——校验规则维持现状（#390 拍板）。
/// @return 长度为 2 的 json 数组：[{login_time, logout_time}, ...]
[[nodiscard]] inline nlohmann::json default_schedules_td() {
    return nlohmann::json::array({
        {{"login_time", "08:45"}, {"logout_time", "15:45"}},
        {{"login_time", "20:45"}, {"logout_time", "02:33"}},
    });
}

/// 行情网关 (dzmd_ctp) 内置默认排程：日盘 08:55~15:20，夜盘 20:55~02:33。
///
/// 与交易网关刻意错峰（行情晚登录、早登出），延续旧版实盘节奏。
/// 生效条件与配置优先语义同 default_schedules_td()。
/// @return 长度为 2 的 json 数组：[{login_time, logout_time}, ...]
[[nodiscard]] inline nlohmann::json default_schedules_md() {
    return nlohmann::json::array({
        {{"login_time", "08:55"}, {"logout_time", "15:20"}},
        {{"login_time", "20:55"}, {"logout_time", "02:33"}},
    });
}

}  // namespace dztrader::ctp

#endif  // DZTRADER_CTP_COMMON_DEFAULT_AUTO_LOGIN_H_
