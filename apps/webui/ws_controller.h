#ifndef DZTRADER_WEBUI_WS_CONTROLLER_H_
#define DZTRADER_WEBUI_WS_CONTROLLER_H_

#include <drogon/WebSocketController.h>
#include <dztrader/shm/reader.h>
#include <dztrader/shm/writer.h>
#include <dztrader/platform/log_config.h>
#include <unordered_map>
#include <string>
#include <memory>
#include <functional>
#include "config.h"
#include "repository.h"
#include "mirror_store.h"
#include "ws_broadcaster.h"

namespace dztrader::webui {

/// 全局广播函数指针：数据变更时由业务 controller 调用，通知所有 WS 客户端刷新
/// main.cpp 创建 WsController 后绑定此指针
using BroadcastDataChangedFn = std::function<void(const std::string& scope)>;
extern BroadcastDataChangedFn g_broadcast_data_changed;

/// 全局踢人函数指针：用户被删除/禁用时强制断开其所有 WS 连接
using KickUserFn = std::function<void(const std::string& username)>;
extern KickUserFn g_kick_user;

class WsController : public drogon::WebSocketController<WsController, false>, public WsBroadcaster {
public:
    WsController(WebuiConfig cfg,
                 std::shared_ptr<Repository> repo,
                 std::shared_ptr<shm::MultiWriter> event_writer,
                 std::shared_ptr<dztrader::platform::LogConfig> self_log,
                 MirrorStore& mirror);
    ~WsController() = default;

    // 禁止拷贝/移动（引用成员）
    WsController(const WsController&) = delete;
    WsController& operator=(const WsController&) = delete;
    WsController(WsController&&) = delete;
    WsController& operator=(WsController&&) = delete;

    WS_PATH_LIST_BEGIN
    WS_PATH_ADD("/ws");
    WS_PATH_LIST_END

    void handleNewConnection(const drogon::HttpRequestPtr&,
                             const drogon::WebSocketConnectionPtr&) override;
    void handleNewMessage(const drogon::WebSocketConnectionPtr&,
                          std::string&&,
                          const drogon::WebSocketMessageType&) override;
    void handleConnectionClosed(const drogon::WebSocketConnectionPtr&) override;

    /// 50ms 定时器回调：仅轮询订阅的日志文件 tail
    void on_timer();

    /// 数据变更广播：通知所有连接某 scope 的数据已变更，前端收到后 REST 刷新
    void broadcast_data_changed(const std::string& scope);

    /// 强制断开指定用户的所有 WS 连接（用户被删除/禁用时调用）
    void kick_user(const std::string& username);

    /// 实现 WsBroadcaster 薄接口：广播一帧 WS 消息（委托给私有 broadcast_frame）
    void broadcast(const std::string& type,
                   const std::string& instance_id,
                   const nlohmann::json& data) override {
        broadcast_frame(type, instance_id, data);
    }

private:
    WebuiConfig cfg_;
    std::shared_ptr<Repository> repo_;
    std::shared_ptr<shm::MultiWriter> event_writer_;
    /// dzweb 自身日志配置（直调源）
    std::shared_ptr<dztrader::platform::LogConfig> self_log_;

    /// 状态镜像（根 json）：instance_id -> { 领域 -> payload }
    /// 快照/增量读写均由 MirrorStore 承担（领域服务更新 + 连接快照推送）
    MirrorStore& mirror_;

    /// 单连接订阅状态
    struct Session {
        std::string user_id;
        std::string subscribed_log_file;  // empty = no subscription
        int last_log_line_count = 0;      // 上次读到的最后一行行号（1-based）；0 = 未读过
        int tail_fail_count = 0;          // 连续 send 失败次数；达到 3 自动退订
    };
    std::unordered_map<drogon::WebSocketConnectionPtr, Session> sessions_;

    /// 处理 JSON 控制消息
    void handle_control_message(const drogon::WebSocketConnectionPtr&, const nlohmann::json& msg);

    /// 轮询订阅的日志文件，推送新增行
    void poll_log_tail();

    /// 向所有已连接 WS 客户端广播一条消息（吞掉单连接异常，避免一个坏连接中断广播）
    void broadcast_to_all(const std::string& msg);

    /// 构建并广播一帧 WS 消息（instance_id 为空时不携带该字段）
    void broadcast_frame(const std::string& type,
                         const std::string& instance_id,
                         const nlohmann::json& data);
};

}  // namespace dztrader::webui

#endif  // DZTRADER_WEBUI_WS_CONTROLLER_H_
