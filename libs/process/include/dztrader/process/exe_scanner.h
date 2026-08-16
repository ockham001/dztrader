#ifndef DZTRADER_PROCESS_EXE_SCANNER_H_
#define DZTRADER_PROCESS_EXE_SCANNER_H_

/**
 * @file exe_scanner.h
 * @brief 进程可执行文件扫描: 2 层目录 + 去重, 跨平台
 *
 * master ProcessRegistry::find_exe_by_stem 与 dzweb list_available 共用此实现,
 * 消除两处重复的扫描规则。
 *
 * 扫描规则 (契约 process):
 * - 锚点: this_process::app_root() (从 exe_dir 向上查找 dztraderd[.exe])
 * - 第一层: 扫描 root 下所有可执行文件
 * - 第二层: 扫描 root 下每个子目录 (仅该子目录, 不递归)
 * - 去重: 根目录优先, 二级子目录同 stem 跳过
 * - 识别前缀: dzmd_/dztd_/dzweb (stem 长度 > 5, 排除 "dzmd_" 退化)
 * - 跨平台: Windows .exe/.bat/.cmd, Linux owner_exec 权限位
 * - 失败路径 C: 子目录 IO 错误时打 WARN 并跳过该子目录
 */

#include <dztrader/core/this_process.h>
#include <dztrader/process/process_kind.h>

#include <filesystem>
#include <optional>
#include <string_view>
#include <vector>

namespace dztrader::process {

/// 扫描指定 root 目录下所有可执行文件 (2 层 + 去重, 根目录优先)
/// 默认 root = this_process::app_root()
/// 返回结果按 name 字典序排序 (保证调用方输出稳定)
/// IO 错误: 子目录错误时 SPDLOG_WARN 并跳过, 不抛异常
std::vector<ProcessExeInfo> scan_all_exes(
    const std::filesystem::path& root = dztrader::this_process::app_root());

/// 实时扫描指定 name 的可执行文件 (基于 scan_all_exes + 按 name 查找)
/// 找到返回 ProcessExeInfo, 未找到返回 nullopt
std::optional<ProcessExeInfo> find_exe_by_stem(
    std::string_view name,
    const std::filesystem::path& root = dztrader::this_process::app_root());

}  // namespace dztrader::process

#endif  // DZTRADER_PROCESS_EXE_SCANNER_H_
