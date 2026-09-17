# 帧契约：合约信息

本契约规定合约信息的**查询化通路**：

- 按需刷新请求 `DZ_FRAME_TD_QUERY_INSTRUMENT`（策略 → td 网关，basic 广播帧，无响应）。

合约静态数据**不再走帧推送**：`DZ_FRAME_TD_INSTRUMENT` 与 `DzInstrumentInfo`/`DzInstrumentLeg`/`DzInstrumentExt`/`DzInstrumentTickTier`
已全链路删除、帧号释放（ADR 0011）；静态数据一律经**统一 td 库** `instruments` 表查询（`dz_db_query_instruments`）。
类型层真相源：`libs/tdstore/include/dztrader/tdstore/records.h`（`InstrumentRecord`）、
`libs/tdstore/include/dztrader/tdstore/schema.h`（TD schema v5）、
`libs/core/include/dztrader/core/core_struct.h`（`DzInstrumentQueryReq`）。总则见《帧契约：通用规则》。

## 1. 覆盖帧

| 帧 | payload | 逻辑方向 | 性质 |
|------|---------|----------|------|
| `DZ_FRAME_TD_QUERY_INSTRUMENT` | `DzInstrumentQueryReq` | 策略 → td 网关 | basic 广播帧，按 `payload.account_id` 路由；无响应 |

本帧为 basic 帧（仅 `DzFrameHeader`，无 `instance_id` 扩展头）、struct payload（编码规则见总则 §6）。
帧号登记：`DZ_FRAME_TD_QUERY_INSTRUMENT` 在 `libs/core/include/dztrader/core/core_data_type.h`。

## 2. 语义 / 数据流 / 路由

**数据流**（形态 5 后台进程间帧，总则 §4.2）：

```
登录（含重连重登）:  td 全量查合约 ─→ tdstore 落库统一 td 库 instruments 表   （不再广播合约帧）
按需刷新:            策略 dz_query_instrument(account_id, instrument_id)
                      └─ 写 DZ_FRAME_TD_QUERY_INSTRUMENT（basic 广播帧）
                           └─ td 按 account_id 命中已就绪会话 → ReqQryInstrument（带目标 symbol）
                                └─ OnRspQryInstrument → InstrumentRecord → 统一库 upsert
                                   （无响应帧；失败仅 td 日志，见 §9）
读数据:              策略（定时器延迟后）dz_db_query_instruments(db, instrument_id, fields)
                      └─ libs/db → tdstore 投影查询 → DzResultSet
```

- **发送方**：`DZ_FRAME_TD_QUERY_INSTRUMENT` 由策略 SDK `dz_query_instrument` 写（`libs/strategy_api/src/api.cpp`）。
- **接收方**：`DZ_FRAME_TD_QUERY_INSTRUMENT` 由 td 网关消费（按 `payload.account_id` 归属过滤；其他策略
  实例的 SDK 将其当平台帧丢弃）。
- **刷新请求定向解析与回写**：td 侧先查统一库现有行的 `symbol`（CZCE 人工消歧），无行则回退 `instrument_id`，
  再发 `ReqQryInstrument`；发起时登记 `symbol → instrument_id` 待回写映射，响应命中时以原 **平台 `instrument_id`**
  作 PK 更新原行（`rec.symbol` 保持响应场所码）→ 原行 `updated_at` 推进、不新增重复行；未命中（登录全量查询）
  行为不变。**不做自动消歧流程**（§5）。
- **前端入口**：无（后台进程间帧，无 REST/WS 入口）。
- **镜像**：不进 dzweb 镜像（§12）。

## 3. Payload

struct 引用（字段定义见对应头文件，本契约不抄写字段表）：

- `DZ_FRAME_TD_QUERY_INSTRUMENT` → `DzInstrumentQueryReq`（`libs/core/include/dztrader/core/core_struct.h`）：
  `account_id`（目标账户，必填，路由键）、`instrument_id`（目标合约，平台唯一键，必填）。

## 4. instruments 表字段（23 列 + 2 元数据）

真相源：`libs/tdstore/include/dztrader/tdstore/records.h`（`InstrumentRecord`）与
`libs/tdstore/include/dztrader/tdstore/schema.h`（`kTdSchemaVersion=5`；v4 = 4 rename + 5 add；v5 = 删除 margin_rates/commission_rates（ADR 0013））。
下表 23 列 + 元数据 2 列 = 25 列，即 `fields` 白名单全集（§7）。

| 区 | 列 | SQLite 类型 | 哨兵 / 约束 | 来源与填值 |
|---|---|---|---|---|
| 身份 | `instrument_id` | TEXT PK | 非空 | CTP `InstrumentID`（平台唯一键） |
| | `exchange_id` | TEXT | | CTP `ExchangeID`（平台注册表值） |
| | `symbol` | TEXT | | CTP `InstrumentID`（场所原生码；可 ≠ `instrument_id`，§5） |
| | `name` | TEXT | | CTP `InstrumentName`（落库前 GBK→UTF-8） |
| 分类 | `product_class` | INTEGER | 0=`DZ_PRODUCT_UNKNOWN` 兜底 | CTP `ProductClass` → `DzProductClass`（§6） |
| | `product_code` | TEXT | | CTP `ProductID` |
| | `settle_cycle` | INTEGER | -1=不适用；0=T+0；1=T+1；2=T+2 | CTP 固定 -1（衍生品） |
| 货币 | `currency` | TEXT | 空=跟随账户本币 | CTP 固定 `"CNY"` |
| | `base_asset` | TEXT | 空=不适用 | CTP 空 |
| | `is_inverse` | INTEGER | 0=线性；1=反向 | CTP 固定 0 |
| 量价 | `volume_multiple` | REAL | >0 | CTP `VolumeMultiple` |
| | `volume_step` | REAL | `<=0` 视为 1 | CTP 固定 `1.0` |
| | `price_tick` | REAL | >0（写端义务） | CTP `PriceTick` |
| 下单量 | `min_limit_order_volume` | INTEGER | `<=0` 未提供 | CTP `MinLimitOrderVolume` |
| | `max_limit_order_volume` | INTEGER | `<=0` 无限制 | CTP `MaxLimitOrderVolume` |
| | `min_market_order_volume` | INTEGER | `<=0` 未提供 | CTP `MinMarketOrderVolume` |
| | `max_market_order_volume` | INTEGER | `<=0` 无限制 | CTP `MaxMarketOrderVolume` |
| 窗口 | `listed_date` | INTEGER（天） | 0=`DZ_DATE_NA` | CTP `OpenDate`；**不保证为交易日** |
| | `delisted_date` | INTEGER（天） | 0=`DZ_DATE_NA` | CTP `ExpireDate`；通常即最后交易日（核对项 §11） |
| 期权 | `option_type` | INTEGER | 0=非期权 | CTP `OptionsType` → `DZ_OPTION_CALL`(1)/`DZ_OPTION_PUT`(-1)/0 |
| | `option_strike` | REAL | | CTP `StrikePrice` |
| | `underlying_id` | TEXT | 空=无标的 | CTP `UnderlyingInstrID` |
| | `underlying_multiple` | REAL | `<=0`=NA | CTP `UnderlyingMultiple` |
| 元数据 | `update_day` | TEXT | `"YYYYMMDD"`；交易日未知时为 `"00000000"` | td 当前交易日（`DzDate` → `YYYYMMDD`） |
| | `updated_at` | INTEGER | epoch ms | 每次 upsert 推进（刷新完成的观测点） |

`updated_at` 为**墙钟毫秒**，**非严格单调**：同一毫秒或系统时钟回拨时可能相等/回退；仅供观测
（刷新完成观测点，§10），**不作排序依据**（行序以 `instrument_id` 为准，§7）。

哨兵规则（写端义务；读端按哨兵判 NA/缺省）：

- **日期**：`listed_date`/`delisted_date` NA=0=`DZ_DATE_NA`（epoch day 0 = 1970-01-01 元旦，全球无开市，
  与真实业务日无碰撞；与 POD 零初始化语义重合）。期货/期权/权证/转债的 `delisted_date` 必填（不得 NA）；
  回测移仓**不得交易未上市合约**（`listed_date` 为 NA 时由策略自行保守处理）。两列均为**日期**，不保证是交易日。
- **下单量**：4 列 `<=0` 分别为"未提供"（min）/"无限制"（max）；不承载场所缺省推断。
- **`volume_step <= 0` 视为 1**（换算工具已收敛此哨兵，见 `libs/core/include/dztrader/instrument_util.h`）。
- **`settle_cycle`**：-1=不适用（衍生品），0=T+0；网关必须显式填值，不得依赖零初始化。历史 POD 零初始化陷阱
  已随合约结构体退役消除（`InstrumentRecord` 默认 -1、DB 列 `DEFAULT -1`）。
- **`currency`** 空=跟随账户本币；**`base_asset`** 空=不适用；`is_inverse` 0=线性、1=反向（反向合约币种取 `base_asset`）。
- **`underlying_multiple <= 0`** = NA（语义核对项 §11）。
- v3 物理列 `settlement_method`/`option_exercise_style`/`option_series` **保留但不承诺、不可查询**（§8）；
  upsert SQL 不写这 3 列，`INSERT OR REPLACE` 的整行替换语义使其在**任意 upsert/刷新后被重置为列默认值**
  （当前无消费方）。

## 5. 身份规则

`instrument_id` 是平台全局唯一键，订单/持仓/行情/合约等所有数据以它引用。三段式身份：

| 字段 | 语义 | 规则 |
|------|------|------|
| `instrument_id` | 平台唯一键 | CTP 合约用**裸交易所代码**（`rb2601`/`MA601`）；其他网关加**网关段前缀**（`IB.266004536`/`BNS.BTCUSDT`）保证跨网关唯一。唯一性由库表 PK 保证（同一 `instrument_id` 重复回报按覆盖语义 upsert）。 |
| `exchange_id` | 交易场所代码 | 平台注册表值（`SHFE`/`CME`/`BNS`/`BNF` 等），非展示字符串，参与路由与规则判定。 |
| `symbol` | 场所原生代码 | 网关对场所 API 发单/订阅时**原样透传**；仅展示与场所交互用。**`symbol ≠ instrument_id` 合法**（CZCE 手工消歧）；刷新请求的场所查询目标优先取库内 `symbol`，无行回退 `instrument_id`；响应按发起时登记的 `symbol → instrument_id` 映射回写**原行**（不新增重复行）；不做自动消歧流程。 |

- **禁止从 `instrument_id` 反向解析 `symbol`**：前缀仅为跨网关唯一性约定，非语法规则
  （`normalize_to_product` 类品种前缀提取仅适用于确认无歧义的裸码域，不得用于恢复 `symbol`）。
- `underlying_id`（期权/权证/可转债标的）必须是 `instruments` 表内有效行的 `instrument_id`
  （引用指数行合法，`DZ_PRODUCT_INDEX` 为非交易参考行）。

## 6. 数量语义与字典

平台单位全帧整数（`DzVolume` = int32 手数语义）：

- **平台单位**：期货/期权 = 手/张，证券 = 股，币圈 = `stepSize` 粒度。下单/持仓帧全部整数。
- **原生数量换算仅发生在网关边界**（网关把 `DzOrderReq.volume`（平台单位）换算为场所原生数量，反之把场所
  回报归一到平台单位），策略核心路径不做任何浮点换算。
- 换算基准为 `volume_step`（1 平台单位对应的原生数量；CTP 恒 1）。网关侧换算约定：`volume_step <= 0` 视为 1；
  原生→平台方向**向下取整到 step 网格**（浮点商误差容差 1e-9）。工具实现见 `libs/core/include/dztrader/instrument_util.h`。
- `volume_multiple`（价值乘数）**仅用于** PnL/名义价值浮点运算，不参与数量换算。

**`product_class` 字典**：取值域为 `DzProductClass`（`DZ_PRODUCT_*` 宏，`libs/strategy_api/include/dztrader/data_type.h`），
`UNKNOWN` 为兜底值——网关无法归类时填 `UNKNOWN`，**不得**以猜测的品种类型替代。CTP 映射
（`apps/ctp/td/td_ctp_mapping.cpp`）：`Futures`→`FUTURES`、`Options`/`SpotOption`→`OPTION`、`Combination`→`SPREAD`、
`Spot`→`SPOT`、其余（EFP/TAS/MI 等）→`UNKNOWN`。

## 7. DB 查询语义（`dz_db_query_instruments`）

签名（`libs/strategy_api/include/dztrader/api.h`）：

```c
DzResultSet* dz_db_query_instruments(DzDatabase* db, const char* instrument_id, const char* fields);
```

`db` 为 `dz_db_open` 打开的统一 td 库（`paths::td_db()` = `<DZTRADER_HOME>/db/td.db`，ADR 0012；只读打开，
库不存在时打开失败）。查询走 `libs/db` → `tdstore::query_instruments`，结果映射为 `DzResultSet`。

| 参数 | 语义 |
|------|------|
| `db` | NULL/无效句柄 → 返回 NULL，`dz_errcode()` = `DZ_EC_INVALID_PARAM` |
| `instrument_id` | NULL/`""` = 全部行；否则按 `instrument_id` **精确匹配**（无前缀/模糊匹配） |
| `fields` | NULL/`""` = 全部 25 个承诺列（**声明序**，见下）；否则逗号分隔列名，逐项去首尾空格/tab、空项跳过（`"a,,b"` 等价 `"a,b"`） |

返回语义：

- 返回列**顺序 = 请求顺序**；`fields` 为空时列序 = 白名单声明序（下表顺序）。
- 行序 = `instrument_id` **升序**（`ORDER BY instrument_id`）。
- 未知列名或**重复**列名 → 返回 NULL，`dz_errcode()` = `DZ_EC_INVALID_PARAM`（错误消息含具体列名）。
- 无匹配行 = 空结果集（非错误）：`dz_resultset_status` = 0、0 行，列元信息仍按请求填充（0 行可遍历列名/类型）。
- 列类型映射：TEXT→`DZ_COL_TYPE_STRING`、INTEGER→`DZ_COL_TYPE_INT64`、REAL→`DZ_COL_TYPE_FLOAT64`。
- 结果集由策略负责 `dz_resultset_close` 释放；遍历/取值 API 见 `api.h`。
- 仅投影 `instruments` 表承诺列；v3 保留列（`settlement_method`/`option_exercise_style`/`option_series`）不可选。

**查询量级提示**：全表查询（`instrument_id` 为 NULL/空串）在万级行下约 10²ms / 10¹MB 量级（含全部 25 列）；
建议按 `instrument_id` 精确查询或仅请求必需 `fields`（合约表为登录期全量 upsert，行数随在役合约增长）。

`fields` 白名单（声明序，共 25；与 `tdstore::instrument_columns()` 一致）：

`instrument_id`, `exchange_id`, `symbol`, `name`, `product_class`, `product_code`, `settle_cycle`, `currency`,
`base_asset`, `is_inverse`, `volume_multiple`, `volume_step`, `price_tick`, `min_limit_order_volume`,
`max_limit_order_volume`, `min_market_order_volume`, `max_market_order_volume`, `listed_date`, `delisted_date`,
`option_type`, `option_strike`, `underlying_id`, `underlying_multiple`, `update_day`, `updated_at`。

标准用法（策略自管延迟/重试/超时；契约 account-status 的"重试由策略自管"同理）：

```c
dz_query_instrument(ctx, account_id, instrument_id);          /* 写帧即返回，无响应 */
/* 定时器延迟后（dz_schedule_after，见契约 strategy）再查：*/
DzResultSet* rs = dz_db_query_instruments(db, instrument_id, "symbol,price_tick,volume_step");
```

## 8. 不采纳字段（防反复）

| 字段 | 去向/理由 |
|------|-----------|
| StartDelivDate / EndDelivDate / DeliveryYear / DeliveryMonth | 投机平台不交割；可交易窗口由 `listed_date`/`delisted_date` 两列覆盖 |
| CreateDate、MaxMarginSideAlgorithm、CombinationType、ExchangeInstID | 无消费方 |
| InstLifePhase / IsTrading | 盘中动态数据，不入静态表 |
| PositionType / PositionDateType | 由 `OffsetConvertMode`（账户/交易所级配置）承载 |
| LongMarginRatio / ShortMarginRatio | 费率数据，不入合约表（费率能力已删除，ADR 0013） |
| `settlement_method` / `option_exercise_style` / `option_series` | 物理列保留、不承诺、不可查询；任意 upsert/刷新重置为默认值（当前无消费方）；对应功能落地时启用 |

## 9. 校验（仅写与总则不同的规则）

- `DZ_FRAME_TD_QUERY_INSTRUMENT` 为单向请求：无响应帧、无 RTN。失败路径：
  - 写端（策略）：`ctx`/`account_id`/`instrument_id` 缺失或空 → `dz_query_instrument` 返回 false，
    `DZ_EC_INVALID_PARAM`；
  - td 端：帧长不足（`< sizeof(DzFrameHeader) + sizeof(DzInstrumentQueryReq)`）、空 `account_id`/`instrument_id`、
    账户未配置、会话未 Ready、`ReqQryInstrument` 发起失败 → WARN 日志丢弃，不落库、无通知。
    （`-3` 流控自动重试仅登录链路全量查询；刷新请求失败不重试，由策略按需重发。）
- **写端（td）义务**：`instrument_id` 唯一；`price_tick > 0`；`delisted_date` 期货/期权不得 NA；哨兵字段按
  §4 填值；`product_class` 无法归类时填 `UNKNOWN` 不猜测；`name` 落库前 GBK→UTF-8；每次 upsert 推进 `updated_at`。
- **读端（策略）义务**：合约数据一律经 `dz_db_query_instruments`（不解析任何合约结构体）。

## 10. 时序与触发

- **响应**：刷新请求**无同步响应**，完成观测点 = 目标行 `updated_at` 推进（重新查询可见）。策略建议
  `dz_query_instrument` → `dz_schedule_after` 延迟 → `dz_db_query_instruments`；失败/重试/超时由策略自管。
- **触发场景**：
  - `DZ_FRAME_TD_QUERY_INSTRUMENT`：策略发现合约信息缺失/新上市/疑似修正时，按需刷新**单个**合约
    （全量刷新属 td 登录链路内部行为，不暴露给策略）。
- **登录链路全量落库**：账户登录（含重连重登）在结算确认后全量 `ReqQryInstrument`，回报逐条经 tdstore upsert
  统一库（`insert or replace`，`updated_at` 每次推进）；查询完成或失败驱动账户状态机（契约 account-status）。
- **不响应** `QUERY_FULL_SNAPSHOT`（合约信息无快照协议；数据在库中随时可查）。
- 本帧**无 `seq`**：不参与《帧契约：TD 数据同步》的水位/回补/重置机制。

## 11. 已知边界与核对项

1. `symbol ≠ instrument_id` 合法（CZCE 手工消歧）；刷新解析优先取库内 `symbol`，无行回退 `instrument_id`；不做自动消歧流程。
2. **核对**：CTP `ExpireDate` 是否等于最后交易日（实盘抽查 rb/IF/IO/m/SR/si + 1 期权），例外写回本契约。
3. **核对**：`UnderlyingMultiple` 语义（CFFEX 指数期权 / 商品期权各一例）。
4. `OffsetConverter` 已实现未接入下单路径（独立待办，见 ADR 0008）。
5. SDK `.so` 分发形态的 PIC 缺口（独立待办）。
6. 旧 dev 库清理：按 td 进程分库的旧文件不再被读取（路径变更见 ADR 0012）；统一库 `db/td.db` 首次打开自动迁移到 schema v5。
   清理旧网关库：`rm -f $DZTRADER_HOME/flow/*/*.db`（仅旧网关库 `.db`；`$DZTRADER_HOME/flow/<gw>/`
   下 CTP 流文件不动——不确定时先 `ls` 确认）。
7. **核对**：CZCE 响应 `InstrumentID` 形态（3 位场所码 vs 4 位消歧码）——决定刷新回写关联是否命中；
   未命中时空操作、无副作用。
8. 第 2、3、7 条在代码中以 `TODO(ctp-verify)` 标记（`grep -rn "TODO(ctp-verify)" apps libs`），完成后随结论一并移除。

## 12. 镜像

- 不进 dzweb 镜像（后台进程间帧；dzweb 不消费 `DZ_FRAME_TD_QUERY_INSTRUMENT`）。
- `DZ_FRAME_TD_QUERY_INSTRUMENT` 是 SDK **写端帧**（由 `dz_query_instrument` 发出），非读端白名单成员（见契约 strategy）。

## 13. 运维约束

- **进程运行中禁止删除/替换/还原覆盖 `db/td.db`**（`rm`、`mv` 覆盖、从备份 `cp` 回滚等）：写连接持有旧 inode
  继续写已被替换的文件，而新读者打开的是新路径文件，两侧看到不同数据（裂脑），无自动收敛机制。
  清库/还原/备份回滚必须停全部 td 与策略进程后操作。
- **升级次序**：先启动 td（打开统一库完成 v5 迁移）再启动策略；反向次序下策略可能以旧 schema 打开库，
  新查询可能遇旧 schema 短暂返回 NULL，迁移完成后恢复。
- **新旧 td 不得混跑**：v5 迁移后旧版 td 打开新库会在准备费率语句时失败退出；新 td 迁移时旧 td 的
  预编译语句失效、持久化批整批丢弃——升级须先停旧 td 再启新 td。
