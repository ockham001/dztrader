/**
 * @file params.h
 * @brief 进程级参数仓 `ParamStore` + `ctx.param<T>()` 的定义（组件文档 §9，ADR 0027 决策 12）
 *
 * 参数是策略自定义的 JSON 键值对：框架只做**运输**（读配置 → 运行期 `param set` → B 重读），
 * 参数名/类型/默认值全由策略决定。v1 参数全品种统一（一份 `params`，每个品种相同）。
 *
 * **为什么 `ctx.param<T>()` 的定义在本头而不是 `context.h`**：`InstrumentContext` 在交易
 * 意图与 tick 热路径上被每个策略 TU 使用，不应为参数读取把内置 nlohmann 单头拉进它的依赖
 * 链（ADR 0026 的隔离口径，与 `execution.h` / `scaling.h` 同一条理由）。因此：
 *   - `context.h` 只**前向声明** `ParamStore`、持 `const ParamStore*`、声明 `param<T>`；
 *   - 本头给出 `ParamStore` 与 `param<T>` 的定义。
 * 不读参数的策略只包含 `context.h` 时不会实例化 `param<T>`，也就不需要 JSON。
 *
 * 公开入口 `<dztrader/bar_strategy.h>` 必然包含本头（管理器要写参数仓），因此策略作者写
 * `ctx.param<double>("fast", 10)` 无需额外 include。
 */
#ifndef DZTRADER_BAR_STRATEGY_PARAMS_H_
#define DZTRADER_BAR_STRATEGY_PARAMS_H_

#include <dztrader/bar_strategy/context.h>
#include <dztrader/json.h>

#include <cstdint>
#include <format>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dztrader {

/// @brief JSON 形态名（错误消息用：`integer` / `number` / `boolean` / `string` / …）
[[nodiscard]] inline std::string_view json_type_name(const nlohmann::json& value) noexcept {
    if (value.is_number_integer()) {
        return "integer";
    }
    if (value.is_number_float()) {
        return "number";
    }
    if (value.is_boolean()) {
        return "boolean";
    }
    if (value.is_string()) {
        return "string";
    }
    if (value.is_null()) {
        return "null";
    }
    if (value.is_array()) {
        return "array";
    }
    return "object";
}

/**
 * @brief 进程级参数仓：JSON 对象 + 类型化读写（组件文档 §9）
 *
 * 值一律是 JSON：`params` 段落盘的形态、`ParamStore` 的内存形态、`param set` 写入的形态
 * 三者相同，因此「配置里的参数」与「运行期改过的参数」没有两套表示，写回不需要转换。
 *
 * 读取语义（{@link get}）：
 *   - 名字不存在 → 返回调用方给的默认值（策略不必为可选参数写存在性判断）；
 *   - 名字存在但**类型不匹配** → 抛 `std::invalid_argument`（响亮失败）。静默退回默认值会让
 *     「配置里写了 `fast_run` 而不是 `fast`」这类拼写错误变成「策略用了默认值跑了一整天」，
 *     与仓库「不静默修正」的配置纪律相悖；抛出后框架把它当作回调异常处理（该实例失败并
 *     打印诊断），错误定位在第一次读参数的地方。
 *
 * 类型口径（与 JSON 类型一一对应，不做隐式放宽）：
 *   | C++ 类型 | 接受的 JSON 形态 |
 *   |---|---|
 *   | `std::int64_t` | 整数（`is_number_integer`） |
 *   | `double` | 任意数值（整数也接受，含 `is_number_float`） |
 *   | `bool` | `true` / `false` |
 *   | `std::string` | 字符串 |
 * 刻意**不**把 `10.0` 当成 `int64_t`：那是静默截断（`10.5 → 10` 会把一个配置错误变成一笔
 * 错误的交易量），策略要小数就声明 `double`。
 *
 * 本类型不在热路径上：`ctx.param` 只在 B 的回调里按需读，`param set` 是命令冷路径。
 */
class ParamStore {
public:
    /// @brief 空参数仓（0 个参数；配置里 `params` 省略时的默认）
    ParamStore() = default;

    /**
     * @brief 用落盘形态构造
     * @param values JSON 对象（配置里的 `params`；`resolve_effective_config` 已校验是 object）
     */
    explicit ParamStore(nlohmann::json values)
        : values_(std::move(values)) {}

    /// @brief 当前全部参数（落盘形态；`param list` 与配置写回的基底）
    [[nodiscard]] const nlohmann::json& values() const noexcept { return values_; }

    /// @brief 参数个数（`status` 行与 `param list` 的空判定）
    [[nodiscard]] std::size_t size() const noexcept { return values_.size(); }

    /// @brief 是否存在该参数
    [[nodiscard]] bool contains(std::string_view name) const { return values_.contains(name); }

    /// @brief 全部参数名（升序，`param list` 的稳定输出顺序）
    [[nodiscard]] std::vector<std::string> names() const {
        std::vector<std::string> result;
        result.reserve(values_.size());
        for (const auto& [name, value] : values_.items()) {
            (void)value;
            result.push_back(name);
        }
        return result;  // nlohmann 的 object 是有序 map：天然按名字升序
    }

    /**
     * @brief 读一个参数（不存在 → 默认值；类型不符 → 抛 `std::invalid_argument`）
     *
     * @tparam T 目标类型（`std::int64_t` / `double` / `bool` / `std::string`）
     * @param name     参数名
     * @param fallback 名字不存在时的返回值
     * @return 参数值或默认值
     * @throw std::invalid_argument 名字存在但 JSON 形态与 `T` 不符
     */
    template <class T>
    [[nodiscard]] T get(std::string_view name, T fallback) const {
        const auto found = values_.find(name);
        if (found == values_.end()) {
            return fallback;
        }
        if (!param_type_matches<T>(*found)) {
            throw std::invalid_argument(
                std::format("param type mismatch | name={} expected={} actual={}", name,
                            param_type_name<T>(), json_type_name(*found)));
        }
        return found->template get<T>();
    }

    /**
     * @brief 写一个参数（存在则覆盖、否则新增）
     * @param name  参数名（非空；空名由命令层在校验阶段拒绝）
     * @param value 新值（JSON）
     */
    void set(std::string_view name, nlohmann::json value) {
        values_[std::string(name)] = std::move(value);
    }

private:
    /// @brief `T` 与 JSON 形态是否匹配（口径见类注释的表；`double` 接受任意数值）
    template <class T>
    [[nodiscard]] static bool param_type_matches(const nlohmann::json& value) {
        if constexpr (std::same_as<T, std::int64_t>) {
            return value.is_number_integer();
        } else if constexpr (std::same_as<T, double>) {
            return value.is_number();
        } else if constexpr (std::same_as<T, bool>) {
            return value.is_boolean();
        } else {
            return value.is_string();
        }
    }

    /// @brief 类型名（错误消息用；与 {@link param_type_matches} 的四个分支一一对应）
    template <class T>
    [[nodiscard]] static constexpr const char* param_type_name() noexcept {
        if constexpr (std::same_as<T, std::int64_t>) {
            return "integer";
        } else if constexpr (std::same_as<T, double>) {
            return "number";
        } else if constexpr (std::same_as<T, bool>) {
            return "boolean";
        } else {
            return "string";
        }
    }

    nlohmann::json values_ = nlohmann::json::object();  ///< 参数（落盘形态，有序）
};

/**
 * @brief `UI_INPUT` 的 `param set` 值 token → JSON 值（组件文档 §7「字符串协议」）
 *
 * 命令语法是空白分隔的 token 行、**不引入 JSON/引号语法**，因此值 token 里的类型信息只能
 * 靠「它长得像什么」推断。规则：能被 JSON 解析成**标量**（整数/小数/布尔/字符串）就按该
 * 类型存，否则按字符串存。
 *
 *   - `10` → 整数 10；`1.5` → 小数；`true` → 布尔；`rb2601` → 字符串（JSON 解析失败）
 *   - `null` / `[1,2]` / `{"a":1}`：token 里不可能出现空白，但 `[1,2]`、`null` 是合法 JSON。
 *     对象/数组/null **不作为参数值接受**（框架只运输标量；结构值会让 `param get` 的渲染
 *     与写回变得含糊），同样按字符串存。
 *   - 前导零（`007`）、`+1`、`.5` 不是合法 JSON 数字 → 按字符串存（不静默改数值）。
 *
 * 这个「能解析成标量就当数值」的口径必须与 {@link format_param_value} 的输出往返一致：
 * `param set x 10` 之后 `param get x` 打印 `10`，再 `param set x 10` 得到同一个值。
 *
 * @param token 值 token（非空、不含空白）
 * @return 要存进 `ParamStore` 的 JSON 标量或字符串
 */
[[nodiscard]] inline nlohmann::json param_value_from_token(std::string_view token) {
    const nlohmann::json parsed = nlohmann::json::parse(token, nullptr, /*allow_exceptions=*/false);
    if (!parsed.is_discarded() &&
        (parsed.is_number() || parsed.is_boolean() || parsed.is_string())) {
        return parsed;
    }
    return std::string(token);
}

/// @brief JSON 参数值的单行文本（`param get` / `param list` / 命令结果）
///
/// 字符串原样输出（不加引号：命令层没有引号语法，加引号会让人以为要按引号再输入一次），
/// 其余用 JSON dump（整数 `10`、小数 `1.5`、布尔 `true`）。
[[nodiscard]] inline std::string format_param_value(const nlohmann::json& value) {
    if (value.is_string()) {
        return value.get<std::string>();
    }
    return value.dump();
}

/**
 * @brief 参数读取（{@link ParamStore::get} 的 B 侧入口；定义放本头以隔离 JSON）
 *
 * B 在回调里读参数：`const double fast = ctx.param<double>("fast", 10.0);`。名字不存在时返回
 * `fallback`；名字存在但类型不符时抛 `std::invalid_argument`（框架把它当回调异常处理，见
 * `ParamStore` 类注释）。
 *
 * `params_ == nullptr`（只有脱离引擎的单元测试这样构造 ctx）时一律返回 `fallback`。
 *
 * @tparam T 参数类型（`std::int64_t` / `double` / `bool` / `std::string`）
 * @param name     参数名
 * @param fallback 名字不存在时的返回值
 * @return 参数值或默认值
 * @throw std::invalid_argument 名字存在但类型不符
 */
template <ParamValue T>
inline T InstrumentContext::param(std::string_view name, T fallback) const {
    if (params_ == nullptr) {
        return fallback;
    }
    return params_->get<T>(name, std::move(fallback));
}

}  // namespace dztrader

#endif  // DZTRADER_BAR_STRATEGY_PARAMS_H_
