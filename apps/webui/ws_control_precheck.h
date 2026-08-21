#ifndef DZTRADER_WEBUI_WS_CONTROL_PRECHECK_H_
#define DZTRADER_WEBUI_WS_CONTROL_PRECHECK_H_

#include <optional>
#include <string>

#include <dztrader/platform/process.h>
#include <nlohmann/json.hpp>
#include "control_guard.h"

namespace dztrader::webui {

/// WS md_connect/md_disconnect 预检结果（契约 webui-ws §3：admin + source 非空 +
/// 事件通道可用 + 目标进程镜像 Running）。从 handle_control_message 提取为纯函数以便单测
/// （WsController 依赖 drogon 连接对象无法脱离框架实例化）。
/// 判定顺序收敛到共享 `evaluate_control_guard`（control_guard.h），此处仅做
/// WS 语义映射 + 消息文本（SourceInvalid/ChannelUnavailable 归并为 InvalidSource，
/// 与 WS 原有"invalid source or writer not ready"消息一致）。
enum class MdConnectPrecheck {
    Ok,                  ///< 全部通过，可写帧
    NotAdmin,            ///< 当前连接非 admin
    InvalidSource,       ///< source 缺失 或 事件通道不可用
    ProcessNotRunning,   ///< 目标进程镜像非 Running（含镜像未就绪，保守拒绝）
};

/// 逐道守卫判定（与 REST /login /logout 一致）。writer_ready = event_writer_ 可用。
inline MdConnectPrecheck evaluate_md_connect_precheck(
    bool is_admin, const std::string& source, bool writer_ready,
    const std::optional<dztrader::platform::ChildState>& state) {
    const bool process_running = state.has_value() && *state == dztrader::platform::ChildState::Running;
    switch (evaluate_control_guard(is_admin, !source.empty(), writer_ready, process_running)) {
        case ControlGuard::NotAdmin: return MdConnectPrecheck::NotAdmin;
        case ControlGuard::SourceInvalid:
        case ControlGuard::ChannelUnavailable: return MdConnectPrecheck::InvalidSource;
        case ControlGuard::ProcessNotRunning: return MdConnectPrecheck::ProcessNotRunning;
        case ControlGuard::Ok: return MdConnectPrecheck::Ok;
    }
    return MdConnectPrecheck::InvalidSource;
}

/// 预检失败时回给前端的 error message（契约 webui-ws §2.3/§3）
inline std::string md_connect_precheck_message(MdConnectPrecheck r, const std::string& source) {
    switch (r) {
        case MdConnectPrecheck::NotAdmin: return "admin required";
        case MdConnectPrecheck::InvalidSource: return "invalid source or writer not ready";
        case MdConnectPrecheck::ProcessNotRunning: return "process not running: " + source;
        case MdConnectPrecheck::Ok: return {};
    }
    return {};
}

/// query_md_subscriptions 的 SHM payload 构造（契约 md-subscription：dzweb 不校验
/// query 取值与互斥——同时出现时两者均透传，由目标 md 进程经 RTN.error=ambiguous_query
/// 表达；payload 不含 source，source 作为 ext_inst_id）。
/// 返回 false 表示 payload 缺少有效 query/instruments（回 error，不写帧）。
inline bool build_subscription_query_payload(const nlohmann::json& payload,
                                             nlohmann::json& out) {
    const bool has_query = payload.contains("query") && payload["query"].is_string();
    const bool has_instruments = payload.contains("instruments") && payload["instruments"].is_array();
    if (has_query) out["query"] = payload["query"];
    if (has_instruments) out["instruments"] = payload["instruments"];
    return has_query || has_instruments;
}

}  // namespace dztrader::webui

#endif  // DZTRADER_WEBUI_WS_CONTROL_PRECHECK_H_