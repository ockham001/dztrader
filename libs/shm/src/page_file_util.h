#ifndef DZTRADER_SHM_PAGE_FILE_UTIL_H_
#define DZTRADER_SHM_PAGE_FILE_UTIL_H_

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace dztrader::shm::page_file_util_internal {

/// 数字页文件名（%08d.dat）；meta.dat 等非数字 .dat 不属于页文件。
inline bool is_page_file(const std::filesystem::path& path) {
    if (path.extension() != ".dat") {
        return false;
    }
    const auto stem = path.stem().string();
    return !stem.empty() && std::all_of(stem.begin(), stem.end(),
                                        [](unsigned char c) { return std::isdigit(c) != 0; });
}

}  // namespace dztrader::shm::page_file_util_internal

#endif  // DZTRADER_SHM_PAGE_FILE_UTIL_H_
