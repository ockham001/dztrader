#include "dztrader/platform/subscription_manager.h"

#include <format>

#include <spdlog/spdlog.h>

#include <dztrader/shm/frame_codec.h>  // decode_ext_inst_json
#include <dztrader/platform/frame_codec.h>  // write_ext_inst_json_obj

namespace dztrader::platform {

SubscriptionManager::SubscriptionManager(const std::string& name,
                                         shm::MultiWriter& event_writer,
                                         NotifyUi& notify_ui)
    : name_(name), event_writer_(event_writer), notify_ui_(notify_ui) {}

// ---- Manual entry ----

std::vector<std::string> SubscriptionManager::subscribe(
    const std::string& instance_id, std::vector<std::string> instruments) {
    std::vector<std::string> to_subscribe;
    to_subscribe.reserve(instruments.size());
    std::set<std::string> seen;
    for (const auto& inst : instruments) {
        if (!seen.insert(inst).second) {
            continue;
        }
        auto [it, is_new] = instruments_sub_info_.try_emplace(inst);
        it->second.subscribers.insert(instance_id);
        if (is_new || it->second.sub_state != SubState::Subscribed) {
            to_subscribe.push_back(inst);
        }
    }
    if (!to_subscribe.empty()) {
        SPDLOG_INFO("sub registered | instance_id={} count={}", instance_id, to_subscribe.size());
    }
    return to_subscribe;
}

std::vector<std::string> SubscriptionManager::unsubscribe(
    const std::string& instance_id, std::vector<std::string> instruments) {
    std::vector<std::string> to_unsub;
    for (const auto& inst : instruments) {
        auto it = instruments_sub_info_.find(inst);
        if (it == instruments_sub_info_.end()) {
            continue;
        }
        it->second.subscribers.erase(instance_id);
        bool need_unsub =
            it->second.subscribers.empty() && it->second.sub_state == SubState::Subscribed;
        if (need_unsub) {
            it->second.sub_state = SubState::NotRequested;
            SPDLOG_INFO("unsub needed | instrument={} instance_id={}", inst, instance_id);
            to_unsub.push_back(inst);
        } else if (it->second.subscribers.empty()) {
            if (it->second.sub_state == SubState::NotRequested) {
                instruments_sub_info_.erase(it);
            }
        }
    }
    return to_unsub;
}

std::vector<std::string> SubscriptionManager::unsubscribe_all(const std::string& instance_id) {
    std::vector<std::string> to_unsub;
    std::vector<std::string> to_erase;
    for (auto& [inst, info] : instruments_sub_info_) {
        info.subscribers.erase(instance_id);
        if (info.subscribers.empty()) {
            if (info.sub_state == SubState::Subscribed) {
                info.sub_state = SubState::NotRequested;
                to_unsub.push_back(inst);
            } else if (info.sub_state == SubState::NotRequested) {
                to_erase.push_back(inst);
            }
        }
    }
    for (const auto& inst : to_erase) {
        instruments_sub_info_.erase(inst);
    }
    if (!to_unsub.empty()) {
        SPDLOG_INFO("unsub all | instance_id={} count={}", instance_id, to_unsub.size());
    }
    return to_unsub;
}

// ---- State machine operations ----

void SubscriptionManager::mark_pending(const std::vector<std::string>& instruments) {
    for (const auto& inst : instruments) {
        auto it = instruments_sub_info_.find(inst);
        if (it != instruments_sub_info_.end()) {
            it->second.sub_state = SubState::Pending;
        }
    }
}

bool SubscriptionManager::on_sub_confirmed(const std::string& instrument, bool success) {
    auto it = instruments_sub_info_.find(instrument);
    if (it == instruments_sub_info_.end()) {
        return false;
    }
    if (!success) {
        // 确认失败且无订阅者: CTP 侧确定无订阅, 直接 erase。
        // 避免残留条目虚增 expected 计数, 及重试链路为无订阅者合约重发订阅。
        if (it->second.subscribers.empty()) {
            instruments_sub_info_.erase(it);
        }
        return false;
    }
    if (it->second.subscribers.empty()) {
        // 确认后乐观退订: 订阅在途期间最后订阅者已退出。
        // 状态先置 NotRequested (与 unsubscribe 对 Subscribed 的处理同构, 状态先行),
        // 保证退订在途窗口内新订阅者会因状态非 Subscribed 而重新触发订阅
        // (依赖 CTP 同连接 FIFO: unsub 先处理 sub 后处理, 最终一致)。
        // 闭环: 调用方发退订后由 on_unsub_confirmed erase 条目。
        it->second.sub_state = SubState::NotRequested;
        return true;
    }
    it->second.sub_state = SubState::Subscribed;
    return false;
}

void SubscriptionManager::on_unsub_confirmed(const std::string& instrument) {
    auto it = instruments_sub_info_.find(instrument);
    if (it == instruments_sub_info_.end()) {
        return;
    }
    if (it->second.subscribers.empty()) {
        instruments_sub_info_.erase(it);
    }
}

std::vector<std::string> SubscriptionManager::check_subscribe_status() {
    std::vector<std::string> pending;
    size_t subscribed_count = 0;
    for (const auto& [inst, info] : instruments_sub_info_) {
        if (info.sub_state == SubState::Pending) {
            pending.push_back(inst);
        } else if (info.sub_state == SubState::Subscribed) {
            ++subscribed_count;
        }
    }
    cached_subscribed_count_ = subscribed_count;
    return pending;
}

void SubscriptionManager::reset_pending_state() {
    size_t reset_count = 0;
    for (auto& [inst, info] : instruments_sub_info_) {
        if (info.sub_state == SubState::Pending) {
            info.sub_state = SubState::NotRequested;
            ++reset_count;
        }
    }
    if (reset_count > 0) {
        SPDLOG_INFO("pending state reset | count={}", reset_count);
    }
}

// ---- Statistics & query ----

size_t SubscriptionManager::expected_subscribe_count() const {
    return instruments_sub_info_.size();
}

size_t SubscriptionManager::subscribed_count() const {
    return cached_subscribed_count_;
}

bool SubscriptionManager::is_instrument_subscribed(const std::string& instrument) const {
    auto it = instruments_sub_info_.find(instrument);
    return it != instruments_sub_info_.end() && it->second.sub_state == SubState::Subscribed;
}

const InstrumentSubInfo* SubscriptionManager::find_instrument(const std::string& instrument) const {
    auto it = instruments_sub_info_.find(instrument);
    return it == instruments_sub_info_.end() ? nullptr : &it->second;
}

const std::map<std::string, InstrumentSubInfo>& SubscriptionManager::all_instruments() const {
    return instruments_sub_info_;
}

// ---- Lifecycle hooks ----

LoginSubPlan SubscriptionManager::on_logged_in() {
    LoginSubPlan plan;
    // 1) cleanup orphan: erase NotRequested, collect Subscribed for unsub
    plan.to_unsubscribe = cleanup_orphan_instruments();
    // 2) reset all sub_state to NotRequested, clear count
    reset_all_sub_state();
    // 3) collect instruments with subscribers for resubscribe
    plan.to_resubscribe = get_instruments_to_resubscribe();
    return plan;
}

void SubscriptionManager::on_idle() {
    for (auto& [inst, info] : instruments_sub_info_) {
        info.sub_state = SubState::NotRequested;
    }
    std::erase_if(instruments_sub_info_,
                  [](const auto& kv) { return kv.second.subscribers.empty(); });
    cached_subscribed_count_ = 0;
}

void SubscriptionManager::on_session_lost() {
    for (auto& [inst, info] : instruments_sub_info_) {
        info.sub_state = SubState::NotRequested;
    }
    cached_subscribed_count_ = 0;
}

// ---- Private helpers ----

std::vector<std::string> SubscriptionManager::cleanup_orphan_instruments() {
    std::vector<std::string> to_unsub;
    std::vector<std::string> to_erase;
    for (auto& [inst, info] : instruments_sub_info_) {
        if (info.subscribers.empty()) {
            if (info.sub_state == SubState::NotRequested) {
                to_erase.push_back(inst);
            } else if (info.sub_state == SubState::Subscribed) {
                to_unsub.push_back(inst);
            }
        }
    }
    for (const auto& inst : to_erase) {
        instruments_sub_info_.erase(inst);
    }
    if (!to_erase.empty() || !to_unsub.empty()) {
        SPDLOG_DEBUG("cleanup orphan | erased={} to_unsub={}", to_erase.size(), to_unsub.size());
    }
    return to_unsub;
}

std::vector<std::string> SubscriptionManager::get_instruments_to_resubscribe() const {
    std::vector<std::string> instruments;
    instruments.reserve(instruments_sub_info_.size());
    for (const auto& [symbol, info] : instruments_sub_info_) {
        if (!info.subscribers.empty() && info.sub_state != SubState::Subscribed) {
            instruments.push_back(symbol);
        }
    }
    return instruments;
}

void SubscriptionManager::reset_all_sub_state() {
    for (auto& [inst, info] : instruments_sub_info_) {
        info.sub_state = SubState::NotRequested;
    }
    cached_subscribed_count_ = 0;
    SPDLOG_INFO("sub state reset | count={}", instruments_sub_info_.size());
}

// ---- Frame handling ----

SubscribeActionResult SubscriptionManager::handle_subscribe_req(const shm::FrameView& view) {
    SubscribeReq req;
    try {
        req = shm::decode_ext_inst_json<SubscribeReq>(view);
    } catch (const nlohmann::json::exception& e) {
        SPDLOG_ERROR("subscribe decode failed | error=\"{}\"", e.what());
        notify_ui_.warn(std::format("订阅请求格式错误: {}", e.what()));
        return {};
    }
    SubscribeActionResult result;
    result.instance_id = req.instance_id;
    switch (req.action) {
        case SubscribeAction::Subscribe:
            if (req.replace) {
                result.to_unsubscribe = unsubscribe_all(req.instance_id);
            }
            result.to_subscribe = subscribe(req.instance_id, std::move(req.instruments));
            break;
        case SubscribeAction::Unsubscribe:
            result.to_unsubscribe = unsubscribe(req.instance_id, std::move(req.instruments));
            break;
        case SubscribeAction::UnsubscribeAll:
            result.to_unsubscribe = unsubscribe_all(req.instance_id);
            break;
    }
    return result;
}

void SubscriptionManager::handle_query_md_subscriptions(const shm::FrameView& view) {
    nlohmann::json req_json;
    try {
        req_json = shm::decode_ext_inst_json<nlohmann::json>(view);
    } catch (const nlohmann::json::exception& e) {
        SPDLOG_WARN("query md subscriptions decode rejected | reason=\"bad_json\" error=\"{}\"",
                    e.what());
        send_query_error_rsp("bad_json");
        return;
    }
    bool has_query = req_json.contains("query") && req_json["query"].is_string();
    bool has_instruments = req_json.contains("instruments") && req_json["instruments"].is_array();
    if (!has_query && !has_instruments) {
        SPDLOG_WARN("query md subscriptions rejected | reason=\"missing_query_or_instruments\"");
        send_query_error_rsp("missing_query_or_instruments");
        return;
    }
    if (has_query && has_instruments) {
        SPDLOG_WARN("query md subscriptions rejected | reason=\"ambiguous_query\"");
        send_query_error_rsp("ambiguous_query");
        return;
    }
    query_subscriptions(req_json);
}

void SubscriptionManager::query_subscriptions(const nlohmann::json& req) {
    auto build_detail = [](const std::string& inst, const InstrumentSubInfo* info) {
        SubscriptionDetail d;
        d.instrument = inst;
        d.sub_state = info ? info->sub_state : SubState::NotRequested;
        d.subscribers =
            info ? std::vector<std::string>(info->subscribers.begin(), info->subscribers.end())
                 : std::vector<std::string>{};
        return d;
    };

    RtnMdSubscriptionsRsp rsp;
    int& returned = rsp.returned_count;
    int& total_matched = rsp.total_matched;
    bool& truncated = rsp.truncated;

    if (req.contains("instruments")) {
        for (const auto& inst : req["instruments"]) {
            if (!inst.is_string()) {
                continue;
            }
            total_matched++;
            if (returned < SUBSCRIPTION_QUERY_MAX) {
                const auto& inst_str = inst.get<std::string>();
                const auto* info = find_instrument(inst_str);
                rsp.subscriptions.push_back(build_detail(inst_str, info));
                returned++;
            } else {
                truncated = true;
            }
        }
    } else if (req.contains("query")) {
        const auto& query = req["query"].get<std::string>();
        if (query == "unsuccessful") {
            const auto& all = all_instruments();
            for (const auto& [inst, info] : all) {
                if (info.sub_state != SubState::Pending) {
                    continue;
                }
                total_matched++;
                if (returned < SUBSCRIPTION_QUERY_MAX) {
                    rsp.subscriptions.push_back(build_detail(inst, &info));
                    returned++;
                } else {
                    truncated = true;
                }
            }
            for (const auto& [inst, info] : all) {
                if (info.sub_state != SubState::NotRequested) {
                    continue;
                }
                total_matched++;
                if (returned < SUBSCRIPTION_QUERY_MAX) {
                    rsp.subscriptions.push_back(build_detail(inst, &info));
                    returned++;
                } else {
                    truncated = true;
                }
            }
        } else {
            SPDLOG_WARN("query md subscriptions rejected | reason=\"unknown_query\" value=\"{}\"",
                        query);
            send_query_error_rsp("unknown_query");
            return;
        }
    }

    try {
        write_ext_inst_json_obj(event_writer_, DZ_FRAME_RTN_MD_SUBSCRIPTIONS, name_,
                                nlohmann::json(rsp));
    } catch (const std::exception& e) {
        SPDLOG_ERROR("rtn_md_subscriptions failed | error=\"{}\"", e.what());
    }
}

void SubscriptionManager::send_query_error_rsp(const std::string& error) {
    RtnMdSubscriptionsRsp rsp;
    rsp.error = error;
    try {
        write_ext_inst_json_obj(event_writer_, DZ_FRAME_RTN_MD_SUBSCRIPTIONS, name_,
                                nlohmann::json(rsp));
    } catch (const std::exception& e) {
        SPDLOG_ERROR("send_query_error_rsp failed | error=\"{}\"", e.what());
    }
}

}  // namespace dztrader::platform