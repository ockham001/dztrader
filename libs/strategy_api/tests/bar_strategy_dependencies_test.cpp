/**
 * @file bar_strategy_dependencies_test.cpp
 * @brief 内部依赖缝的默认实现测试（issue #161；接缝 B/C 的交界）
 *
 * 框架的三件外部数据（历史 bar / 交易小节表 / 合约元数据）都经内部接缝注入。本文件验证
 * **默认实现**——也就是生产路径实际会走的两条：
 *
 *   1. `load_archive_metadata`：`dz_db_query_instruments` 解码。用真实 td 库（SQLite +
 *      tdstore schema）构造平台侧快照，断言「必需字段齐 → Loaded」「没有该合约 → Missing」
 *      「字段非法 → Invalid」「库不可达 → SourceUnavailable」四种结局。四条必须区分开：
 *      启动时前一条会被当作**该实例创建失败**（进程继续），后一条才是「本部署没有元数据
 *      源」的降级（`InstrumentSpec` 全 0 + 已知限制诊断）。
 *   2. `query_archive_history`：归档域未实现（`dz_db_query_bar` 恒返回 NULL +
 *      `DZ_EC_NOT_IMPLEMENTED`，[ADR
 * 0018](../adr/0018-strategy-side-kline-and-trading-sessions.md)/[ADR
 * 0019](../adr/0019-strategy-sdk-db-backend-scope.md)）， 必须按「无历史」返回 Unavailable
 * 而不是错误——历史是增强项，不阻断启动。
 *
 * 默认实现在 Seam B 里被 fake 替换，因此这里只锁「默认实现自己的口径」，不碰帧通道。
 *
 * **覆盖边界（已知限制）**：`query_archive_history` 的**行解码**分支在归档域落地（#4）前
 * 不可达——`dz_db_query_bar` 恒返回 NULL（ADR 0018/0019），因此本文件只能覆盖调用契约与
 * 「未实现 → Unavailable」的降级映射；解码契约（列名 = `bar::Bar` 字段名）在 #4 同步时补
 * 端到端测试。
 */
#include <gtest/gtest.h>

#include <dztrader/api.h>
#include <dztrader/bar_strategy/dependencies.h>
#include <dztrader/db/database.h>
#include <dztrader/tdstore/records.h>
#include <dztrader/tdstore/records_store.h>
#include <dztrader/tdstore/schema_catalog.h>

#include <filesystem>
#include <string>
#include <vector>

#include "test_temp_dir.h"

namespace {

using dztrader::bar_strategy_internal::FrameworkDependencies;
using dztrader::bar_strategy_internal::HistoryQuery;
using dztrader::bar_strategy_internal::HistoryStatus;
using dztrader::bar_strategy_internal::load_archive_metadata;
using dztrader::bar_strategy_internal::MetadataResult;
using dztrader::bar_strategy_internal::MetadataStatus;
using dztrader::bar_strategy_internal::query_archive_history;

/// @brief 依赖缝默认实现测试夹具：临时目录里建一份真实 td 库（instruments 表）
class BarStrategyDependenciesTest : public ::testing::Test {
protected:
    void SetUp() override {
        const std::filesystem::path dir =
            dztrader::test::unique_temp_dir("dz_test_bar_dependencies");
        std::filesystem::create_directories(dir);
        dir_ = dir;
        const std::string db_path = (dir / "td.db").string();

        auto database = dztrader::db::Database::open(
            dztrader::db::Config{.backend = "sqlite", .options = {{"path", db_path}}},
            dztrader::tdstore::schemas());
        database->migrate();
        auto session = database->session();
        const std::vector<dztrader::tdstore::InstrumentRecord> records{
            make_instrument("rb2601", "SHFE", 10.0, 0.5),
            // volume_multiple 必须 > 0：0 会被默认实现判成 Invalid
            make_instrument("broken_multiple", "SHFE", 0.0, 0.5),
            // price_tick 必须 > 0：0 同上
            make_instrument("broken_tick", "DCE", 10.0, 0.0),
        };
        dztrader::tdstore::upsert_instruments(*session, records);

        // dz_db_open 是显式路径（SQLite）语义：与默认配置解析无关，测试不依赖 DZTRADER_HOME
        db_ = dz_db_open(db_path.c_str());
        ASSERT_NE(nullptr, db_) << "dz_db_open failed: " << dz_errmsg();
    }

    void TearDown() override {
        if (db_ != nullptr) {
            dz_db_close(db_);
            db_ = nullptr;
        }
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    /// @brief 一条合约元数据记录（只填元数据缝关心的字段，其余取 schema 默认）
    static dztrader::tdstore::InstrumentRecord make_instrument(const std::string& instrument_id,
                                                               const std::string& exchange_id,
                                                               double volume_multiple,
                                                               double price_tick) {
        dztrader::tdstore::InstrumentRecord record{};
        record.instrument_id = instrument_id;
        record.exchange_id = exchange_id;
        record.symbol = instrument_id;
        record.name = instrument_id;
        record.product_class = DZ_PRODUCT_FUTURES;
        record.product_code = "rb";
        record.settle_cycle = 1;
        record.currency = "CNY";
        record.volume_multiple = volume_multiple;
        record.volume_step = 1.0;
        record.price_tick = price_tick;
        record.min_limit_order_volume = 1;
        record.max_limit_order_volume = 500;
        record.listed_date = 20090;
        record.delisted_date = 20818;
        record.update_day = "20260915";
        record.updated_at = 1757923200000;
        return record;
    }

    std::filesystem::path dir_;
    DzDatabase* db_ = nullptr;
};

// ============================================================
// 合约元数据：四种结局必须互相区分
// ============================================================

TEST_F(BarStrategyDependenciesTest, MetadataLoadedDecodesRequiredFields) {
    const MetadataResult result = load_archive_metadata(db_, "rb2601");
    ASSERT_EQ(result.status, MetadataStatus::Loaded) << result.message;
    EXPECT_EQ(result.spec.exchange_id, "SHFE");
    EXPECT_DOUBLE_EQ(result.spec.volume_multiple, 10.0);
    EXPECT_DOUBLE_EQ(result.spec.price_tick, 0.5);
}

/// 库可达但查不到该合约 → Missing（启动时该实例创建失败，进程继续）
TEST_F(BarStrategyDependenciesTest, MetadataMissingForUnknownInstrument) {
    const MetadataResult result = load_archive_metadata(db_, "rb9999");
    EXPECT_EQ(result.status, MetadataStatus::Missing);
    EXPECT_FALSE(result.message.empty()) << "a missing contract must carry a diagnostic message";
}

/// 库可达、行存在但必需字段非法（乘数 0）→ Invalid
TEST_F(BarStrategyDependenciesTest, MetadataInvalidWhenVolumeMultipleIsNotPositive) {
    const MetadataResult result = load_archive_metadata(db_, "broken_multiple");
    EXPECT_EQ(result.status, MetadataStatus::Invalid);
    EXPECT_FALSE(result.message.empty());
    EXPECT_NE(result.message.find("volume_multiple"), std::string::npos) << result.message;
}

/// 库可达、行存在但必需字段非法（最小变动价位 0）→ Invalid
TEST_F(BarStrategyDependenciesTest, MetadataInvalidWhenPriceTickIsNotPositive) {
    const MetadataResult result = load_archive_metadata(db_, "broken_tick");
    EXPECT_EQ(result.status, MetadataStatus::Invalid);
    EXPECT_FALSE(result.message.empty());
    EXPECT_NE(result.message.find("price_tick"), std::string::npos) << result.message;
}

/// 库不可达（v1 无 platform.db 的常态）→ SourceUnavailable（降级、不判实例失败）
TEST_F(BarStrategyDependenciesTest, MetadataSourceUnavailableWithoutDatabase) {
    const MetadataResult result = load_archive_metadata(nullptr, "rb2601");
    EXPECT_EQ(result.status, MetadataStatus::SourceUnavailable);
    EXPECT_DOUBLE_EQ(result.spec.volume_multiple, 0.0);
    EXPECT_DOUBLE_EQ(result.spec.price_tick, 0.0);
}

// ============================================================
// 历史 bar：归档域未实现 → 按「无历史」降级
// ============================================================

/// @brief 一次最简历史查询（bars 模式，与候选实现口径一致）
HistoryQuery make_query(std::int32_t warmup_bars = 10000) {
    HistoryQuery query;
    query.instrument_id = "rb2601";
    query.period_sec = 60;
    query.max_lookback = 1440;
    query.warmup_bars = warmup_bars;
    return query;
}

/**
 * `dz_db_query_bar` 当前是 stub（恒 NULL + `DZ_EC_NOT_IMPLEMENTED`）：默认实现必须把它当成
 * 「没有历史数据」（`Unavailable`）而不是错误（`Failed`）——否则冷启动会被一个未实现的接口
 * 阻断。错误消息的**措辞归 SDK 契约**（`dz_errmsg()`），这里只断言状态映射与非空诊断，不
 * 把测试钉在 stub 的具体文案上。
 */
TEST_F(BarStrategyDependenciesTest, HistoryUnavailableWhileArchiveDomainIsUnimplemented) {
    const auto result = query_archive_history(db_, make_query());
    EXPECT_EQ(result.status, HistoryStatus::Unavailable);
    EXPECT_TRUE(result.bars.empty());
    EXPECT_FALSE(result.message.empty()) << "a degraded source must carry a diagnostic message";
}

/// 库句柄不可用（无 platform.db / 打开失败）→ Unavailable，不尝试查询
TEST_F(BarStrategyDependenciesTest, HistoryUnavailableWithoutDatabase) {
    const auto result = query_archive_history(nullptr, make_query());
    EXPECT_EQ(result.status, HistoryStatus::Unavailable);
    EXPECT_TRUE(result.bars.empty());
}

// ============================================================
// 依赖聚合的默认值（生产入口不显式注入时框架的行为）
// ============================================================

/// `FrameworkDependencies` 默认 = 退化小节表 + 默认回卷时刻 + 空 loader（走默认实现）
TEST(BarStrategyDependenciesTestDefault, DefaultsUseDegradedTableAndDefaultClock) {
    const FrameworkDependencies dependencies{};
    EXPECT_TRUE(dztrader::is_degraded_session_table(dependencies.session_table));
    EXPECT_EQ(dependencies.rollback_clock_ms, dztrader::DEFAULT_ROLLBACK_CLOCK_MS);
    EXPECT_FALSE(static_cast<bool>(dependencies.history))
        << "an empty loader means the default archive implementation";
    EXPECT_FALSE(static_cast<bool>(dependencies.metadata))
        << "an empty loader means the default archive implementation";
}

}  // namespace
