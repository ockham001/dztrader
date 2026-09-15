# 帧契约：手续费 / 保证金（费率与资金占用）

本契约规定手续费率、保证金率两条 SHM 帧（`DZ_FRAME_TD_MARGIN_RATE`、
`DZ_FRAME_TD_COMMISSION_RATE`）的字段语义与手续费/保证金的建模约定。
字段布局真相源 = `libs/strategy_api/include/dztrader/struct.h`；
总则见《帧契约：通用规则》。

## 1. 覆盖帧

| 帧 | payload | 性质 |
|------|--------|------|
| `TD_MARGIN_RATE` | `DzMarginRate` | 绝对态（保证金率表，按品种，逐条推送） |
| `TD_COMMISSION_RATE` | `DzCommissionRate` | 绝对态（手续费率表，按品种，逐条推送） |

两帧均为 basic 帧（仅 `DzFrameHeader`）、struct payload、无 `seq`（不参与《帧契约：TD 数据同步》水位/回补机制）。消费方：策略进程只读，用于成本/保证金还原与下单决策。

## 2. 三层建模约定

**总原则——「能动态不静态」**：可变/不确定的字段一律走动态通道（柜台每日查询快照 / 独立事件帧）；只有合约固有静态属性（身份/分类/货币/期权属性/生命周期）才进合约信息。`price_tick`/`volume_multiple`/`volume_step`/`min/max_order_volume` 属合约属性、交易所罕见调整，放合约信息，靠每日全量重发兜底。动态代价是多点存储，静态化代价是交易所后期修改难以对接——一律优先动态。

| 层 | 内容 | 承载 |
|----|------|------|
| 层1 | 静态费率"率"表（低频写、策略只读） | `DzMarginRate` / `DzCommissionRate` |
| 层2 | 场所特有、策略决策必需参数 | 正式伴随帧（本次未建；见 §4） |
| 层3 | 动态决策必需数据（funding rate 等） | 独立低频事件帧（本次未建；见 §5） |

## 3. 层1 字段语义

- `DzMarginRate`：
  - `long/short_margin_ratio_by_money`：多/空头保证金费率（比例）。
  - `long/short_margin_ratio_by_volume`：多/空头每手固定保证金金额；期权"每张固定保证金"（XTP `sell_margin`、TORA `MarginUnit`）也走 by_volume。
  - `is_relative`：0=绝对值，1=相对保证金率。
  - **保证金币种不落本表**：线性合约取 `DzInstrumentInfo.currency`，反向合约（`is_inverse=1`）取 `DzInstrumentInfo.base_asset`。
- `DzCommissionRate`：
  - `*_ratio_by_money`：按金额比例费率；`*_ratio_by_volume`：按手固定金额（期权"每张固定手续费"走 by_volume）。
  - 开/平/平今三分量：期货（含今昨拆分）；证券/币圈无"平今"语义时 `close_today_*` 填 0 或与 `close_*` 同值（由网关策略决定，本契约不强制）。

## 4. 决策必需字段的正式通道（层2，未建）

- 以下字段为**决策必需**，按 `struct.h`/`instrument.md` 契约**不得**进 `DzInstrumentExt` K-V：
  - 股票税目：印花税（单边）、过户费、经手费、规费、结算费、最低佣金。
  - 期权卖方保证金算法参数：FixedMargin / MiniMargin / Royalty、UpperRatio、组合保证金差。
  - 币圈 taker/maker 分层费率（账户级、准静态；手续费=成交额×费率，与 by_money 同构，接入时亦可直接进层1 `DzCommissionRate`）。
- 上述字段在**接入对应非 CTP 柜台网关时**，以正式伴随结构体/帧引入；引入前由策略本地参数配置。本契约仅固化此规则。

## 5. 动态数据（层3，未建）

- `funding rate`（资金费率，8h 级周期、与持仓差挂钩、决策必需）：**不**进 `DzInstrumentExt`，**不**塞 `DzTick` 热路径。将来以独立低频事件帧（如 `DZ_FRAME_TD_FUNDING_RATE`）推送，策略只读；引入前策略自行获取。
- 外汇点差/隔夜利息、币圈初始/维持保证金/阶梯档位：同样归本层，接网关时按需建帧。

## 6. 将来扩展位（YAGNI 预留说明）

- `DzCommissionRate` 尾部可追加 `strike_ratio_by_money` / `strike_ratio_by_volume`（SOPT 期权执行手续费，同构现有 6 项双精度，向后兼容）。
- `DzMarginRate` 尾部可追加期权保证金模型字段（SOPT `OptionInstrTradeCost`）。
- 追加原则：仅在对应柜台接入且有数据源时实施，字段追加在尾部、保持 8 字节对齐、同步 SQLite 白名单。

## 7. 按需查询（阶段2）

策略对**单合约**的保证金率/手续费率按需查询，td 网关实时查 CTP 后回填。

### 7.1 请求帧

| 帧 | payload | 方向 | 性质 |
|----|---------|------|------|
| `TD_QUERY_FEE_RATE=DZ_FRAME_TD_QUERY_FEE_RATE` | `DzFeeRateQueryReq` | 策略 → td 网关 | basic 广播帧（按 payload.account_id 路由） |

`DzFeeRateQueryReq`（`libs/core/include/dztrader/core/core_struct.h`）：

- `account_id[32]`：目标账户；空串 = 本网关全部账户。
- `instrument_id[88]`：目标合约（平台唯一键，网关段前缀规则见合约契约）。必填。
- `query_type`：`0`=保证金率，`1`=手续费率，`2`=两者。

策略入口：`dz_query_fee_rate(ctx, account_id, instrument_id, query_type)`（`libs/strategy_api/include/dztrader/api.h`）。

### 7.2 时序（异步回填）

1. 策略 `dz_query_fee_rate` 写入 DZ_FRAME_TD_QUERY_FEE_RATE 帧即返回（**不阻塞**）。
2. td 网关收到后按账户路由到 session，发起 CTP `ReqQryInstrumentMarginRate` / `ReqQryInstrumentCommissionRate`（单合约，带 `BrokerID+InvestorID` 账户级参数）。
3. 响应逐条：**入库**（margin_rates / commission_rates 表）+ **广播** `TD_MARGIN_RATE` / `TD_COMMISSION_RATE`。
4. 策略经 SHM 帧回调或后续 `dz_db_query_commission/margin` 拿新值。无请求-响应关联。

### 7.3 与阶段1（登录收尾批量查询）的差异

| 维度 | 阶段1（登录收尾） | 阶段2（按需查询） |
|------|------------------|------------------|
| 触发 | 登录收尾链（四查询之一） | 策略 `dz_query_fee_rate` |
| 范围 | 全量账户级（InstrumentID 留空） | 单合约 |
| 广播 | **不广播**（只入库，防全量洪泛） | **入库+广播** `TD_MARGIN_RATE`/`TD_COMMISSION_RATE` |
| 数据源 | CTP 全量回报 | CTP 单合约回报 |
| 响应过滤 | 跳过 `IR_All`（交易所统一行），取 `IR_Group`/`IR_Single`（账户特异性） | 同左 |