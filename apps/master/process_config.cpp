#include "process_config.h"

#include <dztrader/core/core_data_type.h>  // DZ_FRAME_RTN_PROCESS_CONFIG

#include <spdlog/spdlog.h>

#include <stdexcept>
#include <utility>

namespace dztrader::master {

using dztrader::platform::apply_process_config_patch;
using dztrader::platform::validate_process_config_full;
using dztrader::platform::validate_process_config_patch;

ProcessConfigStore::ProcessConfigStore(shm::MultiWriter& event_writer,
                                       PersistFn persist_fn,
                                       PersistFn apply_fn)
    : event_writer_(event_writer),
      persist_fn_(std::move(persist_fn)),
      apply_fn_(std::move(apply_fn)) {}

void ProcessConfigStore::load(const nlohmann::json& initial_map) {
    if (!initial_map.is_object()) {
        SPDLOG_ERROR("process config load failed: initial map must be an object");
        return;
    }
    for (auto it = initial_map.begin(); it != initial_map.end(); ++it) {
        const std::string& name = it.key();
        if (auto err = validate_process_config_full(it.value())) {
            SPDLOG_ERROR("process config load skipped invalid entry | name={} error=\"{}\"",
                         name, *err);
            continue;
        }
        cfg_map_[name] = it.value();
    }
}

void ProcessConfigStore::set_process_config(const std::string& target,
                                            const nlohmann::json& patch) {
    // 契约 process: target 不存在时校验失败
    auto it = cfg_map_.find(target);
    if (it == cfg_map_.end()) {
        throw std::runtime_error("target not registered | target=" + target);
    }
    // 增量角色校验（契约 process: SET / Start 携带的 config 同规则）
    if (auto err = validate_process_config_patch(patch)) {
        throw std::runtime_error("invalid process config patch | target=" + target +
                                 " error=" + *err);
    }
    // 在副本上合并（抛异常时镜像不变, 强保证）
    nlohmann::json merged = apply_process_config_patch(it->second, patch);
    // 先持久化后应用（副作用窗口见头文件注释）
    persist_fn_(target, merged);
    apply_fn_(target, merged);
    cfg_map_[target] = std::move(merged);
}

void ProcessConfigStore::register_process(const std::string& name,
                                         const nlohmann::json& full_config) {
    if (cfg_map_.contains(name)) {
        throw std::runtime_error("already registered | target=" + name);
    }
    if (auto err = validate_process_config_full(full_config)) {
        throw std::runtime_error("invalid process config | target=" + name +
                                 " error=" + *err);
    }
    // 先持久化后应用（副作用窗口见头文件注释）
    persist_fn_(name, full_config);
    apply_fn_(name, full_config);
    cfg_map_[name] = full_config;
}

void ProcessConfigStore::remove(const std::string& name) {
    if (!cfg_map_.contains(name)) {
        throw std::runtime_error("target not registered | target=" + name);
    }
    // null 表示删除条目（契约: 条目消失 = 进程已移除）
    persist_fn_(name, nlohmann::json());
    apply_fn_(name, nlohmann::json());
    cfg_map_.erase(name);  // 最后更新镜像（回调失败时镜像不变）
}

void ProcessConfigStore::rtn_process_config() {
    // 始终全量镜像（契约 process），无 error 字段，无 instance_id（契约 process）
    try {
        const auto str = nlohmann::json(cfg_map_).dump();
        if (!event_writer_.write_ext_frame(DZ_FRAME_RTN_PROCESS_CONFIG,
                                           reinterpret_cast<const std::byte*>(str.data()),
                                           static_cast<uint32_t>(str.size()))) {
            SPDLOG_ERROR("process config rtn write failed");
            return;
        }
        event_writer_.notify_subscribers();
    } catch (const std::exception& e) {
        SPDLOG_ERROR("process config rtn serialize failed | error=\"{}\"", e.what());
    }
}

const nlohmann::json* ProcessConfigStore::find(const std::string& name) const {
    auto it = cfg_map_.find(name);
    return it == cfg_map_.end() ? nullptr : &it->second;
}

}  // namespace dztrader::master
