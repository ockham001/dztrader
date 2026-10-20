#include <dztrader/date_time/dt_formatter.h>

#include <chrono>
#include <memory_resource>
#include <string>
#include <gtest/gtest.h>

using namespace dztrader::details;

using Formatter = DateTimeFormatter<std::string, std::chrono::nanoseconds>;
using MilliFormatter = DateTimeFormatter<std::string, std::chrono::milliseconds>;
using MicroFormatter = DateTimeFormatter<std::string, std::chrono::microseconds>;

TEST(FormatterTest, FormatY) {
    Formatter f{.year = 2024};
    EXPECT_EQ(f.format("%Y", -1), "2024");
}

TEST(FormatterTest, Formaty) {
    Formatter f{.year = 2024};
    EXPECT_EQ(f.format("%y", -1), "24");
}

TEST(FormatterTest, Formatm) {
    Formatter f{.month = 3};
    EXPECT_EQ(f.format("%m", -1), "03");
}

TEST(FormatterTest, Formatu) {
    Formatter f{.weekday = 5};
    EXPECT_EQ(f.format("%u", -1), "5");
}

TEST(FormatterTest, Formatd) {
    Formatter f{.day = 15};
    EXPECT_EQ(f.format("%d", -1), "15");
}

TEST(FormatterTest, FormatH) {
    Formatter f{.hour = 9};
    EXPECT_EQ(f.format("%H", -1), "09");
}

TEST(FormatterTest, FormatM) {
    Formatter f{.minute = 30};
    EXPECT_EQ(f.format("%M", -1), "30");
}

TEST(FormatterTest, FormatSNoSubsecond) {
    Formatter f{.second = 45};
    EXPECT_EQ(f.format("%S", -1), "45");
}

TEST(FormatterTest, FormatF) {
    Formatter f{.year = 2024, .month = 3, .day = 15};
    EXPECT_EQ(f.format("%F", -1), "2024-03-15");
}

TEST(FormatterTest, FormatD) {
    Formatter f{.year = 2024, .month = 3, .day = 15};
    EXPECT_EQ(f.format("%D", -1), "03/15/24");
}

TEST(FormatterTest, FormatR) {
    Formatter f{.hour = 14, .minute = 30};
    EXPECT_EQ(f.format("%R", -1), "14:30");
}

TEST(FormatterTest, FormatTNoSubsecond) {
    Formatter f{.hour = 14, .minute = 30, .second = 45};
    EXPECT_EQ(f.format("%T", -1), "14:30:45");
}

TEST(FormatterTest, SubsecondAutoMilli) {
    MilliFormatter f{.second = 45, .subsecond = std::chrono::milliseconds{123}};
    EXPECT_EQ(f.format("%S", -1), "45.123");
}

TEST(FormatterTest, SubsecondAutoMicro) {
    MicroFormatter f{.second = 45, .subsecond = std::chrono::microseconds{123456}};
    EXPECT_EQ(f.format("%S", -1), "45.123456");
}

TEST(FormatterTest, SubsecondAutoNano) {
    Formatter f{.second = 45, .subsecond = std::chrono::nanoseconds{123456789}};
    EXPECT_EQ(f.format("%S", -1), "45.123456789");
}

TEST(FormatterTest, SubsecondFractionZero) {
    Formatter f{.second = 45, .subsecond = std::chrono::nanoseconds{123456789}};
    EXPECT_EQ(f.format("%S", 0), "45");
}

TEST(FormatterTest, SubsecondFractionPositive) {
    Formatter f{.second = 45, .subsecond = std::chrono::nanoseconds{123456789}};
    EXPECT_EQ(f.format("%S", 3), "45.123");
    EXPECT_EQ(f.format("%S", 6), "45.123456");
    EXPECT_EQ(f.format("%S", 9), "45.123456789");
}

TEST(FormatterTest, SubsecondFractionGreaterThan9) {
    Formatter f{.second = 45, .subsecond = std::chrono::nanoseconds{123456789}};
    EXPECT_EQ(f.format("%S", 15), "45.123456789");
}

TEST(FormatterTest, SubsecondZeroAutoMode) {
    Formatter f{.second = 45, .subsecond = std::chrono::nanoseconds{0}};
    EXPECT_EQ(f.format("%S", -1), "45");
}

TEST(FormatterTest, SubsecondZeroFractionPositive) {
    Formatter f{.second = 45, .subsecond = std::chrono::nanoseconds{0}};
    EXPECT_EQ(f.format("%S", 3), "45.000");
}

TEST(FormatterTest, SubsecondNullopt) {
    Formatter f{.second = 45};
    EXPECT_EQ(f.format("%S", -1), "45");
}

TEST(FormatterTest, NulloptFieldsOutputRawChar) {
    Formatter f{};
    EXPECT_EQ(f.format("%Y", -1), "Y");
    EXPECT_EQ(f.format("%y", -1), "y");
    EXPECT_EQ(f.format("%m", -1), "m");
    EXPECT_EQ(f.format("%u", -1), "u");
    EXPECT_EQ(f.format("%d", -1), "d");
    EXPECT_EQ(f.format("%H", -1), "H");
    EXPECT_EQ(f.format("%M", -1), "M");
    EXPECT_EQ(f.format("%S", -1), "S");
}

TEST(FormatterTest, EscapePercent) {
    Formatter f{};
    EXPECT_EQ(f.format("%%", -1), "%");
}

// 契约：未知说明符属于格式/结构错误 → std::invalid_argument（与解析侧一致）。
TEST(FormatterTest, UnknownSpecifier) {
    Formatter f{};
    EXPECT_THROW(f.format("%Z", -1), std::invalid_argument);
}

// 异常分级约定（与实现 4328a32 一致, 也与 C++ 标准库惯例一致）:
//   数值越界（value 超出 [min,max]）→ std::out_of_range
//   格式/结构错误（未知说明符、残留 % 等）→ std::invalid_argument
TEST(FormatterTest, Pad1DigitOutOfRange) {
    Formatter f{.weekday = -1};
    EXPECT_THROW(f.format("%u", -1), std::out_of_range);
    Formatter f2{.weekday = 10};
    EXPECT_THROW(f2.format("%u", -1), std::out_of_range);
}

TEST(FormatterTest, Pad2DigitsOutOfRange) {
    Formatter f{.month = -1};
    EXPECT_THROW(f.format("%m", -1), std::out_of_range);
    Formatter f2{.month = 100};
    EXPECT_THROW(f2.format("%m", -1), std::out_of_range);
}

TEST(FormatterTest, Pad4DigitsOutOfRange) {
    Formatter f{.year = -1};
    EXPECT_THROW(f.format("%Y", -1), std::out_of_range);
    Formatter f2{.year = 10000};
    EXPECT_THROW(f2.format("%Y", -1), std::out_of_range);
}

// 字段语义边界：上界接受、越界拒绝（宽度检查之外，实现须校验 01-12/01-31/00-23/00-59/1-7）。
TEST(FormatterTest, PadFieldSemanticBoundaries) {
    EXPECT_EQ((Formatter{.month = 12}).format("%m", -1), "12");
    EXPECT_EQ((Formatter{.day = 31}).format("%d", -1), "31");
    EXPECT_EQ((Formatter{.hour = 23}).format("%H", -1), "23");
    EXPECT_EQ((Formatter{.minute = 59}).format("%M", -1), "59");
    EXPECT_EQ((Formatter{.second = 59}).format("%S", -1), "59");
    EXPECT_EQ((Formatter{.weekday = 7}).format("%u", -1), "7");

    EXPECT_THROW((Formatter{.month = 13}).format("%m", -1), std::out_of_range);
    EXPECT_THROW((Formatter{.month = 0}).format("%m", -1), std::out_of_range);
    EXPECT_THROW((Formatter{.day = 32}).format("%d", -1), std::out_of_range);
    EXPECT_THROW((Formatter{.day = 0}).format("%d", -1), std::out_of_range);
    EXPECT_THROW((Formatter{.hour = 24}).format("%H", -1), std::out_of_range);
    EXPECT_THROW((Formatter{.minute = 60}).format("%M", -1), std::out_of_range);
    EXPECT_THROW((Formatter{.second = 60}).format("%S", -1), std::out_of_range);
    EXPECT_THROW((Formatter{.weekday = 8}).format("%u", -1), std::out_of_range);
    EXPECT_THROW((Formatter{.weekday = 0}).format("%u", -1), std::out_of_range);
}

// 与解析侧对称：subsecond 合法区间 [0, 999999999] ns，>= 1s 或负值必须拒绝。
TEST(FormatterTest, SubsecondOutOfRange) {
    EXPECT_THROW((Formatter{.second = 45, .subsecond = std::chrono::nanoseconds{1000000000}})
                     .format("%S", -1),
                 std::out_of_range);
    EXPECT_THROW((Formatter{.second = 45, .subsecond = std::chrono::nanoseconds{1500000000}})
                     .format("%S", -1),
                 std::out_of_range);
    EXPECT_THROW(
        (Formatter{.second = 45, .subsecond = std::chrono::nanoseconds{-1}}).format("%S", -1),
        std::out_of_range);

    MilliFormatter f{.second = 45, .subsecond = std::chrono::milliseconds{1000}};
    EXPECT_THROW(f.format("%S", -1), std::out_of_range);
}

// 回归测试：格式串以单独的 % 结尾必须报错，不能静默丢弃
TEST(FormatterTest, ErrorTrailingPercent) {
    Formatter f{.year = 2024};
    EXPECT_THROW(f.format("%Y-%", -1), std::invalid_argument);
    EXPECT_THROW(f.format("%", -1), std::invalid_argument);
}

TEST(FormatterTest, EmptyFormat) {
    Formatter f{.year = 2024};
    EXPECT_EQ(f.format("", -1), "");
}

TEST(FormatterTest, PureText) {
    Formatter f{};
    EXPECT_EQ(f.format("hello", -1), "hello");
}

TEST(FormatterTest, StringTypeTemplate) {
    // 用第二个 StringType（std::pmr::string）验证模板参数真正参与实例化。
    DateTimeFormatter<std::pmr::string, std::chrono::nanoseconds> f{
        .year = 2024, .month = 3, .day = 15};
    const std::pmr::string formatted = f.format("%F", -1);
    EXPECT_EQ(formatted, "2024-03-15");
}
