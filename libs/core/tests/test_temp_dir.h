#ifndef DZTRADER_CORE_TESTS_TEST_TEMP_DIR_H_
#define DZTRADER_CORE_TESTS_TEST_TEMP_DIR_H_

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include <boost/asio/io_context.hpp>
#include <boost/process/v2/process.hpp>

#include <dztrader/core/this_process.h>

namespace dztrader::test {

/// 自派子进程探针的子命令：登记一个 tracked 目录并把路径写进 marker，然后正常退出。
///
/// 定义在共享头而非任一 `.cpp`：探针的**发送方**（用例）与**接收方**（`main()` 分发）
/// 必须看到同一个字面量，否则改一处就静默失配（#516 审查 §2.4）。
inline constexpr std::string_view kTrackedTempDirProbeFlag = "--temp-dir-tracked-probe";

/// 本进程登记过的唯一临时目录（atexit 统一清理，见 unique_temp_dir_tracked）。
inline std::vector<std::filesystem::path>& tracked_dirs() {
    static std::vector<std::filesystem::path> dirs;
    return dirs;
}

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

/// 登记一个「进程唯一目录」以便进程退出时统一清理。
///
/// 唯一化把「一个复用目录」变成「每个测试进程一个新目录」，若调用方不删就会在系统临时
/// 目录里无界堆积（#136 复审实测：`dz_test_logs_*` / `dz_shm_test_*` 从未被删）。
/// 用 atexit 统一回收：与调用方是否写了 TearDown 无关，也不会掩盖断言失败。
///
/// **注册顺序是正确性前提（#516）**：`atexit` 处理器按注册逆序执行，函数局部静态对象按
/// 构造逆序析构，所以必须先构造 `tracked_dirs()` 的 vector、再注册回调，清理时 vector
/// 才仍然存活。顺序写反的后果是回调遍历已释放的 vector 缓冲区：目录删不掉（登记进
/// vector 的路径成了垃圾，`remove_all` 无从命中），且读垃圾 `path` 时可能抛
/// `std::bad_alloc` —— 那发生在退出期、无人 catch，于是 `std::terminate` 把一个
/// 「用例全部 PASSED」的测试进程判成 abort，全量 ctest 变成假红。
inline std::filesystem::path unique_temp_dir_tracked(std::string_view name) {
    const auto dir =
        std::filesystem::temp_directory_path() / (std::string(name) + "_" + unique_suffix());
    // 先构造静态 vector，再注册回调 —— 顺序颠倒即触发上述 use-after-free。
    auto& tracked = tracked_dirs();
    static const bool registered = [] {
        std::atexit([] {
            for (const auto& path : tracked_dirs()) {
                std::error_code ec;
                std::filesystem::remove_all(path, ec);
            }
        });
        return true;
    }();
    (void)registered;
    tracked.push_back(dir);
    return dir;
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

/// 普通临时目录 RAII：构造时建目录、析构时删掉，**不**登记 atexit。
///
/// 与 `unique_temp_dir_tracked()` 的分工：需要「进程退出后仍在」的观察窗口（如验证
/// 退出期清理本身）时用本类，让清理边界落在作用域上、由用例自己掌握。
class ScopedTempDir {
public:
    explicit ScopedTempDir(std::string_view name)
        : path_(unique_temp_dir(name)) {
        std::filesystem::create_directories(path_);
    }

    ~ScopedTempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

/// 以「本测试二进制 + args」自派一个子进程并等它退出，返回退出码
/// （-1 表示进程无法启动）。
///
/// 用途：行为只在本进程**退出时**可观察（如 atexit 清理）、或结果被进程级缓存锚定
/// （如 this_process 的 app_root），进程内无从断言，只能另起一个进程跑一遍再在父进程
/// 里看退出码 / 落盘产物。子命令由被测二进制的 `main()` 自行分发。
///
/// 不用 `std::system`：Windows 上 `cmd /c` 会剥掉命令首尾引号，路径含空格时解析失败；
/// Boost.Process v2 直接 posix_spawn/CreateProcess，跨平台一致。无超时是既有取舍
/// （卡死的探针会挂住门禁），调用方须保证分发侧不会无限等待。
inline int run_self_probe(const std::vector<std::string>& args) {
    try {
        boost::asio::io_context ioc;
        boost::process::v2::process proc(ioc, dztrader::this_process::exe_path().string(), args);
        return proc.wait();
    } catch (...) {
        return -1;
    }
}

}  // namespace dztrader::test

#endif  // DZTRADER_CORE_TESTS_TEST_TEMP_DIR_H_
