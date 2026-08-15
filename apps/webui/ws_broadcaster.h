#ifndef DZTRADER_WEBUI_WS_BROADCASTER_H_
#define DZTRADER_WEBUI_WS_BROADCASTER_H_

#include <nlohmann/json.hpp>
#include <string>

namespace dztrader::webui {

/// 广播薄接口：供领域服务注入广播能力（不依赖 drogon / WsController 具体实现）。
/// 广播一帧 WS 消息（type + 可选 instance_id + data）；单连接失败不阻断其他连接。
class WsBroadcaster {
public:
    virtual ~WsBroadcaster() = default;

    virtual void broadcast(const std::string& type,
                           const std::string& instance_id,
                           const nlohmann::json& data) = 0;
};

}  // namespace dztrader::webui

#endif  // DZTRADER_WEBUI_WS_BROADCASTER_H_
