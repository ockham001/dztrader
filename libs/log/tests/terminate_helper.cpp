#include <dztrader/log/log.h>

#include <spdlog/spdlog.h>

#include <exception>
#include <stdexcept>
#include <string>

namespace {

std::string arg_value(int argc, char** argv, const std::string& key) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (key == argv[i]) {
            return argv[i + 1];
        }
    }
    return {};
}

}  // namespace

/// 独立进程：安装 terminate handler 后主动 abort，供父测试读取日志验证落盘。
///
/// 不用 gtest death test：Windows 上它会重新执行测试二进制，父进程的临时目录/环境
/// 无法可靠传给子进程；普通进程的 argv 是跨平台可靠的。
int main(int argc, char** argv) {
    const std::string log_dir = arg_value(argc, argv, "--log-dir");
    const std::string mode = arg_value(argc, argv, "--mode");
    if (log_dir.empty()) {
        return 2;
    }

    dztrader::log::LoggerSetup config;
    config.logger_name = "th_helper";
    config.enable_console = false;
    config.log_dir = log_dir;
    config.flush_level = spdlog::level::warn;
    dztrader::log::set_default_logger(config);

    if (mode == "uncaught") {
        try {
            throw std::runtime_error("boom from helper");
        } catch (...) {
            std::terminate();
        }
    }
    std::terminate();
}
