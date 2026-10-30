/**
 * @file system_info.cpp
 * @brief `/api/system/info` 响应体实现（契约 rest §2.1）
 */

#include "system_info.h"

#include <dztrader/core/this_process.h>
#include <dztrader/version.h>

namespace dztrader::webui {

nlohmann::json system_info_body() {
    return {
        {"process_name", dztrader::this_process::exe_stem()},
        // 版本唯一真相源是根 CMakeLists.txt 的 project(VERSION)（ADR 0038），
        // 此处只读 version.h 生成物，不维护第二份版本串。
        {"version", DZ_VERSION_STRING},
    };
}

}  // namespace dztrader::webui
