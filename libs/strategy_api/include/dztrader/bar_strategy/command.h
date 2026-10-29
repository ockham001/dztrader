/**
 * @file command.h
 * @brief UI_INPUT 宿主命令的文法解析：文本行 → `Command`（组件文档 §7「UI_INPUT 宿主命令」，
 *        issue #162）
 *
 * 操作员经 `UI_INPUT` 文本行动态增删品种/账户、读写参数、查询状态。文法只有一种形态——
 * **空白分隔的 token 行**（组件文档 §7「字符串协议」）：
 *
 *     instrument add <id>
 *     instrument del <id> [sync|nosync] [market]
 *     instrument list
 *     account add <id> <scale> [sync|nosync] [market]
 *     account set <id> scale <scale> [sync|nosync] [market]
 *     account del <id> [sync|nosync] [market]
 *     account list
 *     param get <name>
 *     param set <name> <value>
 *     param list
 *     status
 *     quit
 *
 * 为什么解析要独立成纯函数：文法边界（可选关键字出现在哪、`scale` 的尾部垃圾、`account set`
 * 的 `scale` 字面量、空行）是「实现看起来对、漏一格就变成另一个命令」的地方，而命令的**执行**
 * 需要写盘、发单、建节点，只能在集成测试里间接观察。抽出来之后每一格都能直接断言，manager
 * 只剩「解析成功后分发到对应动作」。
 *
 * 三条刻意的取舍：
 *
 *   1. **解析器不打印、不抛异常**：失败就是一条单行错误字符串（`ParseResult::error`），由
 *      manager 写 `OUTPUT_UI`。SDK 头不引入日志（ADR 0020/0021），且测试可以逐字断言错误行；
 *   2. **`sync`/`nosync`/`market` 只允许出现在命令尾部**、顺序任意、可省略。把它们当普通位置参数
 *      处理会让 `account add <id> <scale>` 的 id/scale 位置随关键字出现与否漂移——那正是
 *      「带不带 market 是两个不同文法」的退步；
 *   3. **语法宽松、语义留给执行层**：`nosync market` 在语义上 market 无效，但语法上合法。
 *      解析器只回答「这行是不是一个可识别的命令」，不替执行层判断「这个组合有没有意义」。
 *
 * 本头只依赖标准库（**不** include `dztrader/json.h` / `context.h`）：命令是文本协议，不需要
 * JSON，也不该为解析几个 token 把内置 nlohmann 单头拉进 manager 的每个 TU（ADR 0026 的隔离
 * 口径）。
 */
#ifndef DZTRADER_BAR_STRATEGY_COMMAND_H_
#define DZTRADER_BAR_STRATEGY_COMMAND_H_

#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace dztrader::command_internal {

// ============================================================
// 命令类型与解析结果（manager 的调用面）
// ============================================================

/// @brief 宿主命令的种类（组件文档 §7 的 12 条文法，一条命令一个取值）
enum class CommandKind : std::uint8_t {
    InstrumentAdd,   ///< `instrument add <id>`
    InstrumentDel,   ///< `instrument del <id> [sync|nosync] [market]`
    InstrumentList,  ///< `instrument list`
    AccountAdd,      ///< `account add <id> <scale> [sync|nosync] [market]`
    AccountSet,      ///< `account set <id> scale <scale> [sync|nosync] [market]`
    AccountDel,      ///< `account del <id> [sync|nosync] [market]`
    AccountList,     ///< `account list`
    ParamGet,        ///< `param get <name>`
    ParamSet,        ///< `param set <name> <value>`
    ParamList,       ///< `param list`
    Status,          ///< `status`
    Quit             ///< `quit`
};

/**
 * @brief 一条已解析的宿主命令（执行层只读）
 *
 * 字段按命令取用：`target` 是品种 id / 账户 id / 参数名；`scale` 只对 `AccountAdd` /
 * `AccountSet` 有意义；`sync` / `market` 只对 `InstrumentDel` / `Account*` 的尾部关键字有意义
 * （其余命令恒为 `false`）；`value_token` 只对 `ParamSet` 有意义。
 *
 * **`ParamSet` 的值保持 token 原文而不在这里转成 JSON**：参数类型由策略决定（`ctx.param<T>`），
 * 解析器没有类型信息，替它猜「这是数字还是字符串」会把 `"10"` 与 `10` 混为一谈。类型解释在
 * `ParamStore`（组件文档 §9）。
 */
struct Command {
    CommandKind kind = CommandKind::Status;  ///< 命令种类（默认值只为让聚合初始化可读）
    std::string target;                      ///< instrument id / account id / param name
    double scale = 0.0;                      ///< AccountAdd / AccountSet 的挂靠比率（> 0）
    bool sync = false;                       ///< InstrumentDel / Account*：是否发同步单
    bool market = false;                     ///< InstrumentDel / Account*：同步单是否对手价
    std::string value_token;                 ///< ParamSet 的值 token（原文，不做类型解释）
};

/**
 * @brief 解析结果：成功给命令，失败给一条单行错误（二者恰有一个有效）
 *
 * 错误文本遵循仓库「错误描述」约定（AGENTS §四）：小写英文开头、无句号、
 * `prose | k=v k=v`、禁止单引号包裹占位符。首段固定为 `invalid command | reason=<reason>`，
 * 后面按需附 `command=` / `token=` / `value=` 等字段，便于操作员与日志检索。
 *
 * 解析器**不**把错误写到 stderr：命令结果统一由 manager 写 `OUTPUT_UI`（组件文档 §7 的
 * 「单行错误字符串，内存不变」）。
 */
struct ParseResult {
    std::optional<Command> command;  ///< 成功时有值
    std::string error;               ///< 非空 = 单行错误（失败时 `command` 为空）
};

/// @brief 命令种类的诊断/日志名（`instrument_add` … `quit`；与配置 wire 名同风格）
[[nodiscard]] inline const char* command_kind_name(CommandKind kind) noexcept {
    switch (kind) {
        case CommandKind::InstrumentAdd:
            return "instrument_add";
        case CommandKind::InstrumentDel:
            return "instrument_del";
        case CommandKind::InstrumentList:
            return "instrument_list";
        case CommandKind::AccountAdd:
            return "account_add";
        case CommandKind::AccountSet:
            return "account_set";
        case CommandKind::AccountDel:
            return "account_del";
        case CommandKind::AccountList:
            return "account_list";
        case CommandKind::ParamGet:
            return "param_get";
        case CommandKind::ParamSet:
            return "param_set";
        case CommandKind::ParamList:
            return "param_list";
        case CommandKind::Status:
            return "status";
        case CommandKind::Quit:
            return "quit";
    }
    return "status";
}

// ============================================================
// 解析内部实现（不是 manager 契约；只在解析过程中使用）
// ============================================================

/// @brief 是否是 ASCII 空白（空格 / `\t` / `\r` / `\n` / `\f` / `\v`）
///
/// 显式列出而不调 `std::isspace`：后者依赖当前 locale，且对负值 `char` 是 UB（需先
/// `static_cast<unsigned char>`）。协议里的空白只可能是这六个 ASCII 字符——UTF-8 的
/// 全角空格属于 id 的一部分，不是分隔符。
///
/// @param character 待判定字符
/// @return true = 可作 token 分隔符
[[nodiscard]] inline bool is_ascii_space(char character) noexcept {
    return character == ' ' || character == '\t' || character == '\r' || character == '\n' ||
           character == '\f' || character == '\v';
}

/**
 * @brief 按 ASCII 空白切分一行并折叠连续空白（无引号、无转义语法）
 *
 * 返回的是指向 `line` 的视图，**不复制**；调用方必须保证 `line` 在所有 token 用完前有效
 * （`parse_command` 的每次返回都在此约束内）。空行/纯空白因此得到空表——「空行」的判定
 * 就是 `tokens.empty()`。
 *
 * @param line 一整行 UI_INPUT 文本（可带 `\r\n` 结尾）
 * @return token 视图表（不含空 token）
 */
[[nodiscard]] inline std::vector<std::string_view> tokenize(std::string_view line) {
    std::vector<std::string_view> tokens;
    std::size_t position = 0;
    while (position < line.size()) {
        while (position < line.size() && is_ascii_space(line[position])) {
            ++position;
        }
        const std::size_t begin = position;
        while (position < line.size() && !is_ascii_space(line[position])) {
            ++position;
        }
        if (position > begin) {
            tokens.push_back(line.substr(begin, position - begin));
        }
    }
    return tokens;
}

/// @brief `k=v` 里的值 token：含空格/等号/双引号时加双引号（AGENTS §四的格式约定）
///
/// 命令 token 本身不含空白，但可以含 `=` 或 `"`（例如一个畸形的 id）；错误行要能被
/// 「按空白切 k=v」的日志解析器读回，所以这类值必须加引号。
///
/// @param token 值原文
/// @return 可直接拼进 `k=v` 的字符串
[[nodiscard]] inline std::string quote_token(std::string_view token) {
    if (token.find_first_of(" =\"") == std::string_view::npos) {
        return std::string(token);
    }
    std::string quoted;
    quoted.reserve(token.size() + 2);
    quoted += '"';
    quoted += token;
    quoted += '"';
    return quoted;
}

/**
 * @brief 组装单行错误文本：`invalid command | reason=<reason>` + 可选附加字段
 *
 * 前缀集中在这里而不是每个失败分支自行拼接：**错误行只有一种开头**因此是结构保证，而不是
 * 十几处复制粘贴后的自律。`detail` 由调用方用 `std::format` 生成（已含 `k=v`），本函数只
 * 负责补一个分隔空格。
 *
 * @param reason 机器可读的失败原因（snake_case）
 * @param detail 追加的 `k=v` 字段（不含前导空格；为空则只有前缀）
 * @return 单行错误文本
 */
[[nodiscard]] inline std::string error_text(std::string_view reason, std::string_view detail = {}) {
    std::string text = std::format("invalid command | reason={}", reason);
    if (!detail.empty()) {
        text += ' ';
        text += detail;
    }
    return text;
}

/// @brief 失败结果（`command` 为空、`error` = 单行错误）
/// @param message 已组装的错误文本
/// @return 失败结果
[[nodiscard]] inline ParseResult failure(std::string message) {
    ParseResult result;
    result.error = std::move(message);
    return result;
}

/// @brief 成功结果（`error` 为空）
/// @param command 已解析的命令
/// @return 成功结果
[[nodiscard]] inline ParseResult success(Command command) {
    ParseResult result;
    result.command = std::move(command);
    return result;
}

/// @brief 命令尾部可选的 `[sync|nosync] [market]`（组件文档 §7 的方括号参数）
///
/// 三个布尔量刻意分开：`sync` 是**生效值**（默认 nosync），`sync_seen` 是**互斥/不可重复的
/// 判据**。只看生效值无法区分「显式 nosync」与「根本没写」——而 `nosync sync` 必须报错。
struct TailFlags {
    bool sync = false;       ///< 生效值：命中 `sync`（默认 false = nosync）
    bool market = false;     ///< 命中 `market`（可与 nosync 同时出现，语义由执行层决定）
    bool sync_seen = false;  ///< `sync`/`nosync` 分组已出现（再出现即冲突）
};

/// @brief 尾部关键字解析结果：成功时 `flags` 有效，失败时 `error` 非空
struct TailParse {
    TailFlags flags;    ///< 解析出的关键字（失败时调用方不得使用）
    std::string error;  ///< 单行错误（空 = 成功）
};

/**
 * @brief 解析命令尾部的 `[sync|nosync] [market]`（顺序任意、可省略）
 *
 * 两个分组各自「只能出现一次」：`sync` 与 `nosync` 同属一个互斥组（`sync nosync` 与
 * `sync sync` 都是错误），`market` 不可重复（重复几乎必然是误输入）。非关键字 token 一律
 * 报 `unexpected_argument`——尾部不是「自由参数区」。
 *
 * @param tail         从第一个可选关键字开始的 token（紧跟在命令的位置参数之后）
 * @param command_name 报错用的命令名（如 `instrument.del`）
 * @return 解析出的关键字或单行错误
 */
[[nodiscard]] inline TailParse parse_tail_flags(std::span<const std::string_view> tail,
                                                std::string_view command_name) {
    TailParse parsed;
    for (const std::string_view token : tail) {
        if (token == "sync" || token == "nosync") {
            if (parsed.flags.sync_seen) {
                // 互斥组已定值：重复或自相矛盾都当作同一类错误，消息里的 keyword 指向
                // 「第二次出现的那个」，操作员据此能看出是哪一个 token 多余。
                parsed.error =
                    error_text("conflicting_keyword",
                               std::format("command={} keyword={}", command_name, token));
                return parsed;
            }
            parsed.flags.sync_seen = true;
            parsed.flags.sync = token == "sync";
            continue;
        }
        if (token == "market") {
            if (parsed.flags.market) {
                parsed.error = error_text("conflicting_keyword",
                                          std::format("command={} keyword=market", command_name));
                return parsed;
            }
            parsed.flags.market = true;
            continue;
        }
        parsed.error =
            error_text("unexpected_argument",
                       std::format("command={} token={}", command_name, quote_token(token)));
        return parsed;
    }
    return parsed;
}

/**
 * @brief 把 `scale` token 解析成**有限且 > 0** 的 double（`std::from_chars`，全量消费）
 *
 * 四点必须同时成立，缺一即拒绝：
 *
 *   1. **完整消费**：`ptr` 必须走到 token 末尾，否则 `1.0abc` 会被读成 `1.0`——静默接受
 *      尾随垃圾会让「配置里多一个字符」类的手误变成一条照常执行的命令；
 *   2. **可表示**：`from_chars` 报 `result_out_of_range` 时拒绝（`1e999` 不能变成 `inf`）；
 *   3. **有限**：`from_chars` 的浮点重载**接受 `nan` / `inf`**（按 `strtod` 的 subject
 *      sequence），必须用 `std::isfinite` 单独挡掉——`nan` 传进账户表会让之后每次
 *      `logical_position()` 都算不出手数；
 *   4. **正数**：挂靠比率必须 `> 0`（组件文档 §7 的 `(0, max_scale]`；上界由命令执行层
 *      按运行期配置校验，解析器不知道 `max_scale`）。
 *
 * 用 `from_chars` 而不是 `strtod`：后者依赖 locale（小数点可能不是 `.`）且要额外处理
 * `errno`/`end`；`from_chars` 无分配、无 locale、结果直接给出「消费到哪」。
 *
 * @param token 待解析的 token（不得为空）
 * @param scale 输出：解析成功时写入的比率
 * @return true = 合法；false = 非法（`scale` 不被修改）
 */
[[nodiscard]] inline bool parse_positive_scale(std::string_view token, double& scale) noexcept {
    if (token.empty()) {
        return false;
    }
    double value = 0.0;
    const char* const first = token.data();
    const char* const last = first + token.size();
    const std::from_chars_result parsed = std::from_chars(first, last, value);
    if (parsed.ec != std::errc{} || parsed.ptr != last) {
        return false;
    }
    if (!std::isfinite(value) || !(value > 0.0)) {
        return false;
    }
    scale = value;
    return true;
}

/**
 * @brief 解析 `instrument <subcommand> ...`（组件文档 §7 的三条文法）
 *
 * 只有 `del` 接受尾部关键字；`add` / `list` 的多余 token 一律报错——把 `instrument add id sync`
 * 当作「sync 被忽略」会让操作员以为同步单逻辑生效了。
 *
 * @param tokens 已切分的整行 token（`tokens[0] == "instrument"`）
 * @return 解析结果
 */
[[nodiscard]] inline ParseResult parse_instrument(const std::vector<std::string_view>& tokens) {
    if (tokens.size() < 2) {
        return failure(error_text("missing_argument", "command=instrument argument=subcommand"));
    }
    const std::string_view subcommand = tokens[1];
    if (subcommand == "add") {
        if (tokens.size() < 3) {
            return failure(error_text("missing_argument", "command=instrument.add argument=id"));
        }
        if (tokens.size() > 3) {
            return failure(
                error_text("unexpected_argument",
                           std::format("command=instrument.add token={}", quote_token(tokens[3]))));
        }
        Command command;
        command.kind = CommandKind::InstrumentAdd;
        command.target = std::string(tokens[2]);
        return success(std::move(command));
    }
    if (subcommand == "del") {
        if (tokens.size() < 3) {
            return failure(error_text("missing_argument", "command=instrument.del argument=id"));
        }
        Command command;
        command.kind = CommandKind::InstrumentDel;
        command.target = std::string(tokens[2]);
        const TailParse tail = parse_tail_flags(
            std::span<const std::string_view>(tokens).subspan(3), "instrument.del");
        if (!tail.error.empty()) {
            return failure(tail.error);
        }
        command.sync = tail.flags.sync;
        command.market = tail.flags.market;
        return success(std::move(command));
    }
    if (subcommand == "list") {
        if (tokens.size() > 2) {
            return failure(error_text(
                "unexpected_argument",
                std::format("command=instrument.list token={}", quote_token(tokens[2]))));
        }
        Command command;
        command.kind = CommandKind::InstrumentList;
        return success(std::move(command));
    }
    return failure(error_text("unknown_subcommand", std::format("command=instrument subcommand={}",
                                                                quote_token(subcommand))));
}

/**
 * @brief 解析 `account <subcommand> ...`（组件文档 §7 的四条文法）
 *
 * `account set` 的 `scale` 是**字面量**（不是位置：`account set <id> <scale>` 有歧义，
 * 文法固定了 `set <id> scale <v>` 的形态）。写成别的 token 就是错误，不能跳过——否则
 * `account set CTP001 0.5` 会被读成「id=CTP001、scale=0.5」而少一个参数。
 *
 * @param tokens 已切分的整行 token（`tokens[0] == "account"`）
 * @return 解析结果
 */
[[nodiscard]] inline ParseResult parse_account(const std::vector<std::string_view>& tokens) {
    if (tokens.size() < 2) {
        return failure(error_text("missing_argument", "command=account argument=subcommand"));
    }
    const std::string_view subcommand = tokens[1];
    const std::span<const std::string_view> all(tokens);
    if (subcommand == "add") {
        if (tokens.size() < 3) {
            return failure(error_text("missing_argument", "command=account.add argument=id"));
        }
        Command command;
        command.kind = CommandKind::AccountAdd;
        command.target = std::string(tokens[2]);
        if (tokens.size() < 4) {
            return failure(error_text("missing_argument", "command=account.add argument=scale"));
        }
        if (!parse_positive_scale(tokens[3], command.scale)) {
            return failure(error_text("invalid_scale", std::format("command=account.add value={}",
                                                                   quote_token(tokens[3]))));
        }
        const TailParse tail = parse_tail_flags(all.subspan(4), "account.add");
        if (!tail.error.empty()) {
            return failure(tail.error);
        }
        command.sync = tail.flags.sync;
        command.market = tail.flags.market;
        return success(std::move(command));
    }
    if (subcommand == "set") {
        if (tokens.size() < 3) {
            return failure(error_text("missing_argument", "command=account.set argument=id"));
        }
        Command command;
        command.kind = CommandKind::AccountSet;
        command.target = std::string(tokens[2]);
        if (tokens.size() < 4) {
            return failure(
                error_text("missing_argument", "command=account.set argument=scale_literal"));
        }
        if (tokens[3] != "scale") {
            return failure(error_text("expected_literal",
                                      std::format("command=account.set expected=scale token={}",
                                                  quote_token(tokens[3]))));
        }
        if (tokens.size() < 5) {
            return failure(error_text("missing_argument", "command=account.set argument=scale"));
        }
        if (!parse_positive_scale(tokens[4], command.scale)) {
            return failure(error_text("invalid_scale", std::format("command=account.set value={}",
                                                                   quote_token(tokens[4]))));
        }
        const TailParse tail = parse_tail_flags(all.subspan(5), "account.set");
        if (!tail.error.empty()) {
            return failure(tail.error);
        }
        command.sync = tail.flags.sync;
        command.market = tail.flags.market;
        return success(std::move(command));
    }
    if (subcommand == "del") {
        if (tokens.size() < 3) {
            return failure(error_text("missing_argument", "command=account.del argument=id"));
        }
        Command command;
        command.kind = CommandKind::AccountDel;
        command.target = std::string(tokens[2]);
        const TailParse tail = parse_tail_flags(all.subspan(3), "account.del");
        if (!tail.error.empty()) {
            return failure(tail.error);
        }
        command.sync = tail.flags.sync;
        command.market = tail.flags.market;
        return success(std::move(command));
    }
    if (subcommand == "list") {
        if (tokens.size() > 2) {
            return failure(
                error_text("unexpected_argument",
                           std::format("command=account.list token={}", quote_token(tokens[2]))));
        }
        Command command;
        command.kind = CommandKind::AccountList;
        return success(std::move(command));
    }
    return failure(error_text("unknown_subcommand", std::format("command=account subcommand={}",
                                                                quote_token(subcommand))));
}

/**
 * @brief 解析 `param <subcommand> ...`（组件文档 §7 的三条文法）
 *
 * `param set` 的 value 是**最后一个 token**：值里不能有空白（这是 §7「字符串协议」的代价——
 * 不引入引号语法），需要空格的值由策略侧编码，框架不解释。
 *
 * @param tokens 已切分的整行 token（`tokens[0] == "param"`）
 * @return 解析结果
 */
[[nodiscard]] inline ParseResult parse_param(const std::vector<std::string_view>& tokens) {
    if (tokens.size() < 2) {
        return failure(error_text("missing_argument", "command=param argument=subcommand"));
    }
    const std::string_view subcommand = tokens[1];
    if (subcommand == "get") {
        if (tokens.size() < 3) {
            return failure(error_text("missing_argument", "command=param.get argument=name"));
        }
        if (tokens.size() > 3) {
            return failure(
                error_text("unexpected_argument",
                           std::format("command=param.get token={}", quote_token(tokens[3]))));
        }
        Command command;
        command.kind = CommandKind::ParamGet;
        command.target = std::string(tokens[2]);
        return success(std::move(command));
    }
    if (subcommand == "set") {
        if (tokens.size() < 3) {
            return failure(error_text("missing_argument", "command=param.set argument=name"));
        }
        if (tokens.size() < 4) {
            return failure(error_text("missing_argument", "command=param.set argument=value"));
        }
        if (tokens.size() > 4) {
            return failure(
                error_text("unexpected_argument",
                           std::format("command=param.set token={}", quote_token(tokens[4]))));
        }
        Command command;
        command.kind = CommandKind::ParamSet;
        command.target = std::string(tokens[2]);
        command.value_token = std::string(tokens[3]);
        return success(std::move(command));
    }
    if (subcommand == "list") {
        if (tokens.size() > 2) {
            return failure(
                error_text("unexpected_argument",
                           std::format("command=param.list token={}", quote_token(tokens[2]))));
        }
        Command command;
        command.kind = CommandKind::ParamList;
        return success(std::move(command));
    }
    return failure(error_text("unknown_subcommand",
                              std::format("command=param subcommand={}", quote_token(subcommand))));
}

/// @brief 解析无参数的单词命令（`status` / `quit`）：只接受这**一个** token
///
/// @param tokens 已切分的整行 token
/// @param kind   命令种类
/// @param name   报错用的命令名（`status` / `quit`）
/// @return 解析结果
[[nodiscard]] inline ParseResult parse_word_command(const std::vector<std::string_view>& tokens,
                                                    CommandKind kind,
                                                    std::string_view name) {
    if (tokens.size() > 1) {
        return failure(error_text("unexpected_argument", std::format("command={} token={}", name,
                                                                     quote_token(tokens[1]))));
    }
    Command command;
    command.kind = kind;
    return success(std::move(command));
}

// ============================================================
// 入口
// ============================================================

/**
 * @brief 解析一行 UI_INPUT 文本（组件文档 §7 的唯一文法入口）
 *
 * 分发按第一个 token 分组；未知命令名、未知子命令、参数个数不符、多余 token、非法 `scale`、
 * 关键字冲突都返回单行错误且 `command` 为空——**解析失败不产生任何部分命令**，manager 因此
 * 只需在成功时执行、失败时把 `error` 写回 `OUTPUT_UI`。
 *
 * 关键字区分大小写（§7：`sync`/`nosync`/`market` 为小写关键字），`STATUS` 不是 `status`。
 *
 * @param line 一整行文本（前后空白与 `\r\n` 结尾都可以；空行/纯空白是错误）
 * @return 命令或单行错误
 */
[[nodiscard]] inline ParseResult parse_command(std::string_view line) {
    const std::vector<std::string_view> tokens = tokenize(line);
    if (tokens.empty()) {
        return failure(error_text("empty_line"));
    }
    const std::string_view head = tokens.front();
    if (head == "instrument") {
        return parse_instrument(tokens);
    }
    if (head == "account") {
        return parse_account(tokens);
    }
    if (head == "param") {
        return parse_param(tokens);
    }
    if (head == "status") {
        return parse_word_command(tokens, CommandKind::Status, "status");
    }
    if (head == "quit") {
        return parse_word_command(tokens, CommandKind::Quit, "quit");
    }
    return failure(error_text("unknown_command", std::format("command={}", quote_token(head))));
}

}  // namespace dztrader::command_internal

#endif  // DZTRADER_BAR_STRATEGY_COMMAND_H_
