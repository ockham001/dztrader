#ifndef DZTRADER_WEBUI_TD_SESSION_GUARD_H_
#define DZTRADER_WEBUI_TD_SESSION_GUARD_H_

#include <dztrader/platform/process.h>

#include <string>

#include "process_mirror.h"
#include "td_account_registry.h"

namespace dztrader::webui {

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
    GatewayNotRunning,    ///< 目标网关镜像非 Running（含镜像未就绪，保守拒绝）
    ChannelUnavailable,   ///< 事件通道 writer 不可用（API-only 模式）
};

/// 守卫结果；`gateway` 在解析出账户归属后填充（`Ok` 与 `GatewayNotRunning` 时非空，
/// 供写帧与错误文案使用）。
struct TdSessionCheck {
    TdSessionGuard result = TdSessionGuard::Ok;
    std::string gateway;
};

/// 统一守卫纯函数。
/// @param is_admin        当前 REST 请求 / WS 连接是否为 admin
/// @param registry        账户注册表（解析 account_id → gateway；nullptr = 未装配）
/// @param process_mirror  进程镜像（目标网关是否 Running）
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
    bool running = false;
    if (process_mirror != nullptr) {
        const auto status = process_mirror->get_status(entry->gateway);
        running = status.has_value() && status->state == platform::ChildState::Running;
    }
    if (!running) {
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
