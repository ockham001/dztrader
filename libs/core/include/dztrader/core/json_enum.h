#ifndef DZTRADER_CORE_JSON_ENUM_H_
#define DZTRADER_CORE_JSON_ENUM_H_

#include <nlohmann/json.hpp>
#include <magic_enum/magic_enum.hpp>

/// 基于 magic_enum 的 nlohmann 枚举序列化注册宏。
/// 用法: 在 enum class 定义之后 (同命名空间内) 调用 DZ_JSON_ENUM(EnumType)。
/// 自动用 magic_enum::enum_name 派生字符串映射, 反序列化失败抛 nlohmann::json::exception。
///
/// 适用场景: enum class 序列化为字符串 (如 "AddBroker")。
/// 不适用: 需要整数序列化的枚举 (如线协议已固定位 0/1/2), 此类应继续使用 NLOHMANN_JSON_SERIALIZE_ENUM。
///
/// 相比 NLOHMANN_JSON_SERIALIZE_ENUM 的优势:
///   - 无需手写字符串映射, 新增枚举值零维护
///   - 字符串与 magic_enum::enum_name() 输出保证一致 (日志/JSON 统一)
///   - 无效值抛 out_of_range (403) 异常带明确错误消息, 而非静默返回首项
///
/// 生成函数为模板 (受 is_basic_json SFINAE 约束), 兼容任意 basic_json 特化,
/// 可安全用于 NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT 生成的结构体内部字段。
#define DZ_JSON_ENUM(EnumType)                                                         \
    template<typename BasicJsonType,                                                   \
             nlohmann::detail::enable_if_t<                                            \
                 nlohmann::detail::is_basic_json<BasicJsonType>::value, int> = 0>      \
    inline void to_json(BasicJsonType& j, EnumType v) {                                \
        j = std::string(magic_enum::enum_name(v));                                     \
    }                                                                                  \
    template<typename BasicJsonType,                                                   \
             nlohmann::detail::enable_if_t<                                            \
                 nlohmann::detail::is_basic_json<BasicJsonType>::value, int> = 0>      \
    inline void from_json(const BasicJsonType& j, EnumType& v) {                       \
        auto parsed = magic_enum::enum_cast<EnumType>(j.template get<std::string>());  \
        if (!parsed.has_value()) {                                                     \
            throw nlohmann::json::out_of_range::create(                                \
                403, "invalid " #EnumType ": " + j.template get<std::string>(),        \
                nullptr);                                                              \
        }                                                                              \
        v = parsed.value();                                                            \
    }

#endif  // DZTRADER_CORE_JSON_ENUM_H_
