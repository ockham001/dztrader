/**
 * @file td_positions_query_test.cpp
 * @brief `GET /api/td/positions` 直查实现（契约 rest §2.9.1/§2.9.2/§2.9.4/§2.9.6/§2.9.8，
 *        ADR 0045/0046）。
 *
 * 测试准则（docs/unit-test-standard.md）：断言外部可观察行为——响应信封、行 JSON、
 * 账户范围、锚点日、游标分页稳定性、降级 reason 码。不断言 SQL 文本与私有成员。
 *
 * 库形态用**真 schema**（`tdstore::ensure_trade_db` 建库）：测试若与 schema 漂移
 * （列名/列序变化）必须变红，而不是靠手抄 DDL 掩盖。持仓/资金行写入统一走共享夹具
 * `td_position_row_writer.h`（列清单的唯一实现）。
 */

#include "td_orders_query.h"  // kTdOrdersReadSchemaVersion（schema 版本单源守卫）
#include "td_positions_query.h"
#include "td_order_row_writer.h"
#include "td_position_row_writer.h"
#include "test_temp_dir.h"

#include <dztrader/tdstore/schema_catalog.h>
#include <dztrader/tdstore/trade_db.h>
#include <gtest/gtest.h>
#include <sqlite3.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <format>
#include <functional>
#include <map>
#include <optional>
#include <set>
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

class TdPositionsQueryTest : public ::testing::Test {
protected:
    std::filesystem::path dir_;
    std::string db_path_;

    void SetUp() override {
        dir_ = dztrader::test::unique_temp_dir("dz_webui_td_positions_query_test");
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

    void insert_position(dztrader::test::TdPositionRowSeed seed) {
        const std::string error = dztrader::test::insert_td_position_row(db_path_, seed);
        ASSERT_TRUE(error.empty()) << error;
    }

    void insert_account(dztrader::test::TdTradingAccountRowSeed seed) {
        const std::string error = dztrader::test::insert_td_trading_account_row(db_path_, seed);
        ASSERT_TRUE(error.empty()) << error;
    }

    /// 委托（追加流表）的一行，只为证明它**不参与**锚点日。
    /// 列清单走共享夹具（`td_order_row_writer.h`）：orders 有 18 列，手抄一份会让
    /// schema 加列时这里静默写旧形状（unit-test-standard §5.4）。
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

    TdPositionsQuery query() {
        return TdPositionsQuery([this]() { return db_path_; }, 200);
    }

    static std::function<std::string(std::string_view)> params(
        std::map<std::string, std::string> values) {
        return [values = std::move(values)](std::string_view key) -> std::string {
            auto it = values.find(std::string(key));
            return it == values.end() ? std::string{} : it->second;
        };
    }

    /// 以默认在线集合（ACC1）与当前交易日执行查询；非法游标/参数时返回 nullopt。
    std::optional<nlohmann::json> run(const std::map<std::string, std::string>& raw,
                                      std::vector<std::string> online = {"ACC1"},
                                      std::optional<std::string> day = std::string("20261009")) {
        const auto parsed = TdPositionsQuery::parse(params(raw));
        if (!parsed.has_value()) {
            return std::nullopt;
        }
        return query().execute(*parsed, online, day);
    }

    /// 行里的 instrument 集合（顺序无关断言用）。
    static std::vector<std::string> instruments(const nlohmann::json& body) {
        std::vector<std::string> out;
        for (const auto& item : body.at("items")) {
            out.push_back(item.at("instrument_id").get<std::string>());
        }
        return out;
    }
};

// ============================================================================
// 账户范围（契约 rest §2.9.1：全部已登记账户，含离线）
// ============================================================================

TEST_F(TdPositionsQueryTest, OfflineAccountsVisibleBecauseScopeIsAllRegistered) {
    create_db();
    // ACC2 不在在线集合里（离线），但库里有持仓行 → 必须返回。
    insert_position({.account_id = "ACC1", .instrument_id = "rb2601"});
    insert_position({.account_id = "ACC2", .instrument_id = "rb2601"});

    const auto body = run({}, /*online=*/{"ACC1"});
    ASSERT_TRUE(body.has_value());
    EXPECT_TRUE(body->at("available").get<bool>());
    std::set<std::string> accounts;
    for (const auto& item : body->at("items")) {
        accounts.insert(item.at("account_id").get<std::string>());
    }
    EXPECT_EQ(accounts, (std::set<std::string>{"ACC1", "ACC2"}));
}

TEST_F(TdPositionsQueryTest, OnlineAccountWithoutRowsStillContributesToUniverse) {
    create_db();
    insert_position({.account_id = "ACC1", .instrument_id = "rb2601"});
    // ACC9 刚登录、库里还没有任何行：账户全集仍含它（信封 online_accounts 也是这样）。
    const auto body = run({}, /*online=*/{"ACC1", "ACC9"});
    ASSERT_TRUE(body.has_value());
    EXPECT_EQ(body->at("online_accounts").size(), 2U);
    // 但它没有持仓行 → 不出现在 items 里。
    EXPECT_EQ(instruments(*body), std::vector<std::string>{"rb2601"});
}

TEST_F(TdPositionsQueryTest, NoAccountsAtAllIsEmptyListNotDegraded) {
    create_db();
    const auto body = run({}, /*online=*/{});
    ASSERT_TRUE(body.has_value());
    EXPECT_TRUE(body->at("available").get<bool>());
    EXPECT_TRUE(body->at("items").empty());
    EXPECT_TRUE(body->at("next_cursor").is_null());
}

TEST_F(TdPositionsQueryTest, AccountsFilterNarrowsUniverse) {
    create_db();
    insert_position({.account_id = "ACC1", .instrument_id = "rb2601"});
    insert_position({.account_id = "ACC2", .instrument_id = "rb2601"});

    const auto body = run({{"accounts", "ACC2"}}, /*online=*/{"ACC1", "ACC2"});
    ASSERT_TRUE(body.has_value());
    ASSERT_EQ(body->at("items").size(), 1U);
    EXPECT_EQ(body->at("items")[0].at("account_id").get<std::string>(), "ACC2");
}

TEST_F(TdPositionsQueryTest, UnknownAccountInFilterIsIgnored) {
    create_db();
    insert_position({.account_id = "ACC1", .instrument_id = "rb2601"});

    // 未知账户忽略、不报错（契约 rest §2.9.2）。
    const auto body = run({{"accounts", "ACC1,NOPE"}}, /*online=*/{"ACC1"});
    ASSERT_TRUE(body.has_value());
    ASSERT_EQ(body->at("items").size(), 1U);
}

// ============================================================================
// 锚点日（ADR 0046：positions / trading_accounts 两张快照表的 MAX(date)）
// ============================================================================

TEST_F(TdPositionsQueryTest, AnchorDayTakesLatestSnapshotDate) {
    create_db();
    insert_position({.date = "20261008", .instrument_id = "rb2601", .seq = 1});
    insert_position({.date = "20261009", .instrument_id = "rb2601", .seq = 2});

    const auto body = run({});
    ASSERT_TRUE(body.has_value());
    ASSERT_EQ(body->at("items").size(), 1U) << "anchor day must select only the latest snapshot";
    EXPECT_EQ(body->at("items")[0].at("date").get<std::string>(), "20261009");
    EXPECT_EQ(body->at("items")[0].at("seq").get<uint64_t>(), 2U);
}

TEST_F(TdPositionsQueryTest, AnchorDayIgnoresAppendOnlyTables) {
    create_db();
    // 只有 10-08 的持仓快照；但 10-09 的委托已落库（追加流先于快照查询落库的现实时序）。
    insert_position({.date = "20261008", .instrument_id = "rb2601"});
    insert_raw_order("ACC1", "20261009");

    const auto body = run({});
    ASSERT_TRUE(body.has_value());
    // 锚点日必须是 10-08（快照表），不是 10-09（追加流）——否则查询失败期会显示成空仓。
    ASSERT_EQ(body->at("items").size(), 1U);
    EXPECT_EQ(body->at("items")[0].at("date").get<std::string>(), "20261008");
}

TEST_F(TdPositionsQueryTest, FundRowAloneAdvancesAnchorDay) {
    create_db();
    insert_position({.date = "20261008", .instrument_id = "rb2601"});
    // **本用例只锁定查询层的纯口径**（锚点取两张快照表的最大值），不描述生产时序：
    // td 侧由 `on_rsp_qry_trading_account` 的 `position_query_ok_` 守卫保证资金行只在
    // 持仓查询成功后写入（ADR 0045 决策 4），所以「只有资金行推进锚点」在正常运行中
    // 不会出现——它是库被外部改写/历史遗留时的确定性行为，必须可预测而不是随机。
    insert_account({.date = "20261009"});

    const auto body = run({});
    ASSERT_TRUE(body.has_value());
    // 锚点推进到 10-09 → 10-08 的持仓行不属于锚点日 → 列表为空。
    EXPECT_TRUE(body->at("items").empty());
}

TEST_F(TdPositionsQueryTest, AccountWithNoSnapshotRowsIsOmitted) {
    create_db();
    insert_position({.account_id = "ACC1", .instrument_id = "rb2601"});
    insert_raw_order("ACC2", "20261009");  // 只有委托，无任何快照行

    const auto body = run({}, /*online=*/{"ACC1", "ACC2"});
    ASSERT_TRUE(body.has_value());
    // ACC2 无锚点日 → 不产生行（也不会因为「无行」被误算成空仓行）。
    ASSERT_EQ(body->at("items").size(), 1U);
    EXPECT_EQ(body->at("items")[0].at("account_id").get<std::string>(), "ACC1");
}

// ============================================================================
// volume = 0 过滤（契约 rest §2.9.1 空语义）
// ============================================================================

TEST_F(TdPositionsQueryTest, ZeroVolumeRowsAreNotReturned) {
    create_db();
    insert_position(
        {.instrument_id = "rb2601", .volume = 0, .today_volume = 0, .yd_volume = 0, .seq = 5});
    insert_position({.instrument_id = "rb2610", .volume = 3, .seq = 6});

    const auto body = run({});
    ASSERT_TRUE(body.has_value());
    EXPECT_EQ(instruments(*body), std::vector<std::string>{"rb2610"})
        << "volume=0 is a cleared absolute state, not a position fact";
}

TEST_F(TdPositionsQueryTest, ZeroVolumeRowsDoNotConsumePageSlots) {
    create_db();
    // limit=1 + 1 行 volume=0 排在前面：过滤必须在 SQL 层，否则首页会被清零行占满。
    insert_position({.instrument_id = "rb2601", .volume = 0, .seq = 1});
    insert_position({.instrument_id = "rb2610", .volume = 3, .seq = 2});

    const auto body = run({{"limit", "1"}});
    ASSERT_TRUE(body.has_value());
    ASSERT_EQ(body->at("items").size(), 1U);
    EXPECT_EQ(body->at("items")[0].at("instrument_id").get<std::string>(), "rb2610");
    EXPECT_TRUE(body->at("next_cursor").is_null())
        << "zero-volume row must not create a second page";
}

// ============================================================================
// 行形状与派生字段（契约 rest §2.9.4 / §2.9.6）
// ============================================================================

TEST_F(TdPositionsQueryTest, RowShapeExact) {
    create_db();
    insert_instrument("rb2601", 0.5);
    insert_position({.account_id = "ACC1",
                     .date = "20261009",
                     .instrument_id = "rb2601",
                     .exchange_id = "SHFE",
                     .direction = 1,
                     .volume = 2,
                     .frozen_volume = 1,
                     .today_volume = 1,
                     .yd_volume = 1,
                     .price = 3512.35,
                     .seq = 1844});

    const auto body = run({});
    ASSERT_TRUE(body.has_value());
    ASSERT_EQ(body->at("items").size(), 1U);
    const auto& row = body->at("items")[0];

    EXPECT_EQ(row.at("account_id").get<std::string>(), "ACC1");
    EXPECT_EQ(row.at("date").get<std::string>(), "20261009");
    EXPECT_EQ(row.at("instrument_id").get<std::string>(), "rb2601");
    EXPECT_EQ(row.at("exchange_id").get<std::string>(), "SHFE");
    EXPECT_EQ(row.at("direction").get<std::string>(), "Long");
    EXPECT_EQ(row.at("volume").get<int>(), 2);
    EXPECT_EQ(row.at("frozen_volume").get<int>(), 1);
    EXPECT_EQ(row.at("today_volume").get<int>(), 1);
    EXPECT_EQ(row.at("yd_volume").get<int>(), 1);
    EXPECT_DOUBLE_EQ(row.at("price").get<double>(), 3512.35);
    EXPECT_EQ(row.at("price_decimals").get<int>(), 1) << "price_tick=0.5 → 1";
    EXPECT_EQ(row.at("seq").get<uint64_t>(), 1844U);

    // 持仓行没有 sort_key（契约 rest §2.9.4 字段表）；也绝不返回平台内部列。
    EXPECT_FALSE(row.contains("sort_key"));
    EXPECT_FALSE(row.contains("gateway"));
    EXPECT_FALSE(row.contains("ext"));
}

TEST_F(TdPositionsQueryTest, ShortAndNetDirectionsSerializedAsPascalCase) {
    create_db();
    insert_position({.instrument_id = "rb2601", .direction = -1, .seq = 1});
    insert_position({.instrument_id = "rb2610", .direction = 0, .seq = 2});

    const auto body = run({});
    ASSERT_TRUE(body.has_value());
    ASSERT_EQ(body->at("items").size(), 2U);
    EXPECT_EQ(body->at("items")[0].at("direction").get<std::string>(), "Short");
    EXPECT_EQ(body->at("items")[1].at("direction").get<std::string>(), "Net");
}

TEST_F(TdPositionsQueryTest, PriceDecimalsFallBackToPriceWhenNoInstrument) {
    create_db();
    insert_position({.instrument_id = "unknown_inst", .price = 3512.35});

    const auto body = run({});
    ASSERT_TRUE(body.has_value());
    ASSERT_EQ(body->at("items").size(), 1U);
    // 查不到合约 → 用价格最简十进制表示的小数位兜底（契约 rest §2.9.6）。
    EXPECT_EQ(body->at("items")[0].at("price_decimals").get<int>(), 2);
}

// ============================================================================
// 排序与游标分页（契约 rest §2.9.2：坐标 = (account_id, instrument_id, direction) 升序）
// ============================================================================

TEST_F(TdPositionsQueryTest, OrderIsAccountThenInstrumentThenDirectionAscending) {
    create_db();
    insert_position({.account_id = "ACC2", .instrument_id = "rb2601", .direction = 1, .seq = 1});
    insert_position({.account_id = "ACC1", .instrument_id = "rb2610", .direction = 1, .seq = 2});
    insert_position({.account_id = "ACC1", .instrument_id = "rb2601", .direction = 1, .seq = 3});
    insert_position({.account_id = "ACC1", .instrument_id = "rb2601", .direction = -1, .seq = 4});

    const auto body = run({});
    ASSERT_TRUE(body.has_value());
    ASSERT_EQ(body->at("items").size(), 4U);
    // (ACC1, rb2601, -1) < (ACC1, rb2601, 1) < (ACC1, rb2610, 1) < (ACC2, rb2601, 1)
    EXPECT_EQ(body->at("items")[0].at("direction").get<std::string>(), "Short");
    EXPECT_EQ(body->at("items")[1].at("direction").get<std::string>(), "Long");
    EXPECT_EQ(body->at("items")[2].at("instrument_id").get<std::string>(), "rb2610");
    EXPECT_EQ(body->at("items")[3].at("account_id").get<std::string>(), "ACC2");
}

TEST_F(TdPositionsQueryTest, CursorPaginatesWithoutGapsOrDuplicates) {
    create_db();
    for (int i = 0; i < 5; ++i) {
        insert_position(
            {.instrument_id = std::format("rb260{}", i), .seq = static_cast<uint64_t>(i + 1)});
    }

    std::vector<std::string> seen;
    std::optional<std::string> cursor;
    for (int page = 0; page < 10; ++page) {
        std::map<std::string, std::string> raw{{"limit", "2"}};
        if (cursor.has_value()) {
            raw["cursor"] = *cursor;
        }
        const auto body = run(raw);
        ASSERT_TRUE(body.has_value());
        for (const auto& item : body->at("items")) {
            seen.push_back(item.at("instrument_id").get<std::string>());
        }
        if (body->at("next_cursor").is_null()) {
            break;
        }
        cursor = body->at("next_cursor").get<std::string>();
    }
    // **精确断言完整序列**，不是「大小 == 5 且有序」——后者对「一页重复一条、另一页漏一条」
    // 仍然为真（总数对、仍有序），无法守住标题宣称的「无重复、无遗漏」。
    const std::vector<std::string> expected{"rb2600", "rb2601", "rb2602", "rb2603", "rb2604"};
    EXPECT_EQ(seen, expected) << "paging must return every row exactly once, in ascending order";
}

TEST_F(TdPositionsQueryTest, CursorFromOtherEndpointIsRejected) {
    create_db();
    insert_position({});

    // 委托端点的游标拿到持仓端点 → 作用域不符 → 400（nullopt）。
    const auto body = run({{"cursor", "31007c3063366137"}});
    EXPECT_FALSE(body.has_value());
}

TEST_F(TdPositionsQueryTest, MalformedCursorIsRejected) {
    create_db();
    insert_position({});
    const auto body = run({{"cursor", "not-a-valid-cursor"}});
    EXPECT_FALSE(body.has_value());
}

TEST_F(TdPositionsQueryTest, CursorScopeChangesWhenAnchorDayAdvances) {
    create_db();
    for (int i = 0; i < 3; ++i) {
        insert_position(
            {.instrument_id = std::format("rb260{}", i), .seq = static_cast<uint64_t>(i + 1)});
    }
    const auto first = run({{"limit", "1"}});
    ASSERT_TRUE(first.has_value());
    ASSERT_FALSE(first->at("next_cursor").is_null());
    const std::string cursor = first->at("next_cursor").get<std::string>();

    // 锚点日推进（新的完整成功查询写入次日快照）→ 作用域变化 → 旧游标必须被拒。
    sqlite3* db = open();
    ASSERT_EQ(SQLITE_OK, exec_sql(db, "UPDATE positions SET date='20261010'"));
    sqlite3_close(db);

    const auto stale = run({{"limit", "1"}, {"cursor", cursor}});
    EXPECT_FALSE(stale.has_value()) << "cursor is bound to the anchor-day scope";
}

// ============================================================================
// 降级（契约 rest §2.9.8）
// ============================================================================

TEST_F(TdPositionsQueryTest, MissingDbDegradesToNotFound) {
    // 不建库。
    const auto body = run({});
    ASSERT_TRUE(body.has_value());
    EXPECT_FALSE(body->at("available").get<bool>());
    EXPECT_EQ(body->at("reason").get<std::string>(), "not_found");
    EXPECT_EQ(body->size(), 2U) << "degraded body has only available + reason";
}

TEST_F(TdPositionsQueryTest, SchemaMismatchDegradesToSchemaMismatch) {
    create_db();
    sqlite3* db = open();
    ASSERT_EQ(SQLITE_OK, exec_sql(db, "PRAGMA user_version = 999"));
    sqlite3_close(db);

    const auto body = run({});
    ASSERT_TRUE(body.has_value());
    EXPECT_FALSE(body->at("available").get<bool>());
    EXPECT_EQ(body->at("reason").get<std::string>(), "schema_mismatch");
}

TEST_F(TdPositionsQueryTest, InvalidLimitIsRequestError) {
    create_db();
    insert_position({});
    EXPECT_FALSE(run({{"limit", "0"}}).has_value());
    EXPECT_FALSE(run({{"limit", "1001"}}).has_value());
    EXPECT_FALSE(run({{"limit", "abc"}}).has_value());
}

}  // namespace
}  // namespace dztrader::webui
