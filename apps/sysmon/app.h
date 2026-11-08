#ifndef DZTRADER_SYSMON_APP_H_
#define DZTRADER_SYSMON_APP_H_

/**
 * @file app.h
 * @brief dzsysmon 进程主体入口（`run`）与 NTP 传输工厂接缝。
 *
 * `main.cpp` 只保留 `int main`；本头文件把 `run` 暴露给**测试专用可执行文件**
 * （`tests/clock_e2e_runner.cpp`）注入假 NTP 传输——进程级 e2e 起的是真实进程与真实事件
 * 通道，只有网络部分用替身（注入接缝见 `clock_probe.h` 的 `NtpTransport`；CI 不能依赖
 * 外网 NTP）。契约 sysmon §2.7 定义的是探测的计算语义，不规定测试注入方式。
 */

#include "clock_probe.h"

#include <boost/asio/io_context.hpp>

#include <functional>
#include <memory>

namespace dztrader::sysmon {

/**
 * @brief NTP 传输工厂：`run` 用它构造本轮进程的传输实现。
 *
 * 生产路径传空（`run` 内部使用真实 Asio/UDP 实现 `UdpNtpTransport`）；进程级 e2e 的测试
 * 专用可执行文件注入假传输，使「启动即探 / 时刻表 / 失败态 / 运行期重排」不依赖外网。
 */
using NtpTransportFactory = std::function<std::unique_ptr<NtpTransport>(boost::asio::io_context&)>;

/**
 * @brief 进程入口：配置自愈 → 日志初始化 → 平台基线检查 → 事件循环。
 *
 * @param argc               命令行参数个数
 * @param argv               命令行参数（本进程只接受 `--recover`）
 * @param transport_factory  传输工厂；为空时使用真实 UDP 实现（生产路径）
 * @return 进程退出码（0 = 正常退出；初始化失败为非 0）
 */
int run(int argc, char* argv[], const NtpTransportFactory& transport_factory = {});

}  // namespace dztrader::sysmon

#endif  // DZTRADER_SYSMON_APP_H_
