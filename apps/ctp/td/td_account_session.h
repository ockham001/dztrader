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
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <ThostFtdcTraderApi.h>

#include <SQLiteCpp/Database.h>

#include <dztrader/core/timer_queue.h>
#include <dztrader/shm/order_id_meta.h>
#include <dztrader/shm/writer.h>

#include "common/ctp_events.h"
#include "td/td_account_session_pure.h"
#include "td/td_ctp_mapping.h"
#include "td/td_events.h"
#include "td/td_login_finalize.h"
#include "td/td_offset_converter.h"
#include "td/td_persist_writer.h"
#include "td/td_position.h"
#include "td/td_prescan.h"
#include "td/td_report_filter.h"
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
    /// @param account_state_cb 可选账户状态通知回调 (会话内直调状态机的路径经它推送,
    ///   当前仅连接超时; 在 TdApi 主循环线程同步调用)
    /// @param boot 账户启动装载数据 (Task 5 §4.3): start_seq/orders/trades,
    ///   ctor 内初始化 seq 计数器与重放过滤器基准
    AccountSession(std::string account_id,
                   shm::OrderIdMeta& order_id_meta,
                   shm::MultiWriter& event_writer,
                   PersistWriter& persist_writer,
                   const MpmcQueuePtr& event_queue,
                   dztrader::core::TimerQueue& timer_queue,
                   std::function<void(DzAccountState)> account_state_cb = nullptr,
                   SessionBootData boot = {});
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

    /// 按需查询单合约费率/保证金 (阶段2, 契约 td-fee-margin): 入库+广播 (2015/2016), 异步回填.
    /// 由 TdApi 收到 DZ_FRAME_TD_QUERY_FEE_RATE=2116 帧调用.
    /// @param instrument_id 目标合约; @param query_type 0=保证金率, 1=手续费率, 2=两者.
    void query_fee_rate(const char* instrument_id, int8_t query_type);

    // === 状态 ===
    TdState state() const noexcept { return state_machine_.state(); }
    bool is_ready() const noexcept { return state() == TdState::Ready; }
    const std::string& account_id() const noexcept { return account_id_; }
    RiskGate& risk_gate() noexcept { return risk_gate_; }
    const TdStateMachine& state_machine() const noexcept { return state_machine_; }

    /// 设置当前交易日 (DzDate, 距纪元天数). 日切时由 TdApi 调用.
    /// 同步到所有 PositionHolding 的 trading_day; 交易日切换时清空持仓镜像
    /// (绝对态旧日镜像不得拦截新日首报, spec §4.1 跨日清空).
    void set_trading_day(int32_t trading_day);

    /// 重新发起持仓/资金补查 (登录查询失败降级后的定时补查路径).
    /// 仅 Ready 且此前查询未成功时生效; 无会话/非 Ready 时 no-op.
    void resync_account_data();

    /// 数据是否已完整 (持仓/资金双查询都成功). 供 TdApi 定时补查节流.
    bool data_query_ok() const noexcept { return data_query_ok_; }

    /// 重推各追加流最后一条 (委托/成交, 原 seq 直接写帧, 不落库不过滤器,
    /// spec §4.2 "最后一条重推"). 由 TdApi 在 Ready 广播之后调用.
    void repush_last_records();

    /// 注入独立只读连接提供器 (TdApi 注入, 供 §4.3 重连增量装载基准用; 可空).
    /// 返回 nullptr 表示库不可用, 增量装载降级 (保留现有基准).
    void set_prescan_db_provider(std::function<SQLite::Database*()> provider) {
        prescan_db_provider_ = std::move(provider);
    }

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

    /// 确保 holdings_ 中存在该合约的 PositionHolding (缺则创建).
    /// exchange_hint 为空时回退查 instrument_exchange_map_.
    PositionHolding* ensure_holding(const std::string& instrument_id,
                                    const std::string& exchange_hint = {});

    /// 用 PositionHolding 单方向状态填充 DzPositionInfo (account/trading_day/seq).
    void fill_position_info(DzPositionInfo& pos, const PositionHolding& h, DzDirection dir);

    /// 推 DZ_FRAME_POSITION_INFO (2002): 分配 seq + 更新方向 seq + 可选持久化单行 upsert.
    void push_position(PositionHolding& h, DzDirection dir, bool persist = true);

    /// is_last 收口: 应用本轮查询累加器 (diff 推帧/零帧/活动委托重灌/PositionRebuild).
    void apply_position_query();

    /// 从 boot 初始化 seq 计数器 + 重放过滤器基准 (构造时调用).
    void init_from_boot(const SessionBootData& boot);

    /// §4.3 重连重建基准: 从断开时刻后提交的 DB 行增量装载 (seq > max_seq_at_disconnect_).
    /// 需要独立只读连接 (由 TdApi 提供, 该连接为进程级单例). 失败降级 (保留现有基准).
    void reload_incremental(SQLite::Database& db);

    /// LoadingInstruments 期间缓冲回报 (设计 §5.3)
    void buffer_order_rpt(const OnRtnOrderField& f);
    void buffer_trade_rpt(const OnRtnTradeField& f);
    void replay_buffered_reports();

    /// 发起持仓查询 (登录收尾阶段一, CTP 流控串行).
    /// @param login_chain 登录/补查链发起 (失败降级收尾); false=周期重查 (失败仅清在途).
    void req_qry_investor_position(bool login_chain = true);
    /// 发起资金查询 (登录收尾阶段二).
    void req_qry_trading_account();
    /// 发起保证金率查询 (登录收尾阶段三 / 按需查询).
    /// @param instrument_id 空串=全量账户级 (登录收尾), 非空=单合约 (按需查询).
    void req_qry_margin_rate(const char* instrument_id = "");
    /// 发起手续费率查询 (登录收尾阶段四 / 按需查询).
    void req_qry_commission_rate(const char* instrument_id = "");
    /// 双查询完成 (is_last 或失败降级) 后的统一收尾:
    /// 缓冲重放 -> flush 屏障 -> on_instruments_loaded 转 Ready.
    /// 若查询阶段未结束时 (四查询未齐) 调用 no-op (防御).
    void finalize_login();

    /// 尝试推进登录收尾状态机并执行对应阶段动作; 未达前置时停留.
    void drive_finalizer();

    // === 成员 ===
    std::string account_id_;
    shm::OrderIdMeta& order_id_meta_;
    shm::MultiWriter& event_writer_;
    PersistWriter& persist_writer_;
    MpmcQueuePtr event_queue_;
    dztrader::core::TimerQueue& timer_queue_;
    /// 账户状态通知回调 (可空; 会话内直调状态机的路径经它推送 2018, 契约 account-status)
    std::function<void(DzAccountState)> account_state_cb_;

    CThostFtdcTraderApi* api_ = nullptr;
    std::unique_ptr<TdSpi> spi_;

    TdStateMachine state_machine_;
    RiskGate risk_gate_;
    OrderRefMap order_ref_map_;
    int64_t order_ref_ = 0;
    int32_t request_id_ = 0;
    int32_t trading_day_ = 0;  ///< 当前交易日 (DzDate, 距纪元天数)

    /// 账户级状态变更 seq 计数器 (spec §2.2: 主线程分配, 无需原子).
    /// 跨日累积单调, 同一事件 shm 帧与 DB 行带同一 seq.
    uint64_t seq_counter_ = 0;
    /// 重放过滤器基准 (登录/重连时 CTP 私有流去重, spec §4.1).
    std::unique_ptr<ReportFilter> report_filter_;
    /// 持仓查询累加器 (同一 (instrument, direction) 多行合并; 仅主线程).
    struct PositionQueryKey {
        std::string instrument_id;
        int8_t direction = 0;
        bool operator==(const PositionQueryKey&) const = default;
    };
    struct PositionQueryKeyHash {
        size_t operator()(const PositionQueryKey& k) const noexcept {
            size_t h = std::hash<std::string>{}(k.instrument_id);
            h ^= static_cast<size_t>(static_cast<uint8_t>(k.direction)) + 0x9e3779b9u + (h << 6) + (h >> 2);
            return h;
        }
    };
    struct PositionQueryAgg {
        std::string exchange_id;   // 查询行自带 (holding 交易所回退来源)
        int64_t volume = 0;
        int64_t yd = 0;
        double cost = 0.0;
        int64_t ctp_frozen = 0;    // LongFrozen+ShortFrozen, 仅冻结对账 WARN
    };
    std::unordered_map<PositionQueryKey, PositionQueryAgg, PositionQueryKeyHash> position_query_agg_;
    /// 基准就绪: 首个成功查询应用后置位; 断线清空. 周期查询失败不解除.
    bool position_baseline_ready_ = false;
    /// 持仓查询在途 (防周期重查与登录链重叠).
    bool position_query_in_flight_ = false;
    /// 本轮持仓查询全量组 (登录/补查 is_last 时 PositionRebuild 重灌用, spec §3.2).
    /// 每次 req_qry_investor_position 开始时清空, 逐行累加, is_last 时整体 enqueue.
    std::vector<DzPositionInfo> position_query_group_;
    /// 本轮是否已 enqueue PositionRebuild (幂等防御: 迟到的重复 is_last 不得用已消费的
    /// 空组再次重灌清空 DB).
    bool position_rebuild_consumed_ = false;
    /// 登录收尾状态机 (spec §4.2): 双查询齐才可收尾, 失败降级不阻塞 Ready.
    LoginFinalizer finalizer_;
    /// 持仓/资金查询是否已成功 (供登录降级补查节流: 双查询都成功才置 true, spec §4.2).
    bool position_query_ok_ = false;
    bool account_query_ok_ = false;
    /// 保证金率/手续费率查询是否已成功 (供登录降级补查节流).
    bool margin_rate_query_ok_ = false;
    bool commission_rate_query_ok_ = false;
    bool data_query_ok_ = false;
    /// 费率/保证金查询是否广播 SHM: false=登录批量只入库, true=按需查询入库+广播 (2015/2016).
    /// 登录收尾链起点重置 false (避免按需查询残留 true 使批量费率洪泛策略进程).
    bool fee_rate_broadcast_ = false;
    /// 按需查询 (query_type=2) 的串行推进: margin is_last 后是否接着发 commission (CTP 流控).
    bool fee_query_pending_commission_ = false;
    /// 按需查询的目标合约 (query_type=2 串行推进用; 登录链留空).
    std::string fee_query_instrument_;
    /// 上次装载水位: 断连时记录, 重连时增量装载 seq > 该值的行 (spec §4.3).
    uint64_t max_seq_at_disconnect_ = 0;
    /// 独立只读连接提供器 (TdApi 注入; 重连增量装载基准用, 可空则降级).
    std::function<SQLite::Database*()> prescan_db_provider_;

    /// 持仓 map: instrument_id -> PositionHolding (设计 §6)
    std::unordered_map<std::string, PositionHolding> holdings_;

    /// 合约 -> 交易所/最小变动价位 (设计 §7.2, 由 on_rsp_qry_instrument 填充).
    /// place_order 时查表获取 exchange_id, 未命中则拒单; price_tick 供 Task 7 用.
    /// on_front_disconnected 清空 (重连后重新查询).
    struct InstrumentBrief {
        std::string exchange_id;
        double price_tick = 0.0;
    };
    std::unordered_map<std::string, InstrumentBrief> instrument_exchange_map_;

    /// 缓冲回报 (LoadingInstruments 期间, 设计 §5.3)
    std::deque<OnRtnOrderField> buffered_orders_;
    std::deque<OnRtnTradeField> buffered_trades_;
    /// 重放进行中标志: 重放时状态仍是 LoadingInstruments, on_rtn_order/trade 的
    /// 缓冲分支须跳过, 否则重放会把这些回报重新入缓冲 (无限循环). Task 6.
    bool replaying_ = false;
    static constexpr size_t kMaxBuffered = 100000;

    /// 定时器 id (0 = 无挂起)
    dztrader::core::TimerQueue::TimerId connect_timer_id_ = 0;
    dztrader::core::TimerQueue::TimerId login_timer_id_ = 0;
    /// I2: 合约加载超时定时器 id (5 分钟, 设计 §2.4.1 流控持续 -3 超时)
    dztrader::core::TimerQueue::TimerId instruments_load_timer_id_ = 0;
    /// 代际失效: 断线时自增, 使已挂起定时器回调失效 (避免陈旧回调误触发)
    uint64_t generation_ = 0;
    /// 查询链代际 (终检发现 1): 每次查询发起时快照 generation_, 响应处理校验
    /// query_gen_ == generation_ 才继续 — 断连重连后旧会话迟到的查询响应 (含
    /// 数据行与 is_last) 一律丢弃, 防旧响应污染新链 (重灌错行/重复收尾/seq 污染),
    /// 彻底覆盖 "resync 在途断连重连" 交错。
    uint64_t query_gen_ = 0;

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
