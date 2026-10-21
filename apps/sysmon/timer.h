#ifndef DZTRADER_SYSMON_TIMER_H_
#define DZTRADER_SYSMON_TIMER_H_

/**
 * @file timer.h
 * @brief 周期定时器漂移校正纯函数（契约 sysmon §4：计量与推送双定时器）。
 *
 * 定时器回调不按"回调时刻 + 间隔"重排（那会因为处理耗时逐周期累积漂移），而是按
 * "上一个计划 deadline + 间隔"推进；进程停顿导致的整段 tick 直接跳过并计数，
 * 使长期频率保持在基线上、同时把停顿暴露为采样缺口（sampling.missed_ticks）。
 */

#include <chrono>
#include <cstdint>

namespace dztrader::sysmon {

/// 漂移校正结果。
struct DriftTick {
    std::chrono::steady_clock::time_point next;  ///< 下一个计划 deadline
    uint64_t missed = 0;                         ///< 本次被跳过的整 tick 数（缺口）
};

/**
 * @brief 按固定间隔推进下一个计划 deadline，并统计被跳过的 tick 数。
 *
 * 语义：
 * - `next = last_deadline + interval`；当 `now` 严格超过 `next` 时，`next` 继续
 *   前进一个间隔并累加 missed（该 deadline 已整段错过）
 * - `now` 恰好等于某个 deadline 时不算错过（该 tick 仍可立即执行，不计 missed）
 * - 正常情形（回调在 deadline 之后、下一个 deadline 之前执行）missed 为 0
 *
 * @param last_deadline 刚刚处理完的 tick 的计划 deadline
 * @param interval      固定间隔（必须 > 0；配置校验保证 [100, 60000] ms）
 * @param now           当前 steady_clock 时刻
 * @return 下一个 deadline 与本次跳过的 tick 数
 */
[[nodiscard]] inline DriftTick advance_deadline(std::chrono::steady_clock::time_point last_deadline,
                                                std::chrono::milliseconds interval,
                                                std::chrono::steady_clock::time_point now) {
    if (interval.count() <= 0) {
        // 防御：非法间隔不推进，避免死循环（配置校验已保证 > 0）
        return DriftTick{last_deadline, 0};
    }
    auto next = last_deadline + interval;
    uint64_t missed = 0;
    while (next < now) {
        next += interval;
        ++missed;
    }
    return DriftTick{next, missed};
}

}  // namespace dztrader::sysmon

#endif  // DZTRADER_SYSMON_TIMER_H_
