#ifdef _WIN32
#include <winsock2.h>
#endif

#include <dztrader/core/env.h>
#include <dztrader/core/exception.h>
#include <dztrader/core/path.h>
#include <dztrader/error.h>

#include <filesystem>
#include <optional>
#include <random>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

namespace paths = dztrader::paths;
namespace env = dztrader::env;

namespace {

/// 每个用例唯一临时目录（随机后缀：同一进程内连续用例、并行测试 exe 均不冲突）
std::filesystem::path unique_temp_dir(std::string_view name) {
    std::random_device rd;
    return std::filesystem::temp_directory_path() /
           (std::string(name) + "_" + std::to_string(rd()));
}

}  // namespace

/// 每个用例独占一个 DZTRADER_HOME；TearDown 恢复 test-base 注入的原值并清缓存。
///
/// paths::* 的缓存在 path.cpp 内由 g_cache 持有（不再使用 magic static），
/// reset_for_test() 是唯一的清缓存入口。用例不再依赖执行顺序，也不再依赖
/// test-base 注入值作为被测输入。
class PathsTest : public ::testing::Test {
protected:
    void SetUp() override {
        dztrader_home_ = env::get("DZTRADER_HOME");
#ifdef _WIN32
        os_home_key_ = "USERPROFILE";
#else
        os_home_key_ = "HOME";
#endif
        os_home_ = env::get(os_home_key_);

        test_home_ = unique_temp_dir("dztrader_test_home");
        env::set("DZTRADER_HOME", test_home_.string());
        paths::reset_for_test();
    }

    void TearDown() override {
        paths::reset_for_test();
        restore_env("DZTRADER_HOME", dztrader_home_);
        restore_env(os_home_key_, os_home_);
        std::error_code ec;
        std::filesystem::remove_all(test_home_, ec);
    }

    static void restore_env(std::string_view key, const std::optional<std::string>& value) {
        if (value.has_value()) {
            env::set(key, *value);
        } else {
            env::unset(key);
        }
    }

    std::optional<std::string> dztrader_home_;
    std::optional<std::string> os_home_;
    std::string os_home_key_;
    std::filesystem::path test_home_;
};

// ---------------------------------------------------------------------------
// home() — DZTRADER_HOME 已设置
// ---------------------------------------------------------------------------

TEST_F(PathsTest, HomeFromEnvVar) {
    EXPECT_EQ(paths::home(), test_home_);
    EXPECT_TRUE(std::filesystem::exists(test_home_));
}

// ---------------------------------------------------------------------------
// home() — DZTRADER_HOME 是相对路径时归一化为绝对路径
// ---------------------------------------------------------------------------

TEST_F(PathsTest, RelativeHomeIsNormalized) {
    const auto relative =
        std::filesystem::path(unique_temp_dir("dztrader_test_rel_home").filename());
    const auto expected = std::filesystem::absolute(relative);

    env::set("DZTRADER_HOME", relative.string());
    paths::reset_for_test();

    EXPECT_TRUE(paths::home().is_absolute());
    EXPECT_EQ(paths::home(), expected);

    std::error_code ec;
    std::filesystem::remove_all(expected, ec);
}

// ---------------------------------------------------------------------------
// home() — 未设置 DZTRADER_HOME 时回退到 $HOME/.dztrader
// ---------------------------------------------------------------------------

TEST_F(PathsTest, FallsBackToOsHomeWhenEnvUnset) {
    const auto fake_os_home = unique_temp_dir("dztrader_test_os_home");
    env::unset("DZTRADER_HOME");
    env::set(os_home_key_, fake_os_home.string());
    paths::reset_for_test();

    EXPECT_EQ(paths::home(), fake_os_home / ".dztrader");
    EXPECT_TRUE(std::filesystem::exists(fake_os_home / ".dztrader"));

    std::error_code ec;
    std::filesystem::remove_all(fake_os_home, ec);
}

// reset_for_test(): 换环境变量后清缓存, 下一次调用按新 home 重新解析。
TEST_F(PathsTest, ResetForTestReResolvesHome) {
    const auto home_b = unique_temp_dir("dztrader_test_home_b");
    EXPECT_EQ(paths::home(), test_home_);

    env::set("DZTRADER_HOME", home_b.string());
    paths::reset_for_test();
    EXPECT_EQ(paths::home(), home_b);

    std::error_code ec;
    std::filesystem::remove_all(home_b, ec);
}

// ---------------------------------------------------------------------------
// 子目录布局
// ---------------------------------------------------------------------------

TEST_F(PathsTest, HomeSubDirs) {
    EXPECT_EQ(paths::configs(), paths::home() / "configs");
    EXPECT_EQ(paths::logs(), paths::home() / "logs");
    EXPECT_EQ(paths::shm(), paths::home() / "shm");
    EXPECT_EQ(paths::cache(), paths::home() / "cache");
    EXPECT_EQ(paths::strategies(), paths::home() / "strategies");
    EXPECT_EQ(paths::db(), paths::home() / "db");
}

TEST_F(PathsTest, HomeIsAbsolute) { EXPECT_TRUE(paths::home().is_absolute()); }

TEST_F(PathsTest, SubDirsAreAbsolute) {
    EXPECT_TRUE(paths::configs().is_absolute());
    EXPECT_TRUE(paths::logs().is_absolute());
    EXPECT_TRUE(paths::shm().is_absolute());
}

TEST_F(PathsTest, HomeNeverEmpty) { EXPECT_FALSE(paths::home().empty()); }

TEST_F(PathsTest, SubDirPathsContainParent) {
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
// 缓存一致性：多次调用返回同一引用
// ---------------------------------------------------------------------------

TEST_F(PathsTest, CachedReferencesConsistent) {
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

TEST_F(PathsTest, AllSubDirsExist) {
    EXPECT_TRUE(std::filesystem::exists(paths::configs()));
    EXPECT_TRUE(std::filesystem::exists(paths::shm()));
    EXPECT_TRUE(std::filesystem::exists(paths::logs()));
    EXPECT_TRUE(std::filesystem::exists(paths::cache()));
    EXPECT_TRUE(std::filesystem::exists(paths::strategies()));
    EXPECT_TRUE(std::filesystem::exists(paths::db()));
}
