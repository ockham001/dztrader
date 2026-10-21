// 06-sysmon 平台基线真机 oracle 测试：当前机器必须满足启动期平台基线（契约 sysmon）。
#include <dztrader/platform/platform_requirements.h>

#include <gtest/gtest.h>

#include <string>

namespace dztrader::test {

TEST(PlatformRequirementsTest, CurrentMachineMeetsBaseline) {
    const auto error = dztrader::platform::check_platform_baseline();
    ASSERT_FALSE(error.has_value()) << (error.has_value() ? *error : std::string{});
}

}  // namespace dztrader::test
