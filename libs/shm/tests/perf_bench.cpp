/**
 * @file perf_bench.cpp
 * @brief SHM 核心路径延迟基准: 写帧 → 唤醒 → 读者读出, 单次往返计时。
 *
 * 对应 README "行情到策略目标延迟 < 30μs" 的核心 IPC 链路 (同进程简化版:
 * 真实跨进程路径含进程调度开销, 本基准给出下限参考, 主要用于建立防退化基线)。
 * 输出 median/p99/max, 不做阈值断言 (CI 共享 runner 计时噪声大, 阈值判断人工进行)。
 */

#include <dztrader/core/this_process.h>
#include <dztrader/data_type.h>
#include <dztrader/shm/channel_meta.h>
#include <dztrader/shm/named_semaphore.h>
#include <dztrader/shm/reader.h>
#include <dztrader/shm/writer.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <vector>

namespace {

using namespace std::chrono;

constexpr int kWarmup = 1000;
constexpr int kIters = 100000;

}  // namespace

int main() {
    // 独立临时通道 (带 pid, 防并行实例冲突), 不干扰 dev/测试
    const auto dir = std::filesystem::temp_directory_path() /
                     ("dz_perf_bench_" + std::to_string(dztrader::this_process::pid()));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);

    const std::string channel = "perfbench_" + std::to_string(dztrader::this_process::pid());
    const std::string reader_name = "perf_reader_" + std::to_string(dztrader::this_process::pid());

    dztrader::shm::ChannelConfig cfg;
    cfg.channel_name = channel;
    cfg.shm_dir = dir;
    cfg.meta_file_size = 1 * 1024 * 1024;
    cfg.page_size = 1 * 1024 * 1024;

    auto meta = std::make_shared<dztrader::shm::ChannelMeta>(
        dztrader::shm::ChannelMeta::open_or_create(cfg));
    // 注册读者 (本进程为 creator): notify_subscribers 据此唤醒 NamedSemaphore
    (void)meta->add_reader(reader_name, 0);

    dztrader::shm::NamedSemaphore sem(reader_name);
    auto writer = dztrader::shm::MultiWriter::create(meta, "perf_writer");
    auto reader = dztrader::shm::Reader::create(meta, reader_name);

    std::byte payload[32] = {};
    constexpr DzFrameType kFrame = DZ_FRAME_RTN_MD_TICK;  // 行情数据帧类型

    // 预热: 页缓存/分支预测/信号量路径
    for (int i = 0; i < kWarmup; ++i) {
        (void)writer.write_ext_frame(kFrame, payload, sizeof(payload));
        writer.notify_subscribers();
        sem.wait();
        (void)reader.next_frame();
    }

    std::vector<int64_t> ns;
    ns.reserve(kIters);
    for (int i = 0; i < kIters; ++i) {
        const auto t0 = steady_clock::now();
        (void)writer.write_ext_frame(kFrame, payload, sizeof(payload));
        writer.notify_subscribers();
        sem.wait();
        (void)reader.next_frame();
        const auto t1 = steady_clock::now();
        ns.push_back(duration_cast<nanoseconds>(t1 - t0).count());
    }

    std::ranges::sort(ns);
    const auto pct = [&](double p) { return ns[static_cast<size_t>(ns.size() * p)]; };
    std::printf("SHM roundtrip (write+notify+wake+read), %d iters:\n", kIters);
    std::printf("  median: %lld ns\n  p99:    %lld ns\n  max:    %lld ns\n",
                static_cast<long long>(pct(0.5)), static_cast<long long>(pct(0.99)),
                static_cast<long long>(ns.back()));
    std::printf("  (README 目标: 行情到策略 < 30μs = 30000 ns; 本基准为同进程下限参考)\n");

    std::filesystem::remove_all(dir, ec);
    return 0;
}
