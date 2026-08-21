#ifndef DZTRADER_WEBUI_DATA_CHANGE_NOTIFIER_H_
#define DZTRADER_WEBUI_DATA_CHANGE_NOTIFIER_H_

#include <string>

namespace dztrader::webui {

/// 数据变更通知薄接口：供业务 controller 注入广播/踢人能力（不依赖 drogon / WsController 具体实现）。
/// 取代全局函数指针 g_broadcast_data_changed / g_kick_user（P2 任务①：全局指针注入化）。
/// WsController 实现此接口；各业务 controller 构造时注入引用。
class DataChangeNotifier {
public:
    virtual ~DataChangeNotifier() = default;

    /// 通知所有 WS 客户端某 scope 的数据已变更，前端收到后 REST 刷新
    virtual void broadcast_data_changed(const std::string& scope) = 0;

    /// 强制断开指定用户的所有 WS 连接（用户被删除/禁用/降级时调用）
    virtual void kick_user(const std::string& username) = 0;
};

}  // namespace dztrader::webui

#endif  // DZTRADER_WEBUI_DATA_CHANGE_NOTIFIER_H_