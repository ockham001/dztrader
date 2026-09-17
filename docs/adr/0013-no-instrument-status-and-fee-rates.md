# ADR 0013: 不做合约状态与费率（保证金/手续费）

## Status

Accepted（2026-09-16）

## Context

平台此前实现三块 vnpy 参考实现没有的能力：
- 合约交易状态：帧 1007 + `DzInstrumentStatus`，td 转发 CTP `OnRtnInstrumentStatus`（公有流，登录/重连 RESTART 重放）；
- 保证金率（帧 1010）/手续费率（帧 1011）：登录收尾链末两查询 + 两张库表 + 按需查询帧 1036 与 SDK API。

核实 vnpy：`ContractData` 无状态/费率字段；`vnpy_ctp` 网关未实现 `OnRtnInstrumentStatus`，也不发起
`ReqQryInstrumentMarginRate`/`ReqQryInstrumentCommissionRate`。三块能力无消费方（webui/策略均未使用），
且合约状态原始值域为 CTP 专有、跨接口不兼容（`'7'` 在期货与 SOPT 同码异义），维持需引入平台中性枚举与逐柜台映射表。

## Decision

删除三块能力，对齐 vnpy 模型：
- 帧 1007/1010/1011/1036 退役，不复用；
- `DzInstrumentStatus`/`DzMarginRate`/`DzCommissionRate`/`DzFeeRateQueryReq` 与 SDK API
  `dz_query_fee_rate`/`dz_db_query_margin`/`dz_db_query_commission` 删除；
- 登录收尾链 4 查询 → 2 查询（position → account → replay → flush → ready）；
- schema v5 `DROP TABLE margin_rates/commission_rates`；契约 td-fee-margin 删除。

反转 ADR 0011 的 D2"保留 1007 + DzInstrumentStatus"决策；费率边界决策被本 ADR 取代。
保留：`TradeRecord.commission`（成交自带费用）与 `DzTradingAccount.margin/commission`（账户占用）属成交/账户数据，不在删除范围。

## Consequences

- 策略若需成本/可用保证金估算：CTP 柜台在下单时校验；平台后续如需要，另行立项"账户级费率查询"（可回滚路径）。
- 合约状态如未来重启需求：须以平台中性枚举 + 逐柜台映射设计（见本 ADR Context 记录的同码异义问题），从零设计而非恢复旧帧。
- 多接口规划中的股票类柜台（XTP/奇点/OST 等）无期货式状态语义，本决策与该方向一致。
