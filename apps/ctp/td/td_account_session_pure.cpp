#include "td/td_account_session_pure.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

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

int64_t resync_retry_delay_s(int attempt) noexcept {
    // 60s 起, 逐次翻倍 (60/120/240), 封顶 300s (issue #367 增量复核 B1).
    // 封顶与未知枚举上报限频同量级: 柜台持续失败时 5 分钟内最多试一次, 不刷日志不刷流控.
    constexpr int64_t kBase = 60;
    constexpr int64_t kCap = 300;
    if (attempt <= 1) {
        return kBase;
    }
    int64_t delay = kBase;
    for (int64_t i = 1; i < attempt && delay < kCap; ++i) {
        delay *= 2;
    }
    return delay < kCap ? delay : kCap;
}

ResyncRetryAction decide_resync_retry(bool is_ready,
                                      bool data_query_ok,
                                      bool query_chain_busy) noexcept {
    // 顺序即优先级: 先看还需不需要补 (未就绪 / 已完整都收链), 再看能不能补 (在途就等).
    if (!is_ready || data_query_ok) {
        return ResyncRetryAction::kStop;
    }
    if (query_chain_busy) {
        return ResyncRetryAction::kWait;
    }
    return ResyncRetryAction::kResync;
}

}  // namespace dztrader::ctp
