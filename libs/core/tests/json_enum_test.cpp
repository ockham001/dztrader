// DZ_JSON_ENUM 宏独立测试: 验证可复用性, 不依赖 MdConfigOp
#include <dztrader/core/json_enum.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace dztrader::test {

// 测试用枚举: 与 MdConfigOp 完全无关, 证明宏对任意 enum class 可复用
enum class TestColor {
    Red,
    Green,
    Blue,
};

DZ_JSON_ENUM(TestColor)

// 嵌入到 NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT 结构体, 验证 ADL 模板实例化正确
struct TestPalette {
    TestColor primary = TestColor::Red;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(TestPalette, primary)
};

// --- DZ_JSON_ENUM 宏测试 ---

TEST(JsonEnumTest, ValidEnumRoundTrip) {
    for (auto c : {TestColor::Red, TestColor::Green, TestColor::Blue}) {
        nlohmann::json j = c;
        EXPECT_EQ(j.get<TestColor>(), c);
    }
}

TEST(JsonEnumTest, SerializesAsString) {
    nlohmann::json j = TestColor::Red;
    EXPECT_TRUE(j.is_string());
    EXPECT_EQ(j.get<std::string>(), "Red");
}

TEST(JsonEnumTest, InvalidEnumStringThrows) {
    nlohmann::json j = "Yellow";
    EXPECT_THROW(j.get<TestColor>(), nlohmann::json::exception);
}

TEST(JsonEnumTest, NonStringJsonThrows) {
    // 非字符串 JSON (数字) 应由 nlohmann 内部 type_error 抛异常 (派生自 exception)
    nlohmann::json j = 42;
    EXPECT_THROW(j.get<TestColor>(), nlohmann::json::exception);
}

TEST(JsonEnumTest, EmbeddedInStructRoundTrip) {
    TestPalette p;
    p.primary = TestColor::Blue;
    nlohmann::json j = p;
    EXPECT_EQ(j["primary"], "Blue");

    auto loaded = j.get<TestPalette>();
    EXPECT_EQ(loaded.primary, TestColor::Blue);
}

TEST(JsonEnumTest, EmbeddedInStructOmittedFieldUsesDefault) {
    // WITH_DEFAULT: 省略 primary 字段时用结构体默认值 (Red)
    nlohmann::json j = nlohmann::json::object();
    auto loaded = j.get<TestPalette>();
    EXPECT_EQ(loaded.primary, TestColor::Red);
}

}  // namespace dztrader::test
