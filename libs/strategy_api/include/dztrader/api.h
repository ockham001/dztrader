/**
 * @file api.h
 * @brief dztrader 策略接口 — 纯 C 完整接口（ABI 兼容层）
 *
 * 包含所有 C 函数声明，通过 include 子文件获取类型和结构体定义：
 *   - version.h    — 版本号宏（CMake 生成）
 *   - data_type.h  — 宏、对齐、typedef、枚举、帧类型
 *   - error.h      — 错误码常量
 *   - struct.h     — 共享内存结构体、帧格式结构体
 *
 * 策略开发者通常通过 <dztrader.h> 获取完整接口。
 *
 * 字符串参数 NULL/"" 约定：
 *   身份标识类字符串参数（如 account_id、instrument_id），NULL 与空字符串 "" 语义相同。
 *   读侧（查询）：NULL/"" 表示不限定（通配）。
 *   写侧（设置）：NULL/"" 表示通配/清空所有。
 *   共享内存中统一用空字符串表示。
 */
#ifndef DZTRADER_API_H_
#define DZTRADER_API_H_

#include "data_type.h"

/* ==========================================================
 *  C 函数声明
 * ========================================================== */

DZ_BEGIN_C_DECLS

/** @brief 策略运行环境句柄（不透明指针） */
typedef struct DzContext DzContext;

/** @brief 结果集句柄 */
typedef struct DzResultSet DzResultSet;

/* ── 生命周期 ── */

/**
 * @brief 初始化运行环境
 *
 * 必须在调用其他任何函数之前调用。
 * 构造运行环境（事件通道读写器/信号量/订单ID元数据）。惰性幂等已废除：
 * 重复调用返回 NULL（调 dz_errcode() 获取错误码，为
 * DZ_EC_STRATEGY_ALREADY_INITIALIZED）；平台未启动（事件通道不可用）时
 * 返回 NULL，策略进程可安全报错退出而非崩溃。
 *
 * @return 非 NULL 为句柄，NULL 为失败（调 dz_errcode() 获取错误码）
 */
DZ_API DzContext* dz_init(void);

/**
 * @brief 释放策略运行环境
 *
 * 释放后不可再使用旧句柄；可重新调用 dz_init() 获得全新会话。
 * NULL 句柄为 no-op。
 *
 * @param ctx  句柄，可为 NULL
 */
DZ_API void dz_release(DzContext* ctx);

/* ── 句柄契约 ──
 * ctx 必须来自 dz_init() 返回值。传入 NULL 或野指针为未定义行为，SDK 不做检查。
 * 悬垂句柄（release 后使用）、重复 release、非本进程句柄均属用户错误，不设防。
 * dz_release 恰好调用一次，且只能传当前会话句柄（dz_init 最近一次成功返回的指针）；
 * 对非当前句柄调用 dz_release 属未定义行为。
 * 生命周期由单线程保证，SDK 不做线程同步（dz_init/dz_release 非线程安全）。
 * release 后重新 dz_init() 获得全新会话；事件信号量为同名复用（不重置计数），
 * 若 release 前有未消费的通知，新会话首次 dz_wait 可能立即返回（dz_next_event 排空自愈）。 */

DZ_API const char* dz_md_source_name(DzContext* ctx);

/**
 * @brief 阻塞等待，直到有新数据可读或最近一个定时器到期
 *
 * 返回后通过 dz_next_md() / dz_next_event() 逐帧读取数据。
 * 高频模式下可不调用此函数，直接轮询 dz_next_md() / dz_next_event()。
 *
 * 等待行为（纯等待，不做定时器触发）：
 *   - 无待触发定时器：无限阻塞，仅被信号量 notify 唤醒（CPU=0）
 *   - 有待触发定时器：等待至最近到期时间返回；已到期则立即返回（免系统调用）
 *
 * 定时器推进点唯一在 dz_next_event()：到期定时器由 dz_next_event 触发
 * （通道无用户帧时）并经其返回。契约要求策略调用 dz_next_event 消费
 * 事件流——定时器是事件流的一部分，不调用则不触发（不设防）。
 */
DZ_API void dz_wait(DzContext* ctx);

/**
 * @brief 取下一帧事件数据
 *
 * 非阻塞，推进游标返回下一帧指针。无新帧则返回 NULL。
 * 返回的指针指向 DzFrameHeader，按 frame_type 解析 payload。
 *
 * 处理优先级：用户帧 > 定时器帧 > 内部帧。
 *   - 用户帧（本策略 TD 回报、定向本策略的 STG_USER_INPUT、SHUTDOWN）直接返回，零计时器开销；
 *     TD_ORDER_RPT/TD_TRADE_RPT 按 payload strategy_id == 本策略裸名定向（空/他策略拦截），
 *     其余 TD 回报（持仓/资金/费率等）暂不过滤全量放行，STG_USER_INPUT/SHUTDOWN 按 instance_id 定向；
 *   - 通道无用户帧时（通道空或 32 上限让位）触发到期定时器并返回定时器帧；
 *   - 平台内部帧（SHM 预加载通知、订阅者刷新、日志/SHM 配置等）由 SDK
 *     内部消费，不返回给策略；其中 PRELOAD_EVENT_SHM / PRELOAD_MD_SHM
 *     触发 SDK 内部随机延迟预加载，NOTIFY_MD_STARTED 触发自动补订阅。
 *   - REQUEST_SHUTDOWN（定向本策略）由 SDK 完成内部清理后放行。
 *   - 每次调用最多连续消费 32 条内部帧，超过则本次让位（优先返回已到期
 *     定时器帧，否则 NULL，下次调用继续处理），防止内部帧洪峰饿死 dz_next_md。
 *
 * 定时器帧（DZ_FRAME_STG_SCHEDULE，dz_schedule_* 触发）为 SDK 本地合成，
 * 指针仅在下次 dz_next_event / dz_release 调用前有效，需要留存请自行拷贝。
 *
 * @return 帧指针，无数据返回 NULL
 */
DZ_API const void* dz_next_event(DzContext* ctx);

/**
 * @brief 取下一帧行情数据
 *
 * 非阻塞，推进游标返回下一帧指针。无新帧则返回 NULL。
 * 返回的指针指向 DzFrameHeader，按 frame_type 解析 payload。
 *
 * @param ctx  策略运行环境
 * @return 帧指针，无数据返回 NULL
 */
DZ_API const void* dz_next_md(DzContext* ctx);

/**
 * @brief 自通知：唤醒自身信号量
 *
 * 策略被唤醒后自行检查共享内存通道是否有新数据。
 *
 * 线程安全：可在任意线程调用（底层 sem_post/ReleaseSemaphore 线程安全），
 * 前提是 ctx 在调用期间存活（子线程 join 需先于 dz_release）。
 * 典型用法：后台线程（定时器/数据线程）唤醒主事件循环。
 *
 */
DZ_API void dz_notify_self(DzContext* ctx);

/* ── 定时器 ──
 *
 * 单线程契约：定时器推进点唯一在 dz_next_event()（通道无用户帧时 tick），
 * dz_wait() 仅负责等待（无定时器无限阻塞 / 有定时器等待至最近到期）。
 * dz_schedule_* 必须在主循环线程调用（与句柄契约一致）。触发事件以
 * DZ_FRAME_STG_SCHEDULE 帧经 dz_next_event() 返回，不写入共享内存。
 * SDK 内部任务（SHM 预加载随机延迟）与用户定时器共用队列，但内部 ID 对用户
 * 不可见，dz_schedule_cancel / dz_schedule_cancel_all 结构上无法误删内部定时器。 */

/**
 * @brief 一次性延时定时器
 *
 * @param ctx      策略运行环境
 * @param delay_ms 延迟毫秒数，必须 > 0
 * @return 定时器 ID（>=0）；失败返回 DZ_TIMER_INVALID（调 dz_errcode 获取错误码）
 */
DZ_API DzTimerId dz_schedule_after(DzContext* ctx, int32_t delay_ms);

/**
 * @brief 周期定时器
 *
 * 每 delay_ms 触发一次，不取消则一直触发。按「上次到期点 + interval」重排不漂移；
 * 策略停滞期间错过的整周期不再补触发。
 *
 * @param ctx      策略运行环境
 * @param delay_ms 触发间隔毫秒数，必须 > 0
 * @return 定时器 ID；失败返回 DZ_TIMER_INVALID
 */
DZ_API DzTimerId dz_schedule_every(DzContext* ctx, int32_t delay_ms);

/**
 * @brief 一次性定点定时器（wall clock）
 *
 * 在下一个本地时间点触发一次。time_of_day_ms 为距午夜毫秒数，
 * 如 14:55:00 → 53,700,000。已过的时间点次日触发。
 *
 * @param ctx            策略运行环境
 * @param time_of_day_ms 距午夜毫秒数，[0, 86_399_999]
 * @return 定时器 ID；失败返回 DZ_TIMER_INVALID
 */
DZ_API DzTimerId dz_schedule_at(DzContext* ctx, int32_t time_of_day_ms);

/**
 * @brief 每日定点定时器（wall clock）
 *
 * 每天该时间点触发，不取消则一直触发；每次触发后重算次日时间点，
 * 自动适应时区/夏令时变化。
 *
 * @param ctx            策略运行环境
 * @param time_of_day_ms 距午夜毫秒数，[0, 86_399_999]
 * @return 定时器 ID；失败返回 DZ_TIMER_INVALID
 */
DZ_API DzTimerId dz_schedule_daily(DzContext* ctx, int32_t time_of_day_ms);

/**
 * @brief 取消指定定时器
 *
 * @param ctx      策略运行环境
 * @param timer_id 定时器 ID（dz_schedule_* 返回值）
 * @return true 已取消；false 定时器不存在（无效/已触发/内部定时器 ID），
 *         调 dz_errcode 获取错误码（DZ_EC_TIMER_NOT_FOUND）
 */
DZ_API bool dz_schedule_cancel(DzContext* ctx, DzTimerId timer_id);

/**
 * @brief 取消全部用户定时器
 *
 * 仅清用户定时器（含已合成未领取的定时器帧）；SDK 内部定时器不受影响。
 *
 * @param ctx 策略运行环境
 * @return true 成功
 */
DZ_API bool dz_schedule_cancel_all(DzContext* ctx);

/**
 * @brief 获取策略可执行文件所在目录路径
 *
 * 路径以 / 或 \ 结尾。
 *
 * @return 目录路径字符串
 */
DZ_API const char* dz_strategy_home(DzContext* ctx);

/**
 * @brief 获取策略ID
 *
 * 策略ID全局唯一，用于 IPC 命名、配置、日志、展示。
 *
 * @return 策略ID字符串
 */
DZ_API const char* dz_strategy_id(DzContext* ctx);

/* ── 交易接口 ── */

/**
 * @brief 下单
 *
 * 订单 ID 由平台生成，通过 DzOrderReport 推送返回。
 *
 * @param account_id       账户标识
 * @param instrument_id    合约代码
 * @param direction        买卖方向（DZ_DIRECTION_LONG / DZ_DIRECTION_SHORT）
 * @param price_type       价格类型（DZ_PRICE_LIMIT / DZ_PRICE_MARKET 等）
 * @param price            限价/触发价（市价单传 0）
 * @param volume           委托量
 * @param position_effect  开平仓
 * @return >= 0 为订单 ID，< 0 表示失败（调 dz_errcode() 获取错误码）
 */
DZ_API DzOrderId dz_place_order(DzContext* ctx,
                                const char* account_id,
                                const char* instrument_id,
                                DzDirection direction,
                                DzPriceType price_type,
                                double price,
                                DzVolume volume,
                                DzPositionEffect position_effect);

/**
 * @brief 撤单
 *
 * @param account_id  账户标识（与下单时一致，网关按此路由撤单帧）
 * @param order_id    要撤销的订单 ID
 * @return true 成功，false 失败（调 dz_errcode() 获取错误码）
 */
DZ_API bool dz_cancel_order(DzContext* ctx, const char* account_id, DzOrderId order_id);

/**
 * @brief 订阅行情合约
 *
 * @param ctx               策略运行环境
 * @param instruments       合约代码数组，NULL 或 count=0 为无效调用
 * @param count             合约数量
 * @param replace_previous  true=先取消该策略在当前行情源的所有旧订阅，再订阅新合约
 * @return true 请求已写入事件通道且 SDK 期望集合已同步；false 失败（调 dz_errcode() 获取错误码）
 */
DZ_API bool dz_subscribe(DzContext* ctx,
                         const char* const instruments[],
                         uint32_t count,
                         bool replace_previous);

/**
 * @brief 取消订阅行情合约
 *
 * instruments 为 NULL 或 count 为 0 时，取消该策略在当前行情源的所有订阅。
 *
 * @param ctx          策略运行环境
 * @param instruments  合约代码数组，NULL 或 count=0 表示取消所有订阅
 * @param count        合约数量
 * @return true 请求已写入事件通道且 SDK 期望集合已同步；false 失败（调 dz_errcode() 获取错误码）
 */
DZ_API bool dz_unsubscribe(DzContext* ctx,
                           const char* const instruments[],
                           uint32_t count);

/**
 * @brief 设置策略逻辑持仓
 *
 * 逻辑持仓 = 策略认为应该持有的仓位，用于 UI 监控策略持仓与账户实际持仓的偏差。
 *
 * @param account_id     账户标识，NULL/"" 表示所有账户该策略的逻辑持仓设为 0（net_volume
 * 忽略，固定为 0）
 * @param instrument_id  合约代码，account_id 有效时 NULL/"" 表示该账户该策略的所有品种逻辑持仓设为
 * 0（net_volume 忽略，固定为 0）； account_id 为 NULL/"" 时此参数忽略
 * @param net_volume     净手数（正=多，负=空，0=无持仓）
 * @return true 成功，false 失败（调 dz_errcode() 获取错误码）
 */
DZ_API bool dz_set_logical_position(DzContext* ctx,
                                    const char* account_id,
                                    const char* instrument_id,
                                    int32_t net_volume);

/* ── UI 通知 ── */

/**
 * @brief 向 UI 消息中心投递通知消息
 *
 * 策略名自动关联（取自 ctx），无需传参。
 *
 * @param level    消息级别（DZ_NOTIFY_INFO / DZ_NOTIFY_WARN / DZ_NOTIFY_ERROR）
 * @param message  消息内容（UTF-8，策略自行组织）
 * @param popup    是否弹窗
 * @return true 成功，false 失败（调 dz_errcode() 获取错误码）
 */
DZ_API bool dz_notify_ui(DzContext* ctx, DzNotifyLevel level, const char* message, bool popup);

/**
 * @brief 向 UI 发送输出数据
 *
 * 与 dz_notify_ui 的区别：
 *   dz_notify_ui  — 通知消息，有级别和弹窗，类似日志/告警（主动推送）
 *   dz_output_ui  — 自主输出，无级别无弹窗，类似 stdout
 *
 * 典型场景：策略主动向 UI 输出运行状态/自定义数据；也可作为对 UI 输入
 * （STG_USER_INPUT）的响应。
 *
 * 策略名自动关联（取自 ctx），无需传参。
 *
 * @param data  输出数据（UTF-8 文本或 JSON，策略自行组织格式）
 * 数据超过单页可写上限时按字节截断；上限 = min(1MB, 通道页大小 - 帧头开销)，
 * 由 SDK 自动计算。通道页过小无法容纳任何 payload 时返回 false。
 * @return true 成功，false 失败（调 dz_errcode() 获取错误码）
 */
DZ_API bool dz_output_ui(DzContext* ctx, const char* data);

/* ── 数据库接口 ── */

/** @brief 不透明数据库句柄 */
typedef struct DzDatabase DzDatabase;

/**
 * @brief 打开数据库
 *
 * @param path  数据库文件路径
 * @return 非 NULL 为句柄，NULL 为失败（调 dz_errcode() 获取错误码）
 */
DZ_API DzDatabase* dz_db_open(const char* path);

/**
 * @brief 关闭数据库
 *
 * @param db  数据库句柄
 * @return true 成功，false 失败（调 dz_errcode() 获取错误码）
 */
DZ_API bool dz_db_close(DzDatabase* db);

/* ==========================================================
 *  DzResultSet — 查询结果集（不透明结构体 + 独立函数）
 *
 *  统一的二维表格结果集，逐行遍历、按列索引取值。
 *  适用于数据库查询和实时数据查询（行情快照、持仓等）。
 *  策略负责调用 dz_resultset_close() 释放。
 *
 *  使用模式：
 *  @code
 *    DzResultSet* rs = dz_db_query_order(db, NULL, NULL);
 *    if (!rs || dz_resultset_status(rs) != 0) { // 错误处理 }
 *    while (dz_resultset_next(rs)) {
 *        int64_t order_id = dz_resultset_get_int64(rs, 0);
 *        double price     = dz_resultset_get_float64(rs, 1);
 *    }
 *    dz_resultset_close(rs);
 *  @endcode
 * ========================================================== */

/* ── 状态与遍历 ── */

/**
 * @brief 移到下一行记录
 *
 * 首次调用前游标位于第一行之前，首次调用移到第一行。
 *
 * @param rs  结果集句柄
 * @return true 当前有数据行，false 已到末尾
 */
DZ_API bool dz_resultset_next(DzResultSet* rs);

/* ── 列元信息 ── */

/**
 * @brief 获取列类型
 *
 * @param rs     结果集句柄
 * @param index  列索引
 * @return DzColumnType 枚举值，若该列为无效值或不存在则返回 DZ_COL_TYPE_NULL
 */
DZ_API DzColumnType dz_resultset_column_type(DzResultSet* rs, uint32_t index);

/* ── 当前行取值 ── */

/**
 * @brief 当前行指定列是否为 NULL
 *
 * @param rs     结果集句柄
 * @param index  列索引
 * @return true 值为 NULL/缺失，false 值有效
 */
DZ_API bool dz_resultset_is_null(DzResultSet* rs, uint32_t index);

/**
 * @brief 获取 int64 值
 *
 * @param rs     结果集句柄
 * @param index  列索引（0-based）
 * @return int64_t 值，类型不匹配或 NULL 则返回 INT64_MAX
 */
DZ_API int64_t dz_resultset_get_int64(DzResultSet* rs, uint32_t index);

/**
 * @brief 获取 float64 值
 *
 * @param rs     结果集句柄
 * @param index  列索引
 * @return double 值，类型不匹配或 NULL 则返回 DBL_MAX
 */
DZ_API double dz_resultset_get_float64(DzResultSet* rs, uint32_t index);

/**
 * @brief 获取字符串值
 *
 * @param rs     结果集句柄
 * @param index  列索引
 * @return 字符串指针，类型不匹配或 NULL 则返回 ""
 */
DZ_API const char* dz_resultset_get_string(DzResultSet* rs, uint32_t index);

/**
 * @brief 获取 bool 值
 *
 * @note 无法区分"值为 false"和"值为 NULL"，建议先调 dz_resultset_is_null 判断。
 *
 * @param rs     结果集句柄
 * @param index  列索引
 * @return bool 值，类型不匹配或 NULL 则返回 false
 */
DZ_API bool dz_resultset_get_bool(DzResultSet* rs, uint32_t index);

/* ── 生命周期 ── */

/**
 * @brief 关闭结果集，释放相关资源
 *
 * 关闭后不可再使用该句柄。NULL 安全。
 *
 * @param rs  结果集句柄，可为 NULL
 */
DZ_API void dz_resultset_close(DzResultSet* rs);

/**
 * @brief 获取结果集状态
 *
 * 0 表示成功，非 0 为错误码（与 DZ_EC_* 对齐）。
 *
 * @param rs  结果集句柄
 * @return 状态码，rs 为 NULL 返回 DZ_EC_INTERNAL
 */
DZ_API int32_t dz_resultset_status(DzResultSet* rs);

/**
 * @brief 获取列数
 *
 * @param rs  结果集句柄
 * @return 列数，rs 为 NULL 返回 0
 */
DZ_API uint32_t dz_resultset_column_count(DzResultSet* rs);

/**
 * @brief 获取列名
 *
 * 返回指针指向内部字符串，生命周期同 DzResultSet。
 *
 * @param rs     结果集句柄
 * @param index  列索引
 * @return 列名，index 越界或 rs 为 NULL 返回 ""
 */
DZ_API const char* dz_resultset_column_name(DzResultSet* rs, uint32_t index);

/* ── 查询接口（返回 DzResultSet） ── */

/**
 * @brief 查询委托
 *
 * @param db             数据库句柄
 * @param account_id     账户标识，NULL 表示不限定
 * @param instrument_id  合约代码，NULL 表示不限定
 * @return 非 NULL 为结果集（需 dz_resultset_close），NULL 为失败（调 dz_errcode() 获取错误码）
 */
DZ_API DzResultSet* dz_db_query_order(DzDatabase* db,
                                      const char* account_id,
                                      const char* instrument_id);

/**
 * @brief 查询成交
 *
 * @param db             数据库句柄
 * @param account_id     账户标识，NULL 表示不限定
 * @param instrument_id  合约代码，NULL 表示不限定
 * @return 非 NULL 为结果集（需 dz_resultset_close），NULL 为失败（调 dz_errcode() 获取错误码）
 */
DZ_API DzResultSet* dz_db_query_trade(DzDatabase* db,
                                      const char* account_id,
                                      const char* instrument_id);

/**
 * @brief 查询持仓
 *
 * @param db             数据库句柄
 * @param account_id     账户标识，NULL 表示不限定
 * @param instrument_id  合约代码，NULL 表示不限定
 * @return 非 NULL 为结果集（需 dz_resultset_close），NULL 为失败（调 dz_errcode() 获取错误码）
 */
DZ_API DzResultSet* dz_db_query_position(DzDatabase* db,
                                         const char* account_id,
                                         const char* instrument_id);

/**
 * @brief 查询交易账户资金
 *
 * @param db         数据库句柄
 * @param account_id 账户标识，NULL 表示不限定
 * @return 非 NULL 为结果集（需 dz_resultset_close），NULL 为失败（调 dz_errcode() 获取错误码）
 */
DZ_API DzResultSet* dz_db_query_trading_account(DzDatabase* db, const char* account_id);

/**
 * @brief 查询手续费率
 *
 * @param db            数据库句柄
 * @param account_id    账户标识，NULL 表示不限定
 * @param instrument_id 合约代码，NULL 表示不限定
 * @return 非 NULL 为结果集（需 dz_resultset_close），NULL 为失败（调 dz_errcode() 获取错误码）
 */
DZ_API DzResultSet* dz_db_query_commission(DzDatabase* db,
                                           const char* account_id,
                                           const char* instrument_id);

/**
 * @brief 查询保证金率
 *
 * @param db            数据库句柄
 * @param account_id    账户标识，NULL 表示不限定
 * @param instrument_id 合约代码，NULL 表示不限定
 * @return 非 NULL 为结果集（需 dz_resultset_close），NULL 为失败（调 dz_errcode() 获取错误码）
 */
DZ_API DzResultSet* dz_db_query_margin(DzDatabase* db,
                                       const char* account_id,
                                       const char* instrument_id);

/**
 * @brief 查询 K 线数据
 *
 * @param db             数据库句柄
 * @param instrument_id  合约代码，NULL 表示全部
 * @param bar_period     K 线周期（秒数，60=1分钟, 300=5分钟, 86400=日线）
 * @param adjust_type    复权方式
 * @param start_date     起始日期（距纪元天数），0 表示不限定
 * @param end_date       结束日期（距纪元天数），0 表示不限定
 * @return 非 NULL 为结果集（需 dz_resultset_close），NULL 为失败（调 dz_errcode() 获取错误码）
 */
DZ_API DzResultSet* dz_db_query_bar(DzDatabase* db,
                                    const char* instrument_id,
                                    int32_t bar_period,
                                    DzAdjustType adjust_type,
                                    DzDate start_date,
                                    DzDate end_date);

/* ── 通用查询（数据库抽象，低频、可变字段） ── */

/**
 * @brief 通用数据查询
 *
 * 对底层数据库进行抽象，策略无需关心数据库类型。
 * query 为结构化查询描述（数据库无关），filter 为过滤条件。
 *
 * query 类型约定（资源路径式）：
 *   "order"             — 委托
 *   "trade"             — 成交
 *   "position"          — 持仓
 *   "trading_account"   — 交易账户资金
 *   "commission"        — 手续费率
 *   "margin"            — 保证金率
 *   "bar"               — K 线数据
 *   "instrument"        — 合约属性
 *   "instruments"       — 可交易合约列表
 *   "trading_day"       — 交易日
 *   "trading_schedule"  — 交易时间表
 *
 * filter 格式（JSON 对象）：
 *   简单条件：{"instrument_id": "IF2401"}
 *   组合条件：{"account_id": "CTP001", "instrument_id": "IF2401"}
 *   范围查询：{"date": {"$gte": 19736, "$lte": 19740}}
 *   枚举条件：{"status": {"$in": [2, 3]}}
 *
 * @param db       数据库句柄
 * @param query    查询类型（资源路径式）
 * @param filter   过滤条件（JSON 对象字符串），NULL 表示无过滤
 * @param version  数据版本号（0=最新，>0=指定版本，用于缓存校验/增量查询）
 * @return 非 NULL 为结果集（需 dz_resultset_close），NULL 为失败（调 dz_errcode() 获取错误码）
 */
DZ_API DzResultSet* dz_db_query(DzDatabase* db,
                                const char* query,
                                const char* filter,
                                int32_t version);

/* ── 错误信息三函数（SQLite3 风格） ── */

/**
 * @brief 获取最近错误码（thread-local），0 表示无错误
 */
DZ_API int32_t dz_errcode(void);

/**
 * @brief 错误码→描述字符串，始终非 NULL
 *
 * @param errcode  错误码
 */
DZ_API const char* dz_errstr(int32_t errcode);

/**
 * @brief 最近错误的上下文消息（thread-local），无错误返回 ""
 */
DZ_API const char* dz_errmsg(void);

/* ── SDK 诊断输出 ── */

/**
 * @brief SDK 诊断输出：stderr 单行（[dzsdk] 前缀），master 捕获转发进日志
 *
 * SDK 无日志模块，内部失败与策略引擎兜底错误统一经此输出；用户可实现
 * 策略 on_error 自行处理，但内部无法避免的错误（预加载失败、定时器异常等）
 * 仍走本函数。手工运行策略时直接显示在终端。
 *
 * @param message 诊断文本（任意非空内容，无需 [dzsdk] 前缀，自动附加）
 */
DZ_API void dz_diag(const char* message);

/* ── 版本信息 ── */

/** @brief 获取主版本号 */
DZ_API int32_t dz_version_major(void);
/** @brief 获取次版本号 */
DZ_API int32_t dz_version_minor(void);
/** @brief 获取补丁版本号 */
DZ_API int32_t dz_version_patch(void);
/** @brief 获取版本字符串，如 "0.0.1" */
DZ_API const char* dz_version_string(void);
/** @brief 获取版本十六进制，格式 major<<16 | minor<<8 | patch */
DZ_API int32_t dz_version_hex(void);

DZ_END_C_DECLS

#endif /* DZTRADER_API_H_ */
