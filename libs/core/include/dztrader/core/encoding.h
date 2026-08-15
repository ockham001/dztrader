#ifndef DZTRADER_CORE_ENCODING_H_
#define DZTRADER_CORE_ENCODING_H_

/**
 * @file encoding.h
 * @brief 跨平台编码转换工具
 *
 * 设计原则: 按源头编码命名, 调用方一眼看出该用哪个函数。
 * - to_utf8_from_system : boost::error_code::message(), std::filesystem 错误信息
 *                        等 OS 按当前 locale 生成的字符串 (Windows CP_ACP)
 * - to_utf8_from_gbk    : CTP SDK 返回的 ErrorMsg 等固定 GBK 编码的字符串
 *
 * 鲁棒性: 任何输入都不抛异常, 不崩溃。失败时返回原串或带替换字符的 UTF-8,
 * 宁可乱码也不让进程崩溃 (符合项目核心路径容错要求)。
 */

#include <string>

namespace dztrader {

/**
 * @brief 将系统默认编码字符串转换为 UTF-8。
 *
 * 适用: boost::error_code::message(), std::filesystem 错误信息等
 *       由 OS 按当前 locale 生成的字符串。
 *
 * - Windows: 使用 CP_ACP (系统默认 ANSI 代码页, 中文系统为 GBK) 转换为 UTF-8。
 * - Linux/macOS: 直接返回原字符串 (默认 UTF-8 locale)。
 *
 * 失败时不抛异常, 返回原串或带替换字符的 UTF-8。
 */
std::string to_utf8_from_system(const std::string& s);

/**
 * @brief 将 GBK 编码字符串转换为 UTF-8。
 *
 * 适用: CTP SDK 返回的 ErrorMsg 等固定 GBK 编码的字符串,
 *       与运行时系统 locale 无关。
 *
 * - Windows: 显式使用代码页 936 (GBK) 转换, 不依赖 CP_ACP。
 * - Linux/macOS: 直接返回原字符串 (CTP SDK 在 Linux 上已返回 UTF-8)。
 *
 * 失败时不抛异常, 返回原串 (可能不是合法 UTF-8)。
 */
std::string to_utf8_from_gbk(const std::string& s);

}  // namespace dztrader

#endif  // DZTRADER_CORE_ENCODING_H_
