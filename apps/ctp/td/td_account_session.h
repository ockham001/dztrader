#ifndef DZTRADER_CTP_TD_ACCOUNT_SESSION_H_
#define DZTRADER_CTP_TD_ACCOUNT_SESSION_H_

// AccountSession: 单账户会话管理 (设计 §1.1 多账户路由)
//
// 职责:
// - 持有 CThostFtdcTraderApi + TdSpi (per-account 实例)
// - 持有 OrderRefMap + RiskGate + TdStateMachine + PositionHolding map
// - 处理 SPI 事件 (由 TdApi 主循环 pop 队列后调用 on_* 方法)
// - 推 SHM 帧 (DzOrderReport/DzTradeReport 通用字段) + 持久化 (OrderRecord/TradeRecord 含 CTP 扩展)
//
// 线程模型 (设计 §1.2):
// - 主线程: 处理 SPI 事件, 调用 on_* 方法, 推 SHM, 持久化
// - SPI 线程 (CTP 工作线程): 仅 push 事件到 event_queue_, 不接触 AccountSession 状态
//   多账户同进程: 每账户 1 个 SPI 线程共享 event_queue (MPMC, 多生产者单消费者)
//
// 生命周期:
// 1. 构造: 初始化成员, 不连接 CTP
// 2. open(): CreateFtdcTraderApi + RegisterSpi + RegisterFront + Init
// 3. (运行期) TdApi 主循环 pop 事件 -> 调用 on_* 方法
// 4. disconnect(): RegisterSpi(nullptr) + Release
// 5. 析构: disconnect (幂等)

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <ThostFtdcTraderApi.h>

#include <dztrader/core/timer_queue.h>
#include <dztrader/shm/order_id_meta.h>
#include <dztrader/shm/writer.h>

#include "common/ctp_events.h"
#include "td/td_account_session_pure.h"
#include "td/td_ctp_mapping.h"
#include "td/td_events.h"
#include "td/td_offset_converter.h"
#include "td/td_persist_writer.h"
#include "td/td_risk_gate.h"
#include "td/td_schema.h"
#include "td/td_spi.h"
#include "td/td_state.h"

namespace dztrader::ctp {

class AccountSession {
public:
    /// 构造 (不连接 CTP).
    /// @param account_id 账户标识 (CTP InvestorID)
    /// @param order_id_meta 跨进程共享的 order_id 计数器 (外部拥有)
    /// @param event_writer SHM 多写入者 (外部拥有, 多账户共享)
    /// @param persist_writer SQLite 持久化 writer (外部拥有, 多账户共享)
    /// @param event_queue SPI -> 主线程事件队列 (shared_ptr)
    /// @param timer_queue 定时器队列 (外部拥有)
    AccountSession(std::string account_id,
                   shm::OrderIdMeta& order_id_meta,
                   shm::MultiWriter& event_writer,
                   PersistWriter& persist_writer,
                   const MpmcQueuePtr& event_queue,
                   dztrader::core::TimerQueue& timer_queue);
    ~AccountSession();

    AccountSession(const AccountSession&) = delete;
    AccountSession& operator=(const AccountSession&) = delete;
    AccountSession(AccountSession&&) = delete;
    AccountSession& operator=(AccountSession&&) = delete;

    // === 生命周期 ===
    /// 打开 CTP 连接. 创建 TraderApi, 注册 SPI, 注册前置, Init.
    /// @param flow_dir CTP 流文件目录
    /// @param front_addrs 前置地址列表 (tcp://...)
    /// @param broker_id 经纪商代码
    /// @param user_id 投资者代码
    /// @param password 密码
    /// @param auth_code 认证码 (空则跳过认证)
    /// @param app_id 应用 ID (认证用, 空则跳过认证)
    void open(const std::string& flow_dir,
              const std::vector<std::string>& front_addrs,
              const std::string& broker_id,
              const std::string& user_id,
              const std::string& password,
              const std::string& auth_code = "",
              const std::string& app_id = "");

    /// 断开 CTP 连接 (幂等). Release API, 释放 SPI, 取消定时器.
    void disconnect();

    // === SPI 事件处理 (主线程, 由 TdApi 主循环 dispatch 调用) ===
    void on_front_connected();
    void on_front_disconnected(int reason);
    void on_rsp_authenticate(const OnRspAuthenticateField& f);
    void on_rsp_user_login(const OnRspTdUserLoginField& f);
    void on_rsp_settlement_confirm(const OnRspSettlementInfoConfirmField& f);
    void on_rsp_qry_instrument(const OnRspQryInstrumentField& f);
    void on_rtn_order(const OnRtnOrderField& f);
    void on_rtn_trade(const OnRtnTradeField& f);

    // === C5: 补齐缺失的 SPI 事件处理 (Plan 7+ 完整业务逻辑, 当前先记录日志不丢数据) ===
    /// 委托查询响应 (RESTART 崩溃恢复补登, 设计 §5.6)
    void on_rsp_qry_order(const OnRspQryOrderField& f);
    /// 资金查询响应 (重连后主动查询重建)
    void on_rsp_qry_trading_account(const OnRspQryTradingAccountField& f);
    /// 持仓查询响应 (重连后主动查询重建)
    void on_rsp_qry_investor_position(const OnRspQryInvestorPositionField& f);
    /// 持仓明细查询响应 (PositionHolding 重建)
    void on_rsp_qry_investor_position_detail(const OnRspQryInvestorPositionDetailField& f);
    /// 保证金率查询响应
    void on_rsp_qry_instrument_margin_rate(const OnRspQryInstrumentMarginRateField& f);
    /// 手续费率查询响应
    void on_rsp_qry_instrument_commission_rate(const OnRspQryInstrumentCommissionRateField& f);
    /// 合约交易状态回报
    void on_rtn_instrument_status(const OnRtnInstrumentStatusField& f);
    /// 报单录入响应 (CTP 同步拒单, 设计 §11.1)
    void on_rsp_order_insert(const OnRspOrderInsertField& f);
    /// 报单操作响应 (撤单同步拒绝)
    void on_rsp_order_action(const OnRspOrderActionField& f);
    /// 报单录入错误回报 (交易所拒单, 设计 §11.1)
    void on_err_rtn_order_insert(const OnErrRtnOrderInsertField& f);
    /// 报单操作错误回报 (撤单被拒)
    void on_err_rtn_order_action(const OnErrRtnOrderActionField& f);
    /// 出入金响应
    void on_rsp_transfer(const OnRspFromBankToFutureByFutureField& f);
    /// 出入金实时通知 (银行权威结果)
    void on_rtn_transfer(const OnRtnFromBankToFutureByFutureField& f);
    /// 修改登录密码响应
    void on_rsp_user_password_update(const OnRspUserPasswordUpdateField& f);
    /// 修改资金密码响应
    void on_rsp_trading_account_password_update(const OnRspTradingAccountPasswordUpdateField& f);

    // === 业务接口 ===
    /// 下单. 内部: 风控 -> order_ref 递增 -> 映射表登记 -> ReqOrderInsert.
    /// 失败 (非 Ready / 风控拒绝 / instrument 未就绪 / CTP 返回非 0) 记日志, 不抛异常.
    void place_order(const DzOrderReq& req);

    /// 撤单: 反向查找 CancelContext (order_ref + front_id + session_id 三元组) 后 ReqOrderAction.
    /// 返回 false 表示未就绪 / 订单未登记 / CTP 返回非 0, 调用方应感知并通知策略进程.
    bool cancel_order(DzOrderId order_id);

    // === 状态 ===
    TdState state() const noexcept { return state_machine_.state(); }
    bool is_ready() const noexcept { return state() == TdState::Ready; }
    const std::string& account_id() const noexcept { return account_id_; }
    RiskGate& risk_gate() noexcept { return risk_gate_; }
    const TdStateMachine& state_machine() const noexcept { return state_machine_; }

    /// 设置当前交易日 (DzDate, 距纪元天数). 日切时由 TdApi 调用.
    /// 同步到所有 PositionHolding 的 trading_day.
    void set_trading_day(int32_t trading_day);

    /// 释放 td 事件 (主线程 drain 队列时调用, type >= 100 走 td_delete_event_data).
    /// 非 td 事件调 event.delete_data(). data 为 nullptr 时 no-op.
    static void delete_event(Event& event) noexcept;

private:
    // === 内部辅助 ===
    void req_authenticate();
    void req_login();
    void req_settlement_confirm();
    /// 触发合约查询 (设计 §7.2), 进入 LoadingInstruments 状态.
    /// 失败/超时调 on_instruments_load_failed 回退到 LoggedIn (设计 §2.4.1).
    void req_qry_instrument();
    void cancel_connect_timer();
    void cancel_login_timer();
    /// 取消合约加载超时定时器 (I2)
    void cancel_instruments_load_timer();

    /// 推 DzOrderReport 到 SHM (DZ_FRAME_ORDER_REPORT, 通用字段, 不含 CTP 特有).
    void write_order_rpt(const DzOrderReport& rpt);

    /// 推 DzTradeReport 到 SHM (DZ_FRAME_TRADE_REPORT, 通用字段).
    void write_trade_rpt(const DzTradeReport& rpt);

    /// C3: 推拒绝订单回报到 SHM (status=REJECTED), 让策略进程感知拒单.
    /// @param req 原始下单请求 (含 order_id / instrument_id / direction 等)
    /// @param reason 拒绝原因 (中文, 写入 remark 字段)
    void reject_order(const DzOrderReq& req, const std::string& reason);

    /// C3: 推风控拒绝帧到 SHM (DZ_FRAME_TD_RISK_REJECT, 设计 §8.2)
    void write_risk_reject(const std::string& account_id,
                           const std::string& rule_name,
                           const std::string& reason);

    /// 持久化 OrderRecord (enqueue 到 PersistWriter, 含 CTP 扩展字段).
    void persist_order(const OrderRecord& r);

    /// 持久化 TradeRecord.
    void persist_trade(const TradeRecord& r);

    /// LoadingInstruments 期间缓冲回报 (设计 §5.3)
    void buffer_order_rpt(const OnRtnOrderField& f);
    void buffer_trade_rpt(const OnRtnTradeField& f);
    void replay_buffered_reports();

    // === 成员 ===
    std::string account_id_;
    shm::OrderIdMeta& order_id_meta_;
    shm::MultiWriter& event_writer_;
    PersistWriter& persist_writer_;
    MpmcQueuePtr event_queue_;
    dztrader::core::TimerQueue& timer_queue_;

    CThostFtdcTraderApi* api_ = nullptr;
    std::unique_ptr<TdSpi> spi_;

    TdStateMachine state_machine_;
    RiskGate risk_gate_;
    OrderRefMap order_ref_map_;
    int64_t order_ref_ = 0;
    int32_t request_id_ = 0;
    int32_t trading_day_ = 0;  ///< 当前交易日 (DzDate, 距纪元天数)

    /// 持仓 map: instrument_id -> PositionHolding (设计 §6)
    std::unordered_map<std::string, PositionHolding> holdings_;

    /// 合约 -> 交易所映射 (设计 §7.2, 由 on_rsp_qry_instrument 填充).
    /// place_order 时查表获取 exchange_id, 未命中则拒单.
    /// on_front_disconnected 清空 (重连后重新查询).
    std::unordered_map<std::string, std::string> instrument_exchange_map_;

    /// 缓冲回报 (LoadingInstruments 期间, 设计 §5.3)
    std::deque<OnRtnOrderField> buffered_orders_;
    std::deque<OnRtnTradeField> buffered_trades_;
    static constexpr size_t kMaxBuffered = 100000;

    /// 定时器 id (0 = 无挂起)
    dztrader::core::TimerQueue::TimerId connect_timer_id_ = 0;
    dztrader::core::TimerQueue::TimerId login_timer_id_ = 0;
    /// I2: 合约加载超时定时器 id (5 分钟, 设计 §2.4.1 流控持续 -3 超时)
    dztrader::core::TimerQueue::TimerId instruments_load_timer_id_ = 0;
    /// 代际失效: 断线时自增, 使已挂起定时器回调失效 (避免陈旧回调误触发)
    uint64_t generation_ = 0;

    /// CTP 登录诊断字段 (open 时保存, 用于 ReqAuthenticate/ReqUserLogin)
    std::string broker_id_;
    std::string user_id_;
    std::string password_;
    std::string auth_code_;
    std::string app_id_;
    std::vector<std::string> front_addrs_;
    std::string flow_dir_;
    /// CTP API 版本 (open 时保存, 用于 on_rsp_user_login 填充 sys_version)
    std::string api_version_;
};

}  // namespace dztrader::ctp

#endif  // DZTRADER_CTP_TD_ACCOUNT_SESSION_H_
