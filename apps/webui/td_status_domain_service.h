#ifndef DZTRADER_WEBUI_TD_STATUS_DOMAIN_SERVICE_H_
#define DZTRADER_WEBUI_TD_STATUS_DOMAIN_SERVICE_H_

#include "mirror_store.h"
#include "ws_broadcaster.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <string>
#include <string_view>

namespace dztrader::webui {

/// 交易网关账户级细状态领域服务：`RTN_TD_STATUS` 的镜像更新 + WS 广播（契约 td-status §3）。
///
/// 形态对齐 `MdStatusDomainService`，差异：
/// - 镜像 domain 是 `td_status`，key = 帧头 `instance_id` = **账户 ID**（如 `00001`，
///   契约 td-status §3 / 总则 §5）；一个帧只对应一个账户；
/// - 广播消息 `td_rtn_status` 的 `instance_id` 也是账户 ID（契约 webui-ws §2.2）；
/// - payload 整体透传（契约 td-status §4：接收方不做字段校验，按全量覆盖写镜像）。
///
/// 账户状态**不**写进程镜像的一级 key（那是进程），因此用账户 ID 作镜像 key，与 `progress`
/// 域对 TD 账户的 key 约定一致（契约 webui-ws §2.1 的「各进程（TD 为账户 ID）」）。
/// 账户级条目的清理不由本服务承担（key 不含进程名）：网关停止/崩溃/移除与账户从配置帧消失
/// 时由 {@link TdAccountMirrorCleanup} 按账户注册表反查清除（ADR 0056 决策 8）。
///
/// `interface_type` 派生（契约 td-status §1）：payload 不含类型字段，`instance_id` 是账户 ID
/// → 必须先经**账户注册表**解析归属网关再从网关名派生。本服务只负责镜像与透传，解析与派生
/// 由前端解析层完成，数据源是 `GET /api/td/accounts` 的 `account_id → gateway`（契约 rest
/// §2.9.9，即 td-status §1 所说的「查账户注册表」）；注册表未命中（账户已删 / 注册表降级）
/// 时前端回落配置镜像 `td_config`（唯一包含该账户的网关，契约 webui-ws §2.2），两者都未命中
/// 才按已收到的 `ext` 原样使用，不猜接口类型。
///
/// 线程安全：依赖 dzweb 固定单线程事件循环（thread_num=1），所有访问串行执行，不加锁。
class TdStatusDomainService {
public:
    TdStatusDomainService(MirrorStore& mirror, WsBroadcaster& ws)
        : mirror_(mirror),
          ws_(ws) {}

    /// 处理一帧 `TD_RTN_STATUS`。
    ///
    /// @param instance_id 帧头 `instance_id`（= 账户 ID）
    /// @param payload 契约 td-status §2.1 的 TdStatus 全量 payload
    void on_rtn_td_status(const std::string& instance_id, const nlohmann::json& payload) {
        if (instance_id.empty()) {
            // instance_id 由发送方保证（契约 td-status §4），缺失时无处挂镜像，直接丢帧。
            SPDLOG_WARN("td rtn status dropped | reason=missing_instance_id");
            return;
        }
        mirror_.update(instance_id, kDomainTdStatus, payload);
        ws_.broadcast("td_rtn_status", instance_id, payload);
        SPDLOG_DEBUG("td_rtn_status pushed | instance={}", instance_id);
    }

private:
    MirrorStore& mirror_;
    WsBroadcaster& ws_;
};

}  // namespace dztrader::webui

#endif  // DZTRADER_WEBUI_TD_STATUS_DOMAIN_SERVICE_H_
