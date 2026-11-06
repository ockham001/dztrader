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
bool account_query_matches(const std::vector<std::string>& configured, const std::string& queried);
/// 2018 三态去重判定 (契约 account-status 场景 2/3): 非强制时, 同态同交易日且非 Offline 不推;
/// 同态但 `date` 变化 (日切) 必推; Offline 恒推 (删账户/崩溃兜底语义)。
/// force (快照/查询应答) 绕过去重。
bool should_push_account_status(
    DzAccountState last, int32_t last_date, DzAccountState next, int32_t next_date, bool force);
/// "YYYYMMDD" -> 距纪元天数; 空串/非法回落 0 (契约: 非 Ready 或缺失交易日 = 0)。
int32_t epoch_day_from_yyyymmdd(const std::string& yyyymmdd);
/// 契约 account-status: 帧内 `date` 仅 Ready 时为登录返回交易日, 其余状态恒 0。
int32_t account_status_date(DzAccountState state, const std::string& date);
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
// 日期参数口径 = account_status_date 归一后的帧内交易日 (非 Ready 恒 0)。
// 真交易日取值 (距纪元天数) 只在测试内用字面量区分, 不依赖具体日历。

// 同态、同交易日且非 Offline -> 不推 (去重)
TEST(ShouldPushAccountStatus, SameStateSameDateNonOfflineSkipped) {
    EXPECT_FALSE(dztrader::ctp::should_push_account_status(DZ_ACCOUNT_LOGGING_IN, 0,
                                                           DZ_ACCOUNT_LOGGING_IN, 0, false));
    // 同 READY、日期相同 -> 不推送 (场景 2 的翻转去重)
    EXPECT_FALSE(dztrader::ctp::should_push_account_status(DZ_ACCOUNT_READY, 20697,
                                                           DZ_ACCOUNT_READY, 20697, false));
}

// force (快照/查询应答) 绕过去重: 同态同日也推
TEST(ShouldPushAccountStatus, ForceBypassesDedup) {
    EXPECT_TRUE(dztrader::ctp::should_push_account_status(DZ_ACCOUNT_READY, 20697, DZ_ACCOUNT_READY,
                                                          20697, true));
    EXPECT_TRUE(dztrader::ctp::should_push_account_status(DZ_ACCOUNT_LOGGING_IN, 0,
                                                          DZ_ACCOUNT_LOGGING_IN, 0, true));
}

// Offline 恒推: 删账户/崩溃兜底语义依赖必达 (含 Offline->Offline 同日)
TEST(ShouldPushAccountStatus, OfflineAlwaysPushes) {
    EXPECT_TRUE(dztrader::ctp::should_push_account_status(DZ_ACCOUNT_READY, 20697,
                                                          DZ_ACCOUNT_OFFLINE, 0, false));
    EXPECT_TRUE(dztrader::ctp::should_push_account_status(DZ_ACCOUNT_OFFLINE, 0, DZ_ACCOUNT_OFFLINE,
                                                          0, false));
}

// 三态翻转 -> 推
TEST(ShouldPushAccountStatus, StateChangePushes) {
    EXPECT_TRUE(dztrader::ctp::should_push_account_status(DZ_ACCOUNT_OFFLINE, 0,
                                                          DZ_ACCOUNT_LOGGING_IN, 0, false));
    EXPECT_TRUE(dztrader::ctp::should_push_account_status(DZ_ACCOUNT_LOGGING_IN, 0,
                                                          DZ_ACCOUNT_READY, 20697, false));
}

// ---- 契约 account-status 场景 3: 日切强制重推 (同态但交易日变化) ----

// 同 READY、日期不同 (会话存活跨日, 交易日前进) -> 必须推送 (突变: 去掉日期比较则此用例红)
TEST(ShouldPushAccountStatusDaySwitch, SameReadyNewDatePushes) {
    const int32_t day0 = dztrader::Date{2026, 8, 31}.days_since_epoch();
    const int32_t day1 = dztrader::Date{2026, 9, 1}.days_since_epoch();
    EXPECT_TRUE(dztrader::ctp::should_push_account_status(DZ_ACCOUNT_READY, day0, DZ_ACCOUNT_READY,
                                                          day1, false));
}

// 同 READY、日期相同 -> 不推送 (日切判重不得退化为恒推)
TEST(ShouldPushAccountStatusDaySwitch, SameReadySameDateSkipped) {
    const int32_t day0 = dztrader::Date{2026, 8, 31}.days_since_epoch();
    EXPECT_FALSE(dztrader::ctp::should_push_account_status(DZ_ACCOUNT_READY, day0, DZ_ACCOUNT_READY,
                                                           day0, false));
}

// 日期回退 (交易日倒退, 如跨年/时钟回拨) 同样视为变化 -> 推 (以 date 变化为准, 不引入方向)
TEST(ShouldPushAccountStatusDaySwitch, DateRegressionPushes) {
    EXPECT_TRUE(dztrader::ctp::should_push_account_status(DZ_ACCOUNT_READY, 20697, DZ_ACCOUNT_READY,
                                                          20696, false));
}

// 边界: 空日期/非法日期归一为 0 (account_status_date 口径), Ready 从 0 变真交易日必须推
TEST(ShouldPushAccountStatusDaySwitch, EmptyDateBoundary) {
    const int32_t day0 = dztrader::Date{2026, 8, 31}.days_since_epoch();
    const int32_t empty_as_zero = dztrader::ctp::account_status_date(DZ_ACCOUNT_READY, "");
    ASSERT_EQ(empty_as_zero, 0);
    // READY 从「无交易日」(0) 变为有交易日 -> 推
    EXPECT_TRUE(dztrader::ctp::should_push_account_status(DZ_ACCOUNT_READY, empty_as_zero,
                                                          DZ_ACCOUNT_READY, day0, false));
    // READY 两侧都无交易日 (0 == 0) -> 不推
    EXPECT_FALSE(dztrader::ctp::should_push_account_status(DZ_ACCOUNT_READY, empty_as_zero,
                                                           DZ_ACCOUNT_READY, 0, false));
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

// ---- account_status_date: 帧内交易日状态门控 (契约 account-status) ----

TEST(AccountStatusTradingDay, ReadyUsesLoginTradingDay) {
    const int32_t expected = dztrader::Date{2026, 8, 31}.days_since_epoch();
    EXPECT_EQ(dztrader::ctp::account_status_date(DZ_ACCOUNT_READY, "20260831"), expected);
}

TEST(AccountStatusTradingDay, OfflineAlwaysZero) {
    EXPECT_EQ(dztrader::ctp::account_status_date(DZ_ACCOUNT_OFFLINE, "20260831"), 0);
}

TEST(AccountStatusTradingDay, LoggingInAlwaysZero) {
    EXPECT_EQ(dztrader::ctp::account_status_date(DZ_ACCOUNT_LOGGING_IN, "20260831"), 0);
}

TEST(AccountStatusTradingDay, ReadyEmptyOrInvalidTradingDayIsZero) {
    EXPECT_EQ(dztrader::ctp::account_status_date(DZ_ACCOUNT_READY, ""), 0);
    EXPECT_EQ(dztrader::ctp::account_status_date(DZ_ACCOUNT_READY, "bad"), 0);
}
