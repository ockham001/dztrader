#include "process_registry.h"

#include <dztrader/core/encoding.h>
#include <dztrader/core/exception.h>
#include <dztrader/core/path.h>
#include <dztrader/core/this_process.h>
#include <dztrader/error.h>
#include <dztrader/process/exe_scanner.h>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <unordered_set>

namespace dztrader::master {

namespace {

/// ProcessKind -> Category 转换 (master 内部类型映射)
Category kind_to_category(process::ProcessKind kind) {
    switch (kind) {
        case process::ProcessKind::GatewayMd: return Category::GatewayMd;
        case process::ProcessKind::GatewayTd: return Category::GatewayTd;
        case process::ProcessKind::WebUI:     return Category::WebUI;
        default:                              return Category::Strategy;
    }
}

}  // namespace

void ProcessRegistry::load(const std::filesystem::path& config_path) {
    entries_.clear();
    auto cfg = parse_master_json(config_path);
    for (auto& entry : cfg.entries) {
        entries_.push_back(std::move(entry));
    }
    SPDLOG_INFO("registry loaded from json | count={}", entries_.size());
}

const std::vector<ProcessEntry>& ProcessRegistry::entries() const { return entries_; }

const ProcessEntry* ProcessRegistry::find(std::string_view name) const {
    for (const auto& e : entries_) {
        if (e.name == name) {
            return &e;
        }
    }
    return nullptr;
}

const ProcessEntry* ProcessRegistry::find_exe_by_stem(std::string_view name) const {
    // 委托给 libs/process 库 (消除与 dzweb list_available 的扫描规则重复)
    // 保留 thread_local ProcessEntry 缓冲: 调用方持指针, 跨调用不可用
    // 类型转换: ProcessExeInfo -> ProcessEntry (填充 Category + 默认 restart 策略)
    const auto& root = dztrader::this_process::app_root();
    SPDLOG_INFO("find_exe_by_stem | name={} app_root={}", name, root.string());

    auto opt = dztrader::process::find_exe_by_stem(name, root);
    if (!opt) {
        SPDLOG_WARN("find_exe_by_stem: not found | name={}", name);
        return nullptr;
    }

    static thread_local ProcessEntry tls_entry;
    tls_entry = ProcessEntry{};
    tls_entry.name = opt->name;
    tls_entry.category = kind_to_category(opt->kind);
    tls_entry.exe = opt->exe;
    tls_entry.start_dir = opt->start_dir;
    tls_entry.restart = default_restart_policy(tls_entry.category);
    SPDLOG_INFO("find_exe_by_stem: found | name={} exe={}",
                tls_entry.name, tls_entry.exe.string());
    return &tls_entry;
}

bool ProcessRegistry::update_display_name(std::string_view name,
                                           const std::string& display_name) {
    for (auto& e : entries_) {
        if (e.name == name) {
            if (e.display_name == display_name) {
                return false;
            }
            e.display_name = display_name;
            return true;
        }
    }
    return false;
}

void ProcessRegistry::update_entry(std::string_view name,
                                   const std::vector<std::string>& args,
                                   const std::unordered_map<std::string, std::string>& env,
                                   const platform::RestartPolicy& restart,
                                   const std::string& display_name) {
    for (auto& e : entries_) {
        if (e.name == name) {
            e.args = args;
            e.env = env;
            e.restart = restart;
            e.display_name = display_name;
            return;
        }
    }
}

void ProcessRegistry::register_strategy(ProcessEntry entry) {
    if (find(entry.name)) {
        throw Exception(DZ_EC_ALREADY_EXISTS, "strategy already registered | name={}", entry.name);
    }
    entries_.push_back(std::move(entry));
}

void ProcessRegistry::register_gateway(ProcessEntry entry) {
    if (find(entry.name)) {
        throw Exception(DZ_EC_ALREADY_EXISTS, "gateway already registered | name={}", entry.name);
    }
    entries_.push_back(std::move(entry));
}

void ProcessRegistry::unregister(std::string_view name) {
    auto it = std::remove_if(entries_.begin(), entries_.end(),
                             [name](const ProcessEntry& e) { return e.name == name; });
    if (it != entries_.end()) {
        entries_.erase(it, entries_.end());
    }
}

}  // namespace dztrader::master
