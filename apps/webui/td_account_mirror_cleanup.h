#ifndef DZTRADER_WEBUI_TD_ACCOUNT_MIRROR_CLEANUP_H_
#define DZTRADER_WEBUI_TD_ACCOUNT_MIRROR_CLEANUP_H_

/**
 * @file td_account_mirror_cleanup.h
 * @brief 交易账户**账户级镜像**的清理（ADR 0056 决策 8；契约 general §9、td-status「镜像」、
 *        progress「镜像」节）。
 *
 * 为什么需要独立组件：账户级镜像的 key 改成**账户 ID** 后，`td_status` 与账户级 `progress`
 * 的条目**不含进程名**——`ProcessMirror::mark_stale(网关名)` / `clear()` 那类按进程名的清理
 * 路径覆盖不到它们。网关停止 / 崩溃 / 被移除时只能按**账户注册表**反查该网关的账户集，逐个
 * `mirror_.erase(account_id, "td_status"/"progress")`；**注册表降级时放弃清理**（宁残留不猜，
 * 与"按进程名清"的旧路径相反——查不到不等于没有）。账户从配置帧消失时同样清掉。
 *
 * 清理本身**不广播**：webui-ws §2.2 的消息表是登记制，没有"删除镜像条目"的形状；条目消失
 * 对已连接前端不可见（快照只在建连时推送）。调用方在确有条目被清时广播
 * `data_changed{td}`（webui-ws §2.3 的账户基线失效口径），前端据此重拉注册表/交易基线。
 *
 * 线程安全：依赖 dzweb 固定单线程事件循环（thread_num=1），所有访问串行执行，不加锁。
 */

#include "mirror_store.h"
#include "td_account_registry.h"
#include "td_session_guard.h"

#include <dztrader/platform/process.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <string>
#include <string_view>
#include <vector>

namespace dztrader::webui {

/// 账户级镜像的两个 domain（契约 webui-ws §2.1）：清理必须同时覆盖，漏一个会让前端残留
/// 旧登录态（`td_status`）或旧进度（`progress`）。
inline constexpr std::string_view kTdAccountStatusDomain = "td_status";
inline constexpr std::string_view kTdAccountProgressDomain = "progress";

class TdAccountMirrorCleanup {
public:
    TdAccountMirrorCleanup(MirrorStore& mirror, TdAccountRegistry& registry)
        : mirror_(mirror),
          registry_(registry) {}

    // 引用成员 + 无状态：禁拷贝/移动（与领域服务同约定）
    TdAccountMirrorCleanup(const TdAccountMirrorCleanup&) = delete;
    TdAccountMirrorCleanup& operator=(const TdAccountMirrorCleanup&) = delete;
    TdAccountMirrorCleanup(TdAccountMirrorCleanup&&) = delete;
    TdAccountMirrorCleanup& operator=(TdAccountMirrorCleanup&&) = delete;

    /// 进程状态帧到达：`Stopped` / `Crashed` 的**交易网关** → 清该网关全部账户的账户级镜像。
    ///
    /// 只对 `dztd_*` 生效（账户级镜像只挂交易网关的账户）；非网关进程、其他状态、payload
    /// 非法一律为 0（不猜）。
    ///
    /// @return 实际清掉的账户数（0 = 未触发或访问不到注册表；调用方据此决定是否广播）
    size_t on_process_status(const nlohmann::json& payload) {
        dztrader::platform::ProcessStatus status;
        try {
            status = payload.get<dztrader::platform::ProcessStatus>();
        } catch (const std::exception&) {
            return 0;  // 非法帧：领域服务已记 WARN，这里不重复告警
        }
        if (status.state != dztrader::platform::ChildState::Stopped &&
            status.state != dztrader::platform::ChildState::Crashed) {
            return 0;
        }
        if (!is_td_gateway_name(status.name)) {
            return 0;
        }
        return erase_gateway(status.name);
    }

    /// 网关被移除（`RTN_PROCESS_CONFIG` 中该网关条目消失）：清「网关 ∉ live_gateways」的
    /// 账户级镜像。
    ///
    /// 必须在注册表 `prune_gateways_not_in()` **之前**调用——后者会把归属信息从注册表抹掉，
    /// 抹掉后就再也反查不出"这些账户属于已移除的网关"。注册表降级时放弃清理。
    ///
    /// @param live_gateways 当前进程配置里的 `dztd_*` 进程名（全量）
    /// @return 实际清掉的账户数
    size_t erase_departed_gateways(const std::vector<std::string>& live_gateways) {
        if (!registry_.available()) {
            warn_registry_unavailable("gateway_removed");
            return 0;
        }
        std::vector<std::string> account_ids;
        for (const auto& entry : registry_.entries()) {
            if (!contains(live_gateways, entry.gateway)) {
                account_ids.push_back(entry.account_id);
            }
        }
        return erase_accounts(account_ids);
    }

    /// 账户从配置帧消失（reconcile 之后已不在注册表里）：清其账户级镜像。
    ///
    /// @param candidates reconcile **之前**的注册表账户 ID 全集
    /// @return 实际清掉的账户数
    size_t erase_unregistered(const std::vector<std::string>& candidates) {
        std::vector<std::string> removed;
        for (const auto& account_id : candidates) {
            if (!registry_.contains(account_id)) {
                removed.push_back(account_id);
            }
        }
        return erase_accounts(removed);
    }

    /// 直接清这些账户的 `td_status` / `progress` 镜像（重复 ID 无害）。
    ///
    /// @return 实际被清掉的账户数（两个 domain 都不存在的账户不计）
    size_t erase_accounts(const std::vector<std::string>& account_ids) {
        size_t erased = 0;
        for (const auto& account_id : account_ids) {
            if (account_id.empty()) {
                continue;
            }
            const auto instance = mirror_.instance(account_id);
            const bool had_status = instance.contains(kTdAccountStatusDomain);
            const bool had_progress = instance.contains(kTdAccountProgressDomain);
            if (!had_status && !had_progress) {
                continue;
            }
            if (had_status) {
                mirror_.erase(account_id, kTdAccountStatusDomain);
            }
            if (had_progress) {
                mirror_.erase(account_id, kTdAccountProgressDomain);
            }
            ++erased;
        }
        if (erased > 0) {
            SPDLOG_INFO("td account level mirror cleared | accounts={}", erased);
        }
        return erased;
    }

private:
    /// 网关的账户集来自注册表反查；注册表降级时放弃清理（宁残留不猜，契约 general §9）。
    size_t erase_gateway(const std::string& gateway) {
        if (!registry_.available()) {
            warn_registry_unavailable(gateway);
            return 0;
        }
        return erase_accounts(registry_.account_ids_of_gateway(gateway));
    }

    static bool contains(const std::vector<std::string>& values, const std::string& value) {
        for (const auto& item : values) {
            if (item == value) {
                return true;
            }
        }
        return false;
    }

    void warn_registry_unavailable(const std::string& subject) const {
        SPDLOG_WARN(
            "td account level mirror cleanup skipped | reason=registry_unavailable subject={}",
            subject);
    }

    MirrorStore& mirror_;
    TdAccountRegistry& registry_;
};

}  // namespace dztrader::webui

#endif  // DZTRADER_WEBUI_TD_ACCOUNT_MIRROR_CLEANUP_H_
