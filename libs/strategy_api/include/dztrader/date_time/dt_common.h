#ifndef DZTRADER_DATE_TIME_DT_COMMON_H_
#define DZTRADER_DATE_TIME_DT_COMMON_H_
#include <chrono>
#include <cstdint>
#include <ctime>

namespace dztrader {

enum Weekday : int32_t {
    Monday = 1,
    Tuesday = 2,
    Wednesday = 3,
    Thursday = 4,
    Friday = 5,
    Saturday = 6,
    Sunday = 7,
};

namespace details {

inline bool local_time(std::tm& result, std::time_t timestamp) noexcept {
#ifdef _WIN32
    // time_tt 小于 0 或大于 _MAX__TIME64_T 会返回错误
    return ::localtime_s(&result, &timestamp) == 0;
#else
    return ::localtime_r(&timestamp, &result) != nullptr;
#endif
}

template <typename T>
concept TimeStamp = std::is_same_v<T, int64_t> || std::is_same_v<T, int32_t> ||
                    std::is_same_v<T, std::time_t> || std::is_same_v<T, double>;

template <typename T>
concept ChronoDuration = requires {
    typename T::rep;
    typename T::period;
    requires std::derived_from<T, std::chrono::duration<typename T::rep, typename T::period>>;
};

// 亚秒概念：周期小于 1 秒（即 period::num / period::den < 1）
template <typename T>
concept SubsecondDuration =
    ChronoDuration<T> && (std::ratio_less_v<typename T::period, std::ratio<1>>);

template <typename T>
concept CoarseTimeDuration =
    std::is_same_v<T, std::chrono::milliseconds> || std::is_same_v<T, std::chrono::seconds> ||
    std::is_same_v<T, std::chrono::minutes> || std::is_same_v<T, std::chrono::hours>;

template <typename T>
concept PreciseTimeDuration =
    std::is_same_v<T, std::chrono::nanoseconds> || std::is_same_v<T, std::chrono::microseconds> ||
    std::is_same_v<T, std::chrono::milliseconds> || std::is_same_v<T, std::chrono::seconds> ||
    std::is_same_v<T, std::chrono::minutes> || std::is_same_v<T, std::chrono::hours>;

}  // namespace details

}  // namespace dztrader

#endif  // DZTRADER_DATE_TIME_DT_COMMON_H_
