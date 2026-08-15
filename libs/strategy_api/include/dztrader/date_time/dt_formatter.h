#ifndef DZTRADER_DATE_TIME_DT_FORMATTER_H_
#define DZTRADER_DATE_TIME_DT_FORMATTER_H_

#include <cassert>
#include <chrono>
#include <format>
#include <stdexcept>

#include "./dt_common.h"

namespace dztrader::details {

template <typename StringType, details::SubsecondDuration DurationType>
    requires requires(StringType& s, char c, const char* str) {
        StringType{};
        s.push_back(c);
        s += str;
    }
class DateTimeFormatter {
public:
    std::optional<int32_t> year = std::nullopt;
    std::optional<int32_t> month = std::nullopt;
    std::optional<int32_t> weekday = std::nullopt;
    std::optional<int32_t> day = std::nullopt;
    std::optional<int32_t> hour = std::nullopt;
    std::optional<int32_t> minute = std::nullopt;
    std::optional<int32_t> second = std::nullopt;
    std::optional<DurationType> subsecond = std::nullopt;

    StringType format(std::string_view fmt, int32_t fraction) const {
        StringType result;
        bool escape = false;
        for (char ch : fmt) {
            if (escape) {
                switch (ch) {
                    case 'Y':
                        Y_formatter(result, ch);
                        break;
                    case 'y':
                        y_formatter(result, ch);
                        break;
                    case 'm':
                        m_formatter(result, ch);
                        break;
                    case 'u':
                        u_formatter(result, ch);
                        break;
                    case 'd':
                        d_formatter(result, ch);
                        break;
                    case 'H':
                        H_formatter(result, ch);
                        break;
                    case 'M':
                        M_formatter(result, ch);
                        break;
                    case 'S':
                        S_formatter(result, ch, fraction);
                        break;
                    case 'F':
                        F_formatter(result, ch);
                        break;
                    case 'D':
                        D_formatter(result, ch);
                        break;
                    case 'R':
                        R_formatter(result, ch);
                        break;
                    case 'T':
                        T_formatter(result, ch, fraction);
                        break;
                    default:
                        result.push_back(ch);
                        break;
                }
                escape = false;
            } else if (ch == '%') {
                escape = true;
            } else {
                result.push_back(ch);
            }
        }
        // 循环结束后若 escape 仍为 true，说明格式串以单独的 % 结尾
        if (escape) {
            throw std::invalid_argument("incomplete format specifier at end of string");
        }
        return result;
    }

private:
    // 年份 2025
    void Y_formatter(StringType& dest, char ch) const {  // NOLINT
        if (!year.has_value()) {
            dest.push_back(ch);
        } else {
            pad_4_digits(year.value(), dest);
        }
    };

    // 年份最后2位 	25
    void y_formatter(StringType& dest, char ch) const {
        if (!year.has_value()) {
            dest.push_back(ch);
        } else {
            pad_2_digits(year.value() % 100, dest);
        }
    };

    // 月份 01-12
    void m_formatter(StringType& dest, char ch) const {
        if (!month.has_value()) {
            dest.push_back(ch);
        } else {
            pad_2_digits(month.value(), dest);
        }
    };

    // 将 ISO 工作日作为十进制数字写入 (1-7)，其中星期一为 1。
    void u_formatter(StringType& dest, char ch) const {
        if (!weekday.has_value()) {
            dest.push_back(ch);
        } else {
            pad_1_digits(weekday.value(), dest);
        }
    }

    // 日 01-31
    void d_formatter(StringType& dest, char ch) const {
        if (!day.has_value()) {
            dest.push_back(ch);
        } else {
            pad_2_digits(day.value(), dest);
        }
    }

    // 时 00-23
    void H_formatter(StringType& dest, char ch) const {  // NOLINT
        if (!hour.has_value()) {
            dest.push_back(ch);
        } else {
            pad_2_digits(hour.value(), dest);
        }
    }

    // 分 00-59
    void M_formatter(StringType& dest, char ch) const {  // NOLINT
        if (!minute.has_value()) {
            dest.push_back(ch);
        } else {
            pad_2_digits(minute.value(), dest);
        }
    }

    // 秒 00-59
    void S_formatter(StringType& dest, char ch, int32_t fraction) const {  // NOLINT
        if (!second.has_value()) {
            dest.push_back(ch);
            return;
        }
        // 输出整数秒（已保证为 0-59）
        pad_2_digits(second.value(), dest);

        if (!subsecond.has_value()) {
            return;
        }

        if (fraction == 0) {
            return;  // 没有小数部分
        }
        if (subsecond.value() == DurationType::zero() && fraction < 0) {
            return;  // 没有小数部分
        }

        using Period = typename DurationType::period;

        // 判断是否自动模式
        if (fraction < 0) {
            if constexpr (std::is_same_v<Period, std::milli>) {
                fraction = 3;
            } else if constexpr (std::is_same_v<Period, std::micro>) {
                fraction = 6;
            } else {
                fraction = 9;  // 默认纳秒
            }
        } else if (fraction > 9) {
            fraction = 9;
        }

        dest.push_back('.');

        // 预计算 10 的幂
        static constexpr int64_t POWERS_10[] = {1,      10,      100,      1000,      10000,
                                                100000, 1000000, 10000000, 100000000, 1000000000};

        const int32_t idx = 9 - fraction;
        assert(idx >= 0 && idx < 10);

        const int64_t divisor = POWERS_10[idx];  // NOLINT
        const int64_t display_value =
            std::chrono::duration_cast<std::chrono::nanoseconds>(subsecond.value()).count() /
            divisor;

        // 使用 std::to_chars 进行无分配整数转换
        char buffer[16];  // 足够容纳最多9位数字
        auto [ptr, ec] = std::to_chars(buffer, buffer + sizeof(buffer), display_value);
        if (ec != std::errc()) {
            return;  // 几乎不会发生
        }

        size_t len = ptr - buffer;
        if (len < static_cast<size_t>(fraction)) {
            dest.append(fraction - len, '0');  // 补前导零
        }
        dest.append(buffer, len);
    }

    // F 等价于 "%Y-%m-%d" 。
    void F_formatter(StringType& dest, char ch) const {  // NOLINT
        Y_formatter(dest, ch);
        dest.push_back('-');
        m_formatter(dest, ch);
        dest.push_back('-');
        d_formatter(dest, ch);
    }

    // D 等效于"%m/%d/%y"。
    void D_formatter(StringType& dest, char ch) const {  // NOLINT
        m_formatter(dest, ch);
        dest.push_back('/');
        d_formatter(dest, ch);
        dest.push_back('/');
        y_formatter(dest, ch);
    }

    // R 等效于"%H:%M"。
    void R_formatter(StringType& dest, char ch) const {  // NOLINT
        H_formatter(dest, ch);
        dest.push_back(':');
        M_formatter(dest, ch);
    }

    // T 等效于"%H:%M:%S"。
    void T_formatter(StringType& dest, char ch, int32_t fraction) const {  // NOLINT
        H_formatter(dest, ch);
        dest.push_back(':');
        M_formatter(dest, ch);
        dest.push_back(':');
        S_formatter(dest, ch, fraction);
    }

    template <typename DigitType>
    static void pad_1_digits(const DigitType value, StringType& dest) {
        if (value >= 0 && value < 10) [[likely]] {
            dest.push_back(static_cast<char>('0' + value));
        } else [[unlikely]] {
            throw std::out_of_range(std::format("value out of range | value={} range=0-9", value));
        }
    }

    template <typename DigitType>
    static void pad_2_digits(const DigitType value, StringType& dest) {
        static_assert(std::is_integral<DigitType>::value, "DigitType must be an integral type.");
        if (value >= 0 && value < 100) [[likely]] {
            dest.push_back(static_cast<char>('0' + (value / 10)));
            dest.push_back(static_cast<char>('0' + (value % 10)));
        } else [[unlikely]] {
            throw std::out_of_range(
                std::format("value out of range | value={} range=0-99", value));
        }
    }

    template <typename DigitType>
    static void pad_3_digits(const DigitType value, StringType& dest) {
        if (value >= 0 && value < 1000) [[likely]] {
            dest.push_back(static_cast<char>('0' + (value / 100)));
            dest.push_back(static_cast<char>('0' + ((value / 10) % 10)));
            dest.push_back(static_cast<char>('0' + (value % 10)));
        } else [[unlikely]] {
            throw std::out_of_range(
                std::format("value out of range | value={} range=0-999", value));
        }
    }

    template <typename DigitType>
    static void pad_4_digits(const DigitType value, StringType& dest) {
        if (value >= 0 && value < 10000) [[likely]] {
            dest.push_back(static_cast<char>('0' + (value / 1000)));
            dest.push_back(static_cast<char>('0' + ((value / 100) % 10)));
            dest.push_back(static_cast<char>('0' + ((value / 10) % 10)));
            dest.push_back(static_cast<char>('0' + (value % 10)));
        } else [[unlikely]] {
            throw std::out_of_range(
                std::format("value out of range | value={} range=0-9999", value));
        }
    }

    template <typename DigitType>
    static void pad_6_digits(const DigitType value, StringType& dest) {
        if (value >= 0 && value < 1000000) [[likely]] {
            dest.push_back(static_cast<char>('0' + (value / 100000)));
            dest.push_back(static_cast<char>('0' + ((value / 10000) % 10)));
            dest.push_back(static_cast<char>('0' + ((value / 1000) % 10)));
            dest.push_back(static_cast<char>('0' + ((value / 100) % 10)));
            dest.push_back(static_cast<char>('0' + ((value / 10) % 10)));
            dest.push_back(static_cast<char>('0' + (value % 10)));
        } else [[unlikely]] {
            throw std::out_of_range(
                std::format("value out of range | value={} range=0-999999", value));
        }
    }

    template <typename DigitType>
    static void pad_9_digits(const DigitType value, StringType& dest) {
        if (value >= 0 && value < 1000000000) [[likely]] {
            dest.push_back(static_cast<char>('0' + (value / 100000000)));
            dest.push_back(static_cast<char>('0' + ((value / 10000000) % 10)));
            dest.push_back(static_cast<char>('0' + ((value / 1000000) % 10)));
            dest.push_back(static_cast<char>('0' + ((value / 100000) % 10)));
            dest.push_back(static_cast<char>('0' + ((value / 10000) % 10)));
            dest.push_back(static_cast<char>('0' + ((value / 1000) % 10)));
            dest.push_back(static_cast<char>('0' + ((value / 100) % 10)));
            dest.push_back(static_cast<char>('0' + ((value / 10) % 10)));
            dest.push_back(static_cast<char>('0' + (value % 10)));
        } else [[unlikely]] {
            throw std::out_of_range(
                std::format("value out of range | value={} range=0-999999999", value));
        }
    }
};

}  // namespace dztrader::details

#endif  // DZTRADER_DATE_TIME_DT_FORMATTER_H_
