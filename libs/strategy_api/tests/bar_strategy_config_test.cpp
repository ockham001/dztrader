/**
 * @file bar_strategy_config_test.cpp
 * @brief BarStrategy 配置层的解析 / 校验 / 自动值 / 有效配置日志（issue #156 AC1–AC5，组件文档 §7）
 *
 * 断言只落在配置层的公开契约上：`config_path` / `make_minimal_default_config` /
 * `parse_config` / `parse_and_resolve_config` / `format_effective_config` /
 * `format_effective_accounts` 的返回值、异常类型与消息内容（字段名 / 当前值 / 合法区间）。
 * 文件读写与原子写回在 bar_strategy_config_file_test.cpp。
 *
 * 确定性：所有涉及 `warmup.from` 的用例显式注入 `today`（2026-10-28），不依赖墙钟。
 */
#include <dztrader/bar_strategy/config.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

namespace {

using dztrader::BarStrategyConfig;
using dztrader::ConfigValueSource;
using dztrader::EffectiveConfig;
using dztrader::ScaleRounding;

/// 测试固定的「今天」：warmup.from 的上界随用例注入，不依赖真实墙钟
constexpr dztrader::Date kToday{2026, 10, 28};

/// 最小可运行配置：默认配置 + 一个品种（其余全走默认/自动值）
nlohmann::json minimal_run_config() {
    nlohmann::json root = dztrader::make_minimal_default_config();
    root["instruments"] = nlohmann::json::array({"rb2601"});
    return root;
}

/// 组件文档 §7 的完整示例配置（accounts / scaling / params 都显式）
nlohmann::json full_example_config() {
    return nlohmann::json::parse(R"({
        "schema_version": 1,
        "period_sec": 60,
        "instruments": ["rb2601", "rb2605"],
        "accounts": [
            {"account_id": "CTP001", "scale": 1.0},
            {"account_id": "CTP002", "scale": 0.5}
        ],
        "max_scale": 1.0,
        "scale_rounding": "nearest",
        "scale_min_one": false,
        "params": {"fast": 10, "slow": 30, "volume": 1}
    })");
}

void expect_invalid(const std::function<void()>& call,
                    std::initializer_list<std::string_view> fragments) {
    std::string message;
    try {
        call();
    } catch (const std::invalid_argument& e) {
        message = e.what();
    }
    if (message.empty()) {
        FAIL() << "expected std::invalid_argument";
        return;
    }
    for (const std::string_view fragment : fragments) {
        EXPECT_NE(message.find(fragment), std::string::npos) << "message: " << message;
    }
}

/// 解析+校验整条路径上应当报错：断言异常消息包含全部片段（字段名 / 当前值 / 合法区间）
void expect_invalid_json(const nlohmann::json& root,
                         std::initializer_list<std::string_view> fragments) {
    expect_invalid([&root] { (void)dztrader::parse_and_resolve_config(root, kToday); }, fragments);
}

/// 直接对落盘结构做语义校验时应报错（用于 hand-built 配置的防御性分支）
void expect_invalid_config(const BarStrategyConfig& config,
                           std::initializer_list<std::string_view> fragments) {
    expect_invalid([&config] { (void)dztrader::resolve_effective_config(config, kToday); },
                   fragments);
}

// ============================================================
// 配置路径：$dz_strategy_home()/<strategy_id>.json
// ============================================================

TEST(ConfigPathTest, JoinsHomeAndExecutableStem) {
    const std::filesystem::path home = std::filesystem::path{"dz"} / "home";
    EXPECT_EQ(dztrader::config_path(home, "stg_ma"),
              std::filesystem::path{"dz"} / "home" / "stg_ma.json");
}

TEST(ConfigPathTest, AcceptsTrailingSeparatorFromStrategyHomeContract) {
    // dz_strategy_home() 契约：目录以分隔符结尾。带与不带结尾都必须得到同一路径。
    const std::filesystem::path with_separator = std::filesystem::path{"dz"} / "home" / "";
    EXPECT_EQ(dztrader::config_path(with_separator, "stg_ma"),
              std::filesystem::path{"dz"} / "home" / "stg_ma.json");
}

TEST(ConfigPathTest, KeepsDotsInExecutableStem) {
    EXPECT_EQ(dztrader::config_path(std::filesystem::path{"dz"}, "stg_a.b").filename().string(),
              "stg_a.b.json");
}

TEST(ConfigPathTest, RejectsEmptyHome) {
    expect_invalid([] { (void)dztrader::config_path(std::filesystem::path{}, "stg_ma"); },
                   {"field=strategy_home", "value=\"\"", "non-empty directory"});
}

TEST(ConfigPathTest, RejectsEmptyStrategyId) {
    expect_invalid([] { (void)dztrader::config_path(std::filesystem::path{"dz"}, ""); },
                   {"field=strategy_id", "value=\"\"", "non-empty executable stem"});
}

TEST(ConfigPathTest, RejectsPathSeparatorInStrategyId) {
    // exe stem 不可能含分隔符；显式拒绝，防止越出策略目录读写任意路径
    expect_invalid([] { (void)dztrader::config_path(std::filesystem::path{"dz"}, "a/b"); },
                   {"field=strategy_id", "value=\"a/b\"", "no path separator"});
    expect_invalid([] { (void)dztrader::config_path(std::filesystem::path{"dz"}, "a\\b"); },
                   {"field=strategy_id", "value=\"a\\\\b\"", "no path separator"});
}

// ============================================================
// 最小默认配置（缺失时生成）
// ============================================================

TEST(MinimalDefaultConfigTest, HasOnlyTheFiveRequiredKeys) {
    const nlohmann::json expected = {{"schema_version", 1},
                                     {"period_sec", 60},
                                     {"instruments", nlohmann::json::array()},
                                     {"accounts", nlohmann::json::array()},
                                     {"params", nlohmann::json::object()}};
    EXPECT_EQ(dztrader::make_minimal_default_config(), expected);
}

TEST(MinimalDefaultConfigTest, OmitsEveryOptionalField) {
    const nlohmann::json root = dztrader::make_minimal_default_config();
    for (const char* optional_field : {"max_scale", "scale_rounding", "scale_min_one",
                                       "max_lookback", "reserve_bars", "warmup"}) {
        EXPECT_FALSE(root.contains(optional_field)) << optional_field;
    }
}

TEST(MinimalDefaultConfigTest, IsIntentionallyNotRunnable) {
    // 生成的默认配置必须仍过不了加载校验（空品种 → 报错退出），否则框架会拿它静默启动
    expect_invalid_json(dztrader::make_minimal_default_config(),
                        {"field=instruments", "value=[]", "non-empty array"});
}

// ============================================================
// 结构 / 类型层解析错误（parse_config）
// ============================================================

TEST(ParseConfigTest, RejectsNonObjectRoot) {
    expect_invalid_json(nlohmann::json::array(), {"field=config", "value=[]", "expected=object"});
}

TEST(ParseConfigTest, RequiresSchemaVersionPeriodAndInstruments) {
    expect_invalid_json(nlohmann::json::parse("{}"),
                        {"field=schema_version", "value=<missing>", "expected=required field"});

    nlohmann::json missing_period = minimal_run_config();
    missing_period.erase("period_sec");
    expect_invalid_json(missing_period, {"field=period_sec", "value=<missing>"});

    nlohmann::json missing_instruments = minimal_run_config();
    missing_instruments.erase("instruments");
    expect_invalid_json(missing_instruments, {"field=instruments", "value=<missing>"});
}

TEST(ParseConfigTest, RejectsWrongScalarTypes) {
    nlohmann::json root = minimal_run_config();

    root["schema_version"] = "1";
    expect_invalid_json(root, {"field=schema_version", "value=\"1\"", "expected=integer"});

    root = minimal_run_config();
    root["period_sec"] = 60.0;  // JSON 浮点不是整数（拒绝小数，不静默取整）
    expect_invalid_json(root, {"field=period_sec", "value=60.0", "expected=integer"});

    root = minimal_run_config();
    root["max_scale"] = true;
    expect_invalid_json(root, {"field=max_scale", "value=true", "expected=number"});

    root = minimal_run_config();
    root["scale_rounding"] = 1;
    expect_invalid_json(root, {"field=scale_rounding", "value=1", "expected=string"});

    root = minimal_run_config();
    root["scale_min_one"] = 1;
    expect_invalid_json(root, {"field=scale_min_one", "value=1", "expected=boolean"});

    root = minimal_run_config();
    root["max_lookback"] = 100.5;
    expect_invalid_json(root, {"field=max_lookback", "value=100.5", "expected=integer"});

    root = minimal_run_config();
    root["reserve_bars"] = "100";
    expect_invalid_json(root, {"field=reserve_bars", "value=\"100\"", "expected=integer"});

    root = minimal_run_config();
    root["params"] = nlohmann::json::array();
    expect_invalid_json(root, {"field=params", "value=[]", "expected=object"});

    root = minimal_run_config();
    root["accounts"] = nlohmann::json::object();
    expect_invalid_json(root, {"field=accounts", "value={}", "expected=array"});
}

TEST(ParseConfigTest, RejectsNullForKnownFields) {
    // 总则（《帧契约：general》§8）：JSON null 一律校验失败，配置沿用同一纪律
    nlohmann::json root = minimal_run_config();
    root["period_sec"] = nullptr;
    expect_invalid_json(root, {"field=period_sec", "value=null", "expected=integer"});

    root = minimal_run_config();
    root["accounts"] = nullptr;
    expect_invalid_json(root, {"field=accounts", "value=null", "expected=array"});
}

TEST(ParseConfigTest, RejectsNonStringInstrumentEntries) {
    nlohmann::json root = minimal_run_config();
    root["instruments"] = nlohmann::json::array({1, "rb2605"});
    expect_invalid_json(root, {"field=instruments[0]", "value=1", "expected=string"});
}

TEST(ParseConfigTest, RequiresAccountIdAndScaleWithFullFieldPaths) {
    nlohmann::json root = minimal_run_config();
    root["accounts"] = nlohmann::json::array({{{"scale", 1.0}}});
    expect_invalid_json(root, {"field=accounts[0].account_id", "value=<missing>"});

    root = minimal_run_config();
    root["accounts"] = nlohmann::json::array({{{"account_id", "CTP001"}}});
    expect_invalid_json(root, {"field=accounts[0].scale", "value=<missing>"});

    root = minimal_run_config();
    root["accounts"] = nlohmann::json::array({42});
    expect_invalid_json(root, {"field=accounts[0]", "value=42", "expected=object"});
}

TEST(ParseConfigTest, RejectsInvalidScaleRoundingValue) {
    nlohmann::json root = minimal_run_config();
    root["scale_rounding"] = "half";
    expect_invalid_json(root,
                        {"field=scale_rounding", "value=\"half\"", "range=[nearest,floor,ceil]"});
}

TEST(ParseConfigTest, RejectsOutOfInt32CapacityValues) {
    nlohmann::json root = minimal_run_config();
    root["max_lookback"] = std::numeric_limits<std::int64_t>::max();
    expect_invalid_json(root, {"field=max_lookback", "expected=int32 integer"});

    root = minimal_run_config();
    root["reserve_bars"] = 9'999'999'999LL;
    expect_invalid_json(root, {"field=reserve_bars", "expected=int32 integer"});
}

TEST(ParseConfigTest, ValidatesWarmupStructure) {
    nlohmann::json root = minimal_run_config();
    root["warmup"] = nlohmann::json::array();
    expect_invalid_json(root, {"field=warmup", "value=[]", "expected=object"});

    root = minimal_run_config();
    root["warmup"] = {{"bars", 5}, {"from", "2024-01-02"}};
    expect_invalid_json(root, {"field=warmup", "reason=mutually_exclusive"});

    root = minimal_run_config();
    root["warmup"] = nlohmann::json::object();
    expect_invalid_json(root, {"field=warmup", "value={}", "expected=bars or from"});

    root = minimal_run_config();
    root["warmup"] = {{"bars", 5.0}};
    expect_invalid_json(root, {"field=warmup.bars", "value=5.0", "expected=integer"});

    root = minimal_run_config();
    root["warmup"] = {{"from", 20240102}};
    expect_invalid_json(root, {"field=warmup.from", "value=20240102", "expected=string"});
}

TEST(ParseConfigTest, IgnoresUnknownTopLevelFieldsForForwardCompatibility) {
    // 顶层是唯一的前向兼容扩展面：未知顶层字段忽略（《帧契约：general》§8）
    nlohmann::json root = full_example_config();
    root["future_field"] = {{"nested", true}};
    const BarStrategyConfig config = dztrader::parse_config(root);
    EXPECT_EQ(config.instruments, std::vector<std::string>({"rb2601", "rb2605"}));
    EXPECT_EQ(config.accounts.size(), 2U);
}

TEST(ParseConfigTest, RejectsUnknownKeysInsideClosedInlineObjects) {
    // accounts 元素 / warmup 对象是闭集：未知键说明拼写错误，报错而不是静默吞掉
    // （写回时这两个对象由落盘结构重建，忽略未知键会造成静默数据丢失）
    nlohmann::json root = full_example_config();
    root["accounts"][0]["scales"] = 1.0;
    expect_invalid_json(root, {"field=accounts[0].scales", "value=1.0", "reason=unknown_field"});

    root = minimal_run_config();
    root["warmup"] = {{"bars", 20000}, {"future_key", 1}};
    expect_invalid_json(root, {"field=warmup.future_key", "reason=unknown_field"});
}

TEST(ParseConfigTest, ParsesFieldsWithoutSemanticValidation) {
    // 分层契约：parse 只看结构/类型；取值范围（如 period 45）留到 resolve 报错
    nlohmann::json root = minimal_run_config();
    root["period_sec"] = 45;
    root["max_lookback"] = 31;
    const BarStrategyConfig config = dztrader::parse_config(root);
    EXPECT_EQ(config.period_sec, 45);
    ASSERT_TRUE(config.max_lookback.has_value());
    EXPECT_EQ(*config.max_lookback, 31);
}

TEST(ParseConfigTest, ParsesScaleRoundingVariants) {
    for (const auto& [text, expected] :
         std::vector<std::pair<std::string, ScaleRounding>>{{"nearest", ScaleRounding::Nearest},
                                                            {"floor", ScaleRounding::Floor},
                                                            {"ceil", ScaleRounding::Ceil}}) {
        nlohmann::json root = minimal_run_config();
        root["scale_rounding"] = text;
        const EffectiveConfig effective = dztrader::parse_and_resolve_config(root, kToday);
        EXPECT_EQ(effective.scaling.rounding, expected) << text;
        EXPECT_EQ(dztrader::scale_rounding_name(expected), text);
    }
    EXPECT_EQ(dztrader::scale_rounding_name(ScaleRounding::Nearest), "nearest");
    EXPECT_EQ(dztrader::scale_rounding_name(ScaleRounding::Floor), "floor");
    EXPECT_EQ(dztrader::scale_rounding_name(ScaleRounding::Ceil), "ceil");
}

// ============================================================
// 语义校验（resolve_effective_config）
// ============================================================

TEST(ResolveConfigTest, RejectsUnknownSchemaVersion) {
    for (const int version : {0, 2}) {
        nlohmann::json root = minimal_run_config();
        root["schema_version"] = version;
        expect_invalid_json(root, {"field=schema_version", "expected=1"});
    }
}

TEST(ResolveConfigTest, RejectsPeriodOutsideV1Set) {
    nlohmann::json root = minimal_run_config();
    root["period_sec"] = 45;
    expect_invalid_json(root, {"field=period_sec", "value=45", "range=[60,180,300,900]"});

    root = minimal_run_config();
    root["period_sec"] = 3600;
    expect_invalid_json(root, {"field=period_sec", "value=3600"});
}

TEST(ResolveConfigTest, RejectsEmptyOrDuplicateInstruments) {
    nlohmann::json root = minimal_run_config();
    root["instruments"] = nlohmann::json::array();
    expect_invalid_json(root, {"field=instruments", "value=[]", "non-empty array"});

    root = minimal_run_config();
    root["instruments"] = nlohmann::json::array({"rb2601", ""});
    expect_invalid_json(root, {"field=instruments[1]", "value=\"\"", "non-empty string"});

    root = minimal_run_config();
    root["instruments"] = nlohmann::json::array({"rb2601", "rb2601"});
    expect_invalid_json(root, {"field=instruments[1]", "value=\"rb2601\"", "reason=duplicate"});
}

TEST(ResolveConfigTest, RejectsInvalidMaxScale) {
    for (const double max_scale : {0.0, -0.5}) {
        nlohmann::json root = minimal_run_config();
        root["max_scale"] = max_scale;
        expect_invalid_json(root, {"field=max_scale", "positive finite number"});
    }
}

TEST(ResolveConfigTest, RejectsInvalidAccounts) {
    nlohmann::json root = minimal_run_config();
    root["accounts"] = nlohmann::json::array({{{"account_id", ""}, {"scale", 1.0}}});
    expect_invalid_json(root, {"field=accounts[0].account_id", "non-empty string"});

    root = minimal_run_config();
    root["accounts"] = nlohmann::json::array(
        {{{"account_id", "CTP001"}, {"scale", 1.0}}, {{"account_id", "CTP001"}, {"scale", 0.5}}});
    expect_invalid_json(root, {"field=accounts[1].account_id", "value=\"CTP001\"", "duplicate"});

    for (const double scale : {0.0, -1.0}) {
        root = minimal_run_config();
        root["accounts"] = nlohmann::json::array({{{"account_id", "CTP001"}, {"scale", scale}}});
        expect_invalid_json(root, {"field=accounts[0].scale", "positive finite number"});
    }
}

TEST(ResolveConfigTest, RejectsAccountScaleAboveMaxScale) {
    nlohmann::json root = minimal_run_config();
    root["max_scale"] = 0.5;
    root["accounts"] = nlohmann::json::array({{{"account_id", "CTP001"}, {"scale", 0.75}}});
    expect_invalid_json(root, {"field=accounts[0].scale", "value=0.75", "range=(0,0.5]"});

    // 默认 max_scale = 1.0 的边界
    root = minimal_run_config();
    root["accounts"] = nlohmann::json::array({{{"account_id", "CTP001"}, {"scale", 1.5}}});
    expect_invalid_json(root, {"field=accounts[0].scale", "range=(0,1.0]"});
}

TEST(ResolveConfigTest, AcceptsAccountScaleEqualMaxScale) {
    nlohmann::json root = minimal_run_config();
    root["max_scale"] = 2.0;
    root["accounts"] = nlohmann::json::array({{{"account_id", "CTP001"}, {"scale", 2.0}}});
    const EffectiveConfig effective = dztrader::parse_and_resolve_config(root, kToday);
    EXPECT_DOUBLE_EQ(effective.config.accounts.at(0).scale, 2.0);
    EXPECT_DOUBLE_EQ(effective.scaling.max_scale, 2.0);
}

TEST(ResolveConfigTest, RejectsCapacityOutOfRangeWithDocumentedFieldValueRange) {
    nlohmann::json root = minimal_run_config();
    root["max_lookback"] = 31;
    expect_invalid_json(root, {"field=max_lookback", "value=31", "range=[32,16384]"});

    root = minimal_run_config();
    root["max_lookback"] = 16385;
    expect_invalid_json(root, {"field=max_lookback", "value=16385"});

    root = minimal_run_config();
    root["reserve_bars"] = 1583;  // period 60 的 R_min = max(2, ceil(1.1×1440)) = 1584
    expect_invalid_json(root, {"field=reserve_bars", "value=1583", "range=[1584,8192]"});

    root = minimal_run_config();
    root["reserve_bars"] = 8193;
    expect_invalid_json(root, {"field=reserve_bars", "value=8193"});
}

TEST(ResolveConfigTest, RejectsWarmupBarsBelowLookbackOrAboveMax) {
    nlohmann::json root = minimal_run_config();
    root["warmup"] = {{"bars", 1439}};  // 默认 max_lookback = 1440
    expect_invalid_json(root, {"field=warmup.bars", "value=1439", "range=[1440,10000000]"});

    root = minimal_run_config();
    root["warmup"] = {{"bars", 10000001}};
    expect_invalid_json(root, {"field=warmup.bars", "value=10000001"});

    root = minimal_run_config();
    root["warmup"] = {{"bars", -1}};
    expect_invalid_json(root, {"field=warmup.bars", "value=-1", "range=[1440,10000000]"});
}

TEST(ResolveConfigTest, RejectsWarmupFromInFuture) {
    nlohmann::json root = minimal_run_config();
    root["warmup"] = {{"from", "2026-10-29"}};
    expect_invalid_json(
        root, {"field=warmup.from", "value=\"2026-10-29\"", "date not later than 2026-10-28"});
}

TEST(ResolveConfigTest, RejectsMalformedWarmupFrom) {
    for (const char* text :
         {"2024-1-2", "20240102", "", "2024/01/02", "2024-13-01", "2023-02-30"}) {
        nlohmann::json root = minimal_run_config();
        root["warmup"] = {{"from", text}};
        const std::string fragment = std::string("value=\"") + text + "\"";
        expect_invalid_json(root, {"field=warmup.from", fragment});
    }
}

TEST(ResolveConfigTest, DefensivelyRejectsHandBuiltInvalidConfig) {
    // 命令层（#162）会直接改 BarStrategyConfig 后调用 resolve：解析层挡不住的非法值
    // 必须在这里也拒绝，否则会写回一个无法重新加载的配置。
    BarStrategyConfig config;
    config.instruments = {"rb2601"};

    config.max_scale = std::numeric_limits<double>::quiet_NaN();
    expect_invalid_config(config, {"field=max_scale", "positive finite number"});

    config.max_scale.reset();
    config.accounts.push_back(
        dztrader::AttachedAccount{"CTP001", std::numeric_limits<double>::quiet_NaN()});
    expect_invalid_config(config, {"field=accounts[0].scale", "positive finite number"});

    config.accounts.clear();
    config.warmup.bars = 50'000;
    config.warmup.from = "2024-01-02";
    expect_invalid_config(config, {"field=warmup", "reason=mutually_exclusive"});

    config.warmup.from.reset();
    config.instruments.clear();
    expect_invalid_config(config, {"field=instruments", "value=[]", "non-empty array"});
}

// ============================================================
// 自动值与推导结果
// ============================================================

TEST(ResolveConfigTest, DerivesDocumentedAutoValuesForEveryV1Period) {
    struct Case {
        std::int32_t period_sec;
        std::int32_t max_lookback;
        std::int32_t reserve_bars;
        std::int32_t buffer_bars;
        std::int64_t mem_per_series_bytes;
    };
    // 组件文档 §4.5 自动值表（v1 无交易小节表、按 24h 保守估算）
    const std::vector<Case> cases = {
        {60, 1440, 2880, 4320, 34'560},
        {180, 480, 960, 1440, 11'520},
        {300, 288, 576, 864, 6'912},
        {900, 96, 192, 288, 2'304},
    };
    for (const Case& expected : cases) {
        nlohmann::json root = minimal_run_config();
        root["period_sec"] = expected.period_sec;
        const EffectiveConfig effective = dztrader::parse_and_resolve_config(root, kToday);
        EXPECT_EQ(effective.max_lookback, expected.max_lookback) << expected.period_sec;
        EXPECT_EQ(effective.reserve_bars, expected.reserve_bars) << expected.period_sec;
        EXPECT_EQ(effective.buffer_bars, expected.buffer_bars) << expected.period_sec;
        EXPECT_EQ(effective.mem_per_series_bytes, expected.mem_per_series_bytes)
            << expected.period_sec;
        EXPECT_EQ(effective.max_lookback_source, ConfigValueSource::Auto);
        EXPECT_EQ(effective.reserve_bars_source, ConfigValueSource::Auto);
    }
}

TEST(ResolveConfigTest, ExplicitCapacityIsUsedAndMarkedExplicit) {
    nlohmann::json root = minimal_run_config();
    root["max_lookback"] = 2000;
    root["reserve_bars"] = 1600;
    const EffectiveConfig effective = dztrader::parse_and_resolve_config(root, kToday);
    EXPECT_EQ(effective.max_lookback, 2000);
    EXPECT_EQ(effective.reserve_bars, 1600);
    EXPECT_EQ(effective.buffer_bars, 3600);
    EXPECT_EQ(effective.max_lookback_source, ConfigValueSource::Explicit);
    EXPECT_EQ(effective.reserve_bars_source, ConfigValueSource::Explicit);
    ASSERT_TRUE(effective.config.max_lookback.has_value());
    EXPECT_EQ(*effective.config.max_lookback, 2000);
}

TEST(ResolveConfigTest, AcceptsCapacityBoundariesInclusive) {
    // max_lookback / reserve_bars 区间两端都是闭区间；buffer_bars 上限 = 两字段上限之和
    nlohmann::json root = minimal_run_config();
    root["max_lookback"] = 32;
    EXPECT_EQ(dztrader::parse_and_resolve_config(root, kToday).max_lookback, 32);

    root = minimal_run_config();
    root["period_sec"] = 900;
    root["max_lookback"] = 16384;
    root["reserve_bars"] = 8192;
    const EffectiveConfig boundary = dztrader::parse_and_resolve_config(root, kToday);
    EXPECT_EQ(boundary.buffer_bars, 24576);
    EXPECT_EQ(boundary.mem_per_series_bytes, 24576 * 8);

    root = minimal_run_config();
    root["reserve_bars"] = 1584;  // period 60 的 R_min 下界
    EXPECT_EQ(dztrader::parse_and_resolve_config(root, kToday).reserve_bars, 1584);
}

TEST(ResolveConfigTest, AcceptsExplicitEmptyAccountList) {
    nlohmann::json root = minimal_run_config();
    root["accounts"] = nlohmann::json::array();
    const EffectiveConfig effective = dztrader::parse_and_resolve_config(root, kToday);
    EXPECT_TRUE(effective.config.accounts.empty());
    EXPECT_EQ(dztrader::format_effective_accounts(effective.config.accounts),
              "accounts effective | -");
}

TEST(ResolveConfigTest, DefaultWarmupIsTenThousandBars) {
    const EffectiveConfig effective =
        dztrader::parse_and_resolve_config(minimal_run_config(), kToday);
    ASSERT_TRUE(effective.warmup_bars.has_value());
    EXPECT_EQ(*effective.warmup_bars, 10'000);
    EXPECT_FALSE(effective.warmup_from.has_value());
    EXPECT_EQ(effective.warmup_source, ConfigValueSource::Default);
}

TEST(ResolveConfigTest, DefaultWarmupRaisesToMaxLookbackWhenDeeper) {
    nlohmann::json root = minimal_run_config();
    root["max_lookback"] = 12'000;
    root["reserve_bars"] = 2000;
    const EffectiveConfig effective = dztrader::parse_and_resolve_config(root, kToday);
    ASSERT_TRUE(effective.warmup_bars.has_value());
    EXPECT_EQ(*effective.warmup_bars, 12'000);
    EXPECT_EQ(effective.warmup_source, ConfigValueSource::Default);
}

TEST(ResolveConfigTest, ExplicitWarmupBarsAtBoundaries) {
    nlohmann::json root = minimal_run_config();
    root["warmup"] = {{"bars", 1440}};  // == max_lookback 下界
    EffectiveConfig effective = dztrader::parse_and_resolve_config(root, kToday);
    ASSERT_TRUE(effective.warmup_bars.has_value());
    EXPECT_EQ(*effective.warmup_bars, 1440);
    EXPECT_EQ(effective.warmup_source, ConfigValueSource::Explicit);

    root = minimal_run_config();
    root["warmup"] = {{"bars", 10'000'000}};  // 上界
    effective = dztrader::parse_and_resolve_config(root, kToday);
    ASSERT_TRUE(effective.warmup_bars.has_value());
    EXPECT_EQ(*effective.warmup_bars, 10'000'000);
}

TEST(ResolveConfigTest, WarmupFromValidDateAndTodayBoundary) {
    nlohmann::json root = minimal_run_config();
    root["warmup"] = {{"from", "2024-01-02"}};
    EffectiveConfig effective = dztrader::parse_and_resolve_config(root, kToday);
    EXPECT_FALSE(effective.warmup_bars.has_value());
    ASSERT_TRUE(effective.warmup_from.has_value());
    EXPECT_EQ(*effective.warmup_from, "2024-01-02");
    EXPECT_EQ(effective.warmup_source, ConfigValueSource::Explicit);

    root = minimal_run_config();
    root["warmup"] = {{"from", "2026-10-28"}};  // == today：不晚于今天，允许
    effective = dztrader::parse_and_resolve_config(root, kToday);
    ASSERT_TRUE(effective.warmup_from.has_value());
    EXPECT_EQ(*effective.warmup_from, "2026-10-28");
}

TEST(ResolveConfigTest, ParsesFullComponentDocExample) {
    const EffectiveConfig effective =
        dztrader::parse_and_resolve_config(full_example_config(), kToday);
    EXPECT_EQ(effective.config.schema_version, 1);
    EXPECT_EQ(effective.config.period_sec, 60);
    EXPECT_EQ(effective.config.instruments.size(), 2U);
    ASSERT_EQ(effective.config.accounts.size(), 2U);
    EXPECT_EQ(effective.config.accounts.at(0).account_id, "CTP001");
    EXPECT_DOUBLE_EQ(effective.config.accounts.at(0).scale, 1.0);
    EXPECT_DOUBLE_EQ(effective.config.accounts.at(1).scale, 0.5);
    EXPECT_EQ(effective.config.params.at("fast").get<int>(), 10);
    EXPECT_DOUBLE_EQ(effective.scaling.max_scale, 1.0);
    EXPECT_EQ(effective.scaling.rounding, ScaleRounding::Nearest);
    EXPECT_FALSE(effective.scaling.min_one);
    EXPECT_EQ(effective.max_lookback, 1440);
    EXPECT_EQ(effective.buffer_bars, 4320);
}

TEST(ResolveConfigTest, ParamsDefaultToEmptyObjectAndPreserveOrderIndependentValues) {
    const EffectiveConfig without_params =
        dztrader::parse_and_resolve_config(minimal_run_config(), kToday);
    EXPECT_TRUE(without_params.config.params.is_object());
    EXPECT_TRUE(without_params.config.params.empty());

    nlohmann::json root = minimal_run_config();
    root["params"] = {{"fast", 10}, {"nested", {{"enabled", true}}}, {"name", "dual_ma"}};
    const EffectiveConfig effective = dztrader::parse_and_resolve_config(root, kToday);
    EXPECT_EQ(effective.config.params.at("nested").at("enabled").get<bool>(), true);
    EXPECT_EQ(effective.config.params.at("name").get<std::string>(), "dual_ma");
}

// ============================================================
// 有效配置日志（组件文档 §7）
// ============================================================

TEST(EffectiveConfigLogTest, DefaultConfigLineShowsValuesAndSources) {
    const EffectiveConfig effective =
        dztrader::parse_and_resolve_config(full_example_config(), kToday);
    EXPECT_EQ(dztrader::format_effective_config(effective),
              "config effective | schema_version=1 period_sec=60 accounts=2 max_scale=1.0 "
              "scale_rounding=nearest scale_min_one=0 max_lookback=1440 "
              "max_lookback_source=auto reserve_bars=2880 reserve_bars_source=auto "
              "buffer_bars=4320 warmup_bars=10000 warmup_source=default mem_per_series=34560");
    EXPECT_EQ(dztrader::format_effective_accounts(effective.config.accounts),
              "accounts effective | CTP001=1.0 CTP002=0.5");
}

TEST(EffectiveConfigLogTest, ExplicitCapacityAndWarmupAreMarkedExplicit) {
    nlohmann::json root = full_example_config();
    root["max_lookback"] = 2000;
    root["reserve_bars"] = 1600;
    root["warmup"] = {{"bars", 50000}};
    const EffectiveConfig effective = dztrader::parse_and_resolve_config(root, kToday);
    EXPECT_EQ(dztrader::format_effective_config(effective),
              "config effective | schema_version=1 period_sec=60 accounts=2 max_scale=1.0 "
              "scale_rounding=nearest scale_min_one=0 max_lookback=2000 "
              "max_lookback_source=explicit reserve_bars=1600 reserve_bars_source=explicit "
              "buffer_bars=3600 warmup_bars=50000 warmup_source=explicit mem_per_series=28800");
}

TEST(EffectiveConfigLogTest, WarmupFromReplacesBarsField) {
    nlohmann::json root = minimal_run_config();
    root["warmup"] = {{"from", "2024-01-02"}};
    const std::string line =
        dztrader::format_effective_config(dztrader::parse_and_resolve_config(root, kToday));
    EXPECT_NE(line.find("warmup_from=2024-01-02"), std::string::npos) << line;
    EXPECT_EQ(line.find("warmup_bars="), std::string::npos) << line;
    EXPECT_NE(line.find("warmup_source=explicit"), std::string::npos) << line;
}

TEST(EffectiveConfigLogTest, EmptyAccountTableUsesDash) {
    const EffectiveConfig effective =
        dztrader::parse_and_resolve_config(minimal_run_config(), kToday);
    EXPECT_EQ(dztrader::format_effective_accounts(effective.config.accounts),
              "accounts effective | -");
}

TEST(EffectiveConfigLogTest, FormatsRatiosWithShortestRoundTripAndTrailingPointZero) {
    nlohmann::json root = minimal_run_config();
    root["max_scale"] = 0.5;
    root["scale_rounding"] = "ceil";
    root["scale_min_one"] = true;
    root["accounts"] = nlohmann::json::array(
        {{{"account_id", "CTP001"}, {"scale", 0.333}}, {{"account_id", "CTP002"}, {"scale", 0.5}}});
    const EffectiveConfig effective = dztrader::parse_and_resolve_config(root, kToday);
    const std::string line = dztrader::format_effective_config(effective);
    EXPECT_NE(line.find("max_scale=0.5 scale_rounding=ceil scale_min_one=1"), std::string::npos)
        << line;
    EXPECT_EQ(dztrader::format_effective_accounts(effective.config.accounts),
              "accounts effective | CTP001=0.333 CTP002=0.5");

    root = minimal_run_config();
    root["max_scale"] = 2.0;
    root["accounts"] = nlohmann::json::array({{{"account_id", "CTP001"}, {"scale", 1.5}}});
    const std::string integral_line =
        dztrader::format_effective_config(dztrader::parse_and_resolve_config(root, kToday));
    EXPECT_NE(integral_line.find("max_scale=2.0"), std::string::npos) << integral_line;
}

TEST(ResolveConfigTest, WallClockOverloadAcceptsPastWarmupFrom) {
    // 单参重载 = 用本机今天；from 取 2020-01-02（任何现实运行时刻都早于它，确定性足够）
    nlohmann::json root = minimal_run_config();
    root["warmup"] = {{"from", "2020-01-02"}};
    const EffectiveConfig effective = dztrader::parse_and_resolve_config(root);
    ASSERT_TRUE(effective.warmup_from.has_value());
    EXPECT_EQ(*effective.warmup_from, "2020-01-02");
    EXPECT_EQ(effective.warmup_source, ConfigValueSource::Explicit);
}

TEST(EffectiveConfigLogTest, UsesInjectedTodayInsteadOfWallClock) {
    nlohmann::json root = minimal_run_config();
    root["warmup"] = {{"from", "2026-10-29"}};
    // 相对注入的 today（2026-10-28）是未来 → 报错
    expect_invalid_json(root, {"field=warmup.from", "date not later than 2026-10-28"});
    // 换一个更晚的 today → 同一配置合法（证明没有读墙钟）
    const EffectiveConfig effective =
        dztrader::parse_and_resolve_config(root, dztrader::Date{2026, 10, 30});
    ASSERT_TRUE(effective.warmup_from.has_value());
    EXPECT_EQ(*effective.warmup_from, "2026-10-29");
}

}  // namespace
