#ifndef DZTRADER_CORE_TESTS_TEST_TEMP_DIR_H_
#define DZTRADER_CORE_TESTS_TEST_TEMP_DIR_H_

#include <cstdint>
#include <filesystem>
#include <random>
#include <string>
#include <string_view>

#include <dztrader/core/this_process.h>

namespace dztrader::test {

/// 测试临时资源唯一化（[单元测试判据](../../../docs/unit-test-standard.md) §3.5）。
///
/// **为什么必须唯一**：`gtest_discover_tests` 把每个 `TEST`/`TEST_F` 注册成独立 ctest
/// 用例，`ctest -j` 下同一测试二进制的多个用例是**并发进程**。任何进程间共用的固定名
/// 临时目录/文件（`temp_directory_path()/dz_xxx`）都会被并发用例互相 `remove_all`/覆盖，
/// 产生与改动无关的偶发失败（#136 实测：`DbConfigTest.StructuralViolationsThrowAndKeepFile`
/// 因共用 `dz_test_db_config` 偶发失败）。
///
/// **为什么后缀是「进程级」而非「每次调用」**：`paths::home()` / `paths::shm()` 按进程
/// 缓存首次解析结果。既然一个 ctest 用例 = 一个进程，进程级唯一就已经消除了并发冲突；
/// 而同一进程内（直接跑测试二进制时 gtest 顺序执行全部用例）各 fixture 必须拿到**同一个**
/// 目录名，否则后一个 `SetUp` 会删掉前一个已缓存进 `paths::*` 的目录。故后缀在进程内
/// 算一次并缓存。
///
/// 清理责任在调用方（`TearDown` / `atexit`），本头只负责取名。

/// 进程唯一后缀 `<pid>_<random>`：进程内恒等，跨进程必不同。
inline const std::string& unique_suffix() {
    static const std::string suffix = [] {
        std::random_device rd;
        return std::to_string(static_cast<std::uint32_t>(dztrader::this_process::pid())) + "_" +
               std::to_string(rd());
    }();
    return suffix;
}

/// 进程唯一临时目录：`<temp>/<name>_<pid>_<random>`
inline std::filesystem::path unique_temp_dir(std::string_view name) {
    return std::filesystem::temp_directory_path() / (std::string(name) + "_" + unique_suffix());
}

/// 进程唯一临时文件：`<temp>/<stem>_<pid>_<random>[.<ext>]`
inline std::filesystem::path unique_temp_path(std::string_view stem, std::string_view ext = {}) {
    std::string filename = std::string(stem) + "_" + unique_suffix();
    if (!ext.empty()) {
        filename += '.';
        filename += ext;
    }
    return std::filesystem::temp_directory_path() / filename;
}

}  // namespace dztrader::test

#endif  // DZTRADER_CORE_TESTS_TEST_TEMP_DIR_H_
