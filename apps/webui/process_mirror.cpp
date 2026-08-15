#include "process_mirror.h"
#include <spdlog/spdlog.h>

namespace {
// 领域 key（与领域服务写入约定一致，见设计文档 §4.2）
constexpr std::string_view DOMAIN_STATUS = "process_status";
constexpr std::string_view DOMAIN_MD_CONFIG = "md_config";
constexpr std::string_view DOMAIN_MD_STATUS = "md_status";
constexpr std::string_view DOMAIN_PROCESS_CONFIG = "process_config";
constexpr std::string_view DOMAIN_AUTO_LOGIN = "auto_login";
constexpr std::string_view DOMAIN_PROGRESS = "progress";
}  // namespace

namespace dztrader::webui {

ProcessMirror::ProcessMirror() : mirror_(owned_) {}

ProcessMirror::ProcessMirror(MirrorStore& mirror) : mirror_(mirror) {}

void ProcessMirror::update_status(const std::string& name, const platform::ProcessStatus& status) {
    nlohmann::json j;
    to_json(j, status);
    mirror_.update(name, DOMAIN_STATUS, std::move(j));
}

void ProcessMirror::update_config(const std::string& name, const nlohmann::json& config) {
    mirror_.update(name, DOMAIN_MD_CONFIG, config);
}

void ProcessMirror::update_gateway_status(const std::string& name, const nlohmann::json& status) {
    mirror_.update(name, DOMAIN_MD_STATUS, status);
}

void ProcessMirror::mark_stale(const std::string& name) {
    // 保留 process_status；清除状态类领域 md_config/md_status/progress。
    // progress 一并清除: 崩溃/停止后残留的旧登录进度（如崩溃前 "已登录"）会造成
    // 前端 loginState 与 process_state 矛盾（Crashed + online）；进程重启后由
    // 新 RTN_PROGRESS 覆盖。配置类领域（auto_login/log_config）保留（非状态, 重启后覆盖）。
    mirror_.erase(name, DOMAIN_MD_CONFIG);
    mirror_.erase(name, DOMAIN_MD_STATUS);
    mirror_.erase(name, DOMAIN_PROGRESS);
}

std::vector<platform::ProcessStatus> ProcessMirror::get_all() const {
    std::vector<platform::ProcessStatus> result;
    for (auto it = mirror_.snapshot().begin(); it != mirror_.snapshot().end(); ++it) {
        const auto& domains = it.value();
        auto sit = domains.find("process_status");
        if (sit == domains.end()) {
            continue;
        }
        try {
            platform::ProcessStatus s;
            from_json(sit.value(), s);
            result.push_back(std::move(s));
        } catch (const std::exception& e) {
            SPDLOG_WARN("parse process_status failed | instance={} error={}", it.key(), e.what());
        }
    }
    return result;
}

std::optional<platform::ProcessStatus> ProcessMirror::get_status(const std::string& name) const {
    auto j = mirror_.instance(name);
    auto it = j.find("process_status");
    if (it == j.end()) {
        return std::nullopt;
    }
    try {
        platform::ProcessStatus s;
        from_json(it.value(), s);
        return s;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

void ProcessMirror::update_process_configs(const nlohmann::json& full_map) {
    // 全量覆盖前: 找出配置条目消失的进程, 清理其实例镜像（契约 03: 条目消失 = 进程已移除）。
    // 若不清理: Remove 流程中 118(条目消失) 先于 116(Stopped) 到达, 而 Stopped 帧被
    // 注册守卫拒绝（契约"忽略已删除进程的 status"）, 镜像残留 process_status=Stopping,
    // 经 snapshot/REST list（不过滤 Stopping）让已移除进程在重连后"复活"。
    auto old = mirror_.instance(std::string(kMasterInstance));
    auto it = old.find("process_config");
    if (it != old.end() && it.value().is_object()) {
        for (auto pit = it.value().begin(); pit != it.value().end(); ++pit) {
            if (!full_map.contains(pit.key())) {
                remove(pit.key());
            }
        }
    }
    // 全量覆盖（覆盖天然含删除）：整体替换 dztraderd.process_config
    mirror_.update(std::string(kMasterInstance), DOMAIN_PROCESS_CONFIG, full_map);
}

std::optional<nlohmann::json> ProcessMirror::get_process_config(const std::string& name) const {
    auto j = mirror_.instance(std::string(kMasterInstance));
    auto it = j.find("process_config");
    if (it == j.end()) {
        return std::nullopt;
    }
    auto cit = it.value().find(name);
    if (cit == it.value().end()) {
        return std::nullopt;
    }
    return cit.value();
}

std::optional<nlohmann::json> ProcessMirror::get_config(const std::string& name) const {
    auto j = mirror_.instance(name);
    auto it = j.find("md_config");
    if (it == j.end()) {
        return std::nullopt;
    }
    return it.value();
}

std::optional<nlohmann::json> ProcessMirror::get_gateway_status(const std::string& name) const {
    auto j = mirror_.instance(name);
    auto it = j.find("md_status");
    if (it == j.end()) {
        return std::nullopt;
    }
    return it.value();
}

std::optional<nlohmann::json> ProcessMirror::get_auto_login(const std::string& name) const {
    auto j = mirror_.instance(name);
    auto it = j.find(DOMAIN_AUTO_LOGIN);
    if (it == j.end()) {
        return std::nullopt;
    }
    return it.value();
}

void ProcessMirror::remove(const std::string& name) {
    // 清进程相关领域（与 clear()/mark_stale 的领域范围一致）；auto_login/log_config
    // 等配置域保留残留（进程重启后由 RTN 覆盖, 展示层按 process_config 条目过滤）。
    mirror_.erase(name, DOMAIN_STATUS);
    mirror_.erase(name, DOMAIN_MD_CONFIG);
    mirror_.erase(name, DOMAIN_MD_STATUS);
    mirror_.erase(name, DOMAIN_PROGRESS);
    // 同时清理 dztraderd.process_config 中的条目
    auto j = mirror_.instance(std::string(kMasterInstance));
    auto it = j.find("process_config");
    if (it != j.end() && it.value().contains(name)) {
        auto pc = it.value();
        pc.erase(name);
        mirror_.update(std::string(kMasterInstance), DOMAIN_PROCESS_CONFIG, std::move(pc));
    }
}

void ProcessMirror::clear() {
    // 仅清进程相关领域：遍历快照删除三领域 + 清 dztraderd.process_config
    auto snap = mirror_.snapshot();
    for (auto it = snap.begin(); it != snap.end(); ++it) {
        mirror_.erase(it.key(), DOMAIN_STATUS);
        mirror_.erase(it.key(), DOMAIN_MD_CONFIG);
        mirror_.erase(it.key(), DOMAIN_MD_STATUS);
    }
    mirror_.erase(std::string(kMasterInstance), DOMAIN_PROCESS_CONFIG);
}

}  // namespace dztrader::webui
