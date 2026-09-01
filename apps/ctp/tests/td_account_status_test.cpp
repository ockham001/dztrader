// td_account_status_test: 2018 推送相关纯函数单元测试 (account_query_matches 路由 +
// should_push_account_status 三态去重 + epoch_day_from_yyyymmdd 交易日转换)。
// 纯函数实现在 td_api_pure.cpp, 不依赖 TdApi 类和 CTP 头文件,
// 因此测试目标无需链接 dztd_ctp_api (与 td_api_pure_test.cpp 同款 extern 声明模式)。

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include <dztrader/data_type.h>
#include <dztrader/date_time/date_time.h>

// 与既有 td_api_pure_test.cpp 同款 extern 声明模式 (该文件在 namespace dztrader::ctp
// 内 extern 声明, 不 include td_api.h——它携带 TdApi 类定义, 测试目标未链接完整依赖)。
// 本文件同做法: 在 namespace dztrader::ctp {} 内 extern 声明, 签名与 td_api.h 纯函数区一致
namespace dztrader::ctp {
bool account_query_matches(const std::vector<std::string>& configured,
                           const std::string& queried);
/// 2018 三态去重判定 (契约 account-status): 非强制时, 同态且非 Offline 不推;
/// Offline 恒推 (删账户/崩溃兜底语义)。force (快照/查询应答) 绕过去重。
bool should_push_account_status(DzAccountState last, DzAccountState next, bool force);
/// "YYYYMMDD" -> 距纪元天数; 空串/非法回落 0 (契约: 非 Ready 或缺失交易日 = 0)。
int32_t epoch_day_from_yyyymmdd(const std::string& yyyymmdd);
}  // namespace dztrader::ctp

TEST(AccountQueryRoute, EmptyQueryMeansAll) {
    EXPECT_TRUE(dztrader::ctp::account_query_matches({"A", "B"}, ""));
}
TEST(AccountQueryRoute, ConfiguredAccountMatches) {
    EXPECT_TRUE(dztrader::ctp::account_query_matches({"A", "B"}, "A"));
}
TEST(AccountQueryRoute, UnconfiguredNotMatched) {
    EXPECT_FALSE(dztrader::ctp::account_query_matches({"A", "B"}, "C"));
}

// ---- should_push_account_status: 三态去重判定 (audit C-F1: 突变类覆盖) ----

// 同态且非 Offline -> 不推 (去重)
TEST(ShouldPushAccountStatus, SameStateNonOfflineSkipped) {
    EXPECT_FALSE(dztrader::ctp::should_push_account_status(
        DZ_ACCOUNT_LOGGING_IN, DZ_ACCOUNT_LOGGING_IN, false));
    EXPECT_FALSE(dztrader::ctp::should_push_account_status(
        DZ_ACCOUNT_READY, DZ_ACCOUNT_READY, false));
}

// force (快照/查询应答) 绕过去重: 同态也推
TEST(ShouldPushAccountStatus, ForceBypassesDedup) {
    EXPECT_TRUE(dztrader::ctp::should_push_account_status(
        DZ_ACCOUNT_READY, DZ_ACCOUNT_READY, true));
    EXPECT_TRUE(dztrader::ctp::should_push_account_status(
        DZ_ACCOUNT_LOGGING_IN, DZ_ACCOUNT_LOGGING_IN, true));
}

// Offline 恒推: 删账户/崩溃兜底语义依赖必达 (含 Offline->Offline)
TEST(ShouldPushAccountStatus, OfflineAlwaysPushes) {
    EXPECT_TRUE(dztrader::ctp::should_push_account_status(
        DZ_ACCOUNT_READY, DZ_ACCOUNT_OFFLINE, false));
    EXPECT_TRUE(dztrader::ctp::should_push_account_status(
        DZ_ACCOUNT_OFFLINE, DZ_ACCOUNT_OFFLINE, false));
}

// 三态翻转 -> 推
TEST(ShouldPushAccountStatus, StateChangePushes) {
    EXPECT_TRUE(dztrader::ctp::should_push_account_status(
        DZ_ACCOUNT_OFFLINE, DZ_ACCOUNT_LOGGING_IN, false));
    EXPECT_TRUE(dztrader::ctp::should_push_account_status(
        DZ_ACCOUNT_LOGGING_IN, DZ_ACCOUNT_READY, false));
}

// ---- epoch_day_from_yyyymmdd: 交易日转换 (audit C-F1) ----

TEST(EpochDayFromYyyymmdd, ValidDate) {
    const int32_t expected = dztrader::Date{2026, 8, 31}.days_since_epoch();
    EXPECT_EQ(dztrader::ctp::epoch_day_from_yyyymmdd("20260831"), expected);
}

TEST(EpochDayFromYyyymmdd, EmptyStringFallsBackToZero) {
    EXPECT_EQ(dztrader::ctp::epoch_day_from_yyyymmdd(""), 0);
}

TEST(EpochDayFromYyyymmdd, NonNumericFallsBackToZero) {
    EXPECT_EQ(dztrader::ctp::epoch_day_from_yyyymmdd("bad"), 0);
}

TEST(EpochDayFromYyyymmdd, InvalidMonthDayFallsBackToZero) {
    EXPECT_EQ(dztrader::ctp::epoch_day_from_yyyymmdd("20261399"), 0);
}
