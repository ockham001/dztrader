# ADR 0008: TD 持仓模型——聚合查询基准 + 成交/委托增量维护

## Status

Accepted

## Context

td 侧存在两套"持仓计算"的雏形：

1. **明细权威模型**（已实现未接入）：`DzPositionDetail`（公开于 `libs/strategy_api/include/dztrader/struct.h`）+ `PositionHolding::details_`，以 `ReqQryInvestorPositionDetail` 为权威、成交只调整冻结；`on_rsp_qry_investor_position_detail` 目前是 TODO 桩，`OffsetConverter` 无生产调用者。
2. **生产实际行为**：仅登录/重连时 `ReqQryInvestorPosition` 聚合查询 → `PositionMirror` diff → 2002 帧 + DB 重灌；**盘中成交不更新持仓**，策略 `on_position_info` 空窗到下次查询。

问题：公开结构体把 CTP 概念泄露给策略（策略只消费聚合 `DzPositionInfo`）；明细查询有额外流控与数据量成本，对冻结的今昨拆分毫无帮助（CTP 明细也没有冻结字段）；盘中持仓空窗使策略/UI 看到过期持仓；OffsetConverter 需要今昨可用量但无数据源。参考 vnpy_ctp：只用聚合查询（`Position`/`YdPosition`/`TodayPosition`/`PositionCost`），完全不用持仓明细；vnpy converter 的活动挂单重算冻结与今昨拆分语义可直接借鉴。

## Decision

1. **数据源收敛为聚合查询**：`ReqQryInvestorPosition` 建基准，成交回报增量维护今昨/量/均价，委托回报维护冻结（活动平仓挂单本地重算，今仓优先、溢出到昨）。彻底**删除** `DzPositionDetail` 与 `ReqQryInvestorPositionDetail` 全链路（事件 106、SPI override、session 桩、测试）。
2. **持仓模型内部化**：新 `PositionHolding` 位于 `apps/ctp/td/td_position.{h,cpp}`，纯 C++ 结构（非 `DZ_DECLARE_ALIGNED_STRUCT`、不进公开头/SHM），按合约维护多/空两 `PositionSide {today, yd, price, frozen_td, frozen_yd, seq}`（`volume = today + yd` 派生）+ 活动平仓挂单表。
3. **盘中实时化**：任一业务字段变化 → 账户级 `seq` → 2002 帧 + `Kind::Position` 增量 upsert（同 seq）；登录/补查的首个成功查询仍走 `PositionRebuild` 全量重灌。
4. **漂移自愈**：Ready 后每 `qry_position_interval_s`（默认 5 → 60）重查一次，有差异才修正；快照覆盖本地量/今昨/均价（冻结仍由本地挂单维护）。
5. **本波不接入 OffsetConverter**（保留实现与单测，不接入下单路径）；AUTO/父子单聚合回报留待专项。其中 `DZ_POSITION_EFFECT_AUTO` 先 fail-closed 显式拒绝——现默认分支映射为 OPEN，未实现时会静默开仓。

**为什么不用另一个方案（明细权威）**：策略帧只有聚合结构；平今昨拆分只需今昨可用量，聚合查询已提供；冻结今昨无论哪条链路都要本地维护；明细的逐笔成本/开仓日无消费方；活跃会话下明细需额外流控查询且随持仓笔数增长。明细方案只剩"逐笔盈亏"这一未来可能需求，YAGNI，需要时再按新需求引入。

## Alternatives

- **保留明细查询作为今昨权威**：额外 CTP 流控消耗、数据量随 lot 数增长、对冻结拆分无增益、策略不可见。**否决**。
- **纯周期轮询（vnpy 2s 风格，不做增量）**：避免增量与快照的协调成本，但盘中 2002 依赖轮询间隔且全量查询频次高；本方案保留低频轮询做自愈、增量做实时。**否决**。
- **`DzPositionDetail` 移入 ctp 内部保留**：决定聚合模型后无任何消费方，保留即死代码，违反"同一能力不并存两套实现"。**否决**。

## Consequences

- **公开 API/ABI 变更**：`struct.h` 删除 `DzPositionDetail`；SDK/网关同仓同步发版（与 ADR 0007 的整组升级约束一致）。
- **2002 盘中帧量增加**：成交与冻结变化都会推绝对态帧；消费者（SDK ingest、dzweb）无需改动，契约时序更新。
- **本地方案固有近似**：查询在途窗口（请求 → is_last）的成交可能被快照覆盖；冻结为本地活动挂单推导，与 CTP 查询冻结总量不符仅 WARN 不覆盖。均由下轮查询（≤ 一个查询间隔，默认 60s）收敛，需写入契约。
- **登录重放不重复计数**：kReplay 阶段只应用委托状态（冻结），跳过成交增量；基准未建立前不应用持仓（独立就绪标志，周期查询单次失败不解除）。
- **AUTO 行为变更**：未实现的 `DZ_POSITION_EFFECT_AUTO` 由"默认映射为 OPEN"改为显式拒绝；若策略曾依赖零初始化即开仓的未定义行为，需显式改传 OPEN。
- **`enable_lock_mode` 配置保持休眠**，与 OffsetConverter 一并留待 AUTO/拆分专项。

## References

- 帧契约：[td-data-sync](../frame_contracts/td-data-sync.md)
- 组件文档：[dztd_ctp](../components/dztd_ctp.md)
- 参考实现：vnpy_ctp `gateway/ctp_gateway.py` `onRspQryInvestorPosition`；vnpy `trader/converter.py` `PositionHolding`
