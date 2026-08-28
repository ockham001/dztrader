/**
 * @file struct.h
 * @brief 共享内存结构体、帧格式结构体
 *
 * 所有结构体使用 DZ_DECLARE_ALIGNED_STRUCT 声明，保证 8 字节对齐且无 padding。
 */
#ifndef DZTRADER_STRUCT_H_
#define DZTRADER_STRUCT_H_

#include "data_type.h"

DZ_BEGIN_C_DECLS

/* ==========================================================
 *  共享内存结构体
 * ========================================================== */

/** @brief Tick 行情 */
DZ_DECLARE_ALIGNED_STRUCT(DzTick, {
    DzInstrumentId instrument_id;  ///< 合约代码
    DzDate date;                   ///< 交易日（距纪元天数）
    DzTime time;                   ///< 时间（距午夜秒数）
    double last_price;             ///< 最新价
    DzVolume volume;               ///< 成交总量
    DzSubseconds subseconds;       ///< 微秒部分 (0-999999)
    DzLargeVolume open_interest;   ///< 持仓量
    double turnover;               ///< 成交总金额
    double pre_close_price;        ///< 昨收盘价
    double open_price;             ///< 开盘价
    double highest_price;          ///< 最高价
    double lowest_price;           ///< 最低价
    double upper_limit_price;      ///< 涨停价
    double lower_limit_price;      ///< 跌停价
    double bid_price[5];           ///< 买价 1-5
    double ask_price[5];           ///< 卖价 1-5
    DzVolume bid_volume[5];        ///< 买量 1-5
    DzVolume ask_volume[5];        ///< 卖量 1-5
});

/** @brief 委托回报 */
DZ_DECLARE_ALIGNED_STRUCT(DzOrderReport, {
    DzOrderId order_id;                ///< 委托单ID
    DzStrategyId strategy_id;          ///< 策略ID
    DzInstrumentId instrument_id;      ///< 合约代码
    DzDirection direction;             ///< 买卖方向
    DzPriceType price_type;            ///< 价格类型
    DzPositionEffect position_effect;  ///< 持仓影响
    DzOrderStatus status;              ///< 委托单状态
    char reserved[4];
    double price;              ///< 委托价格
    DzVolume volume;           ///< 委托数量
    DzVolume volume_traded;    ///< 已成交数量
    DzDate date;               ///< 交易日（距纪元天数）
    DzTime time;               ///< 时间（距午夜秒数）
    DzAccountId account_id;    ///< 账户ID
    DzExchangeId exchange_id;  ///< 交易所ID
    DzOrderRemark remark;      ///< 委托单备注
});

/** @brief 成交回报 */
DZ_DECLARE_ALIGNED_STRUCT(DzTradeReport, {
    DzOrderId order_id;                ///< 关联委托单 ID
    DzStrategyId strategy_id;          ///< 策略ID
    DzInstrumentId instrument_id;      ///< 合约代码
    double price;                      ///< 成交价
    DzVolume volume;                   ///< 成交量
    DzDirection direction;             ///< 买卖方向
    DzPositionEffect position_effect;  ///< 开平仓
    char reserved[2];
    DzDate date;               ///< 交易日（距纪元天数）
    DzTime time;               ///< 时间（距午夜秒数）
    DzAccountId account_id;    ///< 账户ID
    DzExchangeId exchange_id;  ///< 交易所ID
    DzTradeId trade_id;        ///< 成交ID
});

/** @brief 持仓信息 */
DZ_DECLARE_ALIGNED_STRUCT(DzPositionInfo, {
    DzInstrumentId instrument_id;  ///< 合约代码
    DzExchangeId exchange_id;      ///< 交易所ID
    DzAccountId account_id;        ///< 账户标识
    DzLargeVolume volume;          ///< 总持仓
    DzLargeVolume frozen_volume;   ///< 冻结量
    double price;                  ///< 持仓均价
    DzLargeVolume yd_volume;       ///< 昨仓
    DzLargeVolume today_volume;    ///< 今仓
    DzDate date;                   ///< 交易日（距纪元天数）
    DzDirection direction;         ///< 持仓方向
    char reserved[3];
});

/** @brief 交易账户资金 */
DZ_DECLARE_ALIGNED_STRUCT(DzTradingAccount, {
    DzAccountId account_id;  ///< 账户标识
    double balance;          ///< 权益
    double available;        ///< 可用资金
    double frozen;           ///< 冻结资金
    double commission;       ///< 手续费
    double margin;           ///< 保证金占用
    double withdraw_quota;   ///< 可取资金
    double deposit;          ///< 入金金额
    double withdraw;         ///< 出金金额
    DzDate date;             ///< 交易日（距纪元天数）
    char reserved[4];
});

/* ==========================================================
 *  帧格式结构体
 *
 *  命名约定: 扩展头按携带的标识字段命名 (Inst/无),
 *  描述的是帧布局, 与 shm 唤醒机制无关 —— shm 写入任何帧
 *  都唤醒全部等待进程, 接收方按 frame_type / instance_id
 *  自行过滤是否处理。
 * ========================================================== */

/** @brief 帧固定头部，所有帧类型共用 */
DZ_DECLARE_ALIGNED_STRUCT(DzFrameHeader, {
    uint32_t frame_size;     ///< 整帧大小（含头部 + payload），8 的倍数
    DzFrameType frame_type;  ///< 帧类型，决定 payload 数据结构
    char reserved[2];
});

/** @brief 扩展帧头部，仅变长帧使用，紧跟 DzFrameHeader */
typedef char DzInstanceId[64];

/** @brief 扩展帧头部（含 instance_id），仅变长帧使用，紧跟 DzFrameHeader */
DZ_DECLARE_ALIGNED_STRUCT(DzExtInstFrameHeader, {
    DzInstanceId instance_id;  ///< 实例id (target 或 source)
    uint32_t data_size;        ///< payload 实际字节数（不含头部）
    char reserved[4];
});

/** @brief 扩展帧头部（无 instance_id, 8B），仅变长帧使用，紧跟 DzFrameHeader */
DZ_DECLARE_ALIGNED_STRUCT(DzExtFrameHeader, {
    uint32_t data_size;  ///< payload 实际字节数（不含头部）
    char reserved[4];
});

// 注: DzExtFrameHeader 在 2026-07-30 前指 72B 含 instance_id 的头 (现 DzExtInstFrameHeader);
// 不设同名兼容别名, 让引用旧布局的代码编译报错, 而非静默拿到错误布局

/** @brief SHM 预加载参数, DZ_FRAME_PRELOAD_EVENT_SHM 与 DZ_FRAME_PRELOAD_MD_SHM 的 payload */
DZ_DECLARE_ALIGNED_STRUCT(DzShmPreload, {
    uint64_t bytes;     ///< 预加载字节数
    uint32_t pages;     ///< 预加载页数
    uint32_t reserved;  ///< 保留字段
});

/** @brief 策略定时器触发事件, DZ_FRAME_STG_TIMER 的 payload
 *  仅 SDK 本地合成 (不写共享内存), 指针有效期至下一次 dz_next_event/dz_release */
DZ_DECLARE_ALIGNED_STRUCT(DzTimerEvent, {
    DzTimerId timer_id;  ///< 触发定时器的稳定 ID (与 dz_schedule_* 返回值一致)
});

// ============================================================================
// dztd_ctp 交易网关结构体 (向后兼容新增, 不修改现有结构)
// ============================================================================

/// 持仓明细 (OffsetConverter 依赖, 对应 CTP ReqQryInvestorPositionDetail)
DZ_DECLARE_ALIGNED_STRUCT(DzPositionDetail, {
    DzAccountId account_id;
    DzInstrumentId instrument_id;
    DzExchangeId exchange_id;
    DzDirection direction;
    int8_t hedge_flag;  // 'S'=投机, 'A'=套利, 'H'=套保
    char reserved[2];   // 对齐 DzDate 到 4 字节边界
    DzDate open_date;
    DzTradeId trade_id;
    DzLargeVolume volume;
    double open_price;
    double last_settlement_price;
    double settlement_price;
    double margin;
    double close_profit_by_date;
    double close_profit_by_trade;
    int8_t spec_posi_type;  // 上期所特殊持仓类型
    char reserved2[7];      // 对齐结构体大小到 8 字节倍数
});

/// 保证金率 (按品种归一化, 对应 CTP ReqQryInstrumentMarginRate)
DZ_DECLARE_ALIGNED_STRUCT(DzMarginRate, {
    DzAccountId account_id;
    DzInstrumentId instrument_id;  // CTP 原始返回 (品种或合约)
    char product_code[16];         // 归一化后的品种代码
    DzExchangeId exchange_id;
    int8_t hedge_flag;
    int8_t is_relative;  // 0=绝对值, 1=相对保证金率
    char reserved[6];    // 对齐后续 double 到 8 字节边界
    double long_margin_ratio_by_money;
    double long_margin_ratio_by_volume;
    double short_margin_ratio_by_money;
    double short_margin_ratio_by_volume;
    DzDate date;
    char reserved2[4];  // 对齐结构体大小到 8 字节倍数
});

/// 手续费率 (按品种归一化, 对应 CTP ReqQryInstrumentCommissionRate)
DZ_DECLARE_ALIGNED_STRUCT(DzCommissionRate, {
    DzAccountId account_id;
    DzInstrumentId instrument_id;
    char product_code[16];
    DzExchangeId exchange_id;
    double open_ratio_by_money;
    double open_ratio_by_volume;
    double close_ratio_by_money;
    double close_ratio_by_volume;
    double close_today_ratio_by_money;
    double close_today_ratio_by_volume;
    DzDate date;
    char reserved[4];  // 对齐结构体大小到 8 字节倍数
});

/// 合约信息 (对应 CTP ReqQryInstrument)
DZ_DECLARE_ALIGNED_STRUCT(DzContract, {
    DzInstrumentId instrument_id;
    DzExchangeId exchange_id;
    char name[64];            // GBK->UTF8 转换后
    int8_t product;           // 'F'=期货, 'O'=期权, 'S'=组合
    char reserved[7];         // 对齐 int64_t volume_multiple 到 8 字节边界
    int64_t volume_multiple;  // 合约乘数
    double price_tick;
    int64_t min_limit_order_volume;
    int64_t max_limit_order_volume;
    // 期权字段
    int8_t option_type;  // 1=CALL, -1=PUT, 0=非期权
    char reserved2[7];   // 对齐 double option_strike 到 8 字节边界
    double option_strike;
    DzInstrumentId option_underlying;
    DzDate option_listed;
    DzDate option_expiry;
});

/// 合约交易状态 (对应 CTP OnRtnInstrumentStatus)
DZ_DECLARE_ALIGNED_STRUCT(DzInstrumentStatus, {
    DzInstrumentId instrument_id;
    DzExchangeId exchange_id;
    int8_t status;  // 'B'=BeforeTrading, 'C'=Continous, 'D'=Closed, ...
    char reserved[3];
    DzTime time;
});

/// 出入金请求 (对应 CTP ReqFromBankToFutureByFuture)
DZ_DECLARE_ALIGNED_STRUCT(DzTransferReq, {
    DzAccountId account_id;
    char trade_code[8];  // "202001"/"202002"/"204002"
    char bank_id[8];
    char bank_account[32];
    char bank_password[32];
    char future_password[32];
    char currency_id[8];
    double trade_amount;
    int8_t cust_type;  // 0=个人, 1=机构
    char reserved[7];  // 对齐 int64_t request_id 到 8 字节边界
    int64_t request_id;
});

/// 出入金响应 (OnRsp 接收 / OnRtn 权威结果)
DZ_DECLARE_ALIGNED_STRUCT(DzTransferRsp, {
    DzAccountId account_id;
    char trade_code[8];
    int32_t error_id;
    char error_msg[128];
    char reserved[4];  // 对齐 double bank_balance 到 8 字节边界
    double bank_balance;
    double trade_amount;
    char transfer_status[2];
    char reserved2[2];
    DzTime time;
});

/// 修改密码请求
DZ_DECLARE_ALIGNED_STRUCT(DzPasswordUpdateReq, {
    DzAccountId account_id;
    int8_t password_type;  // 'U'=登录密码, 'A'=资金密码
    char reserved[7];      // 对齐结构体大小到 8 字节倍数
    char old_password[32];
    char new_password[32];
    char currency_id[8];
});

/// 修改密码响应
DZ_DECLARE_ALIGNED_STRUCT(DzPasswordUpdateRsp, {
    DzAccountId account_id;
    int8_t password_type;
    char reserved[3];  // 对齐 int32_t error_id 到 4 字节边界
    int32_t error_id;
    char error_msg[128];
    DzTime time;
    char reserved2[4];  // 对齐结构体大小到 8 字节倍数
});

/// 风控拒绝通知
DZ_DECLARE_ALIGNED_STRUCT(DzRiskReject, {
    DzAccountId account_id;
    char rule_name[32];
    char reason[128];
    int64_t timestamp_ns;
});

DZ_END_C_DECLS

#endif /* DZTRADER_STRUCT_H_ */
