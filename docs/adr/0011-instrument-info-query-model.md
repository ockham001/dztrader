# ADR 0011: 合约信息由推送改为查询、合约结构体退役

## Status

Accepted（2026-09-15）

## Context

合约信息此前由 td 网关在登录链路逐条广播 `DZ_FRAME_TD_INSTRUMENT`（payload = `DzInstrumentInfo`，504B 结构体），
策略在 `dz_next_event` 取帧后自行解析结构体。该模型的问题：

- 合约是**静态数据**，无 push 时序价值；策略却要处理二进制解析与"何时收到全量"的时序；
- 结构体只服务 SHM 帧，无 DB 查询通路；td 落库记录 `InstrumentRecord` 只是结构体的包装；
- 策略只能被动接收，无法按需刷新单合约（新上市/修正场景）。

同时合约字段边界长期未定：哪些进合约信息、哪些属事件/账户级/动态数据，缺少成文依据。

## Decision

1. **查询模型取代推送**：
   - 删除 `DZ_FRAME_TD_INSTRUMENT`(1006) 与 `DzInstrumentInfo`/`DzInstrumentLeg`/`DzInstrumentExt`/`DzInstrumentTickTier`；
   - 新增 `DZ_FRAME_TD_QUERY_INSTRUMENT`(1043) + `DzInstrumentQueryReq`：策略请求**单合约**定向刷新，td 查 CTP 后落库，**无响应帧**；
   - 新增 `dz_db_query_instruments(db, instrument_id, fields)`：DB 查询返回 `DzResultSet`，`fields` 可选（默认全部承诺列）；
   - 全量刷新仍属登录/日切链路，不暴露给策略；策略用定时器延迟后查询。
2. **保留** `DZ_FRAME_TD_INSTRUMENT_STATUS`(1007) 与 `DzInstrumentStatus`：交易状态是盘中事件，不入静态表。（该决策已被 ADR 0013 取代：帧 1007 与 DzInstrumentStatus 已删除。）
3. **字段边界定稿**（23 列 + 2 元数据）：
   - 身份 4、分类 3（`product_class`/`product_code`/`settle_cycle`）、货币 3、量价 3、下单量 4（限价/市价各一对）、
     可交易窗口 2（`listed_date`/`delisted_date`）、期权 4、元数据 2（`update_day`/`updated_at`）；
   - 类型 `DzProduct` → `DzProductClass`（`DZ_PRODUCT_*` 宏不变）；
   - 手续费/保证金**不入合约表**（CTP 不支持全量，走既有账户级按需查询 + `margin_rates`/`commission_rates`）。（费率部分已被 ADR 0013 取代：不保留 margin_rates/commission_rates 与费率查询。）
4. **记录载体的家**：`InstrumentRecord` 落在新库 `libs/tdstore`（规范记录，POD，不含 SQL/后端痕迹），
   供 td 落库与 SDK 查询共用；策略 API 不暴露任何合约结构体。

## Alternatives

- **保留推送，另加查询**：双通路维护成本高、策略仍要处理结构体；且推送时序问题（登录前/后）不解决。**否决**。
- **仅删结构体、改 JSON 推送**：仍是 push，策略仍需解析与处理时序，DB 查询通路缺失。**否决**。
- **查询函数同步等 td 响应**：需要在策略侧引入请求-响应与超时语义，违背"无响应"的既定风格
  （既有 `dz_query_fee_rate` 已是发后即返）；且策略本就可用定时器延迟查询。**否决**。
- **把交割日/到期日/最后交易日都建模**：投机平台不做交割；行业上到期日 ≠ 最后交易日 ≠ 交割期
  （Barchart/CFTC/IB 均分列）。以"可交易窗口"两列覆盖，避免语义混淆。**否决多列**。

## Consequences

- 策略侧：不再解析合约结构体；合约数据一律经 DB 查询（`dz_db_query_instruments`）；刷新用 `dz_query_instrument` + 定时器。
- td 侧：登录链路不再广播合约帧；新增单合约刷新处理；落库改经 `libs/tdstore`。
- v4 schema 迁移：4 rename（`product`→`product_class`、`min_order_volume`→`min_limit_order_volume`、
  `max_order_volume`→`max_limit_order_volume`、`expiry_date`→`delisted_date`）+ 5 add
  （`product_code`、`min_market_order_volume`、`max_market_order_volume`、`underlying_multiple`、`updated_at`）。
- 破坏性变更（无兼容包袱，同 ADR 0010 精神）：帧号 1006 释放、公开 `dz_db_query` 删除、结构体删除。
- 核对项：CTP `ExpireDate` 是否等于最后交易日；`UnderlyingMultiple` 语义。

### 旧 → 新迁移对照

| 旧（已删除） | 新（`instruments` 表 / API / 帧） | 说明 |
|---|---|---|
| `DzInstrumentInfo.instrument_id`/`exchange_id`/`symbol`/`name` | 同名 4 列 | 逐字段直搬（`name` 落库前 GBK→UTF-8） |
| `DzInstrumentInfo.product`（类型 `DzProduct`） | `product_class`（类型 `DzProductClass`） | 类型更名；`DZ_PRODUCT_*` 宏与数值不变（1..14）；另新增 `product_code`（CTP `ProductID`） |
| `settle_cycle` / `settlement_method` / `is_inverse` | `settle_cycle` / `is_inverse` + 保留列 `settlement_method` | 交割方式降为 v3 保留列（不承诺、不可查询） |
| `currency` / `base_asset` | 同名 2 列 | 直搬 |
| `min_order_volume` / `max_order_volume` | `min_limit_order_volume` / `max_limit_order_volume` + 新增 `min_market_order_volume` / `max_market_order_volume` | 限价/市价拆对 |
| `volume_multiple` / `price_tick` / `volume_step` | 同名 3 列 | 直搬 |
| `listed_date` / `expiry_date` | `listed_date` / `delisted_date` | 更名（到期日 → 退市日，语义收敛） |
| `option_type` / `option_exercise_style` / `underlying_id` / `option_strike` / `option_series` | `option_type` / `underlying_id` / `option_strike` + 新增 `underlying_multiple` + 保留列 `option_exercise_style` / `option_series` | 行权方式/调整序列降为保留列；标的乘数新增 |
| （结构体无） | `update_day` / `updated_at` 元数据 2 列 | 新增（刷新完成观测点） |
| `DZ_FRAME_TD_INSTRUMENT`(1006)，登录全量推送 | `DZ_FRAME_TD_QUERY_INSTRUMENT`(1043) + `dz_db_query_instruments` | 推送 → 单合约请求（无响应帧）+ DB 查询；全量刷新留在登录链路 |
| `dz_db_query(db, ...)`（旧通用查询） | `dz_db_query_instruments(db, instrument_id, fields)` | 白名单列投影查询（默认 25 列） |
| `DzInstrumentLeg` / `DzInstrumentExt` / `DzInstrumentTickTier` | 无对应 | 无消费方，不采纳（契约 §8） |

## References

- 设计：`docs/superpowers/specs/2026-09-15-instrument-query-unified-td-db-design.md`
- 帧契约：`docs/frame_contracts/instrument.md`（本次重写）
- 费率/保证金边界：`docs/frame_contracts/td-fee-margin.md`（费率部分已被 ADR 0013 取代：不保留 margin_rates/commission_rates 与费率查询。）
- 帧号规则：`docs/adr/0010-frame-number-reallocation.md`
