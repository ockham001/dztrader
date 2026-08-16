#include "ws_controller.h"
#include "jwt.h"
#include "log_service.h"

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

WsController::WsController(WebuiConfig cfg,
                           std::shared_ptr<Repository> repo,
                           std::shared_ptr<shm::MultiWriter> event_writer,
                           std::shared_ptr<dztrader::platform::LogConfig> self_log,
                           MirrorStore& mirror,
                           std::shared_ptr<ProcessMirror> process_mirror)
    : cfg_(std::move(cfg)),
      repo_(std::move(repo)),
      event_writer_(std::move(event_writer)),
      self_log_(std::move(self_log)),
      mirror_(mirror),
      process_mirror_(std::move(process_mirror)) {
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
    // 从 query 参数或 header 提取 token 并校验
    std::string token = req->getParameter("token");
    if (token.empty()) {
        auto auth = req->getHeader("authorization");
        if (auth.size() >= 7 && auth.starts_with("Bearer ")) {
            token = auth.substr(7);
        }
    }

    std::string user_id;
    if (!jwt_verify(token, cfg_.jwt_secret, user_id)) {
        const nlohmann::json err = {{"type", "error"}, {"payload", {{"message", "auth failed"}}}};
        conn->send(err.dump());
        conn->shutdown();
        return;
    }

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
    sessions_[conn] = Session{.user_id = user_id, .subscribed_log_file = "", .is_admin = is_admin};
    SPDLOG_INFO("ws connected | user={}", user_id);

    // 连接即推全量镜像快照: 前端以此构建初始状态, 之后按增量帧更新
    // 快照来源为共享 MirrorStore（领域服务写入），Task 8 不再保留独立 mirror_
    // 契约 webui-ws §1: snapshot 按连接定向发送（不广播给其他连接，避免误清他人 pending）
    const nlohmann::json snap_msg = {{"type", "snapshot"}, {"data", mirror_.snapshot()}};
    conn->send(snap_msg.dump());
    SPDLOG_DEBUG("mirror snapshot pushed | instances={}", mirror_.snapshot().size());

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
    if (g_broadcast_data_changed) {
        g_broadcast_data_changed("users");
    }

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
        if (g_broadcast_data_changed) {
            g_broadcast_data_changed("users");
        }
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
        // 契约 webui-ws §3（修订）: 与 REST /login /logout 一致--
        // admin 角色 + source 非空 + 事件通道可用 + 目标进程镜像 Running，否则回 error 不写帧
        const auto sit = sessions_.find(conn);
        const bool is_admin = sit != sessions_.end() && sit->second.is_admin;
        if (!is_admin) {
            const nlohmann::json err = {
                {"type", "error"},
                {"seq", msg.value("seq", 0)},
                {"payload", {{"message", "admin required"}}}};
            conn->send(err.dump());
            return;
        }
        const std::string source = payload.value("source", "");
        if (source.empty() || !event_writer_) {
            const nlohmann::json err = {
                {"type", "error"},
                {"seq", msg.value("seq", 0)},
                {"payload", {{"message", "invalid source or writer not ready"}}}};
            conn->send(err.dump());
            return;
        }
        bool running = false;
        if (process_mirror_) {
            auto st = process_mirror_->get_status(source);
            running = st.has_value() && st->state == platform::ChildState::Running;
        }
        if (!running) {
            const nlohmann::json err = {
                {"type", "error"},
                {"seq", msg.value("seq", 0)},
                {"payload", {{"message", "process not running: " + source}}}};
            conn->send(err.dump());
            return;
        }
        // ok 反映真实写入结果（契约 webui-ws §2.4: ok=false 时前端立即提示、不设 pending）
        const bool ok = platform::write_ext_inst_raw(
            *event_writer_,
            type == "md_connect" ? DZ_FRAME_REQUEST_MD_CONNECT : DZ_FRAME_REQUEST_MD_DISCONNECT,
            source);
        const nlohmann::json ack = {
            {"type", type == "md_connect" ? "md_connect_ack" : "md_disconnect_ack"},
            {"seq", msg.value("seq", 0)},
            {"payload", {{"source", source}, {"ok", ok}}}};
        conn->send(ack.dump());
    } else if (type == "query_md_subscriptions") {
        // 写 DZ_FRAME_QUERY_MD_SUBSCRIPTIONS 到 event channel (透传给目标 md 进程)
        // 支持两种模式 (互斥):
        //   {"query": "unsuccessful"}                 - 查未成功 (Pending + NotRequested)
        //   {"instruments": ["IF2506", "IC2506"]}     - 按合约列表查
        // dzweb 不解析业务字段, 仅提取 source 路由, 其余透传给 md
        std::string source = payload.value("source", "");
        if (source.empty() || !event_writer_) {
            const nlohmann::json err = {
                {"type", "error"},
                {"seq", msg.value("seq", 0)},
                {"payload", {{"message", "invalid source or writer not ready"}}}};
            conn->send(err.dump());
            return;
        }
        // 构造 SHM payload: 仅含 query 或 instruments (不含 source, source 作为 ext_inst_id)
        nlohmann::json req_payload;
        if (payload.contains("query") && payload["query"].is_string()) {
            req_payload["query"] = payload["query"];
        } else if (payload.contains("instruments") && payload["instruments"].is_array()) {
            req_payload["instruments"] = payload["instruments"];
        } else {
            const nlohmann::json err = {{"type", "error"},
                                        {"seq", msg.value("seq", 0)},
                                        {"payload", {{"message", "missing query or instruments"}}}};
            conn->send(err.dump());
            return;
        }
        // write 返回真实写入结果（契约 webui-ws §2.4: ok=false 时前端立即提示、不设 pending）
        const bool ok = platform::write_ext_inst_json_obj(
            *event_writer_, DZ_FRAME_QUERY_MD_SUBSCRIPTIONS, source, req_payload);
        const nlohmann::json ack = {{"type", "query_md_subscriptions_ack"},
                                    {"seq", msg.value("seq", 0)},
                                    {"payload", {{"source", source}, {"ok", ok}}}};
        conn->send(ack.dump());
    } else if (type == "subscribe_log") {
        std::string file = payload.value("file", "");
        if (file.empty()) {
            const nlohmann::json err = {{"type", "error"},
                                        {"seq", msg.value("seq", 0)},
                                        {"payload", {{"message", "missing file parameter"}}}};
            conn->send(err.dump());
            return;
        }
        // 拒绝订阅自身日志 (避免自引用死循环)
        // 通过 exe_stem() 动态获取当前后端进程名，支持重命名 dzweb.exe 部署
        // 从路径中提取 basename (兼容 subdir/name 和 flat name 两种格式)
        std::string basename = file;
        {
            auto pos = file.find_last_of("/\\");
            if (pos != std::string::npos) {
                basename = file.substr(pos + 1);
            }
        }
        const std::string& self_process = dztrader::this_process::exe_stem();
        if (LogService::extract_logger(basename) == self_process) {
            SPDLOG_DEBUG("self log tail rejected | file={} process={}", file, self_process);
            const nlohmann::json rej = {
                {"type", "log_tail_unsubscribed"},
                {"seq", msg.value("seq", 0)},
                {"payload", {{"file", file}, {"reason", "self_logs_cannot_be_tailed"}}}};
            conn->send(rej.dump());
            return;
        }
        sessions_[conn].subscribed_log_file = file;
        sessions_[conn].tail_fail_count = 0;  // 重订阅重置失败计数（跨订阅残留会使新订阅的 auto-unsubscribe 阈值被预支）
        // 读取当前行数作为基线
        try {
            dztrader::webui::LogService svc(dztrader::paths::logs());
            auto content = svc.read_content(file, 0, 0, "", "", "", "");
            sessions_[conn].last_log_line_count = static_cast<int>(content.lines.size());
        } catch (...) {
            // 吞因：基线读取失败则保持 last_log_line_count=0，从文件头开始 tail，订阅本身继续
            SPDLOG_DEBUG("log tail baseline read failed, ignored | file={}", file);
        }
        const nlohmann::json ack = {{"type", "subscribe_log_ack"},
                                    {"seq", msg.value("seq", 0)},
                                    {"payload", {{"file", file}}}};
        conn->send(ack.dump());
    } else if (type == "unsubscribe_log") {
        sessions_[conn].subscribed_log_file.clear();
        sessions_[conn].last_log_line_count = 0;
        sessions_[conn].tail_fail_count = 0;  // 退订清零，下次订阅从满额阈值开始
        const nlohmann::json ack = {{"type", "unsubscribe_log_ack"}, {"seq", msg.value("seq", 0)}};
        conn->send(ack.dump());
    } else {
        const nlohmann::json err = {{"type", "error"},
                                    {"seq", msg.value("seq", 0)},
                                    {"payload", {{"message", "unknown type: " + type}}}};
        conn->send(err.dump());
    }
}

/// 全局广播函数指针定义
BroadcastDataChangedFn g_broadcast_data_changed;

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

/// 全局踢人函数指针定义
KickUserFn g_kick_user;

void WsController::kick_user(const std::string& username) {
    if (username.empty()) {
        return;
    }
    for (const auto& [conn, sess] : sessions_) {
        if (sess.user_id == username) {
            try {
                conn->shutdown();
            } catch (...) {
                // 吞因：单连接关闭失败不影响踢除其他连接（尽力而为）
                SPDLOG_DEBUG("kick connection shutdown failed, ignored | user={}", username);
            }
        }
    }
    SPDLOG_INFO("ws kicked | user={}", username);
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
    // 收集所有有日志订阅的连接（拷贝，避免循环中修改 sessions_ 导致迭代器失效）
    std::vector<drogon::WebSocketConnectionPtr> conns_with_log;
    for (const auto& [conn, sess] : sessions_) {
        if (!sess.subscribed_log_file.empty()) {
            conns_with_log.push_back(conn);
        }
    }
    if (conns_with_log.empty()) {
        return;
    }

    try {
        dztrader::webui::LogService svc(dztrader::paths::logs());
        constexpr int k_tail_limit = 200;  // 单次推送上限，防止 burst 洪水

        for (const auto& conn : conns_with_log) {
            auto it = sessions_.find(conn);
            if (it == sessions_.end()) {
                continue;
            }

            std::string file = it->second.subscribed_log_file;  // 拷贝，避免引用失效
            if (file.empty()) {
                continue;  // 可能被前一轮 auto-unsubscribe 清空
            }
            const int from_line = it->second.last_log_line_count;

            // 偏移量读取：只读 from_line 之后的新增行，不全量重读
            auto content = svc.read_tail(file, from_line, k_tail_limit);

            int pushed = 0;
            for (const auto& l : content.lines) {
                const nlohmann::json line_msg = {{"type", "log_line"},
                                                 {"payload",
                                                  {{"file", file},
                                                   {"line",
                                                    {{"n", l.n},
                                                     {"ts", l.ts},
                                                     {"level", l.level},
                                                     {"logger", l.logger},
                                                     {"func", l.func},
                                                     {"file", l.file},
                                                     {"line", l.line_no},
                                                     {"pid", l.pid},
                                                     {"tid", l.tid},
                                                     {"msg", l.msg},
                                                     {"raw", l.raw},
                                                     {"parsed", l.parsed}}}}}};
                try {
                    conn->send(line_msg.dump());
                } catch (...) {
                    it->second.tail_fail_count++;
                    break;
                }
                ++pushed;
            }

            // 更新 baseline
            if (pushed == static_cast<int>(content.lines.size())) {
                // 全部推送成功（或无新行）→ 用 read_tail 返回的 total
                it->second.last_log_line_count = content.total;
                if (pushed > 0) {
                    it->second.tail_fail_count = 0;  // 成功，重置失败计数
                }
            } else {
                // 中途 send 失败 → baseline 只推进到已推送的位置，下次从这继续
                it->second.last_log_line_count = from_line + pushed;
            }

            // 连续失败 3 次自动退订，防止僵尸连接每 50ms 无效 send
            if (it->second.tail_fail_count >= 3) {
                SPDLOG_WARN("auto-unsubscribe log tail | user={} file={} fails={}",
                            it->second.user_id, file, it->second.tail_fail_count);
                it->second.subscribed_log_file.clear();
                it->second.last_log_line_count = 0;
                it->second.tail_fail_count = 0;
                try {
                    const nlohmann::json err = {
                        {"type", "log_tail_unsubscribed"},
                        {"payload", {{"file", file}, {"reason", "send failures"}}}};
                    conn->send(err.dump());
                } catch (...) {
                    // 吞因：连接已坏（连续 send 失败），退订通知本身也是尽力而为
                    SPDLOG_DEBUG("log_tail_unsubscribed send failed, ignored | file={}", file);
                }
            }
        }
    } catch (const std::exception& e) {
        SPDLOG_WARN("poll_log_tail failed | error={}", e.what());
    }
}

}  // namespace dztrader::webui
