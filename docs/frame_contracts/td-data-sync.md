# 帧契约：TD 数据同步

本文件覆盖 `DZ_FRAME_ORDER_REPORT`(2000)、`DZ_FRAME_TRADE_REPORT`(2001)、`DZ_FRAME_POSITION_INFO`(2002)、`DZ_FRAME_TRADING_ACCOUNT`(2003) 四个交易推送帧，及其账户级 `seq` 水位同步机制（衔接 DB 查询快照与 SHM 实时推送两条数据通路）。总则见《帧契约：通用规则》。

**覆盖帧**：

| 帧 | payload | 性质 |
|------|--------|------|
| `ORDER_REPORT`(2000) | `DzOrderReport` | 追加流（委托） |
| `TRADE_REPORT`(2001) | `DzTradeReport` | 追加流（成交） |
| `POSITION_INFO`(2002) | `DzPositionInfo` | 绝对态（持仓） |
| `TRADING_ACCOUNT`(2003) | `DzTradingAccount` | 绝对态（资金） |

四 payload 末尾均带 `uint64_t seq`（**账户级状态变更序号：账户内全类型共享、跨日累积单调**；字段表不重复，类型层真相源 `libs/strategy_api/include/dztrader/struct.h`，语义见 ADR 0007）。相关请求帧 `TD_ORDER_REQ`/`TD_ORDER_CANCEL_REQ` 见《帧契约：交易委托请求》；登录状态帧 `ACCOUNT_STATUS`(2018) 见《帧契约：账户登录状态》；策略 SDK 消费侧见《帧契约：策略》"SDK ingest 过滤职责"。

**语义**：td 推送账户内状态变更（追加流）与登录查询快照（绝对态），消费端（策略 SDK ingest、dzweb TdDataService）以每账户水位 W 过滤/回补/去重/倒退重置，把两条数据通路衔接成"不丢数据、可延迟"的完整当日状态。

**数据流**：形态 5（总则 §4.2）——basic 广播帧（仅 `DzFrameHeader`，无 `instance_id`，身份在 payload `account_id`）；写端 = td 网关（权威）；读端 = 策略 SDK（经 ingest 过滤 → 回调）与 dzweb（TdDataService 建镜像）；无前端入口、无 RTN；master 不消费（drain 透传）。

**时序（触发场景）**：实时回报（下单结果/成交，含本地拒单）；登录 CTP 重放（过滤器吞同后转发差异）；登录完成协议的持仓/资金查询响应（2002/2003 写端）与最后一条重推；不响应 `QUERY_FULL_SNAPSHOT`（快照走 DB 查询，本帧为增量/推送）。

---

## seq 语义

- **账户级**（非进程级）：帧自带 `account_id` 天然过滤；故障隔离（单账户数据重置不拖累他账户）；多账户拆进程原样带走。
- **跨日累积不归零**（uint64 用不完）：零日界逻辑（夜盘归属次日的交易日切换点模糊，按天重置无法定义"何时归零"）；消费者无需与 td 同步重置水位；**倒退检测语义唯一：低 seq = 数据被重置（而非新的一天）**。
- **全类型共享一个计数器**（委托/成交/持仓/资金同取号）：persist 队列跨类型 FIFO ⇒ 前缀不变量跨类型成立；每账户一个 W；一次回补覆盖全部类型。
- **seq 语义 = 状态变更序号**：被重放过滤器判定"与基准相同"的记录不推不落、不分配 seq（见下）。
- 同一事件 shm 帧与 DB 行带同一 seq；账户 ID 全局唯一（多网关账户不撞名，运维约定，见"边界与窗口"）。

## 不变量清单（安全性论证基础）

1. **单写者按序分配**：td 主线程按序分配 seq（事件经单一事件队列派发到主线程，无并发）；同一事件两路（shm 帧 + DB 行）同值。
2. **前缀不变量**：persist 队列 FIFO + 单 Writer 线程 + **吞同不分配** ⇒ DB 已提交集合恒为已分配 seq 的前缀。
3. **shm 帧序 = seq 序**：分配与写帧同线程同序。
4. **消费者铁律**：跳过判断只准用 **DB 查得的水位 W**，永不使用内存见过的最大 seq（防 td 崩溃重启 seq 复用误丢新事件）。

## 生产端：重放过滤与 seq 分配（td）

基准 = **内存轻量镜像**（账户 connect 时从 DB 装载该账户全部订单最新态 + 全部 trade_id 集合，**不按交易日过滤**——connect 时交易日尚未知，且 CTP 重放在登录成功后立即到达，基准必须先于首条重放就绪；量级数百条可接受），热路径**不查 SQLite**（db 归 Writer 线程独占）。镜像在主线程**即时**更新（不等落库），否则 burst 内下一条回报拿旧基准误判。

- 委托：与基准对比 `{update_time, volume_traded, volume_canceled, status}`——集合完全相等 → 忽略（不推/不落/不分配 seq）；不相等 → 防御检查 update_time 回退（异常旧数据则吞并告警），否则更新镜像 + 分配 seq + 推 shm + 落库
- 成交：`(account_id, trading_day, trade_id)` 存在性 → 已存在忽略；不存在转发
- 持仓/资金（查询响应）：绝对态，与基准有差异即转发（幂等覆盖）
- **主判据"不相等即转发"而非"严格新于"**：CTP 私有流本身正序，重放逐条对比+转发天然补发正序中间态、最终收敛最新态；recency 只作防御（update_time 回退 = 异常数据，吞 + 告警）——以"严格新于"为主判据会误吞同一秒内 status 翻转（volume_traded/update_time 均不变的部分成交后撤单拒绝）
- **对比仅业务字段，排除时间戳噪声**（CTP 重放 UpdateTime 空值/精度不稳定，参与对比会误判差异引发全量重推）
- **命中时回填 `order_ref_map_`**（td 重启后内存表为空，老本地单防误判外部单、错配 order_id）
- **CTP 重放模式无关**（RESTART/RESUME/QUICK 均适配）；**保留 `THOST_TERT_RESTART`**（重放是差异来源，换 QUICK 使 td 停机期间变化永久丢失）
- 收益：重放风暴期 DB 写入量 = 实际变化量；消除"每次重登全天重推、策略成交重复计数"隐患

## 登录完成协议（确定性同步点）

```
登录 → CTP 重放(过滤器吞同) → 持仓/资金查询 → persist 提交
     → 广播 login-success（2018 Ready）
     → 重推各追加流最后一条（原 seq 重发）
```

- **查询链路**：Ready 前发起 `ReqQryInvestorPosition`/`ReqQryTradingAccount`（CTP 流控 1 次/秒，**串行间隔发起**；登录不在热路径，耗时秒级可接受）。查询失败**不永久放弃**：先转 Ready（不阻塞交易），由定时调度补查直至成功——否则 positions/trading_accounts 表缺口无帧可补（无变化即无帧），快照永久缺项
- **flush 屏障**：登录路径 persist **排空后才广播 Ready**（`PersistWriter` 同步排空：等待队列空且末批已提交，带超时兜底）——Ready 附加"**DB 已稳定**"语义（持久化完成是 Ready 广播的前提，见《帧契约：账户登录状态》2018 Ready 语义）
- **最后一条重推带原 seq**（同一事实重投）：已同步消费者被 `seq ≤ last_applied` 过滤；未同步者拿它当触发器。真正角色 = **保证到达的触发帧**——重放全被吞、行情再安静，登录后必有一帧带 seq 到来，把"断档挂到开盘"收成"登录完成即自愈"
- "最后一条"只对委托/成交（追加流）有意义；持仓/资金是**多记录绝对态**，由登录查询响应推送覆盖（即 2002/2003 写端）
- 重推为**直接写帧**（不经过滤器、不落库——DB 已含该记录），从内存基准镜像取当日最后一条；无记录则不推（也无断档可能）

## 消费端衔接（SDK ingest + dzweb TdDataService）

**W 水位过滤**：启动开 reader（从最新写位置起，历史帧不重放）→ 查 DB 每账户 W = `MAX(seq)`（不带日过滤，与累积计数器对齐）→ 帧 `seq > W` 应用、`seq ≤ W` 跳过（快照已含）。W 与快照的原子性：四表**分次查询即可，无需跨表事务**——每表查询本身原子（读到该表已提交前缀），设 W = 四次查询 max 的最大值，则"seq ≤ W 的行"必在快照并集中。

**断档回补**：首帧 `seq > W+1` → 断档暴露（在途窗口：帧写在 reader 开启前、落库在快照查询后）→ 四表联查 `WHERE seq > W AND seq < S₀`（S₀ = 首帧 seq）→ 按 seq 序归并应用。赶上落库积压 → 几十 ms 短重试；耗尽 → 按"崩溃丢失"放行 + 日志。运行中断档不可能（单写者 + 通道 append-only ⇒ 帧序 = seq 序、读者连续），断档检测仅启动首帧一次。

**成交去重**：按 `(account_id, trading_day, trade_id)` 集合**二道防线**去重（td 过滤器为第一道），交易日切换清理。

**seq 倒退重置**：发现 `seq` 低于该账户已应用水位（数据被重置，非新的一天）→ 清空该账户 ingest 状态、重查 DB 设新水位后继续。dzweb 另由 `ACCOUNT_STATUS`(2018) Ready/Offline 翻转触发重建/清空（见《帧契约：账户登录状态》镜像节）。**清空必须显式**：重置后若无持仓不会再有持仓帧到达，旧镜像幽灵持仓永无人覆盖。

**消费端分布**：策略 SDK ingest（《帧契约：策略》"SDK ingest 过滤职责"：对策略透明，`on_trade_report` 语义 = **每笔成交恰好一次**）；dzweb TdDataService（同一水位/回补/去重/倒退重置，2018 触发重建）。

## seq 不出后端边界

seq 是 td ↔ SDK/dzweb 后端的**内部协调坐标**：**不进 WS 契约、不进 REST 契约、前端不可见**。前端自有衔接机制：WS 建连/重连推 `snapshot` 全量镜像 + 增量（契约 webui-ws），断连期间错过的增量由重连 snapshot 补齐。dzweb 后端 ingest 用 seq 对齐后照常向 WS 广播领域消息；REST 分页键用 rowid/日期+id。

## 边界与窗口

| 场景 | 行为 |
|------|------|
| td 崩溃丢失（已广播未提交） | seq 永久洞；回补重试耗尽放行 + 日志 |
| persist 积压 > 回补重试窗口 | 放行，后续延迟修复 |
| td 崩溃重启 seq 复用（未提交段） | 消费者铁律（"不变量清单"第 4 条，DB 水位 W）保证不误丢新事件 |
| 账户数据清空/重加 | 重置协议（SDK 倒退检测 / dzweb 2018 触发；清空必须显式） |
| 启动竞态在途窗口 | 回补；安静场景由登录完成协议重推触发 |
| 跨日 | seq 累积无边界；trades 唯一键含 trading_day；positions 单事务重灌（旧日行清除） |
| 旧 SDK + 新 td | `payload_size_matches` 防御性丢帧；td 与 SDK 同仓同步发版 |
| 多策略并发读 td 库 | SQLite 共享锁并发读 OK；写事务期间读端 busy_timeout 吸收 |
| 多网关账户 ID 撞名 | 运维约定全局唯一；帧内加源标识留待需要时（YAGNI） |

## 镜像

dzweb TdDataService 维护内存镜像（orders/trades/positions/trading_accounts），`ACCOUNT_STATUS`(2018) Ready 触发重建（清该账户镜像 + 只读打开 td 库重查 + 设新 W）、Offline 清空该账户镜像，语义见《帧契约：账户登录状态》；WS 暴露留给后续设计。不进 dzweb WS 镜像（高频业务帧，总则 §9）。

---

## 变更流程

本契约随 TD 数据同步实现演进；修改必须执行总则 §11.3 变更 checklist。
