#ifndef DZTRADER_DATE_TIME_DT_PARSER_H_
#define DZTRADER_DATE_TIME_DT_PARSER_H_
#include <cctype>
#include <chrono>
#include <format>
#include <stdexcept>

namespace dztrader::details {
struct DateTimeParser {
    // 解析结果字段
    struct ParsedFields {
        std::optional<std::chrono::year> year;
        std::optional<std::chrono::month> month;
        std::optional<std::chrono::day> day;
        std::optional<std::chrono::hours> hour;
        std::optional<std::chrono::minutes> minute;
        std::optional<std::chrono::seconds> second;
        std::optional<std::chrono::nanoseconds> subsecond;
    };

    // ---------- 基本解析器 ----------
    // 年份4位 (例如 2025)
    struct Y_parser {  // NOLINT
        static constexpr size_t LENGTH_REQUIRED = 4;
        using ResultType = std::chrono::year;
        static constexpr ResultType parse(const char* str) noexcept {
            return std::chrono::year{((str[0] - '0') * 1000) + ((str[1] - '0') * 100) +
                                     ((str[2] - '0') * 10) + (str[3] - '0')};
        }
    };

    // 年份2位 (例如 25 → 2025, 98 → 1998)
    struct y_parser {  // NOLINT
        static constexpr size_t LENGTH_REQUIRED = 2;
        using ResultType = std::chrono::year;
        static constexpr ResultType parse(const char* str) noexcept {
            int32_t yy = ((str[0] - '0') * 10) + (str[1] - '0');
            int32_t year = (yy >= 69) ? 1900 + yy : 2000 + yy;
            return std::chrono::year{year};
        }
    };

    // 月份 01-12
    struct m_parser {  // NOLINT
        static constexpr size_t LENGTH_REQUIRED = 2;
        using ResultType = std::chrono::month;
        static constexpr ResultType parse(const char* str) noexcept {
            return std::chrono::month{((str[0] - '0') * 10U) + (str[1] - '0')};
        }
    };

    // 日 01-31
    struct d_parser {  // NOLINT
        static constexpr size_t LENGTH_REQUIRED = 2;
        using ResultType = std::chrono::day;
        static constexpr ResultType parse(const char* str) noexcept {
            return std::chrono::day{((str[0] - '0') * 10U) + (str[1] - '0')};
        }
    };

    // 小时 00-23
    struct H_parser {  // NOLINT
        static constexpr size_t LENGTH_REQUIRED = 2;
        using ResultType = std::chrono::hours;
        static constexpr ResultType parse(const char* str) noexcept {
            return std::chrono::hours{((str[0] - '0') * 10U) + (str[1] - '0')};
        }
    };

    // 分钟 00-59
    struct M_parser {  // NOLINT
        static constexpr size_t LENGTH_REQUIRED = 2;
        using ResultType = std::chrono::minutes;
        static constexpr ResultType parse(const char* str) noexcept {
            return std::chrono::minutes{((str[0] - '0') * 10U) + (str[1] - '0')};
        }
    };

    // 秒 00-59 (只解析整数部分)
    struct S_parser {  // NOLINT
        static constexpr size_t LENGTH_REQUIRED = 2;
        using ResultType = std::chrono::seconds;
        static constexpr ResultType parse(const char* str) noexcept {
            return std::chrono::seconds{((str[0] - '0') * 10U) + (str[1] - '0')};
        }
    };

    // ---------- 辅助函数 ----------
    // 检查并消耗一个指定字符
    static void match_char(const char*& s, const char* s_end, char expected,
                           const char* str_base) {
        if (s == s_end || *s != expected) {
            throw std::invalid_argument(
                std::format("expected char | char={} pos={}", expected, s - str_base));
        }
        ++s;
    }

    // 使用解析器 P 从当前位置解析一个字段，并移动指针
    template <typename Parser>
    static typename Parser::ResultType parse_field(const char*& s, const char* s_end,
                                                   const char* str_base) {
        if (static_cast<size_t>(s_end - s) < Parser::LENGTH_REQUIRED) {
            throw std::invalid_argument(std::format(
                "unexpected end of string while parsing field | required={} pos={}",
                Parser::LENGTH_REQUIRED, s - str_base));
        }
        // 校验所有字符均为数字，防止 "20ab" 等输入静默产生垃圾值
        for (size_t i = 0; i < Parser::LENGTH_REQUIRED; ++i) {
            if (std::isdigit(static_cast<unsigned char>(s[i])) == 0) {
                throw std::invalid_argument(std::format(
                    "non-digit character in numeric field | char={} pos={}",
                    s[i], (s - str_base) + static_cast<ptrdiff_t>(i)));
            }
        }
        auto value = Parser::parse(s);
        s += Parser::LENGTH_REQUIRED;
        return value;
    }

    static void check_hh_mm_ss(const ParsedFields& fields) {
        if (fields.hour && fields.hour > std::chrono::hours{23}) {
            throw std::out_of_range(
                std::format("hour value out of range | hour={} range=0-23",
                            static_cast<int32_t>(fields.hour->count())));
        }
        if (fields.minute && fields.minute > std::chrono::minutes{59}) {
            throw std::out_of_range(
                std::format("minute value out of range | minute={} range=0-59",
                            static_cast<int32_t>(fields.minute->count())));
        }
        if (fields.second && fields.second > std::chrono::seconds{59}) {
            throw std::out_of_range(
                std::format("second value out of range | second={} range=0-59",
                            static_cast<int32_t>(fields.second->count())));
        }
        if (fields.second && fields.subsecond) {
            if (fields.subsecond.value() > std::chrono::nanoseconds{999'999'999}) {
                throw std::out_of_range(
                    std::format("subsecond value out of range | ns={} range=0-999999999",
                                fields.subsecond->count()));
            }
        }
    }

    // ---------- 主解析函数 ----------
    static ParsedFields parse(std::string_view str,  // NOLINT
                              std::string_view fmt) {
        const char* s = str.data();
        const char* s_end = s + str.size();
        const char* f = fmt.data();
        const char* f_end = f + fmt.size();

        ParsedFields fields;

        while (f != f_end) {
            if (*f == '%') {
                ++f;
                if (f == f_end) {
                    throw std::invalid_argument("incomplete format specifier at end of string");
                }
                switch (*f) {
                    // 基本格式符
                    case 'Y':
                        fields.year = parse_field<Y_parser>(s, s_end, str.data());
                        break;
                    case 'y':
                        fields.year = parse_field<y_parser>(s, s_end, str.data());
                        break;
                    case 'm':
                        fields.month = parse_field<m_parser>(s, s_end, str.data());
                        break;
                    case 'd':
                        fields.day = parse_field<d_parser>(s, s_end, str.data());
                        break;
                    case 'H':
                        fields.hour = parse_field<H_parser>(s, s_end, str.data());
                        break;
                    case 'M':
                        fields.minute = parse_field<M_parser>(s, s_end, str.data());
                        break;
                    case 'S': {
                        fields.second = parse_field<S_parser>(s, s_end, str.data());
                        if (s != s_end && *s == '.') {
                            ++s;
                            int64_t ns = 0;
                            int32_t digits = 0;
                            // 读取所有连续数字，但只取前9位
                            while (s != s_end && (std::isdigit(static_cast<unsigned char>(*s)) != 0)) {
                                if (digits < 9) {
                                    ns = (ns * 10) + (*s - '0');
                                }
                                ++digits;
                                ++s;
                            }
                            // 小数点后必须至少有一位数字
                            if (digits == 0) {
                                throw std::invalid_argument(std::format(
                                    "expected digits after decimal point | pos={}",
                                    s - str.data()));
                            }
                            // 如果实际位数少于9，补零到9位
                            for (int i = digits; i < 9; ++i) {
                                ns *= 10;
                            }
                            fields.subsecond = std::chrono::nanoseconds{ns};
                        }
                        break;
                    }

                    // 复合格式符（在 parse 中直接处理）
                    case 'F': {  // %Y-%m-%d
                        fields.year = parse_field<Y_parser>(s, s_end, str.data());
                        match_char(s, s_end, '-', str.data());
                        fields.month = parse_field<m_parser>(s, s_end, str.data());
                        match_char(s, s_end, '-', str.data());
                        fields.day = parse_field<d_parser>(s, s_end, str.data());
                        break;
                    }
                    case 'D': {  // %m/%d/%y
                        fields.month = parse_field<m_parser>(s, s_end, str.data());
                        match_char(s, s_end, '/', str.data());
                        fields.day = parse_field<d_parser>(s, s_end, str.data());
                        match_char(s, s_end, '/', str.data());
                        fields.year = parse_field<y_parser>(s, s_end, str.data());
                        break;
                    }
                    case 'R': {  // %H:%M
                        fields.hour = parse_field<H_parser>(s, s_end, str.data());
                        match_char(s, s_end, ':', str.data());
                        fields.minute = parse_field<M_parser>(s, s_end, str.data());
                        break;
                    }
                    case 'T': {  // %H:%M:%S (可能带小数)
                        fields.hour = parse_field<H_parser>(s, s_end, str.data());
                        match_char(s, s_end, ':', str.data());
                        fields.minute = parse_field<M_parser>(s, s_end, str.data());
                        match_char(s, s_end, ':', str.data());
                        fields.second = parse_field<S_parser>(s, s_end, str.data());
                        // 小数部分（与 %S 相同处理）
                        if (s != s_end && *s == '.') {
                            ++s;
                            int64_t ns = 0;
                            int32_t digits = 0;
                            while (s != s_end && (std::isdigit(static_cast<unsigned char>(*s)) != 0)) {
                                if (digits < 9) {
                                    ns = (ns * 10) + (*s - '0');
                                }
                                ++digits;
                                ++s;
                            }
                            // 小数点后必须至少有一位数字
                            if (digits == 0) {
                                throw std::invalid_argument(std::format(
                                    "expected digits after decimal point | pos={}",
                                    s - str.data()));
                            }
                            for (int i = digits; i < 9; ++i) {
                                ns *= 10;
                            }
                            fields.subsecond = std::chrono::nanoseconds{ns};
                        }
                        break;
                    }

                    // 字面百分号 %%：匹配输入中的一个 % 字符
                    case '%':
                        match_char(s, s_end, '%', str.data());
                        break;

                    // 不支持的格式符（可以按需添加）
                    default:
                        throw std::invalid_argument(
                            std::format("unsupported format specifier | specifier=%{}", *f));
                }
                ++f;  // 移动到格式字符串的下一个字符
            } else {
                // 普通字符必须匹配
                if (s == s_end || *s != *f) {
                    throw std::invalid_argument(
                        std::format("expected literal | char={} pos={}", *f, s - str.data()));
                }
                ++s;
                ++f;
            }
        }

        // 检查是否还有未解析的输入字符
        if (s != s_end) {
            throw std::invalid_argument(
                std::format("trailing characters after parsed string | pos={}", s - str.data()));
        }
        return fields;
    }
};
}  // namespace dztrader::details

#endif  // DZTRADER_DATE_TIME_DT_PARSER_H_
