/**
 * @file td_orders_query_test.cpp
 * @brief `GET /api/td/orders` 直查实现（契约 rest §2.9.1/§2.9.2/§2.9.3/§2.9.6/§2.9.8）。
 *
 * 测试准则（docs/unit-test-standard.md）：断言外部可观察行为——响应信封、行 JSON、
 * 游标分页的稳定性、降级 reason 码。不断言 SQL 文本与私有成员。
 *
 * 库形态用**真 schema**（`tdstore::ensure_trade_db` 建库）：测试若与 schema 漂移
 * （列名/列序变化）必须变红，而不是靠手抄 DDL 掩盖。委托行写入统一走共享夹具
 * `td_order_row_writer.h`（列清单的唯一实现，unit-test-standard §5.4）。
 */

#include "td_orders_query.h"
#include "td_order_row_writer.h"
#include "test_temp_dir.h"

#include <dztrader/tdstore/schema_catalog.h>
#include <dztrader/tdstore/trade_db.h>
#include <gtest/gtest.h>
#include <sqlite3.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace dztrader::webui {
namespace {

/// 一条委托的构造参数：字段清单来自共享夹具（`td_order_row_writer.h`），默认账户 ACC1。
using OrderSeed = dztrader::test::TdOrderRowSeed;

// dzweb 生产链接不引入 dztdstore（只读库，不需要建库/门面），schema 版本的耦合由这里守：
// `kTdOrdersReadSchemaVersion` 与建库单源声明不一致时本文件编不过，而不是运行时静默降级。
static_assert(kTdOrdersReadSchemaVersion == dztrader::tdstore::kTdSchemaVersion,
              "dzweb read schema version drifted from tdstore::kTdSchemaVersion");

int exec_sql(sqlite3* db, const char* sql) {
    char* err = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        const std::string message = err != nullptr ? err : sqlite3_errmsg(db);
        sqlite3_free(err);
        ADD_FAILURE() << "sqlite3_exec failed: " << message << " | sql=" << sql;
    }
    return rc;
}

class TdOrdersQueryTest : public ::testing::Test {
protected:
    std::filesystem::path dir_;
    std::string db_path_;

    void SetUp() override {
        dir_ = dztrader::test::unique_temp_dir("dz_webui_td_orders_query_test");
        std::filesystem::create_directories(dir_);
        db_path_ = (dir_ / "trade.db").string();
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    /// 真 schema 建库（列名/键/索引与生产同源）。
    void create_db() { (void)dztrader::tdstore::ensure_trade_db(db_path_); }

    sqlite3* open() {
        sqlite3* db = nullptr;
        EXPECT_EQ(SQLITE_OK, sqlite3_open(db_path_.c_str(), &db));
        return db;
    }

    /// 写一行委托（共享夹具 `td_order_row_writer.h` 是列清单的唯一实现）。
    void insert_order(const OrderSeed& seed) {
        const std::string error = dztrader::test::insert_td_order_row(db_path_, seed);
        ASSERT_TRUE(error.empty()) << error;
    }

    void insert_instrument(const std::string& instrument_id, double price_tick) {
        sqlite3* db = open();
        sqlite3_stmt* stmt = nullptr;
        const char* sql =
            "INSERT INTO instruments (instrument_id, exchange_id, price_tick)"
            " VALUES (?, 'SHFE', ?)";
        ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr));
        sqlite3_bind_text(stmt, 1, instrument_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(stmt, 2, price_tick);
        EXPECT_EQ(SQLITE_DONE, sqlite3_step(stmt));
        sqlite3_finalize(stmt);
        sqlite3_close(db);
    }

    TdOrdersQuery query() {
        return TdOrdersQuery([this]() { return db_path_; }, 200);
    }

    static std::function<std::string(std::string_view)> params(
        std::map<std::string, std::string> values) {
        return [values = std::move(values)](std::string_view key) -> std::string {
            auto it = values.find(std::string(key));
            return it == values.end() ? std::string{} : it->second;
        };
    }

    /// 以默认在线集合（ACC1/ACC2）与交易日执行查询；非法游标/参数时返回 nullopt。
    std::optional<nlohmann::json> run(const std::map<std::string, std::string>& raw,
                                      std::vector<std::string> online = {"ACC1", "ACC2"},
                                      std::optional<std::string> day = std::string("20261009")) {
        const auto parsed = TdOrdersQuery::parse(params(raw));
        if (!parsed.has_value()) {
            return std::nullopt;
        }
        return query().execute(*parsed, online, day);
    }
};

// ===== 信封与空语义（契约 rest §2.9.1/§2.9.2）=====

TEST_F(TdOrdersQueryTest, EmptyDatabaseReturnsAvailableEnvelopeWithNoItems) {
    create_db();
    const auto body = run({});
    ASSERT_TRUE(body.has_value());
    EXPECT_TRUE((*body)["available"].get<bool>());
    EXPECT_EQ((*body)["trading_day"].get<std::string>(), "20261009");
    EXPECT_TRUE((*body)["online_accounts"].is_array());
    EXPECT_EQ((*body)["online_accounts"].size(), 2U);
    EXPECT_TRUE((*body)["items"].is_array());
    EXPECT_TRUE((*body)["items"].empty());
    EXPECT_TRUE((*body)["next_cursor"].is_null());
}

TEST_F(TdOrdersQueryTest, NoOnlineAccountsYieldsNullTradingDayAndEmptyItems) {
    create_db();
    insert_order(OrderSeed{});
    const auto body = run({}, /*online=*/{}, /*day=*/std::nullopt);
    ASSERT_TRUE(body.has_value());
    EXPECT_TRUE((*body)["available"].get<bool>());
    EXPECT_TRUE((*body)["trading_day"].is_null());
    EXPECT_TRUE((*body)["items"].empty());
    EXPECT_TRUE((*body)["next_cursor"].is_null());
}

TEST_F(TdOrdersQueryTest, NoOnlineAccountsStillReportsDegradationWhenDatabaseUnreadable) {
    // 无在线账户但库不存在：必须降级（不能拿空 items 冒充「今天没数据」，契约 §2.9.8）
    const auto body = run({}, /*online=*/{}, /*day=*/std::nullopt);
    ASSERT_TRUE(body.has_value());
    EXPECT_FALSE((*body)["available"].get<bool>());
    EXPECT_EQ((*body)["reason"].get<std::string>(), "not_found");
    EXPECT_FALSE((*body).contains("items"));
}

// ===== 行 schema（契约 rest §2.9.3）=====

TEST_F(TdOrdersQueryTest, RowCarriesContractFieldsWithPascalCaseEnums) {
    create_db();
    insert_instrument("rb2601", 0.5);
    OrderSeed seed;
    seed.order_id = 10231;
    seed.status = 3;  // PartTraded
    seed.volume = 2;
    seed.volume_traded = 1;
    seed.avg_price = 3512.3;
    seed.price = 3512.5;
    seed.time = 32400;
    seed.seq = 1842;
    insert_order(seed);

    const auto body = run({});
    ASSERT_TRUE(body.has_value());
    const auto& row = (*body)["items"][0];
    EXPECT_EQ(row["account_id"].get<std::string>(), "ACC1");
    EXPECT_EQ(row["date"].get<std::string>(), "20261009");
    EXPECT_EQ(row["order_id"].get<int64_t>(), 10231);
    EXPECT_EQ(row["strategy_id"].get<std::string>(), "stg1");
    EXPECT_EQ(row["instrument_id"].get<std::string>(), "rb2601");
    EXPECT_EQ(row["exchange_id"].get<std::string>(), "SHFE");
    EXPECT_EQ(row["direction"].get<std::string>(), "Long");
    EXPECT_EQ(row["position_effect"].get<std::string>(), "Open");
    EXPECT_EQ(row["price_type"].get<std::string>(), "Limit");
    EXPECT_EQ(row["status"].get<std::string>(), "PartTraded");
    EXPECT_DOUBLE_EQ(row["price"].get<double>(), 3512.5);
    EXPECT_EQ(row["volume"].get<int>(), 2);
    EXPECT_EQ(row["volume_traded"].get<int>(), 1);
    EXPECT_EQ(row["remark"].get<std::string>(), "");
    EXPECT_EQ(row["time"].get<int>(), 32400);
    EXPECT_DOUBLE_EQ(row["avg_price"].get<double>(), 3512.3);
    EXPECT_EQ(row["price_decimals"].get<int>(), 1);
    EXPECT_EQ(row["avg_price_decimals"].get<int>(), 3);
    EXPECT_EQ(row["seq"].get<uint64_t>(), 1842U);
    EXPECT_TRUE(row["sort_key"].is_string());
    EXPECT_FALSE(row["sort_key"].get<std::string>().empty());
}

TEST_F(TdOrdersQueryTest, AvgPriceIsNullWhenNothingTraded) {
    create_db();
    OrderSeed seed;
    seed.volume_traded = 0;
    seed.avg_price = 0.0;
    insert_order(seed);
    const auto body = run({});
    ASSERT_TRUE(body.has_value());
    EXPECT_TRUE((*body)["items"][0]["avg_price"].is_null());
}

// ===== 排序（契约 rest §2.9.6：18:00 锚点、同秒 order_id 降序）=====

TEST_F(TdOrdersQueryTest, OrdersByTimeAnchorDescendingWithNightSessionAfterDaySession) {
    create_db();
    // 夜盘 21:00 = 75600 秒 → 锚点 (75600+21600)%86400 = 10800
    // 日盘 09:00 = 32400 → 锚点 54000；日盘 14:00 = 50400 → 锚点 72000
    OrderSeed day_09;
    day_09.order_id = 1;
    day_09.time = 32400;
    insert_order(day_09);
    OrderSeed day_14;
    day_14.order_id = 2;
    day_14.time = 50400;
    insert_order(day_14);
    OrderSeed night;
    night.order_id = 3;
    night.time = 75600;
    insert_order(night);

    const auto body = run({});
    ASSERT_TRUE(body.has_value());
    const auto& items = (*body)["items"];
    ASSERT_EQ(items.size(), 3U);
    // 14:00(锚点 72000) > 09:00(54000) > 夜盘(10800)：夜盘排在日盘之后
    EXPECT_EQ(items[0]["order_id"].get<int64_t>(), 2);
    EXPECT_EQ(items[1]["order_id"].get<int64_t>(), 1);
    EXPECT_EQ(items[2]["order_id"].get<int64_t>(), 3);
    // sort_key 字符串降序 = 列表顺序（契约 §2.9.6 性质 1）
    EXPECT_GT(items[0]["sort_key"].get<std::string>(), items[1]["sort_key"].get<std::string>());
    EXPECT_GT(items[1]["sort_key"].get<std::string>(), items[2]["sort_key"].get<std::string>());
}

TEST_F(TdOrdersQueryTest, SameSecondBreaksTieByOrderIdDescending) {
    create_db();
    for (int64_t order_id : {int64_t{101}, int64_t{305}, int64_t{202}}) {
        OrderSeed seed;
        seed.order_id = order_id;
        seed.time = 32400;
        insert_order(seed);
    }
    const auto body = run({});
    ASSERT_TRUE(body.has_value());
    const auto& items = (*body)["items"];
    ASSERT_EQ(items.size(), 3U);
    EXPECT_EQ(items[0]["order_id"].get<int64_t>(), 305);
    EXPECT_EQ(items[1]["order_id"].get<int64_t>(), 202);
    EXPECT_EQ(items[2]["order_id"].get<int64_t>(), 101);
}

// ===== 分页（契约 rest §2.9.2：默认 200、上限 1000、禁 OFFSET、不重不漏）=====

TEST_F(TdOrdersQueryTest, DefaultLimitIsTwoHundredRows) {
    create_db();
    for (int32_t i = 0; i < 205; ++i) {
        OrderSeed seed;
        seed.order_id = 1000 + i;
        seed.time = 32400 + i;
        insert_order(seed);
    }
    const auto body = run({});
    ASSERT_TRUE(body.has_value());
    EXPECT_EQ((*body)["items"].size(), 200U);
    EXPECT_TRUE((*body)["next_cursor"].is_string());
}

TEST_F(TdOrdersQueryTest, CursorPaginationIsStableAndLossless) {
    create_db();
    constexpr int kTotal = 25;
    for (int32_t i = 0; i < kTotal; ++i) {
        OrderSeed seed;
        seed.order_id = 1 + i;
        // 同秒 → 由 order_id 破并列，覆盖游标第二坐标
        seed.time = 32400;
        insert_order(seed);
    }

    std::vector<int64_t> seen;
    std::string cursor;
    std::string previous_sort_key;
    for (int page = 0; page < 10; ++page) {
        std::map<std::string, std::string> raw;
        raw["limit"] = "10";
        if (!cursor.empty()) {
            raw["cursor"] = cursor;
        }
        const auto body = run(raw);
        ASSERT_TRUE(body.has_value()) << "page " << page;
        EXPECT_TRUE((*body)["available"].get<bool>());
        const auto& items = (*body)["items"];
        for (const auto& row : items) {
            const int64_t order_id = row["order_id"].get<int64_t>();
            EXPECT_EQ(std::find(seen.begin(), seen.end(), order_id), seen.end())
                << "duplicate order_id across pages: " << order_id;
            if (!previous_sort_key.empty()) {
                EXPECT_LT(row["sort_key"].get<std::string>(), previous_sort_key)
                    << "sort_key must strictly decrease across the whole list";
            }
            previous_sort_key = row["sort_key"].get<std::string>();
            seen.push_back(order_id);
        }
        if ((*body)["next_cursor"].is_null()) {
            break;
        }
        cursor = (*body)["next_cursor"].get<std::string>();
    }
    EXPECT_EQ(seen.size(), static_cast<size_t>(kTotal));
    // 全序：order_id 降序（同秒）
    for (size_t i = 1; i < seen.size(); ++i) {
        EXPECT_GT(seen[i - 1], seen[i]);
    }
}

TEST_F(TdOrdersQueryTest, CursorBoundToScopeAccountsAndTradingDay) {
    create_db();
    for (int32_t i = 0; i < 5; ++i) {
        OrderSeed seed;
        seed.order_id = 1 + i;
        seed.time = 32400 + i;
        insert_order(seed);
    }
    const auto first = run({{"limit", "2"}});
    ASSERT_TRUE(first.has_value());
    const auto cursor = (*first)["next_cursor"].get<std::string>();

    // 作用域不符（accounts 变化）→ 400（nullopt）
    EXPECT_FALSE(run({{"limit", "2"}, {"cursor", cursor}, {"accounts", "ACC1"}}).has_value());
    // 作用域不符（交易日变化）→ 400
    EXPECT_FALSE(
        run({{"limit", "2"}, {"cursor", cursor}}, {"ACC1", "ACC2"}, std::string("20261010"))
            .has_value());
    // 同一作用域 → 正常续页
    EXPECT_TRUE(run({{"limit", "2"}, {"cursor", cursor}}).has_value());
}

TEST_F(TdOrdersQueryTest, MalformedCursorIsBadRequest) {
    create_db();
    EXPECT_FALSE(run({{"cursor", "not-a-cursor"}}).has_value());
    EXPECT_FALSE(run({{"cursor", "deadbeef"}}).has_value());
}

TEST_F(TdOrdersQueryTest, LimitOutOfRangeOrNonNumericIsBadRequest) {
    create_db();
    EXPECT_FALSE(run({{"limit", "0"}}).has_value());
    EXPECT_FALSE(run({{"limit", "1001"}}).has_value());
    EXPECT_FALSE(run({{"limit", "-1"}}).has_value());
    EXPECT_FALSE(run({{"limit", "abc"}}).has_value());
    EXPECT_FALSE(run({{"limit", "12abc"}}).has_value());
    // 边界值合法
    EXPECT_TRUE(run({{"limit", "1"}}).has_value());
    EXPECT_TRUE(run({{"limit", "1000"}}).has_value());
}

// ===== accounts 过滤（契约 rest §2.9.1：与在线集合求交，未知账户忽略）=====

TEST_F(TdOrdersQueryTest, AccountsFilterIntersectsOnlineSetAndIgnoresUnknown) {
    create_db();
    OrderSeed a;
    a.account_id = "ACC1";
    a.order_id = 1;
    a.time = 32400;
    insert_order(a);
    OrderSeed b;
    b.account_id = "ACC2";
    b.order_id = 2;
    b.time = 32401;
    insert_order(b);
    OrderSeed offline;
    offline.account_id = "ACC3";  // 不在在线集合 → 忽略
    offline.order_id = 3;
    offline.time = 32402;
    insert_order(offline);
    OrderSeed other_day;
    other_day.account_id = "ACC1";
    other_day.order_id = 4;
    other_day.time = 32403;
    other_day.date = "20261008";  // 非当前交易日 → 不返回
    insert_order(other_day);

    const auto body = run({{"accounts", "ACC1, ACC3"}});
    ASSERT_TRUE(body.has_value());
    const auto& items = (*body)["items"];
    ASSERT_EQ(items.size(), 1U);
    EXPECT_EQ(items[0]["account_id"].get<std::string>(), "ACC1");
    EXPECT_EQ(items[0]["order_id"].get<int64_t>(), 1);

    // 空串等价缺省 = 全部在线账户
    const auto all = run({{"accounts", ""}});
    ASSERT_TRUE(all.has_value());
    EXPECT_EQ((*all)["items"].size(), 2U);
}

// ===== 精度（契约 rest §2.9.6）=====

TEST_F(TdOrdersQueryTest, PriceDecimalsDerivedFromInstrumentPriceTick) {
    create_db();
    const std::vector<std::pair<std::string, double>> ticks = {
        {"tick_half", 0.5}, {"tick_cent", 0.01}, {"tick_one", 1.0}, {"tick_micro", 0.00001}};
    for (size_t i = 0; i < ticks.size(); ++i) {
        insert_instrument(ticks[i].first, ticks[i].second);
        OrderSeed seed;
        seed.order_id = static_cast<int64_t>(i) + 1;
        seed.time = 32400 + static_cast<int>(i);
        seed.instrument_id = ticks[i].first;
        insert_order(seed);
    }
    const auto body = run({});
    ASSERT_TRUE(body.has_value());
    const auto& items = (*body)["items"];
    ASSERT_EQ(items.size(), ticks.size());
    // 列表按 sort_key 降序（time 递增 → 倒序），逐行按 instrument 找期望值
    for (const auto& row : items) {
        const std::string instrument = row["instrument_id"].get<std::string>();
        for (const auto& [name, tick] : ticks) {
            if (name != instrument) {
                continue;
            }
            const int expected = name == "tick_half"   ? 1
                                 : name == "tick_cent" ? 2
                                 : name == "tick_one"  ? 0
                                                       : 5;
            EXPECT_EQ(row["price_decimals"].get<int>(), expected) << instrument;
            // 契约 rest §2.9.6：avg = clamp(price_decimals + 2, 2, 6)（tick_micro 会封顶到 6）
            const int expected_avg = std::clamp(expected + 2, 2, 6);
            EXPECT_EQ(row["avg_price_decimals"].get<int>(), expected_avg) << instrument;
        }
    }
}

TEST_F(TdOrdersQueryTest, PriceDecimalsFallBackToPriceDecimalDigitsWhenInstrumentMissing) {
    create_db();
    OrderSeed seed;
    seed.order_id = 1;
    seed.instrument_id = "no_such_instrument";
    seed.price = 552.0;  // 无小数 → 0 位
    seed.volume_traded = 3;
    seed.avg_price = 551.123;
    insert_order(seed);
    OrderSeed seed2;
    seed2.order_id = 2;
    seed2.time = 32401;
    seed2.instrument_id = "no_such_instrument";
    seed2.price = 552.5;  // 1 位
    insert_order(seed2);

    const auto body = run({});
    ASSERT_TRUE(body.has_value());
    const auto& items = (*body)["items"];
    ASSERT_EQ(items.size(), 2U);
    const auto& one_decimal = items[0];  // time 32401 在前
    EXPECT_EQ(one_decimal["price_decimals"].get<int>(), 1);
    EXPECT_EQ(one_decimal["avg_price_decimals"].get<int>(), 3);
    const auto& zero_decimal = items[1];
    EXPECT_EQ(zero_decimal["price_decimals"].get<int>(), 0);
    // 0 + 2 = 2，clamp 下限为 2
    EXPECT_EQ(zero_decimal["avg_price_decimals"].get<int>(), 2);
}

TEST_F(TdOrdersQueryTest, AvgPriceDecimalsClampedToSix) {
    create_db();
    insert_instrument("tick_micro", 0.00001);
    OrderSeed seed;
    seed.instrument_id = "tick_micro";
    insert_order(seed);
    const auto body = run({});
    ASSERT_TRUE(body.has_value());
    // price_decimals = 5 → +2 = 7 → clamp 到 6
    EXPECT_EQ((*body)["items"][0]["price_decimals"].get<int>(), 5);
    EXPECT_EQ((*body)["items"][0]["avg_price_decimals"].get<int>(), 6);
}

// ===== 降级（契约 rest §2.9.8：200 + available:false + reason，不 5xx）=====

TEST_F(TdOrdersQueryTest, DegradesWhenDatabaseMissing) {
    // 不建库
    const auto body = run({});
    ASSERT_TRUE(body.has_value());
    EXPECT_FALSE((*body)["available"].get<bool>());
    EXPECT_EQ((*body)["reason"].get<std::string>(), "not_found");
    EXPECT_FALSE((*body).contains("items"));
    EXPECT_FALSE((*body).contains("next_cursor"));
    EXPECT_FALSE((*body).contains("trading_day"));
    EXPECT_FALSE((*body).contains("online_accounts"));
}

TEST_F(TdOrdersQueryTest, DegradesOnSchemaVersionMismatch) {
    create_db();
    {
        sqlite3* db = open();
        EXPECT_EQ(SQLITE_OK, exec_sql(db, "PRAGMA user_version=999"));
        sqlite3_close(db);
    }
    const auto body = run({});
    ASSERT_TRUE(body.has_value());
    EXPECT_FALSE((*body)["available"].get<bool>());
    EXPECT_EQ((*body)["reason"].get<std::string>(), "schema_mismatch");
}

TEST_F(TdOrdersQueryTest, DegradesOnNonSqliteFile) {
    std::ofstream out(db_path_, std::ios::binary);
    ASSERT_TRUE(out.good());
    out << "this is definitely not a sqlite database file";
    out.close();

    const auto body = run({});
    ASSERT_TRUE(body.has_value());
    EXPECT_FALSE((*body)["available"].get<bool>());
    EXPECT_EQ((*body)["reason"].get<std::string>(), "open_failed");
}

TEST_F(TdOrdersQueryTest, DegradesWhenReadLockCannotBeAcquired) {
    create_db();
    // 生产库是 WAL（写不影响读），读锁竞争只出现在回滚日志模式下：先把库切回 DELETE，
    // 再用另一连接持 EXCLUSIVE，只读连接在 200ms busy_timeout 内拿不到读锁 → reason=busy。
    {
        sqlite3* setup = open();
        EXPECT_EQ(SQLITE_OK, exec_sql(setup, "PRAGMA journal_mode=DELETE"));
        sqlite3_close(setup);
    }
    sqlite3* locker = nullptr;
    ASSERT_EQ(SQLITE_OK,
              sqlite3_open_v2(db_path_.c_str(), &locker, SQLITE_OPEN_READWRITE, nullptr));
    ASSERT_EQ(SQLITE_OK, exec_sql(locker, "BEGIN EXCLUSIVE"));

    const auto body = run({});
    ASSERT_TRUE(body.has_value());
    EXPECT_FALSE((*body)["available"].get<bool>());
    EXPECT_EQ((*body)["reason"].get<std::string>(), "busy");

    EXPECT_EQ(SQLITE_OK, exec_sql(locker, "ROLLBACK"));
    sqlite3_close(locker);
}

TEST_F(TdOrdersQueryTest, DegradesOnQueryFailure) {
    create_db();
    {
        sqlite3* db = open();
        // 版本仍为 v1，但表缺失 → 查询执行失败（不是 schema 版本问题）
        EXPECT_EQ(SQLITE_OK, exec_sql(db, "DROP TABLE orders"));
        sqlite3_close(db);
    }
    const auto body = run({});
    ASSERT_TRUE(body.has_value());
    EXPECT_FALSE((*body)["available"].get<bool>());
    EXPECT_EQ((*body)["reason"].get<std::string>(), "query_failed");
}

// ===== 纯函数（契约 rest §2.9.6 的精度与排序键性质）=====

TEST(TdOrderSortKeyTest, FixedWidthStringOrderMatchesNumericOrder) {
    // 同一秒：order_id 越大 sort_key 越大（字符串比较，前端只做字符串比较）
    EXPECT_GT(td_order_sort_key(32400, 10231), td_order_sort_key(32400, 9999));
    // 时间锚不同：锚点大者 sort_key 大
    EXPECT_GT(td_order_sort_key(50400, 1), td_order_sort_key(32400, 999999));
    // 定宽：位宽相同 → 不会出现"字符数不同导致前缀比较"的歧义
    EXPECT_EQ(td_order_sort_key(0, 0).size(), td_order_sort_key(86399, INT64_MAX).size());
}

TEST(TdPriceDecimalsTest, DerivesFromTickAndFallsBackToPrice) {
    EXPECT_EQ(td_price_decimals(0.5, 0.0), 1);
    EXPECT_EQ(td_price_decimals(0.01, 0.0), 2);
    EXPECT_EQ(td_price_decimals(1.0, 0.0), 0);
    EXPECT_EQ(td_price_decimals(0.00001, 0.0), 5);
    // tick 缺失/非正 → 用该行价格的最短十进制表示小数位
    EXPECT_EQ(td_price_decimals(0.0, 552.0), 0);
    EXPECT_EQ(td_price_decimals(0.0, 552.5), 1);
    EXPECT_EQ(td_price_decimals(-1.0, 3512.35), 2);
    // 大价格下的浮点尾差不得让兜底误判（对 ×10^d 做绝对容差会在这一档给出错误结果）
    EXPECT_EQ(td_price_decimals(0.0, 123456.789), 3);
    // 无法确定 → 0
    EXPECT_EQ(td_price_decimals(0.0, 0.0), 0);
}

TEST(TdAvgPriceDecimalsTest, ClampsIntoTwoToSix) {
    EXPECT_EQ(td_avg_price_decimals(0), 2);
    EXPECT_EQ(td_avg_price_decimals(1), 3);
    EXPECT_EQ(td_avg_price_decimals(4), 6);
    EXPECT_EQ(td_avg_price_decimals(5), 6);
    EXPECT_EQ(td_avg_price_decimals(8), 6);
}

}  // namespace
}  // namespace dztrader::webui
