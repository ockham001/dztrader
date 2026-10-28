/**
 * @file config.h
 * @brief BarStrategy 配置层：解析 / 校验 / 自动值推导 / 有效配置日志（ADR 0027 决策 11，组件文档
 * §7）
 *
 * **内部头，不进公开承诺面**：BarStrategy 框架（#157）与后续命令层（#162）直接包含；
 * 公开入口 `dztrader/bar_strategy.h` 在 #157 里决定暴露哪些类型。策略作者（B 侧）
 * 不直接读写配置——配置由框架在启动时加载。
 *
 * 职责边界（本头不碰文件系统，文件面见 `dztrader/bar_strategy/config_file.h`）：
 *
 *   - `parse_config`：JSON 结构 / 类型 / 枚举取值 / 必填存在性。未知字段忽略（前向兼容），
 *     已知字段出现 JSON `null` 一律非法。**不做语义校验**（取值集合、非空/唯一、数值区间、
 *     跨字段关系都在 `resolve_effective_config`）。
 *   - `resolve_effective_config`：全部语义校验 + 自动值推导（容量、预热、每序列内存），
 *     产出 {@link EffectiveConfig}（含每个推导值的来源）。不合法一律抛
 *     `std::invalid_argument`，消息含字段名 / 当前值 / 合法区间，不静默修正。
 *   - `parse_and_resolve_config`：两步串联，是框架加载配置的常规入口。
 *   - `format_effective_config` / `format_effective_accounts`：启动时打印的「有效配置」
 *     与账户表（组件文档 §7）。本头只产出字符串，打印由框架负责（SDK 不引入日志，
 *     ADR 0020/0021）。
 *
 * **配置路径**：`$dz_strategy_home()/<strategy_id>.json`——`config_path()` 只做纯路径拼接；
 * `dz_strategy_home()` / `dz_strategy_id()` 的取用与缺失配置的启动流程由 #157 接线。
 *
 * 容量自动值完全复用 #155 的 `series_internal::resolve_capacity`（公式与错误消息的唯一
 * 真相源，组件文档 §4.5）；本头只负责把 v1 的周期集合校验放在它前面。
 */
#ifndef DZTRADER_BAR_STRATEGY_CONFIG_H_
#define DZTRADER_BAR_STRATEGY_CONFIG_H_

#include <dztrader/date_time/date_time.h>
#include <dztrader/json.h>
#include <dztrader/series_internal.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <format>
#include <initializer_list>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace dztrader {

// ============================================================
// 落盘字段类型（组件文档 §7「C++ 侧配置类型」）
// ============================================================

/**
 * @brief 挂靠账户（`accounts` 数组元素）：平台账户标识 + 挂靠比率（ADR 0028/0030）
 *
 * 挂靠比率 `scale` 是「逻辑持仓 → 该账户目标持仓」的比例系数：`L_i = f(scale × |P|)`。
 * 账户的登录/凭据/交易接口归属属于平台 td 配置，框架只持有标识与比率。
 */
struct AttachedAccount {
    std::string account_id;  ///< 平台账户标识（非空、配置内唯一）
    double scale = 1.0;      ///< 挂靠比率（`(0, max_scale]`）
};

/**
 * @brief 挂靠比率的取整方向（ADR 0030 决策 2）：`L_i = f(scale_i × |P|)`
 *
 * 进程级取整策略，配置里写作字符串 `"nearest"` / `"floor"` / `"ceil"`：
 *   - `Nearest`：四舍五入（默认）
 *   - `Floor`：向少取整
 *   - `Ceil`：向多取整
 */
enum class ScaleRounding {
    Nearest,  ///< 四舍五入（默认）
    Floor,    ///< 向少取整
    Ceil,     ///< 向多取整
    // 新增取值时必须同步 config_internal::SCALE_ROUNDING_NAMES（唯一映射表与日志/解析共用）
};

namespace config_internal {

/// @brief 取整策略与 wire 名的唯一映射表（enum→string / string→enum 都查它，避免两处定义）
struct ScaleRoundingName {
    ScaleRounding rounding;  ///< 枚举值
    std::string_view name;   ///< 配置字符串
};

inline constexpr std::array<ScaleRoundingName, 3> SCALE_ROUNDING_NAMES{{
    {ScaleRounding::Nearest, "nearest"},
    {ScaleRounding::Floor, "floor"},
    {ScaleRounding::Ceil, "ceil"},
}};

}  // namespace config_internal

/// @brief 取整策略的配置字符串（`nearest` / `floor` / `ceil`），用于写回与日志
[[nodiscard]] inline std::string_view scale_rounding_name(ScaleRounding rounding) noexcept {
    for (const config_internal::ScaleRoundingName& entry : config_internal::SCALE_ROUNDING_NAMES) {
        if (entry.rounding == rounding) {
            return entry.name;
        }
    }
    return "nearest";
}

/**
 * @brief 进程级缩放配置（生效值；组件文档 §3）
 *
 * 由配置层的三个可选字段解析/取默认后得到：`max_scale` 启动加载一次、盘中不可变
 * （运行期编辑无效，重启生效，ADR 0027 决策 11）；`rounding` / `min_one` 是挂靠
 * 手数换算策略（ADR 0030）。`ctx` 按值拷贝一份，随 `InstrumentContext` 分发给各实例。
 *
 * 落盘字段的「省略 vs 显式默认值」在 {@link BarStrategyConfig} 里保留（写回时原样保留
 * 省略状态），本结构只描述生效值。
 */
struct ScalingConfig {
    double max_scale = 1.0;                           ///< 挂靠比率上限（> 0）
    ScaleRounding rounding = ScaleRounding::Nearest;  ///< 取整方向
    bool min_one = false;                             ///< 目标非零时至少 1 手
};

/**
 * @brief 历史预热深度：`bars` 与 `from` 二选一（都省略 = 用默认值）
 *
 * `bars`：从归档域回放最近 N 根；`from`：从本地日期 `YYYY-MM-DD` 起回放（含当日）。
 * 两个字段同时出现或同时省略（在显式 `warmup` 对象内）都非法；整个 `warmup` 省略时
 * 用默认值 `{bars: 10000}`，并自动提升到 ≥ `max_lookback`（组件文档 §7）。
 */
struct WarmupSpec {
    std::optional<std::int64_t>
        bars;  ///< 预热根数（按 JSON 整数 int64 解析；`[max_lookback, 10000000]`）
    std::optional<std::string> from;  ///< 起始本地日期 `YYYY-MM-DD`（不晚于今天）
};

/**
 * @brief 配置文件的落盘字段（只描述文件，不含任何推导值）
 *
 * 推导结果（可读深度 / 回卷余量 / 物理长度 / 预热根数 / 每序列内存）在
 * {@link EffectiveConfig}。`params` 是策略自定义参数的 JSON 对象（v1 全品种统一）；
 * 参数名/类型/默认值由策略决定，框架只做运输（ADR 0027 决策 12）。
 */
struct BarStrategyConfig {
    std::int32_t schema_version = 1;              ///< 配置 schema 版本（当前只接受 1）
    std::int32_t period_sec = 60;                 ///< bar 周期（v1：60/180/300/900）
    std::vector<std::string> instruments;         ///< 品种列表（必填非空、不重复）
    std::vector<AttachedAccount> accounts;        ///< 挂靠账户（可空：0 账户只记逻辑台账）
    std::optional<double> max_scale;              ///< 省略 = 1.0（挂靠比率上限；运行期不可变）
    std::optional<ScaleRounding> scale_rounding;  ///< 省略 = Nearest
    std::optional<bool> scale_min_one;            ///< 省略 = false
    std::optional<std::int32_t> max_lookback;     ///< 省略 = 按周期自动推导
    std::optional<std::int32_t> reserve_bars;     ///< 省略 = 按周期自动推导
    WarmupSpec warmup;  ///< 省略 = {bars: 10000}（自动提升到 ≥ max_lookback）
    nlohmann::json params = nlohmann::json::object();  ///< 策略参数（JSON object）
};

/**
 * @brief 有效配置里每个可推导值的来源（组件文档 §7 的 `*_source` 字段）
 *
 *   - `Explicit`：配置文件显式提供（`explicit`）
 *   - `Auto`：按周期自动推导（`auto`）
 *   - `Default`：省略后用默认值（`default`；`warmup` 的自动提升也归此类）
 */
enum class ConfigValueSource {
    Explicit,  ///< 显式提供
    Auto,      ///< 自动推导
    Default,   ///< 默认值
};

/**
 * @brief 校验 + 推导后的有效配置：落盘字段 + 实际生效值 + 每个推导值的来源
 *
 * `config` 保留落盘字段（省略仍为省略）；其余字段是框架真正使用的值。
 * `warmup_bars` 与 `warmup_from` 恰有一个有值（预热模式二选一已解析）。
 */
struct EffectiveConfig {
    BarStrategyConfig config;       ///< 落盘字段（归一化后）
    ScalingConfig scaling;          ///< 生效的缩放配置（省略字段取默认）
    std::int32_t max_lookback = 0;  ///< 可读深度（显式 / 自动）
    std::int32_t reserve_bars = 0;  ///< 回卷余量（显式 / 自动）
    std::int32_t buffer_bars = 0;   ///< 物理长度 = max_lookback + reserve_bars
    ConfigValueSource max_lookback_source = ConfigValueSource::Auto;  ///< 可读深度来源
    ConfigValueSource reserve_bars_source = ConfigValueSource::Auto;  ///< 回卷余量来源
    std::optional<std::int32_t> warmup_bars;  ///< 预热根数（与 warmup_from 二选一）
    std::optional<std::string> warmup_from;   ///< 预热起始日期（与 warmup_bars 二选一）
    ConfigValueSource warmup_source = ConfigValueSource::Default;  ///< 预热来源
    std::int64_t mem_per_series_bytes =
        0;  ///< 每序列物理内存 = buffer_bars × 8B（double/int64 同宽）
};

// ============================================================
// 内部辅助（dztrader::config_internal）
// ============================================================

namespace config_internal {

/// @brief 当前配置 schema 版本（ADR 0027 决策 11：字段用于后续迁移，当前 1）
inline constexpr std::int32_t SCHEMA_VERSION_CURRENT = 1;
/// @brief 默认预热根数（组件文档 §7）
inline constexpr std::int64_t WARMUP_BARS_DEFAULT = 10'000;
/// @brief 预热根数上限
inline constexpr std::int64_t WARMUP_BARS_MAX = 10'000'000;
/// @brief v1 允许的周期（秒）
inline constexpr std::array<std::int32_t, 4> PERIOD_SEC_ALLOWED{60, 180, 300, 900};

/// @brief 周期合法区间的错误消息文案（唯一来源；parse 的越界提示与 resolve 共用）
[[nodiscard]] inline std::string period_sec_range_text() {
    std::string text = "[";
    for (std::size_t i = 0; i < PERIOD_SEC_ALLOWED.size(); ++i) {
        if (i != 0) {
            text += ',';
        }
        text += std::format("{}", PERIOD_SEC_ALLOWED[i]);
    }
    text += ']';
    return text;
}
/// @brief 缺失字段在错误消息里的值表示
inline constexpr std::string_view VALUE_MISSING = "<missing>";

/// @brief JSON 值的紧凑文本（字符串带双引号、数字/布尔原样），用于错误消息的 `value=`
[[nodiscard]] inline std::string describe_value(const nlohmann::json& value) {
    return value.dump();
}

/// @brief k=v 里的值 token：含空格/等号/双引号时加双引号（AGENTS「日志与异常」格式）
[[nodiscard]] inline std::string quote_value_token(std::string_view value) {
    if (value.find_first_of(" =\"") == std::string_view::npos) {
        return std::string(value);
    }
    std::string quoted;
    quoted.reserve(value.size() + 2);
    quoted += '"';
    quoted += value;
    quoted += '"';
    return quoted;
}

/// @brief 字符串值的 `value=` 表示（带双引号并按 JSON 规则转义，空串写作 `""`）
[[nodiscard]] inline std::string describe_string(std::string_view value) {
    return nlohmann::json(std::string(value)).dump();
}

/// @brief 整数标量的 `value=` 表示
template <class T>
[[nodiscard]] inline std::string describe_scalar(T value) {
    return std::format("{}", value);
}

/**
 * @brief 配置语义错误的统一形态：字段名 / 当前值 / 合法区间（组件文档 §7「不静默修正」）
 * @throw std::invalid_argument 总是抛出
 */
[[noreturn]] inline void throw_range(std::string_view field,
                                     const std::string& value,
                                     std::string_view range) {
    throw std::invalid_argument(std::format("invalid config | field={} value={} range={}", field,
                                            value, quote_value_token(range)));
}

/**
 * @brief 配置语义错误的统一形态：字段名 / 当前值 / 期望形态（类型或非区间约束）
 * @throw std::invalid_argument 总是抛出
 */
[[noreturn]] inline void throw_expected(std::string_view field,
                                        const std::string& value,
                                        std::string_view expected) {
    throw std::invalid_argument(std::format("invalid config | field={} value={} expected={}", field,
                                            value, quote_value_token(expected)));
}

/**
 * @brief 配置语义错误的统一形态：字段名 / 当前值 / 失败原因（重复、互斥等非区间约束）
 * @throw std::invalid_argument 总是抛出
 */
[[noreturn]] inline void throw_reason(std::string_view field,
                                      const std::string& value,
                                      std::string_view reason) {
    throw std::invalid_argument(std::format("invalid config | field={} value={} reason={}", field,
                                            value, quote_value_token(reason)));
}

/// @brief 取对象成员；不存在返回 nullptr
[[nodiscard]] inline const nlohmann::json* find_member(const nlohmann::json& object,
                                                       const char* field) {
    const auto it = object.find(field);
    return it == object.end() ? nullptr : &*it;
}

/// @brief 取必填对象成员；缺失抛错（`label` 用于嵌套字段的完整路径，如 `warmup.bars`）
[[nodiscard]] inline const nlohmann::json& require_member(const nlohmann::json& object,
                                                          const char* field,
                                                          std::string_view label = {}) {
    const auto* value = find_member(object, field);
    if (value == nullptr) {
        throw_expected(label.empty() ? std::string_view(field) : label, std::string(VALUE_MISSING),
                       "required field");
    }
    return *value;
}

/**
 * @brief 取 JSON 整数（拒绝小数/字符串/布尔/nil/超出 int64 的无符号数）
 *
 * JSON 里 `60` 与 `60.0` 是不同类型：配置字段一律按整数解析（组件文档 §7
 * 「warmup.bars 按 JSON 整数解析（int64，拒绝负数/小数）」），不做静默取整。
 */
[[nodiscard]] inline std::int64_t require_integer(const nlohmann::json& object,
                                                  const char* field,
                                                  std::string_view label = {}) {
    const std::string_view field_name = label.empty() ? std::string_view(field) : label;
    const auto& value = require_member(object, field, label);
    if (!value.is_number_integer()) {
        throw_expected(field_name, describe_value(value), "integer");
    }
    if (value.is_number_unsigned() &&
        value.get<std::uint64_t>() > static_cast<std::uint64_t>(INT64_MAX)) {
        throw_expected(field_name, describe_value(value), "int64 integer");
    }
    return value.get<std::int64_t>();
}

/// @brief 取 JSON 整数并检查落在 int32 值域
///
/// `range_hint` 非空时，超出 int32 值域按「字段合法区间」报错（而不是只说类型），
/// 让 AC「错误信息含字段名/当前值/合法区间」对超大值也成立。
[[nodiscard]] inline std::int32_t require_int32(const nlohmann::json& object,
                                                const char* field,
                                                std::string_view label = {},
                                                std::string_view range_hint = {}) {
    const std::int64_t raw = require_integer(object, field, label);
    if (raw < INT32_MIN || raw > INT32_MAX) {
        const std::string_view field_name = label.empty() ? std::string_view(field) : label;
        if (!range_hint.empty()) {
            throw_range(field_name, describe_scalar(raw), range_hint);
        }
        throw_expected(field_name, describe_scalar(raw), "int32 integer");
    }
    return static_cast<std::int32_t>(raw);
}

/// @brief 取 JSON 数字（整数与浮点都接受；有限性由 resolve 层对程序化构造的配置校验）
[[nodiscard]] inline double require_number(const nlohmann::json& object,
                                           const char* field,
                                           std::string_view label = {}) {
    const std::string_view field_name = label.empty() ? std::string_view(field) : label;
    const auto& value = require_member(object, field, label);
    if (!value.is_number()) {
        throw_expected(field_name, describe_value(value), "number");
    }
    return value.get<double>();
}

/// @brief 取 JSON 字符串（不校验非空：非空是语义校验，在 resolve 阶段）
[[nodiscard]] inline std::string require_string(const nlohmann::json& object,
                                                const char* field,
                                                std::string_view label = {}) {
    const std::string_view field_name = label.empty() ? std::string_view(field) : label;
    const auto& value = require_member(object, field, label);
    if (!value.is_string()) {
        throw_expected(field_name, describe_value(value), "string");
    }
    return value.get<std::string>();
}

/// @brief 取 JSON 布尔
[[nodiscard]] inline bool require_boolean(const nlohmann::json& object, const char* field) {
    const auto& value = require_member(object, field);
    if (!value.is_boolean()) {
        throw_expected(field, describe_value(value), "boolean");
    }
    return value.get<bool>();
}

/// @brief 取 JSON 数组（不校验非空：非空是语义校验）
[[nodiscard]] inline const nlohmann::json& require_array(const nlohmann::json& object,
                                                         const char* field) {
    const auto& value = require_member(object, field);
    if (!value.is_array()) {
        throw_expected(field, describe_value(value), "array");
    }
    return value;
}

/// @brief 解析取整策略字符串；非法值抛 `field=scale_rounding` 的错误
[[nodiscard]] inline ScaleRounding parse_scale_rounding_value(const nlohmann::json& object,
                                                              const char* field) {
    const std::string text = require_string(object, field);
    for (const ScaleRoundingName& entry : SCALE_ROUNDING_NAMES) {
        if (text == entry.name) {
            return entry.rounding;
        }
    }
    throw_range(field, describe_string(text), "[nearest,floor,ceil]");
}

/// @brief 拒绝闭集内联对象里的未知键（顶层未知键仍按前向兼容忽略，见 parse_config）
inline void reject_unknown_keys(const nlohmann::json& object,
                                std::initializer_list<std::string_view> allowed,
                                const std::string& prefix) {
    for (const auto& [key, value] : object.items()) {
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end()) {
            throw_reason(std::format("{}.{}", prefix, key), describe_value(value), "unknown_field");
        }
    }
}

/// @brief 字段下标路径（`accounts[2].scale`），错误消息用
[[nodiscard]] inline std::string indexed_field(const char* prefix,
                                               std::size_t index,
                                               const char* suffix) {
    return std::format("{}[{}]{}", prefix, index, suffix);
}

/// @brief 值来源的配置字符串（`explicit` / `auto` / `default`）
[[nodiscard]] inline std::string_view source_name(ConfigValueSource source) noexcept {
    switch (source) {
        case ConfigValueSource::Explicit:
            return "explicit";
        case ConfigValueSource::Auto:
            return "auto";
        case ConfigValueSource::Default:
            return "default";
    }
    return "auto";
}

/// @brief 比率的日志形态：最短往返表示 + 整数值补 `.0`（`1.0` / `0.5` / `0.333`）
[[nodiscard]] inline std::string format_ratio(double value) {
    std::string text = std::format("{}", value);
    if (text.find_first_of(".eE") == std::string::npos) {
        text += ".0";
    }
    return text;
}

/// @brief `YYYY-MM-DD` 形状校验（严格 10 字符定宽；不接受 `2024-1-2` 之类变体）
[[nodiscard]] inline bool is_date_text(std::string_view text) noexcept {
    if (text.size() != 10) {
        return false;
    }
    for (std::size_t i = 0; i < text.size(); ++i) {
        const bool dash_expected = (i == 4 || i == 7);
        if (dash_expected) {
            if (text[i] != '-') {
                return false;
            }
        } else if (text[i] < '0' || text[i] > '9') {
            return false;
        }
    }
    return true;
}

/// @brief 校验 `warmup.from`：严格 `YYYY-MM-DD`、真实日期、不晚于 today
inline void validate_warmup_from(const std::string& text, Date today) {
    if (!is_date_text(text)) {
        throw_expected("warmup.from", describe_string(text), "date YYYY-MM-DD");
    }
    Date parsed{};
    try {
        parsed = Date::from_string(text);
    } catch (const std::exception&) {
        // 形状对但日期不存在（2024-02-30 等）：统一成配置错误，不泄漏 SDK 内部异常文案
        throw_expected("warmup.from", describe_string(text), "valid calendar date");
    }
    if (parsed.days_to(today) < 0) {
        throw_expected("warmup.from", describe_string(text),
                       std::format("date not later than {}", today.to_string()));
    }
}

}  // namespace config_internal

// ============================================================
// 配置路径与最小默认配置（组件文档 §7）
// ============================================================

/**
 * @brief 配置路径：`$dz_strategy_home()/<strategy_id>.json`
 *
 * 纯路径拼接，便于单元测试与 #157 接线（`config_path(dz_strategy_home(ctx),
 * dz_strategy_id(ctx))`）。`strategy_home` 可带可不带结尾分隔符（SDK 契约带分隔符，
 * 这里两种都接受）；`strategy_id` 是策略可执行文件 stem，不得含路径分隔符。
 *
 * @param strategy_home 策略可执行文件目录
 * @param strategy_id   策略可执行文件 stem（配置文件名去掉 `.json`）
 * @return 配置文件路径
 * @throw std::invalid_argument `strategy_home` 为空 / `strategy_id` 为空或含路径分隔符
 */
[[nodiscard]] inline std::filesystem::path config_path(const std::filesystem::path& strategy_home,
                                                       std::string_view strategy_id) {
    if (strategy_home.empty()) {
        config_internal::throw_expected("strategy_home", "\"\"", "non-empty directory");
    }
    if (strategy_id.empty()) {
        config_internal::throw_expected("strategy_id", "\"\"", "non-empty executable stem");
    }
    if (strategy_id.find('/') != std::string_view::npos ||
        strategy_id.find('\\') != std::string_view::npos) {
        config_internal::throw_expected(
            "strategy_id", config_internal::describe_string(strategy_id), "no path separator");
    }
    std::filesystem::path file_name{std::string(strategy_id)};
    file_name += ".json";
    return strategy_home / file_name;
}

/**
 * @brief 缺失配置时生成的最小默认配置（组件文档 §7）
 *
 * 只含五个键：`schema_version=1`、`period_sec=60`、`instruments=[]`、`accounts=[]`、
 * `params={}`；**不含可选字段**（`max_scale` / `scale_rounding` / `scale_min_one` /
 * `max_lookback` / `reserve_bars` / `warmup` 全部省略，走默认值与自动推导）。
 *
 * 这份配置故意不是「可运行配置」：`instruments` 为空，加载校验会失败。框架生成它、
 * 打印提示并以非 0 退出，等作者编辑后重启（不静默替用户猜品种）。
 */
[[nodiscard]] inline nlohmann::json make_minimal_default_config() {
    nlohmann::json root = nlohmann::json::object();
    root["schema_version"] = config_internal::SCHEMA_VERSION_CURRENT;
    root["period_sec"] = 60;
    root["instruments"] = nlohmann::json::array();
    root["accounts"] = nlohmann::json::array();
    root["params"] = nlohmann::json::object();
    return root;
}

// ============================================================
// 解析（结构 / 类型层）
// ============================================================

/**
 * @brief 把配置文件 JSON 解析为落盘字段结构（不做语义校验）
 *
 * 校验范围：根必须是 object；已知字段的类型、必填存在性、枚举取值；`warmup` 的结构
 * （object + `bars`/`from` 二选一的**结构**前提：都有或都没有都是错误）；`params` 必须是
 * object。数值区间、非空/唯一、跨字段关系（`scale <= max_scale`、`warmup.bars >= max_lookback`）
 * 在 {@link resolve_effective_config} 里校验。
 *
 * 未知字段一律忽略（前向兼容，参照《帧契约：general》§8）；已知字段出现 `null` 视为类型错误。
 *
 * @param root 配置文件根 JSON
 * @return 落盘字段结构
 * @throw std::invalid_argument 结构/类型/枚举/必填校验失败
 */
[[nodiscard]] inline BarStrategyConfig parse_config(const nlohmann::json& root) {
    using namespace config_internal;

    if (!root.is_object()) {
        throw_expected("config", describe_value(root), "object");
    }

    BarStrategyConfig config;
    const std::int64_t schema_version = require_integer(root, "schema_version");
    if (schema_version != SCHEMA_VERSION_CURRENT) {
        throw_expected("schema_version", describe_scalar(schema_version), "1");
    }
    config.schema_version = static_cast<std::int32_t>(schema_version);
    config.period_sec = require_int32(root, "period_sec", {}, period_sec_range_text());

    const auto& instruments = require_array(root, "instruments");
    config.instruments.reserve(instruments.size());
    for (std::size_t i = 0; i < instruments.size(); ++i) {
        const auto& item = instruments[i];
        if (!item.is_string()) {
            throw_expected(indexed_field("instruments", i, ""), describe_value(item), "string");
        }
        config.instruments.push_back(item.get<std::string>());
    }

    if (find_member(root, "max_scale") != nullptr) {
        config.max_scale = require_number(root, "max_scale");
    }
    if (find_member(root, "scale_rounding") != nullptr) {
        config.scale_rounding = parse_scale_rounding_value(root, "scale_rounding");
    }
    if (find_member(root, "scale_min_one") != nullptr) {
        config.scale_min_one = require_boolean(root, "scale_min_one");
    }

    if (const auto* accounts = find_member(root, "accounts"); accounts != nullptr) {
        if (!accounts->is_array()) {
            throw_expected("accounts", describe_value(*accounts), "array");
        }
        config.accounts.reserve(accounts->size());
        for (std::size_t i = 0; i < accounts->size(); ++i) {
            const auto& item = (*accounts)[i];
            if (!item.is_object()) {
                throw_expected(indexed_field("accounts", i, ""), describe_value(item), "object");
            }
            reject_unknown_keys(item, {"account_id", "scale"}, indexed_field("accounts", i, ""));
            AttachedAccount account;
            account.account_id =
                require_string(item, "account_id", indexed_field("accounts", i, ".account_id"));
            account.scale = require_number(item, "scale", indexed_field("accounts", i, ".scale"));
            config.accounts.push_back(std::move(account));
        }
    }

    if (find_member(root, "max_lookback") != nullptr) {
        config.max_lookback =
            require_int32(root, "max_lookback", {},
                          std::format("[{},{}]", series_internal::MAX_LOOKBACK_MIN,
                                      series_internal::MAX_LOOKBACK_MAX));
    }
    if (find_member(root, "reserve_bars") != nullptr) {
        // 区间下界随周期变化（R_min）；周期短到区间倒置时不给提示，交给 resolve 的周期校验
        const std::int32_t reserve_floor = series_internal::min_reserve_bars(config.period_sec);
        const std::string reserve_range =
            reserve_floor <= series_internal::RESERVE_BARS_MAX
                ? std::format("[{},{}]", reserve_floor, series_internal::RESERVE_BARS_MAX)
                : std::string{};
        config.reserve_bars = require_int32(root, "reserve_bars", {}, reserve_range);
    }

    if (const auto* warmup = find_member(root, "warmup"); warmup != nullptr) {
        if (!warmup->is_object()) {
            throw_expected("warmup", describe_value(*warmup), "object");
        }
        reject_unknown_keys(*warmup, {"bars", "from"}, "warmup");
        const bool has_bars = find_member(*warmup, "bars") != nullptr;
        const bool has_from = find_member(*warmup, "from") != nullptr;
        if (has_bars && has_from) {
            throw_reason("warmup", describe_value(*warmup), "mutually_exclusive");
        }
        if (!has_bars && !has_from) {
            throw_expected("warmup", describe_value(*warmup), "bars or from");
        }
        if (has_bars) {
            config.warmup.bars = require_integer(*warmup, "bars", "warmup.bars");
        } else {
            config.warmup.from = require_string(*warmup, "from", "warmup.from");
        }
    }

    if (const auto* params = find_member(root, "params"); params != nullptr) {
        if (!params->is_object()) {
            throw_expected("params", describe_value(*params), "object");
        }
        config.params = *params;
    }

    return config;
}

// ============================================================
// 语义校验 + 推导
// ============================================================

/**
 * @brief 语义校验 + 自动值推导，产出有效配置
 *
 * 校验（不合法一律抛 `std::invalid_argument`，不静默修正）：
 *   1. `schema_version` 必须为 1；`period_sec` ∈ {60,180,300,900}
 *   2. `instruments` 非空、每项非空字符串、不重复
 *   3. `max_scale` 有限正数；`accounts` 的 id 非空且唯一、`scale` 有限正数且 `<= max_scale`
 *   4. 容量：`max_lookback` / `reserve_bars` 显式值按 `series_internal::resolve_capacity`
 *      的区间校验（错误消息由该函数给出）；省略则按周期自动推导
 *   5. `warmup`：省略 → `{bars: max(10000, max_lookback)}`（source=Default）；
 *      显式 `bars` ∈ [`max_lookback`, 10000000]；显式 `from` 严格 `YYYY-MM-DD` 且不晚于 `today`
 *
 * 推导：可读深度 / 回卷余量 / 物理长度 / 预热模式与来源 / 每序列内存。
 *
 * @param config 落盘字段（通常来自 {@link parse_config}；也可由命令层修改后调用）
 * @param today  “今天”（本地日期）：`warmup.from` 的上界；可注入以保证测试确定性
 * @return 有效配置
 * @throw std::invalid_argument 任一语义校验失败
 */
[[nodiscard]] inline EffectiveConfig resolve_effective_config(const BarStrategyConfig& config,
                                                              Date today) {
    using namespace config_internal;

    EffectiveConfig effective;
    effective.config = config;

    if (config.schema_version != SCHEMA_VERSION_CURRENT) {
        throw_expected("schema_version", describe_scalar(config.schema_version), "1");
    }
    if (std::find(PERIOD_SEC_ALLOWED.begin(), PERIOD_SEC_ALLOWED.end(), config.period_sec) ==
        PERIOD_SEC_ALLOWED.end()) {
        throw_range("period_sec", describe_scalar(config.period_sec), period_sec_range_text());
    }

    if (config.instruments.empty()) {
        throw_expected("instruments", "[]", "non-empty array");
    }
    {
        std::unordered_set<std::string> seen;
        for (std::size_t i = 0; i < config.instruments.size(); ++i) {
            const std::string& instrument = config.instruments[i];
            if (instrument.empty()) {
                throw_expected(indexed_field("instruments", i, ""), "\"\"", "non-empty string");
            }
            if (!seen.insert(instrument).second) {
                throw_reason(indexed_field("instruments", i, ""), describe_string(instrument),
                             "duplicate");
            }
        }
    }

    effective.scaling.max_scale = config.max_scale.value_or(1.0);
    if (!std::isfinite(effective.scaling.max_scale) || effective.scaling.max_scale <= 0.0) {
        throw_expected("max_scale", format_ratio(effective.scaling.max_scale),
                       "positive finite number");
    }
    effective.scaling.rounding = config.scale_rounding.value_or(ScaleRounding::Nearest);
    effective.scaling.min_one = config.scale_min_one.value_or(false);

    {
        std::unordered_set<std::string> seen;
        for (std::size_t i = 0; i < config.accounts.size(); ++i) {
            const AttachedAccount& account = config.accounts[i];
            if (account.account_id.empty()) {
                throw_expected(indexed_field("accounts", i, ".account_id"), "\"\"",
                               "non-empty string");
            }
            if (!seen.insert(account.account_id).second) {
                throw_reason(indexed_field("accounts", i, ".account_id"),
                             describe_string(account.account_id), "duplicate");
            }
            if (!std::isfinite(account.scale) || account.scale <= 0.0) {
                throw_expected(indexed_field("accounts", i, ".scale"), format_ratio(account.scale),
                               "positive finite number");
            }
            if (account.scale > effective.scaling.max_scale) {
                throw_range(indexed_field("accounts", i, ".scale"), format_ratio(account.scale),
                            std::format("(0,{}]", format_ratio(effective.scaling.max_scale)));
            }
        }
    }

    // 容量：公式、显式值区间、派生上限与错误消息全部复用 #155 的单一实现
    const series_internal::Capacity capacity =
        series_internal::resolve_capacity(series_internal::CapacityInput{
            config.period_sec, config.max_lookback, config.reserve_bars});
    effective.max_lookback = capacity.max_lookback;
    effective.reserve_bars = capacity.reserve_bars;
    effective.buffer_bars = capacity.buffer_bars;
    effective.max_lookback_source =
        config.max_lookback.has_value() ? ConfigValueSource::Explicit : ConfigValueSource::Auto;
    effective.reserve_bars_source =
        config.reserve_bars.has_value() ? ConfigValueSource::Explicit : ConfigValueSource::Auto;

    if (config.warmup.bars.has_value() && config.warmup.from.has_value()) {
        // parse 阶段已经拒绝；这里是 hand-built 配置（命令层/测试）的防御性检查
        throw_reason("warmup",
                     std::format("{{\"bars\":{},\"from\":\"{}\"}}", *config.warmup.bars,
                                 *config.warmup.from),
                     "mutually_exclusive");
    }
    if (config.warmup.bars.has_value()) {
        const std::int64_t bars = *config.warmup.bars;
        if (bars < static_cast<std::int64_t>(effective.max_lookback) || bars > WARMUP_BARS_MAX) {
            throw_range("warmup.bars", describe_scalar(bars),
                        std::format("[{},{}]", effective.max_lookback, WARMUP_BARS_MAX));
        }
        effective.warmup_bars = static_cast<std::int32_t>(bars);
        effective.warmup_source = ConfigValueSource::Explicit;
    } else if (config.warmup.from.has_value()) {
        validate_warmup_from(*config.warmup.from, today);
        effective.warmup_from = *config.warmup.from;
        effective.warmup_source = ConfigValueSource::Explicit;
    } else {
        // 默认值自动提升到 >= max_lookback（组件文档 §7）：来源仍是 default，
        // 自动提升属于默认值的推导口径，不是用户显式配置。
        const std::int64_t bars =
            std::max(WARMUP_BARS_DEFAULT, static_cast<std::int64_t>(effective.max_lookback));
        effective.warmup_bars = static_cast<std::int32_t>(bars);
        effective.warmup_source = ConfigValueSource::Default;
    }

    effective.mem_per_series_bytes = static_cast<std::int64_t>(effective.buffer_bars) * 8;
    return effective;
}

/**
 * @brief 语义校验 + 自动值推导（“今天”取本机本地日期）
 *
 * 等价于 `resolve_effective_config(config, DateTime::local_now().date())`。测试与需要
 * 确定性的调用方应使用带 `today` 参数的重载。
 */
[[nodiscard]] inline EffectiveConfig resolve_effective_config(const BarStrategyConfig& config) {
    return resolve_effective_config(config, DateTime::local_now().date());
}

/// @brief `parse_config` + `resolve_effective_config` 一步到位（“今天”可注入）
[[nodiscard]] inline EffectiveConfig parse_and_resolve_config(const nlohmann::json& root,
                                                              Date today) {
    return resolve_effective_config(parse_config(root), today);
}

/// @brief `parse_config` + `resolve_effective_config` 一步到位（“今天”取本机本地日期）
[[nodiscard]] inline EffectiveConfig parse_and_resolve_config(const nlohmann::json& root) {
    return resolve_effective_config(parse_config(root));
}

// ============================================================
// 有效配置日志（组件文档 §7；只产出字符串，打印由框架负责）
// ============================================================

/**
 * @brief 启动「有效配置」日志行（单行、无换行）
 *
 * 每个自动推导值都带来源（`explicit` / `auto` / `default`），让人一眼看出框架替策略
 * 选了什么。字段顺序与组件文档 §7 示例一致：
 * `config effective | schema_version=… period_sec=… accounts=… max_scale=… scale_rounding=…
 * scale_min_one=… max_lookback=… max_lookback_source=… reserve_bars=… reserve_bars_source=…
 * buffer_bars=… warmup_bars=…|warmup_from=… warmup_source=… mem_per_series=…`
 *
 * @pre `effective.warmup_bars` 与 `effective.warmup_from` 恰有一个有值
 * @param effective 已校验/推导的有效配置
 * @return 单行日志文本（不含换行；格式符合 `prose | k=v` 约定）
 */
[[nodiscard]] inline std::string format_effective_config(const EffectiveConfig& effective) {
    using namespace config_internal;
    std::string warmup_field;
    if (effective.warmup_from.has_value()) {
        warmup_field = std::format("warmup_from={}", *effective.warmup_from);
    } else {
        warmup_field = std::format("warmup_bars={}", effective.warmup_bars.value_or(0));
    }
    return std::format(
        "config effective | schema_version={} period_sec={} accounts={} max_scale={} "
        "scale_rounding={} scale_min_one={} max_lookback={} max_lookback_source={} "
        "reserve_bars={} reserve_bars_source={} buffer_bars={} {} warmup_source={} "
        "mem_per_series={}",
        effective.config.schema_version, effective.config.period_sec,
        effective.config.accounts.size(), format_ratio(effective.scaling.max_scale),
        scale_rounding_name(effective.scaling.rounding), effective.scaling.min_one ? 1 : 0,
        effective.max_lookback, source_name(effective.max_lookback_source), effective.reserve_bars,
        source_name(effective.reserve_bars_source), effective.buffer_bars, warmup_field,
        source_name(effective.warmup_source), effective.mem_per_series_bytes);
}

/**
 * @brief 启动「账户表」日志行（单行、无换行）
 *
 * 非空：`accounts effective | CTP001=1.0 CTP002=0.5`（按配置数组顺序）；
 * 空表：`accounts effective | -`（0 个挂靠账户时策略只记逻辑台账、不发实盘单）。
 *
 * @param accounts 挂靠账户表
 * @return 单行日志文本（不含换行）
 */
[[nodiscard]] inline std::string format_effective_accounts(
    const std::vector<AttachedAccount>& accounts) {
    if (accounts.empty()) {
        return "accounts effective | -";
    }
    std::string line = "accounts effective |";
    for (const AttachedAccount& account : accounts) {
        line += ' ';
        line += account.account_id;
        line += '=';
        line += config_internal::format_ratio(account.scale);
    }
    return line;
}

}  // namespace dztrader

#endif  // DZTRADER_BAR_STRATEGY_CONFIG_H_
