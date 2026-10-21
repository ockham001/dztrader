// 06-sysmon 采集层真机 oracle 测试：断言两平台实现从真实 OS 系统调用拿到的原始数据满足契约不变量。
#include <dztrader/platform/system_collector.h>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <thread>

namespace dztrader::test {

using namespace dztrader::platform;

TEST(SystemCollectorTest, SystemInfoMatchesRealMachine) {
    const auto collector = make_system_collector();
    ASSERT_NE(collector, nullptr);

    const RawSystemInfo info = collector->collect_system_info();

    // 两平台共有不变量（契约 sysmon §2.2）：主机名、OS 版本、内核版本非空，核数 > 0。
    EXPECT_FALSE(info.hostname.empty());
    EXPECT_FALSE(info.os_version.empty());
    EXPECT_FALSE(info.kernel_version.empty());
    EXPECT_GT(info.logical_cores, 0u);

    // 平台分支各自断言等价的编译平台事实。
#if defined(_WIN32)
    EXPECT_EQ(info.os_family, "windows");
    EXPECT_GT(info.os_build, 0u);  // Windows 必须报 build 号（区分 Windows 10/11）
#elif defined(__linux__)
    EXPECT_EQ(info.os_family, "linux");
    EXPECT_EQ(info.os_build, 0u);  // Linux 的 build 恒为 0（契约 sysmon §2.2）
#endif

    // boot_time_unix_sec 必须为正且不晚于当前墙钟。
    const int64_t now_unix_sec = std::chrono::duration_cast<std::chrono::seconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count();
    EXPECT_GT(info.boot_time_unix_sec, 0);
    EXPECT_LE(info.boot_time_unix_sec, now_unix_sec);
}

TEST(SystemCollectorTest, CpuTimesAreMonotonicCounters) {
    const auto collector = make_system_collector();
    ASSERT_NE(collector, nullptr);

    const auto first = collector->collect_cpu_times();
    ASSERT_TRUE(first.has_value()) << "collect_cpu_times must work on a supported host";
    EXPECT_LE(first->idle_ticks, first->total_ticks);

    // 累计计数器按 OS 固定粒度推进：条件轮询直到观察到增量（最多约 1s），
    // 不以固定 sleep 作为同步手段（docs/unit-test-standard.md §1.5）。
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    std::optional<RawCpuTimes> second;
    while (std::chrono::steady_clock::now() < deadline) {
        second = collector->collect_cpu_times();
        if (second.has_value() && second->total_ticks > first->total_ticks) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    ASSERT_TRUE(second.has_value());
    EXPECT_GT(second->total_ticks, first->total_ticks);
    EXPECT_GE(second->idle_ticks, first->idle_ticks);
    EXPECT_LE(second->idle_ticks, second->total_ticks);
}

}  // namespace dztrader::test
