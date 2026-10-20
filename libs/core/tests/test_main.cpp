#include <dztrader/core/this_process.h>

#include <filesystem>
#include <stdexcept>
#include <string_view>
#include <system_error>

#include <gtest/gtest.h>

namespace {

/// 子进程探针：在隔离的目录布局中验证 app_root() 的公开行为。
///
/// 用法: <test_exe> --app-root-probe <expected-root|->
///   expected-root: 期望 app_root() 返回该目录
///   -            : 期望 app_root() 抛 std::runtime_error
/// 返回 0 表示符合预期。
///
/// app_root() 依赖 exe_dir() 且结果被 magic static 缓存，同一进程内无法切换
/// exe 所在目录；this_process_test 把测试二进制复制到临时布局后以本模式运行子进程，
/// 从而同时覆盖「找到」与「找不到」两条分支。
int run_app_root_probe(std::string_view expected) {
    try {
        const auto& root = dztrader::this_process::app_root();
        if (expected == "-") {
            return 1;  // 期望抛异常，实际找到了
        }
        std::error_code ec;
        const bool same = std::filesystem::equivalent(root, std::filesystem::path(expected), ec);
        return (!ec && same) ? 0 : 2;
    } catch (const std::runtime_error&) {
        return expected == "-" ? 0 : 3;
    } catch (...) {
        return 4;
    }
}

}  // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--app-root-probe") {
            if (i + 1 >= argc) {
                return 64;  // 缺少期望参数
            }
            return run_app_root_probe(argv[i + 1]);
        }
    }

    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
