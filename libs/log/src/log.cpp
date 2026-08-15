#include "daily_sink.h"
#include <boost/stacktrace.hpp>
#include <dztrader/core/encoding.h>
#include <dztrader/core/exception.h>
#include <dztrader/log/log.h>
#include <exception>
#include <filesystem>
#include <format>
#include <iostream>
#include <memory>
#include <spdlog/sinks/daily_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>
#include <string>
#include <vector>

namespace dztrader::log {

namespace {

#ifdef _WIN32
/// Windows: 防御性 SEH 保护。terminate_handler 是最后防线，stacktrace 捕获失败不应二次崩溃。
/// 用 SEH __try/__except 包裹 boost::stacktrace 调用，使其在信号/terminate 上下文中
/// 偶发触发的异常（如无效符号访问）被吞掉，仍能记录 reason 便于事后排查。
/// 注意：MSVC 不允许同一函数中既有 __try/__except 又有带析构函数的 C++ 栈对象，
/// 故拆分为 impl (含 boost::stacktrace 对象) + safe (含 __try/__except, 仅 POD/引用) 两层。
void capture_stacktrace_impl(std::string& out) {
    auto st = boost::stacktrace::stacktrace();
    out = boost::stacktrace::to_string(st);
}

bool capture_stacktrace_safe(std::string& out) {
    __try {
        capture_stacktrace_impl(out);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
#else
/// Linux/其他平台：boost::stacktrace 在信号/terminate 上下文可能抛 C++ 异常，用 try/catch 兜底。
bool capture_stacktrace_safe(std::string& out) {
    try {
        auto st = boost::stacktrace::stacktrace();
        out = boost::stacktrace::to_string(st);
        return true;
    } catch (...) {
        return false;
    }
}
#endif

void terminate_handler() {
    try {
        std::string reason;
        if (auto ep = std::current_exception()) {
            try {
                std::rethrow_exception(ep);
            } catch (const std::exception& e) {
                reason = std::format("uncaught exception: {}", e.what());
            } catch (...) {
                reason = "uncaught exception: unknown type";
            }
        } else {
            reason = "std::terminate called (no active exception)";
        }

        std::string stacktrace_str;
        bool captured = capture_stacktrace_safe(stacktrace_str);
        if (captured) {
            spdlog::critical("{}\n{}", reason, stacktrace_str);
        } else {
            // stacktrace 捕获失败 (SEH 或异常)，仍记录 reason 便于事后排查
            spdlog::critical("{}\n<stacktrace capture failed>", reason);
        }
        spdlog::apply_all([](auto logger) {
            try {
                logger->flush();
            } catch (...) {  // NOLINT(bugprone-empty-catch)
                // terminate 上下文中不可抛异常，忽略 flush 失败
            }
        });
    } catch (...) {
        std::cerr << "std::terminate called (stacktrace capture failed)" << std::endl;
    }
    std::abort();
}

/// master logger 的 file sink，由 set_default_logger() 存入
/// 供 forward_logger() 复用，使所有 logger 写同一文件
std::shared_ptr<spdlog::sinks::sink> g_file_sink;

/// 固定的子进程转发 logger，由 set_default_logger() 创建一次
/// 名为 <logger_name>.fwd，共享 g_file_sink，所有 ChildProcess 共享
std::shared_ptr<spdlog::logger> g_forward_logger;

}  // namespace

void set_default_logger(const LoggerSetup& config) {
    if (config.logger_name.empty()) {
        throw Exception(DZ_EC_INVALID_PARAM, "logger_name must not be empty");
    }
    if (config.logger_name.find_first_of("/\\:") != std::string::npos) {
        throw Exception(DZ_EC_INVALID_PARAM, "logger_name must not contain path separator");
    }
    if (config.log_dir.empty()) {
        throw Exception(DZ_EC_INVALID_PARAM, "log_dir must not be empty");
    }

    const auto& name = config.logger_name;
    const auto& log_dir = config.log_dir;

    std::error_code ec;
    if (!std::filesystem::create_directories(log_dir, ec) && ec) {
        throw Exception(DZ_EC_SYSTEM, "failed to create log directory: path={} err={}",
                        log_dir.string(), dztrader::to_utf8_from_system(ec.message()));
    }

    std::vector<std::shared_ptr<spdlog::sinks::sink>> sinks;

    // 显式传 truncate=false 与 max_files=config.max_files
    // max_files=0（默认）不限制历史日志数量；md/td 等 7x24 进程应设为 30 防止磁盘写满
    auto file_sink = std::make_shared<DailyFileSinkMt>(
        (log_dir / (name + ".log")).string(), 0, 0,
        false, config.max_files);  // rotation_hour=0, rotation_minute=0, truncate=false
    file_sink->set_pattern(std::string(FILE_PATTERN));
    g_file_sink = file_sink;  // 供 create_child_logger() 复用
    sinks.push_back(std::move(file_sink));

    const bool use_console = config.enable_console && !config.service_mode;
    if (use_console) {
        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        console_sink->set_pattern(std::string(CONSOLE_PATTERN));
        sinks.push_back(std::move(console_sink));
    }

    auto logger = std::make_shared<spdlog::logger>(name, sinks.begin(), sinks.end());
    logger->set_level(config.level);
    logger->flush_on(config.flush_level);

    spdlog::set_default_logger(logger);

    // 创建固定的子进程转发 logger（名为 <logger_name>.fwd）
    // 共享 master 的 file sink，不带 console sink，继承 level 和 flush_on
    // logger_name 来自调用方（通常为 exe_stem），避免硬编码进程名
    g_forward_logger = std::make_shared<spdlog::logger>(name + ".fwd", g_file_sink);
    g_forward_logger->set_pattern(std::string(FILE_PATTERN));
    g_forward_logger->set_level(config.level);
    g_forward_logger->flush_on(config.flush_level);

    std::set_terminate(terminate_handler);
}

std::shared_ptr<spdlog::logger> forward_logger() {
    return g_forward_logger;
}

}  // namespace dztrader::log
