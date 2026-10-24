/**
 * @file sysmon_history_service_test.cpp
 * @brief 历史查询（《帧契约：系统监控》§6.1）：参数校验、临时库上的真实查询、三种降级路径。
 *
 * 测试准则（docs/unit-test-standard.md）：断言外部可观察行为——HTTP 语义上的 400 / 200，
 * 响应 JSON 的 available/reason/series/marks/gaps，以及「不可用时绝不返回伪造数据」。
 * 不断言内部实现（不检查 SQL、不检查私有成员）。
 *
 * 隔离：每个用例用 `temp_directory_path()` 下的唯一目录（pid + 随机后缀），TearDown 清理；
 * 不触碰 `DZTRADER_HOME`，也不留跨运行残留。
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <random>
#include <string>
#include <vector>

#include <sqlite3.h>

#include <dztrader/core/this_process.h>
#include <dztrader/sysmonstore/history_query.h>
#include <dztrader/sysmonstore/history_store.h>
#include <dztrader/sysmonstore/records.h>
#include <dztrader/sysmonstore/schema.h>

#include "sysmon_history_service.h"

namespace dztrader::webui {
namespace {

using dztrader::sysmonstore::HistoryBatch;
using dztrader::sysmonstore::HistoryMark;
using dztrader::sysmonstore::HistoryStore;
using dztrader::sysmonstore::ScalarRecord;

constexpr int64_t kBaseTs = 1'790'000'000;

std::filesystem::path make_temp_dir() {
    std::random_device rd;
    const auto suffix = std::to_string(dztrader::this_process::pid()) + "_" + std::to_string(rd());
    auto dir = std::filesystem::temp_directory_path() / ("dzsysmon_history_svc_" + suffix);
    std::filesystem::create_directories(dir);
    return dir;
}

/// 构造一条填满全部标量指标（同值）的记录。
ScalarRecord make_scalar(int64_t bucket_ts,
                         double value,
                         int32_t level = dztrader::sysmonstore::kLevelRaw,
                         int64_t interval_sec = 60) {
    ScalarRecord record;
    record.level = level;
    record.bucket_ts = bucket_ts;
    record.interval_sec = interval_sec;
    for (auto& metric : record.metrics) {
        metric.observe(value);
    }
    return record;
}

/// 让指定宽表指标取不同值（其余指标仍用 `seed`），用于验证「取对了列」。
ScalarRecord make_scalar_with_cpu(int64_t bucket_ts, double cpu_ratio, double seed) {
    auto record = make_scalar(bucket_ts, seed);
    for (auto& metric : record.metrics) {
        metric = {};  // 清空后用逐指标 observe 精确设定
    }
    for (size_t i = 0; i < record.metrics.size(); ++i) {
        const auto& spec = dztrader::sysmonstore::kScalarMetrics[i];
        record.metrics[i].observe(spec.key == "cpu.usage_ratio" ? cpu_ratio : seed);
    }
    return record;
}

class SysmonHistoryServiceTest : public ::testing::Test {
protected:
    void SetUp() override { dir_ = make_temp_dir(); }
    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    std::filesystem::path db_path() const { return dir_ / "sysmon.db"; }

    /// 用真实写入方建库并落一批数据（读取方只读，写入方 API 是唯一建库途径）。
    void seed(const std::function<void(HistoryStore&, HistoryBatch&)>& build) {
        auto store = HistoryStore::open(db_path());
        HistoryBatch batch;
        build(store, batch);
        if (!batch.empty()) {
            store.write_batch(batch);
        }
    }

    /// 以干净方式打开写入方（仅建库/schema，不写数据）。
    void create_empty_db() {
        auto store = HistoryStore::open(db_path());
        (void)store;
    }

    SysmonHistoryService service(int64_t now_ts = kBaseTs + 7200) const {
        return SysmonHistoryService(db_path(), 2000, now_ts);
    }

    /// 用 map 模拟 drogon 的 query 参数（缺失即空串，与 getParameter 语义一致）。
    static std::function<std::string(std::string_view)> params(
        std::map<std::string, std::string> values) {
        return [values = std::move(values)](std::string_view key) -> std::string {
            auto it = values.find(std::string(key));
            return it == values.end() ? std::string{} : it->second;
        };
    }

    std::filesystem::path dir_;
};

// ===== 参数解析（《帧契约：系统监控》§6.1「参数非法」）=====

TEST_F(SysmonHistoryServiceTest, ParsesDefaultsWhenNoParametersGiven) {
    const auto svc = service();
    const auto parsed = svc.parse(params({}));
    ASSERT_TRUE(parsed.query.has_value());
    const auto& q = *parsed.query;
    EXPECT_EQ(q.metric, "cpu.usage_ratio");
    EXPECT_TRUE(q.dimension.empty());
    EXPECT_EQ(q.level, "auto");
    EXPECT_EQ(q.max_points, dztrader::sysmonstore::kDefaultMaxPoints);
    // 默认范围：最近 6 小时
    EXPECT_EQ(q.to_ts, kBaseTs + 7200);
    EXPECT_EQ(q.from_ts, kBaseTs + 7200 - 6 * 3600);
    EXPECT_LT(q.from_ts, q.to_ts);
}

TEST_F(SysmonHistoryServiceTest, ParsesExplicitParameters) {
    const auto svc = service();
    const auto parsed = svc.parse(params({
        {"metric", "network.rx_bytes_per_sec"},
        {"dimension", "eth0"},
        {"from", std::to_string(kBaseTs)},
        {"to", std::to_string(kBaseTs + 1800)},
        {"level", "raw"},
        {"max_points", "120"},
    }));
    ASSERT_TRUE(parsed.query.has_value());
    const auto& q = *parsed.query;
    EXPECT_EQ(q.metric, "network.rx_bytes_per_sec");
    EXPECT_EQ(q.dimension, "eth0");
    EXPECT_EQ(q.from_ts, kBaseTs);
    EXPECT_EQ(q.to_ts, kBaseTs + 1800);
    EXPECT_EQ(q.level, "raw");
    EXPECT_EQ(q.max_points, 120);
}

TEST_F(SysmonHistoryServiceTest, RejectsUnknownMetric) {
    const auto svc = service();
    EXPECT_FALSE(svc.parse(params({{"metric", "nope"}})).query.has_value());
    EXPECT_FALSE(svc.parse(params({{"metric", "network.nope"}})).query.has_value());
}

TEST_F(SysmonHistoryServiceTest, RequiresDimensionForDimensionMetrics) {
    const auto svc = service();
    // 维度指标缺 dimension ⇒ 400（本版本每次只查一条序列，不返回全部维度）
    EXPECT_FALSE(svc.parse(params({{"metric", "network.rx_bytes_per_sec"}})).query.has_value());
    // 标量指标给了 dimension ⇒ 也非法（避免静默忽略用户意图）
    EXPECT_FALSE(
        svc.parse(params({{"metric", "cpu.usage_ratio"}, {"dimension", "0"}})).query.has_value());
}

TEST_F(SysmonHistoryServiceTest, RejectsInvalidLevel) {
    const auto svc = service();
    EXPECT_FALSE(svc.parse(params({{"level", "hour"}})).query.has_value());
    EXPECT_FALSE(svc.parse(params({{"level", "RAW"}})).query.has_value());
    // 三个合法值都能过
    EXPECT_TRUE(svc.parse(params({{"level", "auto"}})).query.has_value());
    EXPECT_TRUE(svc.parse(params({{"level", "raw"}})).query.has_value());
    EXPECT_TRUE(svc.parse(params({{"level", "minute"}})).query.has_value());
}

TEST_F(SysmonHistoryServiceTest, RejectsOutOfRangeMaxPoints) {
    const auto svc = service();
    EXPECT_FALSE(svc.parse(params({{"max_points", "9"}})).query.has_value());
    EXPECT_FALSE(svc.parse(params({{"max_points", "20001"}})).query.has_value());
    EXPECT_FALSE(svc.parse(params({{"max_points", "abc"}})).query.has_value());
    EXPECT_FALSE(svc.parse(params({{"max_points", "12abc"}})).query.has_value());
    // 边界值合法
    EXPECT_TRUE(svc.parse(params({{"max_points", "10"}})).query.has_value());
    EXPECT_TRUE(svc.parse(params({{"max_points", "20000"}})).query.has_value());
}

TEST_F(SysmonHistoryServiceTest, RejectsInvalidTimeRange) {
    const auto svc = service();
    EXPECT_FALSE(svc.parse(params({{"from", "abc"}})).query.has_value());
    EXPECT_FALSE(svc.parse(params({{"to", "abc"}})).query.has_value());
    // from >= to 是倒置/空范围，不静默改成默认范围
    EXPECT_FALSE(svc.parse(params({{"from", "200"}, {"to", "100"}})).query.has_value());
    EXPECT_FALSE(svc.parse(params({{"from", "100"}, {"to", "100"}})).query.has_value());
    // from <= 0 无意义
    EXPECT_FALSE(svc.parse(params({{"from", "0"}, {"to", "100"}})).query.has_value());
    EXPECT_FALSE(svc.parse(params({{"from", "-5"}, {"to", "100"}})).query.has_value());
}

// ===== 降级路径（《帧契约：系统监控》§6.1.3）=====

TEST_F(SysmonHistoryServiceTest, DegradesWhenDatabaseMissing) {
    // 库不存在（dzsysmon 未启用历史）：200 + available=false + reason=not_found，不抛
    const auto body = service().query(*service().parse(params({})).query);
    EXPECT_FALSE(body.value("available", true));
    EXPECT_EQ(body.value("reason", ""), "not_found");
    // 绝不返回伪造数据字段
    EXPECT_FALSE(body.contains("series"));
    EXPECT_FALSE(body.contains("marks"));
    EXPECT_FALSE(body.contains("gaps"));
    EXPECT_FALSE(body.contains("interval_sec"));
}

TEST_F(SysmonHistoryServiceTest, DegradesOnSchemaVersionMismatch) {
    // 用原生 sqlite3 造「版本不符」的库（写入方 API 故意不提供改版本的能力）
    {
        sqlite3* raw = nullptr;
        ASSERT_EQ(sqlite3_open(db_path().string().c_str(), &raw), SQLITE_OK);
        ASSERT_EQ(sqlite3_exec(raw,
                               "CREATE TABLE sysmon_meta (key TEXT PRIMARY KEY, value TEXT NOT "
                               "NULL);"
                               "INSERT INTO sysmon_meta (key, value) VALUES ('schema_version', "
                               "'999');",
                               nullptr, nullptr, nullptr),
                  SQLITE_OK);
        sqlite3_close(raw);
    }
    const auto svc = service();
    const auto body = svc.query(*svc.parse(params({})).query);
    EXPECT_FALSE(body.value("available", true));
    EXPECT_EQ(body.value("reason", ""), "schema_mismatch");
    EXPECT_FALSE(body.contains("series"));
}

TEST_F(SysmonHistoryServiceTest, DegradesOnCorruptDatabaseFile) {
    // 文件存在但不是 SQLite（损坏/被替换）：降级为不可用，不崩溃
    {
        std::ofstream out(db_path(), std::ios::binary);
        ASSERT_TRUE(out.good());
        out << "this is definitely not a sqlite database file";
    }
    const auto svc = service();
    const auto body = svc.query(*svc.parse(params({})).query);
    EXPECT_FALSE(body.value("available", true));
    EXPECT_EQ(body.value("reason", ""), "open_failed");
    EXPECT_FALSE(body.contains("series"));
}

TEST_F(SysmonHistoryServiceTest, DegradesOnDatabaseWithoutSchema) {
    // 空 SQLite 文件（无 sysmon 表）：不是「有历史但没数据」，而是「历史库未就绪」
    {
        sqlite3* raw = nullptr;
        ASSERT_EQ(sqlite3_open(db_path().string().c_str(), &raw), SQLITE_OK);
        sqlite3_close(raw);
    }
    const auto svc = service();
    const auto body = svc.query(*svc.parse(params({})).query);
    EXPECT_FALSE(body.value("available", true));
    EXPECT_EQ(body.value("reason", ""), "not_found");
}

// ===== 真实查询（临时库）=====

TEST_F(SysmonHistoryServiceTest, ReturnsEmptySeriesForRangeWithoutBuckets) {
    create_empty_db();
    const auto svc = service();
    const auto body = svc.query(*svc.parse(params({
                                               {"metric", "cpu.usage_ratio"},
                                               {"from", std::to_string(kBaseTs)},
                                               {"to", std::to_string(kBaseTs + 600)},
                                               {"level", "raw"},
                                           }))
                                     .query);
    // 「范围内没有数据」与「历史不可用」是不同事实（契约 §6.1「空结果」）
    EXPECT_TRUE(body.value("available", false));
    EXPECT_EQ(body.value("level", ""), "raw");
    ASSERT_EQ(body["series"].size(), 1U);
    EXPECT_TRUE(body["series"][0]["points"].empty());
    EXPECT_TRUE(body["gaps"].empty());
    EXPECT_TRUE(body["marks"].empty());
    // 无桶即无桶宽：不得猜一个值（契约 §6.1 的 interval_sec 口径）
    EXPECT_TRUE(body["interval_sec"].is_null());
    EXPECT_FALSE(body.value("truncated", true));
}

TEST_F(SysmonHistoryServiceTest, ReturnsScalarEnvelopeWithAllAggregates) {
    seed([](HistoryStore&, HistoryBatch& batch) {
        ScalarRecord record = make_scalar(kBaseTs, 0.0);
        for (auto& metric : record.metrics) {
            metric = {};
        }
        // cpu.usage_ratio 取 0.1 / 0.5 / 0.9：min/max/avg/last 各不相同
        auto& cpu = record.metrics[0];
        cpu.observe(0.1);
        cpu.observe(0.9);
        cpu.observe(0.5);
        batch.scalars.push_back(record);
    });
    const auto svc = service();
    const auto body = svc.query(*svc.parse(params({
                                               {"metric", "cpu.usage_ratio"},
                                               {"from", std::to_string(kBaseTs)},
                                               {"to", std::to_string(kBaseTs + 60)},
                                               {"level", "raw"},
                                           }))
                                     .query);
    ASSERT_TRUE(body.value("available", false));
    ASSERT_EQ(body["series"].size(), 1U);
    EXPECT_TRUE(body["series"][0]["dimension"].get<std::string>().empty());
    const auto& points = body["series"][0]["points"];
    ASSERT_EQ(points.size(), 1U);
    EXPECT_EQ(points[0]["ts"].get<int64_t>(), kBaseTs);
    // 最小 / 最大包络直接来自 min / max（尖峰由 max 保留）
    EXPECT_DOUBLE_EQ(points[0]["min"].get<double>(), 0.1);
    EXPECT_DOUBLE_EQ(points[0]["max"].get<double>(), 0.9);
    EXPECT_DOUBLE_EQ(points[0]["avg"].get<double>(), 0.5);
    EXPECT_DOUBLE_EQ(points[0]["last"].get<double>(), 0.5);
    EXPECT_EQ(points[0]["n"].get<int64_t>(), 3);
    EXPECT_EQ(body["interval_sec"].get<int64_t>(), 60);
}

TEST_F(SysmonHistoryServiceTest, SelectsRequestedScalarColumn) {
    // 防「取错列」：同时写两条桶，只让 memory.used_bytes 与 cpu.usage_ratio 有区分度
    seed([](HistoryStore&, HistoryBatch& batch) {
        batch.scalars.push_back(make_scalar_with_cpu(kBaseTs, 0.25, 7.0));
    });
    const auto svc = service();
    const auto as_json = [&](const std::string& metric) {
        return svc.query(*svc.parse(params({
                                        {"metric", metric},
                                        {"from", std::to_string(kBaseTs)},
                                        {"to", std::to_string(kBaseTs + 60)},
                                        {"level", "raw"},
                                    }))
                              .query);
    };
    const auto cpu = as_json("cpu.usage_ratio");
    EXPECT_DOUBLE_EQ(cpu["series"][0]["points"][0]["last"].get<double>(), 0.25);
    const auto memory = as_json("memory.total_bytes");
    EXPECT_DOUBLE_EQ(memory["series"][0]["points"][0]["last"].get<double>(), 7.0);
}

TEST_F(SysmonHistoryServiceTest, ReturnsEmptyMetricPointsAsNull) {
    // 该桶内该指标无有效样本 ⇒ 四值与 n 全为 null，不得用 0 冒充（契约 §6.1）
    seed([](HistoryStore&, HistoryBatch& batch) {
        ScalarRecord record = make_scalar(kBaseTs, 0.0);
        for (auto& metric : record.metrics) {
            metric = {};  // 全部指标都无样本
        }
        record.metrics[7].observe(12345.0);  // 只填 memory.total_bytes
        batch.scalars.push_back(record);
    });
    const auto svc = service();
    const auto body = svc.query(*svc.parse(params({
                                               {"metric", "cpu.usage_ratio"},
                                               {"from", std::to_string(kBaseTs)},
                                               {"to", std::to_string(kBaseTs + 60)},
                                               {"level", "raw"},
                                           }))
                                     .query);
    ASSERT_TRUE(body.value("available", false));
    const auto& point = body["series"][0]["points"][0];
    EXPECT_TRUE(point["min"].is_null());
    EXPECT_TRUE(point["max"].is_null());
    EXPECT_TRUE(point["avg"].is_null());
    EXPECT_TRUE(point["last"].is_null());
    EXPECT_TRUE(point["n"].is_null());
}

TEST_F(SysmonHistoryServiceTest, AutoLevelSelectsRawForShortRangeAndMinuteForLong) {
    // 同一批数据里同时有原始记录与分钟聚合：auto 的选择必须能从回显 level 看出
    seed([](HistoryStore&, HistoryBatch& batch) {
        batch.scalars.push_back(make_scalar(kBaseTs, 1.0, dztrader::sysmonstore::kLevelRaw));
        batch.scalars.push_back(
            make_scalar(1'780'000'000, 2.0, dztrader::sysmonstore::kLevelMinute, 60));
    });
    const auto svc = service();
    // 1 小时内 ⇒ raw
    const auto short_range = svc.query(*svc.parse(params({
                                                      {"from", std::to_string(kBaseTs)},
                                                      {"to", std::to_string(kBaseTs + 3600)},
                                                  }))
                                            .query);
    EXPECT_EQ(short_range.value("level", ""), "raw");
    EXPECT_EQ(short_range["series"][0]["points"].size(), 1U);
    // 超过 1 小时 ⇒ minute
    const auto long_range = svc.query(*svc.parse(params({
                                                     {"from", std::to_string(1'780'000'000)},
                                                     {"to", std::to_string(1'780'000'000 + 7200)},
                                                 }))
                                           .query);
    EXPECT_EQ(long_range.value("level", ""), "minute");
    EXPECT_EQ(long_range["series"][0]["points"].size(), 1U);
    EXPECT_EQ(long_range["interval_sec"].get<int64_t>(), 60);
}

TEST_F(SysmonHistoryServiceTest, TrimsToNewestPointsAndReportsTruncation) {
    // 20 个桶、上限 10 ⇒ 保留最新 10 个并回显 truncated=true + 生效 from
    seed([](HistoryStore&, HistoryBatch& batch) {
        for (int i = 0; i < 20; ++i) {
            batch.scalars.push_back(
                make_scalar_with_cpu(kBaseTs + i * 60, static_cast<double>(i), 0.0));
        }
    });
    const auto svc = service();
    const auto body = svc.query(*svc.parse(params({
                                               {"from", std::to_string(kBaseTs)},
                                               {"to", std::to_string(kBaseTs + 20 * 60)},
                                               {"level", "raw"},
                                               {"max_points", "10"},
                                           }))
                                     .query);
    EXPECT_TRUE(body.value("available", false));
    EXPECT_TRUE(body.value("truncated", false));
    const auto& points = body["series"][0]["points"];
    ASSERT_EQ(points.size(), 10U);
    // 最新的一段：桶 10..19
    EXPECT_EQ(points.front()["ts"].get<int64_t>(), kBaseTs + 10 * 60);
    EXPECT_EQ(points.back()["ts"].get<int64_t>(), kBaseTs + 19 * 60);
    // 生效 from == 首个返回点的桶时刻（契约 §6.1.2 的精确口径）
    EXPECT_EQ(body["from"].get<int64_t>(), kBaseTs + 10 * 60);
    // 升序输出（前端直接画图）
    for (size_t i = 1; i < points.size(); ++i) {
        EXPECT_LT(points[i - 1]["ts"].get<int64_t>(), points[i]["ts"].get<int64_t>());
    }
}

TEST_F(SysmonHistoryServiceTest, DoesNotReportTruncationWhenDataFits) {
    // 只有 5 个桶、上限 10：哪怕范围宽度远大于 10 个桶宽，也不得标截断（否则前端会以为丢了数据）
    seed([](HistoryStore&, HistoryBatch& batch) {
        for (int i = 0; i < 5; ++i) {
            batch.scalars.push_back(make_scalar(kBaseTs + i * 60, 1.0));
        }
    });
    const auto svc = service();
    const auto body = svc.query(*svc.parse(params({
                                               {"from", std::to_string(kBaseTs)},
                                               {"to", std::to_string(kBaseTs + 24 * 3600)},
                                               {"level", "raw"},
                                               {"max_points", "10"},
                                           }))
                                     .query);
    EXPECT_FALSE(body.value("truncated", true));
    EXPECT_EQ(body["series"][0]["points"].size(), 5U);
    // 未截断时 from 就是请求的 from
    EXPECT_EQ(body["from"].get<int64_t>(), kBaseTs);
}

TEST_F(SysmonHistoryServiceTest, ReturnsDimensionSeriesForRequestedDimension) {
    seed([](HistoryStore&, HistoryBatch& batch) {
        for (int i = 0; i < 3; ++i) {
            dztrader::sysmonstore::DimensionRecord record;
            record.level = dztrader::sysmonstore::kLevelRaw;
            record.bucket_ts = kBaseTs + i * 60;
            record.domain = std::string(dztrader::sysmonstore::kNetworkDomain);
            record.dimension = "eth0";
            record.metric = "rx_bytes_per_sec";
            record.aggregate.observe(100.0 * static_cast<double>(i));
            batch.dimensions.push_back(record);
        }
        // 另一块网卡：必须被 dimension 参数过滤掉
        dztrader::sysmonstore::DimensionRecord other;
        other.level = dztrader::sysmonstore::kLevelRaw;
        other.bucket_ts = kBaseTs;
        other.domain = std::string(dztrader::sysmonstore::kNetworkDomain);
        other.dimension = "eth1";
        other.metric = "rx_bytes_per_sec";
        other.aggregate.observe(999.0);
        batch.dimensions.push_back(other);
    });
    const auto svc = service();
    const auto body = svc.query(*svc.parse(params({
                                               {"metric", "network.rx_bytes_per_sec"},
                                               {"dimension", "eth0"},
                                               {"from", std::to_string(kBaseTs)},
                                               {"to", std::to_string(kBaseTs + 300)},
                                               {"level", "raw"},
                                           }))
                                     .query);
    ASSERT_TRUE(body.value("available", false));
    ASSERT_EQ(body["series"].size(), 1U);
    EXPECT_EQ(body["series"][0]["dimension"].get<std::string>(), "eth0");
    const auto& points = body["series"][0]["points"];
    ASSERT_EQ(points.size(), 3U);
    EXPECT_DOUBLE_EQ(points[0]["max"].get<double>(), 0.0);
    EXPECT_DOUBLE_EQ(points[2]["max"].get<double>(), 200.0);
    // eth1 的 999 不得出现（按维度过滤生效）
    for (const auto& point : points) {
        if (point["max"].is_number()) {
            EXPECT_NE(point["max"].get<double>(), 999.0);
        }
    }
}

// ===== 标记与缺口（《帧契约：系统监控》§6.1.4）=====

TEST_F(SysmonHistoryServiceTest, ReportsMarksAndDerivesGaps) {
    seed([](HistoryStore&, HistoryBatch& batch) {
        batch.scalars.push_back(make_scalar(kBaseTs, 1.0));
        batch.scalars.push_back(make_scalar(kBaseTs + 60, 1.0));
        // 缺口：下一桶跳到 +600（缺口 [base+120, base+600)）
        batch.scalars.push_back(make_scalar(kBaseTs + 600, 1.0));
        HistoryMark restart;
        restart.ts = kBaseTs + 300;
        restart.kind = std::string(dztrader::sysmonstore::kMarkRestart);
        restart.detail = "recover";
        batch.marks.push_back(restart);
    });
    const auto svc = service();
    const auto body = svc.query(*svc.parse(params({
                                               {"from", std::to_string(kBaseTs)},
                                               {"to", std::to_string(kBaseTs + 1200)},
                                               {"level", "raw"},
                                           }))
                                     .query);
    ASSERT_TRUE(body.value("available", false));
    // marks 是事实清单
    ASSERT_EQ(body["marks"].size(), 1U);
    EXPECT_EQ(body["marks"][0]["ts"].get<int64_t>(), kBaseTs + 300);
    EXPECT_EQ(body["marks"][0]["kind"].get<std::string>(), "restart");
    EXPECT_EQ(body["marks"][0]["detail"].get<std::string>(), "recover");
    // gaps 由序列不连续推出，标记解释原因
    ASSERT_EQ(body["gaps"].size(), 1U);
    EXPECT_EQ(body["gaps"][0]["from"].get<int64_t>(), kBaseTs + 120);
    EXPECT_EQ(body["gaps"][0]["to"].get<int64_t>(), kBaseTs + 600);
    EXPECT_EQ(body["gaps"][0]["kind"].get<std::string>(), "restart");
}

TEST_F(SysmonHistoryServiceTest, ReportsUnmarkedGapWhenDataMissingWithoutMark) {
    // 数据确实缺了但没有标记解释：这是要暴露的异常，不得默默当连续
    seed([](HistoryStore&, HistoryBatch& batch) {
        batch.scalars.push_back(make_scalar(kBaseTs, 1.0));
        batch.scalars.push_back(make_scalar(kBaseTs + 600, 1.0));
    });
    const auto svc = service();
    const auto body = svc.query(*svc.parse(params({
                                               {"from", std::to_string(kBaseTs)},
                                               {"to", std::to_string(kBaseTs + 1200)},
                                               {"level", "raw"},
                                           }))
                                     .query);
    ASSERT_EQ(body["gaps"].size(), 1U);
    EXPECT_EQ(body["gaps"][0]["kind"].get<std::string>(), "unmarked");
    EXPECT_TRUE(body["marks"].empty());
}

// ===== 回归（自审 4 条：interval 口径 / 维度缺口 / 标记不被点数上限截断）=====

TEST_F(SysmonHistoryServiceTest, MarksAreNotTruncatedByPointLimit) {
    // 标记解释缺口原因；若标记查询拿 max_points 当上限，标记数超过点数上限时会被截断，
    // 缺口于是被错报成 unmarked（把「有解释的停机」误报成「无人解释的异常」）。
    // 这里造 15 条标记 > max_points(10)，旧实现会只剩 10 条。
    seed([](HistoryStore&, HistoryBatch& batch) {
        for (int i = 0; i < 30; ++i) {
            batch.scalars.push_back(make_scalar(kBaseTs + i * 60, 1.0));
        }
        // 标记必须落在**生效窗口内**（截断后 from = base+20*60），否则它们本就在请求范围之外。
        // 15 条 > max_points(10)：旧实现（用 max_points 当标记上限）会只剩 10 条。
        for (int i = 0; i < 15; ++i) {
            HistoryMark restart;
            restart.ts = kBaseTs + 20 * 60 + i * 10;
            restart.kind = std::string(dztrader::sysmonstore::kMarkRestart);
            restart.detail = "recover";
            batch.marks.push_back(restart);
        }
    });
    const auto svc = service();
    const auto body = svc.query(*svc.parse(params({
                                               {"from", std::to_string(kBaseTs)},
                                               {"to", std::to_string(kBaseTs + 40 * 60)},
                                               {"level", "raw"},
                                               {"max_points", "10"},
                                           }))
                                     .query);
    ASSERT_TRUE(body.value("available", false));
    EXPECT_TRUE(body.value("truncated", false));
    // 记录点被裁到 10 个，但标记一条都不能丢
    EXPECT_EQ(body["series"][0]["points"].size(), 10U);
    EXPECT_EQ(body["marks"].size(), 15U);
}

TEST_F(SysmonHistoryServiceTest, RawIntervalUsesNewestRowWhenRecordIntervalChanged) {
    // 记录间隔可在运行期调整：跨调整点的一批行桶宽不同，interval_sec 必须取最新一行
    // （当前生效桶宽），而不是最早一行（旧配置）。
    seed([](HistoryStore&, HistoryBatch& batch) {
        batch.scalars.push_back(
            make_scalar(kBaseTs, 1.0, dztrader::sysmonstore::kLevelRaw, /*interval=*/300));
        batch.scalars.push_back(
            make_scalar(kBaseTs + 300, 1.0, dztrader::sysmonstore::kLevelRaw, /*interval=*/60));
    });
    const auto svc = service();
    const auto body = svc.query(*svc.parse(params({
                                               {"metric", "cpu.usage_ratio"},
                                               {"from", std::to_string(kBaseTs)},
                                               {"to", std::to_string(kBaseTs + 600)},
                                               {"level", "raw"},
                                           }))
                                     .query);
    ASSERT_TRUE(body.value("available", false));
    ASSERT_EQ(body["series"][0]["points"].size(), 2U);
    // 最新一行是 60 秒
    EXPECT_EQ(body["interval_sec"].get<int64_t>(), 60);
}

TEST_F(SysmonHistoryServiceTest, DimensionRawQueryDerivesIntervalAndDetectsGaps) {
    // 长表没有 interval_sec 列：原始维度查询必须从相邻桶时刻反推桶宽，
    // 否则 interval_sec 恒为 null 且缺口检测被整体跳过（缺口静默消失）。
    seed([](HistoryStore&, HistoryBatch& batch) {
        for (const int64_t offset : {int64_t{0}, int64_t{60}, int64_t{600}}) {
            dztrader::sysmonstore::DimensionRecord record;
            record.level = dztrader::sysmonstore::kLevelRaw;
            record.bucket_ts = kBaseTs + offset;
            record.domain = std::string(dztrader::sysmonstore::kNetworkDomain);
            record.dimension = "eth0";
            record.metric = "rx_bytes_per_sec";
            record.aggregate.observe(1.0);
            batch.dimensions.push_back(record);
        }
    });
    const auto svc = service();
    const auto body = svc.query(*svc.parse(params({
                                               {"metric", "network.rx_bytes_per_sec"},
                                               {"dimension", "eth0"},
                                               {"from", std::to_string(kBaseTs)},
                                               {"to", std::to_string(kBaseTs + 1200)},
                                               {"level", "raw"},
                                           }))
                                     .query);
    ASSERT_TRUE(body.value("available", false));
    ASSERT_EQ(body["series"][0]["points"].size(), 3U);
    // 相邻最小间隔 = 60 ⇒ 名义桶宽 60（不是 null）
    EXPECT_EQ(body["interval_sec"].get<int64_t>(), 60);
    // 缺口 [base+120, base+600) 必须被检出（不是静默消失）
    ASSERT_EQ(body["gaps"].size(), 1U);
    EXPECT_EQ(body["gaps"][0]["from"].get<int64_t>(), kBaseTs + 120);
    EXPECT_EQ(body["gaps"][0]["to"].get<int64_t>(), kBaseTs + 600);
}

TEST_F(SysmonHistoryServiceTest, MinuteLevelIntervalIsNullWhenNoBuckets) {
    // 无桶即无桶宽：分钟层不得在空结果时硬报 60（那会让前端以为有数据可画）
    create_empty_db();
    const auto svc = service();
    const auto body = svc.query(*svc.parse(params({
                                               {"from", std::to_string(kBaseTs)},
                                               {"to", std::to_string(kBaseTs + 7200)},
                                               {"level", "minute"},
                                           }))
                                     .query);
    ASSERT_TRUE(body.value("available", false));
    EXPECT_TRUE(body["series"][0]["points"].empty());
    EXPECT_TRUE(body["interval_sec"].is_null());
}

TEST_F(SysmonHistoryServiceTest, SingleDimensionPointHasNullInterval) {
    // 单点无从反推桶宽：输出 null 而不是猜一个值
    seed([](HistoryStore&, HistoryBatch& batch) {
        dztrader::sysmonstore::DimensionRecord record;
        record.level = dztrader::sysmonstore::kLevelRaw;
        record.bucket_ts = kBaseTs;
        record.domain = std::string(dztrader::sysmonstore::kNetworkDomain);
        record.dimension = "eth0";
        record.metric = "rx_bytes_per_sec";
        record.aggregate.observe(1.0);
        batch.dimensions.push_back(record);
    });
    const auto svc = service();
    const auto body = svc.query(*svc.parse(params({
                                               {"metric", "network.rx_bytes_per_sec"},
                                               {"dimension", "eth0"},
                                               {"from", std::to_string(kBaseTs)},
                                               {"to", std::to_string(kBaseTs + 600)},
                                               {"level", "raw"},
                                           }))
                                     .query);
    ASSERT_TRUE(body.value("available", false));
    ASSERT_EQ(body["series"][0]["points"].size(), 1U);
    EXPECT_TRUE(body["interval_sec"].is_null());
    EXPECT_TRUE(body["gaps"].empty());
}

TEST_F(SysmonHistoryServiceTest, AutoFallsBackWhenPreferredLevelHasNoRows) {
    // **默认部署的关键路径**：压缩只在 record_interval_sec < 60 时发生，默认是 60 ⇒
    // 一分钟聚合层从不产出。若 auto 死守「>1 小时选 minute」而不回退，默认配置下所有
    // >1 小时的范围（含默认 6 小时视图）都会返回空序列——功能整体失效。
    // 这里只写原始记录、请求 2 小时范围（>1h ⇒ auto 首选 minute），必须回退到 raw。
    seed([](HistoryStore&, HistoryBatch& batch) {
        for (int i = 0; i < 20; ++i) {
            batch.scalars.push_back(make_scalar(kBaseTs + i * 300, 1.0));
        }
    });
    const auto svc = service();
    const auto body = svc.query(*svc.parse(params({
                                               {"from", std::to_string(kBaseTs)},
                                               {"to", std::to_string(kBaseTs + 7200)},
                                           }))
                                     .query);
    ASSERT_TRUE(body.value("available", false));
    // 回退发生了：实际层级回显为 raw（不静默假装 minute 有数据）
    EXPECT_EQ(body.value("level", ""), "raw");
    ASSERT_EQ(body["series"][0]["points"].size(), 20U);
    EXPECT_FALSE(body.value("truncated", true));
}

TEST_F(SysmonHistoryServiceTest, AutoPrefersMinuteWhenItHasRows) {
    // 反向保证：当一分钟聚合层确有数据时，auto 仍优先用它（不因回退逻辑破坏原语义）
    seed([](HistoryStore&, HistoryBatch& batch) {
        batch.scalars.push_back(make_scalar(kBaseTs, 1.0));
        batch.scalars.push_back(make_scalar(kBaseTs, 2.0, dztrader::sysmonstore::kLevelMinute, 60));
    });
    const auto svc = service();
    const auto body = svc.query(*svc.parse(params({
                                               {"from", std::to_string(kBaseTs)},
                                               {"to", std::to_string(kBaseTs + 7200)},
                                           }))
                                     .query);
    ASSERT_TRUE(body.value("available", false));
    EXPECT_EQ(body.value("level", ""), "minute");
    ASSERT_EQ(body["series"][0]["points"].size(), 1U);
    EXPECT_DOUBLE_EQ(body["series"][0]["points"][0]["max"].get<double>(), 2.0);
}

TEST_F(SysmonHistoryServiceTest, ExplicitLevelDoesNotFallBack) {
    // 显式给 level 时不回退：用户要的就是该层级，空就如实空（回退只属于 auto）
    seed([](HistoryStore&, HistoryBatch& batch) {
        batch.scalars.push_back(make_scalar(kBaseTs, 1.0));  // 只有原始记录
    });
    const auto svc = service();
    const auto body = svc.query(*svc.parse(params({
                                               {"from", std::to_string(kBaseTs)},
                                               {"to", std::to_string(kBaseTs + 7200)},
                                               {"level", "minute"},
                                           }))
                                     .query);
    ASSERT_TRUE(body.value("available", false));
    EXPECT_EQ(body.value("level", ""), "minute");
    EXPECT_TRUE(body["series"][0]["points"].empty());
}

}  // namespace
}  // namespace dztrader::webui
