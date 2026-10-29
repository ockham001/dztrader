/**
 * @file bar_strategy_command_test.cpp
 * @brief UI_INPUT 宿主命令的文法解析单元测试（issue #162；组件文档 §7「UI_INPUT 宿主命令」）
 *
 * 被测面只有两个纯函数（`command_internal::parse_command` / `command_kind_name`），输入是
 * 文本、输出是值，因此逐格断言而不经过 UI_INPUT/OUTPUT_UI 帧通道：
 *
 *   1. **12 条文法**各一条：逐字段断言 `kind` / `target` / `scale` / `sync` / `market` /
 *      `value_token`——一个字段没断言到，就可能出现「命令种类对、参数接错」这种能执行但做错事
 *      的解析；
 *   2. **错误面**：未知命令/子命令、每个命令的参数个数、多余 token、`scale` 取值、关键字冲突。
 *      错误行是给操作员看的契约的一部分（§7「单行错误字符串」），因此断言它保持统一前缀、
 *      单行、全英文；
 *   3. **token 化**：空白折叠、`\t` / `\r\n` 尾、空行——命令来自 `UI_INPUT` 文本帧，末尾
 *      带 `\r\n` 是常态而不是异常输入。
 *
 * 执行层（写盘/发单/建节点）由 Seam A 集成测试覆盖；这里完全不碰配置、文件与 shm。
 *
 * 断言口径：失败消息带上原始输入行（`line=`），错误文案只查 `reason=` 关键片段而不逐字锁定
 * 整句——错误措辞会随实现演进，锁死整句会让每个无关的文案改动都变成红色。
 */
#include <gtest/gtest.h>

#include <dztrader/bar_strategy/command.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using dztrader::command_internal::Command;
using dztrader::command_internal::command_kind_name;
using dztrader::command_internal::CommandKind;
using dztrader::command_internal::parse_command;
using dztrader::command_internal::ParseResult;

/// 解析一行并断言成功；失败时报告原始行与错误行，返回空 optional 让调用方 ASSERT 收尾
std::optional<Command> require_ok(std::string_view line) {
    ParseResult result = parse_command(line);
    if (!result.command.has_value()) {
        ADD_FAILURE() << "expected a successful parse | line=" << line << " error=" << result.error;
        return std::nullopt;
    }
    return result.command;
}

/// 逐字段断言一条命令（默认值 = 该字段对「不带这些参数的命令」的期望值）
void expect_command(const Command& command,
                    CommandKind kind,
                    std::string_view target = {},
                    double scale = 0.0,
                    bool sync = false,
                    bool market = false,
                    std::string_view value_token = {}) {
    EXPECT_EQ(command.kind, kind) << "kind=" << command_kind_name(command.kind);
    EXPECT_EQ(command.target, std::string(target));
    EXPECT_DOUBLE_EQ(command.scale, scale);
    EXPECT_EQ(command.sync, sync);
    EXPECT_EQ(command.market, market);
    EXPECT_EQ(command.value_token, std::string(value_token));
}

/// 断言解析失败、`command` 为空，且错误行含关键 `reason=` 片段（不逐字锁句）
void expect_error(std::string_view line, std::string_view reason_fragment) {
    ParseResult result = parse_command(line);
    if (result.command.has_value()) {
        ADD_FAILURE() << "expected a parse error | line=" << line
                      << " kind=" << command_kind_name(result.command->kind);
        return;
    }
    EXPECT_NE(result.error.find("invalid command | reason="), std::string::npos)
        << "line=" << line << " error=" << result.error;
    EXPECT_NE(result.error.find(reason_fragment), std::string::npos)
        << "line=" << line << " error=" << result.error;
}

// ============================================================
// 12 条文法：逐字段断言
// ============================================================

/**
 * **`instrument add <id>`**：只取一个 id，`sync`/`market` 恒为 false。
 *
 * 没有尾部关键字——`instrument add rb2601 sync` 是多余 token，不是「同步 add」（§7 文法只给
 * del 配了 `[sync|nosync] [market]`）。
 */
TEST(BarStrategyCommandParseTest, InstrumentAddKeepsTargetAndNoFlags) {
    const std::optional<Command> command = require_ok("instrument add rb2601");
    ASSERT_TRUE(command.has_value());
    expect_command(*command, CommandKind::InstrumentAdd, "rb2601");
}

/// **`instrument del <id>`**：默认 nosync / 非对手价（§7「默认 nosync」）
TEST(BarStrategyCommandParseTest, InstrumentDelDefaultsToNoSync) {
    const std::optional<Command> command = require_ok("instrument del rb2601");
    ASSERT_TRUE(command.has_value());
    expect_command(*command, CommandKind::InstrumentDel, "rb2601");
}

/// **`instrument list`**：无参数
TEST(BarStrategyCommandParseTest, InstrumentListTakesNoArgument) {
    const std::optional<Command> command = require_ok("instrument list");
    ASSERT_TRUE(command.has_value());
    expect_command(*command, CommandKind::InstrumentList);
}

/**
 * **`account add <id> <scale> [sync|nosync] [market]`**：id 与 scale 都在固定位置。
 *
 * `scale` 是字符串 token → double，`1.5` 必须精确等于 1.5（`from_chars` 的最短往返保证，
 * 不是 `atof` 那样的宽松解析）。
 */
TEST(BarStrategyCommandParseTest, AccountAddParsesIdScaleAndTailFlags) {
    const std::optional<Command> plain = require_ok("account add CTP001 1.5");
    ASSERT_TRUE(plain.has_value());
    expect_command(*plain, CommandKind::AccountAdd, "CTP001", 1.5);

    const std::optional<Command> flagged = require_ok("account add CTP002 0.5 sync market");
    ASSERT_TRUE(flagged.has_value());
    expect_command(*flagged, CommandKind::AccountAdd, "CTP002", 0.5, true, true);
}

/**
 * **`account set <id> scale <scale>`**：`scale` 是**字面量**而不是位置参数。
 *
 * 缺字面量（`account set CTP001 0.5`）必须报错：若把 `tokens[3]` 当 scale 用，操作员漏写
 * `scale` 的输入会被静默执行成另一条命令。
 */
TEST(BarStrategyCommandParseTest, AccountSetRequiresScaleLiteralAndParsesScale) {
    const std::optional<Command> command = require_ok("account set CTP001 scale 2.0");
    ASSERT_TRUE(command.has_value());
    expect_command(*command, CommandKind::AccountSet, "CTP001", 2.0);

    expect_error("account set CTP001 2.0", "reason=expected_literal");
}

/// **`account del <id> [sync|nosync] [market]`**：默认 nosync，可带 sync + market
TEST(BarStrategyCommandParseTest, AccountDelParsesTargetAndTailFlags) {
    const std::optional<Command> plain = require_ok("account del CTP001");
    ASSERT_TRUE(plain.has_value());
    expect_command(*plain, CommandKind::AccountDel, "CTP001");

    const std::optional<Command> synced = require_ok("account del CTP001 sync");
    ASSERT_TRUE(synced.has_value());
    expect_command(*synced, CommandKind::AccountDel, "CTP001", 0.0, true, false);

    const std::optional<Command> market_synced = require_ok("account del CTP001 sync market");
    ASSERT_TRUE(market_synced.has_value());
    expect_command(*market_synced, CommandKind::AccountDel, "CTP001", 0.0, true, true);
}

/// **`account list`**：无参数
TEST(BarStrategyCommandParseTest, AccountListTakesNoArgument) {
    const std::optional<Command> command = require_ok("account list");
    ASSERT_TRUE(command.has_value());
    expect_command(*command, CommandKind::AccountList);
}

/// **`param get <name>`**：参数名进 `target`
TEST(BarStrategyCommandParseTest, ParamGetParsesName) {
    const std::optional<Command> command = require_ok("param get fast");
    ASSERT_TRUE(command.has_value());
    expect_command(*command, CommandKind::ParamGet, "fast");
}

/**
 * **`param set <name> <value>`**：值保持 token 原文，不做类型解释。
 *
 * 解析器没有策略的类型信息（`ctx.param<T>` 的 T 由 B 决定），所以 `10`、`sync`、`1.5` 都以
 * 字符串形式交给 `ParamStore`。把值当数字解析会让字符串参数 `"10"` 与数字 `10` 混为一谈。
 */
TEST(BarStrategyCommandParseTest, ParamSetKeepsValueTokenVerbatim) {
    const std::optional<Command> numeric = require_ok("param set fast 10");
    ASSERT_TRUE(numeric.has_value());
    expect_command(*numeric, CommandKind::ParamSet, "fast", 0.0, false, false, "10");

    // 值恰好与尾部关键字同名：param 没有尾部关键字，它就是值
    const std::optional<Command> keyword_like = require_ok("param set mode sync");
    ASSERT_TRUE(keyword_like.has_value());
    expect_command(*keyword_like, CommandKind::ParamSet, "mode", 0.0, false, false, "sync");

    const std::optional<Command> with_equals = require_ok("param set tag a=b");
    ASSERT_TRUE(with_equals.has_value());
    expect_command(*with_equals, CommandKind::ParamSet, "tag", 0.0, false, false, "a=b");
}

/// **`param list`**：无参数
TEST(BarStrategyCommandParseTest, ParamListTakesNoArgument) {
    const std::optional<Command> command = require_ok("param list");
    ASSERT_TRUE(command.has_value());
    expect_command(*command, CommandKind::ParamList);
}

/// **`status` / `quit`**：单个 token，无参数
TEST(BarStrategyCommandParseTest, StatusAndQuitAreWordCommands) {
    const std::optional<Command> status = require_ok("status");
    ASSERT_TRUE(status.has_value());
    expect_command(*status, CommandKind::Status);

    const std::optional<Command> quit = require_ok("quit");
    ASSERT_TRUE(quit.has_value());
    expect_command(*quit, CommandKind::Quit);
}

/**
 * **`command_kind_name` 的 wire 名**（manager 写日志/结果文本用）。
 *
 * 12 个名字逐个钉住：改名会让 OUTPUT_UI 的文本协议漂移，而测试与 dzweb 侧都可能按名字匹配。
 */
TEST(BarStrategyCommandParseTest, CommandKindNamesMatchWireProtocol) {
    struct Case {
        CommandKind kind;
        const char* name;
    };
    const Case cases[] = {
        {CommandKind::InstrumentAdd, "instrument_add"},
        {CommandKind::InstrumentDel, "instrument_del"},
        {CommandKind::InstrumentList, "instrument_list"},
        {CommandKind::AccountAdd, "account_add"},
        {CommandKind::AccountSet, "account_set"},
        {CommandKind::AccountDel, "account_del"},
        {CommandKind::AccountList, "account_list"},
        {CommandKind::ParamGet, "param_get"},
        {CommandKind::ParamSet, "param_set"},
        {CommandKind::ParamList, "param_list"},
        {CommandKind::Status, "status"},
        {CommandKind::Quit, "quit"},
    };
    for (const Case& item : cases) {
        EXPECT_STREQ(command_kind_name(item.kind), item.name);
    }
}

// ============================================================
// 尾部关键字：sync / nosync / market
// ============================================================

/**
 * **顺序任意、可省略**（§7：`[sync|nosync] [market]`）。
 *
 * `sync market` 与 `market sync` 必须等价——文法把这两个关键字定义为尾部可选组，任何位置
 * 相关的实现都会让操作员因为「写反了顺序」得到一条语法错误。
 */
TEST(BarStrategyCommandTailFlagTest, KeywordOrderIsIrrelevant) {
    struct Case {
        std::string_view line;
        bool sync;
        bool market;
    };
    const Case cases[] = {
        {"instrument del rb2601", false, false},         // 默认 nosync
        {"instrument del rb2601 nosync", false, false},  // 显式 nosync 等价于默认
        {"instrument del rb2601 sync", true, false},
        {"instrument del rb2601 market", false, true},  // market 单用合法（语义无效）
        {"instrument del rb2601 sync market", true, true},
        {"instrument del rb2601 market sync", true, true},     // 顺序无关
        {"instrument del rb2601 nosync market", false, true},  // nosync + market 语法合法
    };
    for (const Case& item : cases) {
        const std::optional<Command> command = require_ok(item.line);
        ASSERT_TRUE(command.has_value()) << "line=" << item.line;
        EXPECT_EQ(command->kind, CommandKind::InstrumentDel) << "line=" << item.line;
        EXPECT_EQ(command->sync, item.sync) << "line=" << item.line;
        EXPECT_EQ(command->market, item.market) << "line=" << item.line;
    }
}

/**
 * **`sync` 与 `nosync` 互斥且不可重复，`market` 不可重复**。
 *
 * 只看生效值（`sync = true/false`）无法区分「显式 nosync」与「没写」，所以解析必须单独
 * 记录是否已出现过——否则 `sync nosync` 会静默变成 nosync，与操作员的本意相反。
 */
TEST(BarStrategyCommandTailFlagTest, DuplicateOrConflictingKeywordsAreRejected) {
    for (const std::string_view line : {
             "instrument del rb2601 sync nosync",
             "instrument del rb2601 nosync sync",
             "instrument del rb2601 sync sync",
             "instrument del rb2601 nosync nosync",
             "instrument del rb2601 sync market market",
             "account del CTP001 sync nosync",
             "account add CTP001 1.0 sync nosync",
             "account set CTP001 scale 1.0 nosync sync",
         }) {
        expect_error(line, "reason=conflicting_keyword");
    }
}

/// **尾部不是自由参数区**：非关键字的 token 在尾部一律报错（`instrument del id foo`）
TEST(BarStrategyCommandTailFlagTest, UnknownTokenInTailIsRejected) {
    for (const std::string_view line : {
             "instrument del rb2601 foo",
             "instrument del rb2601 sync foo",
             "account add CTP001 1.0 foo",
             "account del CTP001 foo",
             "account set CTP001 scale 1.0 foo",
         }) {
        expect_error(line, "reason=unexpected_argument");
    }
}

/**
 * **关键字只在尾部位置被解释**：位置参数位置上的同名 token 是普通参数值。
 *
 * `instrument del sync` 的 `sync` 位于 id 位（是品种 id，不是关键字），`instrument add sync`
 * 同理。这条区分了「按位置解析」与「扫全文找关键字」——后者会把一个叫 `sync` 的品种 id
 * 变成一条同步单命令。
 */
TEST(BarStrategyCommandTailFlagTest, KeywordsAreOnlyInterpretedAtTheTail) {
    const std::optional<Command> del_target_named_sync = require_ok("instrument del sync");
    ASSERT_TRUE(del_target_named_sync.has_value());
    expect_command(*del_target_named_sync, CommandKind::InstrumentDel, "sync", 0.0, false, false);

    const std::optional<Command> add_target_named_market = require_ok("instrument add market");
    ASSERT_TRUE(add_target_named_market.has_value());
    expect_command(*add_target_named_market, CommandKind::InstrumentAdd, "market");
}

// ============================================================
// scale：完整消费 + 有限 + > 0
// ============================================================

/**
 * **合法 scale**：正数，含整数、小数与科学计数法。
 *
 * `1` / `1.0` 与 `1.0000000000000002` 不必区分（挂靠比率只做乘法）；这里钉住的是「能被接受
 * 且解析成同一个 double」。
 */
TEST(BarStrategyCommandScaleTest, AcceptsPositiveFiniteValues) {
    struct Case {
        std::string_view token;
        double expected;
    };
    const Case cases[] = {
        {"1", 1.0},     {"0.5", 0.5}, {"1.0", 1.0},    {"2", 2.0},
        {"0.25", 0.25}, {"1.5", 1.5}, {"1e-3", 0.001},
    };
    for (const Case& item : cases) {
        const std::string line = "account add CTP001 " + std::string(item.token);
        const std::optional<Command> command = require_ok(line);
        ASSERT_TRUE(command.has_value()) << "line=" << line;
        EXPECT_EQ(command->kind, CommandKind::AccountAdd) << "line=" << line;
        EXPECT_DOUBLE_EQ(command->scale, item.expected) << "line=" << line;
    }
}

/**
 * **非法 scale**：0、负数、非数字、尾随垃圾、空、NaN/Inf。
 *
 * 三种非法各有来路：`0`/`-1` 越出 `(0, max_scale]`；`1.0x` 是「只看了前缀没看完整 token」的
 * 经典漏洞（`from_chars` 会成功返回 1.0）；`nan`/`inf` 会被 `from_chars` **接受**，必须靠
 * `std::isfinite` 额外挡掉——一个 NaN 挂靠比率会让后续每笔 `logical_position()` 都算不出手数。
 */
TEST(BarStrategyCommandScaleTest, RejectsZeroNegativeGarbageAndNonFinite) {
    for (const std::string_view token : {
             "0",
             "0.0",
             "-1",
             "-0.5",
             "abc",
             "1.0x",
             "1.0.",
             ".",
             "+1",
             "nan",
             "NaN",
             "inf",
             "-inf",
             "infinity",
             "1e999",
         }) {
        const std::string add_line = "account add CTP001 " + std::string(token);
        expect_error(add_line, "reason=invalid_scale");
        const std::string set_line = "account set CTP001 scale " + std::string(token);
        expect_error(set_line, "reason=invalid_scale");
    }
}

/**
 * **空 scale 表现为参数个数错误**：token 化后空 token 不存在，因此 `account add CTP001` 是
 * 「少一个参数」而不是「scale 为空串」。空串 → 错误这一条语义仍然成立，只是 reason 不同。
 */
TEST(BarStrategyCommandScaleTest, EmptyScaleShowsUpAsMissingArgument) {
    expect_error("account add CTP001", "reason=missing_argument");
    expect_error("account set CTP001 scale", "reason=missing_argument");
    expect_error("account set CTP001 scale   ", "reason=missing_argument");
}

// ============================================================
// token 化：空白、空行
// ============================================================

/// **空行 / 纯空白是错误**（不是 no-op）：一条命令总得有个命令名
TEST(BarStrategyCommandTokenizeTest, EmptyAndBlankLinesAreRejected) {
    for (const std::string_view line : {"", " ", "   ", "\t", "\r\n", " \t \r\n \f\v "}) {
        expect_error(line, "reason=empty_line");
    }
}

/**
 * **前导/尾随/连续空白与 `\t`、`\r\n` 都被折叠**。
 *
 * `UI_INPUT` 的 payload 来自 UI 输入框或测试帧，末尾带 `\r\n`（Windows 换行）是常态；把这些
 * 当参数的一部分会让合法命令在 Windows 客户端上全部失败。
 */
TEST(BarStrategyCommandTokenizeTest, WhitespaceRunsAreCollapsed) {
    const std::optional<Command> padded = require_ok("   instrument   add   rb2601   ");
    ASSERT_TRUE(padded.has_value());
    expect_command(*padded, CommandKind::InstrumentAdd, "rb2601");

    const std::optional<Command> tabs = require_ok("\tinstrument\tadd\trb2601\t");
    ASSERT_TRUE(tabs.has_value());
    expect_command(*tabs, CommandKind::InstrumentAdd, "rb2601");

    const std::optional<Command> crlf = require_ok("instrument add rb2601\r\n");
    ASSERT_TRUE(crlf.has_value());
    expect_command(*crlf, CommandKind::InstrumentAdd, "rb2601");

    const std::optional<Command> form_feed = require_ok("status\f\v");
    ASSERT_TRUE(form_feed.has_value());
    expect_command(*form_feed, CommandKind::Status);
}

// ============================================================
// 错误面：未知命令 / 未知子命令 / 参数个数 / 多余 token
// ============================================================

/// **未知命令名**（含大小写不符——§7 的关键字与命令名都是小写）
TEST(BarStrategyCommandErrorTest, UnknownCommandIsRejected) {
    for (const std::string_view line : {
             "bogus",
             "help",
             "exit",
             "STATUS",
             "Instrument add rb2601",
             "instrument_add rb2601",
             "instrumentadd rb2601",
         }) {
        expect_error(line, "reason=unknown_command");
    }
}

/// **未知子命令**（`instrument foo` / `account foo` / `param foo`）与缺子命令
TEST(BarStrategyCommandErrorTest, UnknownSubcommandIsRejected) {
    for (const std::string_view line : {
             "instrument foo",
             "instrument foo rb2601",
             "account foo CTP001",
             "param foo fast",
             "instrument add rb2601 extra",
         }) {
        const ParseResult result = parse_command(line);
        ASSERT_FALSE(result.command.has_value()) << "line=" << line;
        EXPECT_NE(result.error.find("reason="), std::string::npos) << "line=" << line;
    }

    expect_error("instrument foo", "reason=unknown_subcommand");
    expect_error("account foo", "reason=unknown_subcommand");
    expect_error("param foo", "reason=unknown_subcommand");

    // 只有命令名、没有子命令：缺参数，不是未知子命令
    expect_error("instrument", "reason=missing_argument");
    expect_error("account", "reason=missing_argument");
    expect_error("param", "reason=missing_argument");
}

/// **每个命令的参数个数下限**：少一个参数都必须报 `missing_argument`，不能靠默认值糊过去
TEST(BarStrategyCommandErrorTest, MissingArgumentsAreRejected) {
    for (const std::string_view line : {
             "instrument add",
             "instrument del",
             "account add",
             "account add CTP001",
             "account set",
             "account set CTP001",
             "account set CTP001 scale",
             "account del",
             "param get",
             "param set",
             "param set fast",
         }) {
        expect_error(line, "reason=missing_argument");
    }
}

/// **每个命令都拒绝多余 token**（`list`/`status`/`quit` 不接受参数，其余不接受超量参数）
TEST(BarStrategyCommandErrorTest, TrailingTokensAreRejectedForEveryCommand) {
    for (const std::string_view line : {
             "instrument add rb2601 extra",
             "instrument del rb2601 extra",
             "instrument list extra",
             "account add CTP001 1.0 extra",
             "account set CTP001 scale 1.0 extra",
             "account del CTP001 extra",
             "account list extra",
             "param get fast extra",
             "param set fast 10 extra",
             "param list extra",
             "status extra",
             "quit extra",
         }) {
        expect_error(line, "reason=unexpected_argument");
    }
}

/**
 * **错误行本身的契约**：统一前缀、单行、无句号、全 ASCII、无单引号包裹。
 *
 * 错误行最终写进 `OUTPUT_UI` 并可能被 `dz_notify_ui` 弹给操作员（manager 侧），所以它是文本
 * 协议的一部分：多行会破坏「一行一条响应」的读取假设，中文会破坏全英文约定，尾随句号会让
 * 「prose | k=v」的解析器读出一个多余的 `v`。
 */
TEST(BarStrategyCommandErrorTest, EveryErrorLineKeepsTheTextProtocol) {
    const std::vector<std::string> lines = {
        "",
        "   ",
        "bogus",
        "instrument foo",
        "instrument",
        "instrument add",
        "instrument add rb2601 extra",
        "instrument del rb2601 sync nosync",
        "account add CTP001",
        "account add CTP001 abc",
        "account set CTP001 1.0",
        "account set CTP001 scale nan",
        "param set fast",
        "status extra",
    };
    for (const std::string& line : lines) {
        const ParseResult result = parse_command(line);
        ASSERT_FALSE(result.command.has_value()) << "line=" << line;
        ASSERT_FALSE(result.error.empty()) << "line=" << line;

        const std::string& error = result.error;
        EXPECT_EQ(error.find('\n'), std::string::npos) << "line=" << line << " error=" << error;
        EXPECT_EQ(error.find('\r'), std::string::npos) << "line=" << line << " error=" << error;
        EXPECT_EQ(error.find('\''), std::string::npos) << "line=" << line << " error=" << error;
        EXPECT_NE(error.back(), '.') << "line=" << line << " error=" << error;
        EXPECT_TRUE(error.starts_with("invalid command | reason="))
            << "line=" << line << " error=" << error;
        for (const char character : error) {
            EXPECT_LT(static_cast<unsigned char>(character), 0x80)
                << "line=" << line << " error=" << error;
        }
    }
}

/**
 * **`ParseResult` 恰有一个有效字段**：成功时 `error` 为空，失败时 `command` 为空。
 *
 * manager 只判 `command.has_value()` 决定「执行还是写错误行」；两个字段同时非空/同时为空都会
 * 让那条分支做出错误选择（例如把空字符串写进 OUTPUT_UI）。
 */
TEST(BarStrategyCommandErrorTest, ParseResultHoldsExactlyOneOfCommandOrError) {
    const std::vector<std::string> lines = {
        "instrument add rb2601",
        "instrument del rb2601 sync market",
        "instrument list",
        "account add CTP001 1.0",
        "account set CTP001 scale 0.5",
        "account del CTP001",
        "account list",
        "param get fast",
        "param set fast 10",
        "param list",
        "status",
        "quit",
        "bogus",
        "",
        "account add CTP001 0",
    };
    for (const std::string& line : lines) {
        const ParseResult result = parse_command(line);
        if (result.command.has_value()) {
            EXPECT_TRUE(result.error.empty()) << "line=" << line << " error=" << result.error;
        } else {
            EXPECT_FALSE(result.error.empty()) << "line=" << line;
        }
    }
}

}  // namespace
