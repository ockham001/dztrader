/**
 * @file schema_test.cpp
 * @brief 历史库 schema 的单一真相源与版本语义（#78 验收：schema 定义 + 版本）。
 *
 * 断言的是**对外承诺**：schema 版本号、指标键集合的完整性、DDL 与注册表一致、
 * 未注册的域/指标被拒绝。不断言 DDL 文本（那是实现细节，重构会白红）。
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <set>
#include <string>
#include <string_view>

#include <dztrader/sysmonstore/records.h>
#include <dztrader/sysmonstore/schema.h>

namespace dztrader::sysmonstore {
namespace {

TEST(SysmonStoreSchemaTest, SchemaVersionIsPositiveAndStable) {
    // 版本号是读取方判断「能否解码」的唯一依据；0 表示未初始化，因此必须为正。
    EXPECT_GT(kSchemaVersion, 0);
}

TEST(SysmonStoreSchemaTest, ScalarMetricKeysAndColumnsAreUnique) {
    std::set<std::string_view> keys;
    std::set<std::string_view> columns;
    for (const auto& metric : kScalarMetrics) {
        EXPECT_FALSE(metric.key.empty()) << "metric key must not be empty";
        EXPECT_FALSE(metric.column.empty()) << "metric column must not be empty";
        EXPECT_TRUE(keys.insert(metric.key).second) << "duplicate metric key: " << metric.key;
        EXPECT_TRUE(columns.insert(metric.column).second)
            << "duplicate metric column: " << metric.column;
    }
}

TEST(SysmonStoreSchemaTest, ScalarMetricsCoverEverySnapshotDomain) {
    // 契约 sysmon §2 的固定标量域：CPU 使用率/负载/速率、内存、交换、共享内存目录。
    // 少一个域意味着「历史里永远看不到该指标」，因此逐个断言键存在。
    const char* required[] = {
        "cpu.usage_ratio",
        "cpu.load_average.one",
        "cpu.load_average.five",
        "cpu.load_average.fifteen",
        "cpu.context_switches_per_sec",
        "cpu.interrupts_per_sec",
        "memory.total_bytes",
        "memory.available_bytes",
        "memory.used_bytes",
        "memory.used_ratio",
        "memory.cached_bytes",
        "memory.buffers_bytes",
        "swap.total_bytes",
        "swap.used_bytes",
        "swap.used_ratio",
        "shm_dir.total_bytes",
        "shm_dir.page_files",
        "shm_dir.oldest_file_age_sec",
    };
    for (const char* key : required) {
        EXPECT_NE(find_scalar_metric(key), nullptr) << "missing scalar metric: " << key;
    }
}

TEST(SysmonStoreSchemaTest, FindScalarMetricResolvesByKeyAndColumn) {
    const auto* by_key = find_scalar_metric("memory.used_bytes");
    ASSERT_NE(by_key, nullptr);
    // 字节类指标必须是 INTEGER：用 REAL 存 64 位字节数在超过 2^53 后失去精度。
    EXPECT_EQ(by_key->type, ScalarType::Integer);
    const auto* by_column = find_scalar_metric_by_column("memory_used_bytes");
    ASSERT_NE(by_column, nullptr);
    EXPECT_EQ(by_column->key, by_key->key);
}

TEST(SysmonStoreSchemaTest, UnknownScalarMetricReturnsNull) {
    EXPECT_EQ(find_scalar_metric("memory.not_a_metric"), nullptr);
    EXPECT_EQ(find_scalar_metric_by_column("not_a_column"), nullptr);
}

TEST(SysmonStoreSchemaTest, DimensionDomainsCoverEveryLongTableMetric) {
    // 长表覆盖#41 spec 要求的「按核 / 盘 / 网卡展开」+ 卷与温度传感器。
    for (const char* domain : {"cpu_core", "volume", "disk_io", "network", "temperature"}) {
        EXPECT_NE(find_dimension_domain(domain), nullptr) << "missing domain: " << domain;
    }
}

TEST(SysmonStoreSchemaTest, DimensionMetricMembershipIsEnforced) {
    EXPECT_TRUE(dimension_domain_has_metric(kCpuCoreDomain, "usage_ratio"));
    EXPECT_TRUE(dimension_domain_has_metric(kNetworkDomain, "rx_bytes_total"));
    // 域的指标名是白名单：错拼/跨域混用必须被拒（否则脏指标名会落库，读取方永远查不到）。
    EXPECT_FALSE(dimension_domain_has_metric(kCpuCoreDomain, "rx_bytes_total"));
    EXPECT_FALSE(dimension_domain_has_metric(kNetworkDomain, "usage_ratio"));
    EXPECT_FALSE(dimension_domain_has_metric("not_a_domain", "usage_ratio"));
}

TEST(SysmonStoreSchemaTest, EveryDimensionMetricIsUniqueWithinDomain) {
    for (const auto& spec : kDimensionDomains) {
        std::set<std::string_view> metrics;
        for (const auto metric : spec.metrics) {
            EXPECT_TRUE(metrics.insert(metric).second)
                << "duplicate metric in domain " << spec.domain << ": " << metric;
        }
    }
}

TEST(SysmonStoreSchemaTest, MarkKindsAreRegistered) {
    EXPECT_TRUE(is_known_mark_kind(kMarkSampleGap));
    EXPECT_TRUE(is_known_mark_kind(kMarkRestart));
    EXPECT_TRUE(is_known_mark_kind(kMarkClockJump));
    EXPECT_FALSE(is_known_mark_kind("not_a_mark"));
}

TEST(SysmonStoreSchemaTest, SchemaDdlDeclaresEveryTableAndScalarColumn) {
    const std::string ddl = build_schema_ddl();
    // 四张表必须都建（缺表会让写/读/维护路径之一在运行期才炸）。
    for (const auto table : {kTableMeta, kTableScalar, kTableDimension, kTableMark}) {
        EXPECT_NE(ddl.find("CREATE TABLE IF NOT EXISTS " + std::string(table)), std::string::npos)
            << "ddl missing table: " << table;
    }
    // 每个注册的标量指标都必须有对应的物理列（5 个聚合列各一），否则写入方的
    // UPSERT 列清单与实际表结构不一致，运行期才报错。
    for (const auto& metric : kScalarMetrics) {
        for (const char* suffix : {"min", "max", "avg", "last", "n"}) {
            const std::string column = std::string(metric.column) + "_" + suffix;
            EXPECT_NE(ddl.find(column), std::string::npos)
                << "ddl missing column for registered metric: " << column;
        }
    }
}

TEST(SysmonStoreSchemaTest, SchemaDdlIsIdempotentByConstruction) {
    // 全部语句必须带 IF NOT EXISTS：dzsysmon 每次启动都会执行 DDL，不能因为库已存在而失败。
    const std::string ddl = build_schema_ddl();
    std::size_t create_count = 0;
    std::size_t guarded_count = 0;
    for (std::size_t pos = ddl.find("CREATE "); pos != std::string::npos;
         pos = ddl.find("CREATE ", pos + 1)) {
        ++create_count;
        const std::string tail = ddl.substr(pos, 40);
        if (tail.find("IF NOT EXISTS") != std::string::npos) {
            ++guarded_count;
        }
    }
    EXPECT_GT(create_count, 0U);
    EXPECT_EQ(create_count, guarded_count) << "every DDL statement must be idempotent";
}

TEST(SysmonStoreSchemaTest, HistoryFileLivesUnderDbDirectoryName) {
    // 历史库必须与交易库、WebUI 库分离（#41 spec）：只断言文件名，目录由消费方拼。
    EXPECT_EQ(kHistoryFileName, "sysmon.db");
}

}  // namespace
}  // namespace dztrader::sysmonstore
