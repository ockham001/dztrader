/**
 * @file system_info_test.cpp
 * @brief `/api/system/info` 响应体（契约 rest §2.1）：字段齐全且版本与 version.h 同源
 */

#include <dztrader/core/this_process.h>
#include <dztrader/version.h>
#include <gtest/gtest.h>
#include <string>
#include <vector>
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
    // 只断言形态，不断言具体数值——版本升级不该让门禁变红。
    // （与 DZ_VERSION_STRING 同源已由 BodyCarriesProcessNameAndVersion 覆盖，此处不重复断言
    // `DZ_VERSION_MAJOR == version 首段`：两者同出 version.h，改坏响应体也不会让那条变红。）
    const std::string version =
        dztrader::webui::system_info_body().at("version").get<std::string>();
    // 去掉可选的 -prerelease 后缀，剩下必须是三段纯数字：CMake 变量未替换（如残留
    // "${VERSION_MAJOR}"）时这里会直接暴露成非数字段。
    const std::string core = version.substr(0, version.find('-'));
    std::vector<std::string> segments;
    std::size_t begin = 0;
    while (true) {
        const auto dot = core.find('.', begin);
        segments.push_back(
            core.substr(begin, dot == std::string::npos ? std::string::npos : dot - begin));
        if (dot == std::string::npos) break;
        begin = dot + 1;
    }
    EXPECT_EQ(segments.size(), 3u) << "version not semver: " << version;
    for (const auto& segment : segments) {
        EXPECT_FALSE(segment.empty()) << "version not semver: " << version;
        EXPECT_EQ(segment.find_first_not_of("0123456789"), std::string::npos)
            << "non-numeric version segment, not semver: " << version;
    }
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
