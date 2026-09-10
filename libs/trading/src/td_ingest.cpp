#include <dztrader/trading/td_ingest.h>

#include <cstring>

namespace dztrader {

void TdIngestGate::set_watermark(const std::string& account_id, uint64_t w) {
    AccountState& st = accounts_[account_id];
    st.watermark = w;
    st.has_watermark = true;
}

std::string TdIngestGate::trade_segment_prefix(const std::string& account_id) {
    std::string prefix = account_id;
    prefix.push_back('\x1f');
    return prefix;
}

std::string TdIngestGate::trade_segment_key(const std::string& account_id,
                                            const char* trading_day) {
    std::string key = trade_segment_prefix(account_id);
    key.append(trading_day);
    return key;
}

void TdIngestGate::erase_trade_segments(const std::string& account_id) {
    // 段键 = account_id + '\x1f' + day; 必须带分隔符匹配 — 裸前缀
    // (rfind(account_id, 0)) 会让 "ctp1" 误中 "ctp12" 的段。
    const std::string prefix = trade_segment_prefix(account_id);
    for (auto it = trade_segments_.begin(); it != trade_segments_.end();) {
        if (it->first.starts_with(prefix)) {
            it = trade_segments_.erase(it);
        } else {
            ++it;
        }
    }
}

TdIngestGate::Verdict TdIngestGate::admit(const std::string& account_id, uint64_t seq) {
    AccountState& st = accounts_[account_id];

    // 首帧断档: 未见该账户任何已应用帧, 且 seq 与水位间有空洞。
    // 仅首帧检测 (spec §5.2: 运行中断档不可能, 断档检测只在启动首帧做一次)。
    // 无水位 (账户无 DB 快照) 无断档可言。
    if (!st.has_last_applied && st.has_watermark && seq > st.watermark + 1) {
        Gap gap;
        gap.account_id = account_id;
        gap.from = st.watermark + 1;
        gap.to = seq - 1;
        pending_gap_ = std::move(gap);
    }

    // 铁律 (spec §2.3-4): 跳过判定只用 DB 水位 W 与自身 last_applied, 永不用内存见过的最大 seq。
    if (st.has_last_applied && seq <= st.last_applied) {
        return Verdict::kSkip;  // 已应用防重
    }
    if (st.has_watermark && seq <= st.watermark) {
        return Verdict::kSkip;  // 快照已含
    }

    // 应用: 推进本账户已应用水位
    st.last_applied = seq;
    st.has_last_applied = true;
    return Verdict::kApply;
}

std::optional<TdIngestGate::Gap> TdIngestGate::take_pending_gap() {
    std::optional<Gap> result = std::move(pending_gap_);
    pending_gap_.reset();
    return result;
}

bool TdIngestGate::detect_reset(const std::string& account_id, uint64_t seq) {
    const auto it = accounts_.find(account_id);
    if (it == accounts_.end() || !it->second.has_last_applied) {
        return false;  // 无已应用基准 (首帧或未见过) 无倒退可言
    }
    return seq < it->second.last_applied;
}

void TdIngestGate::reset_account(const std::string& account_id, uint64_t new_w) {
    AccountState& st = accounts_[account_id];
    st.watermark = new_w;
    st.has_watermark = true;  // 重置 = 新基准, 新水位从 DB 重查
    st.last_applied = 0;
    st.has_last_applied = false;

    // 清该账户全部成交去重段 (重置 = 新基准)
    erase_trade_segments(account_id);
}

bool TdIngestGate::admit_trade(const std::string& account_id, const char* trading_day,
                               const char* trade_id) {
    if (trading_day == nullptr || trade_id == nullptr) {
        return false;
    }
    // 交易日切换自动清理: 该账户首见新 day 时丢弃全部旧日段 (集合增长有界, spec §5.4)。
    const std::string day(trading_day);
    auto day_it = account_days_.find(account_id);
    if (day_it != account_days_.end() && day_it->second != day) {
        erase_trade_segments(account_id);
    }
    account_days_[account_id] = day;

    auto& segment = trade_segments_[trade_segment_key(account_id, trading_day)];
    return segment.insert(trade_id).second;
}

void TdIngestGate::on_trading_day_changed(const std::string& account_id,
                                          const char* new_trading_day) {
    // 幂等: 仅在新交易日与已见日不同时清段; 同日重复调用为 no-op。
    // 调用方 (SDK 在 2018 ACCOUNT_STATUS 推送时触发) 可能在同一天收到多次状态帧,
    // 重复清段会误删当前日去重段、使二道防线失效, 故以 account_days_ 判变化。
    if (new_trading_day != nullptr) {
        const std::string day(new_trading_day);
        auto day_it = account_days_.find(account_id);
        if (day_it != account_days_.end() && day_it->second == day) {
            return;  // 同日: no-op
        }
        account_days_[account_id] = day;
    }
    // 新交易日: 丢弃该账户全部旧日段 (带分隔符前缀匹配, 当前日段同删, 后续 admit_trade 重建)。
    erase_trade_segments(account_id);
}

}  // namespace dztrader
