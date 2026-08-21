#ifndef DZTRADER_WEBUI_WS_CONTROLLER_H_
#define DZTRADER_WEBUI_WS_CONTROLLER_H_

#include <drogon/WebSocketController.h>
#include <dztrader/shm/reader.h>
#include <dztrader/shm/writer.h>
#include <dztrader/platform/log_config.h>
#include <unordered_map>
#include <string>
#include <memory>
#include "config.h"
#include "log_service.h"
#include "repository.h"
#include "mirror_store.h"
#include "process_mirror.h"
#include "ws_broadcaster.h"
#include "data_change_notifier.h"
#include "md_control_domain_service.h"

namespace dztrader::webui {

/// 提供数据变更通知/踢人能力（实现 DataChangeNotifier 薄接口）：
/// 各业务 controller 构造时注入自身引用（取代原全局函数指针 g_broadcast_data_changed / g_kick_user）
class WsController : public drogon::WebSocketController<WsController, false>,
                     public WsBroadcaster,
                     public DataChangeNotifier {
public:
    WsController(WebuiConfig cfg,
                 std::shared_ptr<Repository> repo,
                 std::shared_ptr<shm::MultiWriter> event_writer,
                 std::shared_ptr<dztrader::platform::LogConfig> self_log,
                 MirrorStore& mirror,
                 MdControlDomainService& md_control);
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

    /// 日志 tail 轮询回调（500ms 惰性定时器：首个日志订阅建立时启动，无订阅时取消）
    void on_timer();

    /// 数据变更广播：通知所有连接某 scope 的数据已变更，前端收到后 REST 刷新
    void broadcast_data_changed(const std::string& scope) override;

    /// 强制断开指定用户的所有 WS 连接（用户被删除/禁用时调用）
    void kick_user(const std::string& username) override;

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

    /// 出方向行情控制领域服务：md_connect/md_disconnect/query_md_subscriptions
    /// （P2 任务③：三个 C2S 分支下沉到服务，WsController 仅负责连接/鉴权/会话/routing）
    MdControlDomainService& md_control_;

    /// 单连接订阅状态
    struct Session {
        std::string user_id;
        std::string subscribed_log_file;  // empty = no subscription
        LogService::TailCursor log_cursor;  // tail 游标（字节偏移+行号，增量读）
        int tail_fail_count = 0;          // 连续 send 失败次数；达到 3 自动退订
        bool is_admin = false;             // 连接时按 DB 角色缓存（admin 角色控制消息预检）
    };
    std::unordered_map<drogon::WebSocketConnectionPtr, Session> sessions_;

    /// 日志 tail 惰性定时器（用户裁决：接受 tail 期间 500ms 轮询；无订阅时关闭）
    static constexpr double kLogTailIntervalSec = 0.5;
    trantor::TimerId log_tail_timer_{};
    bool log_tail_timer_active_ = false;
    /// 连接所在的 IO 循环（threadNum=1 时即 drogon::app().getIOLoop(0)）：
    /// 定时器注册到该循环，使 on_timer/poll_log_tail 与 WS 回调同线程串行，
    /// 消除 sessions_ / log_tail_timer_active_ 的跨线程竞争（惰性缓存，见 start_log_tail_timer）
    trantor::EventLoop* io_loop_ = nullptr;

    /// 首个日志订阅建立时启动 tail 轮询定时器（幂等）
    void start_log_tail_timer();

    /// 无任何日志订阅时取消 tail 轮询定时器（幂等；仍有订阅则不动）
    void stop_log_tail_timer_if_idle();

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
