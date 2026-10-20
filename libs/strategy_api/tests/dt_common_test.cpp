#include <dztrader/date_time/dt_common.h>

#include <chrono>
#include <ctime>
#include <string>
#include <gtest/gtest.h>

using namespace dztrader;

namespace {

/// 校验 local_time 真的填了字段，而不是只返回 true。
/// 不依赖 tzdb/current_zone(): Windows CI 的时区数据库可用性不稳定。
void expect_tm_fields_valid(const std::tm& tm, int32_t min_year, int32_t max_year) {
    EXPECT_GE(tm.tm_year, min_year);
    EXPECT_LE(tm.tm_year, max_year);
    EXPECT_GE(tm.tm_mon, 0);
    EXPECT_LE(tm.tm_mon, 11);
    EXPECT_GE(tm.tm_mday, 1);
    EXPECT_LE(tm.tm_mday, 31);
    EXPECT_GE(tm.tm_hour, 0);
    EXPECT_LE(tm.tm_hour, 23);
    EXPECT_GE(tm.tm_min, 0);
    EXPECT_LE(tm.tm_min, 59);
    EXPECT_GE(tm.tm_sec, 0);
    EXPECT_LE(tm.tm_sec, 60);  // 允许闰秒
}

}  // namespace

TEST(WeekdayTest, IsoEncodingValues) {
    EXPECT_EQ(Weekday::Monday, 1);
    EXPECT_EQ(Weekday::Tuesday, 2);
    EXPECT_EQ(Weekday::Wednesday, 3);
    EXPECT_EQ(Weekday::Thursday, 4);
    EXPECT_EQ(Weekday::Friday, 5);
    EXPECT_EQ(Weekday::Saturday, 6);
    EXPECT_EQ(Weekday::Sunday, 7);
}

TEST(WeekdayTest, ConsistentWithStdChrono) {
    EXPECT_EQ(std::chrono::Monday, std::chrono::weekday{Weekday::Monday});
    EXPECT_EQ(std::chrono::Tuesday, std::chrono::weekday{Weekday::Tuesday});
    EXPECT_EQ(std::chrono::Wednesday, std::chrono::weekday{Weekday::Wednesday});
    EXPECT_EQ(std::chrono::Thursday, std::chrono::weekday{Weekday::Thursday});
    EXPECT_EQ(std::chrono::Friday, std::chrono::weekday{Weekday::Friday});
    EXPECT_EQ(std::chrono::Saturday, std::chrono::weekday{Weekday::Saturday});
    EXPECT_EQ(std::chrono::Sunday, std::chrono::weekday{Weekday::Sunday});
}

TEST(LocalTimeTest, CurrentTimestamp) {
    std::tm result{};
    ASSERT_TRUE(details::local_time(result, std::time(nullptr)));
    expect_tm_fields_valid(result, 124, 200);  // >= 2024，且不晚于 2100
}

TEST(LocalTimeTest, EpochTimestamp) {
    std::time_t epoch = 0;
    std::tm result{};
    EXPECT_TRUE(details::local_time(result, epoch));
    EXPECT_EQ(result.tm_year, 70);
    EXPECT_EQ(result.tm_mon, 0);
    EXPECT_EQ(result.tm_mday, 1);
}

TEST(LocalTimeTest, LargeTimestamp) {
    // 2100-01-01T00:00:00Z；本地日期可能因时区偏移落在 2099-12-31。
    std::tm result{};
    ASSERT_TRUE(details::local_time(result, static_cast<std::time_t>(4102444800)));
    EXPECT_TRUE(result.tm_year == 199 || result.tm_year == 200);  // 2099 或 2100
    expect_tm_fields_valid(result, 199, 200);
}

TEST(LocalTimeTest, NegativeTimestamp) {
    const std::time_t negative = static_cast<std::time_t>(-1);
    std::tm result{};
#ifdef _WIN32
    // MSVC CRT 的 time_t 支持范围为 1970-01-01 起（见 dt_common.h 契约）；负值必须失败。
    EXPECT_FALSE(details::local_time(result, negative));
#else
    // POSIX 允许 1970 之前的时刻；字段内容按本地时区解释（UTC 为 1969-12-31）。
    ASSERT_TRUE(details::local_time(result, negative));
    EXPECT_TRUE(result.tm_year == 69 || result.tm_year == 70);
    expect_tm_fields_valid(result, 69, 70);
#endif
}

TEST(ConceptsTest, TimeStampAcceptsValidTypes) {
    static_assert(details::TimeStamp<int64_t>);
    static_assert(details::TimeStamp<int32_t>);
    static_assert(details::TimeStamp<std::time_t>);
    static_assert(details::TimeStamp<double>);
    SUCCEED();
}

TEST(ConceptsTest, TimeStampRejectsOtherTypes) {
    static_assert(!details::TimeStamp<std::string>);
    static_assert(!details::TimeStamp<float>);
    static_assert(!details::TimeStamp<unsigned int>);
    SUCCEED();
}

TEST(ConceptsTest, ChronoDurationAcceptsStandardDurations) {
    static_assert(details::ChronoDuration<std::chrono::nanoseconds>);
    static_assert(details::ChronoDuration<std::chrono::microseconds>);
    static_assert(details::ChronoDuration<std::chrono::milliseconds>);
    static_assert(details::ChronoDuration<std::chrono::seconds>);
    static_assert(details::ChronoDuration<std::chrono::minutes>);
    static_assert(details::ChronoDuration<std::chrono::hours>);
    static_assert(details::ChronoDuration<std::chrono::days>);
    SUCCEED();
}

TEST(ConceptsTest, ChronoDurationRejectsNonDurations) {
    static_assert(!details::ChronoDuration<int>);
    static_assert(!details::ChronoDuration<std::chrono::system_clock::time_point>);
    SUCCEED();
}

TEST(ConceptsTest, SubsecondDurationRejectsCoarse) {
    static_assert(details::SubsecondDuration<std::chrono::nanoseconds>);
    static_assert(details::SubsecondDuration<std::chrono::microseconds>);
    static_assert(details::SubsecondDuration<std::chrono::milliseconds>);
    static_assert(!details::SubsecondDuration<std::chrono::seconds>);
    static_assert(!details::SubsecondDuration<std::chrono::minutes>);
    static_assert(!details::SubsecondDuration<std::chrono::hours>);
    SUCCEED();
}

TEST(ConceptsTest, CoarseTimeDurationAcceptsOnlySpecific) {
    static_assert(details::CoarseTimeDuration<std::chrono::milliseconds>);
    static_assert(details::CoarseTimeDuration<std::chrono::seconds>);
    static_assert(details::CoarseTimeDuration<std::chrono::minutes>);
    static_assert(details::CoarseTimeDuration<std::chrono::hours>);
    static_assert(!details::CoarseTimeDuration<std::chrono::nanoseconds>);
    static_assert(!details::CoarseTimeDuration<std::chrono::microseconds>);
    SUCCEED();
}

TEST(ConceptsTest, PreciseTimeDurationAcceptsFullRange) {
    static_assert(details::PreciseTimeDuration<std::chrono::nanoseconds>);
    static_assert(details::PreciseTimeDuration<std::chrono::microseconds>);
    static_assert(details::PreciseTimeDuration<std::chrono::milliseconds>);
    static_assert(details::PreciseTimeDuration<std::chrono::seconds>);
    static_assert(details::PreciseTimeDuration<std::chrono::minutes>);
    static_assert(details::PreciseTimeDuration<std::chrono::hours>);
    SUCCEED();
}
