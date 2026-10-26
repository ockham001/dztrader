#include <boost/filesystem.hpp>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <gtest/gtest.h>

namespace {

std::filesystem::path g_test_root;

/// libs/log 各测试文件共用本 test_main.cpp；其中三个通过 extern test_root() 复用进程级唯一
/// 临时根目录（约定见 build-appendix「单元测试约定」）：
/// - daily sink 按 <logger>_YYYY-MM-DD.log 落盘，固定目录名会在重复运行间残留、并行 ctest
/// 时互相干扰；
/// - 这些用例不依赖 DZTRADER_HOME，放系统临时目录，避免测试产物污染项目内 .cache/test_home；
/// - 目录名用 boost::filesystem::unique_path（std::filesystem 无等价 API），每个进程唯一。
void ensure_test_root() {
    if (!g_test_root.empty()) {
        return;
    }
    auto model = boost::filesystem::path(
        // NOLINT(temp-dir-unique): %%%% 是 boost::unique_path 的模板，下一行即展开为唯一路径
        (std::filesystem::temp_directory_path() / "dz-test-%%%%-%%%%").string());
    auto unique = boost::filesystem::unique_path(model);
    std::filesystem::create_directories(unique.string());
    g_test_root = std::filesystem::path(unique.string());
}

void cleanup_test_root() {
    if (g_test_root.empty()) {
        return;
    }
    std::error_code ec;
    std::filesystem::remove_all(g_test_root, ec);
    if (ec) {
        // atexit 阶段 RUN_ALL_TESTS 已返回，不能用 gtest 断言；记录失败但不改变测试结果
        std::fprintf(stderr, "failed to remove test root | path=\"%s\" error=\"%s\"\n",
                     g_test_root.string().c_str(), ec.message().c_str());
    }
}

}  // namespace

int main(int argc, char** argv) {
    ensure_test_root();
    std::atexit(cleanup_test_root);

    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

const std::filesystem::path& test_root() { return g_test_root; }
