#ifndef DZTRADER_SHM_COMMON_H_
#define DZTRADER_SHM_COMMON_H_

#include <cstdint>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>

namespace dztrader::shm {

struct ChannelConfig {
    std::string channel_name;
    std::filesystem::path shm_dir;
    uint64_t meta_file_size;
    uint64_t page_size;
    uint64_t max_bytes_per_minute = 0;
    uint64_t max_bytes_per_session = 0;
    bool lock_memory = false;
    bool prefetch_memory = false;
};

/// 通道名 (event="dzevent", md=行情源名如"dzmd_ctp", 不会冲突)
inline std::string channel_name(std::string_view name) {
    return std::format("{}", name);
}

}  // namespace dztrader::shm

#endif  // DZTRADER_SHM_COMMON_H_
