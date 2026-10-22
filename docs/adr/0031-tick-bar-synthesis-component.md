# ADR 0031: tick→bar 合成通用组件（BarBuilder）

## Status

Accepted（2026-10-22）

## Context

- 旧实盘版本（2023-08 前）的 tick→bar **数据语义**在生产中被验证为正确：真实交易日、产品级交易小节、
  开盘竞价并入后一小节首根、小节末端 tick 归前一根、累计量额差分、日线直接用 tick 当日累计字段。
  其缺陷在实现层：字符串时间键 + 每 tick 哈希查表、两份重复模板、竞价只覆盖两个固定分钟、无法扩展到股票。
- ADR 0018 决策 1 将 K 线限定为"策略进程内自用单周期模板"。但同一份 bar 语义必须同时服务
  数据存储进程（1min + 日线落库）、bar_strategy 框架与未来多因子/价差等策略模板；
  多套实现必然产生实盘/存储/回测口径漂移。
- tick→bar 位于框架 1µs p99 热路径内。旧版多周期路径约 0.2–0.35µs/tick/品种（字符串哈希 + 7 份
  生成器 + bar 结构体入队）；通用组件必须无哈希、无字符串、无分配。
- 参考实现（kungfu `BarGenerator`、vnpy `BarGenerator`）均为墙钟/帧时间对齐的简化实现：
  无交易日/小节/竞价模型、末根依赖下一 tick 且无续接；可借鉴的只有"单实例单合约单周期 + 累计量差分 + 惰性收盘"。
- 归档域（#4）与 `dz_db_query_bar` 仍未实现，其 schema 依赖本 ADR 的 Bar 语义。

## Decision

1. **范围**：组件只做 tick→bar 纯计算，一个实例 = 单合约 + 单周期。消费方：数据存储进程、
   bar_strategy 框架、未来多因子/价差等策略模板。不读 DB、不落库、不传输、不做多周期合成、不做量 bar；
   WebUI 图表经归档域取数。位置 `libs/strategy_api/include/dztrader/bar/`（公开 SDK header-only，仅标准库），
   提供 `IntradayBarBuilder<Sink>` / `DailyBarBuilder<Sink>` 两个类。
2. **时间坐标**：`DzDate` 保持真实交易日；交易日内时间用 `tsec = (DzTime + 21600) % 86400`（18:00 锚点，0..86399），
   同一交易日严格单调。二者**不合并为时间戳**。锚点是平台常量，改动属破坏性变更（需新 ADR）。
3. **bar 边界**：18:00 锚点连续整数网格 `begin = floor(tsec / period) * period`，复现旧版 1/3/5/15 分钟与
   1 小时（自然整点、跨小节休息拼根）口径；内部边界 tick 归新 bar；小节末端 tick（`tsec == session_end`）
   归前一根；无成交的桶不生成 bar；乱序/非交易时段 tick 丢弃并计数。
4. **交易小节与竞价数据**：`TradingSessionTable` 为定长 POD（端点含、`tsec` 坐标），按
   `(exchange_id, product_code)` 一张、交易所默认 + 品种覆盖、存 `platform.db`（沿用 ADR 0018 决策 2/3）。
   竞价窗口数量不设业务上限（容量 8），一个交易日可有多个，各自显式 `kind`（Open/Close）+ `session_index`；
   旧版 `callAuction1/2` 映射为两个 Open 窗口。组件只接收 POD，加载器/C 查询 API 属独立项。
5. **竞价语义**：Open 窗口 tick 归后一小节首桶，按旧口径令 `O=H=L=C=last_price`；窗口内只更新 bar 状态、
   **不调用任何 sink 回调**（策略不可能在竞价时段下单）；Close 窗口按小节内普通 tick 处理。
   窗口长度由数据描述，不写死"一分钟"。
6. **数据口径**：H/L/C 只用 `last_price`；量额用当日累计差分（交易日重置；bar 起始基准 = 上一笔累计量/额）；
   持仓量取最后一笔；非法价格跳过。**不做 tick 累计 high/low 补采**——分钟线 high/low 与标准日线不一致是
   tick 切片固有现象，主流行情软件同样处理。
7. **日线**：独立 `DailyBarBuilder`，不依赖小节表；直接用 tick 的当日累计字段（open/high/low/close/volume/
   turnover/open_interest），交易日变化即收盘/开新日；`begin_sec = 0`（18:00 标记）、`period_sec = 0`。
8. **驱动与续接**：纯 tick 驱动，不建线程/定时器；小节末 bar 在下一笔更晚 tick 关闭，不提供 flush，
   调用方用 `current_bar()` 读取未完成 bar。`Bar.is_final` 区分已收/形成中；`restore(const Bar&)` 按
   final/forming 语义续接（final 不重复 close；forming 同桶只更新、不重发 open）；
   `cum_volume/cum_turnover` 提供差分与续接基准（等价旧 `bov/eov`）。
9. **命名字段（定稿，后续不改）**：
   `Bar { trading_day, begin_sec, last_tick_sec, period_sec, open, high, low, close, volume, turnover,
   open_interest, cum_volume, cum_turnover, is_final }`，方法 `standard_end_sec()/begin_clock()/last_tick_clock()`；
   配套类型 `TradingTime`、`TradingSession`、`AuctionWindow`、`AuctionKind`、`TradingSessionTable`、
   `BarBuilderStats`、`NullBarSink`。旧库映射：`bot→begin_sec`、`eot→last_tick_sec`、`eov→cum_volume`、
   `eoi→open_interest`、`id = begin_sec / 60`；旧 bid/ask/涨跌停列不进入核心 Bar（数据存储侧自补）。
10. **性能与数据一致性验收**：热路径无 map/字符串/分配，单周期更新目标 p99 ≤ 50ns（静态估算约 10ns）；
    基准先 report-only、稳定后升 CI 门禁。数据一致性以**差分测试**为硬验收：测试内置"旧版查表算法"参考实现，
    对随机/真实 tick 流逐根断言 `begin/OHLC/量额/OI/last_tick` 相等。
11. **文档关系**：issue #3 改写为本 spec；`docs/components/bar_strategy.md` §5.3/§5.4 引用本 ADR；
    归档域（#4）按本 ADR 的 Bar 语义与 `is_final` 设计 schema。

## Alternatives

- **保留 ADR 0018 决策 1（策略侧自用模板）**：数据存储与各策略模板各写一套，live/存储/回测口径漂移。否决。
- **引擎/帧层生成并推 bar 帧**：两次写帧 + 唤醒进入策略热路径；0018 已否决，维持。
- **照抄旧版查表实现**：每 tick 字符串哈希、无扩展性；只采纳其语义，不复用实现。否决。
- **参考 kungfu/vnpy 的墙钟/帧时间对齐**：无交易日/小节/竞价/续接，末根丢失；只借鉴"单实例单周期 + 累计差分 + 惰性收盘"。
- **引入定时器/线程做小节收盘**：新增线程与回测/实盘时序差异，且用户明确要求纯 tick 驱动。否决。

## Consequences

- 公开 SDK 头新增 `dztrader/bar.h` 系列（header-only、无 C ABI 变化）；分发（#7）与公开面收敛（#14）需纳入。
- `platform.db` 小节/竞价 schema、种子数据、加载与 C 查询 API 仍是独立项（ADR 0018 决策 2/3），不阻塞本组件；
  缺表/表非法由调用方 fail-fast。
- 归档域 #4 依赖本组件 Bar 语义；因为惰性收盘，数据存储必须持续 upsert forming bar，不能只等 close 落库。
- 旧数据可逐值一致（新增 `turnover/cum_turnover/is_final/period_sec` 列）；旧库 bid/ask/涨跌停列不再逐 bar 产生。
- 不支持多周期/量 bar/定时收盘/跨会话 flush；策略多周期由策略用短周期自行合成（与 ADR 0018 一致）。

## Supersedes

- [ADR 0018: 策略侧 K 线生成与交易小节数据模型](0018-strategy-side-kline-and-trading-sessions.md) — 仅决策 1（K 线由策略进程内自用单周期模板生成）被本 ADR 取代；其决策 2/3/4（交易小节/集合竞价元数据归 `platform.db`、产品级覆盖、竞价合并语义）继续有效并被本 ADR 采纳。

## References

- [ADR 0018](0018-strategy-side-kline-and-trading-sessions.md)（交易小节/竞价数据模型，决策 1 被本 ADR 取代）
- [ADR 0011](0011-instrument-info-query-model.md)（instruments 表：`exchange_id`/`product_code`）
- [ADR 0015](0015-database-interface-port-adapter.md)（归档域依赖的 DB 端口）
- [ADR 0020](0020-strategy-sdk-dynamic-delivery-and-public-surface.md)（SDK 公开面与交付）
- 旧版语义来源：2023-08 前实盘版本 `include/dzApiCpp/barGen.hpp`、`barGenerator.hpp`、`src/dataCenter/io.cpp`
- 参考实现：kungfu `framework/core/src/libkungfu/wingchun/service/bar.cpp`；vnpy `vnpy/trader/utility.py`（`BarGenerator`）
