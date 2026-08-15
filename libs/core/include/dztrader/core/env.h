#ifndef DZTRADER_CORE_ENV_H_
#define DZTRADER_CORE_ENV_H_

/**
 * @file env.h
 * @brief 环境变量操作
 */

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace dztrader::env {

/**
 * @brief 获取环境变量，未设置返回 nullopt
 */
std::optional<std::string> get(std::string_view key);

/**
 * @brief 获取环境变量，未设置返回 default_value
 */
std::string get_or(std::string_view key, std::string_view default_value);

/**
 * @brief 设置环境变量（当前进程），失败抛 Exception
 */
void set(std::string_view key, std::string_view value);

/**
 * @brief 删除环境变量（当前进程），失败抛 Exception
 */
void unset(std::string_view key);

/**
 * @brief 在 PATH 中查找可执行文件，未找到返回 nullopt
 *
 * 跨平台：Linux 搜索 PATH，Windows 额外搜索 PATHEXT
 */
std::optional<std::filesystem::path> find_executable(std::string_view name);

/**
 * @brief 操作系统用户主目录
 *
 * Linux: $HOME，Windows: %USERPROFILE%
 * 无法确定时抛 Exception
 */
std::filesystem::path home_dir();

}  // namespace dztrader::env

#endif  /* DZTRADER_CORE_ENV_H_ */
