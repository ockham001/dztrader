#include "td/td_spi.h"

#include <spdlog/spdlog.h>

#include <dztrader/core/encoding.h>
#include <dztrader/core/string_util.h>

#include "td/td_ctp_mapping.h"  // parse_ctp_date

namespace dztrader::ctp {

TdSpi::TdSpi(std::string account_id, const MpmcQueuePtr& event_queue)
    : account_id_(std::move(account_id)), event_queue_(event_queue) {
    if (!event_queue_) {
        throw std::runtime_error("TdSpi: event_queue is null");
    }
}

// NOLINTBEGIN: CTP 回调命名为 PascalCase, 豁免 naming 规则

// ============================================================================
// 连接
// ============================================================================

void TdSpi::OnFrontConnected() {
    SPDLOG_INFO("td front connected | account={}", account_id_);
    try {
        // 用 td 版本 Field (含 account_id), 不可与 md 的 OnFrontConnectedField 混用
        event_queue_->push(EventType::OnFrontConnected,
                           new OnTdFrontConnectedField{.account_id = account_id_,
                                                       .rsp_time = std::chrono::system_clock::now()});
    } catch (const std::exception& e) {
        SPDLOG_ERROR("td connected push failed | account={} error=\"{}\"", account_id_, e.what());
    }
}

void TdSpi::OnFrontDisconnected(int nReason) {
    SPDLOG_WARN("td front disconnected | account={} reason={}", account_id_, nReason);
    try {
        // 用 td 版本 Field (含 account_id), 不可与 md 的 OnFrontDisconnectedField 混用
        event_queue_->push(EventType::OnFrontDisconnected,
                           new OnTdFrontDisconnectedField{.account_id = account_id_,
                                                           .reason = nReason});
    } catch (const std::exception& e) {
        SPDLOG_ERROR("td disconnected push failed | account={} error=\"{}\"", account_id_, e.what());
    }
}

void TdSpi::OnHeartBeatWarning(int nTimeLapse) {
    // 心跳警告仅日志, 不入队 (无业务影响, 主线程无需处理)
    SPDLOG_WARN("td heartbeat warning | account={} time_lapse={}", account_id_, nTimeLapse);
}

// ============================================================================
// 认证 / 登录 / 登出
// ============================================================================

void TdSpi::OnRspAuthenticate(CThostFtdcRspAuthenticateField* pRspAuthenticateField,
                              CThostFtdcRspInfoField* pRspInfo,
                              int nRequestID, bool bIsLast) {
    try {
        if (pRspInfo && pRspInfo->ErrorID != 0) {
            SPDLOG_ERROR("td auth failed | account={} error_id={} error_msg=\"{}\"",
                         account_id_, pRspInfo->ErrorID,
                         dztrader::to_utf8_from_gbk(pRspInfo->ErrorMsg));
        } else {
            SPDLOG_INFO("td auth ok | account={} request_id={}", account_id_, nRequestID);
        }
        event_queue_->push(
            EventType::OnRspAuthenticate,
            new OnRspAuthenticateField{
                .rsp_authenticate = pRspAuthenticateField
                                        ? std::make_optional(*pRspAuthenticateField)
                                        : std::nullopt,
                .rsp_info = pRspInfo ? std::make_optional(*pRspInfo) : std::nullopt,
                .request_id = nRequestID,
                .is_last = bIsLast,
                .rsp_time = std::chrono::system_clock::now(),
                .account_id = account_id_});
    } catch (const std::exception& e) {
        SPDLOG_ERROR("td auth push failed | account={} error=\"{}\"", account_id_, e.what());
    }
}

void TdSpi::OnRspUserLogin(CThostFtdcRspUserLoginField* pRspUserLogin,
                           CThostFtdcRspInfoField* pRspInfo,
                           int nRequestID, bool bIsLast) {
    try {
        auto now = std::chrono::system_clock::now();
        int32_t days = std::numeric_limits<int32_t>::min();
        std::optional<std::string> parse_err;
        if (pRspInfo && pRspUserLogin && pRspInfo->ErrorID == 0) {
            // 解析 TradingDay "YYYYMMDD" 为距纪元天数
            int32_t parsed = parse_ctp_date(pRspUserLogin->TradingDay);
            if (parsed < 0) {
                parse_err = std::format("invalid TradingDay: {}", pRspUserLogin->TradingDay);
                SPDLOG_WARN("td trading day parse failed | account={} trading_day=\"{}\"",
                            account_id_, pRspUserLogin->TradingDay);
            } else {
                days = parsed;
                SPDLOG_INFO("td login ok | account={} trading_day={} order_ref_max=\"{}\"",
                            account_id_, pRspUserLogin->TradingDay, pRspUserLogin->MaxOrderRef);
            }
        } else if (pRspInfo) {
            SPDLOG_ERROR("td login failed | account={} error_id={} error_msg=\"{}\"",
                         account_id_, pRspInfo->ErrorID,
                         dztrader::to_utf8_from_gbk(pRspInfo->ErrorMsg));
        }
        event_queue_->push(
            EventType::OnRspTdUserLogin,
            new OnRspTdUserLoginField{
                .rsp_user_login = pRspUserLogin ? std::make_optional(*pRspUserLogin) : std::nullopt,
                .rsp_info = pRspInfo ? std::make_optional(*pRspInfo) : std::nullopt,
                .request_id = nRequestID,
                .is_last = bIsLast,
                .days_since_epoch = days,
                .trading_day_parse_error = std::move(parse_err),
                .rsp_time = now,
                .account_id = account_id_});
    } catch (const std::exception& e) {
        SPDLOG_ERROR("td login push failed | account={} error=\"{}\"", account_id_, e.what());
    }
}

void TdSpi::OnRspUserLogout(CThostFtdcUserLogoutField* pUserLogout,
                            CThostFtdcRspInfoField* pRspInfo,
                            int nRequestID, bool bIsLast) {
    (void)pUserLogout;
    (void)bIsLast;
    // 登出仅日志, 不入队 (AccountSession 无需处理, 状态由 disconnect 驱动)
    if (pRspInfo && pRspInfo->ErrorID != 0) {
        SPDLOG_ERROR("td logout failed | account={} error_id={}",
                     account_id_, pRspInfo->ErrorID);
    } else {
        SPDLOG_INFO("td logout ok | account={} request_id={}", account_id_, nRequestID);
    }
}

void TdSpi::OnRspError(CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast) {
    (void)bIsLast;
    // 通用错误仅日志, 不入队 (具体业务错误有对应 OnRsp* 回调)
    // 异常保护: to_utf8_from_gbk 可能抛异常, 不能传播到 CTP
    try {
        if (pRspInfo) {
            SPDLOG_ERROR("td rsp error | account={} error_id={} error_msg=\"{}\" request_id={}",
                         account_id_, pRspInfo->ErrorID,
                         dztrader::to_utf8_from_gbk(pRspInfo->ErrorMsg), nRequestID);
        }
    } catch (const std::exception& e) {
        SPDLOG_ERROR("td rsp error log failed | account={} error=\"{}\"", account_id_, e.what());
    }
}

// ============================================================================
// 结算单 / 密码
// ============================================================================

void TdSpi::OnRspSettlementInfoConfirm(CThostFtdcSettlementInfoConfirmField* pSettlementInfoConfirm,
                                        CThostFtdcRspInfoField* pRspInfo,
                                        int nRequestID, bool bIsLast) {
    try {
        if (pRspInfo && pRspInfo->ErrorID != 0) {
            SPDLOG_ERROR("td settlement confirm failed | account={} error_id={}",
                         account_id_, pRspInfo->ErrorID);
        } else {
            SPDLOG_INFO("td settlement confirmed | account={}", account_id_);
        }
        event_queue_->push(
            EventType::OnRspSettlementInfoConfirm,
            new OnRspSettlementInfoConfirmField{
                .settlement_info_confirm = pSettlementInfoConfirm
                                               ? std::make_optional(*pSettlementInfoConfirm)
                                               : std::nullopt,
                .rsp_info = pRspInfo ? std::make_optional(*pRspInfo) : std::nullopt,
                .request_id = nRequestID,
                .is_last = bIsLast,
                .account_id = account_id_});
    } catch (const std::exception& e) {
        SPDLOG_ERROR("td settlement push failed | account={} error=\"{}\"", account_id_, e.what());
    }
}

void TdSpi::OnRspUserPasswordUpdate(CThostFtdcUserPasswordUpdateField* pUserPasswordUpdate,
                                     CThostFtdcRspInfoField* pRspInfo,
                                     int nRequestID, bool bIsLast) {
    try {
        if (pRspInfo && pRspInfo->ErrorID != 0) {
            SPDLOG_ERROR("td user password update failed | account={} error_id={}",
                         account_id_, pRspInfo->ErrorID);
        } else {
            SPDLOG_INFO("td user password updated | account={}", account_id_);
        }
        event_queue_->push(
            EventType::OnRspUserPasswordUpdate,
            new OnRspUserPasswordUpdateField{
                .password_update = pUserPasswordUpdate
                                       ? std::make_optional(*pUserPasswordUpdate)
                                       : std::nullopt,
                .rsp_info = pRspInfo ? std::make_optional(*pRspInfo) : std::nullopt,
                .request_id = nRequestID,
                .is_last = bIsLast,
                .account_id = account_id_});
    } catch (const std::exception& e) {
        SPDLOG_ERROR("td user password push failed | account={} error=\"{}\"",
                     account_id_, e.what());
    }
}

void TdSpi::OnRspTradingAccountPasswordUpdate(
    CThostFtdcTradingAccountPasswordUpdateField* pTradingAccountPasswordUpdate,
    CThostFtdcRspInfoField* pRspInfo,
    int nRequestID, bool bIsLast) {
    try {
        if (pRspInfo && pRspInfo->ErrorID != 0) {
            SPDLOG_ERROR("td trading account password update failed | account={} error_id={}",
                         account_id_, pRspInfo->ErrorID);
        } else {
            SPDLOG_INFO("td trading account password updated | account={}", account_id_);
        }
        event_queue_->push(
            EventType::OnRspTradingAccountPasswordUpdate,
            new OnRspTradingAccountPasswordUpdateField{
                .password_update = pTradingAccountPasswordUpdate
                                       ? std::make_optional(*pTradingAccountPasswordUpdate)
                                       : std::nullopt,
                .rsp_info = pRspInfo ? std::make_optional(*pRspInfo) : std::nullopt,
                .request_id = nRequestID,
                .is_last = bIsLast,
                .account_id = account_id_});
    } catch (const std::exception& e) {
        SPDLOG_ERROR("td trading account password push failed | account={} error=\"{}\"",
                     account_id_, e.what());
    }
}

// ============================================================================
// 查询响应
// ============================================================================

void TdSpi::OnRspQryOrder(CThostFtdcOrderField* pOrder, CThostFtdcRspInfoField* pRspInfo,
                          int nRequestID, bool bIsLast) {
    try {
        event_queue_->push(
            EventType::OnRspQryOrder,
            new OnRspQryOrderField{
                .order = pOrder ? std::make_optional(*pOrder) : std::nullopt,
                .rsp_info = pRspInfo ? std::make_optional(*pRspInfo) : std::nullopt,
                .request_id = nRequestID,
                .is_last = bIsLast,
                .account_id = account_id_});
    } catch (const std::exception& e) {
        SPDLOG_ERROR("td qry order push failed | account={} error=\"{}\"",
                     account_id_, e.what());
    }
}

void TdSpi::OnRspQryInvestorPosition(CThostFtdcInvestorPositionField* pInvestorPosition,
                                      CThostFtdcRspInfoField* pRspInfo,
                                      int nRequestID, bool bIsLast) {
    try {
        event_queue_->push(
            EventType::OnRspQryInvestorPosition,
            new OnRspQryInvestorPositionField{
                .investor_position = pInvestorPosition
                                         ? std::make_optional(*pInvestorPosition)
                                         : std::nullopt,
                .rsp_info = pRspInfo ? std::make_optional(*pRspInfo) : std::nullopt,
                .request_id = nRequestID,
                .is_last = bIsLast,
                .account_id = account_id_});
    } catch (const std::exception& e) {
        SPDLOG_ERROR("td qry position push failed | account={} error=\"{}\"",
                     account_id_, e.what());
    }
}

void TdSpi::OnRspQryTradingAccount(CThostFtdcTradingAccountField* pTradingAccount,
                                    CThostFtdcRspInfoField* pRspInfo,
                                    int nRequestID, bool bIsLast) {
    try {
        event_queue_->push(
            EventType::OnRspQryTradingAccount,
            new OnRspQryTradingAccountField{
                .trading_account = pTradingAccount
                                       ? std::make_optional(*pTradingAccount)
                                       : std::nullopt,
                .rsp_info = pRspInfo ? std::make_optional(*pRspInfo) : std::nullopt,
                .request_id = nRequestID,
                .is_last = bIsLast,
                .account_id = account_id_});
    } catch (const std::exception& e) {
        SPDLOG_ERROR("td qry account push failed | account={} error=\"{}\"",
                     account_id_, e.what());
    }
}

void TdSpi::OnRspQryInvestorPositionDetail(
    CThostFtdcInvestorPositionDetailField* pInvestorPositionDetail,
    CThostFtdcRspInfoField* pRspInfo,
    int nRequestID, bool bIsLast) {
    try {
        event_queue_->push(
            EventType::OnRspQryInvestorPositionDetail,
            new OnRspQryInvestorPositionDetailField{
                .investor_position_detail = pInvestorPositionDetail
                                                ? std::make_optional(*pInvestorPositionDetail)
                                                : std::nullopt,
                .rsp_info = pRspInfo ? std::make_optional(*pRspInfo) : std::nullopt,
                .request_id = nRequestID,
                .is_last = bIsLast,
                .account_id = account_id_});
    } catch (const std::exception& e) {
        SPDLOG_ERROR("td qry position detail push failed | account={} error=\"{}\"",
                     account_id_, e.what());
    }
}

void TdSpi::OnRspQryInstrumentMarginRate(
    CThostFtdcInstrumentMarginRateField* pInstrumentMarginRate,
    CThostFtdcRspInfoField* pRspInfo,
    int nRequestID, bool bIsLast) {
    try {
        event_queue_->push(
            EventType::OnRspQryInstrumentMarginRate,
            new OnRspQryInstrumentMarginRateField{
                .margin_rate = pInstrumentMarginRate
                                   ? std::make_optional(*pInstrumentMarginRate)
                                   : std::nullopt,
                .rsp_info = pRspInfo ? std::make_optional(*pRspInfo) : std::nullopt,
                .request_id = nRequestID,
                .is_last = bIsLast,
                .account_id = account_id_});
    } catch (const std::exception& e) {
        SPDLOG_ERROR("td qry margin rate push failed | account={} error=\"{}\"",
                     account_id_, e.what());
    }
}

void TdSpi::OnRspQryInstrumentCommissionRate(
    CThostFtdcInstrumentCommissionRateField* pInstrumentCommissionRate,
    CThostFtdcRspInfoField* pRspInfo,
    int nRequestID, bool bIsLast) {
    try {
        event_queue_->push(
            EventType::OnRspQryInstrumentCommissionRate,
            new OnRspQryInstrumentCommissionRateField{
                .commission_rate = pInstrumentCommissionRate
                                       ? std::make_optional(*pInstrumentCommissionRate)
                                       : std::nullopt,
                .rsp_info = pRspInfo ? std::make_optional(*pRspInfo) : std::nullopt,
                .request_id = nRequestID,
                .is_last = bIsLast,
                .account_id = account_id_});
    } catch (const std::exception& e) {
        SPDLOG_ERROR("td qry commission rate push failed | account={} error=\"{}\"",
                     account_id_, e.what());
    }
}

void TdSpi::OnRspQryInstrument(CThostFtdcInstrumentField* pInstrument,
                                CThostFtdcRspInfoField* pRspInfo,
                                int nRequestID, bool bIsLast) {
    try {
        event_queue_->push(
            EventType::OnRspQryInstrument,
            new OnRspQryInstrumentField{
                .instrument = pInstrument ? std::make_optional(*pInstrument) : std::nullopt,
                .rsp_info = pRspInfo ? std::make_optional(*pRspInfo) : std::nullopt,
                .request_id = nRequestID,
                .is_last = bIsLast,
                .account_id = account_id_});
    } catch (const std::exception& e) {
        SPDLOG_ERROR("td qry instrument push failed | account={} error=\"{}\"",
                     account_id_, e.what());
    }
}

// ============================================================================
// 报单录入 / 操作
// ============================================================================

void TdSpi::OnRspOrderInsert(CThostFtdcInputOrderField* pInputOrder,
                              CThostFtdcRspInfoField* pRspInfo,
                              int nRequestID, bool bIsLast) {
    try {
        if (pRspInfo && pRspInfo->ErrorID != 0) {
            SPDLOG_ERROR("td order insert rsp error | account={} error_id={} error_msg=\"{}\"",
                         account_id_, pRspInfo->ErrorID,
                         dztrader::to_utf8_from_gbk(pRspInfo->ErrorMsg));
        }
        event_queue_->push(
            EventType::OnRspOrderInsert,
            new OnRspOrderInsertField{
                .input_order = pInputOrder ? std::make_optional(*pInputOrder) : std::nullopt,
                .rsp_info = pRspInfo ? std::make_optional(*pRspInfo) : std::nullopt,
                .request_id = nRequestID,
                .is_last = bIsLast,
                .account_id = account_id_});
    } catch (const std::exception& e) {
        SPDLOG_ERROR("td order insert rsp push failed | account={} error=\"{}\"",
                     account_id_, e.what());
    }
}

void TdSpi::OnRspOrderAction(CThostFtdcInputOrderActionField* pInputOrderAction,
                              CThostFtdcRspInfoField* pRspInfo,
                              int nRequestID, bool bIsLast) {
    try {
        if (pRspInfo && pRspInfo->ErrorID != 0) {
            SPDLOG_ERROR("td order action rsp error | account={} error_id={} error_msg=\"{}\"",
                         account_id_, pRspInfo->ErrorID,
                         dztrader::to_utf8_from_gbk(pRspInfo->ErrorMsg));
        }
        event_queue_->push(
            EventType::OnRspOrderAction,
            new OnRspOrderActionField{
                .input_order_action = pInputOrderAction
                                          ? std::make_optional(*pInputOrderAction)
                                          : std::nullopt,
                .rsp_info = pRspInfo ? std::make_optional(*pRspInfo) : std::nullopt,
                .request_id = nRequestID,
                .is_last = bIsLast,
                .account_id = account_id_});
    } catch (const std::exception& e) {
        SPDLOG_ERROR("td order action rsp push failed | account={} error=\"{}\"",
                     account_id_, e.what());
    }
}

void TdSpi::OnErrRtnOrderInsert(CThostFtdcInputOrderField* pInputOrder,
                                 CThostFtdcRspInfoField* pRspInfo) {
    try {
        if (pRspInfo) {
            SPDLOG_ERROR("td err rtn order insert | account={} error_id={} error_msg=\"{}\"",
                         account_id_, pRspInfo->ErrorID,
                         dztrader::to_utf8_from_gbk(pRspInfo->ErrorMsg));
        }
        event_queue_->push(
            EventType::OnErrRtnOrderInsert,
            new OnErrRtnOrderInsertField{
                .input_order = pInputOrder ? std::make_optional(*pInputOrder) : std::nullopt,
                .rsp_info = pRspInfo ? std::make_optional(*pRspInfo) : std::nullopt,
                .account_id = account_id_});
    } catch (const std::exception& e) {
        SPDLOG_ERROR("td err rtn order insert push failed | account={} error=\"{}\"",
                     account_id_, e.what());
    }
}

void TdSpi::OnErrRtnOrderAction(CThostFtdcOrderActionField* pOrderAction,
                                 CThostFtdcRspInfoField* pRspInfo) {
    try {
        if (pRspInfo) {
            SPDLOG_ERROR("td err rtn order action | account={} error_id={} error_msg=\"{}\"",
                         account_id_, pRspInfo->ErrorID,
                         dztrader::to_utf8_from_gbk(pRspInfo->ErrorMsg));
        }
        event_queue_->push(
            EventType::OnErrRtnOrderAction,
            new OnErrRtnOrderActionField{
                .order_action = pOrderAction ? std::make_optional(*pOrderAction) : std::nullopt,
                .rsp_info = pRspInfo ? std::make_optional(*pRspInfo) : std::nullopt,
                .account_id = account_id_});
    } catch (const std::exception& e) {
        SPDLOG_ERROR("td err rtn order action push failed | account={} error=\"{}\"",
                     account_id_, e.what());
    }
}

// ============================================================================
// 实时回报 (核心路径)
// ============================================================================

void TdSpi::OnRtnOrder(CThostFtdcOrderField* pOrder) {
    // CTP 文档保证 pOrder 非 null, 但防御性检查
    if (pOrder == nullptr) {
        SPDLOG_WARN("td rtn order null | account={}", account_id_);
        return;
    }
    try {
        event_queue_->push(
            EventType::OnRtnOrder,
            new OnRtnOrderField{.order = *pOrder,
                                .rsp_time = std::chrono::system_clock::now(),
                                .account_id = account_id_});
    } catch (const std::exception& e) {
        SPDLOG_ERROR("td rtn order push failed | account={} error=\"{}\" instrument={} order_ref={}",
                     account_id_, e.what(), pOrder->InstrumentID, pOrder->OrderRef);
    }
}

void TdSpi::OnRtnTrade(CThostFtdcTradeField* pTrade) {
    if (pTrade == nullptr) {
        SPDLOG_WARN("td rtn trade null | account={}", account_id_);
        return;
    }
    try {
        event_queue_->push(
            EventType::OnRtnTrade,
            new OnRtnTradeField{.trade = *pTrade,
                                 .rsp_time = std::chrono::system_clock::now(),
                                 .account_id = account_id_});
    } catch (const std::exception& e) {
        SPDLOG_ERROR("td rtn trade push failed | account={} error=\"{}\" instrument={} trade_id={}",
                     account_id_, e.what(), pTrade->InstrumentID, pTrade->TradeID);
    }
}

void TdSpi::OnRtnInstrumentStatus(CThostFtdcInstrumentStatusField* pInstrumentStatus) {
    if (pInstrumentStatus == nullptr) {
        return;
    }
    try {
        event_queue_->push(
            EventType::OnRtnInstrumentStatus,
            new OnRtnInstrumentStatusField{.instrument_status = *pInstrumentStatus,
                                           .account_id = account_id_});
    } catch (const std::exception& e) {
        SPDLOG_ERROR("td rtn instrument status push failed | account={} error=\"{}\"",
                     account_id_, e.what());
    }
}

// ============================================================================
// 出入金
// ============================================================================

void TdSpi::OnRspFromBankToFutureByFuture(CThostFtdcReqTransferField* pReqTransfer,
                                           CThostFtdcRspInfoField* pRspInfo,
                                           int nRequestID, bool bIsLast) {
    try {
        if (pRspInfo && pRspInfo->ErrorID != 0) {
            SPDLOG_ERROR("td transfer rsp error | account={} error_id={}",
                         account_id_, pRspInfo->ErrorID);
        }
        event_queue_->push(
            EventType::OnRspFromBankToFutureByFuture,
            new OnRspFromBankToFutureByFutureField{
                .req_transfer = pReqTransfer ? std::make_optional(*pReqTransfer) : std::nullopt,
                .rsp_info = pRspInfo ? std::make_optional(*pRspInfo) : std::nullopt,
                .request_id = nRequestID,
                .is_last = bIsLast,
                .account_id = account_id_});
    } catch (const std::exception& e) {
        SPDLOG_ERROR("td transfer rsp push failed | account={} error=\"{}\"",
                     account_id_, e.what());
    }
}

void TdSpi::OnRtnFromBankToFutureByFuture(CThostFtdcRspTransferField* pRspTransfer) {
    if (pRspTransfer == nullptr) {
        return;
    }
    try {
        event_queue_->push(
            EventType::OnRtnFromBankToFutureByFuture,
            new OnRtnFromBankToFutureByFutureField{.rsp_transfer = *pRspTransfer,
                                                    .account_id = account_id_});
    } catch (const std::exception& e) {
        SPDLOG_ERROR("td rtn transfer push failed | account={} error=\"{}\"",
                     account_id_, e.what());
    }
}

// NOLINTEND

}  // namespace dztrader::ctp
