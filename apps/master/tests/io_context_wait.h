#ifndef DZTRADER_MASTER_TESTS_IO_CONTEXT_WAIT_H_
#define DZTRADER_MASTER_TESTS_IO_CONTEXT_WAIT_H_

/**
 * @file io_context_wait.h
 * @brief master 进程级测试的「条件等待」工具：驱动 io_context 直到判据成立或超时。
 *
 * 为什么需要：这些用例等待的是**外部事件**（子进程收到信号后退出 / 被 reap / async_wait
 * 回调触发）。「事件在 N 秒墙钟内发生」不是被测语义——固定 `run_for(N)` 在高负载机器上
 * 会把调度延迟误判成「事件没发生」（#335：门禁 flaky，本机 load 20+ 时必现）。
 * 本工具把超时降级为**上限**：命中判据即早退，正常路径不额外消耗墙钟。
 */

#include <boost/asio/io_context.hpp>

#include <chrono>

namespace dztrader::test {

/// 以 20ms 为片驱动 @p ioc 直到 pred() 成立或超时；返回最终判据，供 ASSERT/EXPECT 使用。
///
/// 每片前 restart()：io_context 一旦因无 handler 而停止，后续 run_for 会立即返回。
template <typename Pred>
bool run_io_until(boost::asio::io_context& ioc,
                  Pred pred,
                  std::chrono::milliseconds timeout = std::chrono::seconds(10)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!pred() && std::chrono::steady_clock::now() < deadline) {
        ioc.restart();
        ioc.run_for(std::chrono::milliseconds(20));
    }
    return pred();
}

}  // namespace dztrader::test

#endif  // DZTRADER_MASTER_TESTS_IO_CONTEXT_WAIT_H_
