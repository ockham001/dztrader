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
    uint64_t seq;              ///< 账户级状态变更序号（账户内全类型共享、跨日累积单调）
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
    uint64_t seq;              ///< 账户级状态变更序号（账户内全类型共享、跨日累积单调）
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
    uint64_t seq;                  ///< 账户级状态变更序号（账户内全类型共享、跨日累积单调）
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
    uint64_t seq;            ///< 账户级状态变更序号（账户内全类型共享、跨日累积单调）
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

/** @brief SHM 预加载参数, 平台预加载通知帧的 payload (帧类型见 core_data_type.h) */
DZ_DECLARE_ALIGNED_STRUCT(DzShmPreload, {
    uint64_t bytes;     ///< 预加载字节数
    uint32_t pages;     ///< 预加载页数
    uint32_t reserved;  ///< 保留字段
});

/** @brief 策略调度触发通知 (某次 dz_schedule_* 到期), DZ_FRAME_SCHEDULE 的 payload
 *  仅 SDK 本地合成 (不写共享内存), 指针有效期至下一次 dz_next_event/dz_release */
DZ_DECLARE_ALIGNED_STRUCT(DzScheduleEvent, {
    DzTimerId timer_id;  ///< 触发定时器的稳定 ID (与 dz_schedule_* 返回值一致)
});

/** @brief 账户登录状态推送 payload (DZ_FRAME_ACCOUNT_STATUS=2018, basic 广播帧,
 *  身份在 payload account_id + gateway_name, 无扩展头, 契约 account-status) */
DZ_DECLARE_ALIGNED_STRUCT(DzAccountStatus, {
    DzAccountId account_id;    ///< 账户标识
    DzInstanceId gateway_name; ///< 网关进程名 (dztd_ctp/dztd_xtp); master 兜底应答为空串
    DzDate trading_day;        ///< 交易日（距纪元天数; Offline 时为 0）
    DzAccountState state;      ///< 三态 (DZ_ACCOUNT_*)
    char reserved[3];
});

// ============================================================================
// dztd_ctp 交易网关结构体 (向后兼容新增, 不修改现有结构)
// ============================================================================

/// 保证金率 (按品种归一化, 对应 CTP ReqQryInstrumentMarginRate)
/// by_volume 系「每手/每张固定金额」，期权"每张固定"保证金/手续费也走 by_volume；
/// 保证金币种不落本表 (见帧契约《手续费/保证金》: 线性=contract.currency, 反向=base_asset)。
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
/// by_volume 系「每手/每张固定金额」，期权"每张固定"手续费也走 by_volume。
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

/// 合约信息 — 平台统一合约静态表
///
/// 发布方: td/md 进程 (登录/每日刷新时发布); 消费方: 策略进程只读
///
/// 身份规则 (instrument_id 全帧统一, 行情/查询/下单共用):
/// - instrument_id 是平台全局唯一键, 订单/持仓/行情等所有帧以它引用;
///   CTP 合约用裸交易所代码 (rb2601/MA601), 其他网关加网关段前缀
///   (IB.266004536 / BNS.BTCUSDT), 保证跨网关唯一, 网关发布时校验。
/// - symbol 是场所原生代码, 网关对场所 API 发单/订阅时原样透传;
///   禁止从 instrument_id 反向解析 symbol (前缀仅为展示约定, 非语法规则)。
/// - underlying_id 必须是合约表内有效行的 instrument_id (指数行合法)。
///
/// 数量语义 (平台单位, 全帧整数):
/// - 期货/期权=手/张, 证券=股, 币圈=stepSize 粒度;
///   下单/持仓帧全部整数 (DzVolume), 原生数量换算仅在网关边界发生 (volume_step)。
DZ_DECLARE_ALIGNED_STRUCT(DzInstrumentInfo, {
    /* ---- 身份区 (320 字节) ---- */
    DzInstrumentId instrument_id;      ///< 平台全局唯一键 (CTP 裸码; 其他网关 "段.原生码")
    DzExchangeId exchange_id;          ///< 交易场所代码 (平台注册表: SHFE/CME/BNS/BNF)
    char symbol[88];                   ///< 场所原生代码 (CTP 6.3.15+ 为 81 字节, 原样透传)
    char name[128];                    ///< 显示名, UTF-8, 网关负责转码; 纯展示不参与匹配

    /* ---- 分类与结算语义 (8 字节) ---- */
    int8_t product;                    ///< 产品类型 (DZ_PRODUCT_*; INDEX=非交易参考行)
    int8_t settle_cycle;               ///< 交收周期: -1=不适用(衍生品), 0=T+0, 1=T+1(A股/美股), 2=T+2(FX现货/HK)
    int8_t settlement_method;          ///< 交割/结算方式: 0=未知, 1=实物, 2=现金(股指期货/期权)
    int8_t is_inverse;                 ///< 反向合约: 0=线性(PnL∝Δp), 1=反向(PnL∝Δp/p, 保证金币种=base_asset)
    char reserved0[4];                 ///< 对齐 currency 到 8 字节边界

    /* ---- 货币区 (16 字节) ---- */
    char currency[8];                  ///< 计价/结算货币 (ISO 4217 或资产码: CNY/USD/USDT; 空=跟随账户本币)
    char base_asset[8];                ///< 基础资产 (SPOT/FOREX/PERPETUAL: EUR/BTC/XAU; 其他衍生品留空)

    /* ---- 量价区 (40 字节) ---- */
    int64_t min_order_volume;          ///< 最小下单量, 平台单位 (A股=100 股, 限买不限卖; <=0=未提供)
    int64_t max_order_volume;          ///< 最大下单量, 平台单位 (<=0=无限制)
    double volume_multiple;            ///< 价值乘数, 仅用于 PnL/保证金浮点运算 (期货=乘数; 证券=1; 外汇=每手10万单位)
    double price_tick;                 ///< 最小变动价位 (必须 > 0, 网关保证)
    double volume_step;                ///< 1 平台单位对应的原生数量 (网关换算; CTP 恒 1; 0=视为 1)

    /* ---- 生命周期 (8 字节, 全品种通用) ---- */
    DzDate listed_date;                ///< 上市日 (DZ_DATE_NA=未知; 回测移仓不得交易未上市合约)
    DzDate expiry_date;                ///< 到期/最后交易日 (期货/期权/权证/转债必填; 永续/现货=DZ_DATE_NA)

    /* ---- 衍生品属性区 (112 字节, 按产品类型选用) ---- */
    int8_t option_type;                ///< 期权方向: DZ_OPTION_CALL(1) / DZ_OPTION_PUT(-1); 0=非期权
    int8_t option_exercise_style;      ///< 行权方式: 0=未知, 1=欧式(境内ETF/股指), 2=美式(美股), 3=百慕大
    DzInstrumentId underlying_id;      ///< 标的合约 instrument_id (期权/权证/可转债; 指数行合法)
    char reserved1[6];                 ///< 对齐 option_strike 到 8 字节边界
    double option_strike;              ///< 行权/转股价 (期权/权证/可转债)
    char option_series[8];             ///< 同价调整序列 (CZCE 期权 M/A/B 调整合约; 空=无)
});                                    ///< 总计 504 字节, 8 字节对齐

/// 组合合约腿 (product == SPREAD 时每腿一行)
DZ_DECLARE_ALIGNED_STRUCT(DzInstrumentLeg, {
    DzInstrumentId combo_id;           ///< 组合合约 instrument_id
    DzInstrumentId leg_id;             ///< 腿合约 instrument_id (须为主表有效行)
    int8_t direction;                  ///< 腿方向 (DZ_DIRECTION_*)
    char reserved[7];                  ///< 对齐 ratio 到 8 字节边界
    double ratio;                      ///< 腿比例 (通常 1)
});                                    ///< 192 字节

/// 合约扩展属性 K-V (场所特有参考信息的合法出口: isin/conid/trading_active 等;
/// 禁止承载交易决策必需字段 — 那些必须进主表或伴随表)
DZ_DECLARE_ALIGNED_STRUCT(DzInstrumentExt, {
    DzInstrumentId instrument_id;
    char key[32];
    char value[128];
    char reserved[8];                  ///< 256 字节
});

/// 阶梯最小变动价位 (JPX 等按价位分档的场所; 每档一行, price_from 升序)
/// 未发布该表的合约一律使用主表 price_tick
DZ_DECLARE_ALIGNED_STRUCT(DzInstrumentTickTier, {
    DzInstrumentId instrument_id;
    double price_from;                 ///< 本档起始价 (含)
    double price_tick;                 ///< 本档最小变动价位
});                                    ///< 104 字节

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
