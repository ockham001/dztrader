/**
 * @file td_trades_query_test.cpp
 * @brief `GET /api/td/trades` 直查实现（契约 rest §2.9.1/§2.9.2/§2.9.3/§2.9.6/§2.9.8）。
 *
 * 测试准则（docs/unit-test-standard.md）：断言外部可观察行为——响应信封、行 JSON、
 * 游标分页的稳定性、降级 reason 码。不断言 SQL 文本与私有成员。
 *
 * 库形态用**真 schema**（`tdstore::ensure_trade_db` 建库）：测试若与 schema 漂移
 * （列名/列序变化）必须变红，而不是靠手抄 DDL 掩盖。成交行写入统一走共享夹具
 * `td_trade_row_writer.h`（列清单的唯一实现）。
 */

#include "td_orders_query.h"  // kTdOrdersReadSchemaVersion（schema 版本单源守卫）
#include "td_trades_query.h"
#include "td_trade_row_writer.h"
#include "test_temp_dir.h"

#include <dztrader/tdstore/schema_catalog.h>
#include <dztrader/tdstore/trade_db.h>
#include <gtest/gtest.h>
#include <sqlite3.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace dztrader::webui {
namespace {

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

class TdTradesQueryTest : public ::testing::Test {
protected:
    std::filesystem::path dir_;
    std::string db_path_;

    void SetUp() override {
        dir_ = dztrader::test::unique_temp_dir("dz_webui_td_trades_query_test");
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

    void insert_trade(dztrader::test::TdTradeRowSeed seed) {
        const std::string error = dztrader::test::insert_td_trade_row(db_path_, seed);
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

    TdTradesQuery query() {
        return TdTradesQuery([this]() { return db_path_; }, 200);
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
        const auto parsed = TdTradesQuery::parse(params(raw));
        if (!parsed.has_value()) {
            return std::nullopt;
        }
        return query().execute(*parsed, online, day);
    }
};

// ===== 信封与空语义（契约 rest §2.9.1/§2.9.2）=====

TEST_F(TdTradesQueryTest, EmptyDatabaseReturnsAvailableEnvelopeWithNoItems) {
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

TEST_F(TdTradesQueryTest, NoOnlineAccountsYieldsNullTradingDayAndEmptyItems) {
    create_db();
    insert_trade({});
    const auto body = run({}, /*online=*/{}, /*day=*/std::nullopt);
    ASSERT_TRUE(body.has_value());
    EXPECT_TRUE((*body)["available"].get<bool>());
    EXPECT_TRUE((*body)["trading_day"].is_null());
    EXPECT_TRUE((*body)["items"].empty());
    EXPECT_TRUE((*body)["next_cursor"].is_null());
}

TEST_F(TdTradesQueryTest, NoOnlineAccountsStillReportsDegradationWhenDatabaseUnreadable) {
    // 无在线账户但库不存在：必须降级（不能拿空 items 冒充「今天没数据」，契约 §2.9.8）
    const auto body = run({}, /*online=*/{}, /*day=*/std::nullopt);
    ASSERT_TRUE(body.has_value());
    EXPECT_FALSE((*body)["available"].get<bool>());
    EXPECT_EQ((*body)["reason"].get<std::string>(), "not_found");
    EXPECT_FALSE((*body).contains("items"));
}

// ===== 行 schema（契约 rest §2.9.3 成交行字段表）=====

TEST_F(TdTradesQueryTest, RowCarriesContractFieldsWithPascalCaseEnums) {
    create_db();
    insert_instrument("rb2601", 0.5);
    dztrader::test::TdTradeRowSeed seed;
    seed.account_id = "ACC1";
    seed.date = "20261009";
    seed.trade_id = "00000000001";
    seed.order_id = 10231;
    seed.strategy_id = "stg_demo";
    seed.instrument_id = "rb2601";
    seed.exchange_id = "SHFE";
    seed.direction = -1;       // Short
    seed.position_effect = 1;  // Open
    seed.price = 3512.3;
    seed.volume = 2;
    seed.time = 32405;
    seed.seq = 1843;
    insert_trade(seed);

    const auto body = run({});
    ASSERT_TRUE(body.has_value());
    const auto& row = (*body)["items"][0];
    EXPECT_EQ(row["account_id"].get<std::string>(), "ACC1");
    EXPECT_EQ(row["date"].get<std::string>(), "20261009");
    EXPECT_EQ(row["trade_id"].get<std::string>(), "00000000001");
    EXPECT_EQ(row["order_id"].get<int64_t>(), 10231);
    EXPECT_EQ(row["strategy_id"].get<std::string>(), "stg_demo");
    EXPECT_EQ(row["instrument_id"].get<std::string>(), "rb2601");
    EXPECT_EQ(row["exchange_id"].get<std::string>(), "SHFE");
    EXPECT_EQ(row["direction"].get<std::string>(), "Short");
    EXPECT_EQ(row["position_effect"].get<std::string>(), "Open");
    EXPECT_DOUBLE_EQ(row["price"].get<double>(), 3512.3);
    EXPECT_EQ(row["volume"].get<int>(), 2);
    EXPECT_EQ(row["time"].get<int>(), 32405);
    EXPECT_EQ(row["price_decimals"].get<int>(), 1);
    EXPECT_EQ(row["seq"].get<uint64_t>(), 1843U);
    EXPECT_TRUE(row["sort_key"].is_string());
    EXPECT_FALSE(row["sort_key"].get<std::string>().empty());
    // 契约 rest §2.9.3 成交行**没有**成交均价字段（那是委托行的）
    EXPECT_FALSE(row.contains("avg_price"));
    EXPECT_FALSE(row.contains("avg_price_decimals"));
    EXPECT_FALSE(row.contains("price_type"));
    EXPECT_FALSE(row.contains("volume_traded"));
    EXPECT_FALSE(row.contains("remark"));
}

// ===== 排序（契约 rest §2.9.6：18:00 锚点 + 同秒业务键降序）=====

TEST_F(TdTradesQueryTest, TradesByTimeAnchorDescendingWithNightSessionAfterDaySession) {
    create_db();
    // 夜盘 21:00 = 75600 秒 → 锚点 (75600+21600)%86400 = 10800
    // 日盘 09:00 = 32400 → 锚点 54000；日盘 14:00 = 50400 → 锚点 72000
    dztrader::test::TdTradeRowSeed day_09;
    day_09.trade_id = "0000000001";
    day_09.time = 32400;
    insert_trade(day_09);
    dztrader::test::TdTradeRowSeed day_14;
    day_14.trade_id = "0000000002";
    day_14.time = 50400;
    insert_trade(day_14);
    dztrader::test::TdTradeRowSeed night;
    night.trade_id = "0000000003";
    night.time = 75600;
    insert_trade(night);

    const auto body = run({});
    ASSERT_TRUE(body.has_value());
    const auto& items = (*body)["items"];
    ASSERT_EQ(items.size(), 3U);
    // 14:00(锚点 72000) > 09:00(54000) > 夜盘(10800)：夜盘排在日盘之后
    EXPECT_EQ(items[0]["trade_id"].get<std::string>(), "0000000002");
    EXPECT_EQ(items[1]["trade_id"].get<std::string>(), "0000000001");
    EXPECT_EQ(items[2]["trade_id"].get<std::string>(), "0000000003");
    // 契约 §2.9.6 性质 1：items 按 sort_key 字符串降序
    EXPECT_GT(items[0]["sort_key"].get<std::string>(), items[1]["sort_key"].get<std::string>());
    EXPECT_GT(items[1]["sort_key"].get<std::string>(), items[2]["sort_key"].get<std::string>());
}

TEST_F(TdTradesQueryTest, SameSecondBreaksTieByAccountThenExchangeThenTradeIdDescending) {
    create_db();
    const auto seed = [](const std::string& account, const std::string& exchange,
                         const std::string& trade_id) {
        dztrader::test::TdTradeRowSeed row;
        row.account_id = account;
        row.exchange_id = exchange;
        row.trade_id = trade_id;
        row.time = 32400;
        return row;
    };
    // 同秒同账户：exchange 降序（SHFE > CFFEX）
    insert_trade(seed("ACC1", "CFFEX", "0000000001"));
    insert_trade(seed("ACC1", "SHFE", "0000000001"));
    // 同秒同账户同交易所：trade_id 降序
    insert_trade(seed("ACC1", "SHFE", "0000000009"));
    // 同秒不同账户：account 降序（ACC2 > ACC1）
    insert_trade(seed("ACC2", "SHFE", "0000000001"));

    const auto body = run({});
    ASSERT_TRUE(body.has_value());
    const auto& items = (*body)["items"];
    ASSERT_EQ(items.size(), 4U);
    EXPECT_EQ(items[0]["account_id"].get<std::string>(), "ACC2");
    EXPECT_EQ(items[1]["account_id"].get<std::string>(), "ACC1");
    EXPECT_EQ(items[1]["exchange_id"].get<std::string>(), "SHFE");
    EXPECT_EQ(items[1]["trade_id"].get<std::string>(), "0000000009");
    EXPECT_EQ(items[2]["exchange_id"].get<std::string>(), "SHFE");
    EXPECT_EQ(items[2]["trade_id"].get<std::string>(), "0000000001");
    EXPECT_EQ(items[3]["exchange_id"].get<std::string>(), "CFFEX");
    for (size_t i = 1; i < items.size(); ++i) {
        EXPECT_GT(items[i - 1]["sort_key"].get<std::string>(),
                  items[i]["sort_key"].get<std::string>());
    }
}

// ===== 分页（契约 rest §2.9.2：默认 200、上限 1000、禁 OFFSET、不重不漏）=====

TEST_F(TdTradesQueryTest, DefaultLimitIsTwoHundredRows) {
    create_db();
    for (int32_t i = 0; i < 205; ++i) {
        dztrader::test::TdTradeRowSeed seed;
        seed.trade_id = std::format("{:010d}", 1000 + i);
        seed.time = 32400 + i;
        insert_trade(seed);
    }
    const auto body = run({});
    ASSERT_TRUE(body.has_value());
    EXPECT_EQ((*body)["items"].size(), 200U);
    EXPECT_TRUE((*body)["next_cursor"].is_string());
}

TEST_F(TdTradesQueryTest, CursorPaginationIsStableAndLossless) {
    create_db();
    constexpr int kTotal = 25;
    for (int32_t i = 0; i < kTotal; ++i) {
        dztrader::test::TdTradeRowSeed seed;
        seed.trade_id = std::format("{:010d}", 1 + i);
        // 同秒 → 由业务键（trade_id）破并列，覆盖游标第二坐标段
        seed.time = 32400;
        insert_trade(seed);
    }

    std::vector<std::string> seen;
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
            const std::string trade_id = row["trade_id"].get<std::string>();
            EXPECT_EQ(std::find(seen.begin(), seen.end(), trade_id), seen.end())
                << "duplicate trade_id across pages: " << trade_id;
            if (!previous_sort_key.empty()) {
                EXPECT_LT(row["sort_key"].get<std::string>(), previous_sort_key)
                    << "sort_key must strictly decrease across the whole list";
            }
            previous_sort_key = row["sort_key"].get<std::string>();
            seen.push_back(trade_id);
        }
        if ((*body)["next_cursor"].is_null()) {
            break;
        }
        cursor = (*body)["next_cursor"].get<std::string>();
    }
    EXPECT_EQ(seen.size(), static_cast<size_t>(kTotal));
    EXPECT_TRUE(std::is_sorted(seen.rbegin(), seen.rend()));
}

// ===== 游标作用域与参数校验（契约 rest §2.9.2）=====

TEST_F(TdTradesQueryTest, CursorBoundToScopeAccountsAndTradingDay) {
    create_db();
    for (int32_t i = 0; i < 5; ++i) {
        dztrader::test::TdTradeRowSeed seed;
        seed.trade_id = std::format("{:010d}", 1 + i);
        seed.time = 32400 + i;
        insert_trade(seed);
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
    // 另一端点（委托）的作用域指纹不同 → 成交端点拒绝（端点参与作用域，契约 §2.9.2）
    const uint64_t orders_scope =
        td_cursor_scope("GET /api/td/orders", {}, std::string_view("20261009"));
    const std::string orders_cursor = td_encode_cursor(orders_scope, "54000|00000000000000000001");
    EXPECT_FALSE(run({{"limit", "2"}, {"cursor", orders_cursor}}).has_value());
    // 同一作用域 → 正常续页
    const auto second = run({{"limit", "2"}, {"cursor", cursor}});
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ((*second)["items"].size(), 2U);
}

TEST_F(TdTradesQueryTest, MalformedCursorIsBadRequest) {
    create_db();
    EXPECT_FALSE(run({{"cursor", "not-a-cursor"}}).has_value());
    EXPECT_FALSE(run({{"cursor", "deadbeef"}}).has_value());
    // 合法外层 hex 但坐标段被破坏 → 400
    const std::string broken = td_hex_encode("1|0000000000000001|bad-coord");
    EXPECT_TRUE(TdTradesQuery::parse(params({{"cursor", broken}})).has_value());
    EXPECT_FALSE(run({{"cursor", broken}}).has_value());
}

TEST_F(TdTradesQueryTest, LimitOutOfRangeOrNonNumericIsBadRequest) {
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

TEST_F(TdTradesQueryTest, AccountsFilterIntersectsOnlineSetAndIgnoresUnknown) {
    create_db();
    dztrader::test::TdTradeRowSeed a;
    a.account_id = "ACC1";
    a.trade_id = "0000000001";
    a.time = 32400;
    insert_trade(a);
    dztrader::test::TdTradeRowSeed b;
    b.account_id = "ACC2";
    b.trade_id = "0000000002";
    b.time = 32401;
    insert_trade(b);
    dztrader::test::TdTradeRowSeed offline;
    offline.account_id = "ACC3";  // 不在在线集合 → 忽略
    offline.trade_id = "0000000003";
    offline.time = 32402;
    insert_trade(offline);
    dztrader::test::TdTradeRowSeed other_day;
    other_day.account_id = "ACC1";
    other_day.trade_id = "0000000004";
    other_day.time = 32403;
    other_day.date = "20261008";  // 非当前交易日 → 不返回
    insert_trade(other_day);

    const auto body = run({{"accounts", "ACC1, ACC3"}});
    ASSERT_TRUE(body.has_value());
    const auto& items = (*body)["items"];
    ASSERT_EQ(items.size(), 1U);
    EXPECT_EQ(items[0]["account_id"].get<std::string>(), "ACC1");
    EXPECT_EQ(items[0]["trade_id"].get<std::string>(), "0000000001");

    // 空串等价缺省 = 全部在线账户
    const auto all = run({{"accounts", ""}});
    ASSERT_TRUE(all.has_value());
    EXPECT_EQ((*all)["items"].size(), 2U);
}

TEST_F(TdTradesQueryTest, MultipleExchangesWithSameTradeIdBothReturned) {
    create_db();
    // 契约 rest §2.9.3：trade_id 仅交易所内唯一，(exchange_id, trade_id) 才唯一。
    dztrader::test::TdTradeRowSeed shfe;
    shfe.exchange_id = "SHFE";
    shfe.trade_id = "0000000042";
    insert_trade(shfe);
    dztrader::test::TdTradeRowSeed cffex;
    cffex.exchange_id = "CFFEX";
    cffex.trade_id = "0000000042";
    insert_trade(cffex);

    const auto body = run({});
    ASSERT_TRUE(body.has_value());
    const auto& items = (*body)["items"];
    ASSERT_EQ(items.size(), 2U);
    EXPECT_EQ(items[0]["exchange_id"].get<std::string>(), "SHFE");  // 降序
    EXPECT_EQ(items[1]["exchange_id"].get<std::string>(), "CFFEX");
    EXPECT_NE(items[0]["sort_key"].get<std::string>(), items[1]["sort_key"].get<std::string>());
}

// ===== 精度（契约 rest §2.9.6）=====

TEST_F(TdTradesQueryTest, PriceDecimalsDerivedFromInstrumentPriceTick) {
    create_db();
    const std::vector<std::pair<std::string, double>> ticks = {
        {"tick_half", 0.5}, {"tick_cent", 0.01}, {"tick_one", 1.0}, {"tick_micro", 0.00001}};
    for (size_t i = 0; i < ticks.size(); ++i) {
        insert_instrument(ticks[i].first, ticks[i].second);
        dztrader::test::TdTradeRowSeed seed;
        seed.trade_id = std::format("{:010d}", i + 1);
        seed.time = 32400 + static_cast<int>(i);
        seed.instrument_id = ticks[i].first;
        insert_trade(seed);
    }
    const auto body = run({});
    ASSERT_TRUE(body.has_value());
    const auto& items = (*body)["items"];
    ASSERT_EQ(items.size(), ticks.size());
    for (const auto& row : items) {
        const std::string instrument = row["instrument_id"].get<std::string>();
        for (const auto& [name, tick] : ticks) {
            if (name != instrument) {
                continue;
            }
            (void)tick;
            const int expected = name == "tick_half"   ? 1
                                 : name == "tick_cent" ? 2
                                 : name == "tick_one"  ? 0
                                                       : 5;
            EXPECT_EQ(row["price_decimals"].get<int>(), expected) << instrument;
        }
    }
}

TEST_F(TdTradesQueryTest, PriceDecimalsFallBackToTradePriceDigitsWhenInstrumentMissing) {
    create_db();
    dztrader::test::TdTradeRowSeed no_instrument;
    no_instrument.trade_id = "0000000001";
    no_instrument.instrument_id = "no_such_instrument";
    no_instrument.price = 552.0;  // 无小数 → 0 位
    insert_trade(no_instrument);
    dztrader::test::TdTradeRowSeed one_decimal;
    one_decimal.trade_id = "0000000002";
    one_decimal.time = 32401;
    one_decimal.instrument_id = "no_such_instrument";
    one_decimal.price = 552.5;  // 1 位
    insert_trade(one_decimal);

    const auto body = run({});
    ASSERT_TRUE(body.has_value());
    const auto& items = (*body)["items"];
    ASSERT_EQ(items.size(), 2U);
    EXPECT_EQ(items[0]["price_decimals"].get<int>(), 1);  // time 32401 在前
    EXPECT_EQ(items[1]["price_decimals"].get<int>(), 0);
}

// ===== 降级（契约 rest §2.9.8：200 + available:false + reason，不 5xx）=====

TEST_F(TdTradesQueryTest, DegradesWhenDatabaseMissing) {
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

TEST_F(TdTradesQueryTest, DegradesOnSchemaVersionMismatch) {
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

TEST_F(TdTradesQueryTest, DegradesOnNonSqliteFile) {
    std::ofstream out(db_path_, std::ios::binary);
    ASSERT_TRUE(out.good());
    out << "this is definitely not a sqlite database file";
    out.close();

    const auto body = run({});
    ASSERT_TRUE(body.has_value());
    EXPECT_FALSE((*body)["available"].get<bool>());
    EXPECT_EQ((*body)["reason"].get<std::string>(), "open_failed");
}

TEST_F(TdTradesQueryTest, DegradesWhenReadLockCannotBeAcquired) {
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

TEST_F(TdTradesQueryTest, DegradesOnQueryFailure) {
    create_db();
    {
        sqlite3* db = open();
        // 版本仍为 v1，但表缺失 → 查询执行失败（不是 schema 版本问题）
        EXPECT_EQ(SQLITE_OK, exec_sql(db, "DROP TABLE trades"));
        sqlite3_close(db);
    }
    const auto body = run({});
    ASSERT_TRUE(body.has_value());
    EXPECT_FALSE((*body)["available"].get<bool>());
    EXPECT_EQ((*body)["reason"].get<std::string>(), "query_failed");
}

// ===== 纯函数：成交排序键（契约 rest §2.9.6 的三条性质）=====

TEST(TdTradeSortKeyTest, StringOrderMatchesContractSortOrder) {
    // 时间锚不同：锚点大者 sort_key 大（"0" 前缀补零保证跨秒可比较）
    EXPECT_GT(td_trade_sort_key(50400, "ACC1", "SHFE", "0000000001"),
              td_trade_sort_key(32400, "ACC2", "CFFEX", "9999999999"));
    // 同一秒：account_id 字节序大者 sort_key 大
    EXPECT_GT(td_trade_sort_key(32400, "ACC2", "SHFE", "0000000001"),
              td_trade_sort_key(32400, "ACC1", "SHFE", "9999999999"));
    // 同一秒同账户：exchange_id 大者 sort_key 大
    EXPECT_GT(td_trade_sort_key(32400, "ACC1", "SHFE", "0000000001"),
              td_trade_sort_key(32400, "ACC1", "CFFEX", "9999999999"));
    // 同一秒同账户同交易所：trade_id 大者 sort_key 大
    EXPECT_GT(td_trade_sort_key(32400, "ACC1", "SHFE", "0000000009"),
              td_trade_sort_key(32400, "ACC1", "SHFE", "0000000001"));
}

TEST(TdTradeSortKeyTest, HexEncodingPreservesByteOrderAndSeparatorIsSafe) {
    // 分隔符 '-' (0x2d) 小于全部 hex 字符（'0'-'9'/'a'-'f'），因此短前缀段不会吃掉下一段
    EXPECT_GT(td_trade_sort_key(32400, "ACC1", "SHFE", "2"),
              td_trade_sort_key(32400, "ACC1", "SHFE", "1"));
    // 字节序与字符序一致：小写字母（0x61+）> 大写字母（0x41+）> 数字（0x30+）
    EXPECT_GT(td_trade_sort_key(32400, "aa", "SHFE", "1"),
              td_trade_sort_key(32400, "ZZ", "SHFE", "1"));
    EXPECT_GT(td_trade_sort_key(32400, "ZZ", "SHFE", "1"),
              td_trade_sort_key(32400, "11", "SHFE", "1"));
    // 前缀情形：前缀段才能真正考验分隔符性质。等长键比较时两条的下一段起点相同位置，
    // 分隔符无论取什么值都不会被比较到——只有「短段是先缀」时才会落到分隔符 vs hex 字符。
    // "A" 的 hex 是 "41"、"AA" 是 "4141"：短段后面紧跟 '-'（0x2d），长段的下一字符是 '4'
    // （0x34）→ 长的更大。若分隔符 >= '0'（例如误用 'a'/'~'），短键会排到长键之后，这里变红。
    EXPECT_GT(td_trade_sort_key(32400, "AA", "SHFE", "1"),
              td_trade_sort_key(32400, "A", "SHFE", "1"));
    // exchange 段同理（account 相同、exchange 是前缀）
    EXPECT_GT(td_trade_sort_key(32400, "ACC1", "SHFE2", "1"),
              td_trade_sort_key(32400, "ACC1", "SHFE", "1"));
    // trade_id 段同理（前两段相同、trade_id 是前缀）
    EXPECT_GT(td_trade_sort_key(32400, "ACC1", "SHFE", "11"),
              td_trade_sort_key(32400, "ACC1", "SHFE", "1"));
    // 任何字节都编码为两个 hex 字符 → 定界安全（不可打印字符不会破坏比较）
    // 用八进制转义 \037 表示 0x1f：'\x1f' 后紧跟字母会被当作更长的 hex 转义（编译告警）。
    const std::string with_control = td_trade_sort_key(32400, "A\037Z", "SHFE", "1");
    EXPECT_EQ(with_control.find('\x1f'), std::string::npos);
    // 控制字符也被编码：0x1f 的 hex 是 "1f"，落在 "0"-"9"/"a"-"f" 区间内，
    // 不会原样出现在 sort_key 里污染定界（"-" 仍唯一分隔符）。
    EXPECT_EQ(with_control, "54000-411f5a-53484645-31");
}

}  // namespace
}  // namespace dztrader::webui
