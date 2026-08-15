#ifndef DZTRADER_DATE_TIME_TIME_H_
#define DZTRADER_DATE_TIME_TIME_H_

#include <chrono>
#include <cstdint>

#include "./dt_common.h"
#include "./dt_formatter.h"
#include "./dt_parser.h"

namespace dztrader {

class Time {
    using Parser = details::DateTimeParser;

public:
    constexpr Time() = default;

    constexpr explicit Time(int32_t millisecs_since_midnight) noexcept
        : duration_(millisecs_since_midnight) {}

    constexpr Time(int32_t h, int32_t m, int32_t s = 0, int32_t ms = 0) noexcept
        : duration_{static_cast<int32_t>(
              (static_cast<int64_t>(h) * MSECS_PER_HOUR) +
              (static_cast<int64_t>(m) * MSECS_PER_MIN) +
              (static_cast<int64_t>(s) * MSECS_PER_SEC) +
              static_cast<int64_t>(ms))} {}

    constexpr Time(std::chrono::hours h,
                   std::chrono::minutes m,
                   std::chrono::seconds s = std::chrono::seconds{0},
                   std::chrono::milliseconds ms = std::chrono::milliseconds{0}) noexcept
        : Time(static_cast<int32_t>(h.count()),
               static_cast<int32_t>(m.count()),
               static_cast<int32_t>(s.count()),
               static_cast<int32_t>(ms.count())) {}

    template <class Rep, class Period>
    constexpr Time(  // NOLINT
        const std::chrono::hh_mm_ss<std::chrono::duration<Rep, Period>>& hms) noexcept
        : duration_{static_cast<int32_t>(
              std::chrono::duration_cast<std::chrono::milliseconds>(hms.to_duration()).count())} {}

    //

    [[nodiscard]] constexpr int32_t hour() const noexcept { return duration_ / MSECS_PER_HOUR; }

    [[nodiscard]] constexpr int32_t minute() const noexcept {
        return (duration_ % MSECS_PER_HOUR) / MSECS_PER_MIN;
    }

    [[nodiscard]] constexpr int32_t second() const noexcept {
        return (duration_ / MSECS_PER_SEC) % SECS_PER_MIN;
    }

    [[nodiscard]] constexpr int32_t millisec() const noexcept { return duration_ % MSECS_PER_SEC; }

    [[nodiscard]] constexpr std::chrono::milliseconds subsecond() const noexcept {
        return std::chrono::milliseconds{millisec()};
    }

    //

    template <details::CoarseTimeDuration DurationType = std::chrono::milliseconds>
    [[nodiscard]] constexpr DurationType time_since_midnight() const noexcept {
        return std::chrono::duration_cast<DurationType>(std::chrono::milliseconds{duration_});
    }

    [[nodiscard]] constexpr int32_t millisecs_since_midnight() const noexcept { return duration_; }

    [[nodiscard]] constexpr int32_t seconds_since_midnight() const noexcept {
        return duration_ / MSECS_PER_SEC;
    }

    [[nodiscard]] constexpr int32_t minutes_since_midnight() const noexcept {
        return duration_ / MSECS_PER_MIN;
    }

    [[nodiscard]] constexpr int32_t hours_since_midnight() const noexcept {
        return duration_ / MSECS_PER_HOUR;
    }

    //

    [[nodiscard]] constexpr Time add_hours(int32_t h) const noexcept {
        return (*this) + std::chrono::hours{h};
    }

    [[nodiscard]] constexpr Time add_minutes(int32_t m) const noexcept {
        return (*this) + std::chrono::minutes{m};
    }

    [[nodiscard]] constexpr Time add_seconds(int32_t s) const noexcept {
        return (*this) + std::chrono::seconds{s};
    }

    [[nodiscard]] constexpr Time add_millisecs(int32_t ms) const noexcept {
        return (*this) + std::chrono::milliseconds{ms};
    }

    [[nodiscard]] constexpr int32_t hours_to(const Time& other) const noexcept {
        return millisecs_to(other) / MSECS_PER_HOUR;
    }

    [[nodiscard]] constexpr int32_t minutes_to(const Time& other) const noexcept {
        return millisecs_to(other) / MSECS_PER_MIN;
    }

    [[nodiscard]] constexpr int32_t seconds_to(const Time& other) const noexcept {
        return millisecs_to(other) / MSECS_PER_SEC;
    }

    [[nodiscard]] constexpr int32_t millisecs_to(const Time& other) const noexcept {
        return other.duration_ - duration_;
    }

    template <typename StringType = std::string>
    [[nodiscard]] StringType to_string(std::string_view fmt = DEFAULT_FORMAT,
                                       int32_t fraction = DEFAULT_FRACTION) const {
        using Formatter = details::DateTimeFormatter<StringType, std::chrono::milliseconds>;
        return Formatter{
            .hour = hour(), .minute = minute(), .second = second(), .subsecond = subsecond()}
            .format(fmt, fraction);
    }

    //

    static Time from_string(std::string_view str, std::string_view fmt = DEFAULT_FORMAT) {
        auto fields = Parser::parse(str, fmt);
        // 至少有一个时间分量（时、分、秒、子秒）被解析
        if (!fields.hour && !fields.minute && !fields.second && !fields.subsecond) {
            throw std::invalid_argument("no time components provided");
        }
        Parser::check_hh_mm_ss(fields);

        return Time::from_hh_mm_ss(fields.hour.value_or(std::chrono::hours{0}),
                                   fields.minute.value_or(std::chrono::minutes{0}),
                                   fields.second.value_or(std::chrono::seconds{0}),
                                   std::chrono::duration_cast<std::chrono::milliseconds>(
                                       fields.subsecond.value_or(std::chrono::nanoseconds{0})));
    }

    static constexpr Time from_hh_mm_ss(int32_t h, int32_t m, int32_t s = 0, int32_t ms = 0) {
        if (is_valid(h, m, s, ms)) [[likely]] {
            return {h, m, s, ms};
        } else [[unlikely]] {
            throw std::invalid_argument("invalid time");
        }
    }

    static constexpr Time from_hh_mm_ss(std::chrono::hours h,
                                        std::chrono::minutes m,
                                        std::chrono::seconds s = std::chrono::seconds{0},
                                        std::chrono::milliseconds ms = std::chrono::milliseconds{
                                            0}) {
        return from_hh_mm_ss(static_cast<int32_t>(h.count()), static_cast<int32_t>(m.count()),
                             static_cast<int32_t>(s.count()), static_cast<int32_t>(ms.count()));
    }

    template <class Rep, class Period>
    static constexpr Time from_hh_mm_ss(
        const std::chrono::hh_mm_ss<std::chrono::duration<Rep, Period>>& hms) {
        const int32_t ms_since_midnight =
            static_cast<int32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(hms.to_duration()).count());
        if (!is_valid(ms_since_midnight)) [[unlikely]] {
            throw std::invalid_argument("invalid time");
        }
        return Time{ms_since_midnight};
    }

    static constexpr Time from_millisecs_since_midnight(int32_t millisecs) {
        if (is_valid(millisecs)) [[likely]] {
            return Time{millisecs};
        } else [[unlikely]] {
            throw std::invalid_argument("invalid time");
        }
    }

    friend constexpr bool operator==(const Time&, const Time&) = default;
    friend constexpr auto operator<=>(const Time&, const Time&) = default;

    template <details::CoarseTimeDuration DurationType>
    constexpr Time operator+(const DurationType& duration) const noexcept {
        const std::chrono::milliseconds total = std::chrono::milliseconds{duration_} + duration;
        const auto days = std::chrono::floor<std::chrono::days>(total);
        const std::chrono::milliseconds new_dur = total - days;
        return Time{static_cast<int32_t>(new_dur.count())};
    }

    template <details::CoarseTimeDuration DurationType>
    constexpr Time operator-(const DurationType& duration) const noexcept {
        return (*this) + (-duration);
    }

    template <details::CoarseTimeDuration DurationType>
    constexpr Time& operator+=(const DurationType& duration) noexcept {
        *this = *this + duration;
        return *this;
    }

    template <details::CoarseTimeDuration DurationType>
    constexpr Time& operator-=(const DurationType& duration) noexcept {
        *this = *this - duration;
        return *this;
    }

    template <details::CoarseTimeDuration DurationType = std::chrono::milliseconds>
    constexpr DurationType operator-(const Time& rhs) const noexcept {
        return std::chrono::duration_cast<DurationType>(
            std::chrono::milliseconds{duration_ - rhs.duration_});
    }

    friend std::ostream& operator<<(std::ostream& os, const Time& t) { return os << t.to_string(); }

private:
    static constexpr bool is_valid(int32_t h, int32_t m, int32_t s, int32_t ms) noexcept {
        return (
            static_cast<uint32_t>(h) < HOURS_PER_DAY && static_cast<uint32_t>(m) < MINS_PER_HOUR &&
            static_cast<uint32_t>(s) < SECS_PER_MIN && static_cast<uint32_t>(ms) < MSECS_PER_SEC);
    }

    static constexpr bool is_valid(int32_t millisecs_since_midnight) noexcept {
        return millisecs_since_midnight >= 0 && millisecs_since_midnight < MSECS_PER_DAY;
    }

    static constexpr char DEFAULT_FORMAT[] = "%H:%M:%S";
    static constexpr int32_t DEFAULT_FRACTION = -1;

    static constexpr int32_t HOURS_PER_DAY = 24;
    static constexpr int32_t SECS_PER_MIN = std::chrono::minutes::period::num;
    static constexpr int32_t SECS_PER_HOUR = std::chrono::hours::period::num;
    static constexpr int32_t SECS_PER_DAY = std::chrono::days::period::num;

    static constexpr int32_t MINS_PER_HOUR =
        std::ratio_divide<std::chrono::hours::period, std::chrono::minutes::period>::num;

    static constexpr int32_t MSECS_PER_SEC = std::chrono::milliseconds::period::den;
    static constexpr int32_t MSECS_PER_MIN = SECS_PER_MIN * MSECS_PER_SEC;
    static constexpr int32_t MSECS_PER_HOUR = SECS_PER_HOUR * MSECS_PER_SEC;
    static constexpr int32_t MSECS_PER_DAY = SECS_PER_DAY * MSECS_PER_SEC;

    int32_t duration_ = 0;  ///< 距离午夜以来的毫秒数
};
}  // namespace dztrader

#endif  // DZTRADER_DATE_TIME_TIME_H_