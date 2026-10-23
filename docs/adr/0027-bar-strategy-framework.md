# ADR 0027: BarStrategy 框架（bar 级 C++ 策略框架）

## Status

Proposed（2026-10-20 初稿；2026-10-21 设计评审多轮修订：品种数据与策略上下文合并为一个对象（决策 4）、ctx 自包含执行（决策 3/4）、账户模型改为「单逻辑账户 + N 挂靠账户」（[ADR 0028](0028-strategy-process-logical-account.md) 重写）、挂靠缩放/同步/回放虚拟成交口径（[ADR 0030](0030-attached-account-scaling.md)）、委托 effect 统一走 AUTO；本文为框架总纲 ADR，执行细节以组件文档为准，后续增量变更拆新 ADR；待评审，通过后改 Accepted）

## Context

- 目标用户是从交易开拓者（TB）与 MultiCharts（MC）迁移过来的传统程序化策略开发者：一个策略关心一个品种、一个周期，按 bar 驱动，习惯序列变量与显式价下单。
- 现有策略 SDK 只有 tick 驱动：`strategy_engine.h` 按帧分发，`StrategyBase` 是 11 个回调的空默认实现；没有 bar、没有序列变量、没有多品种管理。
- [ADR 0018](0018-strategy-side-kline-and-trading-sessions.md) 已决定：K 线不在引擎/帧层生成、不做 SHM bar 推送，且不改 `StrategyBase`；tick→bar 合成本身由 [ADR 0031](0031-tick-bar-synthesis-component.md) 定稿为策略进程与数据存储进程共用的 header-only 组件。
- 用户需求（2026-10-20/21 设计会话）：框架同时管理多品种；一个策略进程绑定一个逻辑账户，实体账户 0..N 个按比率动态挂靠（[ADR 0028](0028-strategy-process-logical-account.md)）；品种可动态增删；历史数据由平台提供（归档接口已存在、当前返回空）；将来支持回测；C++ 与未来 Python API 形状一致；性能优先——单进程常规策略运算 ≤ 1us。
- TB/MC 语义事实（同次调研）：TB 历史每 bar 一次最终 OHLC、实盘 `OnBar` 每 tick 触发、`OnBarClose` 是边界事件；classic TB 手册明确禁止写序列变量历史位（C0115），TBQuant 官方论坛亦称禁止，MultiCharts 未取得官方原文（按禁止处理）；TB 采用显式价下单、开平/锁仓由平台自动解析（AUTO）；classic TB 在 bar 0 上回溯不足时把 `Close[1]` 钳位到当前值，TBQuant 用 `SetBackBarMaxCount` 预分配、MC 用 `MaxBarsBack` 声明所需回溯。
- 2026-10-21 设计评审新增约束：回溯不足必须**响亮失败**（用户选择异常，不钳位、不使用 NaN/初值兜底）；回卷必须移出盘中热路径（选择 A'：盘后主动回卷）；`max_lookback` / `reserve_bars` / `warmup` 全部可省略并由框架按周期自动计算；账户模型为「单逻辑账户 + N 挂靠账户」（[ADR 0028](0028-strategy-process-logical-account.md)），执行与记账口径见 [ADR 0030](0030-attached-account-scaling.md)。
- 委托 effect 以 `DZ_POSITION_EFFECT_AUTO` 为主（策略只表达买卖方向与手数，开平/锁仓由 td 按账户解析）；平台 AUTO 专项见 issue #82，是实盘联调的硬前置。
- [ADR 0026](0026-strategy-sdk-nlohmann-json.md) 解决 JSON 配置的第三方依赖。

## Decision

1. **两层职责**：`BarStrategyManager<Strategy>`（下称 A）负责配置、数据、订阅、执行与动态管理；用户策略类型 `Strategy`（下称 B）只写策略逻辑。B 与 A 之间是编译期鸭子类型（与 `run_strategy` 同一风格），不要求继承。入口 `run_bar_strategy<Strategy>(config)`；A 对引擎而言是唯一的策略对象（实现 `on_start` / `on_tick` / `on_schedule` / `on_ui_input` / `on_stop` 等），内部由 `run_strategy` 驱动。A 在 `on_start` 取得 `DzContext*`，并用 SDK 定时器接口注册自己的盘后回卷定时器（`dz_schedule_daily(18:00)`）；B 不接触引擎回调与 SDK 定时器。
2. **实例 = 品种；进程绑定一个逻辑账户**：实例集合 = 配置的 `instruments` 列表，每个品种一个实例。一个策略进程只对应一个**逻辑账户**（纯虚拟台账，不落地、不参与订单路由），实体账户 0..N 个按挂靠比率复制逻辑持仓（[ADR 0028](0028-strategy-process-logical-account.md)）；策略逻辑看不见实体账户，也没有账户级实例、账户级数据隔离或账户级回放。一个进程一个故障域。
3. **B 契约**：6 个可选回调 `on_init / on_bar_open / on_bar_update / on_bar_close / on_destroy / on_params_update`，统一参数 `InstrumentContext&`；B 不接收交易回报，只读策略逻辑持仓。B **不接触账户身份**（`ctx.account_id()` 移除）：B 只表达策略手数，挂靠账户的手数缩放、同步与上报都在 A/ctx 内部完成（[ADR 0030](0030-attached-account-scaling.md)）；四个交易方法是开平意图（订单 effect 由 AUTO 决定），规则见决策 9。A 在每次回调外围捕获异常：只把该实例标记为失败并记 ERROR，进程与其它实例继续（异常不穿越引擎/C API 边界）。
4. **数据组织**：`instruments_ = unordered_map<string, InstrumentNode<Strategy>>`（节点地址稳定）；节点 = `InstrumentContext`（品种数据 + B 的 API 面，同一对象）+ 内联 B 实例。`InstrumentContext` 直接持有价格数组、用户序列、共享 `WindowState`、`bar_index`、合约属性（`InstrumentSpec`）、虚拟持仓（`PositionState`）、最近 tick（`TickSnapshot`）、缩放配置副本（`ScalingConfig`）、引擎句柄（`DzContext*`）、进程级参数指针（`ParamStore*`）与**挂靠账户表的只读指针**（表由 A 持有、只在两次回调之间修改），**没有 `InstrumentData` 中转对象、没有指向 A 的反向指针、没有账户身份副本**：品种数据与上下文生命周期完全一致、读者唯一。B 只见 public 的只读 API 面，A 通过 friend 做创建注入；**下单执行在 ctx 内完成**（按逻辑持仓差量算每账户手数、对齐/夹取后直调 `dz_place_order`），A 负责账户表维护、生命周期命令与回放后的批量上报。节点在 `unordered_map` 内 `try_emplace` 就地构造，ctx 不可搬移、B 构造后不再搬移。执行门禁只有 ctx 内的两个开关（`live_` / `failed_`）。没有账户实例、账户级数据副本或订单路由表。地址稳定性与热路径约束见组件文档。
5. **Series 层与读语义**：集中管理（`WindowState` + `unique_ptr<T[]>`）。只读基类 `SeriesView<T>`（仅 `base_` / `window_` 两个指针）承载全部读接口，`Series<T>` 派生自它、只增加「写当前 bar `[0]`」；单继承、无虚函数、不新增数据成员（`sizeof` 与基类相同、平凡可拷贝）。**每个品种一个 `WindowState`，价格序列与用户序列共用**（单逻辑账户下两者永远同步推进，不存在相位差）。读接口两种口径：`[n]` 为 TB 口径（`[0]` = 当前 bar，`[n]` = 往回 n），`size()` / `max_lookback()` / `span()` 为 STL 口径（`span` 时间正序、长度 = `size`，可直接喂算法）；**不提供 vector 写接口**（无 `push_back`/`erase`/`resize`/`reserve`），只写 `[0]`。**读语义只有一种**：`n ∉ [0, size)` 一律抛 `std::out_of_range`，不钳位、不产生 NaN、不返回初值；`span()` 是唯一的零检查快路径；**不提供 `[]` + `at()` 双版本**。Debug 构建在抛前加 `assert`。回调门禁（决策 7）保证回调期 `size == max_lookback`，因此正常策略只在「`max_lookback` 配置过小」或「`on_init` 里读序列」时遇到该异常——两者都该响亮失败。用户序列在静默填窗阶段由 A 写入 `init`，第一个回调时历史位即 `init`（carry 语义起点）。可读深度 `max_lookback` 进程级、兼作「策略声明所需最大回溯」，实例创建时一次性分配、**永不扩容/realloc**。物理窗口始终连续，**不是环形缓冲区**：推进用索引滑动，物理缓冲用尽时整窗搬回数组头部（回卷，决策 6）。绝对 bar 序号用 `ctx.bar_index()`（0 起、单调、回卷不重置；classic TB `CurrentBar` 同口径），存于品种级 `InstrumentContext`。指标函数统一收 `const SeriesView<T>&`，无需模板化。
6. **容量与回卷（A'）**：物理缓冲 `buffer_bars = max_lookback + reserve_bars`；`max_lookback` 默认 `clamp(86400/period_sec, 32, 2048)`（一个 24 小时日的 bar 数），区间 `[32, 16384]`；`reserve_bars` 默认 `max(2 × 86400/period_sec, R_min)`、`R_min = max(2, ceil(1.1 × 86400/period_sec))`，配置可省略，显式值须在 `[R_min, 8192]`；`buffer_bars ≤ 24576` 否则配置错误。**回卷 = 纯布局变换**（窗口 `memmove` 到数组头、`end = size`），不改值、不改 `size`/`bar_index`，任何时刻合法、只能发生在两次回调之间。v1 主路径是每天 18:00（日盘收盘后、夜盘开盘前的静默窗）由 A 的定时器主动回卷（覆盖该品种 7 条价格序列与该实例全部用户序列）；定时器错过时在推进前内联兜底（只影响性能，不影响正确性）。不做逐小时/每 max_lookback 根定时回卷。物理数组一次分配、自然对齐，不做 2 的幂取整或 cache-line padding。
7. **事件语义**：历史加载与回测共用「合成 tick」：每根 bar 拆成 4 个合成 tick（上涨 O→L→H→C，下跌 O→H→L→C，平盘同 O→L→H→C，量按 k/4 线性累计），每笔 tick 触发一次 `on_bar_update`，首笔额外触发 `on_bar_close(上一根) → on_bar_open`，即每根 bar 4 次 update、与实盘同构；实盘由真实 tick 驱动，新 bar 首笔顺序为 `on_bar_close(上一根) → 窗口推进并以本笔 tick 播种 → on_bar_open(新 bar) → on_bar_update`，故 `on_bar_open` 内 `open/high/low/close` 已等于首笔 tick 价（TB 的 OnBarOpen 语义）；其后每笔 tick `on_bar_update`。**回调门禁**：价格窗口填满（`size == max_lookback`）之前不调用任何 B 回调（静默填窗；价格直接写最终 OHLC，用户序列写 `init`），第一个回调发生在 `bar_index == max_lookback` 的那根 bar（此前 `max_lookback` 根静默填充，索引 `0..max_lookback-1`）；历史回放同样先静默填满再带回调回放剩余 bar。冷启动（无归档数据）因此要等 `max_lookback` 根 live bar 才开始回调。
8. **bar 边界**：由 tick→bar 通用组件（[ADR 0031](0031-tick-bar-synthesis-component.md)）实现：tick 的 `DzDate`（真实交易日）+ 交易日内秒网格定边界，交易小节/竞价/1 小时/1 日口径见该 ADR；**不使用定时器定义 bar 生命周期**（回卷定时器只做布局，不关闭/开启 bar）。
9. **交易与执行**：v1 只支持限价单，B 通过 `ctx.buy/sell/sell_short/buy_to_cover(volume, price)` 表达意图（四函数是 TB/MC 开平意图：`buy`/`sell_short` 反向时先平对手仓再按参数建仓，`sell`/`buy_to_cover` 只减不增、超出逻辑持仓钳制，`volume=0` 只平不建/全平；订单 effect 仍统一 AUTO）；**B 的唯一持仓口径是「策略逻辑持仓」**——虚拟台账按 B 的下单意图即时记账（`P` 按开平规则推进；净持仓、均价、建仓次数、浮动盈亏、进出场时间），不等待成交、不跟踪在途；B 不接触账户真实持仓/资金/权益，也不接触任何委托与成交明细（`order_id`、委托状态、部分成交、拒单原因都不出现在 B 的 API 面）；**成交/委托/持仓/资金回报 v1 全部忽略**（不建成交台账、不跟踪委托；逻辑账只由 B 的意图推进）。**执行在 ctx 内完成**：ctx 持引擎句柄、合约属性、虚拟持仓（`PositionState`）、最近 tick（`TickSnapshot`）、缩放配置副本（`ScalingConfig`）与 A 的挂靠账户表只读指针，按逻辑持仓差量算每账户手数、`price_tick` 对齐、涨跌停价格夹取（挂涨跌停价而非拒单）并调 `dz_place_order`（effect 固定 `DZ_POSITION_EFFECT_AUTO`，开平/锁仓由 td 按账户解析）；**A 不参与下单**，只维护账户表与批量上报。挂靠与同步口径见 [ADR 0030](0030-attached-account-scaling.md)：正常单不看实际持仓、不做差额吸附；`sync/nosync` 只出现在 `account add/set/del` 与 `instrument del` 命令上。**回放期订单只做虚拟成交**（更新逻辑持仓与 Tier 1，不落地、不逐笔上报），回放完成后一次性上报各账户逻辑持仓。撤单、委托管理、动态挂单不在 v1。v1 直接发 `DZ_POSITION_EFFECT_AUTO`，不因 td 未实现而降级/加开关；td 修复前实盘订单会被拒，属已知限制（#82 是实盘联调前置，不阻塞框架开发与合并）。B 的策略持仓为**净持仓模型**；`long*/short*` 分向系列属于架构级后置。
10. **历史预热与锚点**：配置 `warmup` 二选一：`{"bars": N}`（默认，`N` 默认 10,000、自动提升到 ≥ `max_lookback`、上限 10,000,000）或 `{"from": "YYYY-MM-DD"}`（按起始日期）。A 用平台接口 `dz_db_query_bar` 取数（归档域未实现，当前恒返回 NULL / `DZ_EC_NOT_IMPLEMENTED`，按「无历史数据」处理，不是错误）。**锚点** = 该品种写入窗口的第一根 bar（进程启动时的历史最早一根，或 `instrument add` 时该品种历史的最早一根）；回放从锚点开始，按品种顺序推进，**不存在跨实例回放分叉、视图重绑或 DB/内存拼接问题**。`bars` 模式按每日 bar 数下限估算并回推起始日期，查询后裁剪为最近 N 根；可用历史不足 `max_lookback` 根时先静默填入可用部分、余下从 live bar 继续填满（INFO）。归档可用但回放序列不完整/与时间标注不符时，按归档数据源契约处理（Bar 字段与 `is_final` 见 [ADR 0031](0031-tick-bar-synthesis-component.md)；归档列契约随 #4；当前默认记 WARN 并继续）；v1 归档不可用 → 降级预热（只用 live bar 静默填窗）并打 WARN。**配置不另设历史段设定**（起始日期/回放深度由 `warmup` 承担）。启动时 A 先对全部品种 `dz_subscribe`，再查历史回放；回放完成后按 `Bar.is_final`（[ADR 0031](0031-tick-bar-synthesis-component.md)）排空缓冲 tick 进入 live（细节见组件文档 §5.3）。
11. **配置**：`$dz_strategy_home()/<strategy_id>.json`（`dz_strategy_home()` = 策略可执行文件目录；`strategy_id` = exe stem）；A 启动读取、缺失时自动生成**最小默认配置**并非 0 退出、UI_INPUT 命令成功后原子写回；WebUI 不得直接读写策略配置。字段：`period_sec`、`instruments`、`params`、`accounts`（挂靠账户 `{account_id, scale}` 列表，可为空）、`max_scale`（可选，默认 1.0；启动加载一次、盘中不可变）、取整策略字段（`scale_rounding` / `scale_min_one`）；`max_lookback` / `reserve_bars` / `warmup` **均可省略**（按决策 6/10 自动计算；生成默认配置不写这几个可选字段）。删除 `account_id`：账户身份不再是进程属性（[ADR 0028](0028-strategy-process-logical-account.md)）。显式值按区间校验，不合法一律**报错退出、不静默修正**。启动打印一行「有效配置」日志（含每个值及其来源），让人看到框架替用户选了什么。调试/Release 不额外区分配置文件；回测模式的配置扩展位预留。
12. **参数**：`ctx.param<T>(name, default)` 在 `on_init` 取值；动态修改时 A 调用 `on_params_update(ctx)`，B 重新读取并更新成员；A 只做运输，参数名/类型/默认值由 B 决定。v1 参数**全局统一**（每个品种相同），由 manager 持有唯一一份，`ctx` 持指针读取。策略手数就是普通参数（如 `volume`）；挂靠比率是账户配置、不是策略参数，框架按它换算每账户手数（[ADR 0030](0030-attached-account-scaling.md)）。品种级参数覆盖后置。
13. **动态管理**：v1 支持两类运行期命令：品种 `instrument add|del|list`（新增品种加载自己的历史并按锚点回放预热后进入 live；删除品种支持 `[sync|nosync] [market]`，`sync` = 按命令引起的 `L_i` 变化量对每个账户发差额单、不判断逻辑持仓，随后销毁实例并清逻辑持仓）；挂靠账户 `account add|set|del|list`（修改挂靠关系/比率、立即重算并上报逻辑持仓、可选 `sync` 顺手发同步单、原子写回配置，见 [ADR 0028](0028-strategy-process-logical-account.md) 与 [ADR 0030](0030-attached-account-scaling.md)）。命令 payload 是字符串（`UI_INPUT`/`OUTPUT_UI`），框架侧实现解析与字符串结果；dzweb 入口/权限/请求关联归 #6/#9。账户的登录/凭据/交易接口归属属于平台，框架只持有账户标识与比率。一键同步/账户级头寸监控是平台/WebUI 后续能力。写盘先成功再改内存。
14. **性能目标**：Release、单品种、单挂靠账户、`max_lookback=500`、双均线策略；tick 路径与 bar close 路径 p99 ≤ 1us（不含 IPC 等待与 shm 写入）。tick 热路径没有账户级 fanout（fanout 只发生在交易意图与命令的冷路径，N 通常 1–3）；回卷不在热路径（每天 18:00 定时器内一次，成本进基准报告）；预热回放单列吞吐/启动时长（按实例数 × `warmup` 深度估算并打 WARN 阈值）。先输出基准报告，稳定后升为 CI 硬门禁。1us 指 A 框架计算路径（不含 SDK 帧解析/shm 写/B 自身复杂逻辑）；交易意图 fanout（含 shm 写）与“决策→订单帧落 shm”单列预算，见组件文档 §10。
15. **与既有体系**：不修改 `StrategyBase` / `strategy_engine.h`；订单帧不变（`DzOrderReq.account_id` 填挂靠账户，`position_effect` 填 `DZ_POSITION_EFFECT_AUTO`；AUTO 解析的契约语义随平台专项 issue #82 补齐）；《帧契约：策略》`SET_LOGICAL_POSITION` 改为按挂靠账户上报逻辑持仓（`account_id` = 挂靠账户，`net_volume` = 逻辑持仓），帧结构与字段语义不变（策略上报的仍是「自己认为应持有的仓位」，只是按账户各报一条），dzweb 消费接线时在契约里补这条口径；`net_volume` 已对齐 `DzLargeVolume`（ADR 0029）；公开类型别名 `dztrader::Volume` / `dztrader::LargeVolume` 定义在 `data_type.h` 末尾的 C++ 段（`#ifdef __cplusplus`），C 层仍用 `DzVolume` / `DzLargeVolume`；新增公开头 `dztrader/series.h` 与 `dztrader/bar_strategy.h`（最终文件名在实施时确定）。
16. **功能分期原则**：判据是「改这个决定是否要动数据模型 / 事件模型 / 所有权与生命周期 / 执行边界 / 身份与配置 / 性能路径」。要动的一律算架构级，v1 一次定对；只加只读查询、纯函数或配置项的功能可以后置。v1 已含：抛异常读语义、A' 盘后回卷、回调门禁、锚点预热、品种动态增删、挂靠账户动态增删改、单逻辑账户 + N 挂靠账户模型；回放期虚拟成交。v1 内置 Tier 1 持仓/进出场状态查询；Tier 2 交易统计后置；Tier 3（`Portfolio_*`、信号索引、`long*/short*` 分向系列）不做。只在 v1 已有两个具体实现的地方建接口，不做通用事件总线、插件注册或泛型策略框架等投机抽象。

## Alternatives

- **引擎层内建 `on_bar_*`**：ADR 0018 已否决（引擎从帧分发器变为市场语义组件）；本 ADR 沿用该结论。
- **B 继承 `StrategyBase`**：需要把 bar 语义塞进 tick 回调体系，且强制继承与现有编译期鸭子类型风格冲突；否决。
- **每个实例上下文堆分配（`unique_ptr<Context>`）**：热路径每 tick 多一次指针跳转；可用「品种节点在 `unordered_map` 内就地构造、地址稳定」替代；否决。
- **`InstrumentData` 与 `InstrumentContext` 分开、ctx 持 `data*`（初稿）**：该拆分与指针是为「多账户共享同一份品种数据」而设；采用单逻辑账户 + 挂靠账户后每品种恰好一个 B 实例，数据没有第二个读者、生命周期与上下文完全重合，指针只剩一个生命周期不变式和一次间接，视图也仍要绑定到别处。合并为一个对象后 B 只见公开只读面、A 用 friend 读写内部，地址稳定性照旧由 map 节点提供（`try_emplace` 就地构造、对象不可搬移）。否决。
- **ctx 持 A 指针 / 虚接口宿主（会话中讨论过）**：下单所需的输入只有引擎句柄、挂靠账户表、合约属性、虚拟持仓、最近 tick、缩放配置和进程级参数，全部可复制或已在 ctx 内；A 侧不参与下单、不建成交台账。引入反向指针/虚接口只会多一层生命周期耦合与一次间接，没有收益。否决。
- **单进程多账户（本 ADR 初稿方向，账户 = 实例维度）**：账户级实例集合、账户间数据隔离、迟到账户回放与视图拼接、每 tick 广播、账户 CRUD 都在框架内，策略还要处理账户维度；本 ADR 采用「逻辑账户 + 挂靠账户」（[ADR 0028](0028-strategy-process-logical-account.md)），用一本放大的虚拟台账满足同样需求。否决。
- **物理主账户 + 挂靠（用主账户成交反推逻辑持仓）**：逻辑身份依附实体账户，主账户被移除/故障/换绑即丢逻辑身份，逻辑持仓还会被单账户的拒单与部分成交污染；TB 的虚拟账户不依附任何实体账户。否决（见 [ADR 0028](0028-strategy-process-logical-account.md)）。
- **框架不做挂靠（策略自己按参数缩放 + 多进程部署）**（本 ADR 上一版方向）：动态挂靠、统一逻辑持仓、UI 视图与同步都做不到，账户间一致性靠模板/脚本与运维纪律；只能作为阶段性简化，不能作为终态。否决为终态。
- **环形缓冲推进序列（取模环形）**：推进 O(1) 但物理不连续，`std::span` 与第三方算法不可用，指标循环无法向量化；否决。备选是**镜像环形**（每根 bar 收盘多写一份影子位，无搬移、连续性与读成本不变），接口不变，作为升级路径记录在组件文档 §10/§14；触发条件是秒级以下周期或 p99.9 / max 门禁。
- **回卷策略：每 max_lookback 根在收盘路径搬移（方案 A）**：搬移尖峰（1 分钟默认配置下整窗搬移约 6MB、60–130µs）会落进某根 bar 的收盘路径，实例多时叠加，p99.9 / max 不可控；否决。**逐小时定时回卷**搬移量 ×24 且收益为零，否决。采用 A'：每天 18:00 静默窗回卷 + 推进前兜底。
- **回溯不足钳位 / 返回 NaN / 返回序列初值**：三种做法都会把「配置过小/读太深」变成静默错误状态，且钳位值可能直接产生成交（TB 的已知行为）；与「响亮失败」目标冲突，否决（组件文档 §12 列为刻意差异）。
- **`[]` 与 `at()` 双读接口**：两个入口让调用者猜快路径，且 TB/MC 迁移代码里 `[]` 无处不在，把正确性交给调用者选择没有收益；否决，改为单一检查版 `[]` + 零检查 `span()`。
- **容量取 2 的幂 / cache-line padding / 4KB 同余规避**：A' 的热路径无取模、无依赖容量的位运算，2 的幂无收益；回卷一天一次，对齐收益在测量噪声内；2 的幂反而有 4KB 同余导致的 L1 set 冲突风险。否决（组件文档 §10 记录定量理由与 arena 布局的未来约束）。
- **事件流 + 外部时序库（Kungfu 路线）**：策略只收单根 `Bar` 事件（无 `[n]` 回溯），历史交给 mmap 追加日志 + SQLite 索引按时间 seek，无 series/预热/兜底层；TB/MC 迁移者要重写全部指标状态，与本 ADR 的迁移目标冲突；否决。
- **定时器关闭 bar**：引入时钟依赖与会话语义；维持 tick 驱动（会话尾部在下一 tick 关闭，口径见 [ADR 0031](0031-tick-bar-synthesis-component.md)）；否决。
- **历史按 TB 语义一次性喂最终 OHLC**：与实盘事件形状不一致，策略在历史/实盘行为分叉；否决（差异写入迁移表）。
- **B 可直接操作实际持仓/委托**：破坏「B 只写逻辑、A 负责执行」的边界，且与未来算法下单冲突；否决。
- **配置用命令行参数/环境变量**：现有策略入口明确不用 argv/stdin，且动态修改需要写回文件；否决。

## Consequences

- 新增组件设计文档 `docs/components/bar_strategy.md`、策略开发者指南与 TBQuant/classic TB/MultiCharts 迁移对照表。
- 语义差异（迁移必须知道）：**策略从 `bar_index == max_lookback` 起才开始收到回调**（此前 `max_lookback` 根静默填窗；TB 从 bar 0 起用钳位值回调）；回溯不足**抛 `std::out_of_range`**（不钳位、不产生 NaN、不返回初值）；历史按合成 tick 回放（非每 bar 一次最终 OHLC）；B 无 `BarStatus` 枚举（三回调替代；bar 自身的 `is_final` 见 [ADR 0031](0031-tick-bar-synthesis-component.md)）；`MaxBarsBack` 由配置 `max_lookback` 承担（`warmup` 另管回放深度）；B 不感知成交与账户实际持仓；涨跌停采用挂单而非拒单；v1 无撤单；**委托 effect 统一 AUTO**（订单层只有方向/手数/AUTO，开平/锁仓由 td 按账户解析；逻辑账层按 TB/MC 开平意图推进 `P`，`volume=0` 只平不建/全平；反手/全平是一张 AUTO 单由 td 拆平/开，没有委托级开平推导、框架拆单或基于实际持仓的超出可平量丢弃）；**TB 的 `Buy(0)`/`SellShort(0)`（系统默认手数）不支持**：本框架入口函数 0 = 只平反向、不建仓（MC 口径），迁移必须显式给手数；挂靠账户的手数缩放/同步/回放虚拟成交口径见 [ADR 0030](0030-attached-account-scaling.md)。
- **AUTO 已知限制**：v1 直接发 `DZ_POSITION_EFFECT_AUTO`；td 侧未实现（#82）前实盘订单会被拒，框架逻辑账照常推进，实盘/逻辑偏差由操作员一键同步处理；#82 是实盘联调前置，不阻塞框架开发与合并。
- **Tier 1 口径已定稿**（组件文档 §6.1）：`current_contracts` 非负、`position_profit`/`contract_profit` 乘 `volume_multiple`、极值在 bar 收盘前更新并在平仓后归零、`entries` 按开仓段计数；`max_contracts` 只保留历史峰值口径（与 TB `MaxContracts`/`MaxContractsHeld` 有差异，写入迁移表）。
- **bar 合成已定稿**（[ADR 0031](0031-tick-bar-synthesis-component.md)：tick→bar 口径、交易小节/竞价、`is_final` 续接）；仍后置的是归档域历史 bar 数据源（#4）与 B 的日期/时间/报价查询；`dz_db_query_bar` 当前为 stub，v1 回放路径可降级为冷启动。
- **命令面**：字符串 payload（`UI_INPUT`/`OUTPUT_UI`）；框架侧解析/执行/字符串结果，dzweb 入口/权限/请求关联与 `OUTPUT_UI` 消费归 #6/#9。删除账户/品种与进程退出不撤单、不等待成交，操作员负责外部撤单与账户实际仓位处理。
- 账户边界（[ADR 0028](0028-strategy-process-logical-account.md)）：一个策略进程 = 一个逻辑账户 + 0..N 个挂靠账户（一个进程内多个账户、动态增删改）；策略不可见账户身份，手数按挂靠比率缩放；一个进程一个故障域，进程故障影响全部挂靠账户。品种数据与策略上下文合并为一个对象（`InstrumentContext`），ctx 自包含执行（持引擎句柄与 A 的账户表只读指针，直接调 SDK 下单，不持有 A 指针）；B 只见只读 API 面、A 用 friend 做创建注入并负责账户表与上报。实盘与逻辑的匹配是操作员责任（一键同步），部署不再需要按账户复制进程或配置。
- 预热与回卷的工程假设：回卷依赖 18:00 处于「日盘收盘后、夜盘开盘前」的静默窗（当前国内期货品种满足；跨市场/新品种需评估）；定时器可能被事件积压延迟，必须有推进前兜底；迟到品种（`instrument add`）的历史回放依赖归档域。
- 无归档数据时冷启动要等 `max_lookback` 根 live bar 才开始回调：1 分钟周期默认约 2.4 个交易日（交易小节表落地后约 1 个交易日）；归档域落地后改为预热回放，默认 10,000 根。
- 内存与启动成本（**每个进程**）：1 分钟默认配置 50 品种 ×（7 价格 + 4 用户）≈ 550 条序列 ≈ 19MB；`buffer_bars` 上限 24,576 根/序列（极端 2,050 条 ≈ 403MB）；挂靠账户数不影响内存（共享同一份序列与窗口）。预热回放成本 ∝ 实例数 × `warmup` 深度（每根 6 次 B 回调；10 年 1 分钟 ≈ 1.7–3.5s/实例），启动前打印估算并按 60s / 300s 打 WARN。
- 不改现有 `stg_demo`；新示例策略与真实进程 E2E 另建。
- 帧结构不因本 ADR 变化；`SET_LOGICAL_POSITION` 的语义改为「按挂靠账户上报逻辑持仓」、《td-order》补 AUTO 解析语义（随平台专项 issue #82 与 UI 接线提交）；回卷定时器复用《帧契约：策略》的 `DZ_FRAME_SCHEDULE` 本地定时器机制；UI_INPUT 命令面（含 account 命令）与 issue #9 合并定义。
- v1 后置与分级：归档域历史数据实现（同时是「回放续仓」的前置）、交易小节/竞价数据的 platform.db 落地与查询 API、B 侧定时器、Tier 2 交易统计；回测引擎、Python 绑定、多周期、跨品种数据（Data0..N）、撤单/委托管理/算法下单、参数寻优、品种加载次线程化；**平台侧前置**：td AUTO 开平解析（issue #82）；**平台/UI 后续能力**：账户级头寸监控与一键同步（[ADR 0028](0028-strategy-process-logical-account.md)）、策略组编排。Tier 3 明确不做：`Portfolio_*`、信号索引函数、`long*/short*` 分向系列。
- 性能基准接入 #7 的 SDK 交付与 CI 议程（含回卷与预热回放专项）；历史回放吞吐在归档域落地后补基准。

## References

- 讨论日期：2026-10-20 初稿、2026-10-21 设计评审（BarStrategy 设计 grill 会话）
- [ADR 0018: 策略侧 K 线生成与交易小节数据模型](0018-strategy-side-kline-and-trading-sessions.md)
- [ADR 0020: 策略 SDK 交付形态与公开面](0020-strategy-sdk-dynamic-delivery-and-public-surface.md)
- [ADR 0021: 策略 SDK 不提供日志](0021-strategy-sdk-no-logging-api.md)
- [ADR 0026: 策略 SDK 内置 nlohmann/json](0026-strategy-sdk-nlohmann-json.md)
- [ADR 0028: 策略进程单逻辑账户 + N 挂靠账户](0028-strategy-process-logical-account.md)
- [ADR 0029: 逻辑持仓净手数类型对齐（DzLargeVolume）](0029-logical-position-large-volume.md)
- [ADR 0030: 挂靠账户的缩放、同步与记账口径](0030-attached-account-scaling.md)
- GitHub issue #82（td AUTO 开平解析）、#3（tick→bar 合成通用组件 spec）、#4（归档域）、#6（UI↔策略闭环）、#7（C++ 引擎交付化）、#9（策略命令协议）
