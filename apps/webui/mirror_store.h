#ifndef DZTRADER_WEBUI_MIRROR_STORE_H_
#define DZTRADER_WEBUI_MIRROR_STORE_H_

#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

namespace dztrader::webui {

/// 无 instance_id 帧（master 广播的 RTN_EVENT_SHM_CONFIG / RTN_PROCESS_CONFIG 等）的统一挂载实例
/// （设计文档 §4.3）：master 是这类帧的唯一来源，其 log_config 镜像本就以 dztraderd 为 key。
inline constexpr std::string_view kMasterInstance = "dztraderd";

/// 跨领域服务共用的镜像 domain 名（契约 webui-ws §2.1 的 domain 清单）：写入方（领域服务）
/// 与清理方（`TdAccountMirrorCleanup`）必须用同一个字面量——domain 名是协议取值，两处各写
/// 一份字符串就是「同一事实两处定义」，改一处漏一处会让清理静默失效。
inline constexpr std::string_view kDomainTdStatus = "td_status";
inline constexpr std::string_view kDomainProgress = "progress";

/// 统一状态镜像：instance_id -> { domain -> payload }
/// 契约模型（frame_contracts 各文件的"镜像"节）：dzweb 以 instance_id 为 key 维护各领域镜像，
/// 快照（snapshot）是 WS 连接时全量推送的唯一来源，增量帧由领域服务广播。
/// 线程安全：依赖 dzweb 固定单线程事件循环（thread_num=1），所有访问串行执行，不加锁。
class MirrorStore {
public:
    void update(const std::string& instance_id, std::string_view domain, nlohmann::json payload) {
        mirror_[instance_id][std::string(domain)] = std::move(payload);
    }

    void erase(const std::string& instance_id, std::string_view domain) {
        auto it = mirror_.find(instance_id);
        if (it == mirror_.end()) return;
        it->erase(std::string(domain));
        if (it->empty()) mirror_.erase(it);  // 实例无领域时整体移除，避免空壳累积
    }

    void remove(const std::string& instance_id) { mirror_.erase(instance_id); }

    const nlohmann::json& snapshot() const noexcept { return mirror_; }

    nlohmann::json instance(const std::string& id) const {
        auto it = mirror_.find(id);
        return it == mirror_.end() ? nlohmann::json::object() : it.value();
    }

private:
    nlohmann::json mirror_ = nlohmann::json::object();
};

}  // namespace dztrader::webui

#endif  // DZTRADER_WEBUI_MIRROR_STORE_H_
