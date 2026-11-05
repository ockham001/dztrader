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
/// - 镜像 domain 是 `td_status`，key = 帧头 `instance_id` = **网关名:账户ID**
///   （如 `dztd_ctp:account_001`，契约 td-status §3）；一个帧只对应一个账户；
/// - 广播消息 `td_rtn_status` 的 `instance_id` 也是该复合键（契约 webui-ws §2.2）；
/// - payload 整体透传（契约 td-status §4：接收方不做字段校验，按全量覆盖写镜像）。
///
/// 账户状态**不**写进程镜像的一级 key（那是进程），因此用复合 instance_id 作为镜像 key，
/// 与 `progress` 域对 TD 账户的 key 约定一致（契约 webui-ws §2.1 的「各进程（TD 为账户
/// ID）」）。
///
/// 线程安全：依赖 dzweb 固定单线程事件循环（thread_num=1），所有访问串行执行，不加锁。
class TdStatusDomainService {
public:
    TdStatusDomainService(MirrorStore& mirror, WsBroadcaster& ws)
        : mirror_(mirror),
          ws_(ws) {}

    /// 处理一帧 `TD_RTN_STATUS`。
    ///
    /// @param instance_id 帧头 `instance_id`（= 网关名:账户ID）
    /// @param payload 契约 td-status §2.1 的 TdStatus 全量 payload
    void on_rtn_td_status(const std::string& instance_id, const nlohmann::json& payload) {
        if (instance_id.empty()) {
            // instance_id 由发送方保证（契约 td-status §4），缺失时无处挂镜像，直接丢帧。
            SPDLOG_WARN("td rtn status dropped | reason=missing_instance_id");
            return;
        }
        mirror_.update(instance_id, "td_status", payload);
        ws_.broadcast("td_rtn_status", instance_id, payload);
        SPDLOG_DEBUG("td_rtn_status pushed | instance={}", instance_id);
    }

private:
    MirrorStore& mirror_;
    WsBroadcaster& ws_;
};

}  // namespace dztrader::webui

#endif  // DZTRADER_WEBUI_TD_STATUS_DOMAIN_SERVICE_H_
