#ifndef DZTRADER_WEBUI_TD_SESSION_GUARD_H_
#define DZTRADER_WEBUI_TD_SESSION_GUARD_H_

#include <dztrader/platform/process.h>

#include <string>
#include <string_view>

#include "process_mirror.h"
#include "td_account_registry.h"

namespace dztrader::webui {

/// 交易网关进程名前缀（契约 process 的 `dztd_*` 命名，契约 rest §2.10 预检 2）。
inline constexpr std::string_view kTdGatewayNamePrefix = "dztd_";

/// 进程名是否以交易网关前缀开头（`dztd_*`）。**交易网关前缀谓词的唯一实现**：
/// 会话控制守卫（本头）、REST 交易接口生命周期端点（`td_gateway_controller.cpp`）共用。
///
/// 只判前缀，不判「前缀后是否有内容」——需要可扫描 exe stem 的调用点（如 start 预检 3
/// 的动态注册前置）必须自行加长度约束；`dztd_` 退化名不能成为任何进程身份。
inline bool is_td_gateway_name(std::string_view name) {
    return name.starts_with(kTdGatewayNamePrefix);
}

/// 交易账户会话控制（登录/登出）的统一守卫（契约 td-connection「前端义务」/ rest §2.9.10）：
/// REST 与 WS 双通道只调用这一处，避免两套平行判定。与进程级 control_guard.h 同构，
/// 差异是多了「account_id → 网关」的解析（交易网关是一进程多账户）。
///
/// 判定顺序固定：is_admin → 账户已登记（注册表解析网关）→ 目标网关镜像 Running → 写通道可用。
enum class TdSessionGuard {
    Ok,                   ///< 全部通过，可下发
    NotAdmin,             ///< 非 admin（REST 403 `admin_required`）
    AccountInvalid,       ///< account_id 为空或不在账户注册表（REST 404 `account_not_found`）
    RegistryUnavailable,  ///< 注册表降级（尚未收到配置帧且未从 platform.db 恢复条目）
    GatewayNotRunning,    ///< 目标网关未注册为 dztd_* 或镜像非 Running（含未就绪，保守拒绝）
    ChannelUnavailable,   ///< 事件通道 writer 不可用（API-only 模式）
};

/// 守卫结果；`gateway` 在解析出账户归属后填充（`Ok` 与 `GatewayNotRunning` 时非空，
/// 供写帧与错误文案使用）。
struct TdSessionCheck {
    TdSessionGuard result = TdSessionGuard::Ok;
    std::string gateway;
};

/// 目标网关是否在进程配置中且是 `dztd_*`（契约 rest §2.9.10 预检 3）。
/// 与配置写端点的预检同义：未注册的进程名不该收到会话控制请求（总则 §7 兜底会把误操作
/// 变成超时）。账户注册表能解析出网关只说明「曾收到过配置帧」，进程配置消失后仍可能有残留。
inline bool td_gateway_registered(const ProcessMirror& process_mirror, const std::string& gateway) {
    return is_td_gateway_name(gateway) && process_mirror.get_process_config(gateway).has_value();
}

/// 目标网关在进程镜像中是否为 Running。
inline bool td_gateway_running(const ProcessMirror& process_mirror, const std::string& gateway) {
    const auto status = process_mirror.get_status(gateway);
    return status.has_value() && status->state == platform::ChildState::Running;
}

/// 资金手动刷新（契约 rest §2.9.11）的守卫结果：无 admin 区分（任意已认证用户），
/// 但「注册表 → 网关 Running → 账户在线 → 写通道」顺序与错误映射与 session guard 同源。
enum class TdRefreshGuard {
    Ok,
    RegistryUnavailable,  ///< 注册表未装配 / 降级 → REST 503 account registry unavailable
    AccountInvalid,       ///< 账户未登记 → REST 404 account_not_found
    GatewayNotRunning,    ///< 网关未注册为 dztd_* 或镜像非 Running → REST 409 gateway_not_running
    AccountOffline,       ///< 账户不在在线（Ready）集合 → REST 409 account_offline
    ChannelUnavailable,   ///< 事件通道 writer 不可用 → REST 503 shm writer not available
};

/// 资金刷新守卫纯函数（唯一实现；`td_data_controller` 的 POST refresh 端点消费）。
/// @param writer_ready 事件通道 writer 是否就绪
/// @param account_online 账户是否在 dzweb 在线（Ready）集合
inline TdRefreshGuard evaluate_td_refresh_guard(TdAccountRegistry* registry,
                                                ProcessMirror* process_mirror,
                                                std::string_view account_id,
                                                bool account_online,
                                                bool writer_ready) {
    if (registry == nullptr || !registry->available()) {
        return TdRefreshGuard::RegistryUnavailable;
    }
    if (account_id.empty() || !registry->find(std::string(account_id)).has_value()) {
        return TdRefreshGuard::AccountInvalid;
    }
    const auto entry = registry->find(std::string(account_id));
    if (process_mirror == nullptr || !td_gateway_registered(*process_mirror, entry->gateway) ||
        !td_gateway_running(*process_mirror, entry->gateway)) {
        return TdRefreshGuard::GatewayNotRunning;
    }
    if (!account_online) {
        return TdRefreshGuard::AccountOffline;
    }
    if (!writer_ready) {
        return TdRefreshGuard::ChannelUnavailable;
    }
    return TdRefreshGuard::Ok;
}

/// 统一守卫纯函数。
/// @param is_admin        当前 REST 请求 / WS 连接是否为 admin
/// @param registry        账户注册表（解析 account_id → gateway；nullptr = 未装配）
/// @param process_mirror  进程镜像（目标网关是否已注册为 `dztd_*` 且 Running）
/// @param writer_ready    事件通道 writer 是否可用
/// @param account_id      目标账户 ID
inline TdSessionCheck evaluate_td_session_guard(bool is_admin,
                                                const TdAccountRegistry* registry,
                                                const ProcessMirror* process_mirror,
                                                bool writer_ready,
                                                const std::string& account_id) {
    if (!is_admin) {
        return {.result = TdSessionGuard::NotAdmin, .gateway = {}};
    }
    // 注册表降级时「查不到」不代表「不存在」，无法解析归属 → 与普通未登记账户区分，
    // REST 回 503（基础设施暂不可用，不带 code）、不写帧。
    if (registry == nullptr || !registry->available()) {
        return {.result = TdSessionGuard::RegistryUnavailable, .gateway = {}};
    }
    if (account_id.empty()) {
        return {.result = TdSessionGuard::AccountInvalid, .gateway = {}};
    }
    const auto entry = registry->find(account_id);
    if (!entry.has_value()) {
        return {.result = TdSessionGuard::AccountInvalid, .gateway = {}};
    }
    TdSessionCheck check{.result = TdSessionGuard::Ok, .gateway = entry->gateway};
    const bool gateway_ready = process_mirror != nullptr &&
                               td_gateway_registered(*process_mirror, entry->gateway) &&
                               td_gateway_running(*process_mirror, entry->gateway);
    if (!gateway_ready) {
        check.result = TdSessionGuard::GatewayNotRunning;
        return check;
    }
    if (!writer_ready) {
        check.result = TdSessionGuard::ChannelUnavailable;
        return check;
    }
    return check;
}

}  // namespace dztrader::webui

#endif  // DZTRADER_WEBUI_TD_SESSION_GUARD_H_
