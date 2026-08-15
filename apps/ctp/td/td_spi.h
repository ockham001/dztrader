#ifndef DZTRADER_CTP_TD_SPI_H_
#define DZTRADER_CTP_TD_SPI_H_

// CTP Trader SPI 实现 (设计 §1.2 线程模型)
//
// 职责: 仅 push 事件到 MPMC 队列, 不做业务逻辑 (无 SHM 写, 无状态变更).
// 线程: CTP 工作线程回调, 不与主线程共享可变状态.
// 多账户同进程: 每账户 1 个 SPI 线程, 共享同一个 event_queue (MPMC).
// 异常安全: 所有回调 catch (...) + 日志, 不传播到 CTP ("宁肯乱码也不能崩溃").
//
// 与 md_spi 的差异:
// - md_spi 持有 SingleWriter, OnRtnDepthMarketData 直写 SHM (低延迟路径)
// - td_spi 仅 push 事件, OnRtnOrder/OnRtnTrade 需 AccountSession 处理 (识别外部订单/维护映射)

#include <string>

#include <ThostFtdcTraderApi.h>

#include "common/ctp_events.h"
#include "td/td_events.h"  // OnRspAuthenticateField/OnRtnOrderField 等 td 事件结构

namespace dztrader::ctp {

class TdSpi : public CThostFtdcTraderSpi {
public:
    TdSpi(std::string account_id, const MpmcQueuePtr& event_queue);
    // CThostFtdcTraderSpi 无虚析构函数, 不能用 override; 但本类通过 unique_ptr<TdSpi>
    // 静态销毁, 析构函数正确调用, 不存在切片风险
    ~TdSpi() = default;

    TdSpi(const TdSpi&) = delete;
    TdSpi& operator=(const TdSpi&) = delete;

    // === 连接 ===
    void OnFrontConnected() override;
    void OnFrontDisconnected(int nReason) override;
    void OnHeartBeatWarning(int nTimeLapse) override;

    // === 认证 / 登录 / 登出 ===
    void OnRspAuthenticate(CThostFtdcRspAuthenticateField* pRspAuthenticateField,
                           CThostFtdcRspInfoField* pRspInfo,
                           int nRequestID, bool bIsLast) override;
    void OnRspUserLogin(CThostFtdcRspUserLoginField* pRspUserLogin,
                        CThostFtdcRspInfoField* pRspInfo,
                        int nRequestID, bool bIsLast) override;
    void OnRspUserLogout(CThostFtdcUserLogoutField* pUserLogout,
                         CThostFtdcRspInfoField* pRspInfo,
                         int nRequestID, bool bIsLast) override;
    void OnRspError(CThostFtdcRspInfoField* pRspInfo, int nRequestID, bool bIsLast) override;

    // === 结算单 / 密码 ===
    void OnRspSettlementInfoConfirm(CThostFtdcSettlementInfoConfirmField* pSettlementInfoConfirm,
                                     CThostFtdcRspInfoField* pRspInfo,
                                     int nRequestID, bool bIsLast) override;
    void OnRspUserPasswordUpdate(CThostFtdcUserPasswordUpdateField* pUserPasswordUpdate,
                                  CThostFtdcRspInfoField* pRspInfo,
                                  int nRequestID, bool bIsLast) override;
    void OnRspTradingAccountPasswordUpdate(
        CThostFtdcTradingAccountPasswordUpdateField* pTradingAccountPasswordUpdate,
        CThostFtdcRspInfoField* pRspInfo,
        int nRequestID, bool bIsLast) override;

    // === 查询响应 ===
    void OnRspQryOrder(CThostFtdcOrderField* pOrder, CThostFtdcRspInfoField* pRspInfo,
                       int nRequestID, bool bIsLast) override;
    void OnRspQryInvestorPosition(CThostFtdcInvestorPositionField* pInvestorPosition,
                                   CThostFtdcRspInfoField* pRspInfo,
                                   int nRequestID, bool bIsLast) override;
    void OnRspQryTradingAccount(CThostFtdcTradingAccountField* pTradingAccount,
                                 CThostFtdcRspInfoField* pRspInfo,
                                 int nRequestID, bool bIsLast) override;
    void OnRspQryInvestorPositionDetail(
        CThostFtdcInvestorPositionDetailField* pInvestorPositionDetail,
        CThostFtdcRspInfoField* pRspInfo,
        int nRequestID, bool bIsLast) override;
    void OnRspQryInstrumentMarginRate(
        CThostFtdcInstrumentMarginRateField* pInstrumentMarginRate,
        CThostFtdcRspInfoField* pRspInfo,
        int nRequestID, bool bIsLast) override;
    void OnRspQryInstrumentCommissionRate(
        CThostFtdcInstrumentCommissionRateField* pInstrumentCommissionRate,
        CThostFtdcRspInfoField* pRspInfo,
        int nRequestID, bool bIsLast) override;
    void OnRspQryInstrument(CThostFtdcInstrumentField* pInstrument,
                             CThostFtdcRspInfoField* pRspInfo,
                             int nRequestID, bool bIsLast) override;

    // === 报单录入 / 操作 ===
    void OnRspOrderInsert(CThostFtdcInputOrderField* pInputOrder,
                           CThostFtdcRspInfoField* pRspInfo,
                           int nRequestID, bool bIsLast) override;
    void OnRspOrderAction(CThostFtdcInputOrderActionField* pInputOrderAction,
                           CThostFtdcRspInfoField* pRspInfo,
                           int nRequestID, bool bIsLast) override;
    void OnErrRtnOrderInsert(CThostFtdcInputOrderField* pInputOrder,
                              CThostFtdcRspInfoField* pRspInfo) override;
    void OnErrRtnOrderAction(CThostFtdcOrderActionField* pOrderAction,
                              CThostFtdcRspInfoField* pRspInfo) override;

    // === 实时回报 (核心路径) ===
    void OnRtnOrder(CThostFtdcOrderField* pOrder) override;
    void OnRtnTrade(CThostFtdcTradeField* pTrade) override;
    void OnRtnInstrumentStatus(CThostFtdcInstrumentStatusField* pInstrumentStatus) override;

    // === 出入金 ===
    void OnRspFromBankToFutureByFuture(CThostFtdcReqTransferField* pReqTransfer,
                                        CThostFtdcRspInfoField* pRspInfo,
                                        int nRequestID, bool bIsLast) override;
    void OnRtnFromBankToFutureByFuture(CThostFtdcRspTransferField* pRspTransfer) override;

private:
    std::string account_id_;
    MpmcQueuePtr event_queue_;
};

}  // namespace dztrader::ctp

#endif  // DZTRADER_CTP_TD_SPI_H_
