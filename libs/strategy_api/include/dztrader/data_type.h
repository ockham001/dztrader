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
 *  系统级定时任务类型
 * ========================================================== */

/** @brief 系统级定时任务类型 */
typedef int8_t DzSysSchedType;

/** @brief 交易账户登录 */
#define DZ_SYS_SCHED_TD_LOGIN           ((DzSysSchedType)1)
/** @brief 行情源登录 */
#define DZ_SYS_SCHED_MD_LOGIN           ((DzSysSchedType)2)
/** @brief 开盘前 */
#define DZ_SYS_SCHED_PRE_OPENING        ((DzSysSchedType)3)
/** @brief 集合竞价 */
#define DZ_SYS_SCHED_CALL_AUCTION       ((DzSysSchedType)4)
/** @brief 收盘前 */
#define DZ_SYS_SCHED_PRE_CLOSING        ((DzSysSchedType)5)
/** @brief 行情源登出 */
#define DZ_SYS_SCHED_MD_LOGOUT          ((DzSysSchedType)6)
/** @brief 交易账户登出 */
#define DZ_SYS_SCHED_TD_LOGOUT          ((DzSysSchedType)7)
/** @brief 交易日切换 */
#define DZ_SYS_SCHED_TRADING_DAY_SWITCH ((DzSysSchedType)8)
/** @brief 盘后数据整理 */
#define DZ_SYS_SCHED_POST_MARKET        ((DzSysSchedType)9)
/** @brief 盘后策略计算 */
#define DZ_SYS_SCHED_POST_STRATEGY      ((DzSysSchedType)10)

/* ==========================================================
 *  帧类型
 * ========================================================== */

/** @brief 共享内存帧类型 */
typedef int16_t DzFrameType;

/* ── 系统帧 ── */

/** @brief 无效填充帧（边界填充） */
#define DZ_FRAME_INVALID_FILL       ((DzFrameType)0)
/** @brief 系统广播时间信号 */
#define DZ_FRAME_SYS_SCHED          ((DzFrameType)10)
/** @brief 事件通道预加载通知 (master->所有子进程, 无 instance_id) */
#define DZ_FRAME_PRELOAD_EVENT_SHM  ((DzFrameType)11)
/** @brief 行情数据通道预加载通知 (instance_id=行情源名, 如 "dzmd_ctp") */
#define DZ_FRAME_PRELOAD_MD_SHM     ((DzFrameType)17)
/** @brief 优雅关闭请求 (master->指定子进程, 定向, instance_id=目标进程名) */
#define DZ_FRAME_REQUEST_SHUTDOWN   ((DzFrameType)12)
/** @brief master→行情进程, 通知刷新 md 通道订阅者列表 (定向, instance_id=进程名) */
#define DZ_FRAME_UPDATE_SHM_MD_SUBSCRIBER  ((DzFrameType)13)
/** @brief 设置目标进程日志配置 (dzweb→目标进程, 定向, instance_id=目标进程名, 契约 01-log) */
#define DZ_FRAME_SET_LOG_CONFIG     ((DzFrameType)14)
/** @brief 触发目标进程日志 flush (dzweb→目标进程, 定向, instance_id=目标进程名, 契约 01-log) */
#define DZ_FRAME_FLUSH_LOG         ((DzFrameType)15)
/** @brief 广播停止请求 (所有进程执行, 无需匹配 instance_id) */
#define DZ_FRAME_REQUEST_SHUTDOWN_ALL   ((DzFrameType)20)
/** @brief 广播刷新 event 通道订阅者列表 (所有进程执行, 无需匹配 instance_id) */
#define DZ_FRAME_UPDATE_SHM_EVENT_SUBSCRIBER  ((DzFrameType)21)
/** @brief 设置事件通道配置 (UI->master, payload=ShmChannelConfig) */
#define DZ_FRAME_SET_EVENT_SHM_CONFIG      ((DzFrameType)22)
/** @brief 推送事件通道配置 (master->UI, payload=ShmChannelConfig) */
#define DZ_FRAME_RTN_EVENT_SHM_CONFIG      ((DzFrameType)23)
/** @brief 设置行情通道配置 (UI->md, payload=ShmChannelConfig) */
#define DZ_FRAME_SET_MD_SHM_CONFIG         ((DzFrameType)24)
/** @brief 推送行情通道配置 (md->UI, payload=ShmChannelConfig) */
#define DZ_FRAME_RTN_MD_SHM_CONFIG         ((DzFrameType)25)
/** @brief 进程上报当前日志配置（各进程→dzweb，经事件通道广播，契约 00） */
#define DZ_FRAME_RTN_LOG_CONFIG  ((DzFrameType)16)

/* ── 行情帧 ── */

/** @brief Tick 行情推送 */
#define DZ_FRAME_RTN_MD_TICK            ((DzFrameType)1000)

/* ── 交易帧 ── */

/** @brief 委托回报推送 */
#define DZ_FRAME_TD_ORDER_RPT       ((DzFrameType)2000)
/** @brief 成交回报推送 */
#define DZ_FRAME_TD_TRADE_RPT       ((DzFrameType)2001)
/** @brief 持仓变化推送 */
#define DZ_FRAME_TD_POSITION_INFO   ((DzFrameType)2002)
/** @brief 账户资金推送 */
#define DZ_FRAME_TD_TRADING_ACCOUNT ((DzFrameType)2003)
/** @brief 交易网关状态变化 */
#define DZ_FRAME_TD_GATEWAY_STATUS  ((DzFrameType)2004)

/* ── 策略帧 ── */

/** @brief 用户输入传递给策略（UI→策略） */
#define DZ_FRAME_STG_USER_INPUT     ((DzFrameType)3001)
/** @brief 策略→UI, 用户输入的响应(未来用, 本次只占编号) */
#define DZ_FRAME_STG_USER_OUTPUT     ((DzFrameType)3002)

#endif /* DZTRADER_DATA_TYPE_H_ */