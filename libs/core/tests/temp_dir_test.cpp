#ifdef _WIN32
#include <winsock2.h>
#endif

#include <dztrader/core/this_process.h>

#include <boost/asio/io_context.hpp>
#include <boost/process/v2/process.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>

#include "test_temp_dir.h"

namespace {

/// 子进程探针模式（实现在 tests/test_main.cpp）：登记一个 tracked 临时目录、
/// 把它的路径写进 marker 文件、然后**正常 return**，让 atexit 清理有机会执行。
constexpr std::string_view kTrackedProbe = "--temp-dir-tracked-probe";

using dztrader::test::unique_temp_dir;

/// 普通临时目录 RAII（不登记 atexit）：只给父进程放 marker 文件用，退出前自己删。
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

/// 以探针模式运行本测试二进制，返回退出码（-1 表示进程无法启动）。
/// 不用 std::system：Windows 上 cmd /c 会剥掉命令首尾引号，路径含空格时解析失败。
int run_tracked_probe(const std::filesystem::path& marker) {
    try {
        boost::asio::io_context ioc;
        boost::process::v2::process proc(
            ioc, dztrader::this_process::exe_path().string(),
            std::vector<std::string>{std::string(kTrackedProbe), marker.string()});
        return proc.wait();
    } catch (...) {
        return -1;
    }
}

/// 读取子进程写下的 tracked 目录路径（未写则返回空）。
std::filesystem::path read_marker(const std::filesystem::path& marker) {
    std::ifstream input(marker, std::ios::binary);
    std::string line;
    std::getline(input, line);
    if (line.empty()) {
        return {};
    }
    return std::filesystem::path(line);
}

}  // namespace

/// 公开契约：unique_temp_dir_tracked() 登记的目录，在进程正常退出时被统一清理。
///
/// 用子进程验证而不是在本进程内直接断言：清理发生在本进程退出时，进程内无从观察。
/// 这是 #516 的回归守卫 —— 修复前 atexit 回调跑在 static vector 析构**之后**，
/// 读到已释放内存里的 path，结果既删不掉目录（实测 200/200 残留），也可能在退出期
/// 抛 std::bad_alloc 让「用例全 PASSED 的进程」以 abort 收场、把全量 ctest 判成假红。
TEST(TempDirTrackedTest, ExitCleanupRemovesRegisteredDir) {
    ScopedTempDir sandbox("dz_temp_dir_exit_cleanup");
    const auto marker = sandbox.path() / "tracked_dir.txt";

    const int rc = run_tracked_probe(marker);
    ASSERT_EQ(rc, 0) << "probe 进程未正常退出（退出期崩溃会让全量 ctest 变成假红）| rc=" << rc;

    const auto tracked = read_marker(marker);
    ASSERT_FALSE(tracked.empty()) << "probe 未写出 tracked 目录路径 | marker=" << marker;
    ASSERT_TRUE(tracked.is_absolute()) << "tracked 目录应为绝对路径 | path=" << tracked;

    std::error_code ec;
    const bool exists = std::filesystem::exists(tracked, ec);
    EXPECT_FALSE(exists) << "进程退出后 tracked 目录应已被 atexit 清理 | path=" << tracked;
}
