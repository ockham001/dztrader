#ifndef DZTRADER_SHM_TEST_UTIL_H_
#define DZTRADER_SHM_TEST_UTIL_H_

#include "test_temp_dir.h"

#include <chrono>
#include <dztrader/shm/named_semaphore.h>
#include <dztrader/shm/process_mutex.h>
#include <filesystem>
#include <format>
#include <fstream>
#include <initializer_list>
#include <string>
#include <string_view>

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
constexpr uint64_t GB = MB * 1024ULL;

inline std::string unique_channel_name(const std::string& prefix) {
    auto ts = std::chrono::steady_clock::now().time_since_epoch().count();
#ifdef _WIN32
    auto pid = GetCurrentProcessId();
#else
    auto pid = getpid();
#endif
    return prefix + "_" + std::to_string(ts) + "_" + std::to_string(pid);
}

inline std::filesystem::path test_shm_dir(const std::string& channel_name) {
    // 父目录也按进程唯一（#136）：同机并行跑多个测试二进制时，各自独占
    // <temp>/dz_shm_test_<pid>_<rand>， 不再共享一个固定父目录。channel_name
    // 本身已唯一，这里再加一层是为了满足 「全仓无固定 temp_directory_path()/dz_*
    // 目录」并让清理边界与进程一一对应。
    static const std::filesystem::path root = dztrader::test::unique_temp_dir("dz_shm_test");
    return root / channel_name;
}

inline void create_page_file(const std::filesystem::path& dir, uint64_t page_id, uint64_t size) {
    auto path = dir / std::format("{:08d}.dat", page_id);
    std::ofstream ofs(path, std::ios::binary);
    ofs.seekp(size - 1);
    ofs.put('\0');
}

inline bool cleanup_test_dir(const std::filesystem::path& shm_dir) {
    std::error_code ec;
    std::filesystem::remove_all(shm_dir, ec);
    return !ec;
}

/// 回收测试通道的内核命名对象，再删除共享内存目录。
/// ChannelMeta 构造会创建 <channel>.meta / <channel>.write 两个 ProcessMutex；
/// 读者信号量按 reader 名创建，故需调用方显式列出本用例注册过的 reader。
inline bool cleanup_channel(const std::string& channel_name,
                            const std::filesystem::path& shm_dir,
                            std::initializer_list<std::string_view> reader_names = {}) {
    ProcessMutex::remove(channel_name + ".meta");
    ProcessMutex::remove(channel_name + ".write");
    for (const auto reader_name : reader_names) {
        NamedSemaphore::remove(std::string(reader_name));
    }
    return cleanup_test_dir(shm_dir);
}

}  // namespace dztrader::shm::test

#endif  // DZTRADER_SHM_TEST_UTIL_H_
