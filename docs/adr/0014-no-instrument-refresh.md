# ADR 0014: 不做合约定向刷新（dz_query_instrument）

## Status

Accepted（2026-09-16）

## Context

ADR 0011 引入 `DZ_FRAME_TD_QUERY_INSTRUMENT`(1043) + `DzInstrumentQueryReq` + `dz_query_instrument`：
策略请求单合约定向刷新，td 查 CTP 后落库（无响应帧）。其复杂度集中在 CZCE 消歧回写——
`InstrumentQueryPending` 待回写映射、只读 lookup 连接 + `tdstore::lookup_symbol`、响应 take/override。

核实 vnpy：`ContractData` 只在网关登录/重连时全量查询（`onRspQryInstrument` → `on_contract`），
策略侧无按需刷新入口。该机制无消费方（策略未发布；demo 无引用），登录/重连全量查询已覆盖数据获取。

## Decision

删除定向刷新全链路：帧 1043 与 `DzInstrumentQueryReq`（号删除定义、不保留，按
`general.md §3` 留给后续新帧）、`dz_query_instrument`、td 入口/会话刷新路径、
CZCE 消歧关联机制（`InstrumentQueryPending`/`lookup_symbol`/`to_qry_instrument_field`）。
保留：`dz_db_query_instruments`、`instruments` 表、登录/重连全量查询与 upsert；
CZCE `symbol ≠ instrument_id` 作为数据继续存在。反转 ADR 0011 的 D1/D3 相关决策。

## Consequences

- 策略无法按需刷新单合约；新上市合约在下次登录前不可见（与 vnpy 一致）。
- 回滚路径：若未来重加刷新，须重做消歧关联（symbol→instrument_id 回写）设计，并从零引入帧号。
- 核对项「CZCE 响应 InstrumentID 形态」随功能作废；`TODO(ctp-verify)` 余 2 处（ExpireDate/UnderlyingMultiple）。
