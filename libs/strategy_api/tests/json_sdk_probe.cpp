/**
 * @file json_sdk_probe.cpp
 * @brief 「仅凭 SDK include 目录可编译」双模探针（issue #154 验收项，ADR 0026 决策 6）
 *
 * 本文件被编译两次：
 *   - json_sdk_probe_builtin：默认模式，用 SDK 内置单头；该 target **只链接
 *     `strategy_api_headers`**，即编译命令里只有 SDK 的 include 目录、不带任何
 *     Conan / 系统第三方 include——这正是「外部策略作者不装任何第三方也能编译」的口径。
 *   - json_sdk_probe_external：定义 DZTRADER_JSON_EXTERNAL，用外部
 *     `<nlohmann/json.hpp>`；该 target 额外链接 nlohmann_json::nlohmann_json
 *     提供外部头路径，模拟策略作者自带版本/来源。
 *
 * 编译成功即证明「把 SDK include 目录加进头文件路径就能解析 JSON」；运行本程序再证明
 * 取到的是**能真正工作的** nlohmann（解析、类型化取值、序列化、异常），而不是只过了
 * 预处理的空壳。
 *
 * 同一次编译顺带覆盖 BarStrategy 配置层（#156，header-only）：「仅凭 SDK include 目录」
 * 必须能编译并运行配置解析/默认值/有效配置日志，证明配置层没有偷偷依赖 Conan 或内部库
 * （它只经 `dztrader/json.h` 取 JSON，并包含 SDK 自有的 date_time / series 头）。
 *
 * 模式断言不是形式主义：`DZTRADER_JSON_EXTERNAL` 拼错或没传到编译命令时，外部探针会
 * 静默回退内置模式，双模检查就退化成「同一个头编译两遍」的恒真测试（审查清单 §2.2）。
 * `DZ_JSON_PROBE_EXPECT_EXTERNAL` 由 CMake 按 target 注入，属于测试专用宏，不进公开面。
 */
#include <dztrader/bar_strategy/config.h>
#include <dztrader/json.h>

#include <cstdio>
#include <string>

namespace {

/** @brief 打印诊断行并返回失败码（探针面向开发者，不是策略可见输出） */
int fail(const char* case_name) {
    std::fprintf(stderr, "json sdk probe fail | case=%s\n", case_name);
    return 1;
}

}  // namespace

int main() {
    // 模式断言：编译命令里的 DZTRADER_JSON_EXTERNAL 必须与本 target 的期望一致
#if defined(DZ_JSON_PROBE_EXPECT_EXTERNAL)
#ifndef DZTRADER_JSON_EXTERNAL
    return fail("external_macro_not_applied");
#endif
#else
#ifdef DZTRADER_JSON_EXTERNAL
    return fail("builtin_target_got_external_macro");
#endif
#endif

#ifdef DZTRADER_JSON_EXTERNAL
    constexpr const char* kMode = "external";
#else
    constexpr const char* kMode = "builtin";
#endif

    // 版本宏存在 = 取到的确实是 nlohmann/json（任何来源）
#if !defined(NLOHMANN_JSON_VERSION_MAJOR) || !defined(NLOHMANN_JSON_VERSION_MINOR) || \
    !defined(NLOHMANN_JSON_VERSION_PATCH)
    return fail("missing_version_macros");
#endif

    // 解析 + 类型化取值（框架配置层实际使用的 API 子集）
    const auto config = nlohmann::json::parse(R"({"schema_version":1,"period_sec":60,
                                                 "instruments":["rb2601","rb2605"]})");
    if (config.at("schema_version").get<int>() != 1) {
        return fail("parse_int");
    }
    if (config.at("period_sec").get<int>() != 60) {
        return fail("parse_nested_int");
    }
    const auto& instruments = config.at("instruments");
    if (instruments.size() != 2U || instruments.at(0).get<std::string>() != "rb2601") {
        return fail("parse_array");
    }

    // 序列化往返（配置原子写回用的 API 子集）
    const std::string dumped = config.dump();
    if (nlohmann::json::parse(dumped) != config) {
        return fail("dump_roundtrip");
    }

    // 非法输入必须抛异常：配置层靠它做「绝不静默修正」
    bool threw = false;
    try {
        const auto parsed = nlohmann::json::parse("{");
        static_cast<void>(parsed);  // 解析成功即反例：走到这里说明非法输入被接受
    } catch (const nlohmann::json::parse_error&) {
        threw = true;
    }
    if (!threw) {
        return fail("malformed_json_not_rejected");
    }

    // BarStrategy 配置层（#156）：默认配置 → 解析 → 自动值推导 → 有效配置日志。
    // 显式注入 today，探针不依赖墙钟。
    auto config_root = dztrader::make_minimal_default_config();
    config_root["instruments"] = nlohmann::json::array({"rb2601"});
    const auto effective =
        dztrader::parse_and_resolve_config(config_root, dztrader::Date{2026, 10, 28});
    const std::string effective_line = dztrader::format_effective_config(effective);
    if (effective.max_lookback != 1440 ||
        effective.warmup_source != dztrader::ConfigValueSource::Default) {
        return fail("bar_strategy_config_defaults");
    }
    if (effective_line.find("max_lookback_source=auto") == std::string::npos) {
        return fail("bar_strategy_config_log");
    }
    if (dztrader::format_effective_accounts({}) != "accounts effective | -") {
        return fail("bar_strategy_config_accounts_line");
    }

    std::printf("json sdk probe ok | mode=%s version=%d.%d.%d\n", kMode,
                NLOHMANN_JSON_VERSION_MAJOR, NLOHMANN_JSON_VERSION_MINOR,
                NLOHMANN_JSON_VERSION_PATCH);
    return 0;
}
