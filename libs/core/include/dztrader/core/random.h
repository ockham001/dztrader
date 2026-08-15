#ifndef DZTRADER_CORE_RANDOM_H_
#define DZTRADER_CORE_RANDOM_H_

#include <chrono>
#include <random>

namespace dztrader::core {

/// 返回 [min_ms, max_ms] 范围内的随机毫秒数。
/// 内部使用 thread_local mt19937_64, 首次调用用 std::random_device 播种。
/// 多进程安全: random_device 基于物理熵源 (Windows: BCryptGenRandom,
/// Linux: /dev/urandom), 即使多进程同时启动也不会产生相同序列。
/// @param min_ms 最小毫秒数 (含)
/// @param max_ms 最大毫秒数 (含)
/// @return [min_ms, max_ms] 范围内的随机持续时间
inline std::chrono::milliseconds random_jitter(int min_ms, int max_ms) {
    thread_local std::mt19937_64 gen{std::random_device{}()};
    std::uniform_int_distribution<int> dist(min_ms, max_ms);
    return std::chrono::milliseconds{dist(gen)};
}

}  // namespace dztrader::core

#endif  // DZTRADER_CORE_RANDOM_H_
