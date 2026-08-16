#ifndef DZTRADER_WEBUI_MD_CONFIG_DOMAIN_SERVICE_H_
#define DZTRADER_WEBUI_MD_CONFIG_DOMAIN_SERVICE_H_

#include "process_mirror.h"
#include "repository.h"
#include "ws_broadcaster.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <cctype>
#include <string>

namespace dztrader::webui {

/// 行情源配置领域服务：RTN_MD_CONFIG 的镜像更新 + source 自动入库 + WS 广播（契约 md-config）。
/// 行为逐行对照原 WsController::handle_rtn_md_config：
/// - 镜像更新 process_mirror.update_config(source, payload)；
/// - source 前缀 dzmd_ 检查：未入库时以大写 suffix 为 source_type 自动入库
///   （find_market_source_by_source_name + create_market_source，已存在则跳过）；
/// - 广播 payload = {source, config}（instance_id 为空，与原 broadcast_to_all 消息一致：
///   消息里没有 instance_id 字段，source 在 payload 里）。
/// 线程安全：依赖 dzweb 固定单线程事件循环（thread_num=1），所有访问均发生在主循环串行执行，不加锁。
class MdConfigDomainService {
public:
    MdConfigDomainService(Repository& repo, ProcessMirror& process_mirror, WsBroadcaster& ws)
        : repo_(repo), process_mirror_(process_mirror), ws_(ws) {}

    void on_rtn_md_config(const std::string& source, const nlohmann::json& payload) {
        try {
            process_mirror_.update_config(source, payload);
            if (source.rfind("dzmd_", 0) == 0) {  // 自动入库（原样）
                auto existing = repo_.find_market_source_by_source_name(source);
                if (!existing.has_value()) {
                    std::string suffix = source.substr(5);
                    std::string source_type;
                    source_type.reserve(suffix.size());
                    for (auto c : suffix) {
                        source_type.push_back(
                            static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
                    }
                    repo_.create_market_source(source_type, source, source);
                    SPDLOG_INFO("market source reconciled from RTN_MD_CONFIG | name={} source_type={}",
                                source, source_type);
                }
            }
            ws_.broadcast("md_rtn_config", "",
                          nlohmann::json{{"source", source}, {"config", payload}});
            SPDLOG_DEBUG("md_rtn_config pushed | source={}", source);
        } catch (const std::exception& e) {
            SPDLOG_WARN("failed to parse RTN_MD_CONFIG | error={}", e.what());
        }
    }

private:
    Repository& repo_;
    ProcessMirror& process_mirror_;
    WsBroadcaster& ws_;
};

}  // namespace dztrader::webui

#endif  // DZTRADER_WEBUI_MD_CONFIG_DOMAIN_SERVICE_H_
