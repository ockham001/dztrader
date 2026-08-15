#include <dztrader/date_time/dt_common.h>

#include <chrono>
#include <ctime>
#include <gtest/gtest.h>

using namespace dztrader;

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
    auto now = std::time(nullptr);
    std::tm result{};
    EXPECT_TRUE(details::local_time(result, now));
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
    std::time_t large = 4102444800;
    std::tm result{};
    EXPECT_TRUE(details::local_time(result, large));
}

TEST(LocalTimeTest, NegativeTimestamp) {
    std::time_t neg = -1;
    std::tm result{};
    details::local_time(result, neg);
}

TEST(ConceptsTest, TimeStampAcceptsValidTypes) {
    static_assert(details::TimeStamp<int64_t>);
    static_assert(details::TimeStamp<int32_t>);
    static_assert(details::TimeStamp<std::time_t>);
    static_assert(details::TimeStamp<double>);
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
