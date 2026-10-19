#include "ws_controller.h"
#include "ws_auth.h"

#include <dztrader/shm/channel_meta.h>
#include <dztrader/shm/writer.h>
#include <dztrader/core/core_data_type.h>
#include <dztrader/platform/log_config.h>
#include <dztrader/core/path.h>
#include <dztrader/core/this_process.h>
#include <dztrader/platform/frame_codec.h>
#include <drogon/drogon.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <vector>

namespace dztrader::webui {

namespace {

/// 把 drogon 连接的 send 适配为 LogTailManager 的发送回调：
/// 抛异常视为发送失败（poll 内计入连续失败数）。
LogTailManager::SendFn make_send_fn(const drogon::WebSocketConnectionPtr& conn) {
    return [conn](const std::string& message) {
        try {
            conn->send(message);
            return true;
        } catch (...) {
            return false;
        }
    };
}

}  // namespace

WsController::WsController(WebuiConfig cfg,
                           std::shared_ptr<Repository> repo,
                           std::shared_ptr<shm::MultiWriter> event_writer,
                           std::shared_ptr<dztrader::platform::LogConfig> self_log,
                           MirrorStore& mirror,
                           MdControlDomainService& md_control)
    : cfg_(std::move(cfg)),
      repo_(std::move(repo)),
      event_writer_(std::move(event_writer)),
      self_log_(std::move(self_log)),
      mirror_(mirror),
      md_control_(md_control) {
    // webui 自身 log_config 镜像初值已移入 main 装配（mirror_store->update），
    // 保证 API-only 模式(无 SHM)下前端连接快照也能看到 webui 自身级别
    // （契约 log：dzweb 自身纳入镜像）。
    // SHM 事件监听由 main 直调 EventMonitor（本类不再涉及）。
}

// ~WsController() 已改为类内 `= default`（头文件声明处）：
// 析构为空体，SHM 事件监听由 main 持有的 EventMonitor 负责（start/stop 均由 main 直调），
// EventMonitor 析构时自调 stop()，与 drogon 单例析构顺序无依赖。

void WsController::handleNewConnection(const drogon::HttpRequestPtr& req,
                                       const drogon::WebSocketConnectionPtr& conn) {
    // 从 query 参数或 header 提取 token 并校验（契约 webui-ws §1）
    const auto user_id_opt = ws_authenticate(req->getParameter("token"),
                                             req->getHeader("authorization"), cfg_.jwt_secret);
    if (!user_id_opt.has_value()) {
        const nlohmann::json err = {{"type", "error"}, {"payload", {{"message", "auth failed"}}}};
        conn->send(err.dump());
        conn->shutdown();
        return;
    }
    const std::string user_id = *user_id_opt;

    // 连接时缓存角色（契约 webui-ws §3 修订：md_connect/md_disconnect 与 REST 一致要求 admin）
    bool is_admin = false;
    if (repo_) {
        try {
            auto user = repo_->get_user_by_username(user_id);
            is_admin = user.has_value() && user->role == "admin";
        } catch (...) {
            // 吞因：角色查询失败按非管理员处理（保守拒绝管理操作，不影响只读订阅）
            SPDLOG_DEBUG("role lookup failed, treat as non-admin | user={}", user_id);
        }
    }
    sessions_[conn] = Session{.user_id = user_id, .is_admin = is_admin};
    SPDLOG_INFO("ws connected | user={}", user_id);

    // 连接即推全量镜像快照: 前端以此构建初始状态, 之后按增量帧更新
    // 快照来源为共享 MirrorStore（领域服务写入），Task 8 不再保留独立 mirror_
    // 契约 webui-ws §1: snapshot 按连接定向发送（不广播给其他连接，避免误清他人 pending）
    const nlohmann::json snapshot = mirror_.snapshot();
    const nlohmann::json snap_msg = {{"type", "snapshot"}, {"data", snapshot}};
    conn->send(snap_msg.dump());
    SPDLOG_DEBUG("mirror snapshot pushed | instances={}", snapshot.size());

    // 用户上线：更新状态为 online 并通知所有设备刷新
    if (repo_) {
        try {
            auto user = repo_->get_user_by_username(user_id);
            if (user && user->status != "disabled" && user->status != "locked") {
                repo_->update_user_status(user->id, "online");
            }
        } catch (...) {
            // 吞因：上线状态写 DB 失败不阻断 WS 连接建立（尽力而为）
            SPDLOG_DEBUG("update online status failed, ignored | user={}", user_id);
        }
    }
    broadcast_data_changed("users");

    // 默认密码告警:仅 admin 用户、当前是默认密码、且未确认时推送
    if (cfg_.admin_password_is_default && repo_) {
        try {
            auto admin_user = repo_->get_user_by_username(user_id);
            if (admin_user && admin_user->role == "admin" &&
                admin_user->default_password_acknowledged == 0) {
                const nlohmann::json warning = {
                    {"type", "default_password_warning"},
                    {"payload",
                     {{"message",
                       "系统正在使用默认管理员密码 88888888,存在安全风险,请尽快修改密码"}}}};
                conn->send(warning.dump());
            }
        } catch (...) {
            // 吞因：默认密码告警推送/DB 读取失败不阻断连接（尽力而为）
            SPDLOG_DEBUG("default password warning failed, ignored | user={}", user_id);
        }
    }
}

void WsController::handleNewMessage(const drogon::WebSocketConnectionPtr& conn,
                                    std::string&& message,
                                    const drogon::WebSocketMessageType&) {
    auto it = sessions_.find(conn);
    if (it == sessions_.end()) {
        return;
    }

    try {
        auto msg = nlohmann::json::parse(message);
        handle_control_message(conn, msg);
    } catch (const std::exception& e) {
        const nlohmann::json err = {
            {"type", "error"}, {"payload", {{"message", std::string("bad json: ") + e.what()}}}};
        conn->send(err.dump());
    }
}

void WsController::handleConnectionClosed(const drogon::WebSocketConnectionPtr& conn) {
    auto it = sessions_.find(conn);
    std::string username;
    if (it != sessions_.end()) {
        username = it->second.user_id;
    }
    sessions_.erase(conn);
    log_tail_.drop(conn.get());
    stop_log_tail_timer_if_idle();  // 无订阅时关闭轮询
    SPDLOG_INFO("ws closed");

    // 用户下线：检查是否还有同用户的其他 WS 连接，无则设 offline 并通知所有设备刷新
    if (username.empty()) {
        return;
    }
    bool still_online = false;
    for (const auto& [c, s] : sessions_) {
        if (s.user_id == username) {
            still_online = true;
            break;
        }
    }
    if (!still_online && repo_) {
        try {
            auto user = repo_->get_user_by_username(username);
            if (user && user->status != "disabled" && user->status != "locked") {
                repo_->update_user_status(user->id, "offline");
            }
        } catch (...) {
            // 吞因：下线状态写 DB 失败不阻断连接关闭流程（尽力而为）
            SPDLOG_DEBUG("update offline status failed, ignored | user={}", username);
        }
        broadcast_data_changed("users");
    }
}

void WsController::handle_control_message(const drogon::WebSocketConnectionPtr& conn,
                                          const nlohmann::json& msg) {
    const std::string type = msg.value("type", "");
    const auto& payload = msg.value("payload", nlohmann::json::object());

    if (type == "ping") {
        conn->send(R"({"type":"pong"})");
        return;
    } else if (type == "md_connect" || type == "md_disconnect") {
        // P2 任务③：出方向 C2S 下沉到 MdControlDomainService（守卫 + 写帧 + 回执）。
        // 仅这里做连接级 admin 判定（Session 缓存）与定向回执注入，保持轻量。
        const auto sit = sessions_.find(conn);
        const bool is_admin = sit != sessions_.end() && sit->second.is_admin;
        const std::string source = payload.value("source", "");
        const uint64_t seq = msg.value("seq", static_cast<uint64_t>(0));
        // 定向回执：把服务返回的完整消息帧发给发起连接
        MdControlDomainService::Reply reply = [conn](nlohmann::json frame) {
            conn->send(frame.dump());
        };
        if (type == "md_connect") {
            md_control_.handle_md_connect(is_admin, source, seq, reply);
        } else {
            md_control_.handle_md_disconnect(is_admin, source, seq, reply);
        }
    } else if (type == "query_md_subscriptions") {
        // P2 任务③：下沉到 MdControlDomainService（透传，dzweb 不校验互斥）
        const uint64_t seq = msg.value("seq", static_cast<uint64_t>(0));
        MdControlDomainService::Reply reply = [conn](nlohmann::json frame) {
            conn->send(frame.dump());
        };
        md_control_.handle_query_md_subscriptions(payload.value("source", std::string{}), seq,
                                                  payload, reply);
    } else if (type == "subscribe_log") {
        const std::string file = payload.value("file", "");
        const uint64_t seq = msg.value("seq", static_cast<uint64_t>(0));
        // 校验/基线/ack/error 均由 LogTailManager 统一处理（可单测）
        if (log_tail_.handle_subscribe(conn.get(), file, seq, dztrader::this_process::exe_stem(),
                                       dztrader::paths::logs(), make_send_fn(conn))) {
            start_log_tail_timer();  // 惰性启动 tail 轮询（幂等）
        }
    } else if (type == "unsubscribe_log") {
        const uint64_t seq = msg.value("seq", static_cast<uint64_t>(0));
        log_tail_.handle_unsubscribe(conn.get(), seq, make_send_fn(conn));
        stop_log_tail_timer_if_idle();  // 无订阅时关闭轮询
    } else {
        const nlohmann::json err = {{"type", "error"},
                                    {"seq", msg.value("seq", 0)},
                                    {"payload", {{"message", "unknown type: " + type}}}};
        conn->send(err.dump());
    }
}

void WsController::broadcast_data_changed(const std::string& scope) {
    const nlohmann::json msg = {{"type", "data_changed"}, {"payload", {{"scope", scope}}}};
    broadcast_to_all(msg.dump());
}

void WsController::broadcast_to_all(const std::string& msg) {
    for (const auto& [conn, sess] : sessions_) {
        try {
            conn->send(msg);
        } catch (const std::exception& e) {
            // best-effort 广播: 单个连接失败不阻断其他连接, 但需 WARN 日志便于诊断 WS 推送丢失
            SPDLOG_WARN("broadcast send failed | user={} error=\"{}\"", sess.user_id, e.what());
        } catch (...) {
            SPDLOG_WARN("broadcast send failed | user={} reason=unknown_exception", sess.user_id);
        }
    }
}

void WsController::broadcast_frame(const std::string& type,
                                   const std::string& instance_id,
                                   const nlohmann::json& data) {
    nlohmann::json msg = {{"type", type}, {"data", data}};
    if (!instance_id.empty()) {
        msg["instance_id"] = instance_id;
    }
    broadcast_to_all(msg.dump());
}

void WsController::kick_user(const std::string& username) {
    if (username.empty()) {
        return;
    }
    // 先收集目标连接再统一关闭：若 shutdown 同步触发 close 回调（erase sessions_），
    // 边遍历边关闭会导致 unordered_map 迭代器失效
    std::vector<drogon::WebSocketConnectionPtr> targets;
    for (const auto& [conn, sess] : sessions_) {
        if (sess.user_id == username) {
            targets.push_back(conn);
        }
    }
    for (const auto& conn : targets) {
        try {
            conn->shutdown();
        } catch (...) {
            // 吞因：单连接关闭失败不影响踢除其他连接（尽力而为）
            SPDLOG_DEBUG("kick connection shutdown failed, ignored | user={}", username);
        }
    }
    SPDLOG_INFO("ws kicked | user={} connections={}", username, targets.size());
}

void WsController::start_log_tail_timer() {
    if (log_tail_timer_active_) {
        return;
    }
    // 惰性获取连接所在的 IO 循环并缓存：WsController 构造早于 drogon::app().run()
    // （ioLoopThreadPool_ 未创建，getIOLoop(0) 返回 nullptr），而首次 subscribe_log
    // 必在连接建立后发生，此时 run() 已调用、IO 循环就绪。注意 app().getLoop() 是
    // 独立主循环（不参与连接分发），定时器必须注册到 IO 循环才能与 WS 回调同线程。
    if (io_loop_ == nullptr) {
        io_loop_ = drogon::app().getIOLoop(0);
    }
    log_tail_timer_active_ = true;
    // 惰性启动：首个日志订阅建立时才开启 500ms 轮询（无订阅时零定时器唤醒）
    // 定时器注册到连接所在的 IO 循环，on_timer/poll_log_tail 与 WS 回调同线程串行，
    // sessions_ 与 log_tail_timer_active_ 无跨线程竞争（消除 check-then-act 误杀定时器）
    log_tail_timer_ = io_loop_->runEvery(kLogTailIntervalSec, [this]() { on_timer(); });
}

void WsController::stop_log_tail_timer_if_idle() {
    if (!log_tail_timer_active_) {
        return;
    }
    if (log_tail_.has_subscriptions()) {
        return;  // 仍有订阅，保持轮询
    }
    io_loop_->invalidateTimer(log_tail_timer_);
    log_tail_timer_active_ = false;
}

void WsController::on_timer() {
    // 仅 poll_log_tail（event channel 读取已移到 main 持有的 EventMonitor）
    try {
        poll_log_tail();
    } catch (const std::exception& e) {
        SPDLOG_WARN("on_timer failed | error={}", e.what());
    }
}

void WsController::poll_log_tail() {
    // 读文件/推送/游标回退/连续失败自动退订由 LogTailManager 承担（可单测）
    log_tail_.poll(dztrader::paths::logs());
    // 连续 send 失败自动退订后可能已无订阅，及时关闭轮询
    stop_log_tail_timer_if_idle();
}

}  // namespace dztrader::webui
