/**
 * @file this_process.cpp
 * @brief 当前进程自省实现
 */

#ifdef _WIN32
#include <windows.h>
#endif

#include <dztrader/core/encoding.h>
#include <dztrader/core/exception.h>
#include <dztrader/core/this_process.h>
#include <dztrader/error.h>

#include <filesystem>
#include <stdexcept>
#include <string>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace dztrader::this_process {

namespace {

std::filesystem::path compute_exe_path()
{
#ifdef _WIN32
    // 使用增长缓冲区处理长路径
    // GetModuleFileNameW 成功时返回不含 null 终止符的长度
    // 缓冲区不足时返回 nSize 并设置 ERROR_INSUFFICIENT_BUFFER
    // 注意：len == nSize 表示路径被截断，不是成功
    std::wstring buf;
    DWORD buf_size = MAX_PATH;
    while (true) {
        buf.resize(buf_size);
        const DWORD len = GetModuleFileNameW(nullptr, buf.data(), buf_size);
        if (len == 0) {
            throw Exception(DZ_EC_SYSTEM,
                std::string("msg=\"GetModuleFileNameW failed\""));
        }
        if (len < buf_size) {
            buf.resize(len);
            return std::filesystem::path(buf);
        }
        // len == buf_size：路径被截断或恰好填满缓冲区，扩大缓冲区重试
        buf_size *= 2;
        if (buf_size > 32768) {
            throw Exception(DZ_EC_SYSTEM,
                std::string("msg=\"exe path exceeds 32768 characters\""));
        }
    }
#else
    // /proc/self/exe 是 Linux 标准方式
    std::error_code ec;
    auto p = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (ec) {
        throw Exception(DZ_EC_SYSTEM,
            std::string("msg=\"read_symlink /proc/self/exe failed\" err=\"") + dztrader::to_utf8_from_system(ec.message()) + "\"");
    }
    return p;
#endif
}

/// 检查指定目录下是否存在 dztraderd[.exe] 标记文件
bool has_master_marker(const std::filesystem::path& dir) {
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec) || ec) {
        return false;
    }
#ifdef _WIN32
    // Windows: 同时检查 dztraderd.exe 和 dztraderd
    return std::filesystem::exists(dir / "dztraderd.exe", ec) ||
           std::filesystem::exists(dir / "dztraderd", ec);
#else
    return std::filesystem::exists(dir / "dztraderd", ec);
#endif
}

/// 从 exe_dir 开始向上查找 dztraderd[.exe]，最多向上 3 层
std::filesystem::path compute_app_root()
{
    auto dir = exe_dir();
    for (int i = 0; i < 4; ++i) {  // exe_dir + 3 祖先 = 4 个目录
        if (has_master_marker(dir)) {
            return dir;
        }
        auto parent = dir.parent_path();
        if (parent == dir) {
            // 已到根目录，无法继续向上
            break;
        }
        dir = parent;
    }
    throw std::runtime_error(
        "app root not found: dztraderd[.exe] not found in exe_dir or 3 ancestors");
}

}  // namespace

const std::filesystem::path& exe_path()
{
    static const auto CACHED_PATH = compute_exe_path();
    return CACHED_PATH;
}

const std::filesystem::path& exe_dir()
{
    static const auto CACHED_DIR = exe_path().parent_path();
    return CACHED_DIR;
}

const std::filesystem::path& app_root()
{
    static const auto CACHED_ROOT = compute_app_root();
    return CACHED_ROOT;
}

const std::string& exe_name()
{
    static const auto CACHED_NAME = exe_path().filename().string();
    return CACHED_NAME;
}

const std::string& exe_stem()
{
    static const auto CACHED_STEM = exe_path().stem().string();
    return CACHED_STEM;
}

int pid()
{
#ifdef _WIN32
    return static_cast<int>(GetCurrentProcessId());
#else
    return static_cast<int>(getpid());
#endif
}

}  // namespace dztrader::this_process
