#ifndef DZTRADER_PLATFORM_PROCESS_H_
#define DZTRADER_PLATFORM_PROCESS_H_

#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <dztrader/core/json_enum.h>

namespace dztrader::platform {

// ===== 枚举（契约 process）=====

/// 进程控制动作（契约 process）
enum class ProcessAction : uint8_t {
    Start,   ///< 启动进程
    Stop,    ///< 停止进程
    Remove,  ///< 移除进程（停止 + 清理配置）
};

/// 子进程状态（契约 process）
enum class ChildState : uint8_t {
    Starting,  ///< 启动中
    Running,   ///< 运行中
    Stopping,  ///< 已发送关闭请求，等待退出
    Stopped,   ///< 已正常退出
    Crashed,   ///< 已异常退出（exit_code != 0 或 spawn 失败）
};

/// 进程操作结果事件（契约 process），仅在 RTN_PROCESS_STATUS 中出现
enum class ProcessEvent : uint8_t {
    StartSucceeded,   ///< 启动成功；已在 Running（幂等，重试安全）
    StartFailed,      ///< 启动失败
    StopSucceeded,    ///< 停止请求已受理并已派发（重试安全）
    StopFailed,       ///< 停止失败（进程不存在）
    RemoveSucceeded,  ///< 移除流程已启动
    RemoveFailed,     ///< 移除失败（目标不存在，不幂等）
};

DZ_JSON_ENUM(ProcessAction)
DZ_JSON_ENUM(ChildState)
DZ_JSON_ENUM(ProcessEvent)

// ===== 结构体（契约 process）=====

/// 子进程重启策略（契约 process）。
/// 全量角色三字段必填；enabled=false 时 max_attempts 忽略；实际退避 = backoff_sec × 连续崩溃次数，最小 1s。
struct RestartPolicy {
    bool enabled = false;   ///< 是否启用自动重启
    int max_attempts = 0;   ///< 最大重启次数（>= 0）
    int backoff_sec = 5;    ///< 重启退避间隔（秒，>= 0）
};

/// 进程配置，全量角色（契约 process）。
/// RTN_PROCESS_CONFIG payload 中作为 map 的 value：args/env/restart 必须出现；
/// display_name 可缺失（缺失 = 未设置，前端回退到进程名）。
/// 增量角色（SET/Start 携带的 patch）不由此类型表达，见 validate_process_config_patch。
struct ProcessConfig {
    std::vector<std::string> args;
    std::map<std::string, std::string> env;   ///< map：确定性输出（key 有序）
    RestartPolicy restart;
    std::optional<std::string> display_name;  ///< nullopt = 未设置
};

/// REQUEST_PROCESS_CONTROL payload（契约 process）
struct ProcessControlReq {
    ProcessAction action = ProcessAction::Stop;
    std::string target;
    std::optional<nlohmann::json> config;  ///< 配置 patch，仅 action=Start 有效；null 非法
};

/// RTN_PROCESS_STATUS payload（契约 process），单条完整状态
struct ProcessStatus {
    std::string name;
    ChildState state = ChildState::Stopped;
    int pid = 0;                        ///< 未运行时为 0
    std::string display_name;           ///< 缺省空串；仅用于展示，不回写配置镜像
    std::string message;                ///< 缺省空串
    std::optional<ProcessEvent> event;  ///< 缺失 = 自发状态变化
};

/// SET_PROCESS_CONFIG payload（契约 process；payload 命名由设计文档补充）
struct SetProcessConfigReq {
    std::string target;
    nlohmann::json config;  ///< 配置 patch，必须为 object；null 非法
};

// ===== 序列化（手写：缺失 ≠ 默认值、空串省略，契约 process）=====

inline void to_json(nlohmann::json& j, const RestartPolicy& p) {
    j = nlohmann::json{{"enabled", p.enabled},
                       {"max_attempts", p.max_attempts},
                       {"backoff_sec", p.backoff_sec}};
}

inline void from_json(const nlohmann::json& j, RestartPolicy& p) {
    j.at("enabled").get_to(p.enabled);
    j.at("max_attempts").get_to(p.max_attempts);
    j.at("backoff_sec").get_to(p.backoff_sec);
}

inline void to_json(nlohmann::json& j, const ProcessConfig& c) {
    j = nlohmann::json{{"args", c.args}, {"env", c.env}, {"restart", c.restart}};
    if (c.display_name && !c.display_name->empty()) {
        j["display_name"] = *c.display_name;
    }
}

inline void from_json(const nlohmann::json& j, ProcessConfig& c) {
    j.at("args").get_to(c.args);
    j.at("env").get_to(c.env);
    j.at("restart").get_to(c.restart);
    if (j.contains("display_name") && !j["display_name"].is_null()) {
        c.display_name = j["display_name"].get<std::string>();
    } else {
        c.display_name = std::nullopt;
    }
}

inline void to_json(nlohmann::json& j, const ProcessControlReq& req) {
    j = nlohmann::json{{"action", req.action}, {"target", req.target}};
    if (req.config) {
        j["config"] = *req.config;
    }
}

inline void from_json(const nlohmann::json& j, ProcessControlReq& req) {
    j.at("action").get_to(req.action);
    j.at("target").get_to(req.target);
    if (j.contains("config")) {
        if (j["config"].is_null()) {
            // 契约 process：除 env 内部 value 外任何位置 null 均为校验失败
            throw std::runtime_error("config must not be null");
        }
        req.config = j["config"];
    } else {
        req.config = std::nullopt;
    }
}

inline void to_json(nlohmann::json& j, const ProcessStatus& st) {
    j = nlohmann::json{{"name", st.name}, {"state", st.state}, {"pid", st.pid}};
    if (!st.display_name.empty()) {
        j["display_name"] = st.display_name;
    }
    if (!st.message.empty()) {
        j["message"] = st.message;
    }
    if (st.event) {
        j["event"] = *st.event;
    }
}

inline void from_json(const nlohmann::json& j, ProcessStatus& st) {
    j.at("name").get_to(st.name);
    j.at("state").get_to(st.state);
    j.at("pid").get_to(st.pid);
    st.display_name = j.value("display_name", std::string());
    st.message = j.value("message", std::string());
    if (j.contains("event") && !j["event"].is_null()) {
        st.event = j["event"].get<ProcessEvent>();
    } else {
        st.event = std::nullopt;
    }
}

inline void to_json(nlohmann::json& j, const SetProcessConfigReq& req) {
    j = nlohmann::json{{"target", req.target}, {"config", req.config}};
}

inline void from_json(const nlohmann::json& j, SetProcessConfigReq& req) {
    j.at("target").get_to(req.target);
    if (!j.contains("config") || j["config"].is_null()) {
        throw std::runtime_error("config is required and must not be null");
    }
    req.config = j["config"];
}

// ===== 校验（契约 process 校验规则唯一真相源）=====

namespace detail {

/// 整数或整数值的 double 均接受（02-shm 惯例，总则 §6"值在目标类型范围内即可"）。
/// 注：命名与 shm_config.h 的 detail::get_int_value 区分（同为共享命名空间
/// dztrader::platform::detail 内的 inline 函数），避免同 TU 包含两个头时重定义冲突。
inline bool is_int_value(const nlohmann::json& j) {
    if (j.is_number_integer()) {
        // nlohmann: is_number_integer() 含 number_unsigned; 超出 int64 范围的
        // unsigned 在 to_int64 的 get<int64_t>() 会抛 out_of_range,
        // 违背校验函数"返回错误消息或 nullopt"契约, 此处显式拒绝
        return !j.is_number_unsigned() ||
               j.get<uint64_t>() <= static_cast<uint64_t>((std::numeric_limits<int64_t>::max)());
    }
    if (j.is_number_float()) {
        const auto d = j.get<double>();
        return std::isfinite(d) && d == std::floor(d) &&
               d >= static_cast<double>((std::numeric_limits<int64_t>::min)()) &&
               d < std::ldexp(1.0, 63);
    }
    return false;
}

inline int64_t to_int64(const nlohmann::json& j) {
    return j.is_number_integer() ? j.get<int64_t>() : static_cast<int64_t>(j.get<double>());
}

}  // namespace detail

/// 校验增量角色 patch（SET_PROCESS_CONFIG / Start 携带的 config，契约 process确认同规则）。
/// RFC 7386 语义：出现字段覆盖，缺失字段保留。返回错误消息或 nullopt。
/// 注意：必须定义在 validate_process_config_full 之前（full 内部调用本函数）。
inline std::optional<std::string> validate_process_config_patch(const nlohmann::json& patch) {
    if (!patch.is_object()) {
        return "process config patch must be a JSON object";  // 契约 process
    }
    if (patch.contains("args")) {  // 整体覆盖（契约 process）
        const auto& v = patch["args"];
        if (v.is_null()) {
            return "args must not be null";
        }
        if (!v.is_array()) {
            return "args must be an array";
        }
        for (const auto& item : v) {
            if (!item.is_string()) {
                return "args elements must be strings";
            }
        }
    }
    if (patch.contains("env")) {  // 递归合并（契约 process）
        const auto& v = patch["env"];
        if (v.is_null()) {
            return "env must not be null";
        }
        if (!v.is_object()) {
            return "env must be an object";
        }
        for (auto it = v.begin(); it != v.end(); ++it) {
            const auto& val = it.value();
            if (!val.is_null() && !val.is_string()) {
                return "env value must be string or null: " + it.key();
            }
        }
    }
    if (patch.contains("restart")) {  // 整体覆盖（契约 process）
        const auto& v = patch["restart"];
        if (v.is_null()) {
            return "restart must not be null";
        }
        if (!v.is_object()) {
            return "restart must be an object";
        }
        for (const char* key : {"enabled", "max_attempts", "backoff_sec"}) {
            if (!v.contains(key)) {
                return std::string("restart.") + key + " is required";
            }
        }
        if (!v["enabled"].is_boolean()) {
            return "restart.enabled must be a boolean";
        }
        for (const char* key : {"max_attempts", "backoff_sec"}) {
            const auto& n = v[key];
            if (!detail::is_int_value(n)) {
                return std::string("restart.") + key + " must be an integer";
            }
            if (detail::to_int64(n) < 0) {
                return std::string("restart.") + key + " must be >= 0";  // 契约 process
            }
            // 契约字段类型为 int: 钳制到 int32 范围, 避免 nlohmann get_to(int) 静默回绕
            if (detail::to_int64(n) > (std::numeric_limits<int>::max)()) {
                return std::string("restart.") + key + " must be <= " +
                       std::to_string((std::numeric_limits<int>::max)());
            }
        }
    }
    if (patch.contains("display_name")) {  // 覆盖，空串=清空（契约 process）
        const auto& v = patch["display_name"];
        if (v.is_null()) {
            return "display_name must not be null";
        }
        if (!v.is_string()) {
            return "display_name must be a string";
        }
    }
    return std::nullopt;
}

/// 校验全量角色配置（RTN / 文件加载）：args/env/restart 必须出现且类型正确；
/// display_name 可选。返回错误消息或 nullopt。
inline std::optional<std::string> validate_process_config_full(const nlohmann::json& cfg) {
    if (!cfg.is_object()) {
        return "process config must be a JSON object";
    }
    for (const char* key : {"args", "env", "restart"}) {
        if (!cfg.contains(key)) {
            return std::string("missing required field: ") + key;
        }
    }
    // 类型/null/范围规则与增量角色一致
    if (auto err = validate_process_config_patch(cfg)) {
        return err;
    }
    // 全量角色额外约束: env 内部 value 必须为 string
    // (null 仅为增量角色的删除语义; 全量 env 是实际环境变量, 契约 process,
    // 且 ProcessConfig::from_json 的 map<string,string> 对 null 抛异常)
    for (auto it = cfg["env"].begin(); it != cfg["env"].end(); ++it) {
        if (!it.value().is_string()) {
            return "env value must be a string: " + it.key();
        }
    }
    return std::nullopt;
}

/// 应用增量 patch 到全量配置（RFC 7386 选择性合并，契约 process）。
/// 规则：args/restart 整体覆盖（非递归）；env 递归合并（value 为 null 删除该 key）；
/// display_name 覆盖（空串 = 清空，合并结果中移除该字段）；未知字段忽略。
/// 前置条件：patch 已通过 validate_process_config_patch（本函数不重复校验，
/// 调用方保证先校验；仅按已知合法结构处理）。current 为全量角色合法配置。
/// 返回合并后的新 json，current 不变（纯函数，无副作用）。
inline nlohmann::json apply_process_config_patch(const nlohmann::json& current,
                                                 const nlohmann::json& patch) {
    nlohmann::json merged = current;
    if (patch.contains("args")) {
        merged["args"] = patch["args"];  // 整体覆盖（契约 process）
    }
    if (patch.contains("env")) {  // 递归合并（契约 process）
        for (auto it = patch["env"].begin(); it != patch["env"].end(); ++it) {
            if (it.value().is_null()) {
                merged["env"].erase(it.key());  // null = 删除该 key
            } else {
                merged["env"][it.key()] = it.value();
            }
        }
    }
    if (patch.contains("restart")) {
        merged["restart"] = patch["restart"];  // 整体覆盖（契约 process）
    }
    if (patch.contains("display_name")) {
        if (patch["display_name"].get<std::string>().empty()) {
            merged.erase("display_name");  // 空串 = 清空（契约 process）
        } else {
            merged["display_name"] = patch["display_name"];
        }
    }
    return merged;
}

}  // namespace dztrader::platform

#endif  // DZTRADER_PLATFORM_PROCESS_H_
