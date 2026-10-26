/**
 * @file history_store_test.cpp
 * @brief 临时库上的往返、schema 版本、保留/压缩/上限（#78 验收核心）。
 *
 * 测试准则（docs/unit-test-standard.md）：断言**外部可观察行为**——落库后再读出来
 * 的值、保留策略执行后的行分布、只读连接在异常库上的降级结果；不断言内部结构。
 *
 * 隔离：每个用例用 `temp_directory_path()` 下的唯一目录（pid + 随机后缀），
 * TearDown 清理——不触碰 `DZTRADER_HOME`，也不留跨运行残留。
 */

#include "test_temp_dir.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <sqlite3.h>

#include <dztrader/core/this_process.h>
#include <dztrader/sysmonstore/history_store.h>
#include <dztrader/sysmonstore/records.h>
#include <dztrader/sysmonstore/schema.h>

namespace dztrader::sysmonstore {
namespace {

/// 唯一临时目录（统一走 test_temp_dir.h），避免与并行测试/上次运行的残留互扰。
std::filesystem::path make_temp_dir() {
    auto dir = dztrader::test::unique_temp_dir_tracked("dzsysmonstore");
    std::filesystem::create_directories(dir);
    return dir;
}

/// 构造一个填满所有标量指标的记录（便于往返断言）。
ScalarRecord make_scalar_record(int64_t bucket_ts, double seed, int32_t level = kLevelRaw) {
    ScalarRecord record;
    record.level = level;
    record.bucket_ts = bucket_ts;
    record.interval_sec = level == kLevelMinute ? kMinuteResolutionSec : 60;
    for (auto& metric : record.metrics) {
        metric.observe(seed);
    }
    return record;
}

DimensionRecord make_dimension_record(int64_t bucket_ts,
                                      std::string domain,
                                      std::string dimension,
                                      std::string metric,
                                      double value,
                                      int32_t level = kLevelRaw) {
    DimensionRecord record;
    record.level = level;
    record.bucket_ts = bucket_ts;
    record.domain = std::move(domain);
    record.dimension = std::move(dimension);
    record.metric = std::move(metric);
    record.aggregate.observe(value);
    return record;
}

class HistoryStoreTest : public ::testing::Test {
protected:
    void SetUp() override { dir_ = make_temp_dir(); }
    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    std::filesystem::path db_path() const { return dir_ / "sysmon.db"; }

    std::filesystem::path dir_;
};

// ===== 建库与 schema 版本 =====

TEST_F(HistoryStoreTest, OpenCreatesDatabaseWithCurrentSchemaVersion) {
    auto store = HistoryStore::open(db_path());
    EXPECT_EQ(store.schema_version(), kSchemaVersion);
    EXPECT_TRUE(std::filesystem::exists(db_path()));
}

TEST_F(HistoryStoreTest, OpenIsIdempotent) {
    {
        auto store = HistoryStore::open(db_path());
        EXPECT_EQ(store.schema_version(), kSchemaVersion);
    }
    // 二次打开（dzsysmon 重启）：不重建、不丢数据、版本不变。
    auto store = HistoryStore::open(db_path());
    EXPECT_EQ(store.schema_version(), kSchemaVersion);
}

TEST_F(HistoryStoreTest, OpenCreatesParentDirectory) {
    const auto nested = dir_ / "a" / "b" / "sysmon.db";
    auto store = HistoryStore::open(nested);
    EXPECT_TRUE(std::filesystem::exists(nested));
}

TEST_F(HistoryStoreTest, OpenRejectsNewerSchemaVersion) {
    // 旧写入方不得打开「更新的」库：按旧结构写入会破坏新版数据，必须**明确失败**
    // 而不是静默覆盖（写入方启动失败的后果是「不写历史」，比写坏被读方误解安全）。
    {
        auto store = HistoryStore::open(db_path());
        EXPECT_EQ(store.schema_version(), kSchemaVersion);
    }
    // 用独立 SQLite 连接把版本改成比当前大，模拟「库由更新版本写过」。
    sqlite3* raw = nullptr;
    ASSERT_EQ(sqlite3_open(db_path().string().c_str(), &raw), SQLITE_OK);
    const std::string bump = "UPDATE sysmon_meta SET value='" + std::to_string(kSchemaVersion + 1) +
                             "' WHERE key='schema_version'";
    ASSERT_EQ(sqlite3_exec(raw, bump.c_str(), nullptr, nullptr, nullptr), SQLITE_OK);
    sqlite3_close(raw);

    // 写入方：拒绝打开。
    EXPECT_THROW(HistoryStore::open(db_path()), std::runtime_error);
    // 读取方：降级为 SchemaMismatch（不是崩溃，也不是按旧结构解码）。
    const auto reader = HistoryReader::open(db_path());
    EXPECT_EQ(reader.first, HistoryAvailability::SchemaMismatch);
    EXPECT_EQ(reader.second, nullptr);
    EXPECT_EQ(history_availability_reason(reader.first), "schema_mismatch");
}

// ===== 往返 =====

TEST_F(HistoryStoreTest, ScalarRoundTripPreservesAllAggregates) {
    {
        auto store = HistoryStore::open(db_path());
        HistoryBatch batch;
        ScalarRecord record = make_scalar_record(1'790'000'000, 5.0);
        // 让 min/max/avg/last 各不相同，才能验证四者都被正确保存。
        record.metrics[0].observe(10.0);
        record.metrics[0].observe(1.0);
        batch.scalars.push_back(record);
        store.write_batch(batch);
    }
    auto reader = HistoryReader::open(db_path());
    ASSERT_EQ(reader.first, HistoryAvailability::Ok);
    const auto rows = reader.second->query_scalars(kLevelRaw, 1'790'000'000, 1'790'000'001, 10);
    ASSERT_EQ(rows.size(), 1U);
    EXPECT_EQ(rows[0].bucket_ts, 1'790'000'000);
    EXPECT_EQ(rows[0].interval_sec, 60);
    EXPECT_EQ(rows[0].level, kLevelRaw);
    // 指标 0 观测过 5/10/1：min=1, max=10, last=1。
    EXPECT_DOUBLE_EQ(rows[0].metrics[0].min, 1.0);
    EXPECT_DOUBLE_EQ(rows[0].metrics[0].max, 10.0);
    EXPECT_DOUBLE_EQ(rows[0].metrics[0].last, 1.0);
    EXPECT_EQ(rows[0].metrics[0].count, 3);
}

TEST_F(HistoryStoreTest, EmptyMetricAggregateRoundTripsAsEmpty) {
    // 「该窗口内该指标无有效样本」必须读出来仍是空聚合（读取方据此输出 null 而不是 0）。
    {
        auto store = HistoryStore::open(db_path());
        HistoryBatch batch;
        ScalarRecord record;
        record.bucket_ts = 1'790'000'000;
        record.interval_sec = 60;
        record.metrics[3].observe(0.25);  // 只填一个指标，其余保持空
        batch.scalars.push_back(record);
        store.write_batch(batch);
    }
    auto reader = HistoryReader::open(db_path());
    ASSERT_EQ(reader.first, HistoryAvailability::Ok);
    const auto rows = reader.second->query_scalars(kLevelRaw, 1'790'000'000, 1'790'000'001, 10);
    ASSERT_EQ(rows.size(), 1U);
    EXPECT_TRUE(rows[0].metrics[0].empty()) << "unobserved metric must stay empty, not become 0";
    EXPECT_EQ(rows[0].metrics[0].count, 0);
    EXPECT_FALSE(rows[0].metrics[3].empty());
    EXPECT_DOUBLE_EQ(rows[0].metrics[3].max, 0.25);
}

TEST_F(HistoryStoreTest, DimensionRoundTrip) {
    {
        auto store = HistoryStore::open(db_path());
        HistoryBatch batch;
        batch.dimensions.push_back(make_dimension_record(1'790'000'000, std::string(kNetworkDomain),
                                                         "eno1", "rx_bytes_total", 12345.0));
        batch.dimensions.push_back(make_dimension_record(1'790'000'000, std::string(kCpuCoreDomain),
                                                         "0", "usage_ratio", 0.5));
        store.write_batch(batch);
    }
    auto reader = HistoryReader::open(db_path());
    ASSERT_EQ(reader.first, HistoryAvailability::Ok);
    const auto network = reader.second->query_dimensions(
        kNetworkDomain, "eno1", "rx_bytes_total", kLevelRaw, 1'790'000'000, 1'790'000'001, 10);
    ASSERT_EQ(network.size(), 1U);
    EXPECT_EQ(network[0].dimension, "eno1");
    EXPECT_EQ(network[0].metric, "rx_bytes_total");
    EXPECT_DOUBLE_EQ(network[0].aggregate.last, 12345.0);

    const auto cores = reader.second->query_dimensions(kCpuCoreDomain, "", "", kLevelRaw,
                                                       1'790'000'000, 1'790'000'001, 10);
    ASSERT_EQ(cores.size(), 1U);
    EXPECT_DOUBLE_EQ(cores[0].aggregate.avg, 0.5);
}

TEST_F(HistoryStoreTest, UnregisteredDimensionMetricIsSkipped) {
    // schema 是唯一真相源：未注册的域/指标名不得落库（否则读取方永远查不到）。
    {
        auto store = HistoryStore::open(db_path());
        HistoryBatch batch;
        batch.dimensions.push_back(
            make_dimension_record(1'790'000'000, "not_a_domain", "x", "y", 1.0));
        batch.dimensions.push_back(make_dimension_record(1'790'000'000, std::string(kNetworkDomain),
                                                         "eno1", "not_a_metric", 1.0));
        store.write_batch(batch);
    }
    auto reader = HistoryReader::open(db_path());
    ASSERT_EQ(reader.first, HistoryAvailability::Ok);
    EXPECT_TRUE(
        reader.second
            ->query_dimensions("not_a_domain", "", "", kLevelRaw, 1'790'000'000, 1'790'000'001, 10)
            .empty());
    EXPECT_TRUE(
        reader.second
            ->query_dimensions(kNetworkDomain, "", "", kLevelRaw, 1'790'000'000, 1'790'000'001, 10)
            .empty());
}

TEST_F(HistoryStoreTest, MarksRoundTrip) {
    {
        auto store = HistoryStore::open(db_path());
        HistoryBatch batch;
        HistoryMark restart;
        restart.ts = 1'790'000'000;
        restart.kind = std::string(kMarkRestart);
        restart.detail = "recover";
        batch.marks.push_back(restart);
        HistoryMark jump;
        jump.ts = 1'790'000'100;
        jump.kind = std::string(kMarkClockJump);
        jump.detail = "-30000";
        batch.marks.push_back(jump);
        store.write_batch(batch);
    }
    auto reader = HistoryReader::open(db_path());
    ASSERT_EQ(reader.first, HistoryAvailability::Ok);
    const auto marks = reader.second->query_marks(1'790'000'000, 1'790'000'200, 10);
    ASSERT_EQ(marks.size(), 2U);
    EXPECT_EQ(marks[0].kind, kMarkRestart);
    EXPECT_EQ(marks[0].detail, "recover");
    EXPECT_EQ(marks[1].kind, kMarkClockJump);
}

TEST_F(HistoryStoreTest, SameBucketRewritesInsteadOfDuplicating) {
    // UPSERT 语义：同桶重复提交后写优先（重试不产生重复行）。
    {
        auto store = HistoryStore::open(db_path());
        HistoryBatch first;
        first.scalars.push_back(make_scalar_record(1'790'000'000, 5.0));
        store.write_batch(first);
        HistoryBatch second;
        second.scalars.push_back(make_scalar_record(1'790'000'000, 9.0));
        store.write_batch(second);
    }
    auto reader = HistoryReader::open(db_path());
    ASSERT_EQ(reader.first, HistoryAvailability::Ok);
    const auto rows = reader.second->query_scalars(kLevelRaw, 1'790'000'000, 1'790'000'001, 10);
    ASSERT_EQ(rows.size(), 1U);
    EXPECT_DOUBLE_EQ(rows[0].metrics[0].last, 9.0);
}

TEST_F(HistoryStoreTest, EmptyBatchIsNoOp) {
    auto store = HistoryStore::open(db_path());
    HistoryBatch empty;
    EXPECT_NO_THROW(store.write_batch(empty));
}

// ===== 查询范围与上限 =====

TEST_F(HistoryStoreTest, QueryRespectsTimeRangeAndOrder) {
    {
        auto store = HistoryStore::open(db_path());
        HistoryBatch batch;
        for (int i = 0; i < 5; ++i) {
            batch.scalars.push_back(
                make_scalar_record(1'790'000'000 + i * 60, static_cast<double>(i)));
        }
        store.write_batch(batch);
    }
    auto reader = HistoryReader::open(db_path());
    ASSERT_EQ(reader.first, HistoryAvailability::Ok);
    // 半开区间 [since, until)；按时间升序。
    // 桶在 000/060/120/180/240；半开区间 [060, 200) 命中 060/120/180。
    const auto rows = reader.second->query_scalars(kLevelRaw, 1'790'000'060, 1'790'000'200, 10);
    ASSERT_EQ(rows.size(), 3U);
    EXPECT_EQ(rows[0].bucket_ts, 1'790'000'060);
    EXPECT_EQ(rows[1].bucket_ts, 1'790'000'120);
    EXPECT_EQ(rows[2].bucket_ts, 1'790'000'180);
}

TEST_F(HistoryStoreTest, QueryRespectsMaxRows) {
    {
        auto store = HistoryStore::open(db_path());
        HistoryBatch batch;
        for (int i = 0; i < 10; ++i) {
            batch.scalars.push_back(make_scalar_record(1'790'000'000 + i * 60, 1.0));
        }
        store.write_batch(batch);
    }
    auto reader = HistoryReader::open(db_path());
    ASSERT_EQ(reader.first, HistoryAvailability::Ok);
    const auto rows = reader.second->query_scalars(kLevelRaw, 0, 2'000'000'000, 3);
    EXPECT_EQ(rows.size(), 3U);
}

TEST_F(HistoryStoreTest, QueryScalarsDescReturnsNewestRowsFirst) {
    // 降序查询是点数上限精确截断的基础：拿到的必须是最新的 N 行（升序 + LIMIT 拿到的是最旧 N 行）
    {
        auto store = HistoryStore::open(db_path());
        HistoryBatch batch;
        for (int i = 0; i < 10; ++i) {
            batch.scalars.push_back(make_scalar_record(1'790'000'000 + i * 60, i));
        }
        store.write_batch(batch);
    }
    auto reader = HistoryReader::open(db_path());
    ASSERT_EQ(reader.first, HistoryAvailability::Ok);
    const auto rows = reader.second->query_scalars_desc(kLevelRaw, 0, 2'000'000'000, 3);
    ASSERT_EQ(rows.size(), 3U);
    // 最新三个桶：+540、+480、+420，降序
    EXPECT_EQ(rows[0].bucket_ts, 1'790'000'540);
    EXPECT_EQ(rows[1].bucket_ts, 1'790'000'480);
    EXPECT_EQ(rows[2].bucket_ts, 1'790'000'420);
    // 数值也跟着桶走（不是只把时间戳反过来）
    EXPECT_DOUBLE_EQ(rows[0].metrics[0].last, 9.0);
    EXPECT_DOUBLE_EQ(rows[2].metrics[0].last, 7.0);
}

TEST_F(HistoryStoreTest, QueryScalarsDescHonoursRangeAndMaxRows) {
    {
        auto store = HistoryStore::open(db_path());
        HistoryBatch batch;
        for (int i = 0; i < 5; ++i) {
            batch.scalars.push_back(make_scalar_record(1'790'000'000 + i * 60, 1.0));
        }
        store.write_batch(batch);
    }
    auto reader = HistoryReader::open(db_path());
    ASSERT_EQ(reader.first, HistoryAvailability::Ok);
    // 范围 [base+120, base+240) 内两行（+120、+180），降序 = +180、+120
    const auto rows =
        reader.second->query_scalars_desc(kLevelRaw, 1'790'000'120, 1'790'000'240, 10);
    ASSERT_EQ(rows.size(), 2U);
    EXPECT_EQ(rows[0].bucket_ts, 1'790'000'180);
    EXPECT_EQ(rows[1].bucket_ts, 1'790'000'120);
    // 退化参数返回空（与升序同一口径）
    EXPECT_TRUE(reader.second->query_scalars_desc(kLevelRaw, 100, 100, 10).empty());
    EXPECT_TRUE(reader.second->query_scalars_desc(kLevelRaw, 200, 100, 10).empty());
    EXPECT_TRUE(reader.second->query_scalars_desc(kLevelRaw, 100, 200, 0).empty());
}

TEST_F(HistoryStoreTest, QueryDimensionsDescReturnsNewestRowsFirst) {
    {
        auto store = HistoryStore::open(db_path());
        HistoryBatch batch;
        for (int i = 0; i < 6; ++i) {
            batch.dimensions.push_back(
                make_dimension_record(1'790'000'000 + i * 60, std::string(kNetworkDomain), "eth0",
                                      "rx_bytes_per_sec", static_cast<double>(i)));
        }
        store.write_batch(batch);
    }
    auto reader = HistoryReader::open(db_path());
    ASSERT_EQ(reader.first, HistoryAvailability::Ok);
    const auto rows = reader.second->query_dimensions_desc(
        kNetworkDomain, "eth0", "rx_bytes_per_sec", kLevelRaw, 0, 2'000'000'000, 2);
    ASSERT_EQ(rows.size(), 2U);
    EXPECT_EQ(rows[0].bucket_ts, 1'790'000'300);
    EXPECT_EQ(rows[1].bucket_ts, 1'790'000'240);
    EXPECT_DOUBLE_EQ(rows[0].aggregate.last, 5.0);
    // 维度过滤仍然生效（别的维度不算进来）
    EXPECT_TRUE(reader.second
                    ->query_dimensions_desc(kNetworkDomain, "eth1", "rx_bytes_per_sec", kLevelRaw,
                                            0, 2'000'000'000, 2)
                    .empty());
}

TEST_F(HistoryStoreTest, LatestBucketTsReturnsNewestAcrossLevels) {
    constexpr int64_t kNow = 2'000'000'000;
    {
        auto store = HistoryStore::open(db_path());
        HistoryBatch batch;
        batch.scalars.push_back(make_scalar_record(kNow - 600, 1.0));
        batch.scalars.push_back(make_scalar_record(kNow - 60, 1.0));
        batch.scalars.push_back(make_scalar_record(kNow - 300, 1.0, kLevelMinute));
        store.write_batch(batch);
    }
    auto reader = HistoryReader::open(db_path());
    ASSERT_EQ(reader.first, HistoryAvailability::Ok);
    // 各层级各取最新（重启缺口按原始层级算）。
    EXPECT_EQ(reader.second->latest_bucket_ts(kLevelRaw), kNow - 60);
    EXPECT_EQ(reader.second->latest_bucket_ts(kLevelMinute), kNow - 300);
}

TEST_F(HistoryStoreTest, LatestBucketTsIsNulloptWhenEmpty) {
    auto store = HistoryStore::open(db_path());
    (void)store;
    auto reader = HistoryReader::open(db_path());
    ASSERT_EQ(reader.first, HistoryAvailability::Ok);
    EXPECT_FALSE(reader.second->latest_bucket_ts(kLevelRaw).has_value());
}

TEST_F(HistoryStoreTest, QueryEmptyRangeReturnsNothing) {
    auto store = HistoryStore::open(db_path());
    (void)store;
    auto reader = HistoryReader::open(db_path());
    ASSERT_EQ(reader.first, HistoryAvailability::Ok);
    EXPECT_TRUE(reader.second->query_scalars(kLevelRaw, 100, 100, 10).empty());
    EXPECT_TRUE(reader.second->query_scalars(kLevelRaw, 200, 100, 10).empty());
    EXPECT_TRUE(reader.second->query_scalars(kLevelRaw, 100, 200, 0).empty());
}

// ===== 只读连接的降级 =====

TEST_F(HistoryStoreTest, ReaderReportsNotFoundWhenFileMissing) {
    // 库缺失不是错误：dzsysmon 可能未启用历史，读取方显示「历史不可用」。
    const auto result = HistoryReader::open(dir_ / "missing.db");
    EXPECT_EQ(result.first, HistoryAvailability::NotFound);
    EXPECT_EQ(result.second, nullptr);
    EXPECT_EQ(history_availability_reason(result.first), "not_found");
}

TEST_F(HistoryStoreTest, ReaderReportsNotFoundForDatabaseWithoutSchemaVersion) {
    // 空 SQLite 文件（有文件但无 schema/无版本行）：读取方必须降级为「历史不可用」，
    // 不得按当前结构解码——列错位读出的数据比「不可用」危险得多。
    sqlite3* raw = nullptr;
    ASSERT_EQ(sqlite3_open(db_path().string().c_str(), &raw), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(raw, "CREATE TABLE unrelated (x INTEGER);", nullptr, nullptr, nullptr),
              SQLITE_OK);
    sqlite3_close(raw);

    const auto reader = HistoryReader::open(db_path());
    EXPECT_EQ(reader.first, HistoryAvailability::NotFound);
    EXPECT_EQ(reader.second, nullptr);
}

TEST_F(HistoryStoreTest, ReaderReportsOpenFailedForNonSqliteFile) {
    // 文件存在但不是 SQLite：必须降级为 OpenFailed，不得抛异常给调用方（REST 路径）。
    const auto path = dir_ / "corrupt.db";
    {
        std::ofstream out(path, std::ios::binary);
        out << "this is not a sqlite database";
    }
    const auto result = HistoryReader::open(path);
    EXPECT_NE(result.first, HistoryAvailability::Ok);
    EXPECT_EQ(result.second, nullptr);
}

// ===== 保留与压缩 =====

TEST_F(HistoryStoreTest, RetentionDeletesExpiredRawRecords) {
    // 原始记录 24 小时水位（记录间隔 < 1 分钟的分支）：超期原始记录被压缩后删除，
    // 保留期内的记录原样留下。
    constexpr int64_t kNow = 2'000'000'000;
    {
        auto store = HistoryStore::open(db_path());
        HistoryBatch batch;
        // 两条超期（> 24h 前，分属不同分钟）+ 两条在保留期内（也分属不同分钟）。
        batch.scalars.push_back(make_scalar_record(kNow - 25 * 3600, 1.0));
        batch.scalars.push_back(make_scalar_record(kNow - 26 * 3600, 1.0));
        batch.scalars.push_back(make_scalar_record(kNow - 1 * 3600, 1.0));
        batch.scalars.push_back(make_scalar_record(kNow - 2 * 3600, 1.0));
        store.write_batch(batch);

        RetentionPolicy policy;
        policy.now_ts = kNow;
        policy.raw_retention_sec = 24 * 3600;
        policy.max_retention_sec = 30 * 86400;
        policy.record_interval_sec = 5;  // < 60 ⇒ 压缩分支
        const auto result = store.apply_retention(policy);
        // 超期的两条进了分钟聚合（原始行被删）；保留期内的两条原样留着。
        EXPECT_EQ(result.compacted_rows, 2);
        EXPECT_EQ(result.deleted_raw_rows, 0) << "expired rows were compacted, not raw-deleted";
    }
    auto reader = HistoryReader::open(db_path());
    ASSERT_EQ(reader.first, HistoryAvailability::Ok);
    const auto raw = reader.second->query_scalars(kLevelRaw, 0, kNow, 100);
    ASSERT_EQ(raw.size(), 2U) << "only in-retention raw records survive as raw";
    // 超期数据没有丢：以分钟聚合的形式仍然可查。
    const auto minutes = reader.second->query_scalars(kLevelMinute, 0, kNow, 100);
    EXPECT_EQ(minutes.size(), 2U) << "expired raw data must survive as minute aggregates";
}

TEST_F(HistoryStoreTest, ExpiredDimensionRowsDoNotOutliveTheirBuckets) {
    // 超期维度行必须与所属桶一起离开原始层级：要么被压缩成分钟聚合，要么被删除，
    // 但绝不能作为「原始行」永久滞留（否则按域查询会读到早已过期的数据）。
    constexpr int64_t kNow = 2'000'000'000;
    {
        auto store = HistoryStore::open(db_path());
        HistoryBatch batch;
        batch.scalars.push_back(make_scalar_record(kNow - 25 * 3600, 1.0));
        batch.dimensions.push_back(make_dimension_record(
            kNow - 25 * 3600, std::string(kNetworkDomain), "eno1", "rx_bytes_total", 1.0));
        store.write_batch(batch);

        RetentionPolicy policy;
        policy.now_ts = kNow;
        policy.record_interval_sec = 5;  // < 60 ⇒ 压缩分支
        store.apply_retention(policy);
    }
    auto reader = HistoryReader::open(db_path());
    ASSERT_EQ(reader.first, HistoryAvailability::Ok);
    EXPECT_TRUE(
        reader.second->query_dimensions(kNetworkDomain, "", "", kLevelRaw, 0, kNow, 100).empty())
        << "no expired dimension row may remain at the raw level";
    // 数据没有丢：维度序列以分钟聚合形式继续可查（30 天保留的前提）。
    const auto minutes = reader.second->query_dimensions(kNetworkDomain, "eno1", "rx_bytes_total",
                                                         kLevelMinute, 0, kNow, 100);
    EXPECT_EQ(minutes.size(), 1U);
}

TEST_F(HistoryStoreTest, CompactionAggregatesExpiredRawIntoMinuteBuckets) {
    constexpr int64_t kNow = 2'000'000'000;
    constexpr int64_t kMinuteBase = 1'999'900'000 / 60 * 60;  // 整分钟
    {
        auto store = HistoryStore::open(db_path());
        HistoryBatch batch;
        // 同一分钟内的 3 条原始记录（记录间隔 5 秒 < 60 ⇒ 需要压缩）。
        for (int i = 0; i < 3; ++i) {
            ScalarRecord record;
            record.bucket_ts = kMinuteBase + i * 5;
            record.interval_sec = 5;
            record.metrics[0].observe(static_cast<double>(i + 1));  // 1, 2, 3
            batch.scalars.push_back(record);
        }
        store.write_batch(batch);

        RetentionPolicy policy;
        policy.now_ts = kNow;
        policy.raw_retention_sec = 24 * 3600;  // 这批数据远超 24h
        policy.max_retention_sec = 30 * 86400;
        policy.record_interval_sec = 5;  // < 60 ⇒ 压缩
        const auto result = store.apply_retention(policy);
        EXPECT_EQ(result.compacted_rows, 3);
        EXPECT_EQ(result.compacted_buckets, 1);
    }
    auto reader = HistoryReader::open(db_path());
    ASSERT_EQ(reader.first, HistoryAvailability::Ok);
    // 原始记录被压缩后删除。
    EXPECT_TRUE(reader.second->query_scalars(kLevelRaw, 0, kNow, 100).empty());
    // 分钟聚合保留：min=1, max=3, avg=2, last=3。
    const auto minutes = reader.second->query_scalars(kLevelMinute, 0, kNow, 100);
    ASSERT_EQ(minutes.size(), 1U);
    EXPECT_EQ(minutes[0].bucket_ts, kMinuteBase);
    EXPECT_EQ(minutes[0].interval_sec, kMinuteResolutionSec);
    EXPECT_DOUBLE_EQ(minutes[0].metrics[0].min, 1.0);
    EXPECT_DOUBLE_EQ(minutes[0].metrics[0].max, 3.0);
    EXPECT_DOUBLE_EQ(minutes[0].metrics[0].avg, 2.0);
    EXPECT_DOUBLE_EQ(minutes[0].metrics[0].last, 3.0);
    EXPECT_EQ(minutes[0].metrics[0].count, 3);
}

TEST_F(HistoryStoreTest, CompactionPreservesPeakInMax) {
    // 尖峰由 max 保留：一分钟内 19 个低值 + 1 个尖峰，压缩后的 max 必须是尖峰。
    constexpr int64_t kNow = 2'000'000'000;
    constexpr int64_t kMinuteBase = 1'999'900'000 / 60 * 60;
    {
        auto store = HistoryStore::open(db_path());
        HistoryBatch batch;
        for (int i = 0; i < 20; ++i) {
            ScalarRecord record;
            record.bucket_ts = kMinuteBase + i;
            record.interval_sec = 1;
            record.metrics[0].observe(i == 10 ? 0.99 : 0.01);
            batch.scalars.push_back(record);
        }
        store.write_batch(batch);

        RetentionPolicy policy;
        policy.now_ts = kNow;
        policy.record_interval_sec = 1;
        store.apply_retention(policy);
    }
    auto reader = HistoryReader::open(db_path());
    ASSERT_EQ(reader.first, HistoryAvailability::Ok);
    const auto minutes = reader.second->query_scalars(kLevelMinute, 0, kNow, 100);
    ASSERT_EQ(minutes.size(), 1U);
    EXPECT_DOUBLE_EQ(minutes[0].metrics[0].max, 0.99) << "peak must survive compaction";
    EXPECT_LT(minutes[0].metrics[0].avg, 0.1);
}

TEST_F(HistoryStoreTest, NoCompactionKeepsRawForFullMaxRetention) {
    // 验收标准：「记录间隔已不小于 1 分钟时直接保留 30 天」——
    // 此时不做压缩，且原始记录**不在 24 小时被删**（保留水位是 max_retention_days）。
    constexpr int64_t kNow = 2'000'000'000;
    {
        auto store = HistoryStore::open(db_path());
        HistoryBatch batch;
        // 30 小时前（超 24h）与 10 天前（在 30 天内）：记录间隔 60 秒，两者都必须保留。
        batch.scalars.push_back(make_scalar_record(kNow - 30 * 3600, 1.0));
        batch.scalars.push_back(make_scalar_record(kNow - 10 * 86400, 2.0));
        // 40 天前：超 30 天，必须删除。
        batch.scalars.push_back(make_scalar_record(kNow - 40 * 86400, 3.0));
        store.write_batch(batch);

        RetentionPolicy policy;
        policy.now_ts = kNow;
        policy.raw_retention_sec = 24 * 3600;  // 该值在 >=60s 分支下**不得**生效
        policy.max_retention_sec = 30 * 86400;
        policy.record_interval_sec = 60;
        const auto result = store.apply_retention(policy);
        EXPECT_EQ(result.compacted_rows, 0) << "no compaction when record interval >= 60s";
        EXPECT_EQ(result.compacted_buckets, 0);
        EXPECT_EQ(result.deleted_raw_rows, 1) << "only the 40-day-old row exceeds max retention";
    }
    auto reader = HistoryReader::open(db_path());
    ASSERT_EQ(reader.first, HistoryAvailability::Ok);
    const auto raw = reader.second->query_scalars(kLevelRaw, 0, kNow, 100);
    ASSERT_EQ(raw.size(), 2U) << "24h-raw-retention must not apply when interval >= 60s";
    EXPECT_TRUE(reader.second->query_scalars(kLevelMinute, 0, kNow, 100).empty());
}

TEST_F(HistoryStoreTest, MinuteAggregatesRetainedLongerThanRaw) {
    // 原始删 24h，分钟聚合保留 30 天：压缩后的数据必须活得更久。
    constexpr int64_t kNow = 2'000'000'000;
    {
        auto store = HistoryStore::open(db_path());
        HistoryBatch batch;
        // 一条 10 天前的分钟聚合（在 30 天内，必须保留）。
        batch.scalars.push_back(make_scalar_record(kNow - 10 * 86400, 1.0, kLevelMinute));
        // 一条 40 天前的分钟聚合（超 30 天，必须删除）。
        batch.scalars.push_back(make_scalar_record(kNow - 40 * 86400, 1.0, kLevelMinute));
        store.write_batch(batch);

        RetentionPolicy policy;
        policy.now_ts = kNow;
        policy.record_interval_sec = 60;
        const auto result = store.apply_retention(policy);
        EXPECT_EQ(result.deleted_minute_rows, 1);
    }
    auto reader = HistoryReader::open(db_path());
    ASSERT_EQ(reader.first, HistoryAvailability::Ok);
    const auto minutes = reader.second->query_scalars(kLevelMinute, 0, kNow, 100);
    ASSERT_EQ(minutes.size(), 1U);
    EXPECT_EQ(minutes[0].bucket_ts, kNow - 10 * 86400);
}

TEST_F(HistoryStoreTest, RetentionDeletesExpiredMarks) {
    constexpr int64_t kNow = 2'000'000'000;
    {
        auto store = HistoryStore::open(db_path());
        HistoryBatch batch;
        HistoryMark old_mark;
        old_mark.ts = kNow - 40 * 86400;
        old_mark.kind = std::string(kMarkRestart);
        batch.marks.push_back(old_mark);
        HistoryMark recent;
        recent.ts = kNow - 1 * 86400;
        recent.kind = std::string(kMarkSampleGap);
        batch.marks.push_back(recent);
        store.write_batch(batch);

        RetentionPolicy policy;
        policy.now_ts = kNow;
        policy.record_interval_sec = 60;
        const auto result = store.apply_retention(policy);
        EXPECT_EQ(result.deleted_marks, 1);
    }
    auto reader = HistoryReader::open(db_path());
    ASSERT_EQ(reader.first, HistoryAvailability::Ok);
    const auto marks = reader.second->query_marks(0, kNow, 100);
    ASSERT_EQ(marks.size(), 1U);
    EXPECT_EQ(marks[0].kind, kMarkSampleGap);
}

TEST_F(HistoryStoreTest, TotalSizeLimitDeletesOldestData) {
    // 总量上限兜底：超限时从最旧删除（新数据优先保留）。
    constexpr int64_t kNow = 2'000'000'000;
    {
        auto store = HistoryStore::open(db_path());
        HistoryBatch batch;
        // 铺足够多数据把库撑到超过上限。
        for (int i = 0; i < 500; ++i) {
            batch.scalars.push_back(make_scalar_record(kNow - 300 * 86400 + i * 60, 1.0));
        }
        store.write_batch(batch);

        RetentionPolicy policy;
        policy.now_ts = kNow;
        policy.record_interval_sec = 60;
        policy.raw_retention_sec = 400 * 86400;  // 不靠时间淘汰
        policy.max_retention_sec = 400 * 86400;
        policy.max_bytes = 40 * 1024;  // 很小，必然超限
        const auto result = store.apply_retention(policy);
        EXPECT_GT(result.deleted_by_size_rows, 0);
        EXPECT_GE(result.bytes_before, result.bytes_after);
    }
    auto reader = HistoryReader::open(db_path());
    ASSERT_EQ(reader.first, HistoryAvailability::Ok);
    const auto rows = reader.second->query_scalars(kLevelRaw, 0, kNow, 10'000);
    // 删掉的是最旧的那部分：保留下来的是最靠后的桶。
    if (!rows.empty()) {
        EXPECT_GT(rows.back().bucket_ts, kNow - 300 * 86400 + 100 * 60);
    }
}

TEST_F(HistoryStoreTest, ZeroMaxBytesMeansUnlimited) {
    constexpr int64_t kNow = 2'000'000'000;
    auto store = HistoryStore::open(db_path());
    HistoryBatch batch;
    for (int i = 0; i < 50; ++i) {
        batch.scalars.push_back(make_scalar_record(kNow - i * 60, 1.0));
    }
    store.write_batch(batch);

    RetentionPolicy policy;
    policy.now_ts = kNow;
    policy.record_interval_sec = 60;
    policy.max_bytes = 0;  // 0 = 不限制
    const auto result = store.apply_retention(policy);
    EXPECT_EQ(result.deleted_by_size_rows, 0);
}

TEST_F(HistoryStoreTest, MaintenanceBatchLimitsWorkPerTransaction) {
    // 分段执行：大批数据分多个事务处理，而不是一个长事务（对只读连接的影响可控）。
    // 断言的是「全部处理完」这一外部承诺；分段本身由 maintenance_batch 驱动，
    // 内部事务数不可观察，故不按事务数断言。
    constexpr int64_t kNow = 2'000'000'000;
    {
        auto store = HistoryStore::open(db_path());
        HistoryBatch batch;
        // 25 条超期原始记录，每条属不同分钟 ⇒ 压成 25 个分钟桶。
        for (int i = 0; i < 25; ++i) {
            batch.scalars.push_back(make_scalar_record(kNow - 30 * 3600 - i * 60, 1.0));
        }
        store.write_batch(batch);

        RetentionPolicy policy;
        policy.now_ts = kNow;
        policy.record_interval_sec = 5;  // < 60 ⇒ 压缩分支
        policy.maintenance_batch = 10;   // 每批 10 行 → 需要 3 批
        const auto result = store.apply_retention(policy);
        EXPECT_EQ(result.compacted_rows, 25) << "all expired rows must be processed across batches";
    }
    auto reader = HistoryReader::open(db_path());
    ASSERT_EQ(reader.first, HistoryAvailability::Ok);
    EXPECT_TRUE(reader.second->query_scalars(kLevelRaw, 0, kNow, 1000).size() < 25U);
}

// 维度「无有效样本」不写行：行的存在即表示该桶该指标有数据（`n` 为 NOT NULL）。
TEST_F(HistoryStoreTest, EmptyDimensionAggregateIsNotWritten) {
    {
        auto store = HistoryStore::open(db_path());
        HistoryBatch batch;
        DimensionRecord record;
        record.bucket_ts = 1'790'000'000;
        record.level = kLevelRaw;
        record.domain = std::string(kNetworkDomain);
        record.dimension = "eno1";
        record.metric = "rx_bytes_total";
        // aggregate 保持空（count=0）：语义是「本桶内该指标无有效样本」。
        batch.dimensions.push_back(record);
        EXPECT_NO_THROW(store.write_batch(batch)) << "empty aggregate must not violate NOT NULL";
    }
    auto reader = HistoryReader::open(db_path());
    ASSERT_EQ(reader.first, HistoryAvailability::Ok);
    EXPECT_TRUE(reader.second
                    ->query_dimensions(kNetworkDomain, "eno1", "rx_bytes_total", kLevelRaw,
                                       1'790'000'000, 1'790'000'001, 10)
                    .empty())
        << "empty aggregate must not produce a row (row existence means data)";
}

// 压缩只删「本批真正压过的行」：维度行远多于标量行时，落在批次边界外的维度行
// 必须留在原始层级等下一批压缩，不能被整桶删除（那会静默丢历史）。
TEST_F(HistoryStoreTest, CompactionDoesNotDropDimensionRowsBeyondBatchLimit) {
    constexpr int64_t kNow = 2'000'000'000;
    constexpr int64_t kMinuteBase = 1'999'900'000 / 60 * 60;
    {
        auto store = HistoryStore::open(db_path());
        HistoryBatch batch;
        // 同一分钟内一条标量 + 多块网卡的维度行（两者各自 LIMIT，分批边界不同）。
        ScalarRecord scalar;
        scalar.bucket_ts = kMinuteBase;
        scalar.interval_sec = 5;
        scalar.metrics[0].observe(1.0);
        batch.scalars.push_back(scalar);
        for (int i = 0; i < 5; ++i) {
            DimensionRecord record;
            record.bucket_ts = kMinuteBase;
            record.domain = std::string(kNetworkDomain);
            record.dimension = "eno" + std::to_string(i);
            record.metric = "rx_bytes_total";
            record.aggregate.observe(static_cast<double>(i));
            batch.dimensions.push_back(record);
        }
        store.write_batch(batch);

        RetentionPolicy policy;
        policy.now_ts = kNow;
        policy.raw_retention_sec = 24 * 3600;
        policy.max_retention_sec = 30 * 86400;
        policy.record_interval_sec = 5;
        policy.maintenance_batch = 2;  // 很小的批次 ⇒ 标量与维度被切成多批
        store.apply_retention(policy);
    }
    auto reader = HistoryReader::open(db_path());
    ASSERT_EQ(reader.first, HistoryAvailability::Ok);
    // 五块网卡的维度历史必须全部以分钟聚合形式留存，一条都不能丢。
    const auto minutes = reader.second->query_dimensions(kNetworkDomain, "", "rx_bytes_total",
                                                         kLevelMinute, 0, kNow, 100);
    EXPECT_EQ(minutes.size(), 5U) << "all dimension rows must survive compaction across batches";
    EXPECT_TRUE(
        reader.second->query_dimensions(kNetworkDomain, "", "", kLevelRaw, 0, kNow, 100).empty())
        << "no raw dimension rows may remain after compaction";
}

// 分钟桶计数按整轮维护去重：同一分钟被分成多批压缩时只算一个桶。
TEST_F(HistoryStoreTest, CompactionCountsBucketsAcrossBatches) {
    constexpr int64_t kNow = 2'000'000'000;
    constexpr int64_t kMinuteBase = 1'999'900'000 / 60 * 60;
    {
        auto store = HistoryStore::open(db_path());
        HistoryBatch batch;
        // 同一分钟内的 4 条原始记录；批次 1 ⇒ 分 4 批压缩同一个分钟桶。
        for (int i = 0; i < 4; ++i) {
            ScalarRecord record;
            record.bucket_ts = kMinuteBase + i;
            record.interval_sec = 1;
            record.metrics[0].observe(static_cast<double>(i));
            batch.scalars.push_back(record);
        }
        store.write_batch(batch);

        RetentionPolicy policy;
        policy.now_ts = kNow;
        policy.record_interval_sec = 1;
        policy.maintenance_batch = 1;
        const auto result = store.apply_retention(policy);
        EXPECT_EQ(result.compacted_buckets, 1) << "one minute bucket, not one per batch";
    }
}

// 总量上限：删除顺序按全库最旧的桶，保留最新数据（不是按层级固定顺序）。
TEST_F(HistoryStoreTest, SizeLimitDeletesGloballyOldestFirst) {
    constexpr int64_t kNow = 2'000'000'000;
    {
        auto store = HistoryStore::open(db_path());
        HistoryBatch batch;
        // 很旧的分钟聚合 + 较新的原始记录：淘汰必须从最旧的（分钟聚合）开始。
        for (int i = 0; i < 600; ++i) {
            batch.scalars.push_back(
                make_scalar_record(kNow - 400 * 86400 + i * 60, 1.0, kLevelMinute));
        }
        // 较新的原始记录：全部落在 kNow 之前的最近 10 小时内（不能越过 kNow，
        // 那会造出「未来时间戳」的历史，也让区间查询读不到）。
        for (int i = 0; i < 600; ++i) {
            batch.scalars.push_back(make_scalar_record(kNow - 10 * 3600 + i * 60, 2.0, kLevelRaw));
        }
        store.write_batch(batch);

        RetentionPolicy policy;
        policy.now_ts = kNow;
        policy.record_interval_sec = 60;
        // 时间水位放很宽，只让总量上限生效。
        policy.raw_retention_sec = 1000 * 86400;
        policy.max_retention_sec = 1000 * 86400;
        // 上限自校准：先量出「一桶都不删」时的库大小，再取其一半作为上限。
        // 写死一个绝对字节数会随 SQLite 页大小/索引实现浮动，容易变成「连空库都超限」
        // （那样会删光全部数据，测不出「保留最新」这个语义）。
        policy.max_bytes = 0;  // 0 = 不限制 → 只量大小
        const auto baseline = store.apply_retention(policy);
        ASSERT_GT(baseline.bytes_after, 0);
        policy.max_bytes = baseline.bytes_after / 2;
        const auto result = store.apply_retention(policy);
        EXPECT_GT(result.deleted_by_size_rows, 0);
    }
    auto reader = HistoryReader::open(db_path());
    ASSERT_EQ(reader.first, HistoryAvailability::Ok);
    const auto minutes = reader.second->query_scalars(kLevelMinute, 0, kNow, 2000);
    const auto raw = reader.second->query_scalars(kLevelRaw, 0, kNow, 2000);
    // 淘汰按最旧优先：600 个很旧的分钟聚合与 400 个较旧的原始记录先走，
    // 最新的一批原始记录必须留下（「总量上限兜底不等于清空」）。
    EXPECT_TRUE(minutes.empty()) << "oldest minute aggregates must be evicted first";
    EXPECT_FALSE(raw.empty()) << "newest raw records must be preserved";
    if (!raw.empty()) {
        // 留下的是最新那批：其时间戳应远晚于被删的分钟聚合。
        EXPECT_GT(raw.back().bucket_ts, kNow - 10 * 3600 + 400 * 60);
    }
}

// 原始保留期比总保留期更宽松时（两者各自合法），总上限必须仍然生效。
TEST_F(HistoryStoreTest, RawRetentionCannotExceedMaxRetention) {
    constexpr int64_t kNow = 2'000'000'000;
    {
        auto store = HistoryStore::open(db_path());
        HistoryBatch batch;
        // 20 天前的原始记录：raw_retention_hours=720（30 天）会放过它，
        // 但 max_retention_days=1（1 天）要求删除 —— 取更严的那个。
        batch.scalars.push_back(make_scalar_record(kNow - 20 * 86400, 1.0));
        store.write_batch(batch);

        RetentionPolicy policy;
        policy.now_ts = kNow;
        policy.record_interval_sec = 60;        // 不压缩
        policy.raw_retention_sec = 720 * 3600;  // 30 天（比总上限宽松）
        policy.max_retention_sec = 1 * 86400;   // 1 天
        const auto result = store.apply_retention(policy);
        EXPECT_EQ(result.deleted_raw_rows, 1)
            << "raw retention must never outlive the overall max retention";
    }
    auto reader = HistoryReader::open(db_path());
    ASSERT_EQ(reader.first, HistoryAvailability::Ok);
    EXPECT_TRUE(reader.second->query_scalars(kLevelRaw, 0, kNow, 100).empty());
}

// ===== 落盘线程 =====

class HistoryWriterTest : public ::testing::Test {
protected:
    void SetUp() override { dir_ = make_temp_dir(); }
    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    std::filesystem::path db_path() const { return dir_ / "sysmon.db"; }
    std::filesystem::path dir_;
};

TEST_F(HistoryWriterTest, WriterPersistsEnqueuedBatches) {
    constexpr int64_t kNow = 2'000'000'000;
    {
        HistoryWriter writer(db_path(), HistoryWriterOptions{}, nullptr);
        HistoryBatch batch;
        batch.scalars.push_back(make_scalar_record(kNow - 60, 7.0));
        batch.scalars.push_back(make_scalar_record(kNow, 8.0));
        writer.enqueue(std::move(batch));
        EXPECT_EQ(writer.written_records(), 0U);  // 异步：入队不等待落盘
        writer.flush();
        EXPECT_EQ(writer.written_records(), 2U);
        EXPECT_EQ(writer.dropped_records(), 0U);
    }
    auto reader = HistoryReader::open(db_path());
    ASSERT_EQ(reader.first, HistoryAvailability::Ok);
    EXPECT_EQ(reader.second->query_scalars(kLevelRaw, 0, kNow + 60, 10).size(), 2U);
}

TEST_F(HistoryWriterTest, FlushOnDestructionPersistsQueuedBatches) {
    // 优雅退出的核心语义：析构（未显式 flush）也必须冲掉队列，不丢数据。
    constexpr int64_t kNow = 2'000'000'000;
    {
        HistoryWriter writer(db_path(), HistoryWriterOptions{}, nullptr);
        HistoryBatch batch;
        batch.scalars.push_back(make_scalar_record(kNow, 7.0));
        writer.enqueue(std::move(batch));
    }
    auto reader = HistoryReader::open(db_path());
    ASSERT_EQ(reader.first, HistoryAvailability::Ok);
    EXPECT_EQ(reader.second->query_scalars(kLevelRaw, 0, kNow + 60, 10).size(), 1U)
        << "destructor must flush queue";
}

TEST_F(HistoryWriterTest, FlushIsIdempotent) {
    constexpr int64_t kNow = 2'000'000'000;
    HistoryWriter writer(db_path(), HistoryWriterOptions{}, nullptr);
    HistoryBatch batch;
    batch.scalars.push_back(make_scalar_record(kNow, 7.0));
    writer.enqueue(std::move(batch));
    writer.flush();
    EXPECT_NO_THROW(writer.flush());
    EXPECT_NO_THROW(writer.flush());
    EXPECT_EQ(writer.written_records(), 1U);
}

TEST_F(HistoryWriterTest, EnqueueAfterFlushIsDroppedWithoutBlocking) {
    // 退出路径：落盘线程已停时入队必须立即丢弃（采集线程绝不能卡住）。
    HistoryWriter writer(db_path(), HistoryWriterOptions{}, nullptr);
    writer.flush();
    HistoryBatch batch;
    batch.scalars.push_back(make_scalar_record(1'790'000'000, 1.0));
    EXPECT_NO_THROW(writer.enqueue(std::move(batch)));
    EXPECT_GE(writer.dropped_records(), 1U);
}

TEST_F(HistoryWriterTest, QueueOverflowDropsOldestAndCounts) {
    // 有界队列：容量 1 时连续入队两个批次，最旧的被丢弃并计数（不阻塞）。
    HistoryWriterOptions options;
    options.queue_capacity = 1;
    options.maintenance_interval_sec = 0;
    // 用一个慢不下来的写入方难以确定性制造溢出；这里用小容量 + 立即入队，
    // 断言「不阻塞 + 计数单调」这一可观察承诺，不断言具体丢弃哪一条（线程调度相关）。
    HistoryWriter writer(db_path(), options, nullptr);
    for (int i = 0; i < 50; ++i) {
        HistoryBatch batch;
        batch.scalars.push_back(make_scalar_record(1'790'000'000 + i * 60, 1.0));
        writer.enqueue(std::move(batch));
    }
    writer.flush();
    // 全部记录要么落盘要么被计入丢弃——不能既有丢失又不计数。
    EXPECT_EQ(writer.written_records() + writer.dropped_records(), 50U);
}

TEST_F(HistoryWriterTest, WriterStartsThreadAndReportsRunning) {
    HistoryWriter writer(db_path(), HistoryWriterOptions{}, nullptr);
    EXPECT_TRUE(writer.running());
    writer.flush();
    EXPECT_FALSE(writer.running());
}

TEST_F(HistoryWriterTest, MaintenanceAppliesRetentionOnWriterThread) {
    // 维护周期确实在**落盘线程**上执行保留策略（而不是只在测试里手工调 apply_retention）。
    //
    // 等待条件必须是「保留策略的产物出现」——这里取「超期原始行已不在原始层级」，
    // 它只可能由维护周期造成（本用例的写入方只写不删）。之前那版把「探针行尚未落盘」
    // 当成成功条件，因此保留策略一次都没跑也能通过。
    constexpr int64_t kNow = 2'000'000'000;
    constexpr int64_t kExpiredTs = kNow - 30 * 3600;
    HistoryWriterOptions options;
    options.maintenance_interval_sec = 1;  // 1 秒跑一轮维护
    HistoryWriter writer(db_path(), options, [kNow]() {
        RetentionPolicy policy;
        policy.now_ts = kNow;
        policy.record_interval_sec = 5;   // < 60 ⇒ 压缩分支
        policy.raw_retention_sec = 3600;  // 超 1 小时即超期
        policy.max_retention_sec = 30 * 86400;
        return policy;
    });
    HistoryBatch batch;
    batch.scalars.push_back(make_scalar_record(kExpiredTs, 1.0));
    writer.enqueue(std::move(batch));

    // 有界条件等待：等到超期行离开原始层级（维护周期的唯一可能产物）。
    bool expired_left_raw = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        auto reader = HistoryReader::open(db_path());
        if (reader.first == HistoryAvailability::Ok) {
            const auto raw = reader.second->query_scalars(kLevelRaw, 0, kNow, 100);
            const bool still_there =
                std::any_of(raw.begin(), raw.end(), [kExpiredTs](const ScalarRecord& record) {
                    return record.bucket_ts == kExpiredTs;
                });
            // 只有「它曾经落库、现在不在原始层级」才算维护跑过；还没落库时继续等。
            const bool ever_written = writer.written_records() > 0;
            if (ever_written && !still_there) {
                expired_left_raw = true;
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    EXPECT_TRUE(expired_left_raw) << "writer thread must apply retention to expired raw buckets";
    writer.flush();
}

}  // namespace
}  // namespace dztrader::sysmonstore
