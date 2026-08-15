#ifdef _WIN32
#include <winsock2.h>
#endif

#include <dztrader/core/this_process.h>

#include <filesystem>
#include <string>

#include <gtest/gtest.h>

namespace this_process = dztrader::this_process;

// ---------------------------------------------------------------------------
// exe_path — 返回非空绝对路径
// ---------------------------------------------------------------------------

TEST(ThisProcessTest, ExePathIsAbsolute)
{
    const auto& p = this_process::exe_path();
    EXPECT_TRUE(p.is_absolute());
    EXPECT_FALSE(p.empty());
}

TEST(ThisProcessTest, ExePathExists)
{
    // 可执行文件应该存在于文件系统
    std::error_code ec;
    EXPECT_TRUE(std::filesystem::exists(this_process::exe_path(), ec));
}

// ---------------------------------------------------------------------------
// exe_dir — 返回非空绝对路径，且是 exe_path 的父目录
// ---------------------------------------------------------------------------

TEST(ThisProcessTest, ExeDirIsParentOfExePath)
{
    EXPECT_EQ(this_process::exe_dir(), this_process::exe_path().parent_path());
}

TEST(ThisProcessTest, ExeDirIsAbsolute)
{
    EXPECT_TRUE(this_process::exe_dir().is_absolute());
}

// ---------------------------------------------------------------------------
// exe_name — 返回非空字符串
// ---------------------------------------------------------------------------

TEST(ThisProcessTest, ExeNameNotEmpty)
{
    EXPECT_FALSE(this_process::exe_name().empty());
}

TEST(ThisProcessTest, ExeNameMatchesFilename)
{
    EXPECT_EQ(this_process::exe_name(),
              this_process::exe_path().filename().string());
}

// ---------------------------------------------------------------------------
// exe_stem — 返回非空字符串，不含扩展名
// ---------------------------------------------------------------------------

TEST(ThisProcessTest, ExeStemNotEmpty)
{
    EXPECT_FALSE(this_process::exe_stem().empty());
}

TEST(ThisProcessTest, ExeStemMatchesStem)
{
    EXPECT_EQ(this_process::exe_stem(),
              this_process::exe_path().stem().string());
}

#ifdef _WIN32
TEST(ThisProcessTest, ExeStemNoExeSuffix)
{
    // Windows 上 stem 不应包含 .exe
    EXPECT_EQ(this_process::exe_stem().find(".exe"),
              std::string::npos);
}
#endif

// ---------------------------------------------------------------------------
// pid — 返回正整数
// ---------------------------------------------------------------------------

TEST(ThisProcessTest, PidIsPositive)
{
    EXPECT_GT(this_process::pid(), 0);
}

// ---------------------------------------------------------------------------
// 多次调用返回同一值（缓存验证）
// ---------------------------------------------------------------------------

TEST(ThisProcessTest, CachedValuesStable)
{
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

TEST(ThisProcessTest, ExeDirContainsExePath)
{
    // exe_path 应在 exe_dir 目录下
    const auto& exe = this_process::exe_path();
    const auto& dir = this_process::exe_dir();
    EXPECT_TRUE(exe.string().find(dir.string()) == 0);
}