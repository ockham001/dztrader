/**
 * @file td_trading_accounts_query_test.cpp
 * @brief `GET /api/td/trading_accounts` 直查实现（契约 rest §2.9.1/§2.9.2/§2.9.4/§2.9.8，
 *        ADR 0045/0046）。
 *
 * 测试准则（docs/unit-test-standard.md）：断言外部可观察行为——响应信封、行 JSON、
 * 账户范围（含离线）、锚点日与「无记录占位行」、`accounts` 过滤、不分页语义、降级 reason 码。
 * 不断言 SQL 文本与私有成员。
 *
 * 库形态用**真 schema**（`tdstore::ensure_trade_db` 建库）：测试若与 schema 漂移
 * （列名/列序变化）必须变红，而不是靠手抄 DDL 掩盖。持仓/资金行写入统一走共享夹具
 * `td_position_row_writer.h`（列清单的唯一实现）。
 */

#include "td_orders_query.h"  // kTdOrdersReadSchemaVersion（schema 版本单源守卫）
#include "td_order_row_writer.h"
#include "td_position_row_writer.h"
#include "td_trading_accounts_query.h"
#include "test_temp_dir.h"

#include <dztrader/tdstore/schema_catalog.h>
#include <dztrader/tdstore/trade_db.h>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <cstdint>
#include <filesystem>
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

class TdTradingAccountsQueryTest : public ::testing::Test {
protected:
    std::filesystem::path dir_;
    std::string db_path_;

    void SetUp() override {
        dir_ = dztrader::test::unique_temp_dir("dz_webui_td_trading_accounts_query_test");
        std::filesystem::create_directories(dir_);
        db_path_ = (dir_ / "trade.db").string();
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    /// 真 schema 建库（列名/键/索引与生产同源）。
    void create_db() { (void)dztrader::tdstore::ensure_trade_db(db_path_); }

    void insert_account(dztrader::test::TdTradingAccountRowSeed seed) {
        const std::string error = dztrader::test::insert_td_trading_account_row(db_path_, seed);
        ASSERT_TRUE(error.empty()) << error;
    }

    void insert_position(dztrader::test::TdPositionRowSeed seed) {
        const std::string error = dztrader::test::insert_td_position_row(db_path_, seed);
        ASSERT_TRUE(error.empty()) << error;
    }

    /// 委托（追加流表）的一行，只为证明它**不参与**锚点日（ADR 0046）。
    void insert_raw_order(const std::string& account_id, const std::string& date) {
        dztrader::test::TdOrderRowSeed seed;
        seed.account_id = account_id;
        seed.date = date;
        seed.order_id = 1000;
        seed.instrument_id = "rb2601";
        seed.exchange_id = "SHFE";
        seed.seq = 1;
        const std::string error = dztrader::test::insert_td_order_row(db_path_, seed);
        ASSERT_TRUE(error.empty()) << error;
    }

    /// 直接执行 SQL（只用于构造降级场景，不走生产实现）。
    void exec_sql(const std::string& sql) {
        sqlite3* db = nullptr;
        ASSERT_EQ(SQLITE_OK, sqlite3_open(db_path_.c_str(), &db));
        char* err = nullptr;
        if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
            const std::string message = err != nullptr ? err : sqlite3_errmsg(db);
            sqlite3_free(err);
            sqlite3_close(db);
            ADD_FAILURE() << "sqlite3_exec failed: " << message;
            return;
        }
        sqlite3_close(db);
    }

    TdTradingAccountsQuery query() {
        return TdTradingAccountsQuery([this]() { return db_path_; }, 200);
    }

    static std::function<std::string(std::string_view)> params(
        std::map<std::string, std::string> values) {
        return [values = std::move(values)](std::string_view key) -> std::string {
            auto it = values.find(std::string(key));
            return it == values.end() ? std::string{} : it->second;
        };
    }

    /// 执行查询（资金端点没有 400 路径：`parse` 恒定成功、`execute` 恒定返回响应体）。
    nlohmann::json run(const std::map<std::string, std::string>& raw = {},
                       const std::vector<std::string>& online = {"ACC1"},
                       std::optional<std::string> day = std::string("20261009")) {
        const auto parsed = TdTradingAccountsQuery::parse(params(raw));
        return query().execute(parsed, online, day);
    }

    static const nlohmann::json& row_for(const nlohmann::json& body, const std::string& account) {
        for (const auto& row : body["items"]) {
            if (row["account_id"].get<std::string>() == account) {
                return row;
            }
        }
        ADD_FAILURE() << "no row for account " << account;
        static const nlohmann::json kEmpty;
        return kEmpty;
    }
};

// ===== 账户范围：全部已登记账户（含离线）与 has_data 占位行（契约 rest §2.9.1/§2.9.4）=====

TEST_F(TdTradingAccountsQueryTest, ReturnsRegisteredAccountsIncludingOffline) {
    create_db();
    // ACC1 在线且有资金行；ACC2 离线（从不 Ready）但库里有资金行；ACC3 在线但还没有任何行。
    insert_account({.account_id = "ACC1", .date = "20261009", .seq = 5});
    insert_account({.account_id = "ACC2", .date = "20261008", .seq = 3});

    const auto body = run(/*raw=*/{}, /*online=*/{"ACC1", "ACC3"}, "20261009");
    EXPECT_TRUE(body["available"].get<bool>());
    EXPECT_EQ(body["trading_day"].get<std::string>(), "20261009");
    ASSERT_EQ(body["online_accounts"].size(), 2U);
    ASSERT_EQ(body["items"].size(), 3U) << "every registered account gets a row";

    // 行序 = account_id 升序（契约 rest §2.9.2 资金端点顺序）。
    EXPECT_EQ(body["items"][0]["account_id"].get<std::string>(), "ACC1");
    EXPECT_EQ(body["items"][1]["account_id"].get<std::string>(), "ACC2");
    EXPECT_EQ(body["items"][2]["account_id"].get<std::string>(), "ACC3");

    const auto& offline = row_for(body, "ACC2");
    EXPECT_TRUE(offline["has_data"].get<bool>());
    EXPECT_EQ(offline["date"].get<std::string>(), "20261008")
        << "offline account keeps its own anchor day";

    const auto& no_data = row_for(body, "ACC3");
    EXPECT_FALSE(no_data["has_data"].get<bool>()) << "online account with no rows still gets a row";
    EXPECT_TRUE(no_data["date"].is_null());
    EXPECT_TRUE(no_data["balance"].is_null());
    EXPECT_TRUE(no_data["available"].is_null());
    EXPECT_TRUE(no_data["frozen"].is_null());
    EXPECT_TRUE(no_data["commission"].is_null());
    EXPECT_TRUE(no_data["margin"].is_null());
    EXPECT_TRUE(no_data["withdraw_quota"].is_null());
    EXPECT_TRUE(no_data["deposit"].is_null());
    EXPECT_TRUE(no_data["withdraw"].is_null());
    EXPECT_TRUE(no_data["seq"].is_null());
}

TEST_F(TdTradingAccountsQueryTest, RowCarriesExactFundFieldsAndNoSortKey) {
    create_db();
    insert_account({.account_id = "ACC1",
                    .date = "20261009",
                    .balance = 123456.5,
                    .available = 120000.25,
                    .frozen = 1000.5,
                    .commission = 12.25,
                    .margin = 4444.5,
                    .withdraw_quota = 100000.5,
                    .deposit = 5000.25,
                    .withdraw = 300.75,
                    .seq = 45});
    insert_position({.account_id = "ACC1", .date = "20261009", .seq = 46});

    const auto body = run();
    ASSERT_EQ(body["items"].size(), 1U);
    const auto& row = body["items"][0];
    EXPECT_EQ(row["account_id"].get<std::string>(), "ACC1");
    EXPECT_EQ(row["date"].get<std::string>(), "20261009");
    EXPECT_TRUE(row["has_data"].get<bool>());
    EXPECT_DOUBLE_EQ(row["balance"].get<double>(), 123456.5);
    EXPECT_DOUBLE_EQ(row["available"].get<double>(), 120000.25);
    EXPECT_DOUBLE_EQ(row["frozen"].get<double>(), 1000.5);
    EXPECT_DOUBLE_EQ(row["commission"].get<double>(), 12.25);
    EXPECT_DOUBLE_EQ(row["margin"].get<double>(), 4444.5);
    EXPECT_DOUBLE_EQ(row["withdraw_quota"].get<double>(), 100000.5);
    EXPECT_DOUBLE_EQ(row["deposit"].get<double>(), 5000.25);
    EXPECT_DOUBLE_EQ(row["withdraw"].get<double>(), 300.75);
    EXPECT_EQ(row["seq"].get<uint64_t>(), 45U);
    // 资金是绝对态：没有时间序派生字段（契约 rest §2.9.4）。
    EXPECT_FALSE(row.contains("sort_key"));
    EXPECT_FALSE(row.contains("price_decimals"));
    EXPECT_FALSE(row.contains("gateway")) << "platform-internal column must not leak";
    EXPECT_FALSE(row.contains("ext")) << "platform-internal column must not leak";
}

// ===== 锚点日语义（ADR 0045/0046、契约 rest §2.9.1/§2.9.4）=====

TEST_F(TdTradingAccountsQueryTest, AnchorDayUsesOnlySnapshotTablesNotOrders) {
    create_db();
    // 委托是追加流：登录重放会先于快照查询落库。若它参与锚点，登录当天锚点到 10-10，
    // 而资金行还在 10-09 → 记录永远显示「无数据」/旧日行。
    insert_account({.account_id = "ACC1", .date = "20261009", .seq = 4});
    insert_raw_order("ACC1", "20261010");

    const auto body = run();
    ASSERT_EQ(body["items"].size(), 1U);
    const auto& row = body["items"][0];
    EXPECT_EQ(row["date"].get<std::string>(), "20261009")
        << "orders must not advance the anchor day (ADR 0046)";
    EXPECT_TRUE(row["has_data"].get<bool>());
}

TEST_F(TdTradingAccountsQueryTest, ReturnsOnlyTheAnchorDayFundRowFromMultipleDays) {
    create_db();
    // 多日累积：历史日行必须留在库里，但接口只返回锚点日那一行（契约 §2.9.4）。
    insert_account(
        {.account_id = "ACC1", .date = "20261008", .balance = 100.0, .available = 90.0, .seq = 2});
    insert_account(
        {.account_id = "ACC1", .date = "20261009", .balance = 200.0, .available = 150.0, .seq = 7});

    const auto body = run();
    ASSERT_EQ(body["items"].size(), 1U) << "fund row business key is account_id";
    const auto& row = body["items"][0];
    EXPECT_EQ(row["date"].get<std::string>(), "20261009");
    EXPECT_DOUBLE_EQ(row["balance"].get<double>(), 200.0);
    EXPECT_DOUBLE_EQ(row["available"].get<double>(), 150.0);
    EXPECT_EQ(row["seq"].get<uint64_t>(), 7U);
}

TEST_F(TdTradingAccountsQueryTest, MissingFundRowOnAnchorDayYieldsHasDataFalse) {
    create_db();
    // 持仓查询成功把锚点推到 10-09，但资金查询失败 → 10-09 无资金行。
    // 不得拿 10-08 的旧行冒充锚点日数据（契约 §2.9.4 字面口径）：返回占位行，
    // 让前端明确显示「无数据」，而不是把旧余额标成当日快照。
    insert_account({.account_id = "ACC1", .date = "20261008", .seq = 2});
    insert_position(
        {.account_id = "ACC1", .date = "20261009", .instrument_id = "rb2601", .seq = 6});

    const auto body = run();
    ASSERT_EQ(body["items"].size(), 1U);
    const auto& row = body["items"][0];
    EXPECT_FALSE(row["has_data"].get<bool>())
        << "stale-day fund row must not be presented as the anchor-day snapshot";
    EXPECT_TRUE(row["date"].is_null());
    EXPECT_TRUE(row["balance"].is_null());
    EXPECT_TRUE(row["seq"].is_null());
}

TEST_F(TdTradingAccountsQueryTest, AnchorDayComesFromPositionsWhenFundsAreOlder) {
    create_db();
    insert_account({.account_id = "ACC1", .date = "20261008", .seq = 2});
    insert_position(
        {.account_id = "ACC1", .date = "20261009", .instrument_id = "rb2601", .seq = 6});

    const auto body = run();
    ASSERT_EQ(body["items"].size(), 1U);
    const auto& row = body["items"][0];
    // 锚点日 = 两张快照表 MAX(date) = 20261009（持仓已推进、资金没跟上时，不得退回 10-08
    // 把旧行标成锚点日快照）。
    EXPECT_FALSE(row["has_data"].get<bool>());
    EXPECT_TRUE(row["date"].is_null());
}

TEST_F(TdTradingAccountsQueryTest, AnchorDayComesFromFundsWhenPositionsAreOlder) {
    create_db();
    insert_account({.account_id = "ACC1", .date = "20261009", .seq = 3});
    // 当日全平：持仓行全部消失（重建删除当日行），资金行仍在 → 锚点日由资金表给出。
    insert_position(
        {.account_id = "ACC1", .date = "20261008", .instrument_id = "rb2601", .seq = 2});

    const auto body = run();
    ASSERT_EQ(body["items"].size(), 1U);
    const auto& row = body["items"][0];
    EXPECT_EQ(row["date"].get<std::string>(), "20261009");
    EXPECT_TRUE(row["has_data"].get<bool>());
}

// ===== 参数：accounts 过滤 / limit、cursor 被忽略（契约 rest §2.9.2）=====

TEST_F(TdTradingAccountsQueryTest, AccountsFilterAppliesToRegisteredUniverse) {
    create_db();
    insert_account({.account_id = "ACC1", .date = "20261009", .seq = 5});
    insert_account({.account_id = "ACC2", .date = "20261009", .seq = 6});

    const auto filtered = run({{"accounts", "ACC2,UNKNOWN"}});
    ASSERT_EQ(filtered["items"].size(), 1U);
    EXPECT_EQ(filtered["items"][0]["account_id"].get<std::string>(), "ACC2");
    ASSERT_EQ(filtered["online_accounts"].size(), 1U)
        << "online_accounts remains the envelope fact, not the filtered scope";
    EXPECT_EQ(filtered["online_accounts"][0].get<std::string>(), "ACC1");

    const auto all = run({{"accounts", ""}});
    ASSERT_EQ(all["items"].size(), 2U) << "empty accounts == no filter";
}

TEST_F(TdTradingAccountsQueryTest, LimitAndCursorAreIgnoredAndNextCursorIsAlwaysNull) {
    create_db();
    insert_account({.account_id = "ACC1", .date = "20261009", .seq = 5});
    insert_account({.account_id = "ACC2", .date = "20261009", .seq = 6});

    // 契约 rest §2.9.2：资金端点不适用 cursor——limit/cursor 被忽略，非法值也不 400。
    const auto body = run({{"limit", "0"}, {"cursor", "nonsense"}, {"accounts", ""}});
    EXPECT_TRUE(body["available"].get<bool>());
    EXPECT_TRUE(body["next_cursor"].is_null()) << "fund endpoint never paginates";
    ASSERT_EQ(body["items"].size(), 2U) << "limit must be ignored (no pagination)";
}

// ===== 信封与降级（契约 rest §2.9.2/§2.9.8）=====

TEST_F(TdTradingAccountsQueryTest, EmptyItemsWhenNoRegisteredAccounts) {
    create_db();
    const auto body = run(/*raw=*/{}, /*online=*/{}, std::nullopt);
    EXPECT_TRUE(body["available"].get<bool>());
    EXPECT_TRUE(body["trading_day"].is_null());
    EXPECT_TRUE(body["online_accounts"].empty());
    EXPECT_TRUE(body["items"].empty());
    EXPECT_TRUE(body["next_cursor"].is_null());
}

TEST_F(TdTradingAccountsQueryTest, DegradesWithNotFoundWhenDatabaseMissing) {
    const auto body = run();
    EXPECT_FALSE(body["available"].get<bool>());
    EXPECT_EQ(body["reason"].get<std::string>(), "not_found");
    EXPECT_FALSE(body.contains("items"));
    EXPECT_FALSE(body.contains("trading_day"));
    EXPECT_FALSE(body.contains("online_accounts"));
}

TEST_F(TdTradingAccountsQueryTest, DegradesWithSchemaMismatchWhenVersionDrifted) {
    create_db();
    exec_sql("PRAGMA user_version = 99");
    const auto body = run();
    EXPECT_FALSE(body["available"].get<bool>());
    EXPECT_EQ(body["reason"].get<std::string>(), "schema_mismatch");
}

TEST_F(TdTradingAccountsQueryTest, DegradesWithQueryFailedWhenTableIsUnreadable) {
    create_db();
    // 模拟半损坏库：账户全集与锚点日查询的各表仍在，取资金行的表读不了 → query_failed。
    exec_sql("DROP TABLE trading_accounts");
    const auto body = run();
    EXPECT_FALSE(body["available"].get<bool>());
    EXPECT_EQ(body["reason"].get<std::string>(), "query_failed");
}

}  // namespace
}  // namespace dztrader::webui
