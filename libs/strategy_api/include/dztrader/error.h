/**
 * @file error.h
 * @brief 错误码常量
 *
 * 错误码为负数，0 表示成功。按模块分段：
 *   - 通用 (0 ~ -999)
 *   - 共享内存 (-1001 ~ -1999)
 *   - 策略接口 (-2002 ~ -2999)
 *   - 交易 (-3001 ~ -3999)
 *   - 行情 (-4001 ~ -4999)
 */
#ifndef DZTRADER_ERROR_H_
#define DZTRADER_ERROR_H_

#include "data_type.h"

/** @brief 错误码底层类型 */
typedef int32_t DzErrorCode;

/* ── 通用 ── */

/** @brief 成功 */
#define DZ_EC_OK ((DzErrorCode)0)
/** @brief 系统内部错误 */
#define DZ_EC_INTERNAL ((DzErrorCode)(-1))
/** @brief 无效参数 */
#define DZ_EC_INVALID_PARAM ((DzErrorCode)(-2))
/** @brief 空指针 */
#define DZ_EC_NULL_PTR ((DzErrorCode)(-3))
/** @brief 缓冲区太小 */
#define DZ_EC_BUFFER_TOO_SMALL ((DzErrorCode)(-4))
/** @brief 未找到 */
#define DZ_EC_NOT_FOUND ((DzErrorCode)(-5))
/** @brief 已存在 */
#define DZ_EC_ALREADY_EXISTS ((DzErrorCode)(-6))
/** @brief 权限不足 */
#define DZ_EC_PERMISSION_DENIED ((DzErrorCode)(-7))
/** @brief 超时 */
#define DZ_EC_TIMEOUT ((DzErrorCode)(-8))
/** @brief 系统错误 */
#define DZ_EC_SYSTEM ((DzErrorCode)(-9))

/* ── 共享内存 ── */

/** @brief 共享内存创建失败 */
#define DZ_EC_SHM_CREATE_FAILED ((DzErrorCode)(-1001))
/** @brief 共享内存打开失败 */
#define DZ_EC_SHM_OPEN_FAILED ((DzErrorCode)(-1002))
/** @brief 共享内存映射失败 */
#define DZ_EC_SHM_MAP_FAILED ((DzErrorCode)(-1003))
/** @brief 共享内存解映射失败 */
#define DZ_EC_SHM_UNMAP_FAILED ((DzErrorCode)(-1004))
/** @brief 共享内存文件未找到 */
#define DZ_EC_SHM_FILE_NOT_FOUND ((DzErrorCode)(-1005))
/** @brief 共享内存文件太小 */
#define DZ_EC_SHM_FILE_TOO_SMALL ((DzErrorCode)(-1006))
/** @brief 共享内存对齐错误 */
#define DZ_EC_SHM_ALIGN_ERROR ((DzErrorCode)(-1007))
/** @brief 共享内存写入溢出 */
#define DZ_EC_SHM_WRITE_OVERFLOW ((DzErrorCode)(-1008))
/** @brief 共享内存读取无效 */
#define DZ_EC_SHM_READ_INVALID ((DzErrorCode)(-1009))
/** @brief 共享内存通道已关闭 */
#define DZ_EC_SHM_CHANNEL_CLOSED ((DzErrorCode)(-1010))
/** @brief 共享内存通道已满 */
#define DZ_EC_SHM_CHANNEL_FULL ((DzErrorCode)(-1011))
/** @brief 共享内存锁获取失败 */
#define DZ_EC_SHM_LOCK_FAILED ((DzErrorCode)(-1012))
/** @brief 共享内存锁超时 */
#define DZ_EC_SHM_LOCK_TIMEOUT ((DzErrorCode)(-1013))
/** @brief 共享内存锁被废弃（前持有进程崩溃） */
#define DZ_EC_SHM_LOCK_ABANDONED ((DzErrorCode)(-1014))
/** @brief 信号量创建失败 */
#define DZ_EC_SHM_SEM_CREATE_FAILED ((DzErrorCode)(-1015))
/** @brief 信号量等待失败 */
#define DZ_EC_SHM_SEM_WAIT_FAILED ((DzErrorCode)(-1016))
/** @brief 信号量通知失败 */
#define DZ_EC_SHM_SEM_POST_FAILED ((DzErrorCode)(-1017))
/** @brief 帧无效 */
#define DZ_EC_SHM_FRAME_INVALID ((DzErrorCode)(-1018))
/** @brief 帧大小不匹配 */
#define DZ_EC_SHM_FRAME_SIZE_MISMATCH ((DzErrorCode)(-1019))
/** @brief 帧版本不匹配 */
#define DZ_EC_SHM_VERSION_MISMATCH ((DzErrorCode)(-1020))
/** @brief 共享内存初始化失败 */
#define DZ_EC_SHM_INIT_FAILED ((DzErrorCode)(-1021))
/** @brief 共享内存写入失败 */
#define DZ_EC_SHM_WRITE_FAILED ((DzErrorCode)(-1022))
/** @brief 共享内存空间不足 */
#define DZ_EC_SHM_NO_SPACE ((DzErrorCode)(-1023))
/** @brief 订阅者操作失败 */
#define DZ_EC_SHM_SUBSCRIBER_FAILED ((DzErrorCode)(-1024))
/** @brief 进程列表已满 */
#define DZ_EC_SHM_PROCESS_LIST_FULL ((DzErrorCode)(-1025))
/** @brief 信号量打开失败 */
#define DZ_EC_SHM_SEM_OPEN_FAILED ((DzErrorCode)(-1026))
/** @brief page 文件删除失败 */
#define DZ_EC_SHM_FILE_REMOVE_FAILED ((DzErrorCode)(-1027))
/** @brief 订单ID访问失败（非 creator 角色） */
#define DZ_EC_SHM_ORDER_ID_ACCESS_FAILED ((DzErrorCode)(-1028))

/* ── 策略接口 ── */

/** @brief 策略已初始化 */
#define DZ_EC_STRATEGY_ALREADY_INITIALIZED ((DzErrorCode)(-2002))
/** @brief 策略初始化失败 */
#define DZ_EC_STRATEGY_INIT_FAILED ((DzErrorCode)(-2003))
/** @brief 策略释放失败 */
#define DZ_EC_STRATEGY_RELEASE_FAILED ((DzErrorCode)(-2004))
/** @brief 策略自通知失败 */
#define DZ_EC_STRATEGY_NOTIFY_FAILED ((DzErrorCode)(-2005))
/** @brief 策略目录未找到 */
#define DZ_EC_STRATEGY_HOME_NOT_FOUND ((DzErrorCode)(-2006))

/* ── 交易 ── */

/** @brief 下单失败 */
#define DZ_EC_TRADE_INSERT_FAILED ((DzErrorCode)(-3001))
/** @brief 撤单失败 */
#define DZ_EC_TRADE_CANCEL_FAILED ((DzErrorCode)(-3002))
/** @brief 查询失败 */
#define DZ_EC_TRADE_QUERY_FAILED ((DzErrorCode)(-3003))
/** @brief 委托被拒 */
#define DZ_EC_TRADE_ORDER_REJECTED ((DzErrorCode)(-3004))
/** @brief 委托单未找到 */
#define DZ_EC_TRADE_ORDER_NOT_FOUND ((DzErrorCode)(-3005))
/** @brief 交易账户未找到 */
#define DZ_EC_TRADE_ACCOUNT_NOT_FOUND ((DzErrorCode)(-3006))
/** @brief 交易账户已禁用 */
#define DZ_EC_TRADE_ACCOUNT_DISABLED ((DzErrorCode)(-3007))
/** @brief 保证金不足 */
#define DZ_EC_TRADE_INSUFFICIENT_MARGIN ((DzErrorCode)(-3008))
/** @brief 数量不足 */
#define DZ_EC_TRADE_INSUFFICIENT_VOLUME ((DzErrorCode)(-3009))
/** @brief 价格无效 */
#define DZ_EC_TRADE_PRICE_INVALID ((DzErrorCode)(-3010))
/** @brief 数量无效 */
#define DZ_EC_TRADE_VOLUME_INVALID ((DzErrorCode)(-3011))
/** @brief 持仓无效 */
#define DZ_EC_TRADE_POSITION_INVALID ((DzErrorCode)(-3012))
/** @brief 交易网关未连接 */
#define DZ_EC_TRADE_GATEWAY_NOT_CONNECTED ((DzErrorCode)(-3013))
/** @brief 交易网关错误 */
#define DZ_EC_TRADE_GATEWAY_ERROR ((DzErrorCode)(-3014))
/** @brief 风控限制 */
#define DZ_EC_TRADE_RISK_LIMIT ((DzErrorCode)(-3015))

/* ── 行情 ── */

/** @brief 订阅失败 */
#define DZ_EC_MD_SUBSCRIBE_FAILED ((DzErrorCode)(-4001))
/** @brief 退订失败 */
#define DZ_EC_MD_UNSUBSCRIBE_FAILED ((DzErrorCode)(-4002))
/** @brief 查询失败 */
#define DZ_EC_MD_QUERY_FAILED ((DzErrorCode)(-4003))
/** @brief 合约未找到 */
#define DZ_EC_MD_INSTRUMENT_NOT_FOUND ((DzErrorCode)(-4004))
/** @brief 行情网关未连接 */
#define DZ_EC_MD_GATEWAY_NOT_CONNECTED ((DzErrorCode)(-4005))
/** @brief 行情网关错误 */
#define DZ_EC_MD_GATEWAY_ERROR ((DzErrorCode)(-4006))
/** @brief 无数据 */
#define DZ_EC_MD_NO_DATA ((DzErrorCode)(-4007))

/* ── Master ── */

/** @brief Master 实例锁失败 */
#define DZ_EC_MASTER_LOCK_FAILED ((DzErrorCode)(-5001))
/** @brief Master 实例已在运行 */
#define DZ_EC_MASTER_ALREADY_RUNNING ((DzErrorCode)(-5002))
/** @brief Master 数据库操作失败 */
#define DZ_EC_MASTER_DB_FAILED ((DzErrorCode)(-5003))

#endif /* DZTRADER_ERROR_H_ */