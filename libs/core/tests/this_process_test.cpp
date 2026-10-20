#ifdef _WIN32
#include <winsock2.h>
#endif

#include <dztrader/core/this_process.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <string_view>

#ifndef _WIN32
#include <sys/wait.h>
#endif

#include <gtest/gtest.h>

namespace this_process = dztrader::this_process;

namespace {

/// 每个探针唯一临时目录（随机后缀：同进程重复运行、并行测试 exe 均不冲突）
std::filesystem::path unique_temp_dir(std::string_view name) {
    std::random_device rd;
    return std::filesystem::temp_directory_path() /
           (std::string(name) + "_" + std::to_string(rd()));
}

/// 临时目录 RAII：断言失败提前返回时也能清理。
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

/// 把当前测试二进制复制到 <root>/a/b/tests/：
/// exe_dir 的 4 个检查目录（tests、b、a、root）全部落在 root 内，
/// 因此探针结果不受构建目录/系统临时目录中是否存在 dztraderd 影响。
std::filesystem::path copy_test_exe(const std::filesystem::path& root) {
    const auto dir = root / "a" / "b" / "tests";
    std::filesystem::create_directories(dir);
    const auto dest = dir / this_process::exe_path().filename();
    std::filesystem::copy_file(this_process::exe_path(), dest,
                               std::filesystem::copy_options::overwrite_existing);
    return dest;
}

/// 以 --app-root-probe 运行子进程，返回其退出码（-1 表示无法取得退出码）。
int run_probe(const std::filesystem::path& exe, const std::filesystem::path& expected) {
    const std::string cmd =
        "\"" + exe.string() + "\" --app-root-probe \"" + expected.string() + "\"";
    const int status = std::system(cmd.c_str());
#ifdef _WIN32
    return status;
#else
    if (status == -1 || !WIFEXITED(status)) {
        return -1;
    }
    return WEXITSTATUS(status);
#endif
}

}  // namespace

// ---------------------------------------------------------------------------
// exe_path — 返回非空绝对路径
// ---------------------------------------------------------------------------

TEST(ThisProcessTest, ExePathIsAbsolute) {
    const auto& p = this_process::exe_path();
    EXPECT_TRUE(p.is_absolute());
    EXPECT_FALSE(p.empty());
}

TEST(ThisProcessTest, ExePathExists) {
    // 可执行文件应该存在于文件系统
    std::error_code ec;
    EXPECT_TRUE(std::filesystem::exists(this_process::exe_path(), ec));
}

// ---------------------------------------------------------------------------
// exe_dir — 返回非空绝对路径，且是 exe_path 的父目录
// ---------------------------------------------------------------------------

TEST(ThisProcessTest, ExeDirIsParentOfExePath) {
    EXPECT_EQ(this_process::exe_dir(), this_process::exe_path().parent_path());
}

TEST(ThisProcessTest, ExeDirIsAbsolute) { EXPECT_TRUE(this_process::exe_dir().is_absolute()); }

// ---------------------------------------------------------------------------
// exe_name — 返回非空字符串
// ---------------------------------------------------------------------------

TEST(ThisProcessTest, ExeNameNotEmpty) { EXPECT_FALSE(this_process::exe_name().empty()); }

TEST(ThisProcessTest, ExeNameMatchesFilename) {
    EXPECT_EQ(this_process::exe_name(), this_process::exe_path().filename().string());
}

// ---------------------------------------------------------------------------
// exe_stem — 返回非空字符串，不含扩展名
// ---------------------------------------------------------------------------

TEST(ThisProcessTest, ExeStemNotEmpty) { EXPECT_FALSE(this_process::exe_stem().empty()); }

TEST(ThisProcessTest, ExeStemMatchesStem) {
    EXPECT_EQ(this_process::exe_stem(), this_process::exe_path().stem().string());
}

#ifdef _WIN32
TEST(ThisProcessTest, ExeStemNoExeSuffix) {
    // Windows 上 stem 不应包含 .exe
    EXPECT_EQ(this_process::exe_stem().find(".exe"), std::string::npos);
}
#endif

// ---------------------------------------------------------------------------
// pid — 返回正整数
// ---------------------------------------------------------------------------

TEST(ThisProcessTest, PidIsPositive) { EXPECT_GT(this_process::pid(), 0); }

// ---------------------------------------------------------------------------
// 多次调用返回同一值（缓存验证）
// ---------------------------------------------------------------------------

TEST(ThisProcessTest, CachedValuesStable) {
    const auto& path1 = this_process::exe_path();
    const auto& path2 = this_process::exe_path();
    EXPECT_EQ(&path1, &path2);  // 同一引用

    const auto& dir1 = this_process::exe_dir();
    const auto& dir2 = this_process::exe_dir();
    EXPECT_EQ(&dir1, &dir2);

    // exe_name 和 exe_stem 也应返回同一引用
    const auto& name1 = this_process::exe_name();
    const auto& name2 = this_process::exe_name();
    EXPECT_EQ(&name1, &name2);

    const auto& stem1 = this_process::exe_stem();
    const auto& stem2 = this_process::exe_stem();
    EXPECT_EQ(&stem1, &stem2);
}

// ---------------------------------------------------------------------------
// exe_dir 是 exe_path 的直接子关系
// ---------------------------------------------------------------------------

TEST(ThisProcessTest, ExeDirContainsExePath) {
    // exe_path 应在 exe_dir 目录下
    const auto& exe = this_process::exe_path();
    const auto& dir = this_process::exe_dir();
    EXPECT_TRUE(exe.string().find(dir.string()) == 0);
}

// ---------------------------------------------------------------------------
// app_root — 从 exe_dir 向上最多 3 层查找 dztraderd[.exe]
//
// app_root() 是 magic static 且锚定 exe_dir()，同一进程内无法切换 exe 位置。
// 两条分支通过把测试二进制复制到临时布局、以 --app-root-probe 运行子进程覆盖；
// 检查的 4 个目录全部位于临时 root 内，不依赖真实仓库/构建目录。
// ---------------------------------------------------------------------------

TEST(ThisProcessTest, AppRootFoundInAncestor) {
    ScopedTempDir root("dztrader_app_root_found");
    const auto exe = copy_test_exe(root.path());
#ifdef _WIN32
    std::ofstream(root.path() / "dztraderd.exe").close();
#else
    std::ofstream(root.path() / "dztraderd").close();
#endif

    EXPECT_EQ(run_probe(exe, root.path()), 0);
}

TEST(ThisProcessTest, AppRootNotFoundThrows) {
    ScopedTempDir root("dztrader_app_root_missing");
    const auto exe = copy_test_exe(root.path());

    EXPECT_EQ(run_probe(exe, std::filesystem::path("-")), 0);
}
