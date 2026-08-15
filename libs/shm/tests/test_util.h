#ifndef DZTRADER_SHM_TEST_UTIL_H_
#define DZTRADER_SHM_TEST_UTIL_H_

#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace dztrader::shm::test {

constexpr uint64_t KB = 1024ULL;
constexpr uint64_t MB = KB * 1024ULL;

inline std::string unique_channel_name(const std::string& prefix)
{
    auto ts = std::chrono::steady_clock::now().time_since_epoch().count();
#ifdef _WIN32
    auto pid = GetCurrentProcessId();
#else
    auto pid = getpid();
#endif
    return prefix + "_" + std::to_string(ts) + "_" + std::to_string(pid);
}

inline std::filesystem::path test_shm_dir(const std::string& channel_name)
{
    return std::filesystem::temp_directory_path() / "dz_shm_test" / channel_name;
}

inline void create_page_file(const std::filesystem::path& dir, uint64_t page_id, uint64_t size)
{
    auto path = dir / std::format("{:08d}.dat", page_id);
    std::ofstream ofs(path, std::ios::binary);
    ofs.seekp(size - 1);
    ofs.put('\0');
}

inline void cleanup_test_dir(const std::filesystem::path& shm_dir)
{
    std::error_code ec;
    std::filesystem::remove_all(shm_dir, ec);
}

}  // namespace dztrader::shm::test

#endif  // DZTRADER_SHM_TEST_UTIL_H_
