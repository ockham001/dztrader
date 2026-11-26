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
#include <optional>
#include <string>
#include <string_view>
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
#include "td/td_day_load.h"
#include "td/td_events.h"
#include "td/td_login_finalize.h"
#include "td/td_notify_types.h"
#include "td/td_offset_converter.h"
#include "td/td_persist_records.h"
#include "td/td_persist_writer.h"
#include "td/td_position.h"
#include "td/td_query_lane.h"
#include "td/td_query_pacer.h"
#include "td/td_report_filter.h"
#include "td/td_risk_gate.h"
#include "td/td_spi.h"
#include "td/td_state.h"

namespace dztrader::ctp {

/// 账户会话 UI 通知回调 (登录 flush 屏障超时等; 在 TdApi 主循环线程同步调用).
/// 别名到进程内唯一类型 TdNotifyFn (td_notify_types.h); 空 = 仅日志 (不通知 UI).
using AccountNotifyFn = TdNotifyFn;

class AccountSession {
public:
    /// 构造 (不连接 CTP).
    /// @param account_id 柜台真实投资者代码 (CTP InvestorID 与 UserID 同值, ADR 0056)
    /// @param order_id_meta 跨进程共享的 order_id 计数器 (外部拥有)
    /// @param event_writer SHM 多写入者 (外部拥有, 多账户共享)
    /// @param persist_writer SQLite 持久化 writer (外部拥有, 多账户共享)
    /// @param event_queue SPI -> 主线程事件队列 (shared_ptr)
    /// @param timer_queue 定时器队列 (外部拥有)
    /// @param account_state_cb 可选账户状态通知回调 (会话内直调状态机的路径经它推送,
    ///   当前仅连接超时; 在 TdApi 主循环线程同步调用)
    /// @param seq_allocator 无会话拒单的账户级 seq 分配器 (可空; 空 = 只用 DB 播种)。
    ///   登录日装载时用 max(day_load.start_seq, 本分配器 current) 抬高 seq 计数器。
    /// @param notify 可选账户级 UI 通知回调 (登录 flush 屏障超时 / 登录失败原因经它转 UI;
    ///   主循环线程调用)
    AccountSession(std::string account_id,
                   shm::OrderIdMeta& order_id_meta,
                   shm::MultiWriter& event_writer,
                   PersistWriter& persist_writer,
                   const MpmcQueuePtr& event_queue,
                   dztrader::core::TimerQueue& timer_queue,
                   std::function<void(DzAccountState)> account_state_cb = nullptr,
                   AccountSeqAllocator* seq_allocator = nullptr,
                   AccountNotifyFn notify = nullptr);
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
    /// @param password 密码
    /// @param auth_code 认证码 (空则跳过认证)
    /// @param app_id 应用 ID (认证用, 空则跳过认证)
    /// 账户身份（CTP 的 UserID 与 InvestorID）一律取构造时的 account_id（ADR 0056）。
    void open(const std::string& flow_dir,
              const std::vector<std::string>& front_addrs,
              const std::string& broker_id,
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
    /// 通用错误响应（ADR 0055 / #457）：按在途查询 `nRequestID` 路由到对应失败路径，
    /// 结束在途查询；未匹配（非查询类请求的错误）只记 WARN。
    void on_rsp_error(const OnRspErrorTdField& f);

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

    /// 设置当前交易日 (DzDate, 距纪元天数). 日切/登录时由 TdApi 调用.
    /// 对每个 PositionHolding 执行跨日迁移 (today->yd, 清挂单冻结), 变化侧按
    /// 增量规则推帧/落库 (绝对态旧日镜像不拦截新日首报, spec §4.1 跨日迁移).
    void set_trading_day(int32_t trading_day);

    /// 更新 CTP 查询通道的最小发送间隔（契约 td-config `qry_min_interval_ms`，ADR 0055）。
    /// 配置热更新时由 TdApi 对活跃会话调用；立即作用于后续调度。
    void set_query_min_interval(std::chrono::milliseconds interval);

    // === 运行期资金刷新（ADR 0055 / #458） ===
    /// 运行期资金查询发送钩子（测试注入；空 = 走 `api_`）。返回 `ReqQryTradingAccount` 返回码。
    using QuerySendFn = std::function<int(CThostFtdcQryTradingAccountField&, int)>;
    void set_query_send_hook(QuerySendFn hook);
    /// 查询时钟（测试注入；空 = `steady_clock`）。确定性用例用它消除 sleep。
    using QueryClockFn = std::function<QueryPacer::TimePoint()>;
    void set_query_clock(QueryClockFn clock);
    /// 结算单确认发送钩子（测试注入；空 = 走 `api_`）。返回 `ReqSettlementInfoConfirm` 返回码。
    ///
    /// 为什么需要（#488）：`req_settlement_confirm()` 直接解引用 `api_`，而 `api_` 只能由
    /// `open()` 创建真实 `CThostFtdcTraderApi`（需连柜台）。没有这个钩子，看门狗排定与
    /// `ret != 0` 失败出口两条路径只能靠「读代码」确认，属 `docs/unit-test-standard.md` §1.1
    /// 判定的失效测试形态。与 `set_query_send_hook` 同一形态。
    using SettlementSendFn = std::function<int(CThostFtdcSettlementInfoConfirmField&, int)>;
    void set_settlement_send_hook(SettlementSendFn hook);
    /// 测试夹具专用：驱动生产 `req_settlement_confirm()`（会话须先在 `LoggedIn`）。
    /// 与 `set_settlement_send_hook` 配套 —— 用它才能在真会话上验「看门狗被排定」与
    /// 「发送被拒时停在失败态 + 弹窗」。
    void request_settlement_confirm_for_test();
    /// 测试夹具专用：从 Idle 重放生产登录链的状态迁移，停在 target（生产由 CTP 回调驱动）。
    /// 覆盖登录链各段（`Connected` / `Authenticating` / `Authenticated` / 认证后各段直到
    /// `Ready`）的用例用它把会话摆到失败发生前一步；迁移失败当场抛，防止夹具与生产链漂移。
    /// @note 停在 `Authenticating` 时 10 秒超时定时器**未**武装（武装在 `req_authenticate()`
    ///   里）；需要验证超时收口的用例应从 `Connected` 走 `request_authenticate_for_test()`。
    void advance_login_chain_for_test(TdState target);
    /// 测试夹具专用：把会话推进到 Ready（生产由登录收尾链进入）。
    /// @param position_ok 同时置/清持仓基准就绪标志（覆盖写路径守卫用例）
    void enter_ready_for_test(bool position_ok = true);
    /// 运行期查询调度驱动（lane 定时器回调；测试可直接调用以注入确定时序）。
    void pump_query_lane();
    /// 测试夹具专用：立即触发运行期资金查询的 10s 超时路径（生产由 TimerQueue 触发）。
    void expire_runtime_query_timeout_for_test();
    /// 测试夹具专用：立即触发结算单确认看门狗的超时收口（生产由 TimerQueue 触发）。
    void expire_settlement_confirm_timeout_for_test();
    /// 测试夹具专用：缩短结算单确认看门狗时长（默认 `kSettlementConfirmTimeout`）。
    ///
    /// 用途是让「真 TimerQueue 回调」可测：用 `std::chrono::milliseconds(0)` 武装后
    /// `timer_queue_.tick()` 即触发——否则只能验证「定时器排上了」，而排上的回调是空壳
    /// 也会绿（`docs/unit-test-standard.md` §1.1 断言能失败）。
    void set_settlement_confirm_timeout_for_test(std::chrono::seconds timeout);
    /// 认证发送钩子（测试注入；空 = 走 `api_`）。返回 `ReqAuthenticate` 返回码。
    /// 与 `set_settlement_send_hook` 同形态：`req_authenticate()` 直接解引用 `api_`，而
    /// `api_` 只能由 `open()` 创建真实 `CThostFtdcTraderApi`（需连柜台）。没有它，
    /// 「认证被拒 / 10 s 超时两条失败出口都写 last_error 并推通知」只能靠读代码确认。
    using AuthSendFn = std::function<int(CThostFtdcReqAuthenticateField&, int)>;
    void set_auth_send_hook(AuthSendFn hook);
    /// 测试夹具专用：驱动生产 `req_authenticate()`（会话须已在 `Connected`）。
    /// 与 `request_settlement_confirm_for_test` 配套：它才会**武装认证超时定时器**。
    void request_authenticate_for_test();
    /// 测试夹具专用：立即触发认证超时的收口（生产由 TimerQueue 回调）。
    /// 与 `expire_settlement_confirm_timeout_for_test` 同用途：走同一收口逻辑，不 sleep。
    void expire_auth_timeout_for_test();
    /// 测试夹具专用：缩短认证超时时长（默认 10s）。
    /// 与 `set_settlement_confirm_timeout_for_test` 同用途：让「真 TimerQueue 回调」可测——
    /// 用 `0` 武装后 `timer_queue_.tick()` 即触发，否则只能验证「定时器排上了」，而排上的
    /// lambda 是空壳也会绿（`docs/unit-test-standard.md` §1.1 断言能失败）。
    void set_auth_timeout_for_test(std::chrono::seconds timeout);
    /// 登录发送钩子（测试注入；空 = 走 `api_`）。返回 `ReqUserLogin` 返回码。
    /// 与 `set_auth_send_hook` 同形态：`req_login()` 直接解引用 `api_`，而 `api_` 只能由
    /// `open()` 创建真实 `CThostFtdcTraderApi`（需连柜台）。没有它，登录段的五条失败出口
    /// （柜台拒绝 / 10 秒超时 / 交易日无效 / 基准装载失败 / 发送被拒）在真会话上无法驱动；
    /// 发送被拒这条更依赖它 —— 它本身就是「发送返回非 0」。
    using LoginSendFn = std::function<int(CThostFtdcReqUserLoginField&, int)>;
    void set_login_send_hook(LoginSendFn hook);
    /// 测试夹具专用：驱动生产 `req_login()`（会话须已在 `Connected` / `Authenticated`）。
    /// 它才会**武装登录超时定时器**。
    void request_login_for_test();
    /// 测试夹具专用：立即触发登录超时的收口（生产由 TimerQueue 回调）。
    /// 与 `expire_auth_timeout_for_test` 同用途：走同一收口逻辑，不 sleep。
    void expire_login_timeout_for_test();
    /// 测试夹具专用：缩短登录超时时长（默认 10s），让「真 TimerQueue 回调」可测。
    void set_login_timeout_for_test(std::chrono::seconds timeout);
    /// 测试夹具专用：立即触发 30 s 连接超时的收口（生产由 TimerQueue 回调）。
    /// 与 `expire_login_timeout_for_test` 同用途：走同一收口逻辑，不 sleep。
    void expire_connect_timeout_for_test();
    /// 测试夹具专用：读当前会话交易日。
    /// 存在的唯一理由：迟到登录响应（#495）的副作用全部落在内部状态（会话交易日、重放
    /// 过滤器基准、order_ref），没有它们就没有可失败的断言——用「状态机没变」断言的话，
    /// 无论有没有守卫都会绿（docs/unit-test-standard.md §1.1 的失效测试形态）。
    int32_t trading_day_for_test() const noexcept { return trading_day_; }
    /// 合约查询发送钩子（测试注入；空 = 走 `api_`）。返回 `ReqQryInstrument` 返回码。
    /// 与 `set_settlement_send_hook` 同形态，让「发送被拒」与 `on_rsp_error` 两条失败出口
    /// 在真会话上可驱动（否则只有 `api_` 非空的真机路径能到，测试无法证伪）。
    using InstrumentSendFn = std::function<int(CThostFtdcQryInstrumentField&, int)>;
    void set_instrument_send_hook(InstrumentSendFn hook);
    /// 测试夹具专用：驱动生产 `req_qry_instrument()`（会话须在 `LoadingInstruments`）。
    void request_instrument_query_for_test();
    /// 测试夹具专用：缩短合约加载超时时长（默认 5 分钟）。
    /// 与 `set_settlement_confirm_timeout_for_test` 同用途：让「真 TimerQueue 回调」可测，
    /// 而不是只验「定时器排上了」（排上的 lambda 是空壳也会绿）。
    void set_instruments_load_timeout_for_test(std::chrono::seconds timeout);

    /// 手动刷新请求（契约 td-refresh）：Ready 时置脏一次并返回 true；
    /// 非 Ready / 已离开会话返回 false（调用方记 WARN，不改变账户状态）。
    /// 实际发送仍受账户查询通道的在途 / 最小间隔约束。
    bool request_fund_refresh();

    /// 最近一轮持仓查询是否成功 (未知方向使整轮快照不可信时置 false).
    /// 与「登录收尾是否完成」分开暴露: 会话未登录 / 收尾未完成时本标志可能恒为 false,
    /// 拿它断言「未知方向按查询失败处置」会恒真 (issue #367 审查 F9).
    bool position_query_ok() const noexcept { return position_query_ok_; }

    /// 取走「跨交易日 → 主动登出」请求 (ADR 0049 决策 5 / 契约 td-data-sync §3 §7).
    /// 回报处理路径判定回报不能自证属于当前会话交易日时置位; TdApi::dispatch 在本事件处理完、
    /// 用完本会话之后取走并走既有 disconnect 路径登出 —— 登出会 erase 会话, handler 不能自毁.
    /// 取走即清零 (单次语义: 同一会话只请求一次登出).
    bool take_cross_day_logout_request() noexcept {
        const bool requested = cross_day_logout_requested_;
        cross_day_logout_requested_ = false;
        return requested;
    }

    /// 未知枚举累计计数 (按种类). TdApi 在拼状态帧 ext 时读取.
    const UnknownEnumCounts& unknown_enum_counts() const noexcept { return unknown_enum_counts_; }

    /// 重推各追加流最后一条 (委托/成交, 原 seq 直接写帧, 不落库不过滤器,
    /// spec §4.2 "最后一条重推"). 由 TdApi 在 Ready 广播之后调用.
    void repush_last_records();

    /// 注入独立只读连接提供器 (TdApi 注入, 供登录日装载用; 可空).
    /// 返回 nullptr 表示库不可用, 登录日装载失败按登录失败路径处理 (ADR 0045).
    void set_read_db_provider(std::function<SQLite::Database*()> provider) {
        read_db_provider_ = std::move(provider);
    }

    /// 释放 td 事件 (主线程 drain 队列时调用, type >= 100 走 td_delete_event_data).
    /// 非 td 事件调 event.delete_data(). data 为 nullptr 时 no-op.
    static void delete_event(Event& event) noexcept;

private:
    // === 内部辅助 ===
    void req_authenticate();
    void req_login();
    void req_settlement_confirm();

    /// 推送一条状态机通知 (td 登录链失败出口的**唯一**推送实现, #493).
    ///
    /// 三条登录链失败助手 (#407 登录 / #488 结算确认 / #488 合约加载) 原本各自逐字重复
    /// 「取状态机转移返回值 → 判空 → 判 notify_ → 拼消息 → 调 notify_ → 两层 catch」;
    /// 每加一条失败路径就要再拷一份。收敛到这里后, 各出口只剩「转状态 (含收定时器)
    /// + 拼消息 + 调本函数」。
    ///
    /// 两个早退对应原本各写一遍的两处判断:
    /// - `!notif`: 状态机拒绝了这次转移 (此刻不在该出口对应的那一段状态, 具体是哪一段由各
    ///   调用点说明; 典型为已断线 / 已登出, 状态机已记 WARN) —— 此刻弹窗对不上界面状态,
    ///   不推送。注意这不等于「只有真实登录尝试才推送」: 该情形同样静默, 是有意取舍
    ///   (断线场景由 Offline / 重连反馈兜底)。
    /// - `!notify_`: 未注入 (测试 / 无 UI) —— 状态已转移, 仅少一条通知, 不产生日志噪音。
    ///
    /// 异常兜底: `notify_` 可能抛 (UI 通道故障), 必须在此吞掉 —— 调用点在 TdApi 主循环与
    /// TimerQueue 回调上, 异常逃逸会中断状态机流程 / 打穿主循环。
    ///
    /// **范围**: 只覆盖「消费状态机转移返回值」的登录链失败出口。本类另有若干直接
    /// `notify_(字面量 level, ...)` 的通知 (运行期资金刷新 / 持仓基准查询 / 未知枚举 /
    /// 跨日登出 / 回放外部告警) —— 它们没有状态机通知可消费, 形态不同, 不在本 helper 范围。
    ///
    /// @param notif 状态机转移返回的通知 (level / popup 由状态机决定)
    /// @param message 已拼好的中文通知正文 (各出口用对应的 `*_failed_message` 纯函数构造)
    /// @param prose 异常兜底日志格式串的 prose 部分 (各出口沿用自己收敛前那句 lower-case
    ///   English 措辞, 使日志行与收敛前逐字一致)。
    ///   **已知限制**: 因为 prose 走格式串占位符 (`"{} | account={} ..."`),
    ///   `check_log_format.py` 的 R3/R5 (非英文 / prose 大小写与句号) 只看得到 `"{}"`,
    ///   对这三句不再生效 —— 改措辞时需人工保证 lower-case English、无句号。
    void push_notification(const std::optional<TdNotification>& notif,
                           const std::string& message,
                           std::string_view prose);

    /// 认证失败统一出口 (#489 并入 #494): 状态机转 AuthFailed + 写 last_error + 上浮 UI 通知.
    ///
    /// 与 notify_login_failed 同一问题形态: 两个调用点 (OnRspAuthenticate 被拒 / 10 秒超时)
    /// 此前都丢弃了状态机返回值, 账户静默停在 2/10「已连接」——用户只看到连接成功, 不知道
    /// 认证已经失败, 而这正是本该修复的缺陷。本票同时把失败位置改对: 停在 3/10「认证失败」
    /// (ADR 0059 决策 1), 不再回退。
    ///
    /// 超时与柜台拒绝走同一出口 (ADR 0059 决策 5): 同一失败态、同一 last_error 写入,
    /// 由 code 区分。
    ///
    /// @param code 段内原因码 (auth_rejected 带 CTP error_id / auth_timeout)
    /// @param reason 失败场景 (中文, 供 last_error.message 与通知正文)。柜台拒绝时传 CTP
    ///   `ErrorMsg` 原文 (如「CTP:用户认证失败」, 错误码走 error_id 字段不在正文重复);
    ///   超时传固定文案「10 秒内未收到柜台的认证响应」; 无详情传空串 (前端按 code 兜底)。
    ///   不复用状态机的通用文案「认证失败」——只说失败不说原因, 用户拿不到可用信息。
    /// 时序: 必须先 on_authenticate_failed() 转状态再通知 (与 md 一致)。异常不逃逸
    /// (push_notification 兜底)。状态机拒绝转移 (此刻不在 Authenticating, 如前置已断线或
    /// 已登出) 时不推送, 见 push_notification 的 `!notif` 说明。
    void notify_auth_failed(TdFailureCode code, const std::string& reason, int error_id = 0);

    /// 登录失败统一出口 (#407 / #495): 状态机停在 `LoginFailed` + 写 last_error + 上浮 UI 通知.
    ///
    /// 失败后停在 5/10「登录失败」这个失败态 (ADR 0059 决策 1), 界面本身已显示异常;
    /// 但状态帧的 `state` 只回答「停在哪一段」, 段内原因 (柜台拒绝 / 超时 / 交易日无效 /
    /// 基准装载失败) 由 `last_error.code` 回答, 二者在同一次转移写出 (契约 td-status §2.5)。
    /// 本进程不会自动重发登录 (见 #406), 所以失败也必须让用户看见, 否则账户会静默卡住。
    ///
    /// @param code 段内原因码 (login_rejected / login_timeout / login_parse_error /
    ///   day_load_failed / login_send_failed)。它与 reason 一起写进 last_error,
    ///   并决定通知前缀措辞。
    /// @param reason 失败场景 (中文, 供 last_error.message 与通知正文)。柜台拒绝时传 CTP
    ///   `ErrorMsg` 原文 (错误码走 error_id 字段, 不在正文重复); 超时 / 解析 / 装载失败传
    ///   固定文案; 无详情传空串 (前端按 code 兜底)。不复用状态机的通用文案
    ///   「交易登录失败」——只说失败不说原因, 用户拿不到可用信息。
    /// @param error_id CTP ErrorID (仅 login_rejected 有意义, 无 rsp_info 时为 -1;
    ///   其余传 0 —— 0 在契约里表示「无 CTP 错误码」)。
    ///
    /// 时序: 必须先 on_login_failed() 转状态再通知 (与 md 一致)。异常不逃逸 (push_notification
    /// 兜底)。状态机拒绝转移 (此刻不在 LoggingIn, 如前置已断线或已登出) 时不推送, 见
    /// push_notification 的 `!notif` 说明。
    void notify_login_failed(TdFailureCode code, const std::string& reason, int error_id = 0);

    /// 结算单确认失败统一出口 (#488 / #496): 取消看门狗 + 状态机转失败态 + 失败原因上浮 UI.
    ///
    /// 与 notify_login_failed 同一问题形态: on_settlement_confirm_failed() 停在
    /// `SettlementConfirmFailed` (进度 7/10「结算单确认失败」, #496 起不再回退 LoggedIn)。
    /// 本进程不自动重试登录链 (#406), 不上浮就是静默停下。调用方不再各自处置状态机返回值
    /// (此前 5 个调用点全部丢弃了通知, 失败无痕)。
    ///
    /// @param code 段内原因码 (settlement_rejected / settlement_timeout)。它与 reason 一起
    ///   写进 last_error, 并决定通知前缀措辞。
    /// @param reason 失败场景 (中文, 供 last_error.message 与通知正文; 如「柜台拒绝，错误码
    ///   3：...」/「60 秒内未收到柜台的结算单确认响应」)。柜台拒绝以外的发送被拒也归
    ///   `settlement_rejected` (契约 td-status §2.5: 发送被拒不是柜台裁决, 但同属「被拒」)。
    /// @param error_id CTP ErrorID (仅柜台拒绝响应有意义, 其余传 0)
    /// 时序: 必须先 on_settlement_confirm_failed() 转状态再通知 (与 md 一致)。异常不逃逸。
    /// 状态机拒绝转移 (此刻不在 Confirming: 已断线 / 已登出 / 看门狗已收口后又来了迟到失败)
    /// 时不推送, 见 push_notification 的 `!notif` 说明。
    void notify_settlement_confirm_failed(TdFailureCode code,
                                          const std::string& reason,
                                          int error_id = 0);

    /// 合约加载失败统一出口 (#488 / #496): 取消加载定时器 + 状态机转失败态 + 失败原因上浮.
    ///
    /// 同上: 停在 `InstrumentsLoadFailed` (8/10「合约加载失败」), 且不会有下一次尝试 ——
    /// 必须让用户看见。
    /// @param code 段内原因码 (instruments_rejected / instruments_timeout)。其中
    ///   `instruments_rejected` 覆盖三种来源 (查询发送被拒 / `OnRspError` 错误响应 /
    ///   `is_last` 带错误) —— 三者对用户是同一件事: 柜台侧拒绝了这次加载。
    /// @param reason 失败场景 (中文; 如「柜台拒绝，错误码 3：...」/「查询 5 分钟未收到最后一批
    ///   响应」/「发送合约查询请求失败（ret=-2）」)。状态机拒绝转移 (此刻不在
    ///   LoadingInstruments) 时不推送, 见 push_notification 的 `!notif` 说明。
    /// @param error_id CTP ErrorID (仅柜台错误响应有意义, 其余传 0)
    void notify_instruments_load_failed(TdFailureCode code,
                                        const std::string& reason,
                                        int error_id = 0);

    /// 认证 10 s 超时收口 (#489 并入 #494): 生产由 TimerQueue 回调, 测试经
    /// `expire_auth_timeout_for_test` 注入同一条路径 (不 sleep)。
    /// 已离开 Authenticating (响应已到 / 已断线 / 已登出) 时忽略迟到超时。
    void on_auth_timeout();

    /// 登录 10 s 超时收口 (#495): 生产由 TimerQueue 回调, 测试经
    /// `expire_login_timeout_for_test` 注入同一条路径 (不 sleep)。
    /// 已离开 LoggingIn (响应已到 / 已断线 / 已登出) 时忽略迟到超时。
    void on_login_timeout();

    /// 连接 30 s 超时收口 (#495): 生产由 TimerQueue 回调, 测试经
    /// `expire_connect_timeout_for_test` 注入同一条路径 (不 sleep)。
    /// 例外路径 (ADR 0059 决策 7): 回 `Idle` 不进失败态, 只写 last_error + 通知。
    /// 已离开 Connecting (前置已连上 / 已登出) 时忽略迟到超时。
    void on_connect_timeout();

    /// 结算单确认看门狗超时收口 (#488): 生产由 TimerQueue 回调, 测试经
    /// `expire_settlement_confirm_timeout_for_test` 注入同一条路径 (不 sleep)。
    /// 已离开 Confirming (响应已到 / 已断线 / 已登出) 时忽略迟到超时。
    void on_settlement_confirm_timeout();

    /// 触发合约查询 (设计 §7.2), 进入 LoadingInstruments 状态.
    /// 失败/超时调 notify_instruments_load_failed 回退到 LoggedIn (设计 §2.4.1).
    void req_qry_instrument();
    void cancel_connect_timer();
    void cancel_login_timer();
    /// 取消合约加载超时定时器 (I2)
    void cancel_instruments_load_timer();
    /// 取消结算单确认看门狗定时器 (#488; 收到响应 / 断连 / 登出时清理).
    void cancel_settlement_confirm_timer();
    /// 取消基准持仓查询的重试定时器 (查询成功 / 重试耗尽 / 断连时清理, 不留残留定时器).
    void cancel_position_query_retry_timer();
    /// 取消基准持仓查询单次尝试的超时定时器.
    void cancel_position_query_timeout_timer();

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

    /// 上报一条回报里的未知枚举: 每个种类计一次数 + ERROR 日志 + notify_ui (popup=false)
    /// (issue #367 上报口径). 只计数与通知, **不触发任何查询** (ADR 0049: 持仓无周期查询,
    /// 未知枚举由运维可观测性承接, 不由"重查纠正"承接).
    /// 通知按最小间隔限频 (kUnknownEnumReportIntervalS): 未知枚举可能每条回报都带,
    /// 逐条通知会刷爆前端通知环; 间隔内只计数与记日志.
    /// 只记日志不猜值; 调用方已把对应字段落成 UNKNOWN.
    void report_unknown_enums(const UnknownEnumSink& unknown);

    /// 持久化 TradeRecord.
    void persist_trade(const TradeRecord& r);

    /// 确保 holdings_ 中存在该合约的 PositionHolding (缺则创建).
    /// exchange_hint 为空时回退查 instrument_exchange_map_.
    PositionHolding* ensure_holding(const std::string& instrument_id,
                                    const std::string& exchange_hint = {});

    /// 该合约是否为套利 / 组合合约 (product_class == DZ_PRODUCT_SPREAD).
    /// 组合合约不参与持仓统计 (ADR 0049 决策 6): 持仓查询行 / 成交 / 委托冻结一律跳过;
    /// 合约表未命中 (未加载) 返回 false —— 按普通合约处理, 不猜.
    bool is_spread_instrument(const std::string& instrument_id) const;

    /// 回报交易日判死时的登出请求 (ADR 0049 决策 5): 首次判死置位 + 推 ERROR 通知并返回 true
    /// (调用方据此记 ERROR 日志并丢弃该回报); 已判死过返回 false (静默丢弃, 不重复通知).
    /// 判定本身在 judge_report_trading_day (纯函数, 可单测).
    /// @param verdict judge_report_trading_day 的判定值 (调用方已确认 != Match)
    /// @param report_day 回报自带的原始 TradingDay (仅用于通知文案与日志)
    bool request_cross_day_logout(ReportTradingDayVerdict verdict, const char* report_day);

    /// 记录一条**实时流**回报的柜台时间 (交易日 × 86400 + HH:MM:SS 秒; 0 = 未知不记).
    /// 登录重放回报不调用. 供外部单「只在登录重放出现」告警比对 (ADR 0049 外部单假设).
    void note_live_report_time(int64_t report_epoch_s);

    /// 外部单「只在登录重放中出现」告警 (ADR 0049 外部单假设的运行期可观测性):
    /// 重放到达、且柜台时间不晚于最后一条实时回报的外部委托 → ERROR + 限频通知.
    /// 必须在重放过滤器之后调用 (已在基准里的记录不构成假设失效证据).
    void check_replay_external_order(const OrderRecord& rpt);

    /// 用 PositionHolding 单方向状态填充 DzPositionInfo (account/trading_day/seq).
    void fill_position_info(DzPositionInfo& pos, const PositionHolding& h, DzDirection dir);

    /// 推 DZ_FRAME_POSITION_INFO (2002): 分配 seq + 更新方向 seq + 可选持久化单行 upsert.
    void push_position(PositionHolding& h, DzDirection dir, bool persist = true);

    /// is_last 收口: 应用本轮查询累加器 (diff 推帧/零帧/活动委托重灌/PositionRebuild).
    void apply_position_query();

    /// 登录日装载 (契约 td-data-sync §3 / ADR 0045 决策 5): 只装当前交易日的订单/成交,
    /// 初始化 seq 计数器与重放过滤器基准. 失败按登录失败路径处理 (返回 false).
    bool load_trading_day_baseline();

    /// 成交后刷新委托记录（契约 §3：每笔成交且均价变化 → 新 seq + 推帧 + 落库，不依赖柜台顺序）。
    /// 基准无该 order_id（外部单/委托回报尚未到达）→ 只保留累计，等委托回报带走均价。
    /// 均价未变化 → 不推不落不分配 seq。
    void refresh_order_avg(int64_t order_id);

    /// LoadingInstruments 期间缓冲回报 (设计 §5.3)
    void buffer_order_rpt(const OnRtnOrderField& f);
    void buffer_trade_rpt(const OnRtnTradeField& f);
    void replay_buffered_reports();

    /// 发起基准持仓查询链 (登录收尾阶段一, CTP 流控串行): 重置重试预算后开始首次尝试.
    void req_qry_investor_position();
    /// 发起一次基准持仓查询尝试 (首次或重试; 由 req_qry_investor_position 与重试定时器调用).
    void req_qry_investor_position_once();
    /// 一次基准持仓查询尝试失败: 预算内退避重试 (kPositionQueryMaxRetries /
    /// kPositionQueryRetryInterval), 预算用尽则判本次登录失败 —— 停在当前进度
    /// (LoadingInstruments)、不转 Ready、推 ERROR 通知, 且不排任何定时器 (ADR 0049 决策 3).
    ///
    /// 失败判据 (ADR 0049 决策 3; 判定分散在执行侧, 本函数只统一做计数/重试/停住):
    /// - `ReqQryInvestorPosition` 返回非 0 (含 -3 流控, 同样计入预算);
    /// - `is_last` 带 ErrorID;
    /// - 单次尝试超时 (5 分钟);
    /// - 未知方向使整轮快照不可信 (等价「查询结果不可用」).
    /// 「查不到」不是失败: `is_last` 无错且零行 = 成功, 不经过本函数.
    /// @param reason 失败原因 (仅日志用, 不是判据)
    void on_position_query_attempt_failed(std::string_view reason);
    /// 重试预算用尽的通知 (ERROR + popup=true; 文案见 position_query_failed_message).
    void notify_position_query_failed();
    /// 发起资金查询 (登录收尾阶段二; 只在持仓查询成功后发起).
    void req_qry_trading_account();
    /// 持仓查询成功 + 资金查询完结后的统一收尾:
    /// 缓冲重放 -> flush 屏障 -> on_instruments_loaded 转 Ready.
    /// 查询阶段未完成时调用 no-op (防御: 持仓查询重试耗尽后不会走到这里).
    void finalize_login();

    /// 尝试推进登录收尾状态机并执行对应阶段动作; 未达前置时停留.
    void drive_finalizer();

    /// 资金行写入来源（只影响日志，不影响写入语义）。
    enum class TradingAccountWriteSource { kLogin, kRuntime };
    /// 资金行唯一写入点（ADR 0045 决策 4）：持仓基准守卫 + 当日行 upsert + 新 seq +
    /// 推帧 + persist；登录收尾与运行期刷新共用。
    void apply_trading_account_row(const CThostFtdcTradingAccountField& row,
                                   TradingAccountWriteSource source);
    /// 查询通道复位（断线 / 重连 / 登出共用一处）：取消运行期定时器、清在途与请求 id。
    void reset_query_channel();
    /// 运行期资金刷新：事件置脏（仅 Ready；登录缓冲/重放/未就绪不置脏）。
    /// @return true = 已置脏；false = 非 Ready 忽略
    bool mark_fund_refresh_dirty();
    /// 按 lane 的 next_wake 排绝对时间定时器（无轮询；无待查则取消）。
    void schedule_query_lane_pump();
    /// 发送一笔运行期资金查询（经 lane 闸门；成功 note_sent + 10s 超时）。
    void send_runtime_trading_account(QueryKind kind, QueryPacer::TimePoint now);
    /// 运行期查询失败收口：耗尽通知（限频 ERROR, popup=false）+ 安排下次退避。
    void handle_runtime_query_failure(QueryKind kind);
    /// 失败通知限频（每账户 5 分钟，单调时钟）。
    void notify_runtime_fund_failure();
    /// 10s 超时收口（生产由 TimerQueue 回调；测试经 `expire_runtime_query_timeout_for_test`）。
    void on_runtime_query_timeout(int request_id, QueryKind kind);
    void cancel_runtime_query_timeout();
    [[nodiscard]] QueryPacer::TimePoint query_now() const;

    /// 查询发送闸门（ADR 0055 决策 1）：pacer 不允许立即发送时，把 retry 用 TimerQueue
    /// 延后到最早合法时刻（绝对时间，无轮询），返回 true 表示已延后（调用方应 return）。
    /// 回调带代际/存活检查，断线重登后自动失效。
    bool defer_query_if_pacer_busy(const std::function<void()>& retry, std::string_view query_name);
    /// 查询已被 CTP 接受后登记在途（与 `note_query_done` 配对）。
    void note_query_sent();
    /// 查询收口（响应 / 失败 / 超时）退出在途；幂等。
    void note_query_done();

    // === 成员 ===
    std::string account_id_;
    shm::OrderIdMeta& order_id_meta_;
    shm::MultiWriter& event_writer_;
    PersistWriter& persist_writer_;
    MpmcQueuePtr event_queue_;
    dztrader::core::TimerQueue& timer_queue_;
    /// 账户状态通知回调 (可空; 会话内直调状态机的路径经它推送 2018, 契约 account-status)
    std::function<void(DzAccountState)> account_state_cb_;
    /// 账户级 UI 通知回调 (可空; 登录失败原因 / 登录 flush 屏障超时 ERROR 经它转 UI,
    /// 空则仅日志)
    AccountNotifyFn notify_;

    CThostFtdcTraderApi* api_ = nullptr;
    std::unique_ptr<TdSpi> spi_;

    TdStateMachine state_machine_;
    RiskGate risk_gate_;
    OrderRefMap order_ref_map_;
    int64_t order_ref_ = 0;
    int32_t request_id_ = 0;
    int32_t trading_day_ = 0;  ///< 当前交易日 (DzDate, 距纪元天数)
    /// 跨交易日登出请求 (ADR 0049 决策 5): 回报判死后置位, 由 TdApi::dispatch 在本次事件
    /// 处理完后取走并主动登出; 单次语义 + 首次通知 (见 take_cross_day_logout_request).
    bool cross_day_logout_requested_ = false;

    /// 账户级状态变更 seq 计数器 (spec §2.2: 主线程分配, 无需原子).
    /// 跨日累积单调, 同一事件 shm 帧与 DB 行带同一 seq.
    uint64_t seq_counter_ = 0;
    /// 未知枚举累计计数 (按种类; 见 UnknownEnumCounts). 由 report_unknown_enums 递增.
    UnknownEnumCounts unknown_enum_counts_;
    /// 未知枚举「通知」的最小间隔 (5 分钟, 与 docs/components/dztd_ctp.md
    /// 通知分级的持续性异常限频同量级). 间隔内的未知枚举只计数与记日志.
    static constexpr int64_t kUnknownEnumReportIntervalS = 300;
    /// 上次通知的时刻 (steady_clock 秒; 0 = 尚未发生过).
    int64_t unknown_enum_last_report_s_ = 0;
    /// 最近一条**实时流**回报的柜台时间 (交易日 × 86400 + HH:MM:SS 秒; 0 = 尚未观测).
    /// 跨登录重连保留 (会话对象不变); 登录重放回报不更新它. 外部单「只在登录重放出现」的
    /// 判据基准 —— 晚于它的无法判定 (可能是断线窗口内别的终端下的单), 不告警 (ADR 0049).
    int64_t last_live_report_epoch_s_ = 0;
    /// 外部单「只在登录重放出现」告警的通知限频窗口 (steady_clock 秒; 0 = 尚未告警过).
    int64_t replay_external_last_alert_s_ = 0;
    /// 该告警命中的累计笔数 (日志/通知取证).
    int64_t replay_external_missed_count_ = 0;
    /// 告警通知最小间隔 (5 分钟, 与未知枚举同量级; 假设失效时重放会成批命中, 逐笔通知会刷屏).
    static constexpr int64_t kReplayExternalAlertIntervalS = 300;
    /// 重放过滤器基准 (登录/重连时 CTP 私有流去重, spec §4.1).
    std::unique_ptr<ReportFilter> report_filter_;
    /// 每单成交累计 (委托成交均价的唯一累源; 登录日装载时由当日成交重建, 契约 §3).
    OrderTradeAvgTracker order_avgs_;
    /// 持仓查询累加器 (同一 (instrument, direction) 多行合并; 仅主线程).
    struct PositionQueryKey {
        std::string instrument_id;
        int8_t direction = 0;
        bool operator==(const PositionQueryKey&) const = default;
    };
    struct PositionQueryKeyHash {
        size_t operator()(const PositionQueryKey& k) const noexcept {
            size_t h = std::hash<std::string>{}(k.instrument_id);
            h ^= static_cast<size_t>(static_cast<uint8_t>(k.direction)) + 0x9e3779b9u + (h << 6) +
                 (h >> 2);
            return h;
        }
    };
    struct PositionQueryAgg {
        std::string exchange_id;  // 查询行自带 (holding 交易所回退来源)
        int64_t volume = 0;
        int64_t yd = 0;
        double cost = 0.0;
        double volume_multiple = 0.0;  // 合约乘数 (均价换算: cost / (volume * multiple))
        int64_t ctp_frozen = 0;        // LongFrozen+ShortFrozen, 仅冻结对账 WARN
    };
    std::unordered_map<PositionQueryKey, PositionQueryAgg, PositionQueryKeyHash>
        position_query_agg_;
    /// 基准就绪: 首个成功查询应用后置位; 断线清空. 无周期查询, 只在登录链重建.
    bool position_baseline_ready_ = false;
    /// 持仓查询在途 (同一时刻只允许一条查询链: CTP 在途查询流控只有 1 笔).
    bool position_query_in_flight_ = false;
    /// 每请求 token: 超时定时器只对其对应的查询生效 (防误清新查询的 in_flight).
    uint64_t position_query_token_ = 0;
    /// 当前持仓查询的 CTP request_id: 迟到响应 (超时后被下一轮取代) 据此丢弃.
    int position_query_request_id_ = 0;
    /// 本轮持仓查询明细组 (is_last 时由累加器物化, 供 PositionRebuild 重灌, spec §3.2).
    std::vector<DzPositionInfo> position_query_group_;
    /// 本轮是否已 enqueue PositionRebuild (幂等防御: 迟到的重复 is_last 不得用已消费的
    /// 空组再次重灌清空 DB).
    bool position_rebuild_consumed_ = false;
    /// 本轮持仓查询出现过读不懂的方向: 整轮快照不可信, is_last 时按查询失败处置
    /// (不改表、保留上次已知快照).
    bool position_query_unknown_direction_ = false;
    /// 登录收尾状态机 (spec §4.2): 持仓查询成功 + 资金查询完结才可收尾;
    /// 持仓查询重试耗尽 → 失败终态, 不转 Ready (ADR 0049 决策 3).
    LoginFinalizer finalizer_;
    /// 最近一轮持仓查询是否成功 (未知方向使整轮快照不可信时置 false; 资金行守卫读它).
    bool position_query_ok_ = false;
    /// 基准查询已重试次数 (登录/重连链起点清零; 预算见 kPositionQueryMaxRetries).
    int position_query_retries_ = 0;
    /// 独立只读连接提供器 (TdApi 注入; 登录日装载用, 可空则装载失败).
    std::function<SQLite::Database*()> read_db_provider_;
    /// 无会话拒单 seq 分配器 (可空; 登录日装载据此抬高 seq 计数器).
    AccountSeqAllocator* seq_allocator_ = nullptr;

    /// 持仓 map: instrument_id -> PositionHolding (设计 §6)
    std::unordered_map<std::string, PositionHolding> holdings_;

    /// 合约 -> 交易所/最小变动价位 (设计 §7.2, 由 on_rsp_qry_instrument 填充).
    /// place_order 时查表获取 exchange_id, 未命中则拒单; price_tick 供 Task 7 用.
    /// on_front_disconnected 清空 (重连后重新查询).
    struct InstrumentBrief {
        std::string exchange_id;
        double price_tick = 0.0;
        double volume_multiple = 0.0;                       ///< 合约乘数 (PositionCost -> 均价换算)
        DzProductClass product_class = DZ_PRODUCT_UNKNOWN;  ///< 套利/组合判定 (ADR 0049)
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
    /// 结算单确认看门狗定时器 id (#488; kSettlementConfirmTimeout, 收到响应后取消)
    dztrader::core::TimerQueue::TimerId settlement_confirm_timer_id_ = 0;
    /// I2: 合约加载超时定时器 id (5 分钟, 设计 §2.4.1 流控持续 -3 超时)
    dztrader::core::TimerQueue::TimerId instruments_load_timer_id_ = 0;
    /// 基准持仓查询重试定时器 id (失败后 3s 重试; 0 = 无挂起)
    dztrader::core::TimerQueue::TimerId position_query_retry_timer_id_ = 0;
    /// 基准持仓查询单次尝试的超时定时器 id (5 分钟; 响应/失败后取消)
    dztrader::core::TimerQueue::TimerId position_query_timeout_timer_id_ = 0;
    /// 会话存活令牌: 未跟踪的查询链定时器回调先检查它, 防 logout erase 后 UAF.
    std::shared_ptr<void> alive_token_ = std::make_shared<int>(0);
    /// 代际失效: 断线时自增, 使已挂起定时器回调失效 (避免陈旧回调误触发)
    uint64_t generation_ = 0;
    /// 查询链代际 (终检发现 1): 每次查询发起时快照 generation_, 响应处理校验
    /// query_gen_ == generation_ 才继续 — 断连重连后旧会话迟到的查询响应 (含
    /// 数据行与 is_last) 一律丢弃, 防旧响应污染新链 (重灌错行/重复收尾/seq 污染),
    /// 彻底覆盖 "查询在途断连重连" 交错。
    uint64_t query_gen_ = 0;

    /// CTP 查询闸门（ADR 0055 决策 1）：登录链三查询与运行期资金刷新共用
    /// 「同一时刻至多 1 笔在途 + 任意两笔发送间隔 >= 最小间隔」。
    QueryPacer query_pacer_;
    /// 运行期按需查询调度（ADR 0055 决策 2；#458 接入触发源，当前只建类与单测）。
    QueryLane query_lane_;
    /// 各查询链在途请求 id（OnRspError 路由 + 响应关联；0 = 未在途）。
    int instrument_query_request_id_ = 0;
    int account_query_request_id_ = 0;
    /// 运行期资金查询在途 request id（0 = 未在途；与登录链共用请求计数器）。
    int runtime_account_request_id_ = 0;
    /// 运行期资金查询 10s 超时定时器 id。
    dztrader::core::TimerQueue::TimerId runtime_account_timeout_timer_id_ = 0;
    /// lane 唤醒定时器 id（tag 去重，绝对时间）。
    dztrader::core::TimerQueue::TimerId query_lane_timer_id_ = 0;
    /// 已排 lane 定时器的目标时刻（用于热路径避免每笔回报重排同一定时器）。
    QueryPacer::TimePoint query_lane_timer_deadline_{};
    /// 运行期失败通知上次发出时刻（单调秒；-1 = 尚未发过）。
    int64_t runtime_failure_notify_last_s_ = -1;
    /// 测试注入（空 = 生产实现）。
    QuerySendFn query_send_hook_;
    QueryClockFn query_clock_;
    SettlementSendFn settlement_send_hook_;
    InstrumentSendFn instrument_send_hook_;
    AuthSendFn auth_send_hook_;
    LoginSendFn login_send_hook_;
    /// 认证超时时长（生产 = 10s；测试可缩短）。
    std::chrono::seconds auth_timeout_ = std::chrono::seconds(10);
    /// 登录超时时长（生产 = 10s；测试可缩短）。
    std::chrono::seconds login_timeout_ = std::chrono::seconds(10);
    /// 结算单确认看门狗时长（生产 = kSettlementConfirmTimeout；测试可缩短）。
    std::chrono::seconds settlement_confirm_timeout_ = kSettlementConfirmTimeout;
    /// 合约加载超时时长（生产 = 5 分钟；测试可缩短）。
    std::chrono::seconds instruments_load_timeout_ = std::chrono::minutes(5);
    /// 结算单确认在途 request id（0 = 未在途）。响应侧校验它, 丢弃旧链接的迟到响应。
    int settlement_confirm_request_id_ = 0;

    /// CTP 登录诊断字段 (open 时保存, 用于 ReqAuthenticate/ReqUserLogin)
    std::string broker_id_;
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
