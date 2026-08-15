#ifndef DZTRADER_CTP_TD_EVENTS_H_
#define DZTRADER_CTP_TD_EVENTS_H_

// td 特有的 SPI 事件 Field 结构与 delete_data 处理.
// 与 common/ctp_events.h 配合: ctp_events.h 处理 md 类型 (0-13),
// 本文件处理 td 类型 (100+) + td 版本的 OnFrontConnected/Disconnected (复用值 1/2).
// 主线程 drain 队列时按 type 路由 (AccountSession::delete_event):
//   - td 类型 (>=100) 或 OnFrontConnected/Disconnected: 调 td_delete_event_data(event)
//   - 其他 md 类型: 调 event.delete_data() (md 内联实现)
//
// 注意: td 进程的 OnFrontConnected/Disconnected 事件用 td 版本 Field (OnTdFrontConnectedField/
// OnTdFrontDisconnectedField, 含 account_id), 与 md 模块的 OnFrontConnectedField (无 account_id)
// 同名 EventType 但不同 Field 类型. 两进程事件队列独立, 互不干扰.

#include <chrono>
#include <optional>
#include <string>

#include <ThostFtdcUserApiStruct.h>

#include "common/ctp_events.h"

namespace dztrader::ctp {

// ============================================================================
// 前置连接 (td 版本, 含 account_id, 与 md 的 OnFrontConnectedField 区分)
// ============================================================================
//
// 复用 EventType::OnFrontConnected (值=1) / OnFrontDisconnected (值=2),
// 但用 td 版本 Field (含 account_id). AccountSession::delete_event 对这两个
// EventType 走 td_delete_event_data (而非 md 的 Event::delete_data).
//
// 这两个 Field 不放 ctp_events.h, 因为 md 模块不需要 account_id,
// 且任务约束 "不修改 md 模块".

/// OnFrontConnected 回调数据 (td 版本, 含 account_id 用于多账户路由)
struct OnTdFrontConnectedField {
    std::string account_id;
    std::chrono::system_clock::time_point rsp_time;
};

/// OnFrontDisconnected 回调数据 (td 版本, 含 account_id 用于多账户路由)
struct OnTdFrontDisconnectedField {
    std::string account_id;
    int reason = 0;
};

// ============================================================================
// 认证 / 登录 / 结算单
// ============================================================================

/// OnRspAuthenticate 回调数据
struct OnRspAuthenticateField {
    std::optional<CThostFtdcRspAuthenticateField> rsp_authenticate;
    std::optional<CThostFtdcRspInfoField> rsp_info;
    int request_id = -1;
    bool is_last = true;
    std::chrono::system_clock::time_point rsp_time;
    std::string account_id;  // 多账户路由用, TdSpi push 时填入 (放末尾, 不破坏已有 layout)
};

/// OnRspTdUserLogin 回调数据 (td 专用, 含 MaxOrderRef, 与 md 的 OnRspUserLoginField 区分)
struct OnRspTdUserLoginField {
    std::optional<CThostFtdcRspUserLoginField> rsp_user_login;
    std::optional<CThostFtdcRspInfoField> rsp_info;
    int request_id = -1;
    bool is_last = true;
    /// 当前交易日距纪元天数 (登录成功且 TradingDay 解析成功时为有效值)
    int32_t days_since_epoch = std::numeric_limits<int32_t>::min();
    /// 交易日解析错误信息 (仅 TradingDay 格式非法时填)
    std::optional<std::string> trading_day_parse_error;
    std::chrono::system_clock::time_point rsp_time;
    std::string account_id;  // 多账户路由用, TdSpi push 时填入 (放末尾, 不破坏已有 layout)
};

/// OnRspSettlementInfoConfirm 回调数据
struct OnRspSettlementInfoConfirmField {
    std::optional<CThostFtdcSettlementInfoConfirmField> settlement_info_confirm;
    std::optional<CThostFtdcRspInfoField> rsp_info;
    int request_id = -1;
    bool is_last = true;
    std::string account_id;  // 多账户路由用, TdSpi push 时填入 (放末尾, 不破坏已有 layout)
};

// ============================================================================
// 查询响应 (多次回调, is_last 标记最后一批)
// ============================================================================

/// OnRspQryInstrument 回调数据
struct OnRspQryInstrumentField {
    std::optional<CThostFtdcInstrumentField> instrument;
    std::optional<CThostFtdcRspInfoField> rsp_info;
    int request_id = -1;
    bool is_last = true;
    std::string account_id;  // 多账户路由用, TdSpi push 时填入 (放末尾, 不破坏已有 layout)
};

/// OnRspQryTradingAccount 回调数据
struct OnRspQryTradingAccountField {
    std::optional<CThostFtdcTradingAccountField> trading_account;
    std::optional<CThostFtdcRspInfoField> rsp_info;
    int request_id = -1;
    bool is_last = true;
    std::string account_id;  // 多账户路由用, TdSpi push 时填入 (放末尾, 不破坏已有 layout)
};

/// OnRspQryInvestorPosition 回调数据
struct OnRspQryInvestorPositionField {
    std::optional<CThostFtdcInvestorPositionField> investor_position;
    std::optional<CThostFtdcRspInfoField> rsp_info;
    int request_id = -1;
    bool is_last = true;
    std::string account_id;  // 多账户路由用, TdSpi push 时填入 (放末尾, 不破坏已有 layout)
};

/// OnRspQryInvestorPositionDetail 回调数据
struct OnRspQryInvestorPositionDetailField {
    std::optional<CThostFtdcInvestorPositionDetailField> investor_position_detail;
    std::optional<CThostFtdcRspInfoField> rsp_info;
    int request_id = -1;
    bool is_last = true;
    std::string account_id;  // 多账户路由用, TdSpi push 时填入 (放末尾, 不破坏已有 layout)
};

/// OnRspQryInstrumentMarginRate 回调数据
struct OnRspQryInstrumentMarginRateField {
    std::optional<CThostFtdcInstrumentMarginRateField> margin_rate;
    std::optional<CThostFtdcRspInfoField> rsp_info;
    int request_id = -1;
    bool is_last = true;
    std::string account_id;  // 多账户路由用, TdSpi push 时填入 (放末尾, 不破坏已有 layout)
};

/// OnRspQryInstrumentCommissionRate 回调数据
struct OnRspQryInstrumentCommissionRateField {
    std::optional<CThostFtdcInstrumentCommissionRateField> commission_rate;
    std::optional<CThostFtdcRspInfoField> rsp_info;
    int request_id = -1;
    bool is_last = true;
    std::string account_id;  // 多账户路由用, TdSpi push 时填入 (放末尾, 不破坏已有 layout)
};

/// OnRspQryOrder 回调数据 (RESTART 补登用)
struct OnRspQryOrderField {
    std::optional<CThostFtdcOrderField> order;
    std::optional<CThostFtdcRspInfoField> rsp_info;
    int request_id = -1;
    bool is_last = true;
    std::string account_id;  // 多账户路由用, TdSpi push 时填入 (放末尾, 不破坏已有 layout)
};

// ============================================================================
// 实时回报 (核心路径)
// ============================================================================

/// OnRtnOrder 回调数据 (委托回报)
struct OnRtnOrderField {
    CThostFtdcOrderField order{};  // CTP 保证非 null, 直接拷贝。必须在偏移 0 (layout 测试约束)
    std::chrono::system_clock::time_point rsp_time;  // 接收时刻, 用于延迟诊断
    std::string account_id;  // 多账户路由用, TdSpi push 时填入 (放末尾, 不破坏 order 偏移 0)
};

/// OnRtnTrade 回调数据 (成交回报)
struct OnRtnTradeField {
    CThostFtdcTradeField trade{};
    std::chrono::system_clock::time_point rsp_time;  // 接收时刻, 用于延迟诊断
    std::string account_id;  // 多账户路由用, TdSpi push 时填入 (放末尾, 与 OnRtnOrderField 一致)
};

/// OnRtnInstrumentStatus 回调数据 (合约交易状态)
struct OnRtnInstrumentStatusField {
    CThostFtdcInstrumentStatusField instrument_status{};
    std::string account_id;  // 多账户路由用, TdSpi push 时填入 (放末尾, 与 OnRtnOrderField 一致)
};

// ============================================================================
// 报单录入 / 操作错误回报
// ============================================================================

/// OnRspOrderInsert 回调数据 (报单录入响应, 通常为错误)
struct OnRspOrderInsertField {
    std::optional<CThostFtdcInputOrderField> input_order;
    std::optional<CThostFtdcRspInfoField> rsp_info;
    int request_id = -1;
    bool is_last = true;
    std::string account_id;  // 多账户路由用, TdSpi push 时填入 (放末尾, 不破坏已有 layout)
};

/// OnRspOrderAction 回调数据 (报单操作响应, 通常为错误)
struct OnRspOrderActionField {
    std::optional<CThostFtdcInputOrderActionField> input_order_action;
    std::optional<CThostFtdcRspInfoField> rsp_info;
    int request_id = -1;
    bool is_last = true;
    std::string account_id;  // 多账户路由用, TdSpi push 时填入 (放末尾, 不破坏已有 layout)
};

/// OnErrRtnOrderInsert 回调数据 (报单录入错误回报)
struct OnErrRtnOrderInsertField {
    std::optional<CThostFtdcInputOrderField> input_order;
    std::optional<CThostFtdcRspInfoField> rsp_info;
    std::string account_id;  // 多账户路由用, TdSpi push 时填入 (放末尾, 不破坏已有 layout)
};

/// OnErrRtnOrderAction 回调数据 (报单操作错误回报)
struct OnErrRtnOrderActionField {
    std::optional<CThostFtdcOrderActionField> order_action;
    std::optional<CThostFtdcRspInfoField> rsp_info;
    std::string account_id;  // 多账户路由用, TdSpi push 时填入 (放末尾, 不破坏已有 layout)
};

// ============================================================================
// 出入金 / 密码修改
// ============================================================================

/// OnRspFromBankToFutureByFuture 回调数据 (出入金响应, CTP 用 ReqTransferField)
struct OnRspFromBankToFutureByFutureField {
    std::optional<CThostFtdcReqTransferField> req_transfer;
    std::optional<CThostFtdcRspInfoField> rsp_info;
    int request_id = -1;
    bool is_last = true;
    std::string account_id;  // 多账户路由用, TdSpi push 时填入 (放末尾, 不破坏已有 layout)
};

/// OnRtnFromBankToFutureByFuture 回调数据 (出入金实时通知, CTP 用 RspTransferField, 银行权威)
struct OnRtnFromBankToFutureByFutureField {
    CThostFtdcRspTransferField rsp_transfer{};
    std::string account_id;  // 多账户路由用, TdSpi push 时填入 (放末尾, 与 OnRtnOrderField 一致)
};

/// OnRspUserPasswordUpdate 回调数据 (修改登录密码响应)
struct OnRspUserPasswordUpdateField {
    std::optional<CThostFtdcUserPasswordUpdateField> password_update;
    std::optional<CThostFtdcRspInfoField> rsp_info;
    int request_id = -1;
    bool is_last = true;
    std::string account_id;  // 多账户路由用, TdSpi push 时填入 (放末尾, 不破坏已有 layout)
};

/// OnRspTradingAccountPasswordUpdate 回调数据 (修改资金密码响应)
struct OnRspTradingAccountPasswordUpdateField {
    std::optional<CThostFtdcTradingAccountPasswordUpdateField> password_update;
    std::optional<CThostFtdcRspInfoField> rsp_info;
    int request_id = -1;
    bool is_last = true;
    std::string account_id;  // 多账户路由用, TdSpi push 时填入 (放末尾, 不破坏已有 layout)
};

// ============================================================================
// td 事件 data 释放 (主线程 drain 队列时调用)
// ============================================================================

/// 释放 td 事件 (type >= 100 或 OnFrontConnected/Disconnected) 的 data 内存.
/// 非 td 类型不处理 (调用方应先检查 type, 或调用 event.delete_data() 走 md 路径).
/// 注意: OnFrontConnected/OnFrontDisconnected 在 td 进程用 td 版本 Field
/// (OnTdFrontConnectedField/OnTdFrontDisconnectedField), 不可走 md 的 Event::delete_data
/// (那会以 md 版本 Field 类型 static_cast, 导致 UB).
inline void td_delete_event_data(Event& event) noexcept {
    switch (event.type) {
        case EventType::OnFrontConnected:
            delete static_cast<OnTdFrontConnectedField*>(event.data);  // NOLINT
            break;
        case EventType::OnFrontDisconnected:
            delete static_cast<OnTdFrontDisconnectedField*>(event.data);  // NOLINT
            break;
        case EventType::OnRspAuthenticate:
            delete static_cast<OnRspAuthenticateField*>(event.data);  // NOLINT
            break;
        case EventType::OnRspTdUserLogin:
            delete static_cast<OnRspTdUserLoginField*>(event.data);  // NOLINT
            break;
        case EventType::OnRspSettlementInfoConfirm:
            delete static_cast<OnRspSettlementInfoConfirmField*>(event.data);  // NOLINT
            break;
        case EventType::OnRspQryInstrument:
            delete static_cast<OnRspQryInstrumentField*>(event.data);  // NOLINT
            break;
        case EventType::OnRspQryTradingAccount:
            delete static_cast<OnRspQryTradingAccountField*>(event.data);  // NOLINT
            break;
        case EventType::OnRspQryInvestorPosition:
            delete static_cast<OnRspQryInvestorPositionField*>(event.data);  // NOLINT
            break;
        case EventType::OnRspQryInvestorPositionDetail:
            delete static_cast<OnRspQryInvestorPositionDetailField*>(event.data);  // NOLINT
            break;
        case EventType::OnRspQryInstrumentMarginRate:
            delete static_cast<OnRspQryInstrumentMarginRateField*>(event.data);  // NOLINT
            break;
        case EventType::OnRspQryInstrumentCommissionRate:
            delete static_cast<OnRspQryInstrumentCommissionRateField*>(event.data);  // NOLINT
            break;
        case EventType::OnRspQryOrder:
            delete static_cast<OnRspQryOrderField*>(event.data);  // NOLINT
            break;
        case EventType::OnRtnOrder:
            delete static_cast<OnRtnOrderField*>(event.data);  // NOLINT
            break;
        case EventType::OnRtnTrade:
            delete static_cast<OnRtnTradeField*>(event.data);  // NOLINT
            break;
        case EventType::OnRtnInstrumentStatus:
            delete static_cast<OnRtnInstrumentStatusField*>(event.data);  // NOLINT
            break;
        case EventType::OnRspOrderInsert:
            delete static_cast<OnRspOrderInsertField*>(event.data);  // NOLINT
            break;
        case EventType::OnRspOrderAction:
            delete static_cast<OnRspOrderActionField*>(event.data);  // NOLINT
            break;
        case EventType::OnErrRtnOrderInsert:
            delete static_cast<OnErrRtnOrderInsertField*>(event.data);  // NOLINT
            break;
        case EventType::OnErrRtnOrderAction:
            delete static_cast<OnErrRtnOrderActionField*>(event.data);  // NOLINT
            break;
        case EventType::OnRspFromBankToFutureByFuture:
            delete static_cast<OnRspFromBankToFutureByFutureField*>(event.data);  // NOLINT
            break;
        case EventType::OnRtnFromBankToFutureByFuture:
            delete static_cast<OnRtnFromBankToFutureByFutureField*>(event.data);  // NOLINT
            break;
        case EventType::OnRspUserPasswordUpdate:
            delete static_cast<OnRspUserPasswordUpdateField*>(event.data);  // NOLINT
            break;
        case EventType::OnRspTradingAccountPasswordUpdate:
            delete static_cast<OnRspTradingAccountPasswordUpdateField*>(event.data);  // NOLINT
            break;
        default:
            // 非 td 关心的类型 (md 类型 3,4-13 或 Unknown). 调用方应先按 type 路由到
            // event.delete_data() (md 内联实现). 这里不重复处理, 仅记日志.
            // 注: OnFrontConnected(1)/OnFrontDisconnected(2) 在 td 进程也由 td 处理 (见上方 case).
            if (event.data != nullptr) {
                SPDLOG_ERROR("td_delete_event_data unhandled type | type={} data={}",
                             magic_enum::enum_name(event.type), event.data);
            }
            break;
    }
    event.data = nullptr;
}

}  // namespace dztrader::ctp

#endif  // DZTRADER_CTP_TD_EVENTS_H_
