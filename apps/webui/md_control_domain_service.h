#ifndef DZTRADER_WEBUI_MD_CONTROL_DOMAIN_SERVICE_H_
#define DZTRADER_WEBUI_MD_CONTROL_DOMAIN_SERVICE_H_

#include <dztrader/shm/writer.h>
#include <dztrader/platform/frame_codec.h>
#include <dztrader/core/core_data_type.h>
#include <functional>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "process_mirror.h"
#include "ws_control_precheck.h"

namespace dztrader::webui {

/// 行情控制领域服务（出方向 C2S）：md_connect / md_disconnect / query_md_subscriptions。
/// P2 任务③：把 WsController::handle_control_message 的三个写帧分支下沉到领域服务，
/// 出方向与入方向（RTN 进领域服务）对称，WsController 只保留连接/鉴权/会话。
/// 无 drogon 依赖：回执经 reply(std::function<void(nlohmann::json)>) 定向回调注入，
/// WsController 分发时传入当前连接的定向发送闭包；可脱离框架单测。
///
/// 线程安全：依赖 dzweb 固定单线程事件循环（thread_num=1），服务方法总在 IO 线程
/// （WS 连接回调同线程）串行执行，不加锁。
class MdControlDomainService {
public:
    /// 定向回执回调：注入一帧完整 WS 消息（含 type/seq/payload），由 WsController 发往发起连接
    using Reply = std::function<void(nlohmann::json)>;

    MdControlDomainService(ProcessMirror& process_mirror, shm::MultiWriter* event_writer)
        : process_mirror_(process_mirror), event_writer_(event_writer) {}

    /// C2S md_connect：admin + source 非空 + 事件通道可用 + 目标进程镜像 Running 守卫后
    /// 写 DZ_FRAME_REQUEST_MD_CONNECT；回 md_connect_ack{source, ok}（契约 webui-ws §3/§2.4）
    void handle_md_connect(bool is_admin,
                           const std::string& source,
                           uint64_t seq,
                           const Reply& reply) {
        handle_connect_disconnect(is_admin, source, seq, /*is_connect=*/true, reply);
    }

    /// C2S md_disconnect：同守卫后写 DZ_FRAME_REQUEST_MD_DISCONNECT；回 md_disconnect_ack
    void handle_md_disconnect(bool is_admin,
                              const std::string& source,
                              uint64_t seq,
                              const Reply& reply) {
        handle_connect_disconnect(is_admin, source, seq, /*is_connect=*/false, reply);
    }

    /// C2S query_md_subscriptions：source 非空 + writer 可用 + payload 含 query/instruments
    /// 后写 DZ_FRAME_QUERY_MD_SUBSCRIPTIONS（dzweb 不校验互斥，由目标 md 进程裁决）；
    /// 回 query_md_subscriptions_ack（契约 md-subscription）。这类只读查询不做 admin 限制
    /// （与原实现一致，见原 handle_control_message）。
    void handle_query_md_subscriptions(const std::string& source,
                                       uint64_t seq,
                                       const nlohmann::json& payload,
                                       const Reply& reply) {
        if (source.empty() || !event_writer_) {
            reply(nlohmann::json{{"type", "error"},
                                 {"seq", seq},
                                 {"payload", {{"message", "invalid source or writer not ready"}}}});
            return;
        }
        // 构造 SHM payload（不含 source, source 作为 ext_inst_id）
        nlohmann::json req_payload;
        if (!build_subscription_query_payload(payload, req_payload)) {
            reply(nlohmann::json{{"type", "error"},
                                 {"seq", seq},
                                 {"payload", {{"message", "missing query or instruments"}}}});
            return;
        }
        // write 返回真实写入结果（契约 webui-ws §2.4: ok=false 时前端立即提示、不设 pending）
        const bool ok = platform::write_ext_inst_json_obj(
            *event_writer_, DZ_FRAME_QUERY_MD_SUBSCRIPTIONS, source, req_payload);
        reply(nlohmann::json{{"type", "query_md_subscriptions_ack"},
                             {"seq", seq},
                             {"payload", {{"source", source}, {"ok", ok}}}});
    }

private:
    /// md_connect/md_disconnect 共用的守卫 + 写帧 + 回执（契约 webui-ws §3 修订：
    /// 与 REST /login /logout 一致——admin + source 非空 + 事件通道可用 + 目标进程镜像 Running）
    void handle_connect_disconnect(bool is_admin,
                                   const std::string& source,
                                   uint64_t seq,
                                   bool is_connect,
                                   const Reply& reply) {
        std::optional<platform::ChildState> state;
        if (auto st = process_mirror_.get_status(source)) {
            state = st->state;
        }
        const auto pre =
            evaluate_md_connect_precheck(is_admin, source, event_writer_ != nullptr, state);
        if (pre != MdConnectPrecheck::Ok) {
            reply(nlohmann::json{{"type", "error"},
                                 {"seq", seq},
                                 {"payload", {{"message", md_connect_precheck_message(pre, source)}}}});
            return;
        }
        // ok 反映真实写入结果（契约 webui-ws §2.4: ok=false 时前端立即提示、不设 pending）
        const bool ok = platform::write_ext_inst_raw(
            *event_writer_,
            is_connect ? DZ_FRAME_REQUEST_MD_CONNECT : DZ_FRAME_REQUEST_MD_DISCONNECT,
            source);
        reply(nlohmann::json{{"type", is_connect ? "md_connect_ack" : "md_disconnect_ack"},
                             {"seq", seq},
                             {"payload", {{"source", source}, {"ok", ok}}}});
    }

    ProcessMirror& process_mirror_;
    shm::MultiWriter* event_writer_;
};

}  // namespace dztrader::webui

#endif  // DZTRADER_WEBUI_MD_CONTROL_DOMAIN_SERVICE_H_