#include "td/td_account_session_pure.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <limits>

#include <dztrader/date_time/date.h>

namespace dztrader::ctp {

namespace {

/// 解析 CTP 回报自带的 8 位 "YYYYMMDD" (委托/成交回报的 TradingDay 字段).
/// 先做长度与字符校验再交给 Date: 回报字段是定长字符数组, 空位填充 / 分隔符 / 缓冲区
/// 没清零都会落到这里, 不能靠 Date 的异常兜底 (它只为「月日越界」抛).
///
/// **不复用 `parse_ctp_date`** (td_ctp_mapping.cpp): 那个头携带 CTP 结构体头
/// (`ThostFtdcUserApiStruct.h`), 纯逻辑模块与纯函数测试目标都不链它; 且它用 `strlen` 读
/// 定长 `char[9]`, 字段没带 NUL 时会越界 —— 这里按 8 位上限读、再校验第 9 字节是终止符。
/// 两处判据必须同步 (长度 / 纯数字 / 日历合法性), 改一处就要看另一处。
/// @return true = 解析成功 (写入 *out); false = 长度 / 字符 / 日历非法
bool parse_report_trading_day(const char* yyyymmdd, int32_t* out) noexcept {
    if (yyyymmdd == nullptr || out == nullptr) {
        return false;
    }
    constexpr size_t kDateLen = 8;
    for (size_t i = 0; i < kDateLen; ++i) {
        if (yyyymmdd[i] < '0' || yyyymmdd[i] > '9') {
            return false;
        }
    }
    if (yyyymmdd[kDateLen] != '\0') {
        return false;
    }
    try {
        *out = dztrader::Date::from_string(yyyymmdd, "%Y%m%d").days_since_epoch();
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

}  // namespace

std::string OrderRefMap::normalize_order_ref(const std::string& order_ref) {
    // 空字符串或非数字字符原样返回 (外部订单可能传入非数字 OrderRef)
    if (order_ref.empty()) {
        return order_ref;
    }
    for (char c : order_ref) {
        if (c < '0' || c > '9') {
            return order_ref;
        }
    }
    // 解析为 long long 后用 %012lld 归一化 (与 td_ctp_mapping.cpp:174 一致)
    try {
        long long val = std::stoll(order_ref);
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%012lld", val);
        return std::string(buf);
    } catch (...) {
        // 溢出 stoll 范围, 原样返回
        return order_ref;
    }
}

const DzOrderId* OrderRefMap::find_by_order_ref(const std::string& order_ref) const {
    auto it = ref_to_id_.find(normalize_order_ref(order_ref));
    return (it != ref_to_id_.end()) ? &it->second : nullptr;
}

const DzOrderId* OrderRefMap::find_by_sys_id(const std::string& sys_id) const {
    auto it = sys_to_id_.find(sys_id);
    return (it != sys_to_id_.end()) ? &it->second : nullptr;
}

void OrderRefMap::insert_by_order_ref(const std::string& order_ref, DzOrderId order_id) {
    ref_to_id_[normalize_order_ref(order_ref)] = order_id;
}

void OrderRefMap::insert_by_sys_id(const std::string& sys_id, DzOrderId order_id) {
    sys_to_id_[sys_id] = order_id;
}

void OrderRefMap::erase_by_order_ref(const std::string& order_ref) {
    ref_to_id_.erase(normalize_order_ref(order_ref));
}

const CancelContext* OrderRefMap::find_cancel_context(DzOrderId order_id) const {
    auto it = id_to_ctx_.find(order_id);
    return (it != id_to_ctx_.end()) ? &it->second : nullptr;
}

void OrderRefMap::insert_cancel_context(DzOrderId order_id, const CancelContext& ctx) {
    id_to_ctx_[order_id] = ctx;
}

void OrderRefMap::update_cancel_context(DzOrderId order_id, int32_t front_id, int32_t session_id) {
    auto it = id_to_ctx_.find(order_id);
    if (it == id_to_ctx_.end()) {
        return;  // 未找到, 忽略 (外部订单无 ctx)
    }
    // 仅在非 0 时更新, 避免覆盖已填的有效值 (CTP 早期回报 front_id/session_id 可能为 0)
    if (front_id != 0) {
        it->second.front_id = front_id;
    }
    if (session_id != 0) {
        it->second.session_id = session_id;
    }
}

void OrderRefMap::insert_strategy(DzOrderId order_id, const std::string& strategy_id) {
    id_to_strategy_[order_id] = strategy_id;
}

const std::string* OrderRefMap::find_strategy(DzOrderId order_id) const {
    auto it = id_to_strategy_.find(order_id);
    return (it != id_to_strategy_.end()) ? &it->second : nullptr;
}

void OrderRefMap::erase_strategy(DzOrderId order_id) { id_to_strategy_.erase(order_id); }

void OrderRefMap::clear() noexcept {
    ref_to_id_.clear();
    sys_to_id_.clear();
    id_to_ctx_.clear();
    id_to_strategy_.clear();
}

QueryErrorTarget route_query_error(int request_id,
                                   int instrument_request_id,
                                   int position_request_id,
                                   int account_request_id,
                                   int runtime_account_request_id) noexcept {
    if (request_id <= 0) {
        return QueryErrorTarget::None;
    }
    if (request_id == position_request_id) {
        return QueryErrorTarget::Position;
    }
    if (request_id == instrument_request_id) {
        return QueryErrorTarget::Instrument;
    }
    if (request_id == account_request_id || request_id == runtime_account_request_id) {
        return QueryErrorTarget::TradingAccount;
    }
    return QueryErrorTarget::None;
}

int64_t sync_order_ref(int64_t current_ref, int64_t ctp_max) noexcept {
    // 设计 §9.4: new = max(current+1, ctp_max+1)
    int64_t from_current = current_ref + 1;
    int64_t from_ctp = ctp_max + 1;
    return (from_current >= from_ctp) ? from_current : from_ctp;
}

int64_t parse_max_order_ref(const char* max_order_ref) noexcept {
    if (max_order_ref == nullptr || max_order_ref[0] == '\0') {
        return 0;
    }
    for (size_t i = 0; max_order_ref[i] != '\0'; ++i) {
        if (max_order_ref[i] < '0' || max_order_ref[i] > '9') {
            return 0;
        }
    }
    try {
        return std::stoll(max_order_ref);
    } catch (...) {
        return 0;
    }
}

bool alert_report_due(int64_t now_s, int64_t last_report_s, int64_t interval_s) noexcept {
    // 从未上报过 (含初始 0 与负值) -> 立即上报; 否则看是否已过最小间隔.
    if (last_report_s <= 0) {
        return true;
    }
    return now_s - last_report_s >= interval_s;
}

bool replay_external_only_alarm(bool in_replay,
                                int64_t record_epoch_s,
                                int64_t last_live_epoch_s) noexcept {
    // 实时流记录不是本告警的取证对象; 柜台时间未知 / 尚未观测到实时回报时无法判定.
    if (!in_replay || record_epoch_s <= 0 || last_live_epoch_s <= 0) {
        return false;
    }
    // 记录成立于最后一条实时回报之前/同时 → 那一刻本会话在收实时流, 按投资者级假设
    // 它本应已实时到达并落库 (于是重放会命中基准被吞); 仍以「新记录」出现在重放里 = 告警.
    return record_epoch_s <= last_live_epoch_s;
}

bool trading_day_usable(int32_t days_since_epoch) noexcept {
    // INT32_MIN 是 OnRspTdUserLoginField::days_since_epoch 的「解析失败」哨兵
    // (td_events.h), 由 TdSpi::OnRspUserLogin 在 parse_ctp_date 返回 DZ_DATE_NA 时保留.
    // 0 是 DZ_DATE_NA 且业务上不存在 1970 年的真实交易日, 同样视为不可用.
    return days_since_epoch != std::numeric_limits<int32_t>::min() && days_since_epoch > 0;
}

ReportTradingDayVerdict judge_report_trading_day(int32_t session_day,
                                                 const char* report_day) noexcept {
    // 会话交易日不可用时先判死: 它是本会话全部业务数据的时间锚, 未建立就收到回报本身即异常,
    // 报文是否可解析不改变结论 (更不能把 0 当「1970 年」去比较).
    if (!trading_day_usable(session_day)) {
        return ReportTradingDayVerdict::SessionUnknown;
    }
    if (report_day == nullptr || report_day[0] == '\0') {
        return ReportTradingDayVerdict::Empty;
    }
    int32_t parsed = 0;
    if (!parse_report_trading_day(report_day, &parsed)) {
        return ReportTradingDayVerdict::Unparsable;
    }
    return parsed == session_day ? ReportTradingDayVerdict::Match
                                 : ReportTradingDayVerdict::Mismatch;
}

const char* report_trading_day_reason(ReportTradingDayVerdict verdict) noexcept {
    switch (verdict) {
        case ReportTradingDayVerdict::Match:
            return "match";
        case ReportTradingDayVerdict::SessionUnknown:
            return "session_unknown";
        case ReportTradingDayVerdict::Empty:
            return "empty";
        case ReportTradingDayVerdict::Unparsable:
            return "unparsable";
        case ReportTradingDayVerdict::Mismatch:
            return "mismatch";
    }
    return "unknown";
}

std::string cross_day_logout_message(ReportTradingDayVerdict verdict,
                                     int32_t session_day,
                                     const char* report_day) {
    const std::string raw = report_day != nullptr ? report_day : "";
    // 会话交易日可读化: <=0 是 DZ_DATE_NA / 哨兵 (SessionUnknown 分支), 不引用具体日期,
    // 免得把 0 渲染成 1970-01-01 这种假证据.
    const std::string session_text = trading_day_usable(session_day)
                                         ? dztrader::Date{session_day}.to_string("%Y%m%d")
                                         : std::string("未知");
    const std::string suffix = "，已主动登出，账户将保持离线到下次自动登录";
    switch (verdict) {
        case ReportTradingDayVerdict::Mismatch:
            return std::format("回报交易日与会话交易日不一致（会话 {} / 回报 {}）{}", session_text,
                               raw, suffix);
        case ReportTradingDayVerdict::Empty:
            return std::format("回报未携带交易日，无法确认属于当前会话交易日{}", suffix);
        case ReportTradingDayVerdict::Unparsable:
            return std::format("回报交易日无法解析（原始值 \"{}\"）{}", raw, suffix);
        case ReportTradingDayVerdict::SessionUnknown:
            return std::format("会话交易日未建立时收到回报{}", suffix);
        case ReportTradingDayVerdict::Match:
            break;  // 调用方只对非 Match 调用; 防御性返回通用文案
    }
    return std::format("回报交易日校验未通过{}", suffix);
}

std::string login_failed_message(const std::string& reason) {
    // 恢复路径 = 先登出、再登录 (ADR 0051 决策 4; #409 落地后 WebUI 已有该入口:
    // REST /api/td/accounts/{id}/logout|login 与 WS td_disconnect/td_connect).
    // 文案陈述「发生了什么 + 会怎样 + 下一步」; 不再让用户去界面上做不到的事.
    const std::string consequence = "本进程不会自动重试该账户；请先登出再重新登录";
    if (reason.empty()) {
        // 空原因不产出「（）」这种残缺文案.
        return "交易登录失败，" + consequence;
    }
    return std::format("交易登录失败（{}），{}", reason, consequence);
}

std::string settlement_confirm_failed_message(const std::string& reason) {
    // 口径同 login_failed_message: 「发生了什么 + 会怎样 + 下一步」.
    // 「会怎样」必须写清, 因为状态机只回滚到 LoggedIn (6/10「已登录」) 这个正常中间态,
    // 界面看不出异常; 「下一步」= 先登出再登录 (ADR 0051 决策 4, WebUI 已有该入口, #409).
    const std::string consequence =
        "账户停在「已登录」不能交易，本进程不会自动重试；请先登出再重新登录";
    if (reason.empty()) {
        return "结算单确认失败，" + consequence;
    }
    return std::format("结算单确认失败（{}），{}", reason, consequence);
}

std::string instruments_load_failed_message(const std::string& reason) {
    const std::string consequence =
        "账户停在「已登录」不能交易，本进程不会自动重试；请先登出再重新登录";
    if (reason.empty()) {
        return "合约加载失败，" + consequence;
    }
    return std::format("合约加载失败（{}），{}", reason, consequence);
}

bool position_query_retry_allowed(int retries_done) noexcept {
    return retries_done >= 0 && retries_done < kPositionQueryMaxRetries;
}

std::string position_query_failed_message() {
    // 与 login_failed_message 同一口径: 只陈述事实, 不承诺 UI 上不存在的操作 (#409).
    return std::format("持仓查询失败，账户未就绪（已重试 {} 次）", kPositionQueryMaxRetries);
}

}  // namespace dztrader::ctp
