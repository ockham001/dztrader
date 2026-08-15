#ifndef DZTRADER_MASTER_CONFIG_H_
#define DZTRADER_MASTER_CONFIG_H_

/**
 * @file config.h
 * @brief Master 进程配置：数据结构与 JSON 解析。
 */

#include <dztrader/core/json_section.h>
#include <dztrader/platform/process.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace dztrader::master {

/// 进程类别，决定默认重启策略和启动语义。
enum class Category : uint8_t { GatewayMd, GatewayTd, Strategy, WebUI };

/// 单个进程条目（来自 dztraderd.json 配置）。
struct ProcessEntry {
    std::string name;                              // "dzmd_ctp", "dztd_ctp", "my_strategy"
    Category category;
    std::filesystem::path exe;                     // boost::process 参数 1 (可能为空, launch_child 时按需扫描填充)
    std::vector<std::string> args;                 // boost::process 参数 2
    std::filesystem::path start_dir;               // process_start_dir
    std::unordered_map<std::string, std::string> env;  // 合并到 process_environment
    platform::RestartPolicy restart;

    /// 用户可读显示名 (来自 dztraderd.json md/td section 的 display_name 字段,
    /// 或运行时通过 PROCESS_CONTROL start 帧动态注入)。
    /// 为空时前端 UI 应回退到 name 显示。
    std::string display_name;
};

/// [master] 段配置 (仅 single_stop_timeout_sec; 日志相关字段迁移到 LogConfig)。
struct MasterConfig {
    /// 单进程停止超时阈值 (秒), 超时后主进程强制 kill 子进程
    /// 对应 ProcessSupervisor::single_stop_timeout_sec_, 默认 3
    int single_stop_timeout_sec = 3;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(MasterConfig, single_stop_timeout_sec)

    /// 从 JSON 文件的 "master" section 加载
    static MasterConfig load(const std::filesystem::path& path,
                             const std::string& section = "master");
    /// 原子写入到 "master" section (保留其他 section 不变)
    void save(const std::filesystem::path& path,
              const std::string& section = "master") const;
};

/// SHM 全局参数 (仅 meta_file_size, 启动后不变; event 子段由 EventShmConfig 类管理)。
struct ShmGlobalConfig {
    uint64_t meta_file_size = 1 * 1024 * 1024;  // 默认 1MB, 所有通道共用

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(ShmGlobalConfig, meta_file_size)

    /// 从 JSON 文件的 "shm" section 加载 (只读 meta_file_size, 忽略 event 子段)
    static ShmGlobalConfig load(const std::filesystem::path& path,
                                const std::string& section = "shm");
    /// 原子写入 "shm" section (保留 event 子段不变)
    void save(const std::filesystem::path& path,
              const std::string& section = "shm") const;
};

/// dztraderd.json 解析结果 (启动时聚合加载,运行时不再使用)
/// 注: log section 不在此结构体中——ShmManager 内部的 LogConfig 类自行加载
/// 注: shm.event 子段不在此结构体中——ShmManager 内部的 EventShmConfig 类自行加载
struct Config {
    MasterConfig master;                                       // 从 "master" section 读
    ShmGlobalConfig shm_global;                                // 从 "shm" section 读 (仅 meta_file_size)
    std::vector<ProcessEntry> entries;
};

/// 解析 dztraderd.json 并返回 Config。
/// 文件不存在或 JSON 解析失败抛 std::runtime_error。
Config parse_master_json(const std::filesystem::path& config_path);

/// 生成默认 dztraderd.json 配置文件。
/// 文件已存在时不覆盖。失败抛 std::runtime_error。
void generate_default_config(const std::filesystem::path& json_path);

/// 向 dztraderd.json 写入 [md.<name>] 或 [td.<name>] 段（若已存在则覆盖）。
/// category 决定写入 "md" 还是 "td" section。
/// 契约 05: 内部进程 (dzmd_*/dztd_*/dzweb) 的 exe 路径和 start_dir 不持久化到 json。
void write_gateway_section(const std::filesystem::path& config_path,
                           Category category,
                           const std::string& gateway_name,
                           const std::vector<std::string>& args,
                           const platform::RestartPolicy& restart,
                           const std::string& display_name = "");

/// 向 dztraderd.json 写入 [webui] 段（若已存在则覆盖）。
void write_webui_section(const std::filesystem::path& config_path,
                         const std::vector<std::string>& args,
                         const platform::RestartPolicy& restart,
                         const std::string& display_name = "");

/// 从 dztraderd.json 删除 [md.<name>] 或 [td.<name>] 段（若不存在则无操作）。
void remove_gateway_section(const std::filesystem::path& config_path,
                            Category category,
                            const std::string& gateway_name);

/// 从 dztraderd.json 删除 [webui] 段（若不存在则无操作）。
void remove_webui_section(const std::filesystem::path& config_path);

/// 类别转字符串（用于日志）。
const char* category_str(Category cat);

/// 给定类别的默认重启策略 (platform::RestartPolicy)。
platform::RestartPolicy default_restart_policy(Category cat);

}  // namespace dztrader::master

#endif  // DZTRADER_MASTER_CONFIG_H_
