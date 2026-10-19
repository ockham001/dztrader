#ifndef DZTRADER_CORE_PATH_H_
#define DZTRADER_CORE_PATH_H_

/**
 * @file path.h
 * @brief DZTRADER_HOME 目录布局
 */

#include <filesystem>

namespace dztrader::paths {

/**
 * @brief DZTRADER_HOME 根目录（缓存，线程安全）
 *
 * 优先读环境变量 DZTRADER_HOME，未设置则用系统默认。
 * 首次调用时自动创建所有子目录。
 */
const std::filesystem::path& home();

/**
 * @brief 配置文件目录（缓存，线程安全）
 */
const std::filesystem::path& configs();

/**
 * @brief 共享内存映射文件目录（缓存，线程安全）
 */
const std::filesystem::path& shm();

/**
 * @brief 日志文件目录（缓存，线程安全）
 */
const std::filesystem::path& logs();

/**
 * @brief 缓存数据目录（缓存，线程安全）
 */
const std::filesystem::path& cache();

/**
 * @brief 策略程序目录（缓存，线程安全）
 */
const std::filesystem::path& strategies();

/**
 * @brief 数据库文件目录（缓存，线程安全）
 */
const std::filesystem::path& db();

/**
 * @brief td 统一数据库文件 (db/td.db; 所有 td 网关共写)
 */
const std::filesystem::path& td_db();

/**
 * @brief 清空全部路径缓存（仅测试使用）
 *
 * 测试 fixture 在修改 `DZTRADER_HOME` 后调用一次，使后续 `paths::*`
 * 按新环境重新解析。产品代码不得调用。
 *
 * @warning 调用后此前取得的路径引用全部失效，调用方不得跨重置持有引用。
 */
void reset_for_test();

}  // namespace dztrader::paths

#endif /* DZTRADER_CORE_PATH_H_ */
