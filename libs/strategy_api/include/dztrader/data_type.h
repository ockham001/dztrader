/**
 * @file data_type.h
 * @brief 基础宏、类型定义、枚举常量、帧类型
 *
 * 包含：
 *   - 导入导出宏（DZ_API）、C/C++ 兼容宏
 *   - 对齐宏（DZ_DECLARE_ALIGNED_STRUCT）
 *   - C 接口基础类型（typedef）
 *   - 枚举常量
 *   - 帧类型常量
 *   - 无效值常量
 */
#ifndef DZTRADER_DATA_TYPE_H_
#define DZTRADER_DATA_TYPE_H_

#include <float.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>

/* ==========================================================
 *  导入导出宏
 * ========================================================== */

/**
 * @brief 控制符号的导入导出
 */
#ifdef DZ_API_COMPILE_STATIC
#define DZ_API
#else
#ifdef _WIN32
#ifdef DZ_API_EXPORTS
#define DZ_API __declspec(dllexport)
#else
#define DZ_API __declspec(dllimport)
#endif
#else
#define DZ_API
#endif
#endif

/* ==========================================================
 *  C/C++ 兼容宏
 * ========================================================== */

/** @brief C++ 环境下展开为 extern "C" {，C 环境下为空 */
#ifdef __cplusplus
#define DZ_BEGIN_C_DECLS extern "C" {
#define DZ_END_C_DECLS   }
#else
#define DZ_BEGIN_C_DECLS
#define DZ_END_C_DECLS
#endif

/** @brief 编译期断言 */
#ifdef __cplusplus
#define DZ_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#else
#define DZ_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#endif

/** @brief 跨 C/C++ 的 sizeof 宏，C 侧需要 struct 关键字 */
#ifdef __cplusplus
#define DZ_SIZEOF(Type)        sizeof(Type)
#define DZ_SIZEOF_PACKED(Type) sizeof(__dz_internal_packed_##Type)
#else
#define DZ_SIZEOF(Type)        sizeof(struct Type)
#define DZ_SIZEOF_PACKED(Type) sizeof(struct __dz_internal_packed_##Type)
#endif

/* ==========================================================
 *  DZ_DECLARE_ALIGNED_STRUCT — 对齐结构体声明宏
 * ========================================================== */

#if defined(_MSC_VER)
#define DZ_PACK_TYPE_BEGIN __pragma(pack(push, 1))
#define DZ_PACK_TYPE_END   ;__pragma(pack(pop))
#elif defined(__GNUC__) || defined(__clang__)
#define DZ_PACK_TYPE_BEGIN
#define DZ_PACK_TYPE_END __attribute__((packed));
#else
#error "Unsupported compiler"
#endif

/**
 * @brief 声明 8 字节对齐且无 padding 的结构体
 *
 * 编译期自动检查：8 字节对齐、大小为 8 的倍数、无隐式 padding。
 * 所有写入共享内存的结构体必须使用此宏声明。
 *
 * @par 用法
 * @code
 * DZ_DECLARE_ALIGNED_STRUCT(DzTick, {
 *     DzInstrumentId instrument_id;
 *     double last_price;
 *     // ...
 * });
 * @endcode
 */
#define DZ_DECLARE_ALIGNED_STRUCT(Type, ...)                                                                           \
    struct alignas(8) Type __VA_ARGS__;                                                                                \
    static_assert(sizeof(Type) % 8 == 0, "Size of " #Type " must be multiple of 8");                                   \
    static_assert(alignof(Type) == 8, "Alignment of " #Type " must be 8");                                             \
    DZ_PACK_TYPE_BEGIN                                                                                                 \
    struct __dz_internal_packed_##Type __VA_ARGS__                                                                     \
    DZ_PACK_TYPE_END                                                                                                   \
    static_assert(alignof(__dz_internal_packed_##Type) == 1, "Alignment of __dz_internal_packed_" #Type " must be 1"); \
    static_assert(sizeof(Type) == sizeof(__dz_internal_packed_##Type), #Type " has padding bytes");

/* ==========================================================
 *  标量类型
 * ========================================================== */

/** @brief 订单 ID，平台生成。>= 0 有效，< 0 表示失败 */
typedef int64_t DzOrderId;

/** @brief 成交量、盘口量、下单数量 */
typedef int32_t DzVolume;

/** @brief 持仓量 */
typedef int64_t DzLargeVolume;

/** @brief 日期，距纪元天数 */
typedef int32_t DzDate;

/** @brief 时间，距午夜秒数 */
typedef int32_t DzTime;

/** @brief 微秒部分 (0-999999) */
typedef int32_t DzSubseconds;

/** @brief 定时器 ID（SDK 分配，用户凭 ID 取消；DZ_TIMER_INVALID 表示无效/失败） */
typedef uint64_t DzTimerId;

/** @brief 无效定时器 ID */
#define DZ_TIMER_INVALID ((DzTimerId)UINT64_MAX)

/* ==========================================================
 *  标识类型
 * ========================================================== */

/** @brief 合约代码 */
typedef char DzInstrumentId[88];

/** @brief 账户标识 */
typedef char DzAccountId[32];

/** @brief 策略 ID */
typedef char DzStrategyId[64];

/** @brief 交易所 ID */
typedef char DzExchangeId[16];

/** @brief 成交 ID */
typedef char DzTradeId[32];

/** @brief 订单备注 */
typedef char DzOrderRemark[64];

/* ==========================================================
 *  方向
 * ========================================================== */

/** @brief 买卖方向 */
typedef int8_t DzDirection;

/** @brief 空，卖 */
#define DZ_DIRECTION_SHORT ((DzDirection)(-1))
/** @brief 净 */
#define DZ_DIRECTION_NET   ((DzDirection)0)
/** @brief 多，买 */
#define DZ_DIRECTION_LONG  ((DzDirection)1)

/* ==========================================================
 *  开平仓
 * ========================================================== */

/** @brief 开平仓类型 */
typedef int8_t DzPositionEffect;

/** @brief 开仓 */
#define DZ_POSITION_EFFECT_OPEN          ((DzPositionEffect)1)
/** @brief 平仓 */
#define DZ_POSITION_EFFECT_CLOSE         ((DzPositionEffect)2)
/** @brief 平今 */
#define DZ_POSITION_EFFECT_CLOSE_TODAY   ((DzPositionEffect)3)
/** @brief 平昨 */
#define DZ_POSITION_EFFECT_CLOSE_YESTDAY ((DzPositionEffect)4)
/** @brief 自动拆分 (拆分优先级序列由账户级配置决定: 如 平今-平昨-开仓;
 *         零初始化即 AUTO, 首版仅定义不实现) */
#define DZ_POSITION_EFFECT_AUTO          ((DzPositionEffect)0)

/* ==========================================================
 *  委托单状态
 * ========================================================== */

/** @brief 委托单状态 */
typedef int8_t DzOrderStatus;

/** @brief 提交中 */
#define DZ_ORDER_SUBMITTING  ((DzOrderStatus)1)
/** @brief 未成交 */
#define DZ_ORDER_NOT_TRADED  ((DzOrderStatus)2)
/** @brief 部分成交 */
#define DZ_ORDER_PART_TRADED ((DzOrderStatus)3)
/** @brief 全部成交 */
#define DZ_ORDER_ALL_TRADED  ((DzOrderStatus)4)
/** @brief 已撤销 */
#define DZ_ORDER_CANCELLED   ((DzOrderStatus)5)
/** @brief 拒单 */
#define DZ_ORDER_REJECTED    ((DzOrderStatus)6)

/* ==========================================================
 *  委托单类型
 * ========================================================== */

/** @brief 委托单价格类型 */
typedef int8_t DzPriceType;

/** @brief 限价 */
#define DZ_PRICE_LIMIT  ((DzPriceType)0)
/** @brief 市价 */
#define DZ_PRICE_MARKET ((DzPriceType)1)
/** @brief STOP */
#define DZ_PRICE_STOP   ((DzPriceType)2)
/** @brief FAK（立即成交剩余撤销） */
#define DZ_PRICE_FAK    ((DzPriceType)3)
/** @brief FOK（全部成交否则撤销） */
#define DZ_PRICE_FOK    ((DzPriceType)4)
/** @brief 询价 */
#define DZ_PRICE_RFQ    ((DzPriceType)5)

/* ==========================================================
 *  期权类型
 * ========================================================== */

/** @brief 期权类型 */
typedef int8_t DzOptionType;

/** @brief 看跌 */
#define DZ_OPTION_PUT  ((DzOptionType)(-1))
/** @brief 看涨 */
#define DZ_OPTION_CALL ((DzOptionType)1)

/* ==========================================================
 *  合约产品类型
 * ========================================================== */

/** @brief 合约产品类型 (DzInstrumentInfo.product) */
typedef int8_t DzProduct;

#define DZ_PRODUCT_UNKNOWN   ((DzProduct)0)   ///< 未知
#define DZ_PRODUCT_FUTURES   ((DzProduct)1)   ///< 期货
#define DZ_PRODUCT_OPTION    ((DzProduct)2)   ///< 场内期权
#define DZ_PRODUCT_PERPETUAL ((DzProduct)3)   ///< 永续合约
#define DZ_PRODUCT_SPREAD    ((DzProduct)4)   ///< 交易所组合/价差 (腿见 DzInstrumentLeg)
#define DZ_PRODUCT_EQUITY    ((DzProduct)5)   ///< 股票
#define DZ_PRODUCT_ETF       ((DzProduct)6)   ///< ETF
#define DZ_PRODUCT_FUND      ((DzProduct)7)   ///< 其他上市基金 (LOF/REITs)
#define DZ_PRODUCT_BOND      ((DzProduct)8)   ///< 基础债券
#define DZ_PRODUCT_CB        ((DzProduct)9)   ///< 可转债 (T+0, 交易规则独立故单列)
#define DZ_PRODUCT_WARRANT   ((DzProduct)10)  ///< 权证/涡轮
#define DZ_PRODUCT_INDEX     ((DzProduct)11)  ///< 指数 (非交易参考行, 仅作衍生品标的)
#define DZ_PRODUCT_FOREX     ((DzProduct)12)  ///< 外汇对
#define DZ_PRODUCT_SPOT      ((DzProduct)13)  ///< 现货 (SGE 贵金属等)
#define DZ_PRODUCT_CFD       ((DzProduct)14)  ///< 差价合约

/** @brief 日期未提供 (epoch day 0 = 1970-01-01 元旦, 全球无开市, 与真实业务日无碰撞;
 *         与 POD 零初始化语义重合 — 未赋值即 NA, 安全缺省) */
#define DZ_DATE_NA  ((DzDate)0)

/* ==========================================================
 *  复权方式
 * ========================================================== */

/** @brief 复权方式 */
typedef int8_t DzAdjustType;

/** @brief 前复权 */
#define DZ_ADJUST_FORWARD  ((DzAdjustType)(-1))
/** @brief 不复权 */
#define DZ_ADJUST_NONE     ((DzAdjustType)0)
/** @brief 后复权 */
#define DZ_ADJUST_BACKWARD ((DzAdjustType)1)

/* ==========================================================
 *  查询结果列类型
 * ========================================================== */

/** @brief 查询结果列的数据类型 */
typedef int8_t DzColumnType;

/** @brief 无效值 */
#define DZ_COL_TYPE_NULL    ((DzColumnType)0)
/** @brief bool */
#define DZ_COL_TYPE_BOOL    ((DzColumnType)1)
/** @brief int64_t */
#define DZ_COL_TYPE_INT64   ((DzColumnType)2)
/** @brief double（IEEE 754 双精度） */
#define DZ_COL_TYPE_FLOAT64 ((DzColumnType)3)
/** @brief const char*（内部持有） */
#define DZ_COL_TYPE_STRING  ((DzColumnType)4)

/* ==========================================================
 *  UI 通知级别
 * ========================================================== */

/** @brief UI 通知消息级别 */
typedef int8_t DzNotifyLevel;

/** @brief 信息通知 */
#define DZ_NOTIFY_INFO  ((DzNotifyLevel)2)
/** @brief 警告通知 */
#define DZ_NOTIFY_WARN  ((DzNotifyLevel)3)
/** @brief 错误通知 */
#define DZ_NOTIFY_ERROR ((DzNotifyLevel)4)

/* ==========================================================
 *  账户登录状态
 * ========================================================== */

/** @brief 账户登录状态（三态，td 内部 11 态聚合映射，契约 account-status） */
typedef int8_t DzAccountState;

/** @brief 未登录（未启动/断开/登出/账户已移除） */
#define DZ_ACCOUNT_OFFLINE    ((DzAccountState)0)
/** @brief 登录进行中（连接/认证/登录/结算确认/合约加载全过程） */
#define DZ_ACCOUNT_LOGGING_IN ((DzAccountState)1)
/** @brief 就绪：登录+结算确认+合约加载完成，可下单可查询 */
#define DZ_ACCOUNT_READY      ((DzAccountState)2)

/* ==========================================================
 *  系统级定时任务类型
 *
 *  注: DzSysSchedType / DZ_SYS_SCHED_* / DZ_FRAME_SYS_SCHED 已随
 *  "系统调度域废弃"移除（2026-08 后无任何进程消费）。策略侧定时
 *  需求由 dz_schedule_* 定时器接口承担。
 * ========================================================== */

/* ==========================================================
 *  帧类型
 *
 *  【唯一书写位置】帧的值只写在两个头文件里: 本头（策略可见帧）与
 *  core_data_type.h（平台内部帧）; 契约、注释、日志、测试等其余任何位置
 *  一律引用帧名 DZ_FRAME_*, 不得写值 —— 改帧号只需改这两处。
 *
 *  帧号分段（新增帧取本段下一个空闲号、追加在段尾; 禁止插空档、
 *  禁止跨段散布同一接收域）:
 *
 *    | 值域      | 归属                                             |
 *    |-----------|--------------------------------------------------|
 *    | 0-31      | 系统/通道/日志/进程/自动登录/进度/关闭           |
 *    | 32-63     | 逻辑持仓/策略上行输出                            |
 *    | 64-95     | 行情数据与行情控制                               |
 *    | 1000-1023 | 行情源生命周期 + 交易推送                        |
 *    | 1024-1123 | 交易控制/状态/账户查询/出入金改密                |
 *    | 2000-2099 | 策略输入输出与本地调度                           |
 *
 *  段内稠密的意义: 同一接收域的帧号聚在连续区间, 接收方 switch (frame_type)
 *  用少量值簇即可生成跳转表, 逐帧成本不随段内帧数增长。
 *
 *  本头须可被 C 策略接口包含, 故只放宏与 typedef, 不放 C++ 断言。
 * ========================================================== */

/** @brief 共享内存帧类型 */
typedef int16_t DzFrameType;

/* ── 策略可见帧（策略经 dz_next_event / dz_next_md 识别消费） ── */

/** @brief 优雅关闭请求 (master→指定子进程, 定向, instance_id=目标进程名) */
#define DZ_FRAME_SHUTDOWN ((DzFrameType)24)

/** @brief Tick 行情推送 (dz_next_md 消费) */
#define DZ_FRAME_TICK ((DzFrameType)64)

/** @brief 委托回报推送 (payload=DzOrderReport, 契约 td-data-sync) */
#define DZ_FRAME_ORDER_REPORT ((DzFrameType)1002)

/** @brief 成交回报推送 (payload=DzTradeReport, 契约 td-data-sync) */
#define DZ_FRAME_TRADE_REPORT ((DzFrameType)1003)

/** @brief 持仓变化推送 (payload=DzPositionInfo, 契约 td-data-sync) */
#define DZ_FRAME_POSITION_INFO ((DzFrameType)1004)

/** @brief 账户资金推送 (payload=DzTradingAccount, 契约 td-data-sync) */
#define DZ_FRAME_TRADING_ACCOUNT ((DzFrameType)1005)

/** @brief 合约信息推送 (契约 instrument) */
#define DZ_FRAME_TD_INSTRUMENT ((DzFrameType)1006)

/** @brief 合约交易状态推送 (契约 instrument) */
#define DZ_FRAME_TD_INSTRUMENT_STATUS ((DzFrameType)1007)

/** @brief 保证金率镜像 (契约 td-fee-margin) */
#define DZ_FRAME_TD_MARGIN_RATE ((DzFrameType)1010)

/** @brief 手续费率镜像 (契约 td-fee-margin) */
#define DZ_FRAME_TD_COMMISSION_RATE ((DzFrameType)1011)

/** @brief 账户登录状态推送 (basic 广播帧, payload=DzAccountStatus) */
#define DZ_FRAME_ACCOUNT_STATUS ((DzFrameType)1012)

/** @brief 来自 UI 的输入投递给策略 (UI→策略, on_ui_input 回调) */
#define DZ_FRAME_UI_INPUT ((DzFrameType)2000)

/** @brief 策略调度触发 (dz_schedule_*; 仅 SDK 本地合成, 不写入共享内存) */
#define DZ_FRAME_SCHEDULE ((DzFrameType)2003)

#endif /* DZTRADER_DATA_TYPE_H_ */
