#include <dztrader/core/this_process.h>

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <system_error>

#include <gtest/gtest.h>

#include "test_temp_dir.h"

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

/// 子进程探针：登记一个 tracked 临时目录，把路径写进 marker，然后正常 return。
///
/// 退出期清理只能在本进程退出时观察，进程内断言看不到，所以由 temp_dir_test 以子进程
/// 方式运行本模式（#516）：父进程据此断言「正常退出」+「目录已被 atexit 删掉」。
/// 退出期若因静态析构顺序读已释放内存而 abort，退出码非 0，同样会被父进程抓到。
int run_tracked_temp_dir_probe(std::string_view marker) {
    const auto dir = dztrader::test::unique_temp_dir_tracked("dz_temp_dir_probe");
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        return 65;
    }
    std::ofstream output(std::filesystem::path(marker), std::ios::binary | std::ios::trunc);
    output << dir.string() << '\n';
    output.close();
    if (!output) {
        return 66;
    }
    return 0;
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
        if (std::string_view(argv[i]) == "--temp-dir-tracked-probe") {
            if (i + 1 >= argc) {
                return 64;  // 缺少 marker 路径
            }
            return run_tracked_temp_dir_probe(argv[i + 1]);
        }
    }

    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
