#ifndef DZTRADER_CORE_CORE_DATA_TYPE_H
#define DZTRADER_CORE_CORE_DATA_TYPE_H
// 平台帧类型（策略不可见/仅发送帧）: 系统帧(SHM 维护/日志配置/SHM 配置/填充/预加载)、
// UI 通知、进程控制、行情控制、交易控制/业务、策略上行帧。
// 策略可见帧（策略经 dz_next_event / dz_next_md 识别消费）见 dztrader/data_type.h。
//
// 【唯一书写位置】帧的值只写在两个头文件里: dztrader/data_type.h（策略可见帧）
// 与本头（平台帧）。契约、注释、日志、测试等其余任何位置一律引用帧名 DZ_FRAME_*,
// 不得写值 —— 改帧号只需改这两处。段布局与段内追加规则见 data_type.h "帧类型" 节。
#include <dztrader/data_type.h>

DZ_BEGIN_C_DECLS

/* ── 系统帧（0-31）: SHM 通道维护/预加载、日志、通知、进程、进度 ── */
/** @brief 无效填充帧（边界填充） */
#define DZ_FRAME_INVALID_FILL ((DzFrameType)0)
/** @brief 事件通道预加载通知 (master→所有子进程, 无 instance_id) */
#define DZ_FRAME_PRELOAD_EVENT_SHM ((DzFrameType)1)
/** @brief 行情数据通道预加载通知 (instance_id=行情源名) */
#define DZ_FRAME_PRELOAD_MD_SHM ((DzFrameType)2)
/** @brief 广播刷新 event 通道订阅者列表 (所有进程执行) */
#define DZ_FRAME_UPDATE_SHM_EVENT_SUBSCRIBER ((DzFrameType)3)
/** @brief 刷新 md 通道订阅者列表 (定向, instance_id=进程名) */
#define DZ_FRAME_UPDATE_SHM_MD_SUBSCRIBER ((DzFrameType)4)
/** @brief 设置事件通道配置 (UI→master) */
#define DZ_FRAME_SET_EVENT_SHM_CONFIG ((DzFrameType)5)
/** @brief 推送事件通道配置 (master→UI) */
#define DZ_FRAME_RTN_EVENT_SHM_CONFIG ((DzFrameType)6)
/** @brief 设置行情通道配置 (UI→md) */
#define DZ_FRAME_SET_MD_SHM_CONFIG ((DzFrameType)7)
/** @brief 推送行情通道配置 (md→UI) */
#define DZ_FRAME_RTN_MD_SHM_CONFIG ((DzFrameType)8)
/** @brief 设置目标进程日志配置 (dzweb→目标进程, 契约 log) */
#define DZ_FRAME_SET_LOG_CONFIG ((DzFrameType)9)
/** @brief 触发目标进程日志 flush (dzweb→目标进程, 契约 log) */
#define DZ_FRAME_FLUSH_LOG ((DzFrameType)10)
/** @brief 进程上报当前日志配置 (各进程→dzweb, 契约 log) */
#define DZ_FRAME_RTN_LOG_CONFIG ((DzFrameType)11)
/** @brief UI 通知 (所有进程可见, dzweb 消费, 契约 notify-ui) */
#define DZ_FRAME_NOTIFY_UI ((DzFrameType)12)
/** @brief 全量快照查询 (dzweb→所有进程, 无 instance_id) */
#define DZ_FRAME_QUERY_FULL_SNAPSHOT ((DzFrameType)13)
/** @brief 进程控制请求 (dzweb→master, 契约 process) */
#define DZ_FRAME_REQUEST_PROCESS_CONTROL ((DzFrameType)14)
/** @brief 进程状态推送 (master→dzweb, 契约 process) */
#define DZ_FRAME_RTN_PROCESS_STATUS ((DzFrameType)15)
/** @brief 进程配置修改请求 (dzweb→master, 契约 process) */
#define DZ_FRAME_SET_PROCESS_CONFIG ((DzFrameType)16)
/** @brief 进程配置全量推送 (master→dzweb, 契约 process) */
#define DZ_FRAME_RTN_PROCESS_CONFIG ((DzFrameType)17)
/** @brief 设置自动登录/登出排程 (dzweb→网关, 契约 auto-login) */
#define DZ_FRAME_SET_AUTO_LOGIN ((DzFrameType)18)
/** @brief 上报自动登录/登出排程 (网关→dzweb, 契约 auto-login) */
#define DZ_FRAME_RTN_AUTO_LOGIN ((DzFrameType)19)
/** @brief 进度推送 (进程→dzweb, 契约 progress) */
#define DZ_FRAME_RTN_PROGRESS ((DzFrameType)20)

/* ── UI 帧（32-63）: 逻辑持仓、策略上行输出 ── */
/** @brief 设置逻辑持仓 (策略→平台, 契约 strategy) */
#define DZ_FRAME_SET_LOGICAL_POSITION ((DzFrameType)32)
/** @brief 策略→UI 输出 (dz_output_ui 写入, 策略仅发送无需识别) */
#define DZ_FRAME_OUTPUT_UI ((DzFrameType)33)

/* ── 行情帧（64-95）: 行情配置/状态/连接/订阅、行情通道读者注册 ── */
/** @brief 行情设置配置请求 (op-based, dzweb→md, 契约 md-config) */
#define DZ_FRAME_SET_MD_CONFIG ((DzFrameType)65)
/** @brief 行情配置变化通知 (md→dzweb, 契约 md-config) */
#define DZ_FRAME_RTN_MD_CONFIG ((DzFrameType)66)
/** @brief 行情状态变化通知 (md→dzweb, 契约 md-status) */
#define DZ_FRAME_RTN_MD_STATUS ((DzFrameType)67)
/** @brief 行情连接请求 (dzweb→md, 契约 md-subscription) */
#define DZ_FRAME_REQUEST_MD_CONNECT ((DzFrameType)68)
/** @brief 行情断开连接请求 (dzweb→md, 契约 md-subscription) */
#define DZ_FRAME_REQUEST_MD_DISCONNECT ((DzFrameType)69)
/** @brief 行情订阅请求 (action 区分 subscribe/unsubscribe, 契约 md-subscription) */
#define DZ_FRAME_REQUEST_MD_SUBSCRIBE ((DzFrameType)70)
/** @brief 查询订阅详情 (dzweb→md, 契约 md-subscription) */
#define DZ_FRAME_QUERY_MD_SUBSCRIPTIONS ((DzFrameType)71)
/** @brief 返回订阅详情 (md→dzweb, 契约 md-subscription) */
#define DZ_FRAME_RTN_MD_SUBSCRIPTIONS ((DzFrameType)72)
/** @brief 行情通道读者注册请求 (进程→master, 契约 shm) */
#define DZ_FRAME_REQUEST_MD_READER_REGISTER ((DzFrameType)73)
/** @brief 行情通道读者注销请求 (进程→master, 契约 shm) */
#define DZ_FRAME_REQUEST_MD_READER_UNREGISTER ((DzFrameType)74)
/** @brief 行情通道读者注册响应 (master→请求进程, 契约 shm) */
#define DZ_FRAME_RTN_MD_READER_REGISTER ((DzFrameType)75)
/** @brief 行情通道读者注销响应 (master→请求进程, 契约 shm) */
#define DZ_FRAME_RTN_MD_READER_UNREGISTER ((DzFrameType)76)

/* ── 交易推送帧（1000-1023）: 行情源生命周期 + 交易推送 ── */
/** @brief 行情服务已启动 (md→所有进程, instance_id=行情源名) */
#define DZ_FRAME_NOTIFY_MD_STARTED ((DzFrameType)1000)
/** @brief 行情服务已停止 (master→所有进程, instance_id=行情源名) */
#define DZ_FRAME_NOTIFY_MD_STOPPED ((DzFrameType)1001)

/* ── 交易控制帧（1024-1123）: 交易控制/状态/账户查询/出入金改密 ── */
/** @brief 交易订单请求 (basic 广播帧, 契约 td-order) */
#define DZ_FRAME_TD_ORDER_REQ ((DzFrameType)1024)
/** @brief 取消订单请求 (basic 广播帧, 契约 td-order) */
#define DZ_FRAME_TD_ORDER_CANCEL_REQ ((DzFrameType)1025)
/** @brief 交易修改配置请求 (dzweb→td) */
#define DZ_FRAME_TD_REQ_MODIFY_CONFIG ((DzFrameType)1026)
/** @brief 交易连接请求 (dzweb→td) */
#define DZ_FRAME_TD_CONNECT ((DzFrameType)1027)
/** @brief 交易断开连接请求 (dzweb→td) */
#define DZ_FRAME_TD_DISCONNECT ((DzFrameType)1028)
/** @brief 交易配置变化通知 (td→dzweb) */
#define DZ_FRAME_TD_RTN_CONFIG ((DzFrameType)1029)
/** @brief 交易状态变化通知 (td→dzweb) */
#define DZ_FRAME_TD_RTN_STATUS ((DzFrameType)1030)
/** @brief 交易服务已启动 (td→所有进程, instance_id=网关名) */
#define DZ_FRAME_NOTIFY_TD_STARTED ((DzFrameType)1031)
/** @brief 交易服务已停止 (master→所有进程, instance_id=网关名) */
#define DZ_FRAME_NOTIFY_TD_STOPPED ((DzFrameType)1032)
/** @brief 交易已就绪可下单 (td→所有进程, instance_id=网关名:账户ID) */
#define DZ_FRAME_NOTIFY_TD_CONNECTED ((DzFrameType)1033)
/** @brief 交易不可用 (td→所有进程, instance_id=网关名:账户ID) */
#define DZ_FRAME_NOTIFY_TD_DISCONNECTED ((DzFrameType)1034)
/** @brief 账户状态查询请求 (策略→td+master, 契约 account-status) */
#define DZ_FRAME_TD_QUERY_ACCOUNT_STATUS ((DzFrameType)1035)
/** @brief 风控拒绝通知 (JSON ext 帧, 契约 td-risk-reject) */
#define DZ_FRAME_TD_RISK_REJECT ((DzFrameType)1037)
/** @brief 出入金请求 (JSON ext 帧, 契约 td-account-ops) */
#define DZ_FRAME_TD_TRANSFER_REQ ((DzFrameType)1038)
/** @brief 出入金响应 (JSON ext 帧, 契约 td-account-ops) */
#define DZ_FRAME_TD_TRANSFER_RSP ((DzFrameType)1039)
/** @brief 出入金实时通知 (JSON ext 帧, 契约 td-account-ops) */
#define DZ_FRAME_TD_TRANSFER_RTN ((DzFrameType)1040)
/** @brief 修改密码请求 (JSON ext 帧, 契约 td-account-ops) */
#define DZ_FRAME_TD_PASSWORD_UPDATE_REQ ((DzFrameType)1041)
/** @brief 修改密码响应 (JSON ext 帧, 契约 td-account-ops) */
#define DZ_FRAME_TD_PASSWORD_UPDATE_RSP ((DzFrameType)1042)
/** @brief 单合约信息定向刷新请求 (策略→td, basic 广播帧, 契约 instrument) */
#define DZ_FRAME_TD_QUERY_INSTRUMENT ((DzFrameType)1043)

DZ_END_C_DECLS

namespace dztrader {
constexpr auto CHANNEL_NAME_EVENT = "dzevent";
constexpr auto CHANNEL_NAME_ORDER_ID = "order_id";
constexpr auto STRATEGY_PREFIX = "stg";

}  // namespace dztrader

#endif  // DZTRADER_CORE_CORE_DATA_TYPE_H
