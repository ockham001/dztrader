/**
 * @file exe_scanner_test.cpp
 * @brief libs/process 单元测试: scan_all_exes + find_exe_by_stem
 *
 * 跨平台测试 fixture:
 * - Windows: 用 .exe 扩展名造可执行文件
 * - Linux: 用 chmod +x 造可执行文件
 * - 测试目录: std::filesystem::temp_directory_path() / "dzprocess_test_<pid>_<random>"
 */

#include <dztrader/core/this_process.h>
#include <dztrader/process/exe_scanner.h>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

namespace dztrader::process {
namespace {

/// 跨平台创建指定文件名的可执行文件
/// Windows: 文件名原样 (扩展名规则由被测实现判定)
/// Linux: 文件名原样 + owner_exec 权限位
std::filesystem::path make_exec_file(const std::filesystem::path& dir,
                                     const std::string& filename) {
    std::filesystem::create_directories(dir);
    auto path = dir / filename;
    std::ofstream ofs(path);
    ofs << "stub";
    ofs.close();
#ifndef _WIN32
    std::filesystem::permissions(
        path, std::filesystem::perms::owner_exec | std::filesystem::perms::owner_read,
        std::filesystem::perm_options::add);
#endif
    return path;
}

/// 跨平台创建测试用可执行文件
/// Windows: 创建 name.exe 空文件
/// Linux: 创建 name 空文件并 chmod +x
std::filesystem::path make_test_exe(const std::filesystem::path& dir, const std::string& name) {
#ifdef _WIN32
    return make_exec_file(dir, name + ".exe");
#else
    return make_exec_file(dir, name);
#endif
}

/// 创建非可执行文件
/// Windows: .pdb/.dll 等非执行扩展名
/// Linux: 显式去掉 exec 权限位
std::filesystem::path make_non_exec_file(const std::filesystem::path& dir,
                                         const std::string& name_with_ext) {
    std::filesystem::create_directories(dir);
    auto path = dir / name_with_ext;
    std::ofstream ofs(path);
    ofs << "non-exec";
    ofs.close();
#ifndef _WIN32
    std::filesystem::permissions(path,
                                 std::filesystem::perms::owner_exec |
                                     std::filesystem::perms::group_exec |
                                     std::filesystem::perms::others_exec,
                                 std::filesystem::perm_options::remove);
#endif
    return path;
}

class ExeScannerTest : public ::testing::Test {
protected:
    void SetUp() override {
        // 唯一临时目录: pid + 随机后缀, 多进程 (含多 worktree) 并行不冲突
        // 旧实现用未播种 std::rand(), 每个进程每次运行都是同一个名字, 残留目录会致失败
        std::random_device rd;
        tmp_root_ = std::filesystem::temp_directory_path() /
                    ("dzprocess_test_" + std::to_string(dztrader::this_process::pid()) + "_" +
                     std::to_string(rd()));
        std::filesystem::create_directories(tmp_root_);
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(tmp_root_, ec);
    }

    std::filesystem::path tmp_root_;
};

// =========================================================================
// scan_all_exes 测试
// =========================================================================

TEST_F(ExeScannerTest, EmptyDirectoryReturnsEmpty) {
    auto result = scan_all_exes(tmp_root_);
    EXPECT_TRUE(result.empty());
}

TEST_F(ExeScannerTest, ScanNonexistentRootReturnsEmpty) {
    // 头文件承诺 IO 错误不抛异常: root 不存在时 WARN + 返回空
    auto result = scan_all_exes(tmp_root_ / "nonexistent_root");
    EXPECT_EQ(result.size(), 0u);
}

TEST_F(ExeScannerTest, FindByNameNonexistentRootReturnsNullopt) {
    auto result = find_exe_by_stem("dzmd_ctp", tmp_root_ / "nonexistent_root");
    EXPECT_FALSE(result.has_value());
}

TEST_F(ExeScannerTest, FindsExeInRootDir) {
    make_test_exe(tmp_root_, "dzmd_ctp");

    auto result = scan_all_exes(tmp_root_);
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0].name, "dzmd_ctp");
    EXPECT_EQ(result[0].kind, ProcessKind::GatewayMd);
    // 用 stem 断言, 跨平台兼容 (Windows: dzmd_ctp.exe -> stem "dzmd_ctp", Linux: dzmd_ctp -> stem
    // "dzmd_ctp")
    EXPECT_EQ(result[0].exe.stem().string(), "dzmd_ctp");
    EXPECT_EQ(result[0].start_dir, tmp_root_);
}

TEST_F(ExeScannerTest, FindsExeInSubDir) {
    auto subdir = tmp_root_ / "ctp";
    make_test_exe(subdir, "dzmd_ctp");

    auto result = scan_all_exes(tmp_root_);
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0].name, "dzmd_ctp");
    EXPECT_EQ(result[0].exe.parent_path(), subdir);
}

TEST_F(ExeScannerTest, IgnoresThirdLevelDir) {
    // 扫描规则: root + 一级子目录, 不递归; root/sub/deep 下的可执行文件必须不可见
    make_test_exe(tmp_root_ / "sub" / "deep", "dzmd_ctp");

    auto result = scan_all_exes(tmp_root_);
    EXPECT_EQ(result.size(), 0u);
    EXPECT_FALSE(find_exe_by_stem("dzmd_ctp", tmp_root_).has_value());
}

TEST_F(ExeScannerTest, RootDirPriorityOverSubDir) {
    // 根目录和子目录都有 dzmd_ctp -> 只返回根目录的
    auto root_exe = make_test_exe(tmp_root_, "dzmd_ctp");
    auto subdir = tmp_root_ / "ctp";
    make_test_exe(subdir, "dzmd_ctp");

    auto result = scan_all_exes(tmp_root_);
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0].exe, root_exe);
}

TEST_F(ExeScannerTest, IdentifiesAllPrefixes) {
    make_test_exe(tmp_root_, "dzmd_ctp");
    make_test_exe(tmp_root_, "dztd_ctp");
    make_test_exe(tmp_root_, "dzweb");

    auto result = scan_all_exes(tmp_root_);
    ASSERT_EQ(result.size(), 3u);
    // 按 name 字典序: dzmd_ctp < dztd_ctp < dzweb
    EXPECT_EQ(result[0].name, "dzmd_ctp");
    EXPECT_EQ(result[0].kind, ProcessKind::GatewayMd);
    EXPECT_EQ(result[1].name, "dztd_ctp");
    EXPECT_EQ(result[1].kind, ProcessKind::GatewayTd);
    EXPECT_EQ(result[2].name, "dzweb");
    EXPECT_EQ(result[2].kind, ProcessKind::WebUI);
}

TEST_F(ExeScannerTest, RejectsExecutableNonPrefixFiles) {
    // 可执行但不是 dzmd_/dztd_/dzweb 前缀 -> 一律跳过
    make_test_exe(tmp_root_, "dztraderd");
    make_test_exe(tmp_root_, "dzweb2");

    auto result = scan_all_exes(tmp_root_);
    EXPECT_EQ(result.size(), 0u);
}

TEST_F(ExeScannerTest, SkipsNonExecFiles) {
#ifdef _WIN32
    make_non_exec_file(tmp_root_, "dzmd_ctp.pdb");
    make_non_exec_file(tmp_root_, "dzmd_ctp.dll");
#else
    // Linux: 创建 dzmd_ctp 文件但无 exec 权限
    make_non_exec_file(tmp_root_, "dzmd_ctp");
#endif
    auto result = scan_all_exes(tmp_root_);
    EXPECT_TRUE(result.empty());
}

TEST_F(ExeScannerTest, ExtensionRulesFollowPlatform) {
#ifdef _WIN32
    // Windows: .exe/.bat/.cmd 均为可执行扩展名 (.exe 已由 FindsExeInRootDir 覆盖)
    auto bat = make_exec_file(tmp_root_, "dzmd_ctp.bat");
    auto cmd = make_exec_file(tmp_root_, "dztd_ctp.cmd");

    auto result = scan_all_exes(tmp_root_);
    ASSERT_EQ(result.size(), 2u);
    EXPECT_EQ(result[0].name, "dzmd_ctp");
    EXPECT_EQ(result[0].kind, ProcessKind::GatewayMd);
    EXPECT_EQ(result[0].exe, bat);
    EXPECT_EQ(result[1].name, "dztd_ctp");
    EXPECT_EQ(result[1].kind, ProcessKind::GatewayTd);
    EXPECT_EQ(result[1].exe, cmd);
#else
    // Linux: 判定依据是 owner_exec 权限位, 与扩展名无关
    // 带 exec 位的 .bat 被接受 (stem 剥离扩展名), 无 exec 位的 .cmd 被拒绝
    auto bat = make_exec_file(tmp_root_, "dzmd_ctp.bat");
    make_non_exec_file(tmp_root_, "dztd_ctp.cmd");

    auto result = scan_all_exes(tmp_root_);
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0].name, "dzmd_ctp");
    EXPECT_EQ(result[0].kind, ProcessKind::GatewayMd);
    EXPECT_EQ(result[0].exe, bat);
#endif
}

TEST_F(ExeScannerTest, RejectsDegenerateStem) {
    // "dzmd_" 长度 = 5, 不满足 > 5
    make_test_exe(tmp_root_, "dzmd_");

    auto result = scan_all_exes(tmp_root_);
    EXPECT_TRUE(result.empty());
}

TEST_F(ExeScannerTest, ResultSortedByName) {
    // 故意乱序创建, 验证返回结果按 name 排序
    make_test_exe(tmp_root_, "dztd_ctp");
    make_test_exe(tmp_root_, "dzweb");
    make_test_exe(tmp_root_, "dzmd_ctp");

    auto result = scan_all_exes(tmp_root_);
    ASSERT_EQ(result.size(), 3u);
    EXPECT_EQ(result[0].name, "dzmd_ctp");
    EXPECT_EQ(result[1].name, "dztd_ctp");
    EXPECT_EQ(result[2].name, "dzweb");
}

// =========================================================================
// find_exe_by_stem 测试
// =========================================================================

TEST_F(ExeScannerTest, FindByNameReturnsInfo) {
    make_test_exe(tmp_root_, "dzmd_ctp");

    auto result = find_exe_by_stem("dzmd_ctp", tmp_root_);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->name, "dzmd_ctp");
    EXPECT_EQ(result->kind, ProcessKind::GatewayMd);
    EXPECT_EQ(result->exe.parent_path(), tmp_root_);
}

TEST_F(ExeScannerTest, FindByNameNotFoundReturnsNullopt) {
    make_test_exe(tmp_root_, "dzmd_ctp");

    auto result = find_exe_by_stem("dzmd_nonexistent", tmp_root_);
    EXPECT_FALSE(result.has_value());
}

TEST_F(ExeScannerTest, FindByNamePrefersRootDir) {
    // 根目录和子目录都有 dzmd_ctp -> 返回根目录的
    auto root_exe = make_test_exe(tmp_root_, "dzmd_ctp");
    auto subdir = tmp_root_ / "ctp";
    make_test_exe(subdir, "dzmd_ctp");

    auto result = find_exe_by_stem("dzmd_ctp", tmp_root_);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->exe, root_exe);
}

TEST_F(ExeScannerTest, FindByNameFindsInSubDir) {
    auto subdir = tmp_root_ / "ctp";
    make_test_exe(subdir, "dzmd_ctp");

    auto result = find_exe_by_stem("dzmd_ctp", tmp_root_);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->exe.parent_path(), subdir);
}

}  // namespace
}  // namespace dztrader::process
