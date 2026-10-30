/**
 * @file system_info_test.cpp
 * @brief `/api/system/info` 响应体（契约 rest §2.1）：字段齐全且版本与 version.h 同源
 */

#include <dztrader/core/this_process.h>
#include <dztrader/version.h>
#include <gtest/gtest.h>
#include "system_info.h"

namespace {

TEST(SystemInfoTest, BodyCarriesProcessNameAndVersion) {
    const auto body = dztrader::webui::system_info_body();

    // process_name：exe 文件名不含扩展名（前端据此比对 logger 名禁用自我 tail）
    ASSERT_TRUE(body.contains("process_name"));
    EXPECT_EQ(body.at("process_name").get<std::string>(), dztrader::this_process::exe_stem());

    // version：与 version.h 生成物同源，不存在第二份版本串
    ASSERT_TRUE(body.contains("version"));
    EXPECT_EQ(body.at("version").get<std::string>(), std::string(DZ_VERSION_STRING));
    EXPECT_FALSE(body.at("version").get<std::string>().empty());
}

TEST(SystemInfoTest, VersionIsSemVerShaped) {
    // 契约要求的形态是 SemVer（MAJOR.MINOR.PATCH，可带 -prerelease 后缀）：
    // 这里只断言主版本段可解析，避免把版本号硬编码进测试（版本升级不该让门禁变红）。
    const std::string version =
        dztrader::webui::system_info_body().at("version").get<std::string>();
    const auto first_dot = version.find('.');
    ASSERT_NE(first_dot, std::string::npos) << "version not semver: " << version;
    EXPECT_NE(version.find('.', first_dot + 1), std::string::npos)
        << "version not semver: " << version;
    EXPECT_EQ(DZ_VERSION_MAJOR, std::stoi(version.substr(0, first_dot)));
}

/// 免认证端点不得泄露环境/主机信息：字段集合固定为这两项（新增字段须同步契约并改测试）
TEST(SystemInfoTest, BodyExposesOnlyContractFields) {
    const auto body = dztrader::webui::system_info_body();
    std::vector<std::string> keys;
    for (const auto& [key, value] : body.items()) {
        (void)value;
        keys.push_back(key);
    }
    EXPECT_EQ(keys, (std::vector<std::string>{"process_name", "version"}));
}

}  // namespace
