#include "stg_demo_strategy.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <vector>

namespace dztrader::demo {

namespace {

/// UI 输出行格式: 行首 ASCII 类别标签 + key=value + 中文正文;
/// 含空格的值加双引号。类别集见契约 strategy (UI_INPUT/OUTPUT_UI 无固定 schema,
/// 格式由策略自行约定; 稳定锚点供测试/冒烟机器断言)。

constexpr std::string_view kOrderUsage = "order <账户> <合约> <价格> <手数> [long|short]";

/// 行情源名: SDK 不提供查询 API; master 注入 DZTRADER_MD_SOURCE,
/// dz_init 成功即保证已注入且非空 (缺失/为空返回 DZ_EC_MD_SOURCE_NOT_CONFIGURED)。
const char* md_source_name() {
    const char* const value = std::getenv("DZTRADER_MD_SOURCE");
    return value != nullptr ? value : "";
}

/// 空白分词: 连续空白折叠, 无引号/转义 (UI_INPUT 文本命令语法)
std::vector<std::string_view> split_ws(std::string_view text) {
    std::vector<std::string_view> tokens;
    std::size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i])) != 0) {
            ++i;
        }
        if (i >= text.size()) {
            break;
        }
        const std::size_t start = i;
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i])) == 0) {
            ++i;
        }
        tokens.push_back(text.substr(start, i - start));
    }
    return tokens;
}

bool parse_double(std::string_view text, double& out) {
    const char* first = text.data();
    const char* last = text.data() + text.size();
    const auto result = std::from_chars(first, last, out);
    return result.ec == std::errc{} && result.ptr == last && std::isfinite(out);
}

bool parse_int64(std::string_view text, int64_t& out) {
    const char* first = text.data();
    const char* last = text.data() + text.size();
    const auto result = std::from_chars(first, last, out);
    return result.ec == std::errc{} && result.ptr == last;
}

/// 正整数 (md dump 的条数参数)
bool parse_positive_size(std::string_view text, std::size_t& out) {
    int64_t value = 0;
    if (!parse_int64(text, value) || value <= 0) {
        return false;
    }
    out = static_cast<std::size_t>(value);
    return true;
}

/// 逗号连接 (instruments 列表输出)
std::string join_csv(const std::vector<std::string>& parts) {
    std::string joined;
    for (const auto& part : parts) {
        if (!joined.empty()) {
            joined += ',';
        }
        joined += part;
    }
    return joined;
}

std::vector<std::string> to_strings(std::span<const std::string_view> views) {
    std::vector<std::string> strings;
    strings.reserve(views.size());
    for (const std::string_view view : views) {
        strings.emplace_back(view);
    }
    return strings;
}

/// std::string 容器 -> C 接口用的 c_str 指针数组 (strings 须存活到调用结束)
std::vector<const char*> to_c_strs(const std::vector<std::string>& strings) {
    std::vector<const char*> pointers;
    pointers.reserve(strings.size());
    for (const auto& string : strings) {
        pointers.push_back(string.c_str());
    }
    return pointers;
}

std::string format_time(DzTime time, DzSubseconds subseconds) {
    // 时间字段异常 (负值) 归一为 0, 保证输出格式确定 (演示工具不做时间校验)
    const int32_t secs = time > 0 ? time : 0;
    const int32_t micros = subseconds > 0 ? subseconds : 0;
    return std::format("{:02d}:{:02d}:{:02d}.{:06d}", secs / 3600, secs % 3600 / 60, secs % 60,
                       micros);
}

std::string format_date(DzDate date) {
    if (date <= 0) {
        return "-";  // 0 = 无有效交易日 (Offline/兜底应答)
    }
    const std::chrono::year_month_day ymd{std::chrono::sys_days{std::chrono::days{date}}};
    return std::format("{:04d}-{:02d}-{:02d}", static_cast<int>(ymd.year()),
                       static_cast<unsigned>(ymd.month()), static_cast<unsigned>(ymd.day()));
}

const char* direction_name(DzDirection direction) {
    switch (direction) {
        case DZ_DIRECTION_LONG:
            return "long(买)";
        case DZ_DIRECTION_SHORT:
            return "short(卖)";
        default:
            return "net(净)";
    }
}

const char* position_effect_name(DzPositionEffect effect) {
    switch (effect) {
        case DZ_POSITION_EFFECT_OPEN:
            return "open(开仓)";
        case DZ_POSITION_EFFECT_CLOSE:
            return "close(平仓)";
        case DZ_POSITION_EFFECT_CLOSE_TODAY:
            return "close_today(平今)";
        case DZ_POSITION_EFFECT_CLOSE_YESTDAY:
            return "close_yestday(平昨)";
        default:
            return "auto(自动)";
    }
}

const char* order_status_name(DzOrderStatus status) {
    switch (status) {
        case DZ_ORDER_SUBMITTING:
            return "SUBMITTING(提交中)";
        case DZ_ORDER_NOT_TRADED:
            return "NOT_TRADED(未成交)";
        case DZ_ORDER_PART_TRADED:
            return "PART_TRADED(部分成交)";
        case DZ_ORDER_ALL_TRADED:
            return "ALL_TRADED(全部成交)";
        case DZ_ORDER_CANCELLED:
            return "CANCELLED(已撤销)";
        case DZ_ORDER_REJECTED:
            return "REJECTED(拒单)";
        default:
            return "UNKNOWN(未知)";
    }
}

const char* account_state_name(DzAccountState state) {
    switch (state) {
        case DZ_ACCOUNT_OFFLINE:
            return "offline(离线)";
        case DZ_ACCOUNT_LOGGING_IN:
            return "logging_in(登录中)";
        case DZ_ACCOUNT_READY:
            return "ready(就绪)";
        default:
            return "unknown(未知)";
    }
}

/// tick 输出的时间正序键 (日期 + 当日秒 + 微秒)
auto tick_time_key(const DzTick& tick) { return std::tuple{tick.date, tick.time, tick.subseconds}; }

/// 价格格式化: CTP 行情无值/无效档位用 DBL_MAX 哨兵透传 (md_spi 注释: 由消费者处理),
/// 演示输出归一为 "-", 避免 300+ 位数字撑爆一行并干扰 dump 可读性
std::string format_price(double price) {
    if (!std::isfinite(price) || std::fabs(price) >= std::numeric_limits<double>::max() / 2.0) {
        return "-";
    }
    return std::format("{:.3f}", price);
}

std::string format_tick_line(const DzTick& tick) {
    return std::format(
        "[tick] instrument={} time={} last={} volume={} bid1={}/{} ask1={}/{} "
        "oi={} 缓存行情",
        tick.instrument_id, format_time(tick.time, tick.subseconds), format_price(tick.last_price),
        tick.volume, format_price(tick.bid_price[0]), tick.bid_volume[0],
        format_price(tick.ask_price[0]), tick.ask_volume[0], tick.open_interest);
}

}  // namespace

void DemoStrategy::on_start(DzContext* ctx) {
    ctx_ = ctx;
    emit_line(std::format(
        "[ready] strategy={} md_source={} sdk={} home={} 示例策略就绪，输入 help 查看命令",
        dz_strategy_id(ctx), md_source_name(), dz_version_string(), dz_strategy_home(ctx)));
}

void DemoStrategy::on_stop() {
    if (ctx_ == nullptr) {
        return;
    }
    emit_line(std::format("[stop] strategy={} reason={} 策略已停止", dz_strategy_id(ctx_),
                          quit_ ? "self" : "shutdown"));
}

void DemoStrategy::on_ui_input(std::string_view data) { handle_command(data); }

void DemoStrategy::on_tick(const DzTick& tick) {
    const auto it = tick_cache_.find(std::string_view(tick.instrument_id));
    if (it == tick_cache_.end()) {
        return;  // 未订阅合约的 tick 不缓存 (缓存 key 集合 = 订阅集合)
    }
    auto& cache = it->second;
    cache.push_back(tick);  // 回调参数仅本次回调有效, 必须拷贝
    if (cache.size() > kTickCacheCapacity) {
        cache.pop_front();
    }
}

void DemoStrategy::on_order_report(const DzOrderReport& report) {
    emit_line(std::format(
        "[report] order_id={} strategy={} account={} instrument={} direction={} price_type={} "
        "price={:.3f} volume={} traded={} status={} remark=\"{}\"",
        report.order_id, report.strategy_id, report.account_id, report.instrument_id,
        direction_name(report.direction), static_cast<int>(report.price_type), report.price,
        report.volume, report.volume_traded, order_status_name(report.status), report.remark));
}

void DemoStrategy::on_trade_report(const DzTradeReport& report) {
    emit_line(std::format(
        "[trade] trade_id={} order_id={} account={} instrument={} direction={} effect={} "
        "price={:.3f} volume={} 成交回报",
        report.trade_id, report.order_id, report.account_id, report.instrument_id,
        direction_name(report.direction), position_effect_name(report.position_effect),
        report.price, report.volume));
}

void DemoStrategy::on_position_info(const DzPositionInfo& info) {
    emit_line(std::format(
        "[position] account={} instrument={} direction={} volume={} frozen={} price={:.3f} "
        "持仓信息",
        info.account_id, info.instrument_id, direction_name(info.direction), info.volume,
        info.frozen_volume, info.price));
}

void DemoStrategy::on_trading_account(const DzTradingAccount& account) {
    emit_line(
        std::format("[account] action=fund account={} balance={:.2f} available={:.2f} "
                    "frozen={:.2f} margin={:.2f} commission={:.2f} 资金信息",
                    account.account_id, account.balance, account.available, account.frozen,
                    account.margin, account.commission));
}

void DemoStrategy::on_account_status(const DzAccountStatus& status) {
    emit_line(std::format("[account] action=status account={} gateway={} state={} trading_day={}",
                          status.account_id, status.gateway_name, account_state_name(status.state),
                          format_date(status.trading_day)));
}

void DemoStrategy::on_error(DzErrorCode errcode, std::string_view message) {
    if (ctx_ == nullptr) {
        // dz_init 失败时引擎在 on_start 之前调用 on_error: 无会话即无 UI 通道,
        // 写 stderr (英文) 保留唯一错误信息, 由 master 捕获转发进日志, 否则进程只剩退出码。
        std::cerr << std::format("strategy init failed | code={} message=\"{}\"",
                                 static_cast<int>(errcode), message)
                  << '\n';
        return;
    }
    emit_line(std::format("[error] source=engine errcode={} message=\"{}\" 引擎回调异常",
                          static_cast<int>(errcode), message));
}

void DemoStrategy::handle_command(std::string_view input) {
    if (ctx_ == nullptr) {
        return;
    }
    const std::vector<std::string_view> tokens = split_ws(input);
    if (tokens.empty()) {
        emit_line("[error] reason=empty_command 空命令，输入 help 查看命令列表");
        return;
    }
    const std::string_view cmd = tokens.front();
    const std::span<const std::string_view> args(tokens.data() + 1, tokens.size() - 1);
    if (cmd == "help") {
        cmd_help(args);
    } else if (cmd == "info") {
        cmd_info(args);
    } else if (cmd == "md") {
        cmd_md(args);
    } else if (cmd == "order") {
        cmd_order(args);
    } else if (cmd == "cancel") {
        cmd_cancel(args);
    } else if (cmd == "account") {
        cmd_account(args);
    } else if (cmd == "quit") {
        cmd_quit(args);
    } else {
        emit_error(cmd, "unknown_command", "未知命令，输入 help 查看命令列表");
    }
}

void DemoStrategy::cmd_help(std::span<const std::string_view> args) {
    if (emit_error_on_extra_args("help", args)) {
        return;
    }
    static constexpr std::string_view kHelpLines[] = {
        "[info] command=help desc=显示命令列表",
        "[info] command=info desc=显示策略身份与运行环境",
        "[info] command=\"md add <合约...>\" desc=增量订阅合约",
        "[info] command=\"md del <合约...>\" desc=退订合约并清空该合约 tick 缓存",
        "[info] command=\"md list\" desc=显示当前订阅集合",
        "[info] command=\"md dump [n]\" desc=输出最近缓存 tick (每合约上限 100 条, "
        "每合约时间正序, n 限制每合约条数)",
        "[info] command=\"order <账户> <合约> <价格> <手数> [long|short]\" desc=下限价开仓单",
        "[info] command=\"cancel <账户> <订单ID>\" desc=发送撤单请求",
        "[info] command=\"account [账户]\" desc=查询账户登录状态 (缺省=全部账户)",
        "[info] command=quit desc=退出策略 (exit 0)",
    };
    for (const std::string_view line : kHelpLines) {
        emit_line(line);
    }
}

void DemoStrategy::cmd_info(std::span<const std::string_view> args) {
    if (emit_error_on_extra_args("info", args)) {
        return;
    }
    emit_line(std::format("[info] strategy={} md_source={} sdk={} home={} 示例策略身份",
                          dz_strategy_id(ctx_), md_source_name(), dz_version_string(),
                          dz_strategy_home(ctx_)));
}

void DemoStrategy::cmd_md(std::span<const std::string_view> args) {
    if (args.empty()) {
        emit_error("md", "missing_subcommand", "缺少子命令，用法: md add|del|list|dump");
        return;
    }
    const std::string_view sub = args.front();
    const std::span<const std::string_view> rest = args.subspan(1);
    if (sub == "add") {
        md_subscribe(rest, true);
        return;
    }
    if (sub == "del") {
        md_subscribe(rest, false);
        return;
    }
    if (sub == "list") {
        if (emit_error_on_extra_args("md list", rest)) {
            return;
        }
        emit_md_list();
        return;
    }
    if (sub == "dump") {
        if (rest.size() > 1) {
            emit_error("md dump", "unexpected_args", "用法: md dump [n]");
            return;
        }
        std::size_t limit = 0;
        if (!rest.empty() && !parse_positive_size(rest.front(), limit)) {
            emit_error("md dump", "invalid_count",
                       std::format("n={} 条数必须为正整数", rest.front()));
            return;
        }
        emit_md_dump(limit);
        return;
    }
    emit_error("md", "unknown_subcommand",
               std::format("sub={} 子命令只支持 add|del|list|dump", sub));
}

void DemoStrategy::md_subscribe(std::span<const std::string_view> views, bool add) {
    const std::string_view action = add ? "add" : "del";
    const std::string cmd = std::format("md {}", action);
    if (views.empty()) {
        emit_error(cmd, "missing_instrument", "缺少合约代码");
        return;
    }
    const std::vector<std::string> instruments = to_strings(views);
    const std::vector<const char*> pointers = to_c_strs(instruments);
    const auto count = static_cast<uint32_t>(pointers.size());
    const bool ok = add ? dz_subscribe(ctx_, pointers.data(), count, false)
                        : dz_unsubscribe(ctx_, pointers.data(), count);
    if (!ok) {
        emit_error(
            cmd, add ? "subscribe_failed" : "unsubscribe_failed",
            std::format("errcode={} message=\"{}\"", static_cast<int>(dz_errcode()), dz_errmsg()));
        return;
    }
    for (const auto& instrument : instruments) {
        if (add) {
            tick_cache_.try_emplace(instrument);
        } else {
            tick_cache_.erase(instrument);
        }
    }
    emit_line(std::format("[md] action={} count={} instruments={} {}", action, instruments.size(),
                          join_csv(instruments), add ? "订阅请求已发送" : "退订请求已发送"));
}

void DemoStrategy::cmd_order(std::span<const std::string_view> args) {
    if (args.size() != 4 && args.size() != 5) {
        emit_error("order", "invalid_arguments", std::format("usage=\"{}\"", kOrderUsage));
        return;
    }
    const std::string account(args[0]);
    const std::string instrument(args[1]);
    double price = 0.0;
    if (!parse_double(args[2], price)) {
        emit_error("order", "invalid_price", std::format("value={} 价格必须为数字", args[2]));
        return;
    }
    int64_t volume = 0;
    if (!parse_int64(args[3], volume) || volume <= 0 || volume > INT32_MAX) {
        emit_error("order", "invalid_volume", std::format("value={} 手数必须为正整数", args[3]));
        return;
    }
    DzDirection direction = DZ_DIRECTION_LONG;
    if (args.size() == 5) {
        if (args[4] == "long") {
            direction = DZ_DIRECTION_LONG;
        } else if (args[4] == "short") {
            direction = DZ_DIRECTION_SHORT;
        } else {
            emit_error("order", "invalid_direction",
                       std::format("value={} 方向只能为 long 或 short", args[4]));
            return;
        }
    }
    const DzOrderId order_id =
        dz_place_order(ctx_, account.c_str(), instrument.c_str(), direction, DZ_PRICE_LIMIT, price,
                       static_cast<DzVolume>(volume), DZ_POSITION_EFFECT_OPEN);
    if (order_id < 0) {
        emit_error(
            "order", "place_order_failed",
            std::format("errcode={} message=\"{}\"", static_cast<int>(dz_errcode()), dz_errmsg()));
        return;
    }
    emit_line(
        std::format("[order] action=order order_id={} account={} instrument={} "
                    "direction={} price_type=limit price={:.3f} volume={} 开仓限价单已发送",
                    order_id, account, instrument, direction_name(direction), price, volume));
}

void DemoStrategy::cmd_cancel(std::span<const std::string_view> args) {
    if (args.size() != 2) {
        emit_error("cancel", "invalid_arguments", "usage=\"cancel <账户> <订单ID>\"");
        return;
    }
    const std::string account(args[0]);
    int64_t order_id = 0;
    if (!parse_int64(args[1], order_id) || order_id < 0) {
        emit_error("cancel", "invalid_order_id",
                   std::format("value={} 订单ID 必须为非负整数", args[1]));
        return;
    }
    if (!dz_cancel_order(ctx_, account.c_str(), static_cast<DzOrderId>(order_id))) {
        emit_error(
            "cancel", "cancel_failed",
            std::format("errcode={} message=\"{}\"", static_cast<int>(dz_errcode()), dz_errmsg()));
        return;
    }
    emit_line(std::format("[order] action=cancel account={} order_id={} 撤单请求已发送", account,
                          order_id));
}

void DemoStrategy::cmd_account(std::span<const std::string_view> args) {
    if (args.size() > 1) {
        emit_error("account", "invalid_arguments", "usage=\"account [账户]\"");
        return;
    }
    const std::string account = args.empty() ? std::string{} : std::string(args.front());
    if (!dz_query_account_status(ctx_, account.empty() ? nullptr : account.c_str())) {
        emit_error(
            "account", "query_failed",
            std::format("errcode={} message=\"{}\"", static_cast<int>(dz_errcode()), dz_errmsg()));
        return;
    }
    emit_line(std::format("[account] action=query account={} 查询已发送",
                          account.empty() ? "all" : account));
}

void DemoStrategy::cmd_quit(std::span<const std::string_view> args) {
    if (emit_error_on_extra_args("quit", args)) {
        return;
    }
    emit_line("[info] action=quit 策略将退出");
    quit_ = true;  // 引擎每批事件泵后探测 should_exit, 进程以 exit 0 收尾
}

bool DemoStrategy::emit_error_on_extra_args(std::string_view cmd,
                                            std::span<const std::string_view> args) {
    if (args.empty()) {
        return false;
    }
    emit_error(cmd, "unexpected_args", "该命令不接受参数");
    return true;
}

void DemoStrategy::emit_error(std::string_view cmd,
                              std::string_view reason,
                              std::string_view detail) {
    // 含空格的命令名 (如 md add) 加双引号, 与 key=value 值含空格时的引用规则一致
    const std::string cmd_field = cmd.find(' ') == std::string_view::npos
                                      ? std::format("cmd={}", cmd)
                                      : std::format("cmd=\"{}\"", cmd);
    std::string line = std::format("[error] {} reason={}", cmd_field, reason);
    if (!detail.empty()) {
        line += ' ';
        line += detail;
    }
    emit_line(line);
}

void DemoStrategy::emit_line(std::string_view line) {
    if (ctx_ == nullptr) {
        return;
    }
    const std::string text(line);  // dz_output_ui 要求 NUL 结尾
    if (!dz_output_ui(ctx_, text.c_str())) {
        // 演示工具不中断命令处理; 诊断写 stderr (英文, 符合日志文本规则)。
        // 注意: 超页截断由 SDK 按契约处理且返回 true, 不会走到这里。
        std::cerr << std::format("ui output failed | errcode={} message=\"{}\"",
                                 static_cast<int>(dz_errcode()), dz_errmsg())
                  << '\n';
    }
}

void DemoStrategy::emit_md_list() {
    std::vector<std::string> instruments;
    instruments.reserve(tick_cache_.size());
    for (const auto& [instrument, cache] : tick_cache_) {
        instruments.push_back(instrument);
    }
    emit_line(std::format("[md] action=list count={} instruments={} 当前订阅集合",
                          instruments.size(), instruments.empty() ? "-" : join_csv(instruments)));
}

void DemoStrategy::emit_md_dump(std::size_t limit) {
    std::vector<std::string> instruments;
    instruments.reserve(tick_cache_.size());
    std::size_t total = 0;
    for (const auto& [instrument, cache] : tick_cache_) {
        instruments.push_back(instrument);
        total += (limit > 0 && cache.size() > limit) ? limit : cache.size();
    }
    // 每合约内按 (date,time,subseconds) 时间正序输出; limit>0 时取每合约最近 limit 条
    std::string text =
        std::format("[md] action=dump limit={} count={} instruments={} 每合约时间正序", limit,
                    total, instruments.empty() ? "-" : join_csv(instruments));
    for (const auto& entry : tick_cache_) {
        const std::deque<DzTick>& cache = entry.second;
        if (cache.empty()) {
            continue;
        }
        std::vector<const DzTick*> sorted;
        sorted.reserve(cache.size());
        for (const DzTick& tick : cache) {
            sorted.push_back(&tick);
        }
        // 排序键 = (交易日 date, 当日秒 time, 微秒): 缓存的"时间正序"定义。夜盘 tick 的
        // date 为次一交易日, 故跨夜盘时按交易日内时间排序 (21:00 早于次日 09:00 的自然
        // 日历序不在此键语义内) —— 演示输出以交易日视角为准。
        std::stable_sort(sorted.begin(), sorted.end(), [](const DzTick* lhs, const DzTick* rhs) {
            return tick_time_key(*lhs) < tick_time_key(*rhs);
        });
        const std::size_t begin = (limit > 0 && sorted.size() > limit) ? sorted.size() - limit : 0;
        for (std::size_t i = begin; i < sorted.size(); ++i) {
            text += '\n';
            text += format_tick_line(*sorted[i]);
        }
    }
    emit_line(text);  // md dump 单帧多行: 头部一行 + 每 tick 一行
}

}  // namespace dztrader::demo
