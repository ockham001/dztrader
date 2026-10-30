#ifndef DZTRADER_WEBUI_SYSTEM_INFO_H_
#define DZTRADER_WEBUI_SYSTEM_INFO_H_

/**
 * @file system_info.h
 * @brief `/api/system/info` 响应体（契约 rest §2.1）
 *
 * 从 `main.cpp` 抽出为独立编译单元，使响应体可被单元测试直接断言（`main.cpp` 不进
 * dzweb_obj，无法链接进测试二进制）。
 */

#include <nlohmann/json.hpp>

namespace dztrader::webui {

/**
 * @brief 构造 `/api/system/info` 的响应体。
 *
 * 字段（契约 rest §2.1，仅只读）：
 * - `process_name`：dzweb 可执行文件名（不含扩展名），供前端比对 logger 名以禁用自我日志 tail；
 * - `version`：工程版本串（`DZ_VERSION_STRING`，单一真相源为根 `CMakeLists.txt` 的
 *   `project(VERSION)`，含 pre-release 后缀），供「关于」页显示。
 *
 * 免认证端点，不得包含任何环境/主机敏感信息。
 */
nlohmann::json system_info_body();

}  // namespace dztrader::webui

#endif  // DZTRADER_WEBUI_SYSTEM_INFO_H_
