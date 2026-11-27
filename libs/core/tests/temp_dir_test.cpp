#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include <gtest/gtest.h>

#include "test_temp_dir.h"

namespace {

/// 读取子进程写下的路径（未写则返回空）。
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
/// 这是 #516 的回归守卫——登记目录与注册 atexit 清理的顺序曾写反（清理回调跑在静态
/// vector 析构之后，遍历已释放的缓冲区），后果之一是目录根本删不掉。退出期是否恰好
/// 因此崩溃取决于堆复用时机（#516 的「偶发」假红），而「目录残留」是稳定可断言的形态。
TEST(TempDirTrackedTest, ExitCleanupRemovesRegisteredDir) {
    dztrader::test::ScopedTempDir sandbox("dz_temp_dir_exit_cleanup");
    const auto marker = sandbox.path() / "tracked_dir.txt";

    const int rc = dztrader::test::run_self_probe(
        {std::string(dztrader::test::kTrackedTempDirProbeFlag), marker.string()});
    ASSERT_EQ(rc, 0) << "probe 进程未正常退出（退出期崩溃会让全量 ctest 变成假红）| rc=" << rc;

    const auto tracked = read_marker(marker);
    ASSERT_FALSE(tracked.empty()) << "probe 未写出 tracked 目录路径 | marker=" << marker;

    std::error_code ec;
    const bool exists = std::filesystem::exists(tracked, ec);
    ASSERT_FALSE(static_cast<bool>(ec))
        << "无法判定 tracked 目录是否存在 | path=" << tracked << " error=" << ec.message();
    EXPECT_FALSE(exists) << "进程退出后 tracked 目录应已被 atexit 清理 | path=" << tracked;
}
