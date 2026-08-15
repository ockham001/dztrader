#include <dztrader/date_time/dt_parser.h>

#include <chrono>
#include <gtest/gtest.h>

using namespace dztrader::details;

TEST(ParserTest, ParseY) {
    auto fields = DateTimeParser::parse("2024", "%Y");
    ASSERT_TRUE(fields.year.has_value());
    EXPECT_EQ(static_cast<int>(fields.year.value()), 2024);
}

TEST(ParserTest, Parsey69) {
    auto fields = DateTimeParser::parse("69", "%y");
    ASSERT_TRUE(fields.year.has_value());
    EXPECT_EQ(static_cast<int>(fields.year.value()), 1969);
}

TEST(ParserTest, Parsey68) {
    auto fields = DateTimeParser::parse("68", "%y");
    ASSERT_TRUE(fields.year.has_value());
    EXPECT_EQ(static_cast<int>(fields.year.value()), 2068);
}

TEST(ParserTest, Parsey00) {
    auto fields = DateTimeParser::parse("00", "%y");
    ASSERT_TRUE(fields.year.has_value());
    EXPECT_EQ(static_cast<int>(fields.year.value()), 2000);
}

TEST(ParserTest, Parsem) {
    auto fields = DateTimeParser::parse("03", "%m");
    ASSERT_TRUE(fields.month.has_value());
    EXPECT_EQ(static_cast<uint32_t>(fields.month.value()), 3);
}

TEST(ParserTest, Parsed) {
    auto fields = DateTimeParser::parse("15", "%d");
    ASSERT_TRUE(fields.day.has_value());
    EXPECT_EQ(static_cast<uint32_t>(fields.day.value()), 15);
}

TEST(ParserTest, ParseH) {
    auto fields = DateTimeParser::parse("14", "%H");
    ASSERT_TRUE(fields.hour.has_value());
    EXPECT_EQ(fields.hour.value().count(), 14);
}

TEST(ParserTest, ParseM) {
    auto fields = DateTimeParser::parse("30", "%M");
    ASSERT_TRUE(fields.minute.has_value());
    EXPECT_EQ(fields.minute.value().count(), 30);
}

TEST(ParserTest, ParseSNoSubsecond) {
    auto fields = DateTimeParser::parse("45", "%S");
    ASSERT_TRUE(fields.second.has_value());
    EXPECT_EQ(fields.second.value().count(), 45);
    EXPECT_FALSE(fields.subsecond.has_value());
}

TEST(ParserTest, ParseF) {
    auto fields = DateTimeParser::parse("2024-03-15", "%F");
    ASSERT_TRUE(fields.year.has_value());
    ASSERT_TRUE(fields.month.has_value());
    ASSERT_TRUE(fields.day.has_value());
    EXPECT_EQ(static_cast<int>(fields.year.value()), 2024);
    EXPECT_EQ(static_cast<uint32_t>(fields.month.value()), 3);
    EXPECT_EQ(static_cast<uint32_t>(fields.day.value()), 15);
}

TEST(ParserTest, ParseD) {
    auto fields = DateTimeParser::parse("03/15/24", "%D");
    ASSERT_TRUE(fields.year.has_value());
    ASSERT_TRUE(fields.month.has_value());
    ASSERT_TRUE(fields.day.has_value());
    EXPECT_EQ(static_cast<int>(fields.year.value()), 2024);
    EXPECT_EQ(static_cast<uint32_t>(fields.month.value()), 3);
    EXPECT_EQ(static_cast<uint32_t>(fields.day.value()), 15);
}

TEST(ParserTest, ParseR) {
    auto fields = DateTimeParser::parse("14:30", "%R");
    ASSERT_TRUE(fields.hour.has_value());
    ASSERT_TRUE(fields.minute.has_value());
    EXPECT_EQ(fields.hour.value().count(), 14);
    EXPECT_EQ(fields.minute.value().count(), 30);
}

TEST(ParserTest, ParseTNoSubsecond) {
    auto fields = DateTimeParser::parse("14:30:45", "%T");
    ASSERT_TRUE(fields.hour.has_value());
    ASSERT_TRUE(fields.minute.has_value());
    ASSERT_TRUE(fields.second.has_value());
    EXPECT_EQ(fields.hour.value().count(), 14);
    EXPECT_EQ(fields.minute.value().count(), 30);
    EXPECT_EQ(fields.second.value().count(), 45);
}

TEST(ParserTest, Subsecond1Digit) {
    auto fields = DateTimeParser::parse("45.1", "%S");
    ASSERT_TRUE(fields.subsecond.has_value());
    EXPECT_EQ(fields.subsecond.value().count(), 100000000);
}

TEST(ParserTest, Subsecond3Digits) {
    auto fields = DateTimeParser::parse("45.123", "%S");
    ASSERT_TRUE(fields.subsecond.has_value());
    EXPECT_EQ(fields.subsecond.value().count(), 123000000);
}

TEST(ParserTest, Subsecond6Digits) {
    auto fields = DateTimeParser::parse("45.123456", "%S");
    ASSERT_TRUE(fields.subsecond.has_value());
    EXPECT_EQ(fields.subsecond.value().count(), 123456000);
}

TEST(ParserTest, Subsecond9Digits) {
    auto fields = DateTimeParser::parse("45.123456789", "%S");
    ASSERT_TRUE(fields.subsecond.has_value());
    EXPECT_EQ(fields.subsecond.value().count(), 123456789);
}

TEST(ParserTest, SubsecondMoreThan9Digits) {
    auto fields = DateTimeParser::parse("45.123456789012", "%S");
    ASSERT_TRUE(fields.subsecond.has_value());
    EXPECT_EQ(fields.subsecond.value().count(), 123456789);
}

TEST(ParserTest, SubsecondTAndSConsistent) {
    auto fields_s = DateTimeParser::parse("45.123", "%S");
    auto fields_t = DateTimeParser::parse("14:30:45.123", "%T");
    ASSERT_TRUE(fields_s.subsecond.has_value());
    ASSERT_TRUE(fields_t.subsecond.has_value());
    EXPECT_EQ(fields_s.subsecond.value(), fields_t.subsecond.value());
}

TEST(ParserTest, CheckHhMmSsHourOutOfRange) {
    DateTimeParser::ParsedFields fields;
    fields.hour = std::chrono::hours{24};
    EXPECT_THROW(DateTimeParser::check_hh_mm_ss(fields), std::out_of_range);
}

TEST(ParserTest, CheckHhMmSsMinuteOutOfRange) {
    DateTimeParser::ParsedFields fields;
    fields.minute = std::chrono::minutes{60};
    EXPECT_THROW(DateTimeParser::check_hh_mm_ss(fields), std::out_of_range);
}

TEST(ParserTest, CheckHhMmSsSecondOutOfRange) {
    DateTimeParser::ParsedFields fields;
    fields.second = std::chrono::seconds{60};
    EXPECT_THROW(DateTimeParser::check_hh_mm_ss(fields), std::out_of_range);
}

TEST(ParserTest, CheckHhMmSsSubsecondOutOfRange) {
    DateTimeParser::ParsedFields fields;
    fields.second = std::chrono::seconds{0};
    fields.subsecond = std::chrono::nanoseconds{1'000'000'000};
    EXPECT_THROW(DateTimeParser::check_hh_mm_ss(fields), std::out_of_range);
}

TEST(ParserTest, ErrorLiteralMismatch) {
    EXPECT_THROW(DateTimeParser::parse("2024-03-15", "%Y/%m/%d"), std::invalid_argument);
}

TEST(ParserTest, ErrorTrailingCharacters) {
    EXPECT_THROW(DateTimeParser::parse("2024-03-15extra", "%F"), std::invalid_argument);
}

TEST(ParserTest, ErrorIncompleteFormatSpecifier) {
    EXPECT_THROW(DateTimeParser::parse("2024", "2024%"), std::invalid_argument);
}

TEST(ParserTest, ErrorUnsupportedSpecifier) {
    EXPECT_THROW(DateTimeParser::parse("2024", "%Z"), std::invalid_argument);
}

TEST(ParserTest, ErrorInputTooShort) {
    EXPECT_THROW(DateTimeParser::parse("20", "%Y"), std::invalid_argument);
}

// 回归测试：非数字字符必须抛异常，不能静默产生垃圾值
TEST(ParserTest, ErrorNonDigitInYear) {
    EXPECT_THROW(DateTimeParser::parse("20ab", "%Y"), std::invalid_argument);
}

TEST(ParserTest, ErrorNonDigitInMonth) {
    EXPECT_THROW(DateTimeParser::parse("1x", "%m"), std::invalid_argument);
}

TEST(ParserTest, ErrorNonDigitInDay) {
    EXPECT_THROW(DateTimeParser::parse("3 ", "%d"), std::invalid_argument);
}

TEST(ParserTest, ErrorNonDigitInHour) {
    EXPECT_THROW(DateTimeParser::parse("ax:30", "%H:%M"), std::invalid_argument);
}

TEST(ParserTest, ErrorNonDigitInMinute) {
    EXPECT_THROW(DateTimeParser::parse("10:3o", "%H:%M"), std::invalid_argument);
}

TEST(ParserTest, ErrorNonDigitInCompositeFormat) {
    // %F = %Y-%m-%d，月份位置出现字母
    EXPECT_THROW(DateTimeParser::parse("2024-ab-15", "%F"), std::invalid_argument);
}

TEST(ParserTest, EmptyStringEmptyFormat) {
    auto fields = DateTimeParser::parse("", "");
    EXPECT_FALSE(fields.year.has_value());
    EXPECT_FALSE(fields.month.has_value());
    EXPECT_FALSE(fields.day.has_value());
}

TEST(ParserTest, PureTextMatch) {
    auto fields = DateTimeParser::parse("hello", "hello");
    EXPECT_FALSE(fields.year.has_value());
}
