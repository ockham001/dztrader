#ifndef DZTRADER_CORE_THIS_PROCESS_H_
#define DZTRADER_CORE_THIS_PROCESS_H_

/**
 * @file this_process.h
 * @brief 当前进程自省（可执行文件路径、进程 ID）
 */

#include <filesystem>
#include <string>

namespace dztrader::this_process {

/**
 * @brief 当前可执行文件完整路径（缓存，线程安全）
 *
 * Linux: "/opt/dztrader/dztrader"
 * Windows: "C:\\Program Files\\dztrader\\dztrader.exe"
 */
const std::filesystem::path& exe_path();

/**
 * @brief 可执行文件所在目录（缓存，线程安全）
 */
const std::filesystem::path& exe_dir();

/**
 * @brief App Root 目录（缓存，线程安全）
 *
 * 从 exe_dir() 开始向上查找 dztraderd[.exe]，最多向上 3 层（共检查 4 个目录：
 * exe_dir 及其 3 个祖先目录）。找到则返回该目录，未找到抛 std::runtime_error。
 *
 * App Root 是所有进程发现的扫描锚点（契约 process）。
 */
const std::filesystem::path& app_root();

/**
 * @brief 可执行文件名（含扩展名，缓存，线程安全）
 *
 * Linux: "dztrader"，Windows: "dztrader.exe"
 */
const std::string& exe_name();

/**
 * @brief 可执行文件名（不含扩展名，缓存，线程安全）
 *
 * 跨平台统一: "dztrader"（自动剥离 .exe）
 */
const std::string& exe_stem();

/**
 * @brief 当前进程 ID
 */
int pid();

}  // namespace dztrader::this_process

#endif  /* DZTRADER_CORE_THIS_PROCESS_H_ */
