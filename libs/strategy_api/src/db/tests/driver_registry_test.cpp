/**
 * @file driver_registry_test.cpp
 * @brief 驱动注册缝单元测试：注册/重名/空名/未知 backend 错误码、has_driver、Database::open
 * 经注册表
 */
#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

#include <dztrader/core/this_process.h>
#include <dztrader/db/database.h>

#include "../src/driver_registry.h"
#include "test_temp_dir.h"

namespace {

namespace db = dztrader::db;
namespace internal = dztrader::db::internal;

/// 注册表是进程级 static: backend 名带 pid + 进程内序号, 使 --gtest_repeat 不撞名
/// (等价于 §1.5 的 pid/随机后缀要求; 不引入仅测试可见的生产清理 API)
std::string unique_backend_name(std::string_view prefix) {
    static std::atomic<uint64_t> next{0};
    return std::string(prefix) + "_" +
           std::to_string(static_cast<uint32_t>(dztrader::this_process::pid())) + "_" +
           std::to_string(next.fetch_add(1));
}

struct FactoryProbe {
    int calls = 0;
    std::string backend;
    std::size_t option_count = 0;
};

FactoryProbe& probe() {
    static FactoryProbe instance;
    return instance;
}

/// 测试替身：只验证"查表得到工厂并被调用"，不实现任何真实语义
class StubDatabase final : public db::Database {
public:
    std::unique_ptr<db::Session> session(bool /*read_only*/) override { return nullptr; }
    [[nodiscard]] db::Capability capabilities() const noexcept override {
        return db::Capability::None;
    }
    void migrate() override {}
};

std::unique_ptr<db::Database> make_stub(const db::Config& config,
                                        std::span<const db::ResourceSchema> /*schemas*/) {
    ++probe().calls;
    probe().backend = config.backend;
    probe().option_count = config.options.size();
    return std::make_unique<StubDatabase>();
}

struct CaughtError {
    bool caught = false;
    DzErrorCode code = DZ_EC_OK;
    std::string message;
};

template <typename Fn>
CaughtError catch_error(Fn&& fn) {
    CaughtError error;
    try {
        fn();
    } catch (const dztrader::Exception& e) {
        error.caught = true;
        error.code = e.code();
        error.message = e.what();
    }
    return error;
}

}  // namespace

TEST(DriverRegistryTest, RegisterAndHasDriver) {
    const std::string backend = unique_backend_name("reg_probe");
    internal::register_driver(backend, &make_stub);
    EXPECT_TRUE(internal::has_driver(backend));
    EXPECT_FALSE(internal::has_driver(backend + "_missing"));
}

TEST(DriverRegistryTest, EmptyBackendRejected) {
    const CaughtError error = catch_error([] { internal::register_driver("", &make_stub); });
    EXPECT_TRUE(error.caught);
    EXPECT_EQ(error.code, DZ_EC_INVALID_PARAM);
}

TEST(DriverRegistryTest, DuplicateBackendRejected) {
    const std::string backend = unique_backend_name("reg_duplicate");
    internal::register_driver(backend, &make_stub);
    const CaughtError error =
        catch_error([&backend] { internal::register_driver(backend, &make_stub); });
    EXPECT_TRUE(error.caught);
    EXPECT_EQ(error.code, DZ_EC_INVALID_PARAM);
    EXPECT_EQ(error.message, "driver already registered | backend=" + backend);
}

TEST(DriverRegistryTest, NullFactoryRejected) {
    const CaughtError error =
        catch_error([] { internal::register_driver("reg_null_factory", nullptr); });
    EXPECT_TRUE(error.caught);
    EXPECT_EQ(error.code, DZ_EC_INVALID_PARAM);
}

TEST(DriverRegistryTest, EnsureRegistersBuiltinDriversIdempotently) {
    internal::ensure_builtin_drivers_registered();
    internal::ensure_builtin_drivers_registered();
    EXPECT_TRUE(internal::has_driver("sqlite"));
#if defined(DZ_WITH_MYSQL)
    EXPECT_TRUE(internal::has_driver("mysql"));
#else
    EXPECT_FALSE(internal::has_driver("mysql"));
#endif
}

TEST(DriverRegistryTest, CreateDatabaseDispatchesToRegisteredFactory) {
    const std::string backend = unique_backend_name("reg_dispatch");
    internal::register_driver(backend, &make_stub);
    probe() = FactoryProbe{};
    const db::Config config{.backend = backend, .options = {{"path", "x"}}};
    const std::unique_ptr<db::Database> database = internal::create_database(config, {});
    ASSERT_NE(database, nullptr);
    EXPECT_EQ(probe().calls, 1);
    EXPECT_EQ(probe().backend, backend);
    EXPECT_EQ(probe().option_count, 1u);
    EXPECT_EQ(database->capabilities(), db::Capability::None);
}

TEST(DriverRegistryTest, CreateDatabaseUnknownBackendRejected) {
    const db::Config config{.backend = "nope", .options = {}};
    const CaughtError error =
        catch_error([&] { static_cast<void>(internal::create_database(config, {})); });
    EXPECT_TRUE(error.caught);
    EXPECT_EQ(error.code, DZ_EC_DB_UNSUPPORTED_BACKEND);
    EXPECT_EQ(error.message, "unsupported backend: backend=nope");
}

TEST(DriverRegistryTest, DatabaseOpenUnknownBackendKeepsCodeAndMessage) {
    const db::Config config{.backend = "nope", .options = {}};
    const CaughtError error =
        catch_error([&] { static_cast<void>(db::Database::open(config, {})); });
    EXPECT_TRUE(error.caught);
    EXPECT_EQ(error.code, DZ_EC_DB_UNSUPPORTED_BACKEND);
    EXPECT_EQ(error.message, "unsupported backend: backend=nope");
}

class SqliteRegistryTest : public ::testing::Test {
protected:
    void SetUp() override { path_ = dztrader::test::unique_temp_path("dz_registry", "db"); }

    void TearDown() override {
        session_.reset();
        database_.reset();
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }

    std::filesystem::path path_;
    std::unique_ptr<db::Database> database_;
    std::unique_ptr<db::Session> session_;
};

TEST_F(SqliteRegistryTest, OpenRegisteredSqliteBackend) {
    database_ = db::Database::open(
        db::Config{.backend = "sqlite", .options = {{"path", path_.string()}}}, {});
    ASSERT_NE(database_, nullptr);
    EXPECT_TRUE(db::has(database_->capabilities(), db::Capability::SnapshotRead));
    session_ = database_->session();
    ASSERT_NE(session_, nullptr);
}

TEST_F(SqliteRegistryTest, SqliteMissingPathRejected) {
    const CaughtError error = catch_error([&] {
        static_cast<void>(db::Database::open(db::Config{.backend = "sqlite", .options = {}}, {}));
    });
    EXPECT_TRUE(error.caught);
    EXPECT_EQ(error.code, DZ_EC_DB_OPEN_FAILED);
    EXPECT_EQ(error.message, "sqlite path option is required");
}

TEST_F(SqliteRegistryTest, SqliteEmptyPathRejected) {
    const CaughtError error = catch_error([&] {
        static_cast<void>(
            db::Database::open(db::Config{.backend = "sqlite", .options = {{"path", ""}}}, {}));
    });
    EXPECT_TRUE(error.caught);
    EXPECT_EQ(error.code, DZ_EC_DB_OPEN_FAILED);
    EXPECT_EQ(error.message, "sqlite path option is required");
}
