/**
 * @file clock_e2e_runner.cpp
 * @brief 时钟探测进程级 e2e 的**测试专用可执行文件**（#299）。
 *
 * 与生产 `dzsysmon` 的唯一差别：NTP 传输换成脚本化假实现（`clock_fake_transport.h`）。
 * 其余全部是真实进程主体——配置加载、事件通道、帧编解码、定时器、快照构造与推送。
 * 进程级 e2e（`clock_e2e_test.cpp`）起本可执行文件，因此「真实进程 + 真实事件通道 +
 * 网络替身」三者同时成立，且不依赖外网（契约 sysmon §2.7 的注入接缝）。
 *
 * 先例：`apps/master/tests/worker.cpp`（`test_worker`，同为测试辅助可执行文件）。
 */

#include "app.h"
#include "clock_fake_transport.h"

#include <boost/asio/io_context.hpp>

#include <memory>

int main(int argc, char* argv[]) {
    return dztrader::sysmon::run(
        argc, argv,
        [](boost::asio::io_context& io) -> std::unique_ptr<dztrader::sysmon::NtpTransport> {
            return std::make_unique<dztrader::sysmon::test::FakeNtpTransport>(io);
        });
}
