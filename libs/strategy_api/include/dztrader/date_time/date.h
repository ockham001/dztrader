#ifndef DZTRADER_DATE_TIME_DATE_H_
#define DZTRADER_DATE_TIME_DATE_H_

#include <chrono>
#include <cstdint>
#include <format>
#include <stdexcept>

#include "./dt_common.h"
#include "./dt_formatter.h"
#include "./dt_parser.h"

namespace dztrader {

class Date {
    using Parser = details::DateTimeParser;  ///< 解析器类型别名

public:
    constexpr Date() = default;

    constexpr explicit Date(int32_t days_since_epoch) noexcept
        : duration_(days_since_epoch) {}

    constexpr Date(const std::chrono::year_month_day& ymd) noexcept  // NOLINT
        : Date(std::chrono::sys_days{ymd}.time_since_epoch().count()) {}

    constexpr Date(const std::chrono::year_month_weekday& ymwd)  // NOLINT
        noexcept
        : Date(std::chrono::sys_days{ymwd}.time_since_epoch().count()) {}

    constexpr Date(const std::chrono::year_month_day_last& ymdl)  // NOLINT
        noexcept
        : Date(std::chrono::sys_days{ymdl}.time_since_epoch().count()) {}

    constexpr Date(const std::chrono::year_month_weekday_last& ymwdl)  // NOLINT
        noexcept
        : Date(std::chrono::sys_days{ymwdl}.time_since_epoch().count()) {}

    constexpr Date(std::chrono::year y, std::chrono::month m, std::chrono::day d) noexcept
        : Date(std::chrono::year_month_day{y, m, d}) {}

    constexpr Date(int32_t y, int32_t m, int32_t d) noexcept
        : Date(std::chrono::year(y), std::chrono::month(m), std::chrono::day(d)) {}

    //

    [[nodiscard]] constexpr int32_t year() const noexcept {
        return static_cast<int32_t>(year_month_day().year());
    }

    [[nodiscard]] constexpr int32_t month() const noexcept {
        return static_cast<int32_t>(static_cast<uint32_t>(year_month_day().month()));
    }

    [[nodiscard]] constexpr Weekday weekday() const noexcept {
        return static_cast<Weekday>(
            static_cast<uint32_t>(year_month_weekday().weekday().iso_encoding()));
    }

    [[nodiscard]] constexpr int32_t day() const noexcept {
        return static_cast<int32_t>(static_cast<uint32_t>(year_month_day().day()));
    }

    [[nodiscard]] constexpr std::chrono::year_month_day year_month_day() const noexcept {
        return std::chrono::year_month_day{std::chrono::sys_days{std::chrono::days{duration_}}};
    }

    [[nodiscard]] constexpr std::chrono::year_month_weekday year_month_weekday() const noexcept {
        return std::chrono::year_month_weekday{std::chrono::sys_days{std::chrono::days{duration_}}};
    }

    [[nodiscard]] constexpr int32_t days_since_epoch() const noexcept { return duration_; }

    template <details::ChronoDuration DurationType = std::chrono::days>
    [[nodiscard]] constexpr DurationType time_since_epoch() const noexcept {
        return std::chrono::duration_cast<DurationType>(std::chrono::days{duration_});
    }

    //

    [[nodiscard]] constexpr Date add_days(int32_t days) const noexcept {
        return Date{duration_ + days};
    }

    [[nodiscard]] constexpr Date add_months(int32_t months) const noexcept {
        return *this + std::chrono::months{months};
    }

    [[nodiscard]] constexpr Date add_years(int32_t years) const noexcept {
        return *this + std::chrono::years{years};
    }

    [[nodiscard]] constexpr int32_t days_to(const Date& d) const noexcept {
        return d.duration_ - duration_;
    }

    template <typename StringType = std::string>
    StringType to_string(std::string_view fmt = DEFAULT_FORMAT) const {
        const auto ymd = year_month_day();
        const auto ymwd = year_month_weekday();
        return details::DateTimeFormatter<StringType, std::chrono::nanoseconds>{
            .year = static_cast<int32_t>(ymd.year()),
            .month = static_cast<int32_t>(static_cast<uint32_t>(ymd.month())),
            .weekday = static_cast<int32_t>(ymwd.weekday().iso_encoding()),
            .day = static_cast<int32_t>(static_cast<uint32_t>(ymd.day())),
        }
            .format(fmt, DEFAULT_FRACTION);
    }

    //

    static Date from_string(std::string_view str, std::string_view fmt = DEFAULT_FORMAT) {
        auto fields = Parser::parse(str, fmt);
        if (!fields.year || !fields.month || !fields.day) {
            throw std::invalid_argument(std::format(
                "missing date components | year={} month={} day={}",
                fields.year.has_value(), fields.month.has_value(), fields.day.has_value()));
        }
        return Date::from_year_month_day(*fields.year, *fields.month, *fields.day);
    }

    static constexpr Date from_year_month_day(const std::chrono::year_month_day& ymd) {
        if (ymd.ok()) [[likely]] {
            return Date{ymd};
        } else [[unlikely]] {
            throw std::invalid_argument("invalid year_month_day");
        }
    }

    static constexpr Date from_year_month_day(std::chrono::year y,
                                              std::chrono::month m,
                                              std::chrono::day d) {
        if (!y.ok() || !m.ok() || !d.ok()) [[unlikely]] {
            throw std::invalid_argument("invalid year or month or day");
        }
        return from_year_month_day(std::chrono::year_month_day{y, m, d});
    }

    static constexpr Date from_year_month_day(int32_t y, int32_t m, int32_t d) {
        return from_year_month_day(std::chrono::year(y), std::chrono::month(m),
                                   std::chrono::day(d));
    }

    static constexpr Date from_year_month_weekday(const std::chrono::year_month_weekday& ymwd) {
        if (ymwd.ok()) [[likely]] {
            return Date{ymwd};
        } else [[unlikely]] {
            throw std::invalid_argument("invalid year_month_weekday");
        }
    }

    static constexpr Date from_weekday_indexed(std::chrono::year y,
                                               std::chrono::month m,
                                               std::chrono::weekday_indexed wd) {
        if (!y.ok() || !m.ok() || !wd.ok()) [[unlikely]] {
            throw std::invalid_argument("invalid year or month or weekday_indexed");
        }
        return from_year_month_weekday(std::chrono::year_month_weekday{y, m, wd});
    }

    static constexpr Date from_weekday_indexed(int32_t y, int32_t m, int32_t wd, int32_t index) {
        return from_weekday_indexed(
            std::chrono::year(y), std::chrono::month(m),
            std::chrono::weekday_indexed{std::chrono::weekday(wd), static_cast<uint32_t>(index)});
    }

    static constexpr Date from_year_month_day_last(const std::chrono::year_month_day_last& ymdl) {
        if (ymdl.ok()) [[likely]] {
            return Date{ymdl};
        } else [[unlikely]] {
            throw std::invalid_argument("invalid date");
        }
    }

    static constexpr Date from_year_month_day_last(const std::chrono::year& y,
                                                   const std::chrono::month_day_last& mdl) {
        if (!y.ok() || !mdl.ok()) [[unlikely]] {
            throw std::invalid_argument("invalid date");
        }
        return from_year_month_day_last(std::chrono::year_month_day_last{y, mdl});
    }

    static constexpr Date from_year_month_day_last(const std::chrono::year& y,
                                                   const std::chrono::month& m) {
        if (!y.ok() || !m.ok()) [[unlikely]] {
            throw std::invalid_argument("invalid date");
        }
        return from_year_month_day_last(
            std::chrono::year_month_day_last{y, std::chrono::month_day_last{m}});
    }

    static constexpr Date from_year_month_day_last(int32_t y, int32_t m) {
        return from_year_month_day_last(std::chrono::year(y), std::chrono::month(m));
    }

    static constexpr Date from_year_month_weekday_last(
        const std::chrono::year_month_weekday_last& ymdwl) {
        if (ymdwl.ok()) [[likely]] {
            return Date{ymdwl};
        } else [[unlikely]] {
            throw std::invalid_argument("invalid date");
        }
    }

    static constexpr Date from_year_month_weekday_last(const std::chrono::year& y,
                                                       const std::chrono::month& m,
                                                       const std::chrono::weekday_last& wdl) {
        if (!y.ok() || !m.ok() || !wdl.ok()) [[unlikely]] {
            throw std::invalid_argument("invalid date");
        }
        return from_year_month_weekday_last(std::chrono::year_month_weekday_last{y, m, wdl});
    }

    static constexpr Date from_year_month_weekday_last(const std::chrono::year& y,
                                                       const std::chrono::month& m,
                                                       const std::chrono::weekday& wd) {
        if (!y.ok() || !m.ok() || !wd.ok()) [[unlikely]] {
            throw std::invalid_argument("invalid date");
        }
        return from_year_month_weekday_last(
            std::chrono::year_month_weekday_last{y, m, std::chrono::weekday_last{wd}});
    }

    static constexpr Date from_year_month_weekday_last(int32_t y, int32_t m, int32_t wd) {
        return from_year_month_weekday_last(std::chrono::year(y), std::chrono::month(m),
                                            std::chrono::weekday(wd));
    }

    //

    friend constexpr bool operator==(const Date&, const Date&) = default;
    friend constexpr auto operator<=>(const Date&, const Date&) = default;

    constexpr Date operator+(std::chrono::days d) const noexcept {
        return Date(duration_ + d.count());
    }

    constexpr Date operator-(std::chrono::days d) const noexcept {
        return Date(duration_ - d.count());
    }

    constexpr Date& operator+=(std::chrono::days d) noexcept {
        *this = *this + d;
        return *this;
    }

    constexpr Date& operator-=(std::chrono::days d) noexcept {
        *this = *this - d;
        return *this;
    }

    constexpr std::chrono::days operator-(const Date& rhs) const noexcept {
        return std::chrono::days{duration_ - rhs.duration_};
    }

    constexpr Date operator+(std::chrono::months m) const noexcept {
        if (m.count() == 0) {
            return *this;
        }
        const auto ymd = year_month_day();
        std::chrono::year_month ym = ymd.year() / ymd.month();
        ym += m;
        auto new_ymd = ym / ymd.day();
        if (new_ymd.ok()) {
            return Date{new_ymd};
        }
        return Date{
            std::chrono::year_month_day_last{ym.year(), std::chrono::month_day_last{ym.month()}}};
    }

    constexpr Date operator-(std::chrono::months m) const noexcept {
        return *this + std::chrono::months{-m.count()};
    }

    constexpr Date& operator+=(std::chrono::months m) noexcept {
        *this = *this + m;
        return *this;
    }

    constexpr Date& operator-=(std::chrono::months m) noexcept {
        *this = *this - m;
        return *this;
    }

    constexpr Date operator+(std::chrono::years y) const noexcept {
        if (y.count() == 0) {
            return *this;
        }
        const auto ymd = year_month_day();
        std::chrono::year_month ym = ymd.year() / ymd.month();
        ym += y;
        auto new_ymd = ym / ymd.day();
        if (new_ymd.ok()) {
            return Date{new_ymd};
        }
        return Date{
            std::chrono::year_month_day_last{ym.year(), std::chrono::month_day_last{ym.month()}}};
    }

    constexpr Date operator-(std::chrono::years y) const noexcept {
        return *this + std::chrono::years{-y.count()};
    }

    constexpr Date& operator+=(std::chrono::years y) noexcept {
        *this = *this + y;
        return *this;
    }

    constexpr Date& operator-=(std::chrono::years y) noexcept {
        *this = *this - y;
        return *this;
    }

    friend std::ostream& operator<<(std::ostream& os, const Date& date) {
        os << date.to_string();
        return os;
    }

private:
    // ---------- 静态断言（确保星期枚举与标准库一致）----------
    static_assert(std::chrono::Monday == std::chrono::weekday{Weekday::Monday});
    static_assert(std::chrono::Tuesday == std::chrono::weekday{Weekday::Tuesday});
    static_assert(std::chrono::Wednesday == std::chrono::weekday{Weekday::Wednesday});
    static_assert(std::chrono::Thursday == std::chrono::weekday{Weekday::Thursday});
    static_assert(std::chrono::Friday == std::chrono::weekday{Weekday::Friday});
    static_assert(std::chrono::Saturday == std::chrono::weekday{Weekday::Saturday});
    static_assert(std::chrono::Sunday == std::chrono::weekday{Weekday::Sunday});

    static constexpr char DEFAULT_FORMAT[] = "%Y-%m-%d";
    static constexpr int32_t DEFAULT_FRACTION = -1;

    int32_t duration_ = 0;  ///< 距离纪元（1970-01-01）的天数
};

}  // namespace dztrader

#endif  // DZTRADER_DATE_TIME_DATE_H_