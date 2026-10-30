/**
 * @file public_surface_invariants_test.cpp
 * @brief 公开面不变量（#165 AC4）：导出符号白名单 / C ABI / StrategyBase / 引擎入口 / 帧号 / 帧结构
 *
 * 为什么需要本探针（#165 收口）：#157–#164 一路新增 BarStrategy 能力时，策略二进制能依赖的
 * 唯一承诺就是公开面——动态库导出符号、`dz_*` C ABI、`StrategyBase` 回调、引擎入口模板、
 * 帧号与帧 payload 布局。评审能看见 diff，但没人能可靠回答「这一波有没有悄悄多导出一个
 * 符号、改一个帧号、给帧结构加一个字段」；Windows CI 也没有 nm/readelf/dumpbin 可用。
 * 于是把当前公开面机械冻结成数据表：任何变化都必须是有意识的——同提交改契约/ADR、改这里
 * 的冻结表、在 PR 说明。否则测试直接红，而不是等某个策略进程读到错位的帧。
 *
 * 六组不变量（与 issue #165 AC4 一一对应）：
 *   1. `exported_symbols.map` 的 version script 结构：`global:` 只有 `dz_*;`，`local:` 只有
 *      `*;`——新增导出必须走 ADR/契约评审；
 *   2. `api.h` 与 `src/api.cpp` 的 `DZ_API` 函数名集合 == 冻结的 49 个 `dz_*`（声明与定义
 *      必须同时存在，新增/删除/改名即失败）；
 *   3. `strategy_base.h` 的全部 `virtual` 声明（虚析构 + 11 个回调，去注释、归一化空白后按
 *      出现顺序）；
 *   4. 引擎入口（**编译期**）：`run_strategy<T>` / `run_bar_strategy<T>` 的函数指针类型、
 *      `Strategy` / `BarStrategy` concept、`sizeof(StrategyBase) == sizeof(void*)`——只取类型，
 *      不 ODR-use，因此不链接任何 C API 符号；
 *   5. 帧号：`dztrader/data_type.h`（策略可见帧的唯一书写位置）必须精确等于 9 项冻结映射；
 *      `libs/core/.../core_data_type.h`（平台内部帧）做存在性 + 值不变检查，允许追加新帧、
 *      禁止改旧值；两个书写位置的帧名不得重复；
 *   6. 帧 payload 结构：`struct.h` 的 11 个 `DZ_DECLARE_ALIGNED_STRUCT` 与 `core_struct.h` 的
 *      `DzOrderReq` / `DzLogicalPosition` 的字段声明有序表逐字段一致（宏本身已 static_assert
 *      8 字节对齐且无 padding，字段表冻结即布局冻结）。
 *
 * 实现方式是**纯文本扫描**（repo_root 定位与 `public_header_coverage_test.cpp` 同款）：
 * 不依赖 git、不依赖 nm/readelf/dumpbin 等外部工具——Windows CI 也要跑，且这些工具看到的
 * 是「构建后的事实」而不是「源头的承诺」。冻结表刻意硬编码而不是从被测头文件生成：
 * 生成式冻结表恒真（把当前值抄一遍），没有捕获变更的能力；这里冻结的是**评审过的快照**。
 *
 * 已知盲区（有意）：本测试不读动态符号表，因此它冻结的是「白名单 + `DZ_API` 声明 + 帧/结构
 * 头文件」这份唯一书写源，而不是链接产物；后者由 ADR 0038 的链接器白名单执行。要发现
 * 「白名单被改坏后仍有人导出非 dz_* 符号」需要平台工具，不在跨平台单测的能力范围内。
 */
#include <gtest/gtest.h>

#include <dztrader/bar_strategy.h>
#include <dztrader/data_type.h>
#include <dztrader/strategy_base.h>
#include <dztrader/strategy_engine.h>
#include <dztrader/struct.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <regex>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

// ============================================================
// 被冻结的公开面文件（相对仓库根）
// ============================================================

/// @brief 动态库导出白名单（链接器 version script，ADR 0038 决策 7）
constexpr const char* kExportedSymbolsMap = "libs/strategy_api/exported_symbols.map";
/// @brief C ABI 唯一声明处（49 个 dz_* 函数）
constexpr const char* kCApiHeader = "libs/strategy_api/include/dztrader/api.h";
/// @brief C ABI 实现处（必须与声明一一对应）
constexpr const char* kCApiSource = "libs/strategy_api/src/api.cpp";
/// @brief StrategyBase 回调基类
constexpr const char* kStrategyBaseHeader = "libs/strategy_api/include/dztrader/strategy_base.h";
/// @brief 策略可见帧号的唯一书写位置
constexpr const char* kStrategyDataTypeHeader = "libs/strategy_api/include/dztrader/data_type.h";
/// @brief 平台内部帧号的唯一书写位置
constexpr const char* kCoreDataTypeHeader = "libs/core/include/dztrader/core/core_data_type.h";
/// @brief 策略可见帧 payload 结构体
constexpr const char* kStrategyStructHeader = "libs/strategy_api/include/dztrader/struct.h";
/// @brief 平台内部帧 payload 结构体（只冻结 BarStrategy 框架用到的两个）
constexpr const char* kCoreStructHeader = "libs/core/include/dztrader/core/core_struct.h";

/// @brief 有意变更公开面时的统一处置提示（#165 AC4 要求失败信息可操作）
constexpr const char* kIntentionalChangeHint =
    "if this public surface change is intentional: update the frame contract / ADR and this "
    "frozen table in the same commit, then explain the change in the PR; otherwise revert it";

/// @brief 平台内部帧的追加规则提示（只允许追加、不允许改旧值）
constexpr const char* kPlatformFrameHint =
    "platform-internal frames may be appended, but existing frame numbers must never change; if "
    "the change is intentional: update the frame contract / ADR and this frozen table in the "
    "same commit, then explain it in the PR";

// ============================================================
// 通用文本工具
// ============================================================

/// @brief 仓库根（测试从 build 目录运行，向上找 .git；写法与 public_header_coverage_test.cpp 一致）
///
/// 不抽公共测试头：这两个测试目标各自独立编译，共享 3 个 15 行 helper 会引入一个新的跨目标
/// 依赖面（头放哪、改谁验证、两个 target 的编译顺序），收益不抵成本；两处都保持"从 .git 向
/// 上找根"这一条唯一口径，写错会在本测试的 ASSERT_FALSE 上直接暴露。
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

/// @brief 读取仓库内文本文件；文件不存在/不可读时返回空串，由调用方断言
std::string read_repo_file(const std::filesystem::path& root, const char* relative_path) {
    std::ifstream stream(root / relative_path, std::ios::binary);
    if (!stream) {
        return {};
    }
    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

/// @brief 标识符字符（用于 `DZ_API` / `virtual` 的词边界判断）
bool is_identifier_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

/// @brief 去掉 C/C++ 注释（保留换行，块注释内的行尾不丢失）
std::string strip_comments(std::string_view source) {
    std::string stripped;
    stripped.reserve(source.size());
    bool in_line_comment = false;
    bool in_block_comment = false;
    for (std::size_t i = 0; i < source.size(); ++i) {
        const char c = source[i];
        if (in_line_comment) {
            if (c == '\n') {
                in_line_comment = false;
                stripped.push_back(c);
            }
            continue;
        }
        if (in_block_comment) {
            if (c == '*' && i + 1 < source.size() && source[i + 1] == '/') {
                in_block_comment = false;
                ++i;
            } else if (c == '\n') {
                stripped.push_back(c);
            }
            continue;
        }
        if (c == '/' && i + 1 < source.size() && source[i + 1] == '/') {
            in_line_comment = true;
            ++i;
            continue;
        }
        if (c == '/' && i + 1 < source.size() && source[i + 1] == '*') {
            in_block_comment = true;
            ++i;
            continue;
        }
        stripped.push_back(c);
    }
    return stripped;
}

/// @brief 空白归一化：连续空白折叠为单空格，去掉首尾空白
std::string normalize_space(std::string_view text) {
    std::string normalized;
    normalized.reserve(text.size());
    bool pending_space = false;
    for (const char c : text) {
        if (std::isspace(static_cast<unsigned char>(c)) != 0) {
            pending_space = !normalized.empty();
            continue;
        }
        if (pending_space) {
            normalized.push_back(' ');
            pending_space = false;
        }
        normalized.push_back(c);
    }
    return normalized;
}

/// @brief 把差异列表渲染成多行失败信息（每行缩进两格，便于 gtest 输出阅读）
std::string join_problems(const std::vector<std::string>& problems) {
    std::string joined;
    for (const std::string& problem : problems) {
        joined += "  " + problem + "\n";
    }
    return joined;
}

// ============================================================
// version script 白名单解析（exported_symbols.map）
// ============================================================

/// @brief 链接器 version script 的两段白名单（已去掉注释、花括号与空 token）
struct VersionScriptWhitelist {
    bool global_found = false;                          ///< 存在 `global:` 段
    bool local_found = false;                           ///< 存在 `local:` 段
    std::vector<std::string> global_entries;            ///< `global:` 下的符号模式（不含分号）
    std::vector<std::string> local_entries;             ///< `local:` 下的符号模式（不含分号）
    std::vector<std::string> unknown_sections;          ///< 无法识别的段名（不是 global/local）
    std::vector<std::string> entries_outside_sections;  ///< 出现在任何段之外的条目
};

/// @brief 解析 version script：只认识 `global:` / `local:` 两段与分号结尾的条目
VersionScriptWhitelist parse_version_script(std::string_view source) {
    const std::string text = strip_comments(source);
    VersionScriptWhitelist whitelist;
    enum class Section { None, Global, Local };
    Section section = Section::None;

    std::vector<std::string> tokens;
    std::string token;
    for (const char c : text) {
        if (std::isspace(static_cast<unsigned char>(c)) != 0) {
            if (!token.empty()) {
                tokens.push_back(token);
                token.clear();
            }
            continue;
        }
        token.push_back(c);
    }
    if (!token.empty()) {
        tokens.push_back(token);
    }

    for (std::string current : tokens) {
        // 只由花括号/分号组成的 token（`{`、`}`、`};`、`;`）是结构符号，不是条目
        if (current.find_first_not_of("{};") == std::string::npos) {
            continue;
        }
        if (current.back() == ':') {
            current.pop_back();
            if (current == "global") {
                whitelist.global_found = true;
                section = Section::Global;
            } else if (current == "local") {
                whitelist.local_found = true;
                section = Section::Local;
            } else {
                whitelist.unknown_sections.push_back(current);
                section = Section::None;
            }
            continue;
        }
        if (current.back() == ';') {
            current.pop_back();
        }
        switch (section) {
            case Section::Global:
                whitelist.global_entries.push_back(current);
                break;
            case Section::Local:
                whitelist.local_entries.push_back(current);
                break;
            case Section::None:
                whitelist.entries_outside_sections.push_back(current);
                break;
        }
    }
    return whitelist;
}

/// @brief 把条目列表渲染为 `a, b` 形式（空列表返回 "(none)"）
std::string describe_entries(const std::vector<std::string>& entries) {
    if (entries.empty()) {
        return "(none)";
    }
    std::string described;
    for (const std::string& entry : entries) {
        if (!described.empty()) {
            described += ", ";
        }
        described += entry;
    }
    return described;
}

// ============================================================
// C ABI / StrategyBase / 帧宏 / 结构体解析
// ============================================================

/// @brief 抽取源文本里所有 `DZ_API <返回类型> <函数名>(...)` 的函数名（签名可跨多行）
std::vector<std::string> dz_api_function_names(std::string_view source) {
    const std::string text = strip_comments(source);
    constexpr std::string_view kMarker = "DZ_API";
    std::vector<std::string> names;
    std::size_t pos = text.find(kMarker);
    while (pos != std::string::npos) {
        const std::size_t marker_end = pos + kMarker.size();
        // 排除 `DZ_API_COMPILE_STATIC` 这类同前缀标识符
        if (marker_end < text.size() && is_identifier_char(text[marker_end])) {
            pos = text.find(kMarker, marker_end);
            continue;
        }
        std::size_t cursor = marker_end;
        while (cursor < text.size() && text[cursor] != '(' && text[cursor] != ';' &&
               text[cursor] != '{') {
            ++cursor;
        }
        if (cursor < text.size() && text[cursor] == '(') {
            std::size_t name_end = cursor;
            while (name_end > marker_end &&
                   std::isspace(static_cast<unsigned char>(text[name_end - 1])) != 0) {
                --name_end;
            }
            std::size_t name_begin = name_end;
            while (name_begin > marker_end && is_identifier_char(text[name_begin - 1])) {
                --name_begin;
            }
            names.push_back(text.substr(name_begin, name_end - name_begin));
        }
        pos = text.find(kMarker, marker_end);
    }
    return names;
}

/// @brief virtual 声明的结束位置（`= default;` 的分号，或内联函数体的右花括号）
std::size_t virtual_declaration_end(const std::string& text, std::size_t start) {
    int brace_depth = 0;
    for (std::size_t i = start; i < text.size(); ++i) {
        switch (text[i]) {
            case '{':
                ++brace_depth;
                break;
            case '}':
                --brace_depth;
                if (brace_depth == 0) {
                    return i + 1;
                }
                break;
            case ';':
                if (brace_depth == 0) {
                    return i + 1;
                }
                break;
            default:
                break;
        }
    }
    return std::string::npos;
}

/// @brief 抽取头文件里的全部 virtual 声明（去注释、空白归一化，按出现顺序）
std::vector<std::string> virtual_declarations(std::string_view source) {
    const std::string text = strip_comments(source);
    constexpr std::string_view kKeyword = "virtual";
    std::vector<std::string> declarations;
    std::size_t pos = text.find(kKeyword);
    while (pos != std::string::npos) {
        const std::size_t after_keyword = pos + kKeyword.size();
        const bool left_boundary = pos == 0 || !is_identifier_char(text[pos - 1]);
        const bool right_boundary =
            after_keyword >= text.size() || !is_identifier_char(text[after_keyword]);
        if (!left_boundary || !right_boundary) {
            pos = text.find(kKeyword, after_keyword);
            continue;
        }
        const std::size_t end = virtual_declaration_end(text, pos);
        if (end == std::string::npos) {
            break;
        }
        declarations.push_back(normalize_space(text.substr(pos, end - pos)));
        pos = text.find(kKeyword, end);
    }
    return declarations;
}

/// @brief 帧号宏名→值（只认两个书写头里的 `#define DZ_FRAME_<NAME> ((DzFrameType)<value>)`）
std::vector<std::pair<std::string, int>> parse_frame_macros(std::string_view source) {
    const std::string text = strip_comments(source);
    const std::regex pattern(
        R"(#define\s+(DZ_FRAME_[A-Z0-9_]+)\s+\(\(DzFrameType\)\s*(-?[0-9]+)\s*\))");
    std::vector<std::pair<std::string, int>> macros;
    for (std::sregex_iterator it(text.begin(), text.end(), pattern), end; it != end; ++it) {
        macros.emplace_back((*it)[1].str(), std::stoi((*it)[2].str()));
    }
    return macros;
}

/// @brief 结构体名 → 字段声明有序表（只认 `DZ_DECLARE_ALIGNED_STRUCT(Name, { ... });`）
///
/// 假设 payload 是**纯字段声明**（含数组与 `char reserved[N]`）——`DZ_DECLARE_ALIGNED_STRUCT`
/// 的语义就是 POD 字段集合，嵌套花括号会让该块解析为空，从而在本测试的逐字段比较中立刻暴露
/// （缺失结构体），不会静默通过。
std::map<std::string, std::vector<std::string>> parse_aligned_structs(std::string_view source) {
    const std::string text = strip_comments(source);
    const std::regex pattern(
        R"(DZ_DECLARE_ALIGNED_STRUCT\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*,\s*\{([^}]*)\}\s*\)\s*;)");
    std::map<std::string, std::vector<std::string>> structs;
    for (std::sregex_iterator it(text.begin(), text.end(), pattern), end; it != end; ++it) {
        std::vector<std::string> fields;
        const std::string body = (*it)[2].str();
        std::size_t field_begin = 0;
        while (field_begin <= body.size()) {
            const std::size_t field_end = body.find(';', field_begin);
            const std::size_t slice_end = field_end == std::string::npos ? body.size() : field_end;
            const std::string field = normalize_space(
                std::string_view(body).substr(field_begin, slice_end - field_begin));
            if (!field.empty()) {
                fields.push_back(field);
            }
            if (field_end == std::string::npos) {
                break;
            }
            field_begin = field_end + 1;
        }
        structs.emplace((*it)[1].str(), std::move(fields));
    }
    return structs;
}

// ============================================================
// 冻结表
// ============================================================

/// @brief 冻结的 C ABI 函数名（49 个 `dz_*`，排序后便于比对；来源 = api.h / api.cpp）
constexpr const char* kFrozenCApiFunctions[] = {
    "dz_cancel_order",
    "dz_db_close",
    "dz_db_open",
    "dz_db_open_default",
    "dz_db_query_bar",
    "dz_db_query_instruments",
    "dz_db_query_order",
    "dz_db_query_position",
    "dz_db_query_trade",
    "dz_db_query_trading_account",
    "dz_errcode",
    "dz_errmsg",
    "dz_errstr",
    "dz_init",
    "dz_next_event",
    "dz_next_md",
    "dz_notify_ui",
    "dz_output_ui",
    "dz_place_order",
    "dz_query_account_status",
    "dz_release",
    "dz_resultset_close",
    "dz_resultset_column_count",
    "dz_resultset_column_name",
    "dz_resultset_column_type",
    "dz_resultset_get_bool",
    "dz_resultset_get_float64",
    "dz_resultset_get_int64",
    "dz_resultset_get_string",
    "dz_resultset_is_null",
    "dz_resultset_next",
    "dz_resultset_status",
    "dz_schedule_after",
    "dz_schedule_at",
    "dz_schedule_cancel",
    "dz_schedule_cancel_all",
    "dz_schedule_daily",
    "dz_schedule_every",
    "dz_set_logical_position",
    "dz_strategy_home",
    "dz_strategy_id",
    "dz_subscribe",
    "dz_unsubscribe",
    "dz_version_major",
    "dz_version_minor",
    "dz_version_patch",
    "dz_version_string",
    "dz_wait",
    "dz_wakeup",
};

/// @brief 冻结的 StrategyBase virtual 声明（虚析构 + 11 个回调，按出现顺序）
///
/// 归一化规则与 `virtual_declarations()` 一致：去注释、连续空白折叠为单空格。内联空实现的
/// 函数体也一并冻结——默认实现同样是基类承诺的一部分（例如 on_start 的 `(void)ctx;`）。
constexpr const char* kFrozenStrategyBaseVirtuals[] = {
    "virtual ~StrategyBase() = default;",
    "virtual void on_start(DzContext* ctx) { (void)ctx; }",
    "virtual void on_stop() {}",
    "virtual void on_tick(const DzTick& tick) { (void)tick; }",
    "virtual void on_trade_report(const DzTradeReport& trade_report) { (void)trade_report; }",
    "virtual void on_order_report(const DzOrderReport& order_report) { (void)order_report; }",
    "virtual void on_position_info(const DzPositionInfo& position_info) { (void)position_info; }",
    "virtual void on_trading_account(const DzTradingAccount& account) { (void)account; }",
    "virtual void on_schedule(const DzScheduleEvent& timer_event) { (void)timer_event; }",
    "virtual void on_ui_input(std::string_view data) { (void)data; }",
    "virtual void on_account_status(const DzAccountStatus& status) { (void)status; }",
    "virtual void on_error(DzErrorCode errcode, std::string_view message) { (void)errcode; "
    "(void)message; }",
};

/// @brief 冻结的策略可见帧号（唯一书写位置 = dztrader/data_type.h，精确相等）
constexpr std::pair<std::string_view, int> kFrozenStrategyFrames[] = {
    {"DZ_FRAME_SHUTDOWN", 24},         {"DZ_FRAME_TICK", 64},
    {"DZ_FRAME_ORDER_REPORT", 1002},   {"DZ_FRAME_TRADE_REPORT", 1003},
    {"DZ_FRAME_POSITION_INFO", 1004},  {"DZ_FRAME_TRADING_ACCOUNT", 1005},
    {"DZ_FRAME_ACCOUNT_STATUS", 1012}, {"DZ_FRAME_UI_INPUT", 2000},
    {"DZ_FRAME_SCHEDULE", 2003},
};

/// @brief 冻结的平台内部帧号（唯一书写位置 = core_data_type.h）
///
/// 平台内部帧允许**追加**（新帧取段内下一个空闲号），因此本表只做存在性 + 值不变检查：
/// 新增帧不会让测试红，改旧值/删除旧帧会失败。BarStrategy 框架直接依赖 32/33/1024 三个，
/// 但整张表一起冻结，避免「只保护自己用到的」留下静默改号空间。
constexpr std::pair<std::string_view, int> kFrozenCoreFrames[] = {
    {"DZ_FRAME_INVALID_FILL", 0},
    {"DZ_FRAME_PRELOAD_EVENT_SHM", 1},
    {"DZ_FRAME_PRELOAD_MD_SHM", 2},
    {"DZ_FRAME_UPDATE_SHM_EVENT_SUBSCRIBER", 3},
    {"DZ_FRAME_UPDATE_SHM_MD_SUBSCRIBER", 4},
    {"DZ_FRAME_SET_EVENT_SHM_CONFIG", 5},
    {"DZ_FRAME_RTN_EVENT_SHM_CONFIG", 6},
    {"DZ_FRAME_SET_MD_SHM_CONFIG", 7},
    {"DZ_FRAME_RTN_MD_SHM_CONFIG", 8},
    {"DZ_FRAME_SET_LOG_CONFIG", 9},
    {"DZ_FRAME_FLUSH_LOG", 10},
    {"DZ_FRAME_RTN_LOG_CONFIG", 11},
    {"DZ_FRAME_NOTIFY_UI", 12},
    {"DZ_FRAME_QUERY_FULL_SNAPSHOT", 13},
    {"DZ_FRAME_REQUEST_PROCESS_CONTROL", 14},
    {"DZ_FRAME_RTN_PROCESS_STATUS", 15},
    {"DZ_FRAME_SET_PROCESS_CONFIG", 16},
    {"DZ_FRAME_RTN_PROCESS_CONFIG", 17},
    {"DZ_FRAME_SET_AUTO_LOGIN", 18},
    {"DZ_FRAME_RTN_AUTO_LOGIN", 19},
    {"DZ_FRAME_RTN_PROGRESS", 20},
    {"DZ_FRAME_RTN_SYSMON_SNAPSHOT", 25},
    {"DZ_FRAME_SET_SYSMON_CONFIG", 26},
    {"DZ_FRAME_RTN_SYSMON_CONFIG", 27},
    {"DZ_FRAME_SET_LOGICAL_POSITION", 32},
    {"DZ_FRAME_OUTPUT_UI", 33},
    {"DZ_FRAME_SET_MD_CONFIG", 65},
    {"DZ_FRAME_RTN_MD_CONFIG", 66},
    {"DZ_FRAME_RTN_MD_STATUS", 67},
    {"DZ_FRAME_REQUEST_MD_CONNECT", 68},
    {"DZ_FRAME_REQUEST_MD_DISCONNECT", 69},
    {"DZ_FRAME_REQUEST_MD_SUBSCRIBE", 70},
    {"DZ_FRAME_QUERY_MD_SUBSCRIPTIONS", 71},
    {"DZ_FRAME_RTN_MD_SUBSCRIPTIONS", 72},
    {"DZ_FRAME_REQUEST_MD_READER_REGISTER", 73},
    {"DZ_FRAME_REQUEST_MD_READER_UNREGISTER", 74},
    {"DZ_FRAME_RTN_MD_READER_REGISTER", 75},
    {"DZ_FRAME_RTN_MD_READER_UNREGISTER", 76},
    {"DZ_FRAME_NOTIFY_MD_STARTED", 1000},
    {"DZ_FRAME_NOTIFY_MD_STOPPED", 1001},
    {"DZ_FRAME_TD_ORDER_REQ", 1024},
    {"DZ_FRAME_TD_ORDER_CANCEL_REQ", 1025},
    {"DZ_FRAME_TD_REQ_MODIFY_CONFIG", 1026},
    {"DZ_FRAME_TD_CONNECT", 1027},
    {"DZ_FRAME_TD_DISCONNECT", 1028},
    {"DZ_FRAME_TD_RTN_CONFIG", 1029},
    {"DZ_FRAME_TD_RTN_STATUS", 1030},
    {"DZ_FRAME_NOTIFY_TD_STARTED", 1031},
    {"DZ_FRAME_NOTIFY_TD_STOPPED", 1032},
    {"DZ_FRAME_NOTIFY_TD_CONNECTED", 1033},
    {"DZ_FRAME_NOTIFY_TD_DISCONNECTED", 1034},
    {"DZ_FRAME_TD_QUERY_ACCOUNT_STATUS", 1035},
    {"DZ_FRAME_TD_RISK_REJECT", 1037},
    {"DZ_FRAME_TD_TRANSFER_REQ", 1038},
    {"DZ_FRAME_TD_TRANSFER_RSP", 1039},
    {"DZ_FRAME_TD_TRANSFER_RTN", 1040},
    {"DZ_FRAME_TD_PASSWORD_UPDATE_REQ", 1041},
    {"DZ_FRAME_TD_PASSWORD_UPDATE_RSP", 1042},
};

/// @brief 冻结的结构体字段表（名称 + 字段声明有序列表）
struct FrozenStruct {
    std::string_view name;
    std::vector<std::string_view> fields;
};

/// @brief 冻结的策略可见帧 payload 结构（唯一书写位置 = dztrader/struct.h）
///
/// `DZ_DECLARE_ALIGNED_STRUCT` 已 static_assert：8 字节对齐、大小为 8 的倍数、无隐式 padding。
/// 因此字段声明有序表相等 == 布局相等（包含 `reserved` 占位字段与数组长度）。
const FrozenStruct kFrozenStrategyStructs[] = {
    {"DzTick",
     {
         "DzInstrumentId instrument_id",
         "DzDate date",
         "DzTime time",
         "double last_price",
         "DzVolume volume",
         "DzSubseconds subseconds",
         "DzLargeVolume open_interest",
         "double turnover",
         "double pre_close_price",
         "double open_price",
         "double highest_price",
         "double lowest_price",
         "double upper_limit_price",
         "double lower_limit_price",
         "double bid_price[5]",
         "double ask_price[5]",
         "DzVolume bid_volume[5]",
         "DzVolume ask_volume[5]",
     }},
    {"DzOrderReport",
     {
         "DzOrderId order_id",
         "DzStrategyId strategy_id",
         "DzInstrumentId instrument_id",
         "DzDirection direction",
         "DzPriceType price_type",
         "DzPositionEffect position_effect",
         "DzOrderStatus status",
         "char reserved[4]",
         "double price",
         "DzVolume volume",
         "DzVolume volume_traded",
         "DzDate date",
         "DzTime time",
         "DzAccountId account_id",
         "DzExchangeId exchange_id",
         "DzOrderRemark remark",
         "uint64_t seq",
     }},
    {"DzTradeReport",
     {
         "DzOrderId order_id",
         "DzStrategyId strategy_id",
         "DzInstrumentId instrument_id",
         "double price",
         "DzVolume volume",
         "DzDirection direction",
         "DzPositionEffect position_effect",
         "char reserved[2]",
         "DzDate date",
         "DzTime time",
         "DzAccountId account_id",
         "DzExchangeId exchange_id",
         "DzTradeId trade_id",
         "uint64_t seq",
     }},
    {"DzPositionInfo",
     {
         "DzInstrumentId instrument_id",
         "DzExchangeId exchange_id",
         "DzAccountId account_id",
         "DzLargeVolume volume",
         "DzLargeVolume frozen_volume",
         "double price",
         "DzLargeVolume yd_volume",
         "DzLargeVolume today_volume",
         "DzDate date",
         "DzDirection direction",
         "char reserved[3]",
         "uint64_t seq",
     }},
    {"DzTradingAccount",
     {
         "DzAccountId account_id",
         "double balance",
         "double available",
         "double frozen",
         "double commission",
         "double margin",
         "double withdraw_quota",
         "double deposit",
         "double withdraw",
         "DzDate date",
         "char reserved[4]",
         "uint64_t seq",
     }},
    {"DzFrameHeader",
     {
         "uint32_t frame_size",
         "DzFrameType frame_type",
         "char reserved[2]",
     }},
    {"DzExtInstFrameHeader",
     {
         "DzInstanceId instance_id",
         "uint32_t data_size",
         "char reserved[4]",
     }},
    {"DzExtFrameHeader",
     {
         "uint32_t data_size",
         "char reserved[4]",
     }},
    {"DzShmPreload",
     {
         "uint64_t bytes",
         "uint32_t pages",
         "uint32_t reserved",
     }},
    {"DzScheduleEvent",
     {
         "DzTimerId timer_id",
     }},
    {"DzAccountStatus",
     {
         "DzAccountId account_id",
         "DzInstanceId gateway_name",
         "DzDate trading_day",
         "DzAccountState state",
         "char reserved[3]",
     }},
};

/// @brief 冻结的平台内部帧 payload 结构（只冻结 BarStrategy 框架直接读写的两个）
///
/// 平台内部帧结构允许随功能追加（例如 DzOrderCancelReq 的后续字段），因此这里只冻结与
/// 策略框架有直接数据交换的 `DzLogicalPosition`（SET_LOGICAL_POSITION 上报）与
/// `DzOrderReq`（TD_ORDER_REQ 下单）——它们的布局变化会静默改掉策略发出的帧语义。
const FrozenStruct kFrozenCoreStructs[] = {
    {"DzOrderReq",
     {
         "DzOrderId order_id",
         "DzStrategyId strategy_id",
         "DzAccountId account_id",
         "DzInstrumentId instrument_id",
         "DzOrderRemark remark",
         "double price",
         "DzVolume volume",
         "DzDirection direction",
         "DzPriceType price_type",
         "DzPositionEffect position_effect",
         "char reserved[1]",
     }},
    {"DzLogicalPosition",
     {
         "DzAccountId account_id",
         "DzInstrumentId instrument_id",
         "DzStrategyId strategy_id",
         "DzLargeVolume net_volume",
     }},
};

// ============================================================
// 引擎入口的编译期探针（#165 AC4 第 4 项）
// ============================================================

/// @brief 满足 dztrader::Strategy 的最小策略（只有必选回调 on_start）
struct ProbeStrategy {
    void on_start(DzContext* ctx) { (void)ctx; }
};

/// @brief 满足 dztrader::BarStrategy 的探针策略（六个 B 回调全实现，均为空操作）
struct ProbeBarStrategy {
    void on_init(dztrader::InstrumentContext& ctx) { (void)ctx; }
    void on_bar_open(dztrader::InstrumentContext& ctx) { (void)ctx; }
    void on_bar_update(dztrader::InstrumentContext& ctx) { (void)ctx; }
    void on_bar_close(dztrader::InstrumentContext& ctx) { (void)ctx; }
    void on_params_update(dztrader::InstrumentContext& ctx) { (void)ctx; }
    void on_destroy(dztrader::InstrumentContext& ctx) { (void)ctx; }
};

// ============================================================
// 测试用比较 helper（先定义后使用；测试主体只留断言）
// ============================================================

/// @brief C ABI 集合比较：frozen 中缺失 / actual 中多出都记一条问题
void append_c_api_diff(std::vector<std::string>& problems,
                       const char* file,
                       const std::set<std::string>& frozen,
                       const std::set<std::string>& actual) {
    for (const std::string& name : frozen) {
        if (!actual.contains(name)) {
            problems.push_back("missing " + std::string(file) + ": " + name);
        }
    }
    for (const std::string& name : actual) {
        if (!frozen.contains(name)) {
            problems.push_back("unexpected " + std::string(file) + ": " + name +
                               " (not in the frozen C ABI list)");
        }
    }
}

/// @brief 把冻结的帧号数组转成 `name -> value` 映射
template <std::size_t N>
std::map<std::string, int> frozen_frame_map(const std::pair<std::string_view, int> (&frames)[N]) {
    std::map<std::string, int> map;
    for (const auto& [name, value] : frames) {
        map.emplace(name, value);
    }
    return map;
}

/// @brief 把解析出的帧宏数组转成 `name -> value` 映射
std::map<std::string, int> to_frame_map(const std::vector<std::pair<std::string, int>>& macros) {
    std::map<std::string, int> map;
    for (const auto& [name, value] : macros) {
        map.emplace(name, value);
    }
    return map;
}

/// @brief 帧号精确比较：缺项 / 改值 / 新增都算失败（策略可见帧表必须一字不差）
void append_frame_diff_exact(std::vector<std::string>& problems,
                             const std::map<std::string, int>& expected,
                             const std::map<std::string, int>& actual) {
    for (const auto& [name, value] : expected) {
        const auto it = actual.find(name);
        if (it == actual.end()) {
            problems.push_back("missing `" + name + "` (frozen value " + std::to_string(value) +
                               ")");
        } else if (it->second != value) {
            problems.push_back("changed `" + name + "` expected=" + std::to_string(value) +
                               " actual=" + std::to_string(it->second));
        }
    }
    for (const auto& [name, value] : actual) {
        if (!expected.contains(name)) {
            problems.push_back("unexpected `" + name + "`=" + std::to_string(value) +
                               " (new strategy-visible frame needs contract review)");
        }
    }
}

/// @brief 帧号存在性 + 值比较：允许新增（平台内部帧追加），缺项 / 改值失败
void append_frame_diff_existing(std::vector<std::string>& problems,
                                const std::map<std::string, int>& expected,
                                const std::map<std::string, int>& actual) {
    for (const auto& [name, value] : expected) {
        const auto it = actual.find(name);
        if (it == actual.end()) {
            problems.push_back("missing `" + name + "` (frozen value " + std::to_string(value) +
                               ")");
        } else if (it->second != value) {
            problems.push_back("changed `" + name + "` expected=" + std::to_string(value) +
                               " actual=" + std::to_string(it->second));
        }
    }
}

/// @brief 结构体字段表逐字段比较：结构体缺失 / 字段数量 / 字段序号内容都记一条问题
void append_struct_diff(std::vector<std::string>& problems,
                        const char* file,
                        const FrozenStruct& frozen,
                        const std::map<std::string, std::vector<std::string>>& actual_structs) {
    const auto it = actual_structs.find(std::string(frozen.name));
    if (it == actual_structs.end()) {
        problems.push_back("missing struct `" + std::string(frozen.name) + "` in " + file);
        return;
    }
    const std::vector<std::string>& actual = it->second;
    if (actual.size() != frozen.fields.size()) {
        problems.push_back("struct `" + std::string(frozen.name) +
                           "` field count expected=" + std::to_string(frozen.fields.size()) +
                           " actual=" + std::to_string(actual.size()));
    }
    const std::size_t common = std::min(actual.size(), frozen.fields.size());
    for (std::size_t i = 0; i < common; ++i) {
        if (actual[i] != frozen.fields[i]) {
            problems.push_back("struct `" + std::string(frozen.name) + "` field[" +
                               std::to_string(i) + "] expected=`" + std::string(frozen.fields[i]) +
                               "` actual=`" + actual[i] + "`");
        }
    }
    for (std::size_t i = common; i < frozen.fields.size(); ++i) {
        problems.push_back("struct `" + std::string(frozen.name) + "` missing field[" +
                           std::to_string(i) + "] `" + std::string(frozen.fields[i]) + "`");
    }
    for (std::size_t i = common; i < actual.size(); ++i) {
        problems.push_back("struct `" + std::string(frozen.name) + "` unexpected field[" +
                           std::to_string(i) + "] `" + actual[i] + "`");
    }
}

// ============================================================
// 测试
// ============================================================

/**
 * @brief 动态库只导出 dz_* C ABI：version script 白名单结构被冻结
 *
 * ADR 0038 决策 7 用链接器白名单（而非 -fvisibility=hidden）把内部 C++ 与第三方符号本地化。
 * 白名单一旦被「顺手」扩成 `*;` 或加进非 dz_* 前缀条目，导出面就回到 13038 个符号——链接器
 * 不会报错，只有本测试会。新增公开导出属于契约变更，必须先走 ADR/契约评审。
 */
TEST(PublicSurfaceInvariants, ExportedSymbolWhitelistIsCAbiOnly) {
    const std::filesystem::path root = repo_root();
    const std::string text = read_repo_file(root, kExportedSymbolsMap);
    ASSERT_FALSE(text.empty()) << "cannot read " << kExportedSymbolsMap << " under "
                               << root.generic_string();

    const VersionScriptWhitelist whitelist = parse_version_script(text);
    std::vector<std::string> problems;
    if (!whitelist.global_found) {
        problems.emplace_back("`global:` section is missing");
    }
    if (!whitelist.local_found) {
        problems.emplace_back("`local:` section is missing");
    }
    if (!(whitelist.global_entries.size() == 1 && whitelist.global_entries.front() == "dz_*")) {
        problems.push_back("`global:` must contain exactly one entry `dz_*`; actual entries: [" +
                           describe_entries(whitelist.global_entries) + "]");
    }
    if (!(whitelist.local_entries.size() == 1 && whitelist.local_entries.front() == "*")) {
        problems.push_back("`local:` must contain exactly one entry `*`; actual entries: [" +
                           describe_entries(whitelist.local_entries) + "]");
    }
    for (const std::string& section : whitelist.unknown_sections) {
        problems.push_back("unknown version-script section `" + section +
                           ":` (only `global:` and `local:` are recognized)");
    }
    for (const std::string& entry : whitelist.entries_outside_sections) {
        problems.push_back("entry `" + entry + "` appears outside any section");
    }

    EXPECT_TRUE(problems.empty()) << "the dynamic-library export whitelist changed ("
                                  << kExportedSymbolsMap << "):\n"
                                  << join_problems(problems)
                                  << "new exports must go through ADR / contract review: "
                                  << kIntentionalChangeHint;
}

/**
 * @brief C ABI 冻结：api.h 声明与 api.cpp 定义都精确等于 49 个冻结的 dz_*
 *
 * 两个方向都查：删函数（声明没了/定义没了）与加函数（多一个 dz_*）都会失败。名字集合相等
 * 也隐含了「声明与定义一一对应」——只加声明不实现、或只实现不声明，都会在这里红。
 * Windows 侧无法用 nm/dumpbin 检查 dll 导出，因此「源头冻结」是跨平台可执行的唯一形态。
 */
TEST(PublicSurfaceInvariants, CApiSurfaceIsFrozen) {
    const std::filesystem::path root = repo_root();
    const std::string header_text = read_repo_file(root, kCApiHeader);
    ASSERT_FALSE(header_text.empty())
        << "cannot read " << kCApiHeader << " under " << root.generic_string();
    const std::string source_text = read_repo_file(root, kCApiSource);
    ASSERT_FALSE(source_text.empty())
        << "cannot read " << kCApiSource << " under " << root.generic_string();

    const std::vector<std::string> header_names = dz_api_function_names(header_text);
    const std::vector<std::string> source_names = dz_api_function_names(source_text);
    ASSERT_FALSE(header_names.empty()) << "no DZ_API function names parsed from " << kCApiHeader
                                       << " (parser expects `DZ_API <return-type> dz_name(...)`)";
    ASSERT_FALSE(source_names.empty()) << "no DZ_API function names parsed from " << kCApiSource
                                       << " (parser expects `DZ_API <return-type> dz_name(...) {`)";

    const std::set<std::string> header_set(header_names.begin(), header_names.end());
    const std::set<std::string> source_set(source_names.begin(), source_names.end());
    const std::set<std::string> frozen_set(std::begin(kFrozenCApiFunctions),
                                           std::end(kFrozenCApiFunctions));

    std::vector<std::string> problems;
    append_c_api_diff(problems, kCApiHeader, frozen_set, header_set);
    append_c_api_diff(problems, kCApiSource, frozen_set, source_set);

    EXPECT_TRUE(problems.empty()) << "the C ABI surface changed (frozen list has "
                                  << frozen_set.size() << " dz_* functions):\n"
                                  << join_problems(problems) << kIntentionalChangeHint;
}

/**
 * @brief StrategyBase 冻结：虚析构 + 11 个回调的声明序列不变
 *
 * 基类是引擎鸭子类型协议的可选「省写空实现」载体，回调签名/顺序/默认实现都是公开面。
 * 去注释、空白归一化后按出现顺序比较——插入、删除、重排、改签名、改默认实现都会失败。
 * 新增回调必须先确认引擎有对应探测与分发（跨头文件变更），因此同样需要契约评审。
 */
TEST(PublicSurfaceInvariants, StrategyBaseSurfaceIsFrozen) {
    const std::filesystem::path root = repo_root();
    const std::string text = read_repo_file(root, kStrategyBaseHeader);
    ASSERT_FALSE(text.empty()) << "cannot read " << kStrategyBaseHeader << " under "
                               << root.generic_string();

    const std::vector<std::string> actual = virtual_declarations(text);
    ASSERT_FALSE(actual.empty()) << "no virtual declarations parsed from " << kStrategyBaseHeader;

    std::vector<std::string> problems;
    const std::size_t frozen_count = std::size(kFrozenStrategyBaseVirtuals);
    const std::size_t common = std::min(frozen_count, actual.size());
    for (std::size_t i = 0; i < common; ++i) {
        if (actual[i] != kFrozenStrategyBaseVirtuals[i]) {
            problems.push_back("virtual declaration[" + std::to_string(i) + "] expected=`" +
                               kFrozenStrategyBaseVirtuals[i] + "` actual=`" + actual[i] + "`");
        }
    }
    for (std::size_t i = common; i < frozen_count; ++i) {
        problems.push_back("missing virtual declaration[" + std::to_string(i) + "] `" +
                           kFrozenStrategyBaseVirtuals[i] + "`");
    }
    for (std::size_t i = common; i < actual.size(); ++i) {
        problems.push_back("unexpected virtual declaration[" + std::to_string(i) + "] `" +
                           actual[i] + "`");
    }

    EXPECT_TRUE(problems.empty()) << "the StrategyBase callback surface changed ("
                                  << kStrategyBaseHeader << "):\n"
                                  << join_problems(problems) << kIntentionalChangeHint;
}

/**
 * @brief 引擎入口冻结（编译期）：模板签名 / concept / StrategyBase 大小
 *
 * 只取函数指针类型与 concept，不调用入口、不 ODR-use，因此本测试**不链接**任何 dz_* 符号
 * （它不属于 SDK_API_TESTS，也不该属于）。`run_strategy` 与 `run_bar_strategy` 的返回类型、
 * 参数形态是策略 `main()` 直接依赖的契约：改签名会让所有下游策略二进制编译失败，必须在
 * 契约层评审后才能动。
 */
TEST(PublicSurfaceInvariants, EngineEntryPointsAreFrozen) {
    static_assert(dztrader::Strategy<ProbeStrategy>,
                  "run_strategy entry contract changed: ProbeStrategy no longer satisfies "
                  "dztrader::Strategy");
    static_assert(dztrader::BarStrategy<ProbeBarStrategy>,
                  "run_bar_strategy entry contract changed: ProbeBarStrategy no longer satisfies "
                  "dztrader::BarStrategy");
    static_assert(std::is_same_v<decltype(&dztrader::run_strategy<ProbeStrategy>),
                                 std::int32_t (*)(ProbeStrategy&)>,
                  "dztrader::run_strategy signature changed (expected int32_t(Strategy&))");
    static_assert(
        std::is_same_v<decltype(&dztrader::run_bar_strategy<ProbeBarStrategy>), std::int32_t (*)()>,
        "dztrader::run_bar_strategy signature changed (expected int32_t())");
    static_assert(sizeof(dztrader::StrategyBase) == sizeof(void*),
                  "StrategyBase layout changed: it must stay a plain vptr-only base class");

    // 运行时再断言一次：只有 static_assert 的 TEST 在运行期是空壳（失败只会出现在编译期），
    // 读测试日志的人看不出它到底检了什么。这里让「检查过」本身也出现在用例输出里。
    EXPECT_TRUE((std::is_same_v<decltype(&dztrader::run_strategy<ProbeStrategy>),
                                std::int32_t (*)(ProbeStrategy&)>));
    EXPECT_TRUE((std::is_same_v<decltype(&dztrader::run_bar_strategy<ProbeBarStrategy>),
                                std::int32_t (*)()>));
    EXPECT_TRUE(dztrader::Strategy<ProbeStrategy>);
    EXPECT_TRUE(dztrader::BarStrategy<ProbeBarStrategy>);
    EXPECT_EQ(sizeof(dztrader::StrategyBase), sizeof(void*));
}

/**
 * @brief 帧号冻结：策略可见帧精确相等，平台内部帧存在且旧值不变
 *
 * 帧号的值只写在两个头文件里（data_type.h / core_data_type.h 的「唯一书写位置」约定），
 * 契约、注释、测试其余位置一律引用名字。这里扫描的正是那两处书写源：
 *   - 策略可见帧（data_type.h）：**精确相等**，新增/删除/改值都失败——增删帧号是协议变更；
 *   - 平台内部帧（core_data_type.h）：存在性 + 值不变，**允许追加**（段内下一个空闲号）；
 *   - 两个书写位置的帧名不得重复（每帧只能有一个书写位置）。
 */
TEST(PublicSurfaceInvariants, FrameNumbersAreFrozen) {
    const std::filesystem::path root = repo_root();

    // ── 策略可见帧：精确相等 ──
    const std::string strategy_text = read_repo_file(root, kStrategyDataTypeHeader);
    ASSERT_FALSE(strategy_text.empty())
        << "cannot read " << kStrategyDataTypeHeader << " under " << root.generic_string();
    const std::vector<std::pair<std::string, int>> parsed_strategy =
        parse_frame_macros(strategy_text);
    const std::map<std::string, int> actual_strategy = to_frame_map(parsed_strategy);
    ASSERT_FALSE(actual_strategy.empty())
        << "no DZ_FRAME_* macros parsed from " << kStrategyDataTypeHeader;
    // map.emplace 会静默吞掉同名重复项：同一书写位置里把一帧写两次也破坏「每帧一个书写位置」。
    ASSERT_EQ(parsed_strategy.size(), actual_strategy.size())
        << "duplicate DZ_FRAME_* #define in " << kStrategyDataTypeHeader
        << " (each frame must be defined exactly once in its single writing location)";

    const std::map<std::string, int> frozen_strategy = frozen_frame_map(kFrozenStrategyFrames);
    std::vector<std::string> strategy_problems;
    append_frame_diff_exact(strategy_problems, frozen_strategy, actual_strategy);
    EXPECT_TRUE(strategy_problems.empty())
        << "strategy-visible frame numbers changed (" << kStrategyDataTypeHeader << "):\n"
        << join_problems(strategy_problems)
        << "a new or removed strategy-visible frame is a protocol change: "
        << kIntentionalChangeHint;

    // ── 平台内部帧：存在 + 旧值不变（允许追加） ──
    const std::string core_text = read_repo_file(root, kCoreDataTypeHeader);
    ASSERT_FALSE(core_text.empty())
        << "cannot read " << kCoreDataTypeHeader << " under " << root.generic_string();
    const std::vector<std::pair<std::string, int>> parsed_core = parse_frame_macros(core_text);
    const std::map<std::string, int> actual_core = to_frame_map(parsed_core);
    ASSERT_FALSE(actual_core.empty()) << "no DZ_FRAME_* macros parsed from " << kCoreDataTypeHeader;
    ASSERT_EQ(parsed_core.size(), actual_core.size())
        << "duplicate DZ_FRAME_* #define in " << kCoreDataTypeHeader
        << " (each frame must be defined exactly once in its single writing location)";

    const std::map<std::string, int> frozen_core = frozen_frame_map(kFrozenCoreFrames);
    std::vector<std::string> core_problems;
    append_frame_diff_existing(core_problems, frozen_core, actual_core);
    EXPECT_TRUE(core_problems.empty())
        << "existing platform-internal frame numbers changed (" << kCoreDataTypeHeader << "):\n"
        << join_problems(core_problems) << kPlatformFrameHint;

    // ── 两个唯一书写位置不允许重复定义同名帧 ──
    std::vector<std::string> duplicate_problems;
    for (const auto& [name, value] : actual_core) {
        if (actual_strategy.contains(name)) {
            duplicate_problems.push_back("frame `" + name + "` is defined in both " +
                                         kStrategyDataTypeHeader + " and " + kCoreDataTypeHeader +
                                         " (each frame must have exactly one writing location)");
            (void)value;
        }
    }
    EXPECT_TRUE(duplicate_problems.empty())
        << "frame writing-location invariant broken:\n"
        << join_problems(duplicate_problems) << kIntentionalChangeHint;
}

/**
 * @brief 帧 payload 结构冻结：字段声明有序表逐字段一致
 *
 * 对齐宏已把「8 字节对齐 / 无 padding / 大小为 8 的倍数」变成编译期约束，所以在字段表层面
 * 比较即等价于布局比较。失败信息给出结构体名、字段序号、期望与实际，便于直接定位是
 * 哪个结构体、第几个字段变了（含 `reserved` 占位与数组长度）。
 */
TEST(PublicSurfaceInvariants, FramePayloadStructsAreFrozen) {
    const std::filesystem::path root = repo_root();

    const std::string strategy_text = read_repo_file(root, kStrategyStructHeader);
    ASSERT_FALSE(strategy_text.empty())
        << "cannot read " << kStrategyStructHeader << " under " << root.generic_string();
    const std::map<std::string, std::vector<std::string>> strategy_structs =
        parse_aligned_structs(strategy_text);
    ASSERT_FALSE(strategy_structs.empty())
        << "no DZ_DECLARE_ALIGNED_STRUCT blocks parsed from " << kStrategyStructHeader;

    // 冻结表是「完整集合」：struct.h 是策略可见帧 payload 的唯一书写位置，新增一个对齐结构体
    // 就是新增一种公开帧布局——必须同提交更新冻结表（不存在"合法新增但无需登记"的路径）。
    std::vector<std::string> unexpected_structs;
    for (const auto& [name, fields] : strategy_structs) {
        const bool frozen =
            std::ranges::any_of(kFrozenStrategyStructs,
                                [&name](const FrozenStruct& entry) { return entry.name == name; });
        if (!frozen) {
            unexpected_structs.push_back(name + " (" + std::to_string(fields.size()) + " fields)");
        }
    }
    EXPECT_TRUE(unexpected_structs.empty())
        << kStrategyStructHeader << " contains aligned structs missing from the frozen table:\n"
        << join_problems(unexpected_structs)
        << "a new frame payload struct is a public surface change: " << kIntentionalChangeHint;

    std::vector<std::string> strategy_problems;
    for (const FrozenStruct& frozen : kFrozenStrategyStructs) {
        append_struct_diff(strategy_problems, kStrategyStructHeader, frozen, strategy_structs);
    }
    EXPECT_TRUE(strategy_problems.empty())
        << "frame payload structures changed (" << kStrategyStructHeader << "):\n"
        << join_problems(strategy_problems) << kIntentionalChangeHint;

    const std::string core_text = read_repo_file(root, kCoreStructHeader);
    ASSERT_FALSE(core_text.empty())
        << "cannot read " << kCoreStructHeader << " under " << root.generic_string();
    const std::map<std::string, std::vector<std::string>> core_structs =
        parse_aligned_structs(core_text);
    ASSERT_FALSE(core_structs.empty())
        << "no DZ_DECLARE_ALIGNED_STRUCT blocks parsed from " << kCoreStructHeader;

    std::vector<std::string> core_problems;
    for (const FrozenStruct& frozen : kFrozenCoreStructs) {
        append_struct_diff(core_problems, kCoreStructHeader, frozen, core_structs);
    }
    EXPECT_TRUE(core_problems.empty())
        << "frame payload structures changed (" << kCoreStructHeader << "):\n"
        << join_problems(core_problems) << kIntentionalChangeHint;
}

}  // namespace
