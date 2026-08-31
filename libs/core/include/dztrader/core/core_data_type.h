#ifndef DZTRADER_CORE_CORE_DATA_TYPE_H
#define DZTRADER_CORE_CORE_DATA_TYPE_H
#include <dztrader/data_type.h>

DZ_BEGIN_C_DECLS
/** @brief 通知UI帧类型 */
#define DZ_FRAME_NOTIFY_UI ((DzFrameType)101)

/** @brief 设置逻辑持仓帧类型 */
#define DZ_FRAME_SET_LOGICAL_POSITION ((DzFrameType)103)

/** @brief 全量快照查询 (dzweb→所有进程, 无 instance_id): master 响应配置+进程状态, 子进程响应各自配置 */
#define DZ_FRAME_QUERY_FULL_SNAPSHOT ((DzFrameType)113)

/** @brief 进程控制请求 (dzweb→master, 无 instance_id, payload=ProcessControlReq) */
#define DZ_FRAME_REQUEST_PROCESS_CONTROL ((DzFrameType)115)
/** @brief 进程状态推送 (master→dzweb, 无 instance_id, payload=ProcessStatus) */
#define DZ_FRAME_RTN_PROCESS_STATUS      ((DzFrameType)116)
/** @brief 进程配置修改请求 (dzweb→master, 无 instance_id, payload=SetProcessConfigReq) */
#define DZ_FRAME_SET_PROCESS_CONFIG      ((DzFrameType)117)
/** @brief 进程配置全量推送 (master→dzweb, 无 instance_id, payload=map<name, ProcessConfig>) */
#define DZ_FRAME_RTN_PROCESS_CONFIG      ((DzFrameType)118)

/** @brief 设置自动登录/登出排程 (dzweb->网关, instance_id=目标网关, payload=AutoLoginConfig 增量) */
#define DZ_FRAME_SET_AUTO_LOGIN ((DzFrameType)119)
/** @brief 上报自动登录/登出排程 (网关->dzweb, instance_id=来源网关, payload=AutoLoginConfig 全量) */
#define DZ_FRAME_RTN_AUTO_LOGIN ((DzFrameType)120)

/** @brief 进度推送 (进程->dzweb, instance_id=来源进程, payload=ProgressStatus, 仅 RTN 无 SET) */
#define DZ_FRAME_RTN_PROGRESS ((DzFrameType)121)

/** @brief 行情设置配置请求 (op-based, dzweb→子进程) */
#define DZ_FRAME_SET_MD_CONFIG ((DzFrameType)1001)
/** @brief 行情配置变化通知 */
#define DZ_FRAME_RTN_MD_CONFIG ((DzFrameType)1002)
/** @brief 行情状态变化通知 */
#define DZ_FRAME_RTN_MD_STATUS ((DzFrameType)1003)
/** @brief 行情连接请求 */
#define DZ_FRAME_REQUEST_MD_CONNECT ((DzFrameType)1004)
/** @brief 行情断开连接请求 */
#define DZ_FRAME_REQUEST_MD_DISCONNECT ((DzFrameType)1005)
/** @brief 行情订阅请求（action 字段区分 subscribe/unsubscribe/unsubscribe_all） */
#define DZ_FRAME_REQUEST_MD_SUBSCRIBE ((DzFrameType)1006)
/** @brief 行情服务已启动 (dzmd_ctp→所有进程, instance_id=source, 通知策略/数据存储进程重订阅) */
#define DZ_FRAME_NOTIFY_MD_STARTED  ((DzFrameType)1007)
/** @brief 行情服务已停止 (master→所有进程, instance_id=source, 通知策略/数据存储进程关闭行情通道) */
#define DZ_FRAME_NOTIFY_MD_STOPPED   ((DzFrameType)1008)
/** @brief 查询订阅详情 (dzweb->md, payload={"query":"unsuccessful"} 或 {"instruments":["IF2506"]}) */
#define DZ_FRAME_QUERY_MD_SUBSCRIPTIONS  ((DzFrameType)1011)
/** @brief 返回订阅详情 (md->dzweb, payload={"subscriptions":[...],"returned_count":N,"total_matched":N,"truncated":bool,"error":string?}) */
#define DZ_FRAME_RTN_MD_SUBSCRIPTIONS    ((DzFrameType)1012)
/** @brief 行情通道读者注册请求 (策略/数据进程→master, instance_id=目标行情进程名, payload={"subscriber":"stg.<name>"}) */
#define DZ_FRAME_REQUEST_MD_READER_REGISTER   ((DzFrameType)1013)
/** @brief 行情通道读者注销请求 (策略/数据进程→master, instance_id=目标行情进程名, payload={"subscriber":"stg.<name>"}) */
#define DZ_FRAME_REQUEST_MD_READER_UNREGISTER ((DzFrameType)1014)
/** @brief 行情通道读者注册响应 (master→请求进程, instance_id=请求进程名, payload={"channel":str,"ok":bool,"message":str 失败必填}) */
#define DZ_FRAME_RTN_MD_READER_REGISTER     ((DzFrameType)1015)
/** @brief 行情通道读者注销响应 (master→请求进程, instance_id=请求进程名, payload 同上) */
#define DZ_FRAME_RTN_MD_READER_UNREGISTER   ((DzFrameType)1016)

/** @brief 交易订单请求 (basic 广播帧, 契约 td-order) */
#define DZ_FRAME_TD_ORDER_REQ ((DzFrameType)2100)

/** @brief 取消订单请求 (basic 广播帧, 契约 td-order) */
#define DZ_FRAME_TD_ORDER_CANCEL_REQ ((DzFrameType)2101)
/** @brief 交易修改配置请求 */
#define DZ_FRAME_TD_REQ_MODIFY_CONFIG ((DzFrameType)2102)
/** @brief 交易连接请求 */
#define DZ_FRAME_TD_CONNECT ((DzFrameType)2108)
/** @brief 交易断开连接请求 */
#define DZ_FRAME_TD_DISCONNECT ((DzFrameType)2109)
/** @brief 订阅账户/持仓变化(未来 td 用, 本次只占编号) */
#define DZ_FRAME_TD_SUBSCRIBE ((DzFrameType)2110)

// ============================================================================
// 交易业务帧 (2005-2017, 2000-2004 已在 data_type.h 定义为通用 TD 推送帧)
// ============================================================================
/** @brief 合约信息推送 */
#define DZ_FRAME_TD_INSTRUMENT          ((DzFrameType)2005)
/** @brief 合约交易状态推送 */
#define DZ_FRAME_TD_INSTRUMENT_STATUS   ((DzFrameType)2006)
/** @brief 交易错误回报 */
#define DZ_FRAME_TD_ERROR_RPT           ((DzFrameType)2007)
/** @brief 风控拒绝通知 */
#define DZ_FRAME_TD_RISK_REJECT         ((DzFrameType)2008)
/** @brief 出入金请求 */
#define DZ_FRAME_TD_TRANSFER_REQ        ((DzFrameType)2009)
/** @brief 出入金响应 (OnRsp, CTP 接收) */
#define DZ_FRAME_TD_TRANSFER_RSP        ((DzFrameType)2010)
/** @brief 出入金实时通知 (OnRtn, 银行权威结果) */
#define DZ_FRAME_TD_TRANSFER_RTN        ((DzFrameType)2011)
/** @brief 修改密码请求 */
#define DZ_FRAME_TD_PASSWORD_UPDATE_REQ ((DzFrameType)2012)
/** @brief 修改密码响应 */
#define DZ_FRAME_TD_PASSWORD_UPDATE_RSP ((DzFrameType)2013)
/** @brief 结算单文本 (变长 ext 帧) */
#define DZ_FRAME_TD_SETTLEMENT_INFO     ((DzFrameType)2014)
/** @brief 保证金率镜像 */
#define DZ_FRAME_TD_MARGIN_RATE         ((DzFrameType)2015)
/** @brief 手续费率镜像 */
#define DZ_FRAME_TD_COMMISSION_RATE     ((DzFrameType)2016)
/** @brief 持仓明细 */
#define DZ_FRAME_TD_POSITION_DETAIL     ((DzFrameType)2017)

// ============================================================================
// 交易配置/状态帧 (2103-2114)
// ============================================================================
/** @brief 交易配置变化通知 (网关->master) */
#define DZ_FRAME_TD_RTN_CONFIG          ((DzFrameType)2103)
/** @brief 交易状态变化通知 (网关->master) */
#define DZ_FRAME_TD_RTN_STATUS          ((DzFrameType)2104)
/** @brief 交易全量查询 (预留, 复用 DZ_FRAME_QUERY_FULL_SNAPSHOT=113) */
#define DZ_FRAME_TD_QUERY_ALL           ((DzFrameType)2105)
/** @brief 交易用户通知 (预留, 复用 DZ_FRAME_NOTIFY_UI=101) */
#define DZ_FRAME_TD_NOTIFY_UI           ((DzFrameType)2106)
/** @brief 交易服务已启动 (网关→所有进程, instance_id=source) */
#define DZ_FRAME_NOTIFY_TD_STARTED      ((DzFrameType)2111)
/** @brief 交易服务已停止 (master→所有进程, instance_id=source) */
#define DZ_FRAME_NOTIFY_TD_STOPPED       ((DzFrameType)2112)
/** @brief 交易已就绪可下单 (网关→所有进程, instance_id="name_:account_id") */
#define DZ_FRAME_NOTIFY_TD_CONNECTED     ((DzFrameType)2113)
/** @brief 交易不可用 (网关→所有进程, instance_id="name_:account_id") */
#define DZ_FRAME_NOTIFY_TD_DISCONNECTED ((DzFrameType)2114)
/** @brief 账户状态查询请求 (策略/SDK→td+master, basic 广播帧, payload=DzAccountStatusReq, 契约 account-status) */
#define DZ_FRAME_TD_QUERY_ACCOUNT_STATUS ((DzFrameType)2115)

DZ_END_C_DECLS

namespace dztrader {
constexpr auto CHANNEL_NAME_EVENT = "dzevent";
constexpr auto CHANNEL_NAME_ORDER_ID = "order_id";
constexpr auto STRATEGY_PREFIX = "stg";

}  // namespace dztrader

#endif  // DZTRADER_CORE_CORE_DATA_TYPE_H
