#ifndef DZTRADER_PROCESS_PROCESS_KIND_H_
#define DZTRADER_PROCESS_PROCESS_KIND_H_

/**
 * @file process_kind.h
 * @brief 进程类别枚举与扫描结果结构体
 */

#include <cstdint>
#include <filesystem>
#include <string>

namespace dztrader::process {

/// 进程类别, 由可执行文件名前缀识别 (dzmd_/dztd_/dzweb)
enum class ProcessKind : uint8_t {
    GatewayMd,    // dzmd_*
    GatewayTd,    // dztd_*
    WebUI,        // dzweb
    Unknown,
};

/// 单个可执行文件扫描结果
struct ProcessExeInfo {
    std::string name;                  // 进程名 (stem, 如 "dzmd_ctp")
    ProcessKind kind;                  // 类别
    std::filesystem::path exe;         // 完整可执行文件路径
    std::filesystem::path start_dir;   // 父目录 (启动 cwd)
};

}  // namespace dztrader::process

#endif  // DZTRADER_PROCESS_PROCESS_KIND_H_
