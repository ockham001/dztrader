# 帧契约：合约信息

本文件覆盖 `DZ_FRAME_TD_INSTRUMENT`、`DZ_FRAME_TD_INSTRUMENT_STATUS` 两个交易推送帧（合约静态信息与合约交易状态）。总则见《帧契约：通用规则》。

**覆盖帧**：

| 帧 | payload | 性质 |
|------|--------|------|
| `TD_INSTRUMENT` | `DzInstrumentInfo` | 绝对态（合约静态表，逐条推送） |
| `TD_INSTRUMENT_STATUS` | `DzInstrumentStatus` | 状态事件（合约交易状态变更） |

两帧均为 basic 帧（仅 `DzFrameHeader`，无 `instance_id` 扩展头）、struct payload（编码规则见总则 §6）。类型层真相源：`libs/strategy_api/include/dztrader/struct.h`（`DzInstrumentInfo` 及伴随结构体）、`libs/core/include/dztrader/core/core_data_type.h`（帧号）。帧号定义：`TD_INSTRUMENT = DZ_FRAME_TD_INSTRUMENT`、`TD_INSTRUMENT_STATUS = DZ_FRAME_TD_INSTRUMENT_STATUS`。

## 语义 / 数据流 / 路由

| 帧 | 逻辑方向 | 路由方式 |
|---|---|---|
| `TD_INSTRUMENT` | td 网关 → 策略进程 | basic 广播（事件通道），策略只读消费 |
| `TD_INSTRUMENT_STATUS` | td 网关 → 策略进程 | basic 广播（事件通道），策略只读消费 |

- **数据流**：形态 5（总则 §4.2）——td 网关在合约查询回报/状态回报时逐条 `write_struct` 写入事件通道；策略进程只读消费；无前端入口、无 RTN；master 不消费（drain 透传）。
- **发送方**：td 网关进程（`dztd_*`）。`TD_INSTRUMENT` 于账户登录链内合约全量查询时逐条发布（查询完成/失败时账户转 Ready，见《帧契约：账户登录状态》）；`TD_INSTRUMENT_STATUS` 于盘中场所合约交易状态变更时推送（无对应请求帧）。
- **接收方**：策略进程。策略 SDK 对两帧**仅放行不解析 payload**（不进 TD ingest 水位过滤——payload 无 `seq` 字段，不在《帧契约：TD 数据同步》机制内），帧经 `dz_next_event` 返回给策略用户，按 `frame_type` 自行解析；合约信息可用性（下单前校验 `price_tick`/`volume_step` 等）由策略自负责。
- **不使用** `DzExtInstFrameHeader`：一律按 basic 布局解析。
- 网关在写帧的同时将合约行持久化至 td 库 `instruments` 表（审计/复盘用，非消费通路；schema 见 TD schema v3）。

## Payload

struct 引用（不抄写字段表，字段定义见 `libs/strategy_api/include/dztrader/struct.h`）：

- `TD_INSTRUMENT` → `DzInstrumentInfo`（504 字节，8 字节对齐）
- `TD_INSTRUMENT_STATUS` → `DzInstrumentStatus`

`DzInstrumentInfo.product` 取值域为 `DzProduct` 枚举（`DZ_PRODUCT_*`，`libs/strategy_api/include/dztrader/data_type.h`），`UNKNOWN` 为兜底值——网关无法归类时填 `UNKNOWN`，**不得**以猜测的品种类型替代（v2 起 product 为整数枚举，历史字符编码已废弃）。

## 身份规则

`instrument_id` 是平台全局唯一键，订单/持仓/行情/合约等所有帧以它引用。三段式身份：

| 字段 | 语义 | 规则 |
|------|------|------|
| `instrument_id` | 平台唯一键 | CTP 合约用**裸交易所代码**（`rb2601`/`MA601`）；其他网关加**网关段前缀**（`IB.266004536`/`BNS.BTCUSDT`）保证跨网关唯一。唯一性由网关发布时校验（重复即网关缺陷）。 |
| `exchange_id` | 交易场所代码 | 平台注册表值（`SHFE`/`CME`/`BNS`/`BNF` 等），非展示字符串，参与路由与规则判定。 |
| `symbol` | 场所原生代码 | 网关对场所 API 发单/订阅时**原样透传**；仅展示与场所交互用。 |

- **禁止从 `instrument_id` 反向解析 `symbol`**：前缀仅为跨网关唯一性约定，非语法规则（`normalize_to_product` 类品种前缀提取仅适用于确认无歧义的裸码域，不得用于恢复 `symbol`）。
- `underlying_id`（期权/权证/可转债标的）必须是合约表内有效行的 `instrument_id`（引用指数行合法，`DZ_PRODUCT_INDEX` 为非交易参考行）。

## 数量语义

平台单位全帧整数（`DzVolume` = int32 手数语义）：

- **平台单位**：期货/期权 = 手/张，证券 = 股，币圈 = `stepSize` 粒度。下单/持仓帧全部整数。
- **原生数量换算仅发生在网关边界**（网关把 `DzOrderReq.volume`（平台单位）换算为场所原生数量，反之把场所回报归一到平台单位），策略核心路径不做任何浮点换算。
- 换算基准为 `volume_step`（1 平台单位对应的原生数量；CTP 恒 1）。网关侧换算约定：`volume_step <= 0` 视为 1；原生→平台方向**向下取整到 step 网格**（浮点商误差容差 1e-9）。工具实现见 `libs/core/include/dztrader/instrument_util.h`。
- `volume_multiple`（价值乘数）**仅用于** PnL/保证金浮点运算，不参与数量换算。

## 哨兵值

struct 为 POD 零初始化，未显式赋值字段读到 0——哨兵体系与零初始化语义重合（"未赋值即未提供"），网关发布者按下表义务填值：

| 字段 | 哨兵 | 语义 |
|------|------|------|
| `listed_date` / `expiry_date` | `DZ_DATE_NA` = 0 | 日期未提供。0 = epoch day 0 = 1970-01-01（元旦），**全球无开市**，与真实业务日无碰撞，可安全用作 NA；与 POD 零初始化重合（未赋值即 NA，安全缺省）。期货/期权/权证/转债的 `expiry_date` 必填（不得 NA）；回测移仓**不得交易未上市合约**（`listed_date` 为 NA 时由策略自行保守处理）。 |
| `min_order_volume` | `<= 0` | 未提供（无最小下单量约束；A 股场景为 100 股这类值，限买不限卖）。 |
| `max_order_volume` | `<= 0` | 无限制。 |
| `volume_step` | `<= 0` | 视为 1（网关换算工具已收敛此哨兵，见 `instrument_util.h`）。 |
| `settle_cycle` | 0 = T+0 | **零初始化陷阱**：POD 零初始化使未填值落 0，而 0 是合法值 T+0（非"不适用"）。网关发布时**必须显式填值**：衍生品填 -1（不适用），现货/证券按场所规则填 0/1/2。当前平台无该字段消费方，但语义已定，禁止依赖"未填即无所谓"。 |

## 校验（仅写与总则不同的规则）

- 两帧为纯推送，无请求-响应环路，无写端授权校验。
- **写端（网关）义务**：`instrument_id` 发布时校验唯一性；`price_tick` 必须 > 0；哨兵字段按上表填值；`product` 无法归类时填 `UNKNOWN` 不猜测。
- **读端（策略）义务**：SDK 仅放行不校验 payload；策略消费时自行校验 `frame_size >= sizeof(DzFrameHeader) + sizeof(payload struct)`，不足丢弃（截断帧防御）。

## 时序与触发

- **响应**：无同步响应帧，无 RTN。
- **触发场景**：
  - `TD_INSTRUMENT`：账户登录链内的合约全量查询（CTP `ReqQryInstrument` 回报逐条一帧）；查询完成（is_last）或失败时账户转 Ready。每日刷新（网关重登录/日切重查）同样重发全量——消费方按覆盖语义处理（同一 `instrument_id` 后到覆盖先到）。
  - `TD_INSTRUMENT_STATUS`：盘中场所推送合约交易状态变更（CTP `OnRtnInstrumentStatus`），无请求帧对应。
- **不响应** `QUERY_FULL_SNAPSHOT`（合约表无快照协议；策略在账户 Ready 前后被动接收全量）。
- 与 DZ_FRAME_ORDER_REPORT-DZ_FRAME_TRADING_ACCOUNT TD 推送帧不同，两帧**无 `seq`**：不参与《帧契约：TD 数据同步》的水位/回补/重置机制；丢帧自愈依赖每日全量重发。

## 镜像

- 不进 dzweb 镜像（后台进程间帧，dzweb 无 DZ_FRAME_TD_INSTRUMENT/DZ_FRAME_TD_INSTRUMENT_STATUS 消费；策略 SDK 对 DZ_FRAME_TD_INSTRUMENT/DZ_FRAME_TD_INSTRUMENT_STATUS 仅放行不解析 payload，策略用户经回调自取，见《帧契约：通用规则》§9）。

## 保留声明

以下为 v2 已定义、尚未进入帧协议的类型，本契约预留其语义位：

- `DZ_POSITION_EFFECT_AUTO`（`(DzPositionEffect)0`，`libs/strategy_api/include/dztrader/data_type.h`）：自动拆分平仓方向（零初始化即 AUTO），**已定义未实现**，拆分优先级序列由账户级配置决定；未实现期间网关收到 AUTO 语义委托显式拒绝（fail-closed），策略需显式传 OPEN/CLOSE。
- `DzInstrumentLeg`（组合合约腿，`product == DZ_PRODUCT_SPREAD` 时每腿一行）、`DzInstrumentExt`（合约扩展属性 K-V，场所特有参考信息合法出口；禁止承载交易决策必需字段）、`DzInstrumentTickTier`（阶梯最小变动价位，JPX 类场所）：**已定义未发布**——无对应帧号，经后续帧扩展引入；引入前 `DzInstrumentInfo` 主表为合约信息唯一载体。
