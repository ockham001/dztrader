# ADR 0032: 竞价窗口内的策略事件与竞价相位（is_auction）

## Status

Accepted（2026-10-23）

## Context

- tick→bar 组件（[ADR 0031](0031-tick-bar-synthesis-component.md)）在 Open 竞价窗口内只更新 bar 状态、
  **不调用任何 sink 回调**（决策 5），依据是"策略不可能在竞价时段下单"。
- BarStrategy 框架（[ADR 0027](0027-bar-strategy-framework.md)，Proposed）尚未实现，B 侧还没有 bar 回调；
  组件目前只有测试消费方，没有生产消费方。
- 需求（2026-10-23 设计会话）：竞价撮合结果出来后、连续交易开始前有一段静默窗口（商品期货约 1 分钟、
  股票约 5 分钟），行情在窗口内正常推送。这段时间应可用于跨日重计算与指标预热，避免开盘后第一笔
  tick 上做重活；策略还需知道"当前是否处于竞价窗口"，以便自行跳过下单（"不能交易"由交易所保证，
  框架不按时间拦截）。
- 旧实盘版本（2023-08 前）已验证该行为：`common_attribute` 表的 `callAuction1/2`（种子值 `20:59:00`）
  在启动时映射为"竞价分钟 → 后一小节首根 bar"并置 `isCallAuction=true`；竞价 tick 照常回调策略
  （`onBar` / `updateBar`），`bar.isCallAuction` 字段注明"仅针对盘中有效、不落库"；策略自行
  `if (bar.isCallAuction) return;` 跳过交易。ADR 0031 继承了其数据口径（竞价并入后一小节首根），
  但把"窗口内不回调"写死，与新需求冲突。
- 约束：`Bar` 字段与语义在 ADR 0031 决策 9 定稿（归档 schema 依据），相位不落在 `Bar` 上；帧契约不变
  （bar 在策略进程内生成、不跨帧）；连续交易热路径不得增加工作量；不引入新数据源。

## Decision

1. **事件模型**：Open 竞价窗口内不再静默。窗口首笔 tick 触发
   `on_bar_close(上一根)` → `on_bar_open(新首根)` → `on_bar_update`；窗口内后续每笔 tick 只触发
   `on_bar_update`；离开窗口后的第一笔连续交易 tick 只触发 `on_bar_update`（同一根 bar 继续更新，
   不再 open）。"每根 bar 恰好 open 一次、close 一次，每笔 tick 一次 update"的既有契约不变，
   只是首次 open/close 提前到窗口内发生。
2. **数据口径不变**：竞价 tick 仍归后一小节首根（ADR 0018 决策 4 / ADR 0031 决策 5）：`begin_sec`
   为小节首桶起点（可晚于 tick 时刻）、`last_tick_sec` 为真实竞价时刻、`O=H=L=C=竞价价`、
   `is_final=false`；不做独立竞价 bar，`Bar` 不新增字段。
3. **相位判定**：B 通过 `ctx.is_auction()` 查询；语义 = **触发本次回调的 tick 是否处于 Open 竞价窗口**
   （回调时刻的环境相位），不是被交出 bar 的属性。组件 sink 的三个 bar 回调签名为
   `(const Bar&, bool is_auction)`，bool 取自已算出的竞价窗口匹配结果，不新增查表或分支；
   BarStrategy 在每个回调入口写入 ctx。历史回放、静默填窗、`on_init` / `on_params_update` 期间恒为 false。
4. **执行不拦截**：框架不因相位拒单、缓存或延后发单，不新增按时间的执行门禁；竞价窗口内的
   `buy/sell/...` 与其它时刻同路径（无效单由交易所拒，属策略/操作员责任）。过滤是策略的责任：
   `if (ctx.is_auction()) return;`（旧版口径）。
5. **仅 Open 窗口**：v1 只标记 `AuctionKind::Open`；`AuctionKind::Close` 维持 ADR 0031 决策 5 的
   "按小节内普通 tick 处理"（回调照旧、`is_auction()` 为 false）。收盘竞价的委托参与收盘撮合，
   与"不能交易"语义不同；将来需要时再加枚举值（源码兼容）。
6. **删除静默补发机制**：为"窗口内零回调"服务的待补发队列及其配套状态（`pending_bars_` / `PendingBar` /
   `BarOrigin::Auction` / 静默收尾与补发函数 / 队列溢出丢弃策略）整块删除；组件不再有延迟补发
   open/close 的路径。
7. **无 tick 不保证**：维持纯 tick 驱动（ADR 0031 决策 8），窗口内没有 tick 就没有回调，不新增定时器
   或兜底事件；策略需容忍该退化（在第一笔连续 tick 的 update 里补算）。
8. **数据源不动**：竞价窗口表仍由调用方按 ADR 0018 决策 2/3 提供（platform.db 落地后置）；本决策
   不引入新数据源或占位配置，BarStrategy 侧在表数据源落地后启用。

## Alternatives

- **保持窗口内零回调、只加查询函数**：查询没有调用时机，等于放弃该窗口；否决。
- **新增第七个回调 `on_auction(ctx)`**：`on_bar_update`-only 策略拿不到；事件双轨；纯 tick 驱动无法
  判定"首笔/末笔"，语义模糊；否决。
- **在 `Bar` 上加 `is_auction` 字段**（旧版做法）：`Bar` 已冻结且是归档 schema 依据；一根 bar 从竞价
  tick 开始、到连续交易里继续更新，相位是事件属性不是记录属性，bool 会在根内翻转；否决。
- **框架按相位拦截下单**：用时间推断"可交易性"应由交易所/柜台保证，拦截会让逻辑账与实盘分叉；
  排队到开盘发单是另一特性（跳空与价格失效语义未定），不在本决策；否决。
- **收盘竞价也置 true**：收盘竞价窗口内委托仍参与撮合，标 true 会误导策略；v1 否决。
- **保留静默模式作编译期开关**：多一套状态机与两套测试，当前没有消费方需要静默；否决。

## Consequences

- 公开头变更：`libs/strategy_api/include/dztrader/bar/types.h` 的 Sink 契约与 `NullBarSink` 三个回调
  增加 `bool is_auction`；`intraday_bar_builder.h` 删除静默机制（净减约百行）并更新事件说明。
  当前无生产消费方（仅测试），是改动成本最低的时机。
- 事件时序提前：上一根 bar 的 close 与首根的 open 由"离开窗口后第一笔连续 tick 补发"改为"窗口首笔
  tick 当场触发"；"tick 流在窗口内结束则 bar 永不补发"的规则随之消失。
- 事件语义边界：品种实例的**首根 bar 没有 close**（第一笔有效 tick 只触发 `on_bar_open → on_bar_update`，
  两者携带同一份快照；竞价窗口内启动则两者 `is_auction` 为 true）；此后每根新 bar 才是
  `on_bar_close(上一根) → on_bar_open(新根) → on_bar_update`。又因回调门禁（ADR 0027 决策 7），
  B 的**第一次**回调序列是 `on_bar_close(静默填窗最后一根) → on_bar_open → on_bar_update`，
  第一次收到的 close 属于历史填充而非首根 live bar。
- 测试与基准：`intraday_bar_auction_test.cpp` 的"窗口内零回调/补发"断言改为事件序断言；
  `bar_perf_bench` 的 auction 路径自检更新，基线记录追加新行（历史记录不修改）；连续交易
  `update` 路径数值不应变化。
- 性能：连续交易热路径不变；竞价 tick 每次多 1–3 次模板静态分发（每天个位数到几十笔）；内存少
  8 根待补发 bar 快照（约 832B/实例）；净减一套状态机。
- 策略语义：窗口内会收到 `on_bar_close`（上一根）与 `on_bar_update`，`is_auction()` 为 true 期间
  不应下单（策略责任）；`on_bar_open` 会出现在竞价窗口内，迁移与开发者文档需写明。
- 文档：`docs/components/bar_strategy.md` 的 B 可见面增加 `ctx.is_auction()`、§5.1/§5.4 事件序与 §14
  同步；ADR 0027 无需改动（`is_auction` 是 ctx 查询，不新增回调）。不阻塞 BarStrategy 框架 spec，
  后者引用本 ADR 并依赖交易小节表数据源落地。

## Supersedes

- [ADR 0031](0031-tick-bar-synthesis-component.md) 决策 5 中"窗口内不调用任何 sink 回调"的部分
  （含为此刻意设计的延迟补发语义）；决策 5 的其余内容（Open tick 归后一小节首桶、`O=H=L=C`、
  Close 窗口按普通 tick 处理）与 ADR 0031 其它决策继续有效。

## References

- [ADR 0018](0018-strategy-side-kline-and-trading-sessions.md)（交易小节/竞价元数据归 platform.db）
- [ADR 0027](0027-bar-strategy-framework.md)（BarStrategy 框架总纲与 B 契约）
- [ADR 0031](0031-tick-bar-synthesis-component.md)（tick→bar 合成；决策 5 部分被取代）
- 组件设计：[docs/components/bar_strategy.md](../components/bar_strategy.md)
- 旧版实盘证据（2023-08 前）：`include/dzApiCpp/struct.h`（`bar.isCallAuction`，"仅针对盘中有效、
  不落库"）、`src/dataCenter/io.cpp`（`createTimeIDComMap`：竞价分钟映射到下一分钟 bar 并置
  `isCallAuction`，种子 `callAuction1=20:59:00`）、`app/strategyDevelop/ddb_cci/logicMaster.cpp`
  （策略按 `bar.isCallAuction` 跳过交易）
