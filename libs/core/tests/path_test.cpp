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

namespace {

/// 每个用例唯一临时目录（随机后缀：同一进程内连续用例、并行测试 exe 均不冲突）
std::filesystem::path unique_temp_dir(std::string_view name) {
    std::random_device rd;
    return std::filesystem::temp_directory_path() /
           (std::string(name) + "_" + std::to_string(rd()));
}

/// 临时切换 DZTRADER_HOME 并重置 paths 缓存；析构时恢复环境并再次重置。
class ScopedHome {
public:
    explicit ScopedHome(const std::filesystem::path& home)
        : orig_(dztrader::env::get("DZTRADER_HOME")) {
        dztrader::env::set("DZTRADER_HOME", home.string());
        paths::reset_for_test();
    }

    ~ScopedHome() {
        if (orig_) {
            dztrader::env::set("DZTRADER_HOME", *orig_);
        } else {
            dztrader::env::unset("DZTRADER_HOME");
        }
        paths::reset_for_test();
    }

    ScopedHome(const ScopedHome&) = delete;
    ScopedHome& operator=(const ScopedHome&) = delete;

private:
    std::optional<std::string> orig_;
};

}  // namespace

// ---------------------------------------------------------------------------
// home() — DZTRADER_HOME 已设置
// ---------------------------------------------------------------------------

TEST(PathsTest, HomeFromEnvVar) {
    const auto test_home = unique_temp_dir("dztrader_test_home");
    {
        ScopedHome guard(test_home);
        EXPECT_EQ(paths::home(), test_home);
    }
    std::filesystem::remove_all(test_home);
}

// reset_for_test(): 换环境变量后清缓存, 下一次调用按新 home 重新解析。
TEST(PathsTest, ResetForTestReResolvesHome) {
    const auto home_a = unique_temp_dir("dztrader_test_home_a");
    const auto home_b = unique_temp_dir("dztrader_test_home_b");
    {
        ScopedHome guard(home_a);
        EXPECT_EQ(paths::home(), home_a);

        dztrader::env::set("DZTRADER_HOME", home_b.string());
        paths::reset_for_test();
        EXPECT_EQ(paths::home(), home_b);
    }
    std::filesystem::remove_all(home_a);
    std::filesystem::remove_all(home_b);
}

// ---------------------------------------------------------------------------
// 子目录布局（使用 test-base 注入的 DZTRADER_HOME, 不依赖用例执行顺序）
// ---------------------------------------------------------------------------

TEST(PathsTest, HomeSubDirs) {
    EXPECT_EQ(paths::configs(), paths::home() / "configs");
    EXPECT_EQ(paths::logs(), paths::home() / "logs");
    EXPECT_EQ(paths::shm(), paths::home() / "shm");
    EXPECT_EQ(paths::cache(), paths::home() / "cache");
    EXPECT_EQ(paths::strategies(), paths::home() / "strategies");
    EXPECT_EQ(paths::db(), paths::home() / "db");
}

TEST(PathsTest, HomeIsAbsolute) { EXPECT_TRUE(paths::home().is_absolute()); }

TEST(PathsTest, SubDirsAreAbsolute) {
    EXPECT_TRUE(paths::configs().is_absolute());
    EXPECT_TRUE(paths::logs().is_absolute());
    EXPECT_TRUE(paths::shm().is_absolute());
}

TEST(PathsTest, HomeNeverEmpty) { EXPECT_FALSE(paths::home().empty()); }

TEST(PathsTest, SubDirPathsContainParent) {
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

TEST(PathsTest, CachedReferencesConsistent) {
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

TEST(PathsTest, AllSubDirsExist) {
    EXPECT_TRUE(std::filesystem::exists(paths::configs()));
    EXPECT_TRUE(std::filesystem::exists(paths::shm()));
    EXPECT_TRUE(std::filesystem::exists(paths::logs()));
    EXPECT_TRUE(std::filesystem::exists(paths::cache()));
    EXPECT_TRUE(std::filesystem::exists(paths::strategies()));
    EXPECT_TRUE(std::filesystem::exists(paths::db()));
}
