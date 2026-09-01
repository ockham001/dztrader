#include "md/md_spi.h"
#include "md/md_state.h"

#include <cfloat>
#include <mutex>

#include <spdlog/spdlog.h>

#include <dztrader/core/encoding.h>
#include <dztrader/core/last_error.h>
#include <dztrader/core/string_util.h>
#include <dztrader/data_type.h>
#include <dztrader/struct.h>

namespace dztrader::ctp {

MdSpi::MdSpi(const std::string& name,
             const std::filesystem::path& shm_dir,
             const SpscQueuePtr& event_queue)
    : md_writer_{shm::SingleWriter::create(shm::channel_name(name), shm_dir,
                                        std::format("gw.{}", name))},
      event_queue_{event_queue} {
    if (!event_queue_) {
        throw std::runtime_error("event_queue is null");
    }
}

// NOLINTBEGIN: CTP 回调命名为 PascalCase, 豁免 naming 规则
void MdSpi::OnFrontConnected() {
    SPDLOG_INFO("front connected");
    try {
        event_queue_->push(EventType::OnFrontConnected,
                           new OnFrontConnectedField{.rsp_time = std::chrono::system_clock::now()});
    } catch (std::exception& e) {
        SPDLOG_ERROR("push failed | event=front_connected error=\"{}\"", e.what());
    }
}

void MdSpi::OnFrontDisconnected(int nReason) {
    SPDLOG_WARN("front disconnected | reason={}", nReason);
    try {
        event_queue_->push(EventType::OnFrontDisconnected,
                           new OnFrontDisconnectedField{.reason = nReason});
    } catch (std::exception& e) {
        SPDLOG_ERROR("push failed | event=front_disconnected error=\"{}\"", e.what());
    }
}

void MdSpi::OnHeartBeatWarning(int nTimeLapse) {
    SPDLOG_WARN("heartbeat warning | time_lapse={}", nTimeLapse);
    try {
        event_queue_->push(EventType::OnHeartbeatWarning,
                           new OnHeartBeatWarningField{.time_lapse = nTimeLapse});
    } catch (std::exception& e) {
        SPDLOG_ERROR("push failed | event=heartbeat_warning error=\"{}\"", e.what());
    }
}

void MdSpi::OnRspUserLogin(CThostFtdcRspUserLoginField* pRspUserLogin,
                           CThostFtdcRspInfoField* pRspInfo,
                           int nRequestID,
                           bool bIsLast) {
    try {
        auto now = std::chrono::system_clock::now();
        std::optional<std::string> trading_day_parse_error;
        if (pRspInfo && pRspUserLogin && pRspInfo->ErrorID == 0) {
            try {
                days_since_epoch_ = trading_day_to_days(&pRspUserLogin->TradingDay[0]);
            } catch (std::exception& e) {
                trading_day_parse_error = e.what();
                SPDLOG_WARN("trading day parse failed | error=\"{}\" trading_day=\"{}\"",
                            e.what(), pRspUserLogin->TradingDay);
            }
            SPDLOG_INFO("login rsp ok | request_id={} is_last={} trading_day={}",
                        nRequestID, bIsLast, pRspUserLogin->TradingDay);
        } else if (pRspInfo) {
            SPDLOG_ERROR("login rsp failed | error_id={} error_msg=\"{}\" request_id={}",
                         pRspInfo->ErrorID, dztrader::to_utf8_from_gbk(pRspInfo->ErrorMsg), nRequestID);
        }
        event_queue_->push(
            EventType::OnRspUserLogin,
            new OnRspUserLoginField{
                .rsp_user_login = pRspUserLogin ? std::make_optional(*pRspUserLogin) : std::nullopt,
                .rsp_info = pRspInfo ? std::make_optional(*pRspInfo) : std::nullopt,
                .request_id = nRequestID,
                .is_last = bIsLast,
                .days_since_epoch = days_since_epoch_,
                .trading_day_parse_error = std::move(trading_day_parse_error),
                .rsp_time = now});
    } catch (std::exception& e) {
        SPDLOG_ERROR("push failed | event=login error=\"{}\"", e.what());
    }
}

void MdSpi::OnRspUserLogout(CThostFtdcUserLogoutField* pUserLogout,
                            CThostFtdcRspInfoField* pRspInfo,
                            int nRequestID,
                            bool bIsLast) {
    try {
        if (pRspInfo && pRspInfo->ErrorID != 0) {
            SPDLOG_ERROR("logout rsp failed | error_id={} error_msg=\"{}\" request_id={}",
                         pRspInfo->ErrorID, dztrader::to_utf8_from_gbk(pRspInfo->ErrorMsg), nRequestID);
        } else {
            SPDLOG_INFO("logout rsp ok | request_id={} is_last={}", nRequestID, bIsLast);
        }
        event_queue_->push(
            EventType::OnRspUserLogout,
            new OnRspUserLogoutField{
                .user_logout = pUserLogout ? std::make_optional(*pUserLogout) : std::nullopt,
                .rsp_info = pRspInfo ? std::make_optional(*pRspInfo) : std::nullopt,
                .request_id = nRequestID,
                .is_last = bIsLast,
                .days_since_epoch = days_since_epoch_});
    } catch (std::exception& e) {
        SPDLOG_ERROR("push failed | event=logout error=\"{}\"", e.what());
    }
}

void MdSpi::OnRspError(CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast) {
    try {
        if (pRspInfo) {
            SPDLOG_ERROR("error rsp | error_id={} error_msg=\"{}\" request_id={} is_last={}",
                         pRspInfo->ErrorID, dztrader::to_utf8_from_gbk(pRspInfo->ErrorMsg), nRequestID, bIsLast);
        }
        event_queue_->push(
            EventType::OnRspError,
            new OnRspErrorField{.rsp_info = pRspInfo ? std::make_optional(*pRspInfo) : std::nullopt,
                                .request_id = nRequestID,
                                .is_last = bIsLast});
    } catch (std::exception& e) {
        SPDLOG_ERROR("push failed | event=error_rsp error=\"{}\"", e.what());
    }
}

void MdSpi::OnRspSubMarketData(CThostFtdcSpecificInstrumentField* pSpecificInstrument,
                               CThostFtdcRspInfoField* pRspInfo,
                               int nRequestID,
                               bool bIsLast) {
    try {
        if (pRspInfo && pRspInfo->ErrorID != 0) {
            SPDLOG_ERROR(
                "sub rsp failed | instrument={} error_id={} error_msg=\"{}\" request_id={}",
                pSpecificInstrument ? pSpecificInstrument->InstrumentID : "",
                pRspInfo->ErrorID, dztrader::to_utf8_from_gbk(pRspInfo->ErrorMsg), nRequestID);
        } else {
            SPDLOG_INFO("sub rsp ok | instrument={} request_id={} is_last={}",
                        pSpecificInstrument ? pSpecificInstrument->InstrumentID : "",
                        nRequestID, bIsLast);
        }
        event_queue_->push(
            EventType::OnRspSubMarketData,
            new OnRspSubMarketDataField{
                .specific_instrument =
                    pSpecificInstrument ? std::make_optional(*pSpecificInstrument) : std::nullopt,
                .rsp_info = pRspInfo ? std::make_optional(*pRspInfo) : std::nullopt,
                .request_id = nRequestID,
                .is_last = bIsLast});
    } catch (std::exception& e) {
        SPDLOG_ERROR("push failed | event=sub_rsp error=\"{}\"", e.what());
    }
}

void MdSpi::OnRspUnSubMarketData(CThostFtdcSpecificInstrumentField* pSpecificInstrument,
                                 CThostFtdcRspInfoField* pRspInfo,
                                 int nRequestID,
                                 bool bIsLast) {
    try {
        if (pRspInfo && pRspInfo->ErrorID != 0) {
            SPDLOG_ERROR(
                "unsub rsp failed | instrument={} error_id={} error_msg=\"{}\" request_id={}",
                pSpecificInstrument ? pSpecificInstrument->InstrumentID : "",
                pRspInfo->ErrorID, dztrader::to_utf8_from_gbk(pRspInfo->ErrorMsg), nRequestID);
        } else {
            SPDLOG_INFO("unsub rsp ok | instrument={} request_id={} is_last={}",
                        pSpecificInstrument ? pSpecificInstrument->InstrumentID : "",
                        nRequestID, bIsLast);
        }
        event_queue_->push(
            EventType::OnRspUnsubMarketData,
            new OnRspUnSubMarketDataField{
                .specific_instrument =
                    pSpecificInstrument ? std::make_optional(*pSpecificInstrument) : std::nullopt,
                .rsp_info = pRspInfo ? std::make_optional(*pRspInfo) : std::nullopt,
                .request_id = nRequestID,
                .is_last = bIsLast});
    } catch (std::exception& e) {
        SPDLOG_ERROR("push failed | event=unsub_rsp error=\"{}\"", e.what());
    }
}

void MdSpi::OnRtnDepthMarketData(CThostFtdcDepthMarketDataField* pDepthMarketData) {
    // 性能关键路径 (核心 30μs 目标): 仅做必要过滤与帧写入, 成功路径无日志
    if (pDepthMarketData == nullptr) {
        return;
    }
    // CTP 用 DBL_MAX 表示"无值", 过滤无有效价格或时间戳的帧。
    if (pDepthMarketData->LastPrice == DBL_MAX || pDepthMarketData->OpenPrice == DBL_MAX ||
        pDepthMarketData->HighestPrice == DBL_MAX || pDepthMarketData->LowestPrice == DBL_MAX ||
        pDepthMarketData->UpdateTime[0] == '\0') {
        return;
    }

    // UpdateTime 格式 "HH:MM:SS", 解析为距午夜秒数。
    const auto secs_since_midnight =
        ((static_cast<DzTime>(pDepthMarketData->UpdateTime[0]) - '0') * 36000) +
        ((static_cast<DzTime>(pDepthMarketData->UpdateTime[1]) - '0') * 3600) +
        ((static_cast<DzTime>(pDepthMarketData->UpdateTime[3]) - '0') * 600) +
        ((static_cast<DzTime>(pDepthMarketData->UpdateTime[4]) - '0') * 60) +
        ((static_cast<DzTime>(pDepthMarketData->UpdateTime[6]) - '0') * 10) +
        (static_cast<DzTime>(pDepthMarketData->UpdateTime[7]) - '0');

    // 写 SHM 帧, 与主线程的 refresh/prefetch/close 互斥。
    std::lock_guard<shm::SpinLock> lk(thread_lock_);
    auto* tick = reinterpret_cast<DzTick*>(md_writer_.open_frame(DZ_FRAME_TICK, sizeof(DzTick)));
    if (!tick) {
        SPDLOG_ERROR("write failed | event=tick err_code={} msg=\"{}\"", LastError::code(),
                     LastError::msg());
        return;
    }
    copy_string(tick->instrument_id, pDepthMarketData->InstrumentID, true);
    tick->date = days_since_epoch_;
    tick->time = secs_since_midnight;
    tick->last_price = pDepthMarketData->LastPrice;
    tick->volume = pDepthMarketData->Volume;
    tick->subseconds = pDepthMarketData->UpdateMillisec * 1000;  // ms -> us
    // CTP OpenInterest 虽为 double 类型, 实际推送整数值, 截断无精度损失
    tick->open_interest = static_cast<DzLargeVolume>(pDepthMarketData->OpenInterest);
    tick->turnover = pDepthMarketData->Turnover;
    tick->pre_close_price = pDepthMarketData->PreClosePrice;
    tick->open_price = pDepthMarketData->OpenPrice;
    tick->highest_price = pDepthMarketData->HighestPrice;
    tick->lowest_price = pDepthMarketData->LowestPrice;
    tick->upper_limit_price = pDepthMarketData->UpperLimitPrice;
    tick->lower_limit_price = pDepthMarketData->LowerLimitPrice;
    // bid/ask 5 档价格与量保留 CTP 原始值直接转发, 不处理 DBL_MAX 哨兵:
    // - 5 档并非所有交易所都推送 (有的推 5 档, 有的仅 1 档, 无效档位为 DBL_MAX)
    // - 多数策略 (普通策略、k线生成) 不使用 bid/ask
    // - 仅 tick 存储、高频策略等场景使用, 由消费者按需处理 DBL_MAX
    // - 此处增加判断会拖慢核心路径, 综合考虑直接转发为最优设计
    // bid 价格
    tick->bid_price[0] = pDepthMarketData->BidPrice1;
    tick->bid_price[1] = pDepthMarketData->BidPrice2;
    tick->bid_price[2] = pDepthMarketData->BidPrice3;
    tick->bid_price[3] = pDepthMarketData->BidPrice4;
    tick->bid_price[4] = pDepthMarketData->BidPrice5;
    // ask 价格
    tick->ask_price[0] = pDepthMarketData->AskPrice1;
    tick->ask_price[1] = pDepthMarketData->AskPrice2;
    tick->ask_price[2] = pDepthMarketData->AskPrice3;
    tick->ask_price[3] = pDepthMarketData->AskPrice4;
    tick->ask_price[4] = pDepthMarketData->AskPrice5;
    // bid 量
    tick->bid_volume[0] = pDepthMarketData->BidVolume1;
    tick->bid_volume[1] = pDepthMarketData->BidVolume2;
    tick->bid_volume[2] = pDepthMarketData->BidVolume3;
    tick->bid_volume[3] = pDepthMarketData->BidVolume4;
    tick->bid_volume[4] = pDepthMarketData->BidVolume5;
    // ask 量
    tick->ask_volume[0] = pDepthMarketData->AskVolume1;
    tick->ask_volume[1] = pDepthMarketData->AskVolume2;
    tick->ask_volume[2] = pDepthMarketData->AskVolume3;
    tick->ask_volume[3] = pDepthMarketData->AskVolume4;
    tick->ask_volume[4] = pDepthMarketData->AskVolume5;
    md_writer_.close_frame();
    md_writer_.notify_subscribers();
}

// NOLINTEND

void MdSpi::refresh_subscribers() {
    std::lock_guard<shm::SpinLock> lk(thread_lock_);
    md_writer_.refresh_subscribers();
}

void MdSpi::prefetch_for_bytes(uint64_t bytes) {
    std::lock_guard<shm::SpinLock> lk(thread_lock_);
    md_writer_.prefetch_for_bytes(bytes);
}

void MdSpi::prefetch_pages(uint64_t count) {
    std::lock_guard<shm::SpinLock> lk(thread_lock_);
    md_writer_.prefetch_pages(count);
}

void MdSpi::close_old_pages() {
    std::lock_guard<shm::SpinLock> lk(thread_lock_);
    md_writer_.close_old_pages();
}

}  // namespace dztrader::ctp
