#ifndef DZTRADER_WEBUI_TD_CONFIG_DOMAIN_SERVICE_H_
#define DZTRADER_WEBUI_TD_CONFIG_DOMAIN_SERVICE_H_

#include "data_change_notifier.h"
#include "mirror_store.h"
#include "td_account_registry.h"
#include "ws_broadcaster.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <optional>
#include <string>
#include <vector>

namespace dztrader::webui {

/// 交易网关配置领域服务：`RTN_TD_CONFIG` 的镜像更新 + 账户注册表 reconcile + WS 广播
/// （契约 td-config §4/§5/§8）。
///
/// 形态对齐 `MdConfigDomainService`，差异：
/// - 镜像 domain 是 `td_config`（key = 网关进程名，即帧头 `instance_id`）；
/// - 广播消息 `td_rtn_config` 的 `instance_id` = 网关进程名（契约 webui-ws §2.2）——
///   md 侧把 `source` 内嵌在 payload 且 `instance_id` 为空，td 侧按契约用 `instance_id`；
/// - 到达后按网关**整组 reconcile** 账户注册表（契约 td-config §5）。注册表**只读通用层**
///   （`account_id` / `enabled`），不读 `ext`——`ext` 是柜台特有连接参数，与唯一性无关。
///
/// payload 按契约 td-config §2.4 的最终形态消费：`{qry_*, accounts[], ext}`，与 td 侧
/// 通用层 + ext 分层一致（dzweb 不改写 payload，整体透传广播，前端自己按 interface_type
/// 取字段）。
///
/// 线程安全：依赖 dzweb 固定单线程事件循环（thread_num=1），所有访问串行执行，不加锁。
class TdConfigDomainService {
public:
    /// @param mirror 共享状态镜像（写 domain `td_config`）
    /// @param registry 账户注册表（按网关整组 reconcile）
    /// @param ws 广播薄接口（WsController 实现）
    /// @param notifier 数据变更通知（注册表集合增减时广播 `data_changed{td}`，契约
    ///        webui-ws §2.3 / rest §2.9.7「可观察效果」）
    TdConfigDomainService(MirrorStore& mirror,
                          TdAccountRegistry& registry,
                          WsBroadcaster& ws,
                          DataChangeNotifier& notifier)
        : mirror_(mirror),
          registry_(registry),
          ws_(ws),
          notifier_(notifier) {}

    /// 处理一帧 `TD_RTN_CONFIG`。
    ///
    /// @param gateway 帧头 `instance_id`（= 网关进程名，如 `dztd_ctp`）
    /// @param payload 契约 td-config §2.4 的全量 payload（脱敏）
    void on_rtn_td_config(const std::string& gateway, const nlohmann::json& payload) {
        // 镜像：始终全量覆盖该实例（契约 td-config §4「始终全量」）
        mirror_.update(gateway, "td_config", payload);

        // 注册表：先置「已收到配置帧」标志（降级判定，rest §2.9.9），再尝试 reconcile。
        // schema 异常（缺 accounts 数组或类型不符）只置标志、不 reconcile——用空集合
        // reconcile 会把该网关账户从注册表抹掉，比不更新更危险（注册表是账户配置页
        // 唯一列表来源，抹掉等于让账户在 UI 里消失）。
        registry_.note_config_frame_received();
        const auto accounts = parse_accounts(payload);
        if (!accounts.has_value()) {
            SPDLOG_WARN("td rtn config without usable accounts array, registry kept | gateway={}",
                        gateway);
        } else if (registry_.reconcile_gateway(gateway, *accounts)) {
            // 账户集合/启用状态变化 → 交易数据基线失效（前端清空重拉，契约 webui-ws §2.3）。
            notifier_.broadcast_data_changed("td");
        }

        ws_.broadcast("td_rtn_config", gateway, payload);
        SPDLOG_DEBUG("td_rtn_config pushed | gateway={}", gateway);
    }

private:
    /// 从 payload 抽取注册表用的通用层条目（契约 td-config §2.2）。
    /// `accounts` 缺失 / 非数组 → nullopt（reconcile 跳过）；元素缺 `account_id` 或类型不符
    /// → 跳过该元素（单条坏数据不该让整组不更新）；`enabled` 缺失/类型不符 → true
    /// （契约 §2.2 默认 true，不把账户误标为禁用）。
    static std::optional<std::vector<TdAccountRegistryEntry>> parse_accounts(
        const nlohmann::json& payload) {
        if (!payload.is_object()) {
            return std::nullopt;
        }
        auto it = payload.find("accounts");
        if (it == payload.end() || !it->is_array()) {
            return std::nullopt;
        }
        std::vector<TdAccountRegistryEntry> result;
        result.reserve(it->size());
        for (const auto& item : *it) {
            if (!item.is_object()) {
                continue;
            }
            auto id = item.find("account_id");
            if (id == item.end() || !id->is_string()) {
                continue;
            }
            TdAccountRegistryEntry entry;
            entry.account_id = id->get<std::string>();
            if (entry.account_id.empty()) {
                continue;
            }
            auto enabled = item.find("enabled");
            entry.enabled =
                enabled == item.end() || !enabled->is_boolean() ? true : enabled->get<bool>();
            result.push_back(std::move(entry));
        }
        return result;
    }

    MirrorStore& mirror_;
    TdAccountRegistry& registry_;
    WsBroadcaster& ws_;
    DataChangeNotifier& notifier_;
};

}  // namespace dztrader::webui

#endif  // DZTRADER_WEBUI_TD_CONFIG_DOMAIN_SERVICE_H_
