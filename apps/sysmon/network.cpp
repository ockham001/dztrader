#include "network.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "cpu_usage.h"

namespace dztrader::sysmon {
namespace {

/// 两平台网络计数器均为 64 位（Linux `/proc/net/dev`；Windows `MIB_IF_ROW2` 的 ULONG64），
/// 因此回绕跨度为 0：下降一律视为计数器重置（契约 sysmon §2.7）。
constexpr uint64_t kNetworkWrapSpan = 0;

/// 在上一帧中查找同一网卡（网卡集合可在帧间变化，按接口名匹配而不是数组下标）。
const platform::RawNetworkInfo* find_interface(const std::vector<platform::RawNetworkInfo>& prev,
                                               const std::string& interface_name) noexcept {
    for (const auto& entry : prev) {
        if (entry.interface_name == interface_name) {
            return &entry;
        }
    }
    return nullptr;
}

/// 由一个累计计数器算速率；无基线时返回 std::nullopt（不得用 0 冒充）。
std::optional<double> rate_of(uint64_t prev, uint64_t cur, double elapsed_seconds) noexcept {
    return compute_rate(prev, cur, elapsed_seconds, kNetworkWrapSpan);
}

}  // namespace

std::vector<platform::SysmonNetworkInfo> compute_network_rates(
    const std::vector<platform::RawNetworkInfo>& prev,
    const std::vector<platform::RawNetworkInfo>& cur,
    double elapsed_seconds) noexcept {
    std::vector<platform::SysmonNetworkInfo> results;
    results.reserve(cur.size());
    // 采样窗口非法（定时器异常/配置重排导致非正间隔）时速率整帧不可用。
    const bool window_valid = std::isfinite(elapsed_seconds) && elapsed_seconds > 0.0;
    for (const auto& current : cur) {
        platform::SysmonNetworkInfo info;
        info.interface_name = current.interface_name;
        // 八个累计计数器按本帧原样填：累计值没有"不可用"语义（契约 sysmon §7）。
        info.rx_bytes_total = current.rx_bytes;
        info.tx_bytes_total = current.tx_bytes;
        info.rx_packets_total = current.rx_packets;
        info.tx_packets_total = current.tx_packets;
        info.rx_errors_total = current.rx_errors;
        info.tx_errors_total = current.tx_errors;
        info.rx_drops_total = current.rx_drops;
        info.tx_drops_total = current.tx_drops;
        info.is_virtual = current.is_virtual;

        // 基线按接口名匹配：本帧新出现的网卡等同首次采样，速率全部 null。
        const platform::RawNetworkInfo* baseline =
            window_valid ? find_interface(prev, current.interface_name) : nullptr;
        if (baseline != nullptr) {
            info.rx_bytes_per_sec = rate_of(baseline->rx_bytes, current.rx_bytes, elapsed_seconds);
            info.tx_bytes_per_sec = rate_of(baseline->tx_bytes, current.tx_bytes, elapsed_seconds);
            info.rx_errors_per_sec =
                rate_of(baseline->rx_errors, current.rx_errors, elapsed_seconds);
            info.tx_errors_per_sec =
                rate_of(baseline->tx_errors, current.tx_errors, elapsed_seconds);
            info.rx_drops_per_sec = rate_of(baseline->rx_drops, current.rx_drops, elapsed_seconds);
            info.tx_drops_per_sec = rate_of(baseline->tx_drops, current.tx_drops, elapsed_seconds);
        }
        results.push_back(std::move(info));
    }
    return results;
}

}  // namespace dztrader::sysmon
