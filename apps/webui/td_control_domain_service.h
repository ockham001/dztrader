#ifndef DZTRADER_WEBUI_TD_CONTROL_DOMAIN_SERVICE_H_
#define DZTRADER_WEBUI_TD_CONTROL_DOMAIN_SERVICE_H_

#include <cstdint>
#include <functional>
#include <string>

#include <nlohmann/json.hpp>

#include "process_mirror.h"
#include "shm_writer.h"
#include "td_account_registry.h"
#include "td_session_guard.h"

namespace dztrader::webui {

/// 守卫失败 → WS error 文案（契约 webui-ws §3：message 说明原因，前端不解析 code）。
inline std::string td_session_guard_message(const TdSessionCheck& check,
                                            const std::string& account_id) {
    switch (check.result) {
        case TdSessionGuard::NotAdmin:
            return "admin required";
        case TdSessionGuard::AccountInvalid:
            return "account not found: " + account_id;
        case TdSessionGuard::RegistryUnavailable:
            return "account registry unavailable";
        case TdSessionGuard::GatewayNotRunning:
            return "td gateway not running: " + check.gateway;
        case TdSessionGuard::ChannelUnavailable:
            return "shm writer not available";
        case TdSessionGuard::Ok:
            break;
    }
    return "unknown guard failure";
}

/// 交易账户会话控制领域服务（出方向 C2S）：td_connect / td_disconnect（契约 td-connection）。
///
/// 与 MdControlDomainService 同型：把守卫 + 写帧 + 回执从 WsController 下沉到领域服务，
/// WsController 只保留连接/鉴权/会话/路由。差异是账户级寻址：payload `account_id` 经注册表
/// 解析出网关，帧 `instance_id` = 网关进程名、payload = `{"account_id": …}`。
///
/// 无 drogon 依赖：回执经 reply(std::function) 定向注入；可脱离框架单测。
/// 线程安全：依赖 dzweb 固定单线程事件循环（thread_num=1），方法总在 IO 线程串行执行。
class TdControlDomainService {
public:
    /// 定向回执回调：注入一帧完整 WS 消息（含 type/seq/payload）
    using Reply = std::function<void(nlohmann::json)>;

    /// @param registry 账户注册表（account_id → gateway 解析）
    /// @param process_mirror 进程镜像（目标网关 Running 校验）
    /// @param writer 事件通道写入（nullptr = API-only 模式，守卫回 ChannelUnavailable）
    TdControlDomainService(TdAccountRegistry& registry,
                           ProcessMirror& process_mirror,
                           ShmWriter* writer)
        : registry_(registry),
          process_mirror_(process_mirror),
          writer_(writer) {}

    /// C2S td_connect：守卫后写 DZ_FRAME_TD_CONNECT；回 td_connect_ack{account_id, ok}
    /// （契约 webui-ws §3/§2.4、契约 td-connection）
    void handle_td_connect(bool is_admin,
                           const std::string& account_id,
                           uint64_t seq,
                           const Reply& reply) {
        handle_session_action(is_admin, account_id, seq, /*is_connect=*/true, reply);
    }

    /// C2S td_disconnect：同守卫后写 DZ_FRAME_TD_DISCONNECT；回 td_disconnect_ack
    void handle_td_disconnect(bool is_admin,
                              const std::string& account_id,
                              uint64_t seq,
                              const Reply& reply) {
        handle_session_action(is_admin, account_id, seq, /*is_connect=*/false, reply);
    }

private:
    void handle_session_action(bool is_admin,
                               const std::string& account_id,
                               uint64_t seq,
                               bool is_connect,
                               const Reply& reply) {
        const bool writer_ready = writer_ != nullptr && writer_->is_ready();
        const auto check = evaluate_td_session_guard(is_admin, &registry_, &process_mirror_,
                                                     writer_ready, account_id);
        if (check.result != TdSessionGuard::Ok) {
            reply(nlohmann::json{
                {"type", "error"},
                {"seq", seq},
                {"payload", {{"message", td_session_guard_message(check, account_id)}}}});
            return;
        }
        // ok 反映真实写入结果（契约 webui-ws §2.4：ok=false 时前端立即提示、不设 pending）
        const bool ok = is_connect ? writer_->write_td_connect(check.gateway, account_id)
                                   : writer_->write_td_disconnect(check.gateway, account_id);
        reply(nlohmann::json{{"type", is_connect ? "td_connect_ack" : "td_disconnect_ack"},
                             {"seq", seq},
                             {"payload", {{"account_id", account_id}, {"ok", ok}}}});
    }

    TdAccountRegistry& registry_;
    ProcessMirror& process_mirror_;
    ShmWriter* writer_;
};

}  // namespace dztrader::webui

#endif  // DZTRADER_WEBUI_TD_CONTROL_DOMAIN_SERVICE_H_
