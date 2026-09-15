#ifndef DZTRADER_PLATFORM_TD_ACCOUNT_OPS_H_
#define DZTRADER_PLATFORM_TD_ACCOUNT_OPS_H_

// 出入金与改密 payload（帧 DZ_FRAME_TD_TRANSFER_REQ / DZ_FRAME_TD_TRANSFER_RSP /
// DZ_FRAME_TD_TRANSFER_RTN / DZ_FRAME_TD_PASSWORD_UPDATE_REQ / DZ_FRAME_TD_PASSWORD_UPDATE_RSP，
// 契约 td-account-ops）: 五帧均为无 instance_id 的 DzExtFrameHeader JSON ext 帧，
// 事件通道广播，接收方按 payload account_id 过滤。
//
// 字段 schema 的真相源是契约 td-account-ops；改动必须同步契约与
// libs/platform/tests/td_account_ops_test.cpp 的 GoldenJson 断言。
//
// 安全约束（契约"安全约束"节）: bank_password / future_password / old_password / new_password
// 在 SHM 内明文传输；各进程禁止将 payload 原文落日志，禁止 WS/REST 回显。
// RSP/RTN 结构体不含任何密码字段。

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

namespace dztrader::platform {

/// 出入金请求（dzweb → td）。JSON key 顺序 = 字段声明顺序（GoldenJson 锁定）。
struct DzTransferReq {
    /// 目标交易账户 ID（必填）
    std::string account_id;
    /// CTP 交易代码（必填）: "202001"=银行→期货, "202002"=期货→银行, "204002"=查询
    std::string trade_code;
    /// 银行代码
    std::string bank_id;
    /// 银行账号
    std::string bank_account;
    /// 银行密码（敏感）
    std::string bank_password;
    /// 期货资金密码（敏感）
    std::string future_password;
    /// 币种，如 "CNY"
    std::string currency_id;
    /// 金额（必填）
    double trade_amount = 0.0;
    /// 0=个人，1=机构
    int32_t cust_type = 0;
    /// 请求追踪 ID
    int32_t request_id = 0;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(DzTransferReq, account_id, trade_code, bank_id,
                                                bank_account, bank_password, future_password,
                                                currency_id, trade_amount, cust_type, request_id)
};

/// 出入金响应：DZ_FRAME_TD_TRANSFER_RSP（OnRsp 即时受理）与 DZ_FRAME_TD_TRANSFER_RTN
/// （OnRtn 银行权威结果）共用本 schema。
struct DzTransferRsp {
    /// 目标交易账户 ID
    std::string account_id;
    /// 原样回传请求的交易代码
    std::string trade_code;
    /// 0=成功；非 0 为 CTP 错误码
    int32_t error_id = 0;
    /// 错误描述（UTF-8；成功为空串）
    std::string error_msg;
    /// CTP 出入金字段无银行余额，恒 0
    double bank_balance = 0.0;
    /// 金额
    double trade_amount = 0.0;
    /// CTP TransferStatus 原始单字符；空串=CTP 未返回（'\0'）
    std::string transfer_status;
    /// 距午夜秒数；-1=CTP TradeTime 缺失/非法，0=午夜
    int32_t time = 0;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(DzTransferRsp, account_id, trade_code, error_id,
                                                error_msg, bank_balance, trade_amount,
                                                transfer_status, time)
};

/// 修改密码请求（dzweb → td）
struct DzPasswordUpdateReq {
    /// 目标交易账户 ID（必填）
    std::string account_id;
    /// "U"=登录密码，"A"=资金密码（必填）
    std::string password_type;
    /// 旧密码（敏感，必填）
    std::string old_password;
    /// 新密码（敏感，必填）
    std::string new_password;
    /// 币种，资金密码场景使用
    std::string currency_id;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(DzPasswordUpdateReq, account_id, password_type,
                                                old_password, new_password, currency_id)
};

/// 修改密码响应（td → dzweb）；无异步 RTN
struct DzPasswordUpdateRsp {
    /// 目标交易账户 ID
    std::string account_id;
    /// 原样回传："U"=登录密码，"A"=资金密码
    std::string password_type;
    /// 0=成功；非 0 为 CTP 错误码
    int32_t error_id = 0;
    /// 错误描述（UTF-8；成功为空串）
    std::string error_msg;
    /// CTP 无时间字段，恒 0
    int32_t time = 0;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(DzPasswordUpdateRsp, account_id, password_type,
                                                error_id, error_msg, time)
};

// from_json 一律 WITH_DEFAULT（缺字段回退默认值、整体 null 解码为全默认结构，不抛错）；
// 契约"校验"节要求接线时 handler 显式校验必填字段，不得依赖解码错误。宏在各结构体内。

}  // namespace dztrader::platform

#endif  // DZTRADER_PLATFORM_TD_ACCOUNT_OPS_H_
