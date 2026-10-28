/**
 * @file public_header_coverage_test.cpp
 * @brief 公开 SDK 头必须有「只链 `strategy_api_headers`」的编译单元覆盖（#154/#157 复用）
 *
 * 为什么需要本探针：Windows CI 曾因 `bar_strategy/config_file.h` 里的弃用 `_wfopen`
 * 变红（C4996 → /WX → C2220）。该头在 Linux 上**完全正确**（`#ifdef` 走 `std::fopen`），
 * 所以 Linux 门禁永远看不到；它被抓到纯属巧合——有人恰好把一个包含它的头加进了
 * `json_sdk_probe.cpp`，而那个探针只链 `strategy_api_headers`（没有内部库顺带给出的
 * `_CRT_SECURE_NO_WARNINGS`）。换句话说：**校验确实发生了，但没人能说清它为什么有效、
 * 也没人能发现它什么时候悄悄失效了。**
 *
 * 本探针把那份隐式覆盖变成显式契约：`libs/strategy_api/include/` 下每个公开头都必须在
 * `PUBLIC_HEADER_PROBES` 里被某个「只链 strategy_api_headers」的 TU 直接包含。新增公开头
 * 却忘了纳入探针 → 本测试失败，而不是等到某天的 Windows CI。
 *
 * 实现方式是**文本扫描**而非包含：包含全部头会制造一个巨型 TU（编译时间与失败定位都变差），
 * 且本探针要断言的正是「某个 TU 包含了它」这件事本身，扫源码是最直接的证据。
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <string>
#include <vector>

namespace {

/// 公开头目录（相对仓库根）
constexpr const char* kPublicIncludeDir = "libs/strategy_api/include";

/**
 * @brief 「只链 `strategy_api_headers`」的探针 TU（见 libs/strategy_api/CMakeLists.txt）
 *
 * 只有这两个目标不带任何内部/第三方库：它们的编译命令里没有 `_CRT_SECURE_NO_WARNINGS`
 * 之类由内部 target 传播的宏，因此等价于「下游策略作者自己的工程」。新增这类探针时
 * 一并加进来。
 */
constexpr const char* kSdkOnlyProbes[] = {
    "libs/strategy_api/tests/json_sdk_probe.cpp",
    "libs/strategy_api/tests/aggregate_header_probe.cpp",
};

/// 非公开头：内部实现面与第三方 vendored 代码，不进公开承诺面
constexpr const char* kExcludedPaths[] = {
    "third_party/",
};

/// @brief 仓库根（测试从 build 目录运行，向上找 .git）
std::filesystem::path repo_root() {
    std::filesystem::path dir = std::filesystem::current_path();
    for (;;) {
        if (std::filesystem::exists(dir / ".git")) {
            return dir;
        }
        if (!dir.has_parent_path() || dir.parent_path() == dir) {
            return std::filesystem::current_path();
        }
        dir = dir.parent_path();
    }
}

/// @brief 收集全部公开头（相对仓库根的路径，正斜杠）
std::vector<std::string> public_headers(const std::filesystem::path& root) {
    std::vector<std::string> headers;
    const std::filesystem::path dir = root / kPublicIncludeDir;
    if (!std::filesystem::exists(dir)) {
        return headers;
    }
    for (const auto& entry : std::filesystem::recursive_directory_iterator(dir)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".h") {
            continue;
        }
        const std::string rel = std::filesystem::relative(entry.path(), root).generic_string();
        const bool excluded = std::ranges::any_of(kExcludedPaths, [&rel](const char* prefix) {
            return rel.find(prefix) != std::string::npos;
        });
        if (!excluded) {
            headers.push_back(rel);
        }
    }
    std::ranges::sort(headers);
    return headers;
}

/**
 * @brief 从 `libs/strategy_api/include/` 推导某个公开头的可包含形式
 *
 * `libs/strategy_api/include/dztrader/bar/types.h` → `<dztrader/bar/types.h>`；
 * 聚合头 `libs/strategy_api/include/dztrader.h` → `<dztrader.h>`。
 */
std::string include_name(const std::string& header_rel) {
    const std::string prefix = std::string(kPublicIncludeDir) + "/";
    return header_rel.substr(prefix.size());
}

/// @brief 探针 TU 是否直接包含了该头
bool probe_includes(const std::filesystem::path& root,
                    const char* probe,
                    const std::string& include) {
    std::ifstream stream(root / probe);
    if (!stream) {
        return false;
    }
    const std::string text((std::istreambuf_iterator<char>(stream)),
                           std::istreambuf_iterator<char>());
    const std::string quoted = "#include <" + include + ">";
    return text.find(quoted) != std::string::npos;
}

}  // namespace

/**
 * @brief 每个公开头都被某个 SDK-only 探针直接包含
 *
 * 失败信息给出「哪个头没被覆盖」与「应该加进哪个探针」，而不是让人自己去猜。
 */
TEST(PublicHeaderCoverageTest, EveryPublicHeaderIsCompiledByAnSdkOnlyProbe) {
    const std::filesystem::path root = repo_root();
    const std::vector<std::string> headers = public_headers(root);
    ASSERT_FALSE(headers.empty()) << "public header dir not found under " << root;

    std::vector<std::string> uncovered;
    for (const std::string& header : headers) {
        const std::string include = include_name(header);
        const bool covered = std::ranges::any_of(kSdkOnlyProbes, [&](const char* probe) {
            return probe_includes(root, probe, include);
        });
        if (!covered) {
            uncovered.push_back(header + "  (add `#include <" + include +
                                ">` to one of the SDK-only probes)");
        }
    }

    EXPECT_TRUE(uncovered.empty())
        << "public SDK headers without an SDK-only compile unit (these are never built the "
           "way a downstream strategy builds them, so MSVC-only breakage in them can only "
           "surface in CI):\n  "
        << [&uncovered] {
               std::string joined;
               for (const std::string& line : uncovered) {
                   joined += line + "\n  ";
               }
               return joined;
           }();
}
