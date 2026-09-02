#include "td/td_account_session_pure.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_set>
#include <vector>

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

void OrderRefMap::erase_strategy(DzOrderId order_id) {
    id_to_strategy_.erase(order_id);
}

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

// ============================================================================
// PositionMirror: 持仓绝对态镜像 (2002 写端 diff)
// ============================================================================

bool PositionMirror::same_position(const DzPositionInfo& a, const DzPositionInfo& b) noexcept {
    // 业务字段集对比 (seq/date 不参与: 镜像内日期可能落后, seq 由调用方分配)
    return a.volume == b.volume && a.frozen_volume == b.frozen_volume &&
           a.price == b.price && a.yd_volume == b.yd_volume &&
           a.today_volume == b.today_volume;
}

bool PositionMirror::update_if_changed(const DzPositionInfo& pos) {
    Key key{pos.account_id, pos.instrument_id, pos.direction};
    auto it = positions_.find(key);
    if (it == positions_.end()) {
        positions_.emplace(std::move(key), pos);
        return true;  // 首次遇到该 key = 变化
    }
    if (same_position(it->second, pos)) {
        return false;  // 与镜像相同, 不转发
    }
    it->second = pos;
    return true;
}

uint64_t PositionMirror::seq_of(const std::string& account_id,
                                const std::string& instrument_id,
                                int8_t direction) const noexcept {
    auto it = positions_.find(Key{account_id, instrument_id, direction});
    return it == positions_.end() ? 0 : it->second.seq;
}

void PositionMirror::update_seq(const std::string& account_id,
                                const std::string& instrument_id,
                                int8_t direction,
                                uint64_t seq) noexcept {
    auto it = positions_.find(Key{account_id, instrument_id, direction});
    if (it != positions_.end()) {
        it->second.seq = seq;
    }
}

std::vector<DzPositionInfo> PositionMirror::keys_not_in_group(
    const std::vector<DzPositionInfo>& group) const {
    std::unordered_set<Key, KeyHash> in_group;
    for (const auto& p : group) {
        in_group.emplace(p.account_id, p.instrument_id, p.direction);
    }
    std::vector<DzPositionInfo> missing;
    for (const auto& [key, stored] : positions_) {
        if (in_group.find(key) == in_group.end()) {
            // 返回镜像中该 key 的最后一次行: 调用方以它为基础置 volume=0
            // (同 account/instrument/direction 即 key 覆盖语义, 绝对态清零)。
            missing.push_back(stored);
        }
    }
    return missing;
}

}  // namespace dztrader::ctp
