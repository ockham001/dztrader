#ifndef DZTRADER_DATE_TIME_DATE_TIME_H_
#define DZTRADER_DATE_TIME_DATE_TIME_H_

#include <chrono>
#include <cstdint>
#include <cerrno>
#include <ctime>
#include <string>
#include <string_view>

#include "./dt_common.h"
#include "./dt_formatter.h"
#include "./dt_parser.h"
#include "./date.h"
#include "./time.h"

namespace dztrader {
class DateTime {
    using Parser = details::DateTimeParser;

public:
    DateTime() = default;

    constexpr explicit DateTime(int64_t nanosecs_since_epoch) noexcept
        : duration_(nanosecs_since_epoch) {}

    template <class Rep, class Period>
    constexpr explicit DateTime(
        const std::chrono::duration<Rep, Period>& duration_since_epoch) noexcept
        : DateTime(
              std::chrono::duration_cast<std::chrono::nanoseconds>(duration_since_epoch).count()) {}

    template <class Rep, class Period>
    constexpr explicit DateTime(
        const std::chrono::year_month_day& ymd,
        const std::chrono::hh_mm_ss<std::chrono::duration<Rep, Period>>& hms) noexcept
        : DateTime(std::chrono::sys_days(ymd).time_since_epoch() + hms.to_duration()) {}

    constexpr explicit DateTime(Date date, Time time) noexcept
        : DateTime(date.time_since_epoch() + time.time_since_midnight()) {}

    [[nodiscard]] constexpr Date date() const noexcept { return Date{days_since_epoch()}; }

    [[nodiscard]] constexpr Time time() const noexcept {
        return Time{
            static_cast<int32_t>(duration_since_midnight<std::chrono::milliseconds>().count())};
    }

    [[nodiscard]] constexpr int32_t year() const noexcept {
        return static_cast<int32_t>(year_month_day().year());
    }

    [[nodiscard]] constexpr int32_t month() const noexcept {
        return static_cast<int32_t>(static_cast<uint32_t>(year_month_day().month()));
    }

    [[nodiscard]] constexpr int32_t day() const noexcept {
        return static_cast<int32_t>(static_cast<uint32_t>(year_month_day().day()));
    }

    [[nodiscard]] constexpr Weekday weekday() const noexcept {
        return static_cast<Weekday>(
            static_cast<uint32_t>(year_month_weekday().weekday().iso_encoding()));
    }

    [[nodiscard]] constexpr int32_t hour() const noexcept {
        auto ns = duration_since_midnight<std::chrono::nanoseconds>().count();
        return static_cast<int32_t>(ns / NANOS_PER_HOUR);
    }

    [[nodiscard]] constexpr int32_t minute() const noexcept {
        auto ns = duration_since_midnight<std::chrono::nanoseconds>().count();
        return static_cast<int32_t>((ns / NANOS_PER_MINUTE) % 60);
    }

    [[nodiscard]] constexpr int32_t second() const noexcept {
        auto ns = duration_since_midnight<std::chrono::nanoseconds>().count();
        return static_cast<int32_t>((ns / NANOS_PER_SECOND) % 60);
    }

    [[nodiscard]] constexpr int32_t millisec() const noexcept {
        auto ns = duration_since_midnight<std::chrono::nanoseconds>().count();
        return static_cast<int32_t>((ns / NANOS_PER_MILLISECOND) % 1000);
    }

    [[nodiscard]] constexpr int32_t microsec() const noexcept {
        auto ns = duration_since_midnight<std::chrono::nanoseconds>().count();
        return static_cast<int32_t>((ns / NANOS_PER_MICROSECOND) % 1000);
    }

    [[nodiscard]] constexpr int32_t nanosec() const noexcept {
        auto ns = duration_since_midnight<std::chrono::nanoseconds>().count();
        return static_cast<int32_t>(ns % 1000LL);
    }

    [[nodiscard]] constexpr int64_t nanosecs_since_midnight() const noexcept {
        return duration_since_midnight<std::chrono::nanoseconds>().count();
    }

    [[nodiscard]] constexpr int64_t microsecs_since_midnight() const noexcept {
        return duration_since_midnight<std::chrono::microseconds>().count();
    }

    [[nodiscard]] constexpr int32_t millisecs_since_midnight() const noexcept {
        return static_cast<int32_t>(duration_since_midnight<std::chrono::milliseconds>().count());
    }

    [[nodiscard]] constexpr int32_t seconds_since_midnight() const noexcept {
        return static_cast<int32_t>(duration_since_midnight<std::chrono::seconds>().count());
    }

    [[nodiscard]] constexpr int32_t minutes_since_midnight() const noexcept {
        return duration_since_midnight<std::chrono::minutes>().count();
    }

    [[nodiscard]] constexpr int32_t hours_since_midnight() const noexcept {
        return duration_since_midnight<std::chrono::hours>().count();
    }

    [[nodiscard]] constexpr int64_t nanosecs_since_epoch() const noexcept { return duration_; }

    [[nodiscard]] constexpr int64_t microsecs_since_epoch() const noexcept {
        return std::chrono::floor<std::chrono::microseconds>(std::chrono::nanoseconds{duration_})
            .count();
    }

    [[nodiscard]] constexpr int64_t millisecs_since_epoch() const noexcept {
        return std::chrono::floor<std::chrono::milliseconds>(std::chrono::nanoseconds{duration_})
            .count();
    }

    [[nodiscard]] constexpr int64_t seconds_since_epoch() const noexcept {
        return std::chrono::floor<std::chrono::seconds>(std::chrono::nanoseconds{duration_})
            .count();
    }

    [[nodiscard]] constexpr int64_t minutes_since_epoch() const noexcept {
        return std::chrono::floor<std::chrono::minutes>(std::chrono::nanoseconds{duration_})
            .count();
    }

    [[nodiscard]] constexpr int64_t hours_since_epoch() const noexcept {
        return std::chrono::floor<std::chrono::hours>(std::chrono::nanoseconds{duration_}).count();
    }

    [[nodiscard]] constexpr int32_t days_since_epoch() const noexcept {
        return std::chrono::floor<std::chrono::days>(std::chrono::nanoseconds{duration_}).count();
    }

    //

    [[nodiscard]] constexpr std::chrono::year_month_day year_month_day() const noexcept {
        return std::chrono::year_month_day{sys_days()};
    }

    [[nodiscard]] constexpr std::chrono::year_month_weekday year_month_weekday() const noexcept {
        return std::chrono::year_month_weekday{sys_days()};
    }

    template <details::PreciseTimeDuration DurationType = std::chrono::nanoseconds>
    [[nodiscard]] constexpr std::chrono::hh_mm_ss<DurationType> hh_mm_ss() const noexcept {
        return std::chrono::hh_mm_ss<DurationType>{duration_since_midnight<DurationType>()};
    }

    template <details::SubsecondDuration DurationType = std::chrono::nanoseconds>
    [[nodiscard]] DurationType subseconds() const noexcept {
        auto total_ns = std::chrono::nanoseconds{duration_};
        auto seconds_part = std::chrono::floor<std::chrono::seconds>(total_ns);
        auto subseconds_ns = total_ns - seconds_part;  // 始终非负
        return std::chrono::duration_cast<DurationType>(subseconds_ns);
    }

    //
    template <details::TimeStamp T = std::time_t>
    [[nodiscard]] constexpr T timestamp() const noexcept {
        if constexpr (std::is_floating_point_v<T>) {
            return static_cast<T>(duration_) * 1e-9;
        } else {
            return static_cast<T>(seconds_since_epoch());
        }
    }

    template <details::PreciseTimeDuration DurationType = std::chrono::nanoseconds>
    [[nodiscard]] constexpr DurationType duration_since_midnight() const noexcept {
        auto tp = std::chrono::sys_time{std::chrono::nanoseconds{duration_}};
        auto dp = std::chrono::floor<std::chrono::days>(tp);
        return std::chrono::duration_cast<DurationType>(tp - dp);
    }

    //

    [[nodiscard]] constexpr DateTime add_nanosecs(int32_t ns) const noexcept {
        return (*this) + std::chrono::nanoseconds{ns};
    }

    [[nodiscard]] constexpr DateTime add_microsecs(int32_t us) const noexcept {
        return (*this) + std::chrono::microseconds{us};
    }

    [[nodiscard]] constexpr DateTime add_millisecs(int32_t ms) const noexcept {
        return (*this) + std::chrono::milliseconds{ms};
    }

    [[nodiscard]] constexpr DateTime add_seconds(int32_t s) const noexcept {
        return (*this) + std::chrono::seconds{s};
    }

    [[nodiscard]] constexpr DateTime add_minutes(int32_t m) const noexcept {
        return (*this) + std::chrono::minutes{m};
    }

    [[nodiscard]] constexpr DateTime add_hours(int32_t h) const noexcept {
        return (*this) + std::chrono::hours{h};
    }

    [[nodiscard]] constexpr DateTime add_days(int32_t d) const noexcept {
        return (*this) + std::chrono::days{d};
    }

    [[nodiscard]] constexpr DateTime add_months(int32_t m) const noexcept {
        return (*this) + std::chrono::months{m};
    }

    [[nodiscard]] constexpr DateTime add_years(int32_t y) const noexcept {
        return (*this) + std::chrono::years{y};
    }

    [[nodiscard]] constexpr int32_t days_to(const DateTime& d) const noexcept {
        return (d.sys_days() - sys_days()).count();
    }

    //

    template <typename StringType = std::string>
    [[nodiscard]] StringType to_string(std::string_view fmt = DEFAULT_FORMAT,
                                       int32_t fraction = DEFAULT_FRACTION) const {
        const auto days = sys_days();
        const auto ymd = std::chrono::year_month_day{days};
        const auto ymwd = std::chrono::year_month_weekday{days};
        const auto hms = hh_mm_ss<std::chrono::nanoseconds>();

        return details::DateTimeFormatter<StringType, std::chrono::nanoseconds>{
            .year = static_cast<int32_t>(ymd.year()),
            .month = static_cast<int32_t>(static_cast<uint32_t>(ymd.month())),
            .weekday = static_cast<int32_t>(ymwd.weekday().iso_encoding()),
            .day = static_cast<int32_t>(static_cast<uint32_t>(ymd.day())),
            .hour = static_cast<int32_t>(hms.hours().count()),
            .minute = static_cast<int32_t>(hms.minutes().count()),
            .second = static_cast<int32_t>(hms.seconds().count()),
            .subsecond = hms.subseconds()}
            .format(fmt, fraction);
    }

    //

    static DateTime from_string(std::string_view str, std::string_view fmt = DEFAULT_FORMAT) {
        auto fields = Parser::parse(str, fmt);
        if (!fields.year || !fields.month || !fields.day) {
            throw std::invalid_argument(std::format(
                "missing date components | year={} month={} day={}",
                fields.year.has_value(), fields.month.has_value(), fields.day.has_value()));
        }

        // 构造日期部分
        auto ymd = std::chrono::year_month_day{*fields.year, *fields.month, *fields.day};
        if (!ymd.ok()) {
            throw std::invalid_argument("invalid date components");
        }

        // 构造时间部分（默认为午夜）
        auto total_ns = std::chrono::nanoseconds::zero();
        if (fields.hour || fields.minute || fields.second || fields.subsecond) {
            Parser::check_hh_mm_ss(fields);

            total_ns = fields.hour.value_or(std::chrono::hours{0}) +
                       fields.minute.value_or(std::chrono::minutes{0}) +
                       fields.second.value_or(std::chrono::seconds{0}) +
                       fields.subsecond.value_or(std::chrono::nanoseconds{0});
        }
        auto hms = std::chrono::hh_mm_ss<std::chrono::nanoseconds>{total_ns};

        return DateTime{ymd, hms};
    }

    template <class Rep, class Period>
    static constexpr DateTime from_ymd_hms(
        const std::chrono::year_month_day& ymd,
        const std::chrono::hh_mm_ss<std::chrono::duration<Rep, Period>>& hms) {
        if (!ymd.ok()) [[unlikely]] {
            throw std::invalid_argument("invalid year, month, or day");
        }
        const auto duration_since_midnight = hms.to_duration();
        if (duration_since_midnight < std::chrono::nanoseconds{0} ||
            duration_since_midnight >= std::chrono::hours{24}) [[unlikely]] {
            throw std::invalid_argument("invalid hour, minute, or second");
        }
        return DateTime{ymd, hms};
    }

    static constexpr DateTime from_ymd_hms(int32_t y,
                                           int32_t m,
                                           int32_t d,
                                           int32_t h = 0,
                                           int32_t minute = 0,
                                           int32_t s = 0,
                                           int32_t ms = 0,
                                           int32_t us = 0,
                                           int32_t ns = 0) {
        if (h < 0 || h >= 24 || minute < 0 || minute >= 60 || s < 0 || s >= 60 || ms < 0 ||
            ms >= 1000 || us < 0 || us >= 1000 || ns < 0 || ns >= 1000) [[unlikely]] {
            throw std::invalid_argument(std::format(
                "invalid time component | h={} min={} s={} ms={} us={} ns={}",
                h, minute, s, ms, us, ns));
        }
        return DateTime::from_ymd_hms(
            std::chrono::year_month_day{std::chrono::year{y},
                                        std::chrono::month{static_cast<uint32_t>(m)},
                                        std::chrono::day{static_cast<uint32_t>(d)}},
            std::chrono::hh_mm_ss<std::chrono::nanoseconds>{
                std::chrono::hours{h} + std::chrono::minutes{minute} + std::chrono::seconds{s} +
                std::chrono::milliseconds{ms} + std::chrono::microseconds{us} +
                std::chrono::nanoseconds{ns}});
    }

    template <details::TimeStamp T = std::time_t>
    static constexpr DateTime from_timestamp(T timestamp) noexcept {
        if constexpr (std::is_floating_point_v<T>) {
            return DateTime(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::duration<double>(timestamp))
                                .count());
        } else {
            return DateTime(std::chrono::seconds{timestamp});
        }
    }

    static DateTime system_now() noexcept(noexcept(DateTime{
        std::chrono::system_clock::now().time_since_epoch()})) {
        return DateTime{std::chrono::system_clock::now().time_since_epoch()};
    }

    static DateTime local_now() { return system_to_local(DateTime::system_now()); }

    static DateTime system_to_local(const DateTime& system_time) {
        std::tm local_tm;  // NOLINT
        if (details::local_time(local_tm, system_time.timestamp<int64_t>())) [[likely]] {
            const auto local_days = std::chrono::local_days{std::chrono::year_month_day{
                std::chrono::year{local_tm.tm_year + 1900},
                std::chrono::month{static_cast<uint32_t>(local_tm.tm_mon) + 1},
                std::chrono::day{static_cast<uint32_t>(local_tm.tm_mday)}}};

            const std::chrono::nanoseconds local_nanosecs_since_epoch =
                local_days.time_since_epoch() + std::chrono::hours{local_tm.tm_hour} +
                std::chrono::minutes{local_tm.tm_min} + std::chrono::seconds{local_tm.tm_sec} +
                system_time.subseconds<std::chrono::nanoseconds>();

            return DateTime{local_nanosecs_since_epoch.count()};
        } else [[unlikely]] {
            // 用chrono库
            auto local_time = std::chrono::zoned_time(
                                  std::chrono::current_zone(),
                                  std::chrono::sys_time<std::chrono::nanoseconds>{
                                      std::chrono::nanoseconds{system_time.nanosecs_since_epoch()}})
                                  .get_local_time();
            return DateTime{local_time.time_since_epoch()};
        }
    }

    static DateTime local_to_system(const DateTime& local_time) {
        const auto local_ymd = local_time.year_month_day();
        const auto local_hms = local_time.hh_mm_ss<std::chrono::nanoseconds>();
        std::tm local_tm{};
        local_tm.tm_sec  = static_cast<int32_t>(local_hms.seconds().count());
        local_tm.tm_min  = static_cast<int32_t>(local_hms.minutes().count());
        local_tm.tm_hour = static_cast<int32_t>(local_hms.hours().count());
        local_tm.tm_mday = static_cast<int32_t>(static_cast<uint32_t>(local_ymd.day()));
        local_tm.tm_mon  = static_cast<int32_t>(static_cast<uint32_t>(local_ymd.month())) - 1;
        local_tm.tm_year = static_cast<int32_t>(local_ymd.year()) - 1900;
        local_tm.tm_isdst = -1;

        errno = 0;
        time_t result = ::mktime(&local_tm);

        // C 标准规定 mktime 失败返回 (time_t)-1；errno 是 POSIX 扩展，不保证一定设置。
        // 两者都检查：result != -1 优先（覆盖标准失败语义），errno == 0 作辅助诊断。
        if (result != static_cast<time_t>(-1) && errno == 0) [[likely]] {
            return DateTime{std::chrono::seconds{result} + local_hms.subseconds()};
        } else [[unlikely]] {
            auto system_time = std::chrono::zoned_time(
                std::chrono::current_zone(), std::chrono::local_time(std::chrono::nanoseconds{
                                                 local_time.nanosecs_since_epoch()}));
            return DateTime{std::chrono::duration_cast<std::chrono::nanoseconds>(
                system_time.get_sys_time().time_since_epoch())};
        }
    }

    static constexpr DateTime action_day_to_trading_day(const DateTime& action_dt) {
        return action_dt.add_days(
            action_day_to_trading_day_impl(action_dt.weekday(), action_dt.hour()));
    }

    static constexpr Date action_day_to_trading_day(const Date& action_date,
                                                    const Time& action_time) {
        return action_date.add_days(
            action_day_to_trading_day_impl(action_date.weekday(), action_time.hour()));
    }

    static constexpr DateTime trading_day_to_action_day(const DateTime& trading_dt) {
        return trading_dt.add_days(
            trading_day_to_action_day_impl(trading_dt.weekday(), trading_dt.hour()));
    }

    static constexpr Date trading_day_to_action_day(const Date& trading_date,
                                                    const Time& action_time) {
        return trading_date.add_days(
            trading_day_to_action_day_impl(trading_date.weekday(), action_time.hour()));
    }

    //

    friend constexpr bool operator==(const DateTime&, const DateTime&) = default;
    friend constexpr auto operator<=>(const DateTime&, const DateTime&) = default;

    template <typename DurationType>
        requires details::PreciseTimeDuration<DurationType> ||
                 std::is_same_v<DurationType, std::chrono::days> ||
                 std::is_same_v<DurationType, std::chrono::weeks>
    constexpr DateTime operator+(const DurationType& duration) const noexcept {
        return DateTime{duration_ +
                        std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count()};
    }

    template <typename DurationType>
        requires std::is_same_v<DurationType, std::chrono::months> ||
                 std::is_same_v<DurationType, std::chrono::years>
    constexpr DateTime operator+(const DurationType& duration) const noexcept {
        auto dt = date();
        dt += duration;
        return DateTime{
            (dt.time_since_epoch() + duration_since_midnight<std::chrono::nanoseconds>())};
    }

    template <typename DurationType>
        requires details::PreciseTimeDuration<DurationType> ||
                 std::is_same_v<DurationType, std::chrono::days> ||
                 std::is_same_v<DurationType, std::chrono::weeks>
    constexpr DateTime operator-(const DurationType& duration) const noexcept {
        return *this + (-duration);
    }

    template <typename DurationType>
        requires std::is_same_v<DurationType, std::chrono::months> ||
                 std::is_same_v<DurationType, std::chrono::years>
    constexpr DateTime operator-(const DurationType& duration) const noexcept {
        return *this + (-duration);
    }

    template <class Rep, class Period>
    constexpr DateTime& operator+=(const std::chrono::duration<Rep, Period>& duration) noexcept {
        *this = *this + duration;
        return *this;
    }

    template <class Rep, class Period>
    constexpr DateTime& operator-=(const std::chrono::duration<Rep, Period>& duration) noexcept {
        *this = *this - duration;
        return *this;
    }

    template <typename DurationType = std::chrono::nanoseconds>
    constexpr DurationType operator-(const DateTime& rhs) const noexcept {
        return std::chrono::duration_cast<DurationType>(
            std::chrono::nanoseconds{duration_ - rhs.duration_});
    }

    friend std::ostream& operator<<(std::ostream& os, const DateTime& dt) {
        return os << dt.to_string();
    }

private:
    // ---------- 私有辅助方法 ----------

    [[nodiscard]] constexpr std::chrono::sys_time<std::chrono::nanoseconds> sys_time()
        const noexcept {
        return std::chrono::sys_time{std::chrono::nanoseconds{duration_}};
    }

    [[nodiscard]] constexpr std::chrono::sys_days sys_days() const noexcept {
        return std::chrono::floor<std::chrono::days>(sys_time());
    }

    // 期货夜盘，业务日转真实交易日
    static constexpr int32_t action_day_to_trading_day_impl(Weekday weekday, int32_t hour) {
        // 周一 0 -> 18 属于 action == trading
        // 周一 18->24 属于 action+1
        //
        // 周二 0->18 属于action == trading
        // 周二 18->24 属于 action+1
        //
        // 周三 0->18 属于action == trading
        // 周三 18->24 属于 action+1
        //
        // 周四 0->18 属于action == trading
        // 周四 18->24 属于 action+1
        //
        // 周五 0->18 属于action == trading
        // 周五 18->24 属于 action+3
        //
        // 周六 0->24 属于action+2
        //
        // 周日 0->24 属于action+1
        switch (weekday) {
            case Weekday::Monday:
            case Weekday::Tuesday:
            case Weekday::Wednesday:
            case Weekday::Thursday:
                return hour >= 18 ? 1 : 0;
            case Weekday::Friday:
                return hour >= 18 ? 3 : 0;
            case Weekday::Saturday:
                return 2;
            case Weekday::Sunday:
                return 1;
            default:
                throw std::invalid_argument(
                    std::format("invalid weekday | weekday={}", static_cast<int32_t>(weekday)));
        }
    }

    static constexpr int32_t trading_day_to_action_day_impl(Weekday weekday, int32_t hour) {
        // 交易日是周一， 时间0-6 属于 上周周六 action - 2
        // 交易日是周一， 时间6-18 属于 action == trading
        // 交易日是周一， 时间18-24 属于 上周五夜盘 action-3

        // 交易日是周二， 时间0-18 属于 action == trading
        // 交易日是周二， 时间18-24 属于 周一夜盘 action-1

        // 交易日是周三， 时间0-18 属于 action == trading
        // 交易日是周三， 时间18-24 属于 周二夜盘 action-1

        // 交易日是周四， 时间0-18 属于 action == trading
        // 交易日是周四， 时间18-24 属于 周三夜盘 action-1

        // 交易日是周五， 时间0-18 属于 action == trading
        // 交易日是周五， 时间18-24 属于 周四夜盘 action-1

        switch (weekday) {
            case Weekday::Monday:
                if (hour < 6) {
                    return -2;
                } else if (hour < 18) {
                    return 0;
                } else {
                    return -3;
                }
            case Weekday::Tuesday:
            case Weekday::Wednesday:
            case Weekday::Thursday:
            case Weekday::Friday:
                return hour >= 18 ? -1 : 0;
            default:
                throw std::invalid_argument(
                    std::format("trading day cannot be weekend | weekday={}",
                                static_cast<int32_t>(weekday)));
        }
    }

    static constexpr char DEFAULT_FORMAT[] = "%Y-%m-%d %H:%M:%S";
    static constexpr int32_t DEFAULT_FRACTION = -1;

    static constexpr int64_t NANOS_PER_HOUR = 3'600'000'000'000LL;  // 3600 * 1e9
    static constexpr int64_t NANOS_PER_MINUTE = 60'000'000'000LL;   // 60 * 1e9
    static constexpr int64_t NANOS_PER_SECOND = 1'000'000'000LL;    // 1e9
    static constexpr int64_t NANOS_PER_MILLISECOND = 1'000'000LL;   // 1e6
    static constexpr int64_t NANOS_PER_MICROSECOND = 1'000LL;       // 1e3

    int64_t duration_ = 0;  ///< 自纪元以来的纳秒数
};
}  // namespace dztrader

#endif  // DZTRADER_DATE_TIME_DATE_TIME_H_