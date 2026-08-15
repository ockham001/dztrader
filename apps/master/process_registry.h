#ifndef DZTRADER_MASTER_PROCESS_REGISTRY_H_
#define DZTRADER_MASTER_PROCESS_REGISTRY_H_

/**
 * @file process_registry.h
 * @brief 进程注册：从 dztraderd.json 加载进程条目。
 *
 * 仅解析 json 配置得到 entries_(不含 scan-only 条目)。
 * 启动子进程时(launch_child)若 entry.exe 为空, 调 find_exe_by_stem 实时扫描填充。
 */

#include "config.h"

#include <filesystem>
#include <string>
#include <vector>

namespace dztrader::master {

class ProcessRegistry {
public:
    /// 从 dztraderd.json 加载进程条目 (不再扫描 app_root)。
    void load(const std::filesystem::path& config_path);

    /// 获取所有已注册的进程条目 (仅 json 声明条目)。
    const std::vector<ProcessEntry>& entries() const;

    /// 按名称查找，未找到返回 nullptr。
    const ProcessEntry* find(std::string_view name) const;

    /// 实时扫描 App Root 和子目录, 查找指定 stem 的可执行文件。
    /// 找到返回 ProcessEntry (含 exe 路径), 未找到返回 nullptr。
    /// 不会修改 entries_ 缓存 (扫描结果不缓存)。
    /// 失败路径 C: 子目录 IO 错误时打 WARN 并跳过该子目录。
    const ProcessEntry* find_exe_by_stem(std::string_view name) const;

    /// 运行时更新 display_name (用于 PROCESS_CONTROL start 时
    /// 将 dzweb 透传的 display_name 注入 registry, 供后续 send_process_status 查询)。
    /// 未找到条目时无操作。返回是否实际更新。
    bool update_display_name(std::string_view name, const std::string& display_name);

    /// 运行时更新条目的运行配置 (store apply 回调用; 未找到无操作)。
    /// 更新 args/env/restart/display_name, 保留 name/category/exe/start_dir。
    void update_entry(std::string_view name,
                      const std::vector<std::string>& args,
                      const std::unordered_map<std::string, std::string>& env,
                      const platform::RestartPolicy& restart,
                      const std::string& display_name);

    /// 运行时动态注册（迭代 2）。
    void register_strategy(ProcessEntry entry);

    /// 运行时动态注册网关进程（PROCESS_CONTROL start 未注册目标扫描命中时,
    /// 契约 03 修订: "添加行情源"场景动态注册并持久化 dztraderd.json）。
    /// 已存在同名条目: 抛 Exception(DZ_EC_ALREADY_EXISTS)。
    void register_gateway(ProcessEntry entry);

    /// 运行时注销（迭代 2）。
    void unregister(std::string_view name);

private:
    std::vector<ProcessEntry> entries_;
};

}  // namespace dztrader::master

#endif  // DZTRADER_MASTER_PROCESS_REGISTRY_H_
