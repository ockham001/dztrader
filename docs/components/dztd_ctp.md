# dztd_ctp（交易网关）架构设计

本文档描述 dztd_ctp 交易进程的架构设计与核心机制。要点：**一进程管理多账户**（每账户一组 CTP API + SPI），仅事件通道（无行情通道），登录流程含认证 / 结算确认 / 合约加载，查询受 CTP 流控约束。

## 并发与调度

### 线程划分

**主线程**：承载全部逻辑——处理各账户 SPI 转发的事件、处理 event 通道帧、驱动定时任务，集中管理全部账户会话。

**账户 SPI 回调线程**（CTP SDK 内部，每账户一个）：交易回调（`OnRtnOrder` / `OnRtnTrade` / `OnRspQry*` 等）打包入共享事件队列，转发主线程。N 个账户即 N 个 SPI 线程，均写入同一队列。

### 事件汇入：单队列 + 单信号量

- 进程内只创建一个命名信号量，共享事件队列和 event 通道共用它；主线程一次唤醒即可同时响应两类事件源，定时调度复用它的超时等待。
- 共享事件队列为**多写者单读者**模型：N 个账户 SPI 线程写入（进程内互斥锁保护），主线程独占读取。
- 实时写入优先于后台维护：两者竞争时，实时写入优先，后台维护不得长时间占用。

### 定时调度机制

- 定时器队列是**定时调度的核心宿主**，承载全部延时 / 周期任务（固定时间点、固定时间间隔），如自动登录 / 登出排程（契约 auto-login）。
- 复用主循环信号量的超时等待按点唤醒，无独立线程。
- 无到期定时器时不设超时，仅由事件唤醒，CPU 接近零。
- 所有定时任务共享同一宿主机制，不各自开辟独立线程或单独轮询。

## SHM 通道

仅事件通道，无行情通道。

### event 通道：多写者模型

- 写入端由**进程锁（崩溃安全）**保护跨进程写入互斥。
- 进程内读取与写入均由主线程独占，读取端无锁；跨进程读写安全由通道内建发布机制保证：帧完整写入后才原子发布写位置，读取只消费已发布的帧。
- 维护为**被动响应**：按到达帧处理，无独立定时维护。
- **被动预加载**：收到 `PRELOAD_EVENT_SHM` 广播后执行。

## 会话与账户

### 多账户会话模型

- 一进程多账户：每账户一个 CTP Trader API + SPI + `AccountSession`（持有 `TdStateMachine` 独立状态机实例与 `account_id`）。
- **账户隔离**：账户级 `instance_id` = 账户 ID（全局唯一，跨全部接口类型，总则 §5），不携带进程名；状态、进度、回报按账户隔离，单账户失败不影响其他账户。
- 主线程集中管理全部账户会话，状态机不依赖 CTP SDK、可独立测试。

### 登录流程

连接 → 认证（`ReqAuthenticate`）→ 登录（`ReqUserLogin`）→ 结算确认（`ReqSettlementInfoConfirm`）→ 合约加载（`ReqQryInstrument`）→ Ready。状态保护：仅特定状态可发起下一步；认证 / 结算确认失败回退重试，合约加载受流控约束重试。

- 本进程不发起登出：UI 登出即彻底释放 CTP SDK API（直接断开），回 Idle；不存在"已登出、连接保留"的稳定状态。
- 例外：若收到服务器主动登出的回调，状态已偏离预期，属异常情况，非本进程主动为之。此时保留前置连接，由自动调度重新登录恢复，并记录错误日志、上报告警，供排查。

### 登录日装载（重放基准）

登录成功（含盘中重连、删户重加、盘中换柜台）后、处理首条重放回报之前，从交易库装载**当前交易日**作为重放基准（`apps/ctp/td/td_day_load.{h,cpp}`；ADR 0045 决策 5；《帧契约：TD 数据同步》§3）：

- **只装当日**：订单最新态（`order_ref` / OrderSysID → 既有 `order_id` 的匹配基准，CTP 扩展字段从 `ext` JSON 恢复）与当日成交（去重键基准）。**不装历史**——历史委托 / 成交只在交易库，内存里不留（全史预扫已在 #246 删除）。
- **seq 播种**：`seq_counter_ = max(当日四表 MAX(seq), 无会话取号器已分配值)`。四表共享取号、跨日累积，`MAX(seq)` 不带日过滤。无会话取号器（`AccountSeqAllocator`）每次取号都吸收库内进展，避免同账户先有会话推进 seq 后旧种子撞号。
- **失败即登录失败**：装载失败（只读连接不可用 / 查询异常）按登录失败路径处理（`on_login_failed`，状态机回退待重试），不带着空基准进入重放——空基准会把整段重放当成新数据，重复落库与重复推帧。
- **主线程同步装载**：登录频率低、当日行量级小，同步装载消除"装载 / 重放"竞态；热路径不查库（读连接归装载与只读查询方，写归 Writer 线程）。
- **内存只保留当日**：订单最新态、成交去重键、**每单成交累计（均价）**、OrderRef 映射、当前持仓 / 资金。历史增长不影响进程内存。
- **均价基准重建**：装载当日成交后按 `order_id` 聚合 `Σ价×量 / Σ量` 播种 `OrderTradeAvgTracker`，并用该聚合值覆盖装载委托的 `avg_price`（当日无成交的委托保留库内值），见下节「成交均价」。
- 重放过滤器对比命中时回填 `order_ref` / OrderSysID 映射（重启后内存表为空，老本地单若不回填会被误判为外部单）。

### 成交均价

成交均价（`ORDER_REPORT.avg_price` / `orders.avg_price`）是**派生字段**：柜台（CTP）不提供，由本进程从成交回报实时累计（`OrderTradeAvgTracker`，`Σ价×量 / Σ量`，double 全精度；《帧契约：TD 数据同步》§3）：

- **顺序无关**：每笔成交先累计再判断是否刷新委托行——成交先于委托回报到达时只累计，委托回报到达时带走累计值（`to_order_record` 本身不填 `avg_price`，由会话在本进程内回填）；委托回报尚未到达的成交不会凭空生成委托行。
- **每笔成交刷新委托行**：累计均价相对基准行**变化**时，用基准行的其余字段（`update_time` / `volume_traded` / `volume_canceled` / `status` 保持基准值）+ 新 `avg_price` + **新 `seq`** 生成一条完整 `ORDER_REPORT` 推帧并落库；均价未变化不推、不落、不分配 seq。保持其余字段不变是为了让后续重放对比仍判「相同」而被吞，不会把同一条柜台回报重复落库。**「变化」判定带浮点容差**（`avg_price_unchanged`）：累计值每次由 `Σ价×量 / Σ量` 全量重算，同价多笔成交会有 1 ULP 级漂移（例如 `3512.3` 三笔后为 `3512.3000000000006`），裸相等会把「未变化」误判成变化、多推帧并多取 seq；容差只用于该判定，落库与推帧仍写全精度累计值。
- **无成交语义**：帧 / DB 值为 0；REST / WS JSON 为 `null`（序列化与显示精度是视图层语义，ADR 0043）。拒单与无会话拒单恒无成交，`avg_price = 0`。
- **跨日 GFD 委托的已知限制**：累计只含当前会话交易日的成交——登录日装载只装当日（契约 §3 明文），日切时累计基准清空重建，昨日成交部分不回填——跨日未成交完的委托在日切后只反映当日段的均价，直到该单再次成交。历史回看页（后续）按 `trades` 重算不受此限。
- **对统计计数器的已知窗口**：均价变化时产生的刷新 `ORDER_REPORT` 在委托已终态后仍会到达，dzweb 统计计数器会把它当新委托计一次（+1 笔 +volume），≤1 个对账周期（30s）自动纠正（契约 rest §2.9.5 的「已知精度边界（成交均价刷新帧）」与 `dzweb.md`）。

### 登录后自动查询

登录成功后自动发起基础查询（资金、持仓等），供 UI 与策略初始数据。查询受 CTP 流控约束，需排队调度；流控窗口、批次、失败回滚等调度细节属实现。持仓的基准建立、盘中增量维护与周期重查见《持仓维护与偏移转换》。

## 交易委托与回报

- 接收策略广播的 `TD_ORDER_REQ` / `TD_ORDER_CANCEL_REQ`（basic 帧，按 payload `account_id` 归属过滤，见《帧契约：交易委托请求》）。
- 委托 / 成交回报（`OnRtnOrder` / `OnRtnTrade`）经 event 通道广播（`ORDER_REPORT` / `TRADE_REPORT`），供策略与前端消费；每笔状态变化按业务键 upsert 交易库，帧与 DB 行同 `seq`（《帧契约：TD 数据同步》§1）。
- **成交去重键**：`(account_id, date, exchange_id, trade_id)`——`trade_id` 仅交易所内唯一，含 `exchange_id` 才不丢多交易所同号成交。
- **成交均价刷新**：每笔成交更新的累计均价若变化，同一委托会额外产生一条 `ORDER_REPORT`（新 `seq`、委托行同 `seq`）；消费方按业务键 + `seq` 合并即为幂等（见上节「成交均价」）。
- **无会话拒单**（#13）：账户已配置但当前无运行会话（从未连接 / 连接前窗口）时下单，仍分配账户级 `seq`、落库并推 `ORDER_REPORT`（`REJECTED`），否则 `seq=0` 会被消费端水位当作"快照已含"静默吞掉。**已知限制**：此时没有会话交易日可用，`date` 取本地自然日、`time` 取本地时钟秒——与真实交易日可能相差一天（夜盘跨越午夜时），且该行不参与当日登录日装载；后续由 `ReqQryOrder` 对账或历史回看页校正。

## 持仓维护与偏移转换

持仓在进程内**自维护**：聚合查询建基准 + 成交/委托回报增量更新（决策见 [ADR 0008](../adr/0008-td-position-model.md)），不复用 CTP 明细查询。

- **模型**（`apps/ctp/td/td_position.{h,cpp}`，纯 C++ 内部结构，不进公开头/SHM）：按合约 `PositionHolding` 维护多/空两侧 `PositionSide {today, yd, price, frozen_td, frozen_yd, seq}`（`volume = today + yd` 派生）与活动平仓挂单表（按合约隔离）。任一业务字段变化才取新 seq 推 2002 + `Kind::Position` upsert，未变化不推不落；原 `PositionMirror` 删除（差异/seq 职责并入 `PositionSide`）。`DzPositionDetail` 与 `ReqQryInvestorPositionDetail` 链路整体移除。
- **查询建基准**：`ReqQryInvestorPosition` 响应按 (instrument, direction) 多行合并，`is_last` 一次性应用（volume/yd/price 以快照为准；frozen 不由快照覆盖）。CTP `PositionCost` 为金额，均价按 `均价 = PositionCost / (持仓量 × 合约乘数)` 换算（合约乘数取自合约查询映射，缺乘数时均价留 0）。差异侧推 2002 + `Kind::Position` upsert，消失 side 发 volume=0 帧（只推零帧、不 upsert，行由重灌 DELETE 兜底）；基准未就绪时的首个成功查询强制 `PositionRebuild` 重灌，已就绪后的查询（含补查重入与周期查询）仅在存在差异/消失时重灌。基准就绪标志独立于每轮查询成功标志（周期查询单次失败不得停用增量应用）；就绪前成交/委托的持仓增量一律不应用，首个成功查询统一重建。
- **成交增量**：OPEN 增今仓、均价加权；平今/平昨按标志；非 SHFE generic CLOSE 今→昨溢出，SHFE generic CLOSE 减昨；量归零时均价清零，异常负值夹到 0 + WARN（下轮查询校正）。登录缓冲重放阶段（kReplay）只应用委托状态、跳过成交，避免与查询快照双重计数。
- **冻结**：只由活动平仓挂单（含外部单）本地推导 `volume - volume_traded`，今仓优先、溢出到昨、按量夹取；拒绝/撤单/全成释放。登录种入按当前交易日过滤，CTP 冻结总量不符仅 WARN。
- **日切**：today→yd、清挂单冻结，变化侧按上规则推帧/落库，下一轮查询权威校正。
- **漂移自愈**：Ready 后每 `qry_position_interval_s`（默认 60s，可配置）重查一次（统一 TimerQueue 定时，不另起轮询线程，符合"无轮询"原则）；查询在途窗口的成交以快照覆盖为准，≤ 一个查询间隔收敛（近似语义见《帧契约：TD 数据同步》）。周期查询的失败处理与登录收尾链解耦：仅记日志/等下一轮，不推进 finalizer、不触发资金查询；登录查询失败期的补查沿用既有固定 60s 降级路径。
- **下单**：本波不接入 OffsetConverter（SHFE 拆分/AUTO/父子单聚合回报留待专项），`DZ_POSITION_EFFECT_AUTO` 显式拒绝；风控上下文填入该合约真实多空持仓与合约 `price_tick`（现规则实际使用 `price_tick`）。

### 快照生命周期（#249 锁定的行为）

下列行为由 `apps/ctp/tests/td_persist_writer_test.cpp`（写入器层）、`td_data_sync_integration_test.cpp`（Writer 时序 + 收敛窗）与 `td_position_lifecycle_test.cpp`（**会话层**：真 `AccountSession` + 真 SQLite + 真 persist 线程）断言**库内容**覆盖，是《帧契约：TD 数据同步》§3 / §7 与 ADR 0045 / ADR 0046 的可执行形态（改坏任一条应红）：

- **登录不清零、不预置 0**：新会话登录后、首个完整成功持仓查询之前，`positions` 当日**没有行**，上一次已知快照（前一日行，含跨会话 / 删户重加 / 换柜台）原样保留；不写 `volume=0` 占位行。
- **查询失败不改表**：持仓查询 `is_last` 带 `ErrorID` 时，当日既有行的 `seq` 与内容完全不变（既不删也不改），资金行同样不写——锚点日停在上一成功日（ADR 0046）。
- **整组原子替换的作用域 = 该账户 + 当日**：完整成功查询在单事务内删该账户当日行 + 写快照组；其他账户的当日行、同账户的历史交易日行都不动。响应里消失的合约行被删除（不产生幽灵持仓）。
- **已就绪后的周期查询无差异不重建**：`position_baseline_ready_` 之后，无差异且无消失的一轮查询不产生 `PositionRebuild`（DB 各行的 `seq` 不变，即无新写入）。
- **`volume=0` 增量可查**：全平时日内增量走单行 upsert，落库是**存在的一行 `volume=0`**（不是跳过、不是删行）——「当日全平」与「从未成功查询过」因此在库里可区分。
- **日切 today→yd 只碰当日行**：日切后当日行落成 `today_volume=0 / yd_volume=原今仓`，上一交易日的行保持它自己当时的今昨拆分（历史行不可变）。
- **删户重加 / 盘中换柜台**：同一 `account_id` 二次登录后再次整组重建，当日结果是**整组替换**——无重复行（唯一键）、不残留上一轮独有的合约、历史交易日行不变。

> **测试边界（`td_position_lifecycle_test`）**：该目标驱动的是**持仓查询响应分支**（`on_rsp_qry_investor_position → apply_position_query`），不覆盖「登录链本身」（合约加载 → 结算确认 → 收尾 → Ready）——那条链在 `req_settlement_confirm` 里无条件解引用 `api_`，而 `api_` 只能由 `open()` 创建真实 `CThostFtdcTraderApi`（需连柜台）。登录链的 flush/Ready 语义由 `td_login_finalize_test`（纯逻辑）与 `td_data_sync_integration_test`（Writer 时序）覆盖。两条合起来即完整链路，不重复也不留缺口。

### 已知限制（持仓）

- **今昨冻结未拆分落库**：**schema v1 的 `positions` 只有 `frozen_volume` 单列**。td 内存里冻结是 `frozen_td` / `frozen_yd` 拆分维护的（今仓优先、溢出到昨），但落库与 `POSITION_INFO` 帧只带合计值；读端（REST / WebUI）只看得到总冻结，无法分别展示今/昨冻结。窗口 = 本轮 schema。
- **旧会话残留写入的有界收敛窗**：跨进程旧会话已入队未提交的 persist 任务可能晚于新会话整组替换落地 → 当日持仓短暂回到旧值，由下一次完整成功查询收敛，**上界 = 一个 `qry_position_interval_s`（默认 60s）**；**不丢行、不产生键冲突**（不变量与理由见《帧契约：TD 数据同步》§7 与 ADR 0045 决策 7）。
- 其余持仓准确性边界（查询在途窗口的成交以快照覆盖为准、其他终端交易的收敛期、资金盘中不刷新）见《帧契约：TD 数据同步》§7。

### 资金快照（#250 锁定的行为）

资金查询走与持仓同一条登录收尾链：`on_rsp_qry_investor_position` 收口 → 发起 `ReqQryTradingAccount` → `on_rsp_qry_trading_account` 写库/推帧（`DZ_FRAME_TRADING_ACCOUNT`）。行为由 `td_position_lifecycle_test.cpp`（会话层）、`td_persist_writer_test.cpp`（写入器层）断言库内容：

- **按 `(account_id, date)` 多日累积**：每个交易日一行最新快照；同账户同日重复查询是 upsert（覆盖余额类列与 `seq`，不追加行），历史交易日行不被改写（`SameDayFundQueryUpsertsInsteadOfAppending` / `DaySwitchFundQueriesCoexistAsTwoDayRows`）。
- **资金行只在持仓查询成功后写入**：`on_rsp_qry_trading_account` 的唯一写入点有 `position_query_ok_` 守卫——持仓查询失败时资金行不落库、不推帧，锚点日（两张快照表 `MAX(date)`，ADR 0046）不会被资金行单独推进。
- **盘中不做周期资金查询**（已知限制，契约 td-data-sync §7）：资金查询只有登录收尾链与失败补查链（`resync_account_data`，持仓成功后按同序重查）两处触发，盘中停在最近一次查询结果；其他终端交易造成的资金变化不收敛到下一次登录。

## 配置（契约 td-config / td-status）

配置与状态 payload 都是**通用层 + 柜台 `ext` 分层**：新增接口类型只换 `ext`，通用层（`account_id` / `enabled` / `risk_control_enabled` / `currency_id`；状态帧的 `state` / `date` / `login_time` / `progress_*`）不变。类型层真相源 `apps/ctp/td/td_config.h` 与 `td_state.h`，JSON schema 以契约为唯一语义真相源。

### 配置结构与持久化

- **文件形态 = 帧形态**：`td` section 与 `RTN_TD_CONFIG` payload 同形（顶层 `qry_account_interval_s` / `qry_position_interval_s` / `qry_flush_interval_ms` / `ext` / `accounts[]`；账户 `ext` 内是 `broker_id` / `user_id` / `password` / `product_info` / `frontends` / `auth_code` / `app_id` / `flow_dir`）。不做「文件旧形 / 帧新形」双序列化——同一事实两处定义必然漂移。代价：**旧扁平格式（连接参数直接在账户对象上）不再兼容**，开发期无迁移义务。
- **启动加载两级语义**（契约 td-config §6）：文件 / `td` section **缺失** → 用默认值 + 原子修复文件（自愈，首次启动的正常形态，写失败只记 ERROR 不阻断）；内容**非法**（JSON 解析失败 / section 类型不符）→ 记 CRITICAL 后**退出非零码**，不回落默认值。理由：账户连接参数是运维资产，静默清空比启动失败更危险，且自愈后的空账户集无法与"本来就空"区分。旧扁平格式因未知字段被忽略（总则 §8）不会触发解码失败，但账户 `ext` 为空 → `validate()` 以 `broker_id empty` 拒绝 → 同样致命退出。
- **启动主动推一次**：`run()` 首次进入即 `report_full_snapshot()`（首帧 `report_config()`），供 dzweb 账户注册表启动快照（契约 td-config §4 场景 2）。配置在 `td_main` 中先于 `TdApi` 构造加载，推的是已加载的当前值。
- **op 收敛**：进程级柜台特有配置统一走 `SetExt`（字段级补丁），旧 `SetLockMode` 已删除，不留双实现；`ext.enable_lock_mode` 是本进程唯一形态。账户级 op（`AddAccount` / `UpdateAccount` / `SetAccountEnabled` / `SetAccountRiskControl` / `SetAccountCurrency`）与 `SetQryIntervals` 语义见契约 §2.1。
- **状态保护**：`UpdateAccount` / `RemoveAccount` 在账户有活跃会话时拒绝（连接参数不可热改；删除时销毁会话并推 Offline，不删交易数据，ADR 0045）；其余 op 不受状态保护。
- **失败四件套**（总则 §8）：回滚旧值 + 日志 + `NOTIFY_UI` 错误弹窗 + 回 `RTN_TD_CONFIG` 旧值，清前端 pending。
- **唯一性边界**：本进程只查自身配置文件内的 `account_id` 重复，**不是**跨网关防线——跨网关唯一性由 dzweb 权威校验（契约 td-config §5，ADR 0045）。

### 状态上报

- 账户级细状态 `RTN_STATUS` 按 `instance_id = <网关名>:<account_id>` 推送（总则 §5 复合键例外），该复合键只用于帧路由与 dzweb 镜像分区，**账户身份仍是 `account_id`**。
- `date` 是**会话交易日**（登录响应确定），断线（`Disconnected`，SDK 自动重连中）期间不清空——它是本交易日全部业务数据的时间锚；进入 `Idle` 才清 `date` / `login_time` / `ext.sys_version`（`ext.api_version` 是进程生命周期常量，保留）。
- 日切（会话交易日变化，含日切后重新登录）以新 `date` 全量推送；前端以字段值变化为准，不自行推断清空时机。

## 持久化（trade.db）

交易数据（委托 / 成交 / 持仓 / 资金，以及暂留的合约字典）由本进程异步写入 `$DZTRADER_HOME/db/trade.db`。**`trade.db` 不是交易真相源**（真相在柜台回报与 td 内存态）：持久化失败只降级，不阻塞交易、帧推送与 WebSocket。schema 与业务键见 [ADR 0041](../adr/0041-sqlite-only-trade-store-schema-v1.md)，失败策略见 [ADR 0042](../adr/0042-persist-failure-backpressure-and-retry.md)，账户快照生命周期见 [ADR 0045](../adr/0045-trading-account-identity-lifecycle-and-accuracy.md)。

### 写入模型

- **进程级单 Writer 线程**（`apps/ctp/td/td_persist_writer.{h,cpp}`）：本进程全部账户共用同一任务队列与同一 Writer 线程；主线程只做内存入队，Writer 线程独占 SQLite 连接与预编译语句并串行提交——单写者按序提交是账户级 `seq` 前缀不变量的前提（《帧契约：TD 数据同步》§2）。
- **任务类型**：委托 / 成交 / 合约字典 / 持仓单行 upsert / 资金单行 upsert / 持仓整组重建 / flush 哨兵。行的 upsert 由 tdstore 表声明单源生成幂等 SQL（同键覆盖最新态）；`PositionRebuild` 在批内**原位**先 `DELETE` 该账户当日持仓行再写入本组，单事务原子替换（只碰当日行，历史交易日行不可变，ADR 0045）。业务侧何时产生哪种任务（持仓基准重建、日切等）见「持仓维护与偏移转换」。
- **建连**：首次建连（`open()`）与 Writer 线程重连都经 `tdstore::ensure_trade_db` 幂等门面（建库职责归 master，td 侧兜底：缺失库补建 / 旧版本 / 坏库改名重建），随后以读写方式打开并设 `busy_timeout=10s`、WAL、`synchronous=FULL`、缓存 8MB、`temp_store=memory`；WAL 切换遇锁视为可重试的建连失败。`open()` 失败不抛异常（只记 ERROR + 通知），由 Writer 线程按退避重连（ADR 0042 决策 3）。
- **schema v1 无迁移链**：DDL 由 tdstore 表声明单源生成，`user_version` 不符或库损坏即旧库改名保留 + 新建空库（旧 dev 库、旧 `td.db` 直接丢弃，ADR 0041 决策 5）。行内容 = 帧业务字段 + `seq` + `gateway`（写入来源 td 实例名，仅审计，不进唯一键）+ `ext`（柜台差异 JSON）；`date` 存 `YYYYMMDD`、时间为交易所真实时钟秒。

### 提交、积压与重试

- **单事务批提交**：Writer 线程把队列排空为一批，按任务原始序在单事务内执行；**提交成功才出队**并放行批内 flush 哨兵。失败则整批回滚、批保留在 Writer 线程，按退避序列主动重试**同一批**：1s → 2s → 5s → 10s → 30s（末值封顶），不依赖下一次业务事件触发（ADR 0042 决策 2）。
- **队列不设上限、入队不阻塞**：调用线程只做内存入队；积压达阈值（默认 10 万条）时发一次 WARN 通知（不弹窗）+ 日志，回落到阈值一半以下后重新武装（迟滞）。这是内存兜底告警，不是对交易链路的背压（ADR 0042 决策 1）。
- **重试期间新任务继续入队**，FIFO 不被破坏；DB 修复后自动追平。打开失败、锁等待、磁盘满等连接 / 执行错误走同一退避重试路径；`busy_timeout=10s` 吸收与并发只读连接（dzweb / SDK）的锁等待（ADR 0042 决策 3；《帧契约：TD 数据同步》§5）。

### 坏库重建

- 建库/写事务命中主 result code `SQLITE_CORRUPT`（11）/ `SQLITE_NOTADB`（26）（覆盖文件头非法、非 SQLite 文件）→ 关闭连接，由下一轮建连经 `ensure_trade_db` 重建：旧库（含 `-wal` / `-shm` 伴生文件）改名 `.old-<timestamp>` 保留 + 新建空库，记 ERROR 并通知；**不在同一坏文件上无限重试**（ADR 0042 决策 3）。
- `SQLITE_BUSY` / `SQLITE_LOCKED` 属锁竞争（可重试），**不**判定为损坏库——避免并发读者持锁时把健康库改名；建库门面探测遇 busy / locked 抛可重试错误。
- 同一批连续损坏重建设有上限（当前 3 次），达上限后只退避重试，防止坏盘无限改名（`.old-*` 副本风暴）。
- 重建后交易路径照常可用；重建前的历史查询缺口由备份 / 归档兜底（本轮不实现）。

### 通知分级

只定义通知语义，不新增持久化状态字段 / 常驻横幅（ADR 0042 决策 4）：

- **首次失败**：ERROR + 弹窗 1 次；
- **持续失败**：每 5 分钟最多 1 次 WARN + 弹窗（限频）；
- **恢复**（失败后首次提交成功）：INFO 1 次，不弹窗。

日志与通知同级、不按条 / 按批刷屏（被限频的失败只留 DEBUG 轨迹）；通知回调为空时只写日志。回调可能从 Writer 线程（失败 / 恢复）或调用 `enqueue` 的线程（积压告警）触发，实现须线程安全。

### 登录 flush 屏障

登录收尾链 `kReplay → kFlush → kReady` 的 `kFlush` 阶段：入队 flush 哨兵，等待此前任务全部提交后才推进 Ready；等待超时 5s 兜底——超时仍照常推进 Ready 并记 ERROR + 弹窗通知（ADR 0042 决策 5）。因此 **Ready 的语义是"登录完成、可交易"，不再承诺"DB 已稳定"**；失败批的哨兵在重试期间保持 pending，持久化降级不阻塞交易、帧推送与 WS（《帧契约：TD 数据同步》§4）。

### 退出与已知限制

- `stop()`：通知 Writer 停止，对失败批做最后一次立即重试；成功则继续排空队列后退出；仍失败只记 ERROR 并退出——未提交的内存积压丢弃，登记为已知限制（ADR 0042 决策 6）。等待 Writer 退出设 30s 上限（大事务 fsync 兜底；超时则快速退出进程、跳过析构）；正常路径必须显式 `stop()`，析构仅作 best-effort 兜底。
- 坏库重建后历史查询缺口由备份 / 归档兜底（本轮不实现，ADR 0042）。
- 跨进程旧会话已入队未提交的任务可能晚于新会话的整组替换落地：当日持仓有 ≤ 一个持仓查询间隔的收敛窗，不丢行、不产生键冲突（《帧契约：TD 数据同步》§7；ADR 0045 决策 7）。

### order_id 与外部标识

- `order_id` 唯一来源是**共享 shm 计数器**（`CHANNEL_NAME_ORDER_ID`）：td 无私有计数器；master 启动时按库内 `MAX(order_id)`（走 `idx_orders_order_id`）播种，td 侧 `OrderIdMeta::ensure_at_least` 仅作幂等兜底（ADR 0041 决策 6）。
- `OrderIdMeta` 只保留 `open_or_create` / `generate` / `ensure_at_least` / `current`（#245 收口；旧 `open_only` / `order_id()` / `name()` 已删除）。
- 柜台外部标识（CTP `order_ref` / FrontID / SessionID / OrderSysID 等）只作为 `ext` 中的**引用**：用于登录重放匹配、撤单定位、排查对账；不建唯一约束、不参与 upsert 键 / 去重，任何路径不得用它反改 `order_id`（ADR 0041 决策 6 / ADR 0045）。

## 设计原则

- **锁选型对齐并发模型**：单写者用轻量自旋锁（进程内跨线程互斥）；多写者用崩溃安全的进程锁（跨进程互斥）；读取端无锁。锁不跨出通道边界。
- **实时写入优先于后台维护**：两者竞争时，实时写入优先，后台维护不得长时间占用。
- **主动与被动职责分明**：维护要么由定时器主动触发、要么按到达帧被动响应，由主线程集中承载，不混淆。
- **宁多勿少，容错优先**：具不确定性的操作采用乐观策略，宁可多发 / 多收也不漏，正确性优先于资源精确。
- **多账户隔离**：账户状态与回报互不干扰，单账户异常不影响其他账户的登录、查询与交易。
- **异常自愈并记录告警**：非本进程原因的异常，由自动调度恢复，同时记录日志、上报告警，供排查而非静默或阻塞。
- **无轮询**：一切事件源均由信号量或回调驱动，绝不采用轮询。
- **事件驱动、单一宿主**：全部延时调度由统一宿主承载，复用信号量超时唤醒，无多余线程。
