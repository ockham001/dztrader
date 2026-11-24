#ifndef DZTRADER_CORE_UTF8_H_
#define DZTRADER_CORE_UTF8_H_

/**
 * @file utf8.h
 * @brief UTF-8 字符串工具（Unicode NFC 规范化）。
 *
 * 与 encoding.h 的分工：encoding.h 负责「按源编码解码成 UTF-8」，本文件只处理已经
 * 是 UTF-8 的字符串。NFC 用于唯一性判定前的等价形式统一（ADR 0056 决策 4）：
 * 例如 `"é"`（U+00E9，预组合）与 `"e"` + U+0301（基字母 + 组合重音）渲染相同但
 * 字节不同，NFC 规范化后二者字节相同，字符串比较才有「字形等价」语义。
 */

#include <optional>
#include <string>
#include <string_view>

namespace dztrader {

/**
 * @brief 把 UTF-8 字符串规范化为 NFC（Unicode Normalization Form C）。
 *
 * 输入必须是合法 UTF-8；非法字节序列返回 std::nullopt（不替换、不猜测，由调用方
 * 决定拒绝策略）。内嵌 NUL 也视为非法（本函数按 NUL 结尾语义处理输入）。空串返回
 * 空串。
 *
 * @param text UTF-8 输入
 * @return NFC 规范化结果；输入非法 UTF-8 时为 std::nullopt
 */
std::optional<std::string> utf8_nfc(std::string_view text);

}  // namespace dztrader

#endif  // DZTRADER_CORE_UTF8_H_
