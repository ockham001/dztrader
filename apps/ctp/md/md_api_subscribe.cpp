#include "md/md_api.h"
#include "md/md_batch_check.h"

#include <algorithm>
#include <chrono>

#include <spdlog/spdlog.h>
#include <magic_enum/magic_enum.hpp>

#include <dztrader/core/encoding.h>
#include <dztrader/data_type.h>
#include <dztrader/struct.h>
#include <dztrader/shm/frame_codec.h>
#include <dztrader/log/log.h>

namespace dztrader::ctp {

using namespace dztrader::shm;

// md_api_subscribe.cpp: 订阅批次管理
// - subscribe/unsubscribe API (3 个)
// - 批次队列管理 (enqueue/send_next/on_batch_complete/resubscribe/batch_unsubscribe)
// - 订阅 SPI 回调 (on_rsp_sub_market_data / on_rsp_unsub_market_data)

void MdApi::enqueue_batches(std::vector<std::string> instruments) {
    // 防御: batch_size <= 0 会导致死循环 (validate 已校验, 此处兜底)
    int batch_size = ctp_md_config_.config().subscribe_batch_size;
    if (batch_size <= 0) {
        SPDLOG_ERROR("invalid subscribe_batch_size | value={}", batch_size);
        batch_size = 1;
    }
    auto batches = make_batches(std::move(instruments), batch_size);
    for (auto& b : batches) {
        pending_batches_.push_back(std::move(b));
    }
}

void MdApi::send_next_batch() {
    if (api_ == nullptr || state_machine_.state() != MdState::LoggedIn) {
        SPDLOG_DEBUG("send_batch discarded | reason=\"not_logged_in\"");
        return;
    }
    const auto gen = sub_generation_;  // 捕获当前代, 用于使陈旧定时器回调失效
    if (pending_batches_.empty()) {
        // 所有批次发完, 注册补订检查定时器
        sub_timer_id_ = timer_queue_.schedule_after(
            std::chrono::milliseconds(ctp_md_config_.config().sub_check_interval_ms), [this, gen]() {
                if (gen != sub_generation_) {
                    SPDLOG_DEBUG("timer callback discarded | reason=\"stale_generation\"");
                    return;
                }
                on_batch_complete();
            });
        return;
    }

    auto batch = std::move(pending_batches_.front());
    pending_batches_.pop_front();

    std::vector<char*> ptrs;
    ptrs.reserve(batch.size());
    for (auto& inst : batch) {
        ptrs.push_back(inst.data());
    }

    int ret = api_->SubscribeMarketData(ptrs.data(), static_cast<int>(ptrs.size()));
    if (ret != 0) {
        SPDLOG_WARN("sub req failed | ret={} count={}", ret, ptrs.size());
    } else {
        SPDLOG_DEBUG("sub batch sent | count={} remaining={}", ptrs.size(),
                     pending_batches_.size());
    }

    // 下一批延迟发送(定时器驱动, 非递归)
    sub_timer_id_ = timer_queue_.schedule_after(
        std::chrono::milliseconds(ctp_md_config_.config().subscribe_batch_delay_ms), [this, gen]() {
            if (gen != sub_generation_) {
                SPDLOG_DEBUG("timer callback discarded | reason=\"stale_generation\"");
                return;
            }
            send_next_batch();
        });
}

void MdApi::on_batch_complete() {
    // 防御: 断线或未登录时不再继续补订链路
    if (api_ == nullptr || state_machine_.state() != MdState::LoggedIn) {
        SPDLOG_DEBUG("batch_complete discarded | reason=\"not_logged_in\"");
        sub_check_active_ = false;
        sub_retry_count_ = 0;
        sub_timer_id_ = 0;
        return;
    }
    auto pending = sub_manager_.check_subscribe_status();
    // 决策表见 md_batch_check.h: 滞留批次 (已入队未发送) 继续发送链, 不计重试
    switch (decide_batch_check_action(!pending_batches_.empty(), !pending.empty(),
                                      sub_retry_count_, ctp_md_config_.config().sub_max_retry)) {
        case BatchCheckAction::ContinueSend:
            // 补订检查窗口内到达的新订阅: 已入队未发送, 不是"已发未确认"。
            // 误判为重试会虚增 retry_count, 订阅洪峰连续命中检查窗口会超限后
            // reset_pending_state 静默放弃订阅 (少订)。
            send_next_batch();
            return;
        case BatchCheckAction::Done:
            sub_check_active_ = false;
            sub_retry_count_ = 0;
            sub_timer_id_ = 0;
            SPDLOG_INFO("sub check done | subscribed={} expected={}",
                        sub_manager_.subscribed_count(),
                        sub_manager_.expected_subscribe_count());
            // 确认链路不触发状态转移, 完成时主动推送, 否则 UI 订阅数停在旧值
            report_md_status();
            report_progress();
            return;
        case BatchCheckAction::GiveUp:
            sub_retry_count_++;
            SPDLOG_ERROR("sub failed after retries | pending={} retry={} max={}", pending.size(),
                         sub_retry_count_, ctp_md_config_.config().sub_max_retry);
            // 仅重置 Pending 合约为 NotRequested, 已 Subscribed 的不受影响, 等下次重连
            sub_manager_.reset_pending_state();
            sub_check_active_ = false;
            sub_retry_count_ = 0;
            sub_timer_id_ = 0;
            // 放弃后推送最终态, UI 感知 subscribed < expected
            report_md_status();
            report_progress();
            return;
        case BatchCheckAction::Retry:
            sub_retry_count_++;
            SPDLOG_WARN("sub retry | pending={} retry={} max={}", pending.size(),
                        sub_retry_count_, ctp_md_config_.config().sub_max_retry);
            // 重新分批, 追加到队列
            enqueue_batches(std::move(pending));
            send_next_batch();
            return;
    }
}

void MdApi::batch_unsubscribe(const std::vector<std::string>& instruments) {
    if (instruments.empty() || api_ == nullptr) {
        return;
    }
    // 防御: batch_size <= 0 会导致死循环 (validate 已校验, 此处兜底)
    int batch_size = ctp_md_config_.config().subscribe_batch_size;
    if (batch_size <= 0) {
        SPDLOG_ERROR("invalid subscribe_batch_size | value={}", batch_size);
        batch_size = 1;
    }
    for (size_t offset = 0; offset < instruments.size(); offset += batch_size) {
        auto count = std::min<size_t>(batch_size, instruments.size() - offset);
        std::vector<char*> ptrs;
        ptrs.reserve(count);
        for (size_t i = offset; i < offset + count; ++i) {
            ptrs.push_back(const_cast<char*>(instruments[i].c_str()));  // NOLINT
        }
        int ret = api_->UnSubscribeMarketData(ptrs.data(), static_cast<int>(ptrs.size()));
        if (ret != 0) {
            SPDLOG_WARN("unsub req failed | ret={} count={}", ret, count);
        } else {
            SPDLOG_DEBUG("unsub batch sent | count={}", count);
        }
    }
}

void MdApi::on_rsp_sub_market_data(const OnRspSubMarketDataField& rsp) {
    if (api_ == nullptr) {
        SPDLOG_DEBUG("sub rsp api is null");
        return;
    }
    if (!rsp.rsp_info.has_value() || !rsp.specific_instrument.has_value()) {
        SPDLOG_ERROR("sub rsp info or instrument missing");
        return;
    }
    // 断线后丢弃残留的订阅确认: reset_all_sub_state 已将合约设为 NotRequested,
    // 残留回调会错误地设为 Subscribed, 污染状态机, 导致重连后漏订阅。
    if (state_machine_.state() != MdState::LoggedIn) {
        SPDLOG_DEBUG("sub rsp discarded | reason=\"not_logged_in\" instrument={}",
                     rsp.specific_instrument->InstrumentID);
        return;
    }
    bool success = (rsp.rsp_info->ErrorID == 0);
    if (!success) {
        SPDLOG_ERROR("sub rsp rejected | instrument={} error_id={} error_msg=\"{}\"",
                     rsp.specific_instrument->InstrumentID, rsp.rsp_info->ErrorID,
                     dztrader::to_utf8_from_gbk(rsp.rsp_info->ErrorMsg));
    }
    // 确认后乐观退订: 订阅在途期间最后订阅者已退出, on_sub_confirmed 已将状态
    // 先置 NotRequested 并返回退订信号; 此处补发退订, 由 on_unsub_confirmed erase 闭环。
    if (sub_manager_.on_sub_confirmed(rsp.specific_instrument->InstrumentID, success)) {
        SPDLOG_INFO("orphan sub confirmed, optimistic unsub | instrument={}",
                    rsp.specific_instrument->InstrumentID);
        batch_unsubscribe({std::string(rsp.specific_instrument->InstrumentID)});
        return;
    }
    if (success) {
        SPDLOG_INFO("sub ok | instrument={}", rsp.specific_instrument->InstrumentID);
    }
}

void MdApi::on_rsp_unsub_market_data(const OnRspUnSubMarketDataField& rsp) {
    if (api_ == nullptr) {
        SPDLOG_DEBUG("unsub rsp api is null");
        return;
    }
    if (!rsp.rsp_info.has_value() || !rsp.specific_instrument.has_value()) {
        SPDLOG_ERROR("unsub rsp info or instrument missing");
        return;
    }
    // 断线后丢弃残留的退订确认: 乐观退订已在 remove_subscriber 时设 NotRequested,
    // 残留回调可能 erase 仍有订阅者的条目, 导致重连后漏订阅。
    if (state_machine_.state() != MdState::LoggedIn) {
        SPDLOG_DEBUG("unsub rsp discarded | reason=\"not_logged_in\" instrument={}",
                     rsp.specific_instrument->InstrumentID);
        return;
    }
    if (rsp.rsp_info->ErrorID != 0) {
        SPDLOG_ERROR("unsub rsp rejected | instrument={} error_id={} error_msg=\"{}\"",
                     rsp.specific_instrument->InstrumentID, rsp.rsp_info->ErrorID,
                     dztrader::to_utf8_from_gbk(rsp.rsp_info->ErrorMsg));
        return;
    }
    sub_manager_.on_unsub_confirmed(rsp.specific_instrument->InstrumentID);
    SPDLOG_INFO("unsub ok | instrument={}", rsp.specific_instrument->InstrumentID);
}

}  // namespace dztrader::ctp
