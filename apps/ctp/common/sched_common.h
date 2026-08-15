#ifndef DZTRADER_CTP_SCHED_COMMON_H_
#define DZTRADER_CTP_SCHED_COMMON_H_

#include <string>

#include <nlohmann/json.hpp>

namespace dztrader::ctp {

/// 调度条目 (登录/登出时间对), md/td 网关共用。
/// JSON 序列化为对象数组 [{login_time, logout_time}, ...]。
/// 注意: login_time / logout_time 均为进程所在机器的本地时间,
/// 用户需确保 OS 时区与交易所时区一致 (如 CTP 需设为 Asia/Shanghai)。
struct Schedule {
    std::string login_time;   // "HH:MM" (本地时间)
    std::string logout_time;  // "HH:MM" (本地时间)

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(Schedule, login_time, logout_time)
};

}  // namespace dztrader::ctp

#endif  // DZTRADER_CTP_SCHED_COMMON_H_
