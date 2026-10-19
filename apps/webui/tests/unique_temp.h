#ifndef DZTRADER_WEBUI_TESTS_UNIQUE_TEMP_H_
#define DZTRADER_WEBUI_TESTS_UNIQUE_TEMP_H_

#include <cstdint>
#include <filesystem>
#include <random>
#include <string>

#include <dztrader/core/this_process.h>

namespace dztrader::webui::test {

/// 唯一化工具（docs/unit-test-standard.md §3.5）：ctest -j 下同一测试二进制的
/// 各用例作为独立进程并发运行，固定名目录/文件会互相删改冲突，优先追加
/// PID + 随机后缀。

/// 进程唯一后缀（PID + 随机数）
inline std::string unique_suffix() {
    std::random_device rd;
    return std::to_string(static_cast<uint32_t>(dztrader::this_process::pid())) + "_" +
           std::to_string(rd());
}

/// 进程唯一临时目录: <temp>/<name>_<pid>_<random>
inline std::filesystem::path unique_temp_dir(const std::string& name) {
    return std::filesystem::temp_directory_path() / (name + "_" + unique_suffix());
}

}  // namespace dztrader::webui::test

#endif  // DZTRADER_WEBUI_TESTS_UNIQUE_TEMP_H_
