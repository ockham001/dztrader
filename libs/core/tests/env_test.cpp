#ifdef _WIN32
#include <winsock2.h>
#endif

#include <dztrader/core/env.h>
#include <dztrader/core/exception.h>

#include <boost/process/v2/environment.hpp>

#include <filesystem>
#include <string>

#include <gtest/gtest.h>

namespace bp2env = boost::process::v2::environment;

namespace env = dztrader::env;

// ---------------------------------------------------------------------------
// env::get — 存在的环境变量
// ---------------------------------------------------------------------------

TEST(EnvTest, GetExistingVar)
{
    auto val = env::get("PATH");
    EXPECT_TRUE(val.has_value());
    EXPECT_FALSE(val->empty());
}

// ---------------------------------------------------------------------------
// env::get — 不存在的环境变量
// ---------------------------------------------------------------------------

TEST(EnvTest, GetNonexistentVar)
{
    auto val = env::get("DZTRADER_NONEXISTENT_VAR_12345");
    EXPECT_FALSE(val.has_value());
}

// ---------------------------------------------------------------------------
// env::get_or — 存在时返回实际值
// ---------------------------------------------------------------------------

TEST(EnvTest, GetOrExistingVar)
{
    auto result = env::get_or("PATH", "/usr/bin");
    EXPECT_FALSE(result.empty());
    // PATH 应包含实际值而非 default
    EXPECT_NE(result, std::string("/usr/bin"));
}

// ---------------------------------------------------------------------------
// env::get_or — 不存在时返回默认值
// ---------------------------------------------------------------------------

TEST(EnvTest, GetOrDefaultValue)
{
    auto result = env::get_or("DZTRADER_NONEXISTENT_VAR_12345", "default_val");
    EXPECT_EQ(result, "default_val");
}

// ---------------------------------------------------------------------------
// env::set / env::get — 设置后可读取
// ---------------------------------------------------------------------------

TEST(EnvTest, SetAndGet)
{
    env::set("DZTRADER_TEST_VAR", "hello_world");
    auto val = env::get("DZTRADER_TEST_VAR");
    EXPECT_TRUE(val.has_value());
    EXPECT_EQ(*val, "hello_world");
    env::unset("DZTRADER_TEST_VAR");
}

// ---------------------------------------------------------------------------
// env::unset — 删除后不可读取
// ---------------------------------------------------------------------------

TEST(EnvTest, UnsetRemovesVar)
{
    env::set("DZTRADER_TEST_VAR_UNSET", "temporary");
    env::unset("DZTRADER_TEST_VAR_UNSET");
    auto val = env::get("DZTRADER_TEST_VAR_UNSET");
    EXPECT_FALSE(val.has_value());
}

// ---------------------------------------------------------------------------
// env::find_executable — 查找已知可执行文件
// ---------------------------------------------------------------------------

TEST(EnvTest, FindExecutableExists)
{
#ifdef _WIN32
    auto result = env::find_executable("cmd");
#else
    auto result = env::find_executable("ls");
#endif
    EXPECT_TRUE(result.has_value());
    EXPECT_FALSE(result->empty());
}

// ---------------------------------------------------------------------------
// env::find_executable — 查找不存在的东西
// ---------------------------------------------------------------------------

TEST(EnvTest, FindExecutableNotFound)
{
    auto result = env::find_executable("dztrader_nonexistent_exe_12345");
    EXPECT_FALSE(result.has_value());
}

// ---------------------------------------------------------------------------
// env::home_dir — 返回非空绝对路径
// ---------------------------------------------------------------------------

TEST(EnvTest, HomeDirIsAbsolute)
{
    auto home = env::home_dir();
    EXPECT_TRUE(home.is_absolute());
    EXPECT_FALSE(home.empty());
}

TEST(EnvTest, HomeDirNeverEmpty)
{
    EXPECT_FALSE(env::home_dir().empty());
}

// ---------------------------------------------------------------------------
// env::get — 空值环境变量（回归测试）
// Linux 上环境变量可以设置为空字符串，get() 应返回 optional("") 而非 nullopt
// Windows 不支持空值环境变量，此测试仅在 Linux 上验证
// ---------------------------------------------------------------------------

TEST(EnvTest, GetEmptyValueVar)
{
#ifndef _WIN32
    // Linux: 设置空值环境变量
    env::set("DZTRADER_TEST_EMPTY_VAR", "");
    auto val = env::get("DZTRADER_TEST_EMPTY_VAR");
    // 空值环境变量存在，应返回 has_value() == true，值为空字符串
    EXPECT_TRUE(val.has_value());
    EXPECT_TRUE(val->empty());
    env::unset("DZTRADER_TEST_EMPTY_VAR");
#else
    // Windows 不支持空值环境变量，跳过
    GTEST_SKIP() << "Windows does not support empty-value env vars";
#endif
}