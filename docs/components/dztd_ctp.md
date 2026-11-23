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
- **内置默认排程**：日盘 `08:45→15:45`、夜盘 `20:45→02:33`，`enabled` 默认开启，装好即按此节奏自动登录 / 登出。默认表定义在 `apps/ctp/common/default_auto_login.h`（与行情网关共用同一处定义、各取自己那份，避免两边漂移）；平台层配置类只接受调用方注入的默认表，不知道 CTP 时刻。
- **空表 = 未配置，不是清空**：配置文件缺 `auto_login` section、或 `schedules` 为空数组时，网关归一为上述默认表并材料化到配置文件（内存 / 上报 / 文件三者恒等）；**关闭自动登录只能用 `enabled=false`**，删空时段表达的是"恢复默认"。运维写入自定义排程后以配置为准，不再回落到默认表。完整语义见契约 auto-login「持久化与默认值」。
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

- **零账户运行**：账户是交易接口内部的配置项（持久化在本进程配置文件，契约 td-config §6）；接口生命周期由交易账户页显式管理（动态创建 / 启动 / 移除，ADR 0053），进程可在没有任何账户时启动与运行，等待后续添加账户。
- **生命周期的平台侧归属**：动态注册 / 移除由 master 的进程配置驱动（契约 process / rest §2.10），本进程不感知自己是被 REST「添加交易接口」动态创建，还是由 `dztraderd.json` 预注册——两条路径的启动行为一致（配置文件缺失 → 默认值自愈；首帧 `report_full_snapshot` 提供零账户配置快照）。移除接口前平台要求账户已清空（`gateway_has_accounts`），本进程只按 `RemoveAccount` 删除自身配置条目。
- 一进程多账户：每账户一个 CTP Trader API + SPI + `AccountSession`（持有 `TdStateMachine` 独立状态机实例与 `account_id`）。
- **账户隔离**：账户级 `instance_id` = 账户 ID（全局唯一，跨全部接口类型，总则 §5），不携带进程名；状态、进度、回报按账户隔离，单账户失败不影响其他账户。
- 主线程集中管理全部账户会话，状态机不依赖 CTP SDK、可独立测试。

### 登录流程

连接 → 认证（`ReqAuthenticate`）→ 登录（`ReqUserLogin`）→ 结算确认（`ReqSettlementInfoConfirm`）→ 合约加载（`ReqQryInstrument`）→ Ready。状态保护：仅特定状态可发起下一步；合约加载受流控约束重试。

- **本进程暂无登录链自动重试**（缺口，见 #406）：认证失败 / 登录失败 / 结算确认失败都回滚到 `Authenticated` / `Connected` / `LoggedIn`，但从这些状态**没有任何路径重发请求**——`AccountSession::req_login()` 只在前置刚连上（`on_front_connected`）与认证刚成功（`on_rsp_authenticate`）两处被调用；`on_sched_timer` / `try_recover_login` / `connect_account_by_id` 都按 `find_session() == nullptr` 过滤，session 已存在时一律跳过。因此失败后账户停在帧侧 `LoggingIn`（`account_state_of` 把 `Authenticated`/`Connected` 都映射为 `LoggingIn`）。md 侧有 `can_login()` 重登能力（契约 md-status 失败语义），td 尚未对齐。
- **登录失败必须让用户看得见**（#407）：正因为上一条——失败后停在正常中间态、且不自动重试——失败若不上浮，账户就会静默卡住。四条失败路径（柜台拒绝 / 交易日无效 / 当日基准装载失败 / 登录超时）统一经 `notify_login_failed()` 出口：先 `on_login_failed()` 转状态，再以 `DZ_NOTIFY_ERROR` + 弹窗推送 `NOTIFY_UI`，正文带上**具体场景**，不用状态机的通用文案「交易登录失败」——只说失败不说原因，用户拿不到可用信息。文案构造在 `login_failed_message()`（纯函数可单测）。状态机拒绝转移（非 `LoggingIn`，如前置已断线或已登出）时不推送，避免弹一条对不上界面状态的告警。
- **失败原因仅瞬时可见，不进 `td-status`**（#407 决策，后续票 #427）：本票只做 `NOTIFY_UI` 弹窗，**不**在 `TdStatus` 新增 `last_error`（md-status 有该字段，td-status 没有）。是否把失败原因做成持久字段（而非弹窗一闪而过、断线后无从回看）已拆到 #427（与 md 对齐：`last_error` + 登录 pending 即时清除），不阻塞本流程。
- **手动登录/登出入口**（#409 / ADR 0051，契约 td-connection）：恢复路径 = **先登出、再登录**（#406 拍板，登录失败不自动重试）。`TD_CONNECT` / `TD_DISCONNECT` 由 dzweb 生产：REST `POST /api/td/accounts/{account_id}/login|logout`、WS `td_connect/td_disconnect`（守卫 = admin + 账户注册表解析网关 + 网关 Running + 写通道）；前端账户行内「登录 / 登出」双按钮。**登录仅在完全未登录（progress 到 min）可用；登出在任意非未登录状态可用**（连接中/登录中/重连中/失败回滚/就绪 = 强制中断）。账户配置在有活跃会话时仍拒绝 `UpdateAccount`，提示「请先登出再修改配置」——该提示现在可执行。
- **状态即时推送**（#409 补齐）：`RTN_TD_STATUS` / `RTN_PROGRESS` 不再只在启动快照与配置变更时推送——主循环每轮用 `td_status_publish_key`（`state` / `progress_current` / `date` / `login_time`）对比上次推送，任一状态字段变化即推（`publish_changed_states()`）；新建会话与登出（会话销毁前）也各推一次终态。此前缺口正是 WebUI 的登录按钮 / pending 无法随登录结果翻转的原因，详见 `docs/frame_contracts/td-status.md` 触发场景 1。
- 本进程不发起柜台登出请求：用户的「登出」（任意非未登录状态可发）即彻底释放 CTP SDK API（直接断开），回 Idle；不存在"已登出、连接保留"的稳定状态（契约 td-connection）。
- 例外：若收到服务器主动登出的回调，状态已偏离预期，属异常情况，非本进程主动为之。此时保留前置连接，由自动调度重新登录恢复，并记录错误日志、上报告警，供排查。

### 登录日装载（重放基准）

登录成功（含盘中重连、删户重加、盘中换柜台）后、处理首条重放回报之前，从交易库装载**当前交易日**作为重放基准（`apps/ctp/td/td_day_load.{h,cpp}`；ADR 0045 决策 5；《帧契约：TD 数据同步》§3）：

- **只装当日**：订单最新态（`order_ref` / OrderSysID → 既有 `order_id` 的匹配基准，CTP 扩展字段从 `ext` JSON 恢复）与当日成交（去重键基准）。**不装历史**——历史委托 / 成交只在交易库，内存里不留（全史预扫已在 #246 删除）。
- **seq 播种**：`seq_counter_ = max(当日四表 MAX(seq), 无会话取号器已分配值)`。四表共享取号、跨日累积，`MAX(seq)` 不带日过滤。无会话取号器（`AccountSeqAllocator`）每次取号都吸收库内进展，避免同账户先有会话推进 seq 后旧种子撞号。
- **失败即登录失败**：装载失败（只读连接不可用 / 查询异常）按登录失败路径处理（`on_login_failed`，状态机回滚到 `Authenticated` / `Connected`），不带着空基准进入重放——空基准会把整段重放当成新数据，重复落库与重复推帧。
- **交易日解析失败即登录失败**：登录响应里的 `TradingDay` 解析不出有效交易日（哨兵 `INT32_MIN` / `DZ_DATE_NA` / 非正数）时同样走 `on_login_failed`，不进入 `LoggedIn`，对齐 md 的 `LoginParseError` 口径。会话交易日是本会话全部业务数据的时间锚（订单 / 成交 / 持仓 / 资金的 `date`、`ACCOUNT_STATUS` 帧的 `date`、回报交易日校验全取它）；容忍它缺失会导致数据盖上无效日期、而状态帧 `date` 回落 0，日切感知（`ACCOUNT_STATUS` 的 `date` 变化）随之失真。判定在 `trading_day_usable()`（`td_account_session_pure`，纯函数可单测）。
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
- **对统计计数器的影响（已消除）**：均价变化时产生的刷新 `ORDER_REPORT` 在委托已终态后仍会到达；dzweb 统计计数器按每账户 `order_id` 单调水位判首见，该帧是「见过的委托」，不会重复计 total / volume（ADR 0050，契约 rest §2.9.5）。刷新帧形态本身不变。

### 查询通道（登录 + 运行期资金刷新，ADR 0055）

CTP 的 `ReqQry*` 在同一 API 实例上共用查询流控：同时至多 1 笔在途、前置按每秒限额。td 因此给**每个账户一条查询通道**（`QueryPacer` + 运行期 `QueryLane`）：

- **登录链**：结算确认后查合约、合约加载后查持仓、持仓收口后查资金；三笔共享同一闸门（发送间隔 ≥ qry_min_interval_ms，默认 1100ms），不足间隔用 `TimerQueue` 延后发送（登录收尾状态机与失败语义不变）。持仓基准建立与盘中增量见《持仓维护与偏移转换》——**无周期重查**（ADR 0049）。
- **运行期资金刷新**：账户 Ready 后，`OnRtnOrder`（任意状态）/ `OnRtnTrade` / 出入金 `RSP`/`RTN` 置脏，UI 手动刷新帧 `TD_REFRESH_REQ`（契约 td-refresh）同样置脏；窗口内事件合并，空闲且满足间隔时发一笔，发送后到达的事件在收口后补一笔（不丢）。**无周期查询、无轮询**——只有事件与绝对时间定时器（`TimerQueue`，主循环 `next_timeout` 驱动）。
- **失败**：有界退避（≤3 次），失败不改变账户状态、不阻塞交易；`OnRspError` 与 `nRequestID` 关联补齐（查询失败不再悬挂到 5 分钟超时）。

## 交易委托与回报

- 接收策略广播的 `TD_ORDER_REQ` / `TD_ORDER_CANCEL_REQ`（basic 帧，按 payload `account_id` 归属过滤，见《帧契约：交易委托请求》）。
- 委托 / 成交回报（`OnRtnOrder` / `OnRtnTrade`）经 event 通道广播（`ORDER_REPORT` / `TRADE_REPORT`），供策略与前端消费；每笔状态变化按业务键 upsert 交易库，帧与 DB 行同 `seq`（《帧契约：TD 数据同步》§1）。
- **成交去重键**：`(account_id, date, exchange_id, trade_id)`——`trade_id` 仅交易所内唯一，含 `exchange_id` 才不丢多交易所同号成交。
- **成交均价刷新**：每笔成交更新的累计均价若变化，同一委托会额外产生一条 `ORDER_REPORT`（新 `seq`、委托行同 `seq`）；消费方按业务键 + `seq` 合并即为幂等（见上节「成交均价」）。
- **无会话拒单**（#13）：账户已配置但当前无运行会话（从未连接 / 连接前窗口）时下单，仍分配账户级 `seq`、落库并推 `ORDER_REPORT`（`REJECTED`），否则 `seq=0` 会被消费端水位当作"快照已含"静默吞掉。**已知限制**：此时没有会话交易日可用，`date` 取本地自然日、`time` 取本地时钟秒——与真实交易日可能相差一天（夜盘跨越午夜时），且该行不参与当日登录日装载；后续由 `ReqQryOrder` 对账或历史回看页校正。

## 持仓维护与偏移转换

持仓在进程内**自维护**：聚合查询建基准 + 成交/委托回报增量更新（决策见 [ADR 0008](../adr/0008-td-position-model.md)），不复用 CTP 明细查询。

- **模型**（`apps/ctp/td/td_position.{h,cpp}`，纯 C++ 内部结构，不进公开头/SHM）：按合约 `PositionHolding` 维护多/空两侧 `PositionSide {today, yd, price, frozen_td, frozen_yd, seq}`（`volume = today + yd` 派生）与活动平仓挂单表（按合约隔离）。任一业务字段变化才取新 seq 推 2002 + `Kind::Position` upsert，未变化不推不落；原 `PositionMirror` 删除（差异/seq 职责并入 `PositionSide`）。`DzPositionDetail` 与 `ReqQryInvestorPositionDetail` 链路整体移除。
- **查询建基准**：`ReqQryInvestorPosition` 响应按 (instrument, direction) 多行合并，`is_last` 一次性应用（volume/yd/price 以快照为准；frozen 不由快照覆盖）。**只在登录 / 重连时发起**——这是持仓的唯一权威初始值。CTP `PositionCost` 为金额，均价按 `均价 = PositionCost / (持仓量 × 合约乘数)` 换算（合约乘数取自合约查询映射，缺乘数时均价留 0）。**今昨口径**：`today = TodayPosition`、`yd = Position - TodayPosition`（`YdPosition` 是交易日开始时的静态昨仓，平昨后不减少，不得用于当前昨仓）。差异侧推 2002 + `Kind::Position` upsert，消失 side 发 volume=0 帧（只推零帧、不 upsert，行由重灌 DELETE 兜底）；首个成功查询强制 `PositionRebuild` 重灌，后续查询仅在存在差异/消失时重灌（重连即是新基准，见下）。就绪前成交/委托的持仓增量一律不应用，首个成功查询统一重建。
- **基准查询失败即登录失败**：重试 3 次、间隔 3 秒；仍失败则停在当前进度、不转 Ready、推 ERROR 通知，由用户处理。`is_last` 正常到达但零行 = **成功**（空账户合法，"查不到"不是失败）。
- **成交增量**：OPEN 增今仓、均价加权；平今/平昨按标志；非 SHFE generic CLOSE 今→昨溢出，SHFE generic CLOSE 减昨；量归零时均价清零，异常负值夹到 0 + WARN。登录缓冲重放阶段（kReplay）只应用委托状态、跳过成交，避免与查询快照双重计数。
- **冻结**：只由活动平仓挂单（含外部单）本地推导 `volume - volume_traded`，今仓优先、溢出到昨、按量夹取；拒绝/撤单/全成释放。登录种入按当前交易日过滤，CTP 冻结总量不符仅 WARN。
- **日切**：today→yd、清挂单冻结，变化侧按上规则推帧/落库。**会话不得跨交易日**（ADR 0049 决策 5 / 契约 td-data-sync §3 §7）：`AccountSession::on_rtn_order` / `on_rtn_trade` 在入口（登录缓冲重放之前）校验回报自带的 `TradingDay` 能否自证等于会话交易日——空值 / 无法解析 / 不相等一律记 ERROR（`reason=empty/unparsable/mismatch`，会话未建立为 `session_unknown`）+ 推 ERROR 弹窗通知，并经 `request_cross_day_logout` 置登出请求；`TdApi::dispatch` 在本次事件处理完、用完会话后取走请求，走既有 `disconnect_account_by_id`（推 Offline + erase）主动登出，重登由自动调度在下一个 `login_time` 触发。被拒回报**丢弃**（不分配 seq / 不推帧 / 不落库）；同一会话只登出一次。判定纯逻辑在 `judge_report_trading_day`（`td_account_session_pure`，可单测）。
- **无周期查询**（[ADR 0049](../adr/0049-position-pure-incremental-no-periodic-query.md)）：持仓不再有任何定时重查，取消 60s 周期查询与其配置项、以及登录失败后的两条补查重试链。代价是**失去自愈**——增量累积误差、私有流缺口会持续到下一次登录的基准查询。相应地，正确性依赖两条前提：会话不跨交易日（期权行权/履约/到期、盘后自动组合零回调，只能由次日登录吸收）、私有流按投资者级推送（外部单与自营单同权实时进账）。
- **外部单与自营单同权**：`OrderRef`/OrderSysID 在本地映射中找不到的回报标为外部单（策略标识 `/external`），走与自营单完全相同的持仓路径，不做 `FrontID/SessionID` 过滤；套保仓（`HedgeFlag != 投机`）全部计入，不静默丢弃。**假设失效的运行期告警**（ADR 0049 / 契约 td-data-sync §7）：登录重放里新出现的外部委托，若其柜台时间不晚于本会话最后一条**实时**回报的柜台时间——说明那一刻本会话已在收实时流、按投资者级假设该单本应已实时到达并落库（落库即命中重放过滤器被吞，走不到告警）——记 ERROR 并累计计数，通知按 5 分钟窗口限频。晚于最后实时回报的记录可能是断线窗口内别的终端下的单，不告警（只会漏报、不会误报）。判据基准存在会话对象里，**只有会话存活期间的 CTP 自动重连**才保留它；调度登出会重建 `AccountSession`，此时无从比较（保守静默）。
- **期权**：**支持**持仓统计与普通报单（买卖开平，与期货同构）；**不支持**发起行权 / 自对冲 / 放弃行权指令。
- **套利 / 组合合约**：`product_class == DZ_PRODUCT_SPREAD` 的持仓行**不推帧、不落库**（DEBUG 日志），成交照常记录但不参与持仓统计；单腿合约是普通期货合约，照常增量维护。判定取登录期合约查询映射的 `product_class`（合约查询先于持仓查询；合约表未命中的持仓行按普通合约处理，属已知漏判面）。不支持组合动作（`ReqCombActionInsert`）与郑商所套利/套保确认（`ReqSpdApply` / `ReqHedgeCfm`）。
- **`PosiDirection` 为净持仓（`'1'`）的行：跳过该行 + 记 WARN**，不产生持仓记录——**不按未知方向处置**（"未知方向作废整轮快照"是给读不懂的码的防线，`PD_Net` 是合法枚举，升级它会让账户登录不上），也不静默归为多头（会凭空造多头持仓）。CTP 持仓查询实际按多/空返回（实测组合持仓行方向也是 `'2'`/`'3'`），此值为死分支。
- **交割不支持**（不处理交割配对产生的持仓变化）。
- **下单**：本波不接入 OffsetConverter（SHFE 拆分/AUTO/父子单聚合回报留待专项），`DZ_POSITION_EFFECT_AUTO` 显式拒绝；风控上下文填入该合约真实多空持仓与合约 `price_tick`（现规则实际使用 `price_tick`）。

### 快照生命周期（#249 锁定的行为）

下列行为由 `apps/ctp/tests/td_persist_writer_test.cpp`（写入器层）、`td_data_sync_integration_test.cpp`（Writer 时序）与 `td_position_lifecycle_test.cpp`（**会话层**：真 `AccountSession` + 真 SQLite + 真 persist 线程）断言**库内容**覆盖，是《帧契约：TD 数据同步》§3 / §7 与 ADR 0045 / ADR 0046 / ADR 0049 的可执行形态（改坏任一条应红）：

- **登录不清零、不预置 0**：新会话登录后、首个完整成功持仓查询之前，`positions` 当日**没有行**，上一次已知快照（前一日行，含跨会话 / 删户重加 / 换柜台）原样保留；不写 `volume=0` 占位行。
- **查询失败不改表**：持仓查询 `is_last` 带 `ErrorID` 时，当日既有行的 `seq` 与内容完全不变（既不删也不改），资金行同样不写——锚点日停在上一成功日（ADR 0046）。
- **整组原子替换的作用域 = 该账户 + 当日**：完整成功查询在单事务内删该账户当日行 + 写快照组；其他账户的当日行、同账户的历史交易日行都不动。响应里消失的合约行被删除（不产生幽灵持仓）。
- **首个成功查询强制重灌，后续查询无差异不重建**：`position_baseline_ready_` 之前由 `PositionRebuild` 整组重灌；之后（重连带来新基准时）无差异且无消失的一轮查询不产生 `PositionRebuild`（DB 各行的 `seq` 不变，即无新写入）。
- **`volume=0` 增量可查**：全平时日内增量走单行 upsert，落库是**存在的一行 `volume=0`**（不是跳过、不是删行）——「当日全平」与「从未成功查询过」因此在库里可区分。
- **日切 today→yd 只碰当日行**：日切后当日行落成 `today_volume=0 / yd_volume=原今仓`，上一交易日的行保持它自己当时的今昨拆分（历史行不可变）。
- **删户重加 / 盘中换柜台**：同一 `account_id` 二次登录后再次整组重建，当日结果是**整组替换**——无重复行（唯一键）、不残留上一轮独有的合约、历史交易日行不变。

> **测试边界（`td_position_lifecycle_test`）**：该目标驱动的是**持仓查询响应分支**（`on_rsp_qry_investor_position → apply_position_query`），不覆盖「登录链本身」（合约加载 → 结算确认 → 收尾 → Ready）——那条链在 `req_settlement_confirm` 里无条件解引用 `api_`，而 `api_` 只能由 `open()` 创建真实 `CThostFtdcTraderApi`（需连柜台）。登录链的 flush/Ready 语义由 `td_login_finalize_test`（纯逻辑）与 `td_data_sync_integration_test`（Writer 时序）覆盖。两条合起来即完整链路，不重复也不留缺口。

### 已知限制（持仓）

- **今昨冻结未拆分落库**：**schema v1 的 `positions` 只有 `frozen_volume` 单列**。td 内存里冻结是 `frozen_td` / `frozen_yd` 拆分维护的（今仓优先、溢出到昨），但落库与 `POSITION_INFO` 帧只带合计值；读端（REST / WebUI）只看得到总冻结，无法分别展示今/昨冻结。窗口 = 本轮 schema。
- **旧会话残留写入无自愈路径**：跨进程旧会话已入队未提交的 persist 任务可能晚于新会话整组替换落地 → 当日持仓回到旧值，**要等到下一次完整成功查询（下一次登录 / 重连）才被替换**；不丢行、不产生键冲突（《帧契约：TD 数据同步》§7、ADR 0049）。
- 其余持仓准确性边界（增量累积误差与私有流缺口无自愈、非 SHFE generic 平仓今昨归属为本地约定、期权行权/履约/到期与盘后自动组合依赖次日登录吸收、套利组合行不统计、资金刷新的事件缺失窗口）见《帧契约：TD 数据同步》§7。

### 资金快照（#250 / ADR 0055 锁定的行为）

资金查询有两个触发面、一个写入点：登录收尾链（`on_rsp_qry_investor_position` 收口 → 发起 `ReqQryTradingAccount`）与运行期按需刷新（委托 / 成交 / 出入金事件 + UI 手动刷新，见上节）；响应统一走 `on_rsp_qry_trading_account` 写库/推帧（`DZ_FRAME_TRADING_ACCOUNT`）。行为由 `td_position_lifecycle_test.cpp`（会话层）、`td_persist_writer_test.cpp`（写入器层）断言库内容：

- **按 `(account_id, date)` 多日累积**：每个交易日一行最新快照；同账户同日重复查询是 upsert（覆盖余额类列与 `seq`，不追加行），历史交易日行不被改写（`SameDayFundQueryUpsertsInsteadOfAppending` / `DaySwitchFundQueriesCoexistAsTwoDayRows`）。
- **资金行只在持仓查询成功后写入**：`on_rsp_qry_trading_account` 的唯一写入点有 `position_query_ok_` 守卫——持仓查询失败时资金行不落库、不推帧，锚点日（两张快照表 `MAX(date)`，ADR 0046）不会被资金行单独推进。
- **无周期查询、事件驱动刷新**（ADR 0049 / ADR 0055）：登录收尾后由委托 / 成交 / 出入金变化与 UI 手动刷新触发按需查询；同一账户任意两笔查询间隔 ≥ qry_min_interval_ms（默认 1100ms）、窗口内事件合并；失败有界退避、不改变账户状态。事件缺失（断线 / 私有流缺口）时资金停在最近一次成功查询（已知限制，契约 td-data-sync §7）。

## 配置（契约 td-config / td-status）

配置与状态 payload 都是**通用层 + 柜台 `ext` 分层**：新增接口类型只换 `ext`，通用层（`account_id` / `enabled` / `risk_control_enabled` / `currency_id`；状态帧的 `state` / `date` / `login_time` / `progress_*`）不变。类型层真相源 `apps/ctp/td/td_config.h` 与 `td_state.h`，JSON schema 以契约为唯一语义真相源。

### 配置结构与持久化

- **文件形态 = 帧形态**：`td` section 与 `RTN_TD_CONFIG` payload 同形（顶层 qry_min_interval_ms / `ext` / `accounts[]`；账户 `ext` 内是 `broker_id` / `user_id` / `password` / `product_info` / `frontends` / `auth_code` / `app_id` / `flow_dir`）。不做「文件旧形 / 帧新形」双序列化——同一事实两处定义必然漂移。代价：**旧扁平格式（连接参数直接在账户对象上）不再兼容**，开发期无迁移义务。持仓查询间隔已随 [ADR 0049](../adr/0049-position-pure-incremental-no-periodic-query.md) 删除；`qry_account_interval_s` / `qry_flush_interval_ms` 已随 [ADR 0055](../adr/0055-td-query-channel-event-driven-fund-refresh.md) 删除。
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
- 未知枚举计数挂在 `ext.unknown_*`（口径见《帧契约：交易网关状态》§2.3）。

### 未知枚举：为什么没有编译期防线（issue #367 故事 12）

本仓**放弃**用编译器抓「枚举新增值没被映射」，因为 `-Wswitch` 在本仓的两个层面都**永不生效**（实测确认，不是可以选的）：

- **CTP 侧**：枚举是 `char` 的 typedef 加 `char` 字面量宏（如 `#define THOST_FTDC_OST_AllTraded '0'`）。对 `char` 做 `switch` 时 `-Wswitch` 永远不可能触发——这是硬限制，换编译器/换开关都不行。
- **自有枚举**：`DzOrderStatus` / `DzDirection` / `DzPriceType` / `DzPositionEffect` 等是 `typedef int8_t` + 宏，不是真正的枚举类型。实测即使显式打开 `-Wswitch -Wswitch-enum` 并去掉 `default`，也**零告警**。

因此本仓的映射层策略是**运行期显式化 + 计数可观测**，而不是编译期检测：

- 回报方向（柜台 → 平台）：读不懂的值落成对应的 `DZ_*_UNKNOWN`，原始字符登记到 `UnknownEnumSink`，由 `AccountSession::report_unknown_enums` 记 ERROR 日志 + 按种类计数（进 `RTN_STATUS.ext`）+ `notify_ui(popup=false)`；通知限频（5 分钟内最多一次），计数不受限频影响。**不触发任何查询**——取消周期查询后不存在"重查纠正"路径（[ADR 0049](../adr/0049-position-pure-incremental-no-periodic-query.md)），可观测性由计数与通知承接。
- 出账方向（平台 → 柜台）：映射函数返回 `std::nullopt`，`place_order` 本地拒单——**绝不**兜底成某个看起来最像的真实值（兜底等于向交易所发错误指令：止损单变限价单、平仓单变开仓单）。
- 未知开平标志的成交/委托**不参与持仓增量与活动委托表**（当开仓会放大敞口，当平仓会平掉对方真实持仓，两者都是撒谎）。

要拿到编译期防线，需要把自有枚举改成真枚举（`enum : int8_t`）——那是独立的源码级不兼容变更，另票（#368）并需 ADR；**CTP 侧无论如何都拿不到**。改动映射层时请照此约定：`default` 只有两种合法行为——返回 `UNKNOWN` 并上报，或者返回失败由调用方拒单。

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
- **关停时必须先析构 `TdApi` 再 `spdlog::shutdown()`**（`td_main.cpp` 显式 `api.reset()`）：`~TdApi` → `PersistWriter::stop()` 在"已经停止"的确认路径上会打 info 日志，而 shutdown 之后 default logger 为 null → SIGSEGV（退出码 11）。后果是 master 把正常关停上报成 `Crashed` 并弹「child crashed」错误通知（#440 E2E 实测；`SmokeTest` 已加 Crashed 回归断言：逐轮 drain 记录 Crashed、不依赖与快照兜底 Stopped 的先后，并在收尾留 1s 晚到观察窗）。md 侧同型风险已在 #449 对齐修复（`md_main.cpp` 同样先析构 `MdApi`；`~MdApi` 入口的关停日志让该守卫对 md 同样可红），删掉任一侧的 `api.reset()` 都会使守卫变红。
- 坏库重建后历史查询缺口由备份 / 归档兜底（本轮不实现，ADR 0042）。
- 跨进程旧会话已入队未提交的任务可能晚于新会话的整组替换落地：当日持仓回到旧值后**无自愈路径**，需等下一次完整成功查询（下一次登录 / 重连）替换，不丢行、不产生键冲突（《帧契约：TD 数据同步》§7；ADR 0049）。

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
