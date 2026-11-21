#include "td/td_account_session_pure.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <limits>

namespace dztrader::ctp {

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

bool unknown_enum_report_due(int64_t now_s, int64_t last_report_s, int64_t interval_s) noexcept {
    // 从未上报过 (含初始 0 与负值) -> 立即上报; 否则看是否已过最小间隔.
    if (last_report_s <= 0) {
        return true;
    }
    return now_s - last_report_s >= interval_s;
}

bool trading_day_usable(int32_t days_since_epoch) noexcept {
    // INT32_MIN 是 OnRspTdUserLoginField::days_since_epoch 的「解析失败」哨兵
    // (td_events.h), 由 TdSpi::OnRspUserLogin 在 parse_ctp_date 返回 DZ_DATE_NA 时保留.
    // 0 是 DZ_DATE_NA 且业务上不存在 1970 年的真实交易日, 同样视为不可用.
    return days_since_epoch != std::numeric_limits<int32_t>::min() && days_since_epoch > 0;
}

std::string login_failed_message(const std::string& reason) {
    // 不承诺「请登出后重新登录」: WebUI 目前没有 td 的连接/断开入口
    // (DZ_FRAME_TD_CONNECT/TD_DISCONNECT 无生产端, 无 REST/WS 控制消息, 见 #409),
    // 让用户去做一件界面上做不到的事, 比只说事实更糟. 这里只陈述「发生了什么 + 会怎样」,
    // 具体恢复路径由运维侧处置 (等自动调度窗口 / 重启网关), 见 docs/components/dztd_ctp.md.
    if (reason.empty()) {
        // 空原因不产出「（）」这种残缺文案.
        return "交易登录失败，本进程不会自动重试该账户";
    }
    return std::format("交易登录失败（{}），本进程不会自动重试该账户", reason);
}

}  // namespace dztrader::ctp
