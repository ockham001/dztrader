#ifndef DZTRADER_CORE_INSTRUMENT_UTIL_H_
#define DZTRADER_CORE_INSTRUMENT_UTIL_H_

#include <cmath>
#include <cstdint>

#include <dztrader/struct.h>

namespace dztrader::instrument {

/// 生效的换算步长 (volume_step <= 0 视为 1, 哨兵处理收在此处)
inline double volume_step_of(const DzInstrumentInfo& c) noexcept {
    return c.volume_step > 0 ? c.volume_step : 1.0;
}

/// 平台单位 -> 原生数量 (仅网关边界调用; 策略核心路径不使用)
inline double to_native_volume(const DzInstrumentInfo& c, int64_t platform_volume) noexcept {
    return static_cast<double>(platform_volume) * volume_step_of(c);
}

/// 原生数量 -> 平台单位, 向下取整到 step 网格 (仅网关边界调用)
/// 1e-9 为浮点商误差容差 (0.0105/0.001 = 10.499...998 类边界)
inline int64_t from_native_volume(const DzInstrumentInfo& c, double native_volume) noexcept {
    const double step = volume_step_of(c);
    return static_cast<int64_t>(std::floor(native_volume / step + 1e-9));
}

}  // namespace dztrader::instrument

#endif  // DZTRADER_CORE_INSTRUMENT_UTIL_H_
