#ifndef DZTRADER_PLATFORM_SYSTEM_COLLECTOR_INTERNAL_H_
#define DZTRADER_PLATFORM_SYSTEM_COLLECTOR_INTERNAL_H_

/**
 * @file system_collector_internal.h
 * @brief 两平台采集实现共用的**平台无关**小工具（系统监控采集层内部接缝）。
 *
 * 仅采集层实现与平台单测使用，不属于安装头文件。这里只放与 OS 无关的纯判定：
 * 需要平台 API 或平台路径类型的东西留在各自的实现文件里。
 *
 * 抽出来的理由是「同一判定不允许两份实现」（AGENTS 三.1）：页文件判定同时被 Linux 与
 * Windows 的共享内存目录占用统计使用，而它也是 shm 层页文件命名的约定
 * （`libs/shm/src/page_file_util.h` 的判定口径必须与此一致）。
 */

#include <algorithm>
#include <cctype>
#include <string_view>

namespace dztrader::platform::system_collector_internal {

/// shm 页文件扩展名（`<数字>.dat`）。
inline constexpr std::string_view kShmPageFileExtension = ".dat";

/**
 * @brief 判断文件名是否 shm **页文件**：`<纯数字>.dat`。
 *
 * `meta.dat`（通道元数据）不是页文件，不计入页文件数、也不参与最旧页文件年龄
 * （契约 sysmon §2.4）：它不会随页清理消失，把它算进年龄会把「页清理失效」
 * 掩盖成「有个旧文件」。
 *
 * @param filename 文件名（不含路径）；Windows 调用方先转成 UTF-8。
 * @return 是页文件返回 true。
 */
[[nodiscard]] inline bool is_shm_page_file_name(std::string_view filename) {
    if (!filename.ends_with(kShmPageFileExtension)) {
        return false;
    }
    const auto stem = filename.substr(0, filename.size() - kShmPageFileExtension.size());
    return !stem.empty() && std::all_of(stem.begin(), stem.end(),
                                        [](unsigned char c) { return std::isdigit(c) != 0; });
}

}  // namespace dztrader::platform::system_collector_internal

#endif  // DZTRADER_PLATFORM_SYSTEM_COLLECTOR_INTERNAL_H_
