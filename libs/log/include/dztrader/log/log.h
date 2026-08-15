/**
 * @file log.h
 * @brief 日志初始化与崩溃处理
 *
 * 提供 set_default_logger() 便捷函数，创建并安装 spdlog default logger
 * 及 std::set_terminate 崩溃处理器（输出符号化堆栈跟踪）。
 */
#ifndef DZTRADER_LOG_LOG_H_
#define DZTRADER_LOG_LOG_H_

#include <dztrader/core/exception.h>
#include <filesystem>
#include <magic_enum/magic_enum.hpp>
#include <memory>
#include <spdlog/common.h>
#include <spdlog/spdlog.h>
#include <string>
#include <string_view>

namespace dztrader::log {

/// 文件日志格式（完整，便于事后分析与 AI 识别）
/// 每个字段用 key=value 显式标识，AI 无需查阅 pattern 文档即可理解
/// 示例: 2026-07-15T07:34:23.995522100+08:00 info dztraderd [func=main file=main.cpp:72 pid=32464 tid=31600] dztraderd starting | k=v
inline constexpr std::string_view FILE_PATTERN = "%Y-%m-%dT%T.%F%z %l %n [func=%! file=%s:%# pid=%P tid=%t] %v";

/// 控制台日志格式（简洁，便于实时查看）
/// 含月/日+秒级时间，省略 TID/文件/行号/函数名
inline constexpr std::string_view CONSOLE_PATTERN = "[%m/%d %H:%M:%S] [%^%l%$] [%n] %v";

/// set_default_logger() 的配置（启动时一次性使用，运行后不变）
struct LoggerSetup {
    std::string logger_name;        ///< logger 名，用于 logger 名和日志文件名
    std::filesystem::path log_dir;  ///< 日志目录，不存在时自动创建
#ifndef NDEBUG
    bool enable_console = true;                                    ///< 是否启用控制台输出
    spdlog::level::level_enum level = spdlog::level::debug;        ///< 日志级别
    spdlog::level::level_enum flush_level = spdlog::level::debug;  ///< 触发立即刷新的日志级别
#else
    bool enable_console = false;                                  ///< 是否启用控制台输出
    spdlog::level::level_enum level = spdlog::level::info;        ///< 日志级别
    spdlog::level::level_enum flush_level = spdlog::level::info;  ///< 触发立即刷新的日志级别
#endif
    bool service_mode = false;  ///< 服务模式，强制禁用控制台输出（适用于守护进程）
    uint16_t max_files = 0;     ///< 历史日志文件最大数量，0 表示不限制；7x24 长期运行建议设置（如 30）防止磁盘写满
};

/**
 * @brief 创建并安装 spdlog default logger + terminate handler
 *
 * - 校验 logger_name 和 log_dir（空值抛 Exception）
 * - 若 log_dir 不存在则自动创建
 * - 构建 sinks：daily_file_sink（始终）+ stdout_color_sink（enable_console && !service_mode）
 * - 创建同步 logger，设置 level 和 flush_on(flush_level)
 * - 调用 spdlog::set_default_logger
 * - 安装 std::set_terminate 处理器，崩溃时输出符号化堆栈跟踪
 *
 * 幂等：可重复调用（spdlog::set_default_logger 为替换式，set_terminate 为替换式）
 */
void set_default_logger(const LoggerSetup& config);

/**
 * @brief 获取固定的子进程转发 logger（名为 <logger_name>.fwd）
 *
 * 在 set_default_logger() 时创建一次，所有子进程共享。
 * 用于 master 进程转发子进程 stdout/stderr，日志的 %n 字段为 <logger_name>.fwd
 * （logger_name 由调用方传入，通常为 exe_stem，如 dztraderd.fwd）。
 * 子进程身份由消息体的 child_pid= 和 child_entry= 标识。
 *
 * - 复用 set_default_logger() 创建的 DailyFileSinkMt（线程安全）
 * - 不带 console sink（避免子进程输出到 master 控制台造成混乱）
 * - level / flush_on 继承 master logger 的设置
 * - func/file 字段由调用方通过 source_loc 占位符注入（如 func=forwarded file=external:1）
 *
 * @return 转发 logger shared_ptr；若 set_default_logger() 未先调用则返回 nullptr
 */
std::shared_ptr<spdlog::logger> forward_logger();

}  // namespace dztrader::log

#endif  // DZTRADER_LOG_LOG_H_
