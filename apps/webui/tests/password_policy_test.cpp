/**
 * @file password_policy_test.cpp
 * @brief 口令强度策略（契约 rest §1「密码策略」）：服务端强制的最小长度
 */

#include <gtest/gtest.h>

#include <string>

#include "password_policy.h"

namespace dztrader::webui {
namespace {

TEST(PasswordPolicyTest, MinLengthIsTwelve) {
    // 契约 rest §1 的固定值；改动即契约变更（前端 composables/passwordPolicy.ts 同值）
    EXPECT_EQ(kMinPasswordLength, 12u);
}

TEST(PasswordPolicyTest, AcceptsExactlyAtMinLength) {
    EXPECT_TRUE(is_password_acceptable(std::string(kMinPasswordLength, 'a')));
    EXPECT_TRUE(is_password_acceptable("123456789012"));
}

TEST(PasswordPolicyTest, RejectsShorterThanMinLength) {
    EXPECT_FALSE(is_password_acceptable(std::string(kMinPasswordLength - 1, 'a')));
    EXPECT_FALSE(is_password_acceptable("password123"));  // 11 位
    EXPECT_FALSE(is_password_acceptable(""));
    // 系统默认口令本身不满足策略（因此管理员改密后无法再设回它）
    EXPECT_FALSE(is_password_acceptable("88888888"));
}

TEST(PasswordPolicyTest, LengthIsTheOnlyRule) {
    // 纯长度策略：不强制大小写/数字/符号（维护者 2026-10-31 决策）
    EXPECT_TRUE(is_password_acceptable("aaaaaaaaaaaa"));
    EXPECT_TRUE(is_password_acceptable("            "));
}

}  // namespace
}  // namespace dztrader::webui
