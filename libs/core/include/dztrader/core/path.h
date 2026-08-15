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

}  // namespace dztrader::paths

#endif  /* DZTRADER_CORE_PATH_H_ */
