/**
 * @file bar_diff_compare.h
 * @brief issue #92 差分验收的 Bar 逐字段比较器（fast / slow 两个 target 共用）
 *
 * 本头只存在于测试代码，不属于生产 seam。比较器由**字段表驱动**：新增 Bar 公开字段时
 * 必须同时扩表，否则 `static_assert(sizeof(Bar) == 96)` 会变红（Bar 无新增字段时大小恒为
 * 96 字节：四个 int32 + 四个 double + 四个 8 字节累计量 + bool + 尾部 padding）。
 */
#ifndef DZTRADER_STRATEGY_API_TESTS_BAR_DIFF_COMPARE_H_
#define DZTRADER_STRATEGY_API_TESTS_BAR_DIFF_COMPARE_H_

#include <dztrader/bar/types.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace dztrader::strategy_api::test {

/**
 * @brief Bar 的一个被比较字段
 *
 * 整型字段用 `as_int`、浮点字段用 `as_double`（按 `is_double` 二选一），使比较器可以只
 * 遍历字段表而不必为每个字段写一遍比较代码——新增字段时忘记扩表会由
 * `static_assert(sizeof(Bar) == BAR_EXPECTED_SIZE)` 拦下。
 */
struct BarFieldSpec {
    const char* name;                                ///< 字段名（失败信息里原样输出）
    bool is_double;                                  ///< true = 浮点字段（用 as_double 比较）
    int64_t (*as_int)(const dztrader::bar::Bar&);    ///< 整型取值（is_double 时不用）
    double (*as_double)(const dztrader::bar::Bar&);  ///< 浮点取值（整型字段时不用）
};

/// @brief Bar 在 64 位平台上的预期大小；字段增删会改变该值并触发编译期失败
inline constexpr std::size_t BAR_EXPECTED_SIZE = 96;

static_assert(sizeof(dztrader::bar::Bar) == BAR_EXPECTED_SIZE,
              "Bar layout changed: 新增/删除字段时必须同步 BAR_FIELDS 与差分白名单说明");
static_assert(std::is_trivially_copyable_v<dztrader::bar::Bar>,
              "differential comparison assumes Bar is trivially copyable");

/// @brief Bar 全部公开字段的比较表（顺序即逐字段比较顺序）
inline constexpr std::array<BarFieldSpec, 14> BAR_FIELDS = {{
    {"trading_day", false, [](const auto& bar) { return static_cast<int64_t>(bar.trading_day); },
     nullptr},
    {"begin_sec", false, [](const auto& bar) { return static_cast<int64_t>(bar.begin_sec); },
     nullptr},
    {"last_tick_sec", false,
     [](const auto& bar) { return static_cast<int64_t>(bar.last_tick_sec); }, nullptr},
    {"period_sec", false, [](const auto& bar) { return static_cast<int64_t>(bar.period_sec); },
     nullptr},
    {"open", true, nullptr, [](const auto& bar) { return bar.open; }},
    {"high", true, nullptr, [](const auto& bar) { return bar.high; }},
    {"low", true, nullptr, [](const auto& bar) { return bar.low; }},
    {"close", true, nullptr, [](const auto& bar) { return bar.close; }},
    {"volume", false, [](const auto& bar) { return static_cast<int64_t>(bar.volume); }, nullptr},
    {"turnover", true, nullptr, [](const auto& bar) { return bar.turnover; }},
    {"open_interest", false,
     [](const auto& bar) { return static_cast<int64_t>(bar.open_interest); }, nullptr},
    {"cum_volume", false, [](const auto& bar) { return static_cast<int64_t>(bar.cum_volume); },
     nullptr},
    {"cum_turnover", true, nullptr, [](const auto& bar) { return bar.cum_turnover; }},
    {"is_final", false, [](const auto& bar) { return static_cast<int64_t>(bar.is_final); },
     nullptr},
}};

static_assert(BAR_FIELDS.size() == 14, "Bar field table must cover every public field");

/// @brief 单根 bar 的一个不一致字段
struct FieldMismatch {
    std::string field;     ///< 字段名
    std::string expected;  ///< oracle 期望值（字符串化）
    std::string actual;    ///< builder 实际值（字符串化）
};

/// @brief bar 序列比较结果（首个不一致即停止）
struct BarSequenceDiff {
    bool matched = true;                    ///< true = 序列与全部字段一致
    std::size_t index = 0;                  ///< 首个不一致的 bar 下标
    std::string reason;                     ///< 不一致原因（数量不等 / 字段不等）
    std::vector<FieldMismatch> mismatches;  ///< 该 bar 上全部不一致字段
};

/// @brief 把 double 以可读且可区分的形式字符串化（17 位有效位保证可区分）
inline std::string format_double(double value) {
    std::ostringstream stream;
    stream.precision(17);
    stream << value;
    return stream.str();
}

/// @brief 比较单根 bar 的全部公开字段（遍历 BAR_FIELDS），收集不一致字段
inline std::vector<FieldMismatch> compare_bar_fields(const dztrader::bar::Bar& actual,
                                                     const dztrader::bar::Bar& expected) {
    std::vector<FieldMismatch> mismatches;
    mismatches.reserve(BAR_FIELDS.size());
    for (const BarFieldSpec& field : BAR_FIELDS) {
        if (field.is_double) {
            const double actual_value = field.as_double(actual);
            const double expected_value = field.as_double(expected);
            // 浮点用精确比较：oracle 与 builder 对同一 tick 执行同一批差分运算，
            // 语义一致时结果必然逐位相同；用容差会掩盖「换了运算顺序/口径」的真实回归。
            if (actual_value != expected_value) {
                mismatches.push_back(
                    {field.name, format_double(expected_value), format_double(actual_value)});
            }
            continue;
        }
        const int64_t actual_value = field.as_int(actual);
        const int64_t expected_value = field.as_int(expected);
        if (actual_value != expected_value) {
            mismatches.push_back(
                {field.name, std::to_string(expected_value), std::to_string(actual_value)});
        }
    }
    return mismatches;
}

/**
 * @brief 逐根比较 bar 序列，定位首个不一致的 bar 与字段
 *
 * @param actual 被测 builder 产出的 bar 序列
 * @param expected oracle 产出的 bar 序列
 * @param scenario 场景描述（拼进失败信息，便于定位是哪条流）
 * @return 比较结果
 */
inline BarSequenceDiff diff_bar_sequences(const std::vector<dztrader::bar::Bar>& actual,
                                          const std::vector<dztrader::bar::Bar>& expected,
                                          const std::string& scenario) {
    BarSequenceDiff result;
    const std::size_t common = actual.size() < expected.size() ? actual.size() : expected.size();
    for (std::size_t i = 0; i < common; ++i) {
        const std::vector<FieldMismatch> mismatches = compare_bar_fields(actual[i], expected[i]);
        if (!mismatches.empty()) {
            result.matched = false;
            result.index = i;
            result.mismatches = mismatches;
            result.reason = "bar field mismatch | scenario=" + scenario;
            return result;
        }
    }
    if (actual.size() != expected.size()) {
        result.matched = false;
        result.index = common;
        result.reason = "bar count differs | scenario=" + scenario +
                        " actual_count=" + std::to_string(actual.size()) +
                        " expected_count=" + std::to_string(expected.size());
    }
    return result;
}

/// @brief 把比较结果拼成可直接贴进断言失败信息的文本（首个不一致 bar + 逐字段期望/实际）
inline std::string describe_diff(const BarSequenceDiff& diff,
                                 const std::vector<dztrader::bar::Bar>& actual,
                                 const std::vector<dztrader::bar::Bar>& expected) {
    std::ostringstream stream;
    stream << diff.reason << " | first_mismatch_bar=" << diff.index;
    if (diff.index < actual.size()) {
        stream << " actual_id=(" << actual[diff.index].trading_day << ","
               << actual[diff.index].begin_sec << ")";
    }
    if (diff.index < expected.size()) {
        stream << " expected_id=(" << expected[diff.index].trading_day << ","
               << expected[diff.index].begin_sec << ")";
    }
    for (const FieldMismatch& mismatch : diff.mismatches) {
        stream << " | field=" << mismatch.field << " expected=" << mismatch.expected
               << " actual=" << mismatch.actual;
    }
    return stream.str();
}

/**
 * @brief 差分断言：bar 序列逐根逐字段一致（失败时给出首个不一致 bar 与字段）
 *
 * 同时要求两侧序列非空：生成器退化产出 0 根 bar 时，逐根循环会空转变绿，等于掩盖缺陷。
 */
inline void expect_sequences_equal(const std::vector<dztrader::bar::Bar>& actual,
                                   const std::vector<dztrader::bar::Bar>& expected,
                                   const std::string& scenario) {
    ASSERT_FALSE(actual.empty()) << "builder produced no bar | scenario=" << scenario;
    ASSERT_FALSE(expected.empty()) << "oracle produced no bar | scenario=" << scenario;
    const BarSequenceDiff diff = diff_bar_sequences(actual, expected, scenario);
    ASSERT_TRUE(diff.matched) << describe_diff(diff, actual, expected);
}

}  // namespace dztrader::strategy_api::test

#endif  // DZTRADER_STRATEGY_API_TESTS_BAR_DIFF_COMPARE_H_
