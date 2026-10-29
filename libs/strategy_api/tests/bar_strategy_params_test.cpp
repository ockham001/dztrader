/**
 * @file bar_strategy_params_test.cpp
 * @brief 参数仓 `ParamStore` 与 `param set` 值 token 解释的单元测试（issue #162；组件文档 §9）
 *
 * 三组东西都是纯值语义、零依赖，因此在纯单元层逐格断言，不经帧通道：
 *
 *   1. **读取口径**：名字不存在返回默认值；类型不符**抛异常**（这是文档承诺的「响亮失败」，
 *      而静默退回默认值会让拼写错误变成「用默认值跑了一整天」）；
 *   2. **类型映射不隐式放宽**：`int64_t` 不接受 `10.0`（静默截断 `10.5 → 10` 会把配置错误变成
 *      一笔错误的交易量）、`double` 接受任意数值、`bool`/`string` 各自只认自己；
 *   3. **值 token 的类型推断与渲染往返**：`param set x 10` 之后 `param get` 打印 `10`，再
 *      `param set x 10` 得到同一个值——命令没有引号语法，类型信息只能靠 token 形状推断，
 *      往返一致是这条协议能用的前提。
 *
 * 服务端集成面（`param set` 真的写盘、真的通知 B）由 `bar_strategy_framework_test.cpp` 覆盖
 * （`ParamSetNotifiesLiveInstancesAndTheyReread` /
 * `ParamUpdateIsDeferredUntilFirstCallbackAndCoalesced`）， 两者互补：这里锁语义细节，那里锁接线。
 */
#include <gtest/gtest.h>

#include <dztrader/bar_strategy/params.h>
#include <dztrader/json.h>

#include <cstdint>
#include <stdexcept>
#include <string>

namespace {

using dztrader::ParamStore;
using dztrader::params_internal::format_param_value;
using dztrader::params_internal::json_type_name;
using dztrader::params_internal::param_value_from_token;

// ============================================================
// 读取口径：缺省 vs 类型不符
// ============================================================

/// 名字不存在 → 返回给定默认值（策略不必为可选参数写存在性判断）
TEST(BarStrategyParamsTest, MissingNameReturnsFallback) {
    const ParamStore store(nlohmann::json{{"fast", 10}});
    EXPECT_EQ(store.get<std::int64_t>("absent", 42), 42);
    EXPECT_DOUBLE_EQ(store.get<double>("absent", 1.5), 1.5);
    EXPECT_TRUE(store.get<bool>("absent", true));
    EXPECT_EQ(store.get<std::string>("absent", "fallback"), "fallback");
}

/**
 * **名字存在但类型不符 → 抛 `std::invalid_argument`**（组件文档 §9「读取口径」）。
 *
 * 四条路径各一格：整数位置给小数、数字位置给字符串、布尔位置给整数、字符串位置给数字。
 * 断言错误消息带 `param type mismatch` 与 `name=` —— 它是操作员定位「哪个参数写错了」的唯一
 * 线索；只断言"抛了异常"会让故意抛别的异常也通过。
 */
TEST(BarStrategyParamsTest, TypeMismatchThrowsWithNameAndTypes) {
    const ParamStore store(
        nlohmann::json{{"ratio", 1.5}, {"label", "fast"}, {"flag", 1}, {"count", 7}});

    struct Case {
        const char* name;
        const char* expected_type;  // 错误消息里的 expected=
    };
    const Case cases[] = {
        {"ratio", "integer"},  // 1.5 不是整数（不静默截断）
        {"label", "number"},   // "fast" 不是数值
        {"flag", "boolean"},   // 1 不是布尔
        {"count", "string"},   // 7 不是字符串
    };

    for (const Case& item : cases) {
        try {
            if (std::string_view(item.name) == "ratio") {
                (void)store.get<std::int64_t>(item.name, 0);
            } else if (std::string_view(item.name) == "label") {
                (void)store.get<double>(item.name, 0.0);
            } else if (std::string_view(item.name) == "flag") {
                (void)store.get<bool>(item.name, false);
            } else {
                (void)store.get<std::string>(item.name, "");
            }
            FAIL() << "expected std::invalid_argument for name=" << item.name;
        } catch (const std::invalid_argument& error) {
            const std::string message = error.what();
            EXPECT_NE(message.find("param type mismatch"), std::string::npos) << message;
            EXPECT_NE(message.find(std::format("name={}", item.name)), std::string::npos)
                << message;
            EXPECT_NE(message.find(std::format("expected={}", item.expected_type)),
                      std::string::npos)
                << message;
        }
    }
}

/**
 * **整数参数不接受浮点表示**（静默截断是比其他类型错误更危险的一类）。
 *
 * `volume: 10.5` 若被截成 10，一笔「多 0.5 手」的配置错误会变成一笔照常执行的交易。
 */
TEST(BarStrategyParamsTest, IntegerParameterRejectsFloatingPoint) {
    const ParamStore store(nlohmann::json{{"fast", 10.0}});
    EXPECT_THROW((void)store.get<std::int64_t>("fast", 0), std::invalid_argument);

    // 但 double 参数接受整数（JSON 的 `10` 是合法数值）
    const ParamStore mixed(nlohmann::json{{"slow", 30}});
    EXPECT_DOUBLE_EQ(mixed.get<double>("slow", 0.0), 30.0);
}

// ============================================================
// 写入与遍历
// ============================================================

/// `set` 覆盖已有值、新增缺失值；`contains` 反映两者
TEST(BarStrategyParamsTest, SetOverwritesAndInserts) {
    ParamStore store(nlohmann::json{{"fast", 10}});
    EXPECT_TRUE(store.contains("fast"));
    EXPECT_FALSE(store.contains("slow"));

    store.set("fast", 20);
    store.set("slow", 30);
    EXPECT_EQ(store.get<std::int64_t>("fast", 0), 20);
    EXPECT_EQ(store.get<std::int64_t>("slow", 0), 30);
    EXPECT_EQ(store.size(), 2U);
}

/// `names()` 升序（`param list` 的稳定输出顺序来自这里）
TEST(BarStrategyParamsTest, NamesAreSorted) {
    const ParamStore store(nlohmann::json{{"slow", 30}, {"fast", 10}, {"volume", 1}});
    const std::vector<std::string> names = store.names();
    ASSERT_EQ(names.size(), 3U);
    EXPECT_EQ(names[0], "fast");
    EXPECT_EQ(names[1], "slow");
    EXPECT_EQ(names[2], "volume");
}

/// 空仓库（配置 `params` 省略时的形态）大小 0、无名字、读取一律默认值
TEST(BarStrategyParamsTest, EmptyStore) {
    const ParamStore store;
    EXPECT_EQ(store.size(), 0U);
    EXPECT_TRUE(store.names().empty());
    EXPECT_FALSE(store.contains("anything"));
    EXPECT_EQ(store.get<std::int64_t>("anything", 7), 7);
}

// ============================================================
// 值 token 的类型推断
// ============================================================

/**
 * **token → JSON 标量的推断规则**（组件文档 §7「字符串协议」+ §9「值 token 类型推断」）。
 *
 * 「能被 JSON 解析成**标量**就按该类型存，否则按字符串存」。这里覆盖四类合法标量、以及
 * 三类**不作为参数值接受**的合法 JSON（`null` / 数组 / 对象——框架只运输标量），还有三类
 * 不是合法 JSON 数字的写法（前导零 / 正号 / 裸小数点），它们必须落成字符串而不是被静默改成
 * 数值。
 */
TEST(BarStrategyParamsTest, ValueTokenInfersScalarTypes) {
    // 数值
    EXPECT_TRUE(param_value_from_token("10").is_number_integer());
    EXPECT_EQ(param_value_from_token("10").get<std::int64_t>(), 10);
    EXPECT_TRUE(param_value_from_token("1.5").is_number_float());
    EXPECT_DOUBLE_EQ(param_value_from_token("1.5").get<double>(), 1.5);
    EXPECT_TRUE(param_value_from_token("-3").is_number_integer());
    EXPECT_DOUBLE_EQ(param_value_from_token("1e3").get<double>(), 1000.0);

    // 布尔
    EXPECT_TRUE(param_value_from_token("true").is_boolean());
    EXPECT_TRUE(param_value_from_token("true").get<bool>());
    EXPECT_TRUE(param_value_from_token("false").is_boolean());
    EXPECT_FALSE(param_value_from_token("false").get<bool>());

    // 字符串（JSON 解析失败 → 按字符串存）
    EXPECT_TRUE(param_value_from_token("rb2601").is_string());
    EXPECT_EQ(param_value_from_token("rb2601").get<std::string>(), "rb2601");

    // 带引号的 token 会被 JSON 解析成字符串，且引号被吃掉——这是可接受的（命令层没有引号
    // 语法，操作员不会这么写；万一写了也得到语义合理的字符串）
    EXPECT_TRUE(param_value_from_token("\"quoted\"").is_string());
    EXPECT_EQ(param_value_from_token("\"quoted\"").get<std::string>(), "quoted");

    // 合法 JSON 但不是标量 → 不作为参数值，按字符串存
    EXPECT_TRUE(param_value_from_token("null").is_string());
    EXPECT_EQ(param_value_from_token("null").get<std::string>(), "null");
    EXPECT_TRUE(param_value_from_token("[1,2]").is_string());
    EXPECT_TRUE(param_value_from_token("{\"a\":1}").is_string());

    // 看起来像数字但不是合法 JSON 数字 → 字符串（不静默改数值）
    for (const char* token : {"007", "+1", ".5"}) {
        EXPECT_TRUE(param_value_from_token(token).is_string()) << "token=" << token;
        EXPECT_EQ(param_value_from_token(token).get<std::string>(), token);
    }
}

// ============================================================
// 渲染与往返一致
// ============================================================

/**
 * **`format_param_value` 的渲染口径**：字符串原样输出（不加引号——命令层没有引号语法，
 * 加引号会让人以为要按引号再输入一次），其余用 JSON dump。
 */
TEST(BarStrategyParamsTest, FormatParamValueRendering) {
    EXPECT_EQ(format_param_value(nlohmann::json(10)), "10");
    EXPECT_EQ(format_param_value(nlohmann::json(1.5)), "1.5");
    EXPECT_EQ(format_param_value(nlohmann::json(true)), "true");
    EXPECT_EQ(format_param_value(nlohmann::json("rb2601")), "rb2601");
    // 字符串里的数字不加引号，也不被当成 JSON 数字
    EXPECT_EQ(format_param_value(nlohmann::json("10")), "10");
    // 含等号的字符串同样原样输出（OUTPUT_UI 是给人看的，不需要 k=v 可解析）
    EXPECT_EQ(format_param_value(nlohmann::json("a=b")), "a=b");
}

/**
 * **往返一致**：`set(x, token)` → `format(get x)` → 再 `set(x, 该输出)` 得到同一个值。
 *
 * 这条性质是命令层「先 `param get` 看一眼、再 `param set` 改回去」能成立的前提。往返只需要
 * 对**能经 token 表达的类型**成立（四种标量），字符串是最容易破的那一类。
 */
TEST(BarStrategyParamsTest, TokenRoundTripIsStable) {
    for (const char* token : {"10", "-3", "1.5", "true", "false", "rb2601", "a=b", "007"}) {
        const nlohmann::json first = param_value_from_token(token);
        const std::string rendered = format_param_value(first);
        const nlohmann::json second = param_value_from_token(rendered);
        EXPECT_EQ(first, second) << "round trip broken | token=" << token
                                 << " rendered=" << rendered;
    }
}

/// JSON 形态名（错误消息里的 `actual=`；与 `param_type_matches` 的四个分支一一对应）
TEST(BarStrategyParamsTest, JsonTypeNames) {
    EXPECT_EQ(json_type_name(nlohmann::json(1)), "integer");
    EXPECT_EQ(json_type_name(nlohmann::json(1.5)), "number");
    EXPECT_EQ(json_type_name(nlohmann::json(true)), "boolean");
    EXPECT_EQ(json_type_name(nlohmann::json("x")), "string");
    EXPECT_EQ(json_type_name(nlohmann::json(nullptr)), "null");
    EXPECT_EQ(json_type_name(nlohmann::json::array()), "array");
    EXPECT_EQ(json_type_name(nlohmann::json::object()), "object");
}

}  // namespace
