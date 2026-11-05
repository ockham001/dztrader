/**
 * @file td_list_query_test.cpp
 * @brief 列表端点通用参数 / 账户范围 / 游标 / 时间锚（契约 rest §2.9.2/§2.9.6）。
 *
 * 委托与成交共用同一份实现（`td_list_query.h`）：这些纯函数是两条读路径的共同前置条件，
 * 任何一条断言失败都会同时影响委托与成交列表，因此单独成文件而不是塞进某个端点用例。
 * 只断言对外承诺（解析结果、作用域相等性、锚点值），不断言内部编码细节。
 */

#include "td_list_query.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace dztrader::webui {
namespace {

std::function<std::string(std::string_view)> params(std::map<std::string, std::string> values) {
    return [values = std::move(values)](std::string_view key) -> std::string {
        auto it = values.find(std::string(key));
        return it == values.end() ? std::string{} : it->second;
    };
}

// ===== 参数解析（契约 rest §2.9.2）=====

TEST(TdListParamsTest, DefaultsAreTwoHundredWithNoCursorAndAllAccounts) {
    const auto parsed = td_parse_list_params(params({}));
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->limit, kTdListDefaultLimit);
    EXPECT_EQ(parsed->limit, 200);
    EXPECT_TRUE(parsed->cursor.empty());
    EXPECT_TRUE(parsed->accounts.empty());
}

TEST(TdListParamsTest, LimitAcceptsOneThroughOneThousand) {
    for (const int limit : {1, 200, 1000}) {
        const auto parsed = td_parse_list_params(params({{"limit", std::to_string(limit)}}));
        ASSERT_TRUE(parsed.has_value()) << limit;
        EXPECT_EQ(parsed->limit, limit);
    }
    for (const char* bad : {"0", "1001", "-1", "abc", "12abc", ""}) {
        const auto parsed = td_parse_list_params(params({{"limit", bad}}));
        if (std::string_view(bad).empty()) {
            // 空值 = 缺省，不是错误
            EXPECT_TRUE(parsed.has_value()) << "empty limit should fall back to default";
            EXPECT_EQ(parsed->limit, kTdListDefaultLimit);
        } else {
            EXPECT_FALSE(parsed.has_value()) << bad;
        }
    }
}

TEST(TdListParamsTest, AccountsAreTrimmedDedupedAndSorted) {
    // 契约 rest §2.9.2：去首尾空白、空项跳过、重复忽略；归一化使 a,b 与 b,a 同作用域。
    const auto parsed = td_parse_list_params(params({{"accounts", " ACC2 , ACC1,,ACC2 , "}}));
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->accounts, (std::vector<std::string>{"ACC1", "ACC2"}));
}

TEST(TdListParamsTest, EmptyAccountsMeansAllOnlineAccounts) {
    const auto parsed = td_parse_list_params(params({{"accounts", ""}}));
    ASSERT_TRUE(parsed.has_value());
    EXPECT_TRUE(parsed->accounts.empty());
}

TEST(TdListParamsTest, CursorIsTrimmedButKeptOpaque) {
    const auto parsed = td_parse_list_params(params({{"cursor", "  abc123  "}}));
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->cursor, "abc123");
}

// ===== 账户范围（契约 rest §2.9.1）=====

TEST(TdScopeAccountsTest, EmptyRequestMeansAllOnlineSortedAndDeduped) {
    const auto scope = td_scope_accounts({}, {"ACC2", "ACC1", "ACC2"});
    EXPECT_EQ(scope, (std::vector<std::string>{"ACC1", "ACC2"}));
}

TEST(TdScopeAccountsTest, RequestIntersectsOnlineSetIgnoringUnknown) {
    const auto scope = td_scope_accounts({"ACC2", "ACC9"}, {"ACC1", "ACC2"});
    EXPECT_EQ(scope, (std::vector<std::string>{"ACC2"}));
}

TEST(TdScopeAccountsTest, EmptyScopeWhenNothingIntersects) {
    EXPECT_TRUE(td_scope_accounts({"ACC9"}, {"ACC1"}).empty());
    EXPECT_TRUE(td_scope_accounts({}, {}).empty());
}

// ===== 游标（契约 rest §2.9.2：不透明、绑定作用域、往返无损）=====

TEST(TdCursorTest, EncodeDecodeRoundTripsTheCoordinate) {
    const uint64_t scope = td_cursor_scope("GET /api/td/trades", {"ACC1"}, "20261009");
    const std::string cursor = td_encode_cursor(scope,
                                                "54000|abc\x1f"
                                                "def");
    const auto coord = td_decode_cursor(cursor, scope);
    ASSERT_TRUE(coord.has_value());
    EXPECT_EQ(*coord,
              "54000|abc\x1f"
              "def");
}

TEST(TdCursorTest, CursorIsOpaqueHexWithoutControlCharacters) {
    const uint64_t scope = td_cursor_scope("GET /api/td/orders", {}, "20261009");
    const std::string cursor = td_encode_cursor(scope,
                                                "a\x1f"
                                                "b");
    for (const char ch : cursor) {
        const bool is_hex = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
        EXPECT_TRUE(is_hex) << "cursor must be plain hex, got char code " << int(ch);
    }
}

TEST(TdCursorTest, ScopeMismatchIsRejected) {
    const uint64_t scope = td_cursor_scope("GET /api/td/trades", {"ACC1"}, "20261009");
    const std::string cursor = td_encode_cursor(scope, "54000|x");
    // 账户变化 / 交易日变化 / 端点变化 都必须让游标作废（契约 rest §2.9.2）。
    EXPECT_FALSE(td_decode_cursor(
                     cursor, td_cursor_scope("GET /api/td/trades", {"ACC1", "ACC2"}, "20261009"))
                     .has_value());
    EXPECT_FALSE(
        td_decode_cursor(cursor, td_cursor_scope("GET /api/td/trades", {"ACC1"}, "20261010"))
            .has_value());
    EXPECT_FALSE(
        td_decode_cursor(cursor, td_cursor_scope("GET /api/td/orders", {"ACC1"}, "20261009"))
            .has_value());
}

TEST(TdCursorTest, MalformedCursorIsRejected) {
    const uint64_t scope = td_cursor_scope("GET /api/td/trades", {}, "20261009");
    for (const char* bad : {"", "zz", "0", "abc", "deadbeef"}) {
        EXPECT_FALSE(td_decode_cursor(bad, scope).has_value()) << bad;
    }
}

TEST(TdCursorTest, ScopeFingerprintIsStableAcrossCalls) {
    // 跨进程/重启稳定（FNV-1a，不用 std::hash）：同输入必同值。
    const uint64_t first = td_cursor_scope("GET /api/td/trades", {"ACC1", "ACC2"}, "20261009");
    const uint64_t second = td_cursor_scope("GET /api/td/trades", {"ACC1", "ACC2"}, "20261009");
    EXPECT_EQ(first, second);
    // 不同端点 / 账户 / 交易日至少有一个不同（碰撞概率可忽略）
    EXPECT_NE(first, td_cursor_scope("GET /api/td/orders", {"ACC1", "ACC2"}, "20261009"));
    EXPECT_NE(first, td_cursor_scope("GET /api/td/trades", {"ACC1"}, "20261009"));
    EXPECT_NE(first, td_cursor_scope("GET /api/td/trades", {"ACC1", "ACC2"}, "20261010"));
}

// ===== 排序时间锚（契约 rest §2.9.6：18:00 锚点）=====

TEST(TdTimeAnchorTest, ShiftsByEighteenHours) {
    // 18:00 = 64800 → 锚点 0
    EXPECT_EQ(td_time_anchor(64800), 0);
    // 09:00 = 32400 → 锚点 54000
    EXPECT_EQ(td_time_anchor(32400), 54000);
    // 夜盘 21:00 = 75600 → 锚点 10800（归入所属交易日，且排在日盘之后）
    EXPECT_EQ(td_time_anchor(75600), 10800);
    // 23:59:59 = 86399 → 锚点 21599
    EXPECT_EQ(td_time_anchor(86399), 21599);
    // 00:00 → 锚点 21600
    EXPECT_EQ(td_time_anchor(0), 21600);
}

TEST(TdTimeAnchorTest, NormalizesNegativeAndOverflowingSeconds) {
    // 脏数据不得让排序崩：负数与超过一天的值按 24h 取模归一。
    EXPECT_EQ(td_time_anchor(-1), td_time_anchor(86399));
    EXPECT_EQ(td_time_anchor(-21600), 0);
    EXPECT_EQ(td_time_anchor(86400), td_time_anchor(0));
    EXPECT_EQ(td_time_anchor(86400 + 32400), td_time_anchor(32400));
}

TEST(TdTimeAnchorTest, SqlExpressionIsSameSourceAsTheAnchorFunction) {
    // SQL 片段必须与 td_time_anchor 同源：直接对同一秒校验表达式形状（契约 §2.9.6 只约束语义，
    // 此处只验证「常量来自同一实现」这一结构事实：表达式里出现 21600 与 86400）。
    const std::string sql = td_time_anchor_sql("t");
    EXPECT_NE(sql.find("t.time"), std::string::npos);
    EXPECT_NE(sql.find("21600"), std::string::npos);
    EXPECT_NE(sql.find("86400"), std::string::npos);
    EXPECT_EQ(td_time_anchor_sql("t"), td_time_anchor_sql("t"));
}

// ===== 响应信封（契约 rest §2.9.2）=====

TEST(TdListEnvelopeTest, CarriesTheFiveContractKeys) {
    const nlohmann::json body = td_list_envelope(std::string("20261009"), {"ACC1", "ACC2"});
    EXPECT_TRUE(body["available"].get<bool>());
    EXPECT_EQ(body["trading_day"].get<std::string>(), "20261009");
    ASSERT_EQ(body["online_accounts"].size(), 2U);
    EXPECT_TRUE(body["items"].is_array());
    EXPECT_TRUE(body["items"].empty());
    EXPECT_TRUE(body["next_cursor"].is_null());
    EXPECT_EQ(body.size(), 5U);
}

TEST(TdListEnvelopeTest, NullTradingDayWhenNoOnlineAccounts) {
    const nlohmann::json body = td_list_envelope(std::nullopt, {});
    EXPECT_TRUE(body["trading_day"].is_null());
    EXPECT_TRUE(body["items"].empty());
    EXPECT_TRUE(body["next_cursor"].is_null());
}

}  // namespace
}  // namespace dztrader::webui
