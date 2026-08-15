#ifdef _WIN32
#include <winsock2.h>
#endif

#include <dztrader/core/exception.h>
#include <dztrader/core/path.h>
#include <dztrader/error.h>

#include <boost/process/v2/environment.hpp>

#include <filesystem>
#include <string>

#include <gtest/gtest.h>

namespace bp2env = boost::process::v2::environment;
namespace paths = dztrader::paths;

// ---------------------------------------------------------------------------
// home() — DZTRADER_HOME 已设置
// 使用跨平台临时目录作为测试路径
// ---------------------------------------------------------------------------

TEST(PathsTest, HomeFromEnvVar)
{
    const auto test_home = std::filesystem::temp_directory_path() / "dztrader_test_home";
    bp2env::set("DZTRADER_HOME", test_home.string().c_str());
    EXPECT_EQ(paths::home(), test_home);
    bp2env::unset("DZTRADER_HOME");
}

// 注意：paths::home() 使用 magic static，首次调用后缓存。
// 以下测试假设测试运行顺序：先 HomeFromEnvVar（设置 DZTRADER_HOME），
// 再 HomeSubDirs。由于 magic static 只初始化一次，后续测试无法重新触发。

TEST(PathsTest, HomeSubDirs)
{
    // home() 已在 HomeFromEnvVar 中初始化
    EXPECT_EQ(paths::configs(), paths::home() / "configs");
    EXPECT_EQ(paths::logs(), paths::home() / "logs");
    EXPECT_EQ(paths::shm(), paths::home() / "shm");
    EXPECT_EQ(paths::cache(), paths::home() / "cache");
    EXPECT_EQ(paths::strategies(), paths::home() / "strategies");
    EXPECT_EQ(paths::db(), paths::home() / "db");
}

TEST(PathsTest, HomeIsAbsolute)
{
    EXPECT_TRUE(paths::home().is_absolute());
}

TEST(PathsTest, SubDirsAreAbsolute)
{
    EXPECT_TRUE(paths::configs().is_absolute());
    EXPECT_TRUE(paths::logs().is_absolute());
    EXPECT_TRUE(paths::shm().is_absolute());
}

TEST(PathsTest, HomeNeverEmpty)
{
    EXPECT_FALSE(paths::home().empty());
}

TEST(PathsTest, SubDirPathsContainParent)
{
    // 所有子目录应以 home() 为前缀
    const auto h = paths::home().string();
    EXPECT_TRUE(paths::configs().string().find(h) == 0);
    EXPECT_TRUE(paths::logs().string().find(h) == 0);
    EXPECT_TRUE(paths::shm().string().find(h) == 0);
    EXPECT_TRUE(paths::cache().string().find(h) == 0);
    EXPECT_TRUE(paths::strategies().string().find(h) == 0);
    EXPECT_TRUE(paths::db().string().find(h) == 0);
}

// ---------------------------------------------------------------------------
// 缓存一致性：多次调用返回同一引用（magic static）
// ---------------------------------------------------------------------------

TEST(PathsTest, CachedReferencesConsistent)
{
    // home() 已初始化，多次调用应返回同一引用
    const auto& h1 = paths::home();
    const auto& h2 = paths::home();
    EXPECT_EQ(&h1, &h2);

    const auto& c1 = paths::configs();
    const auto& c2 = paths::configs();
    EXPECT_EQ(&c1, &c2);

    const auto& l1 = paths::logs();
    const auto& l2 = paths::logs();
    EXPECT_EQ(&l1, &l2);
}

// ---------------------------------------------------------------------------
// 所有子目录均已创建且可访问
// ---------------------------------------------------------------------------

TEST(PathsTest, AllSubDirsExist)
{
    EXPECT_TRUE(std::filesystem::exists(paths::configs()));
    EXPECT_TRUE(std::filesystem::exists(paths::shm()));
    EXPECT_TRUE(std::filesystem::exists(paths::logs()));
    EXPECT_TRUE(std::filesystem::exists(paths::cache()));
    EXPECT_TRUE(std::filesystem::exists(paths::strategies()));
    EXPECT_TRUE(std::filesystem::exists(paths::db()));
}