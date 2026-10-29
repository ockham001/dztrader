# BarStrategy 框架组件设计

> 决策记录见 [ADR 0027](../adr/0027-bar-strategy-framework.md)、[ADR 0028](../adr/0028-strategy-process-logical-account.md)（逻辑账户 + 挂靠账户）与 [ADR 0030](../adr/0030-attached-account-scaling.md)（缩放/同步/记账口径）；JSON 依赖见 [ADR 0026](../adr/0026-strategy-sdk-nlohmann-json.md)；类型别名（`Volume`/`LargeVolume`）与逻辑持仓类型见 [ADR 0029](../adr/0029-logical-position-large-volume.md)；td 的 AUTO 开平解析见 issue #82（框架 v1 前置依赖）。
> 本文是组件级设计文档（蓝图），细节以代码与测试为准；帧协议语义仍以 [frame_contracts](../frame_contracts/README.md) 为唯一真相源。

## 1. 定位与范围

面向传统程序化策略（TB/MC 迁移）的 bar 级 C++ 框架：单品种、单周期时间序列策略；框架负责多品种、动态管理、历史预热与执行，策略作者只写策略逻辑。

**进程与账户模型**：一个策略进程 = 一个策略 = 一个**逻辑账户**（纯虚拟台账，不落地、不参与订单路由）；实体账户 0..N 个按挂靠比率（`scale`）动态挂靠，策略逻辑看不见任何实体账户（[ADR 0028](../adr/0028-strategy-process-logical-account.md)）。实例 = 品种；账户不是实例维度——多个账户共享同一份逻辑持仓，各自按比率镜像。

**v1 范围**：单进程、单行情源、0..N 个挂靠账户（运行期增删改、默认不同步）、多品种、单周期（1/3/5/15 分钟）、限价单 + `DZ_POSITION_EFFECT_AUTO`（td 侧解析见 issue #82；未修复前实盘单会被拒，属已知限制，不阻塞框架开发）、品种动态增删、逻辑持仓（净持仓口径）、Tier 1 持仓/进出场状态查询、**历史预热**（回放期虚拟成交、用于重启续仓；`dz_db_query_bar` 接口已存在但归档域未实现、当前返回空并自动降级为冷启动）、1us 性能目标（口径见 §10）。

**v1 不做**：账户的登录/凭据/交易接口管理（属于平台 td 配置）、框架内一键同步/账户级头寸监控（平台/WebUI 后续能力）、跨账户策略组/统一启停/参数同步、归档域历史数据实现（回放续仓依赖它）、Tier 2 交易统计、`Portfolio_*`、信号索引函数、`long*/short*` 分向系列、回测引擎、Python 绑定、1 小时/1 日周期（依赖交易小节）、多周期、跨品种数据（Data0..N）、B 侧定时器回调（`on_schedule`）、撤单/委托管理/算法下单、参数寻优、品种加载次线程化、交易小节/竞价窗口表的数据源（platform.db 落地，[ADR 0018](../adr/0018-strategy-side-kline-and-trading-sessions.md) 决策 2/3）。

**竞价窗口内事件**：B 契约已包含竞价相位（[ADR 0032](../adr/0032-auction-window-strategy-events.md)）——Open 竞价窗口内正常触发三事件、`ctx.is_auction()` 标识相位；框架侧已实现（#163，注入含竞价窗口的小节表即生效）；生产入口 v1 仍用内置退化小节表（单 24h 小节、无竞价窗口，§5.5），竞价 tick 按普通 tick 处理、`ctx.is_auction()` 恒 false。

## 2. 角色与进程模型

- 一个策略 = 一个可执行文件 `stg_<id>` = 一个进程 = 一个逻辑账户；实体账户 0..N 个挂在进程内（既有「一个策略一个进程」不变量不变）。
- 一个进程只有一种用户策略类型 `Strategy`（B）；框架管理器 `BarStrategyManager<Strategy>`（A）对引擎而言是唯一的策略对象，入口 `run_bar_strategy<Strategy>(config)` 内部以 A 调用既有 `run_strategy`。
- **实现状态（#157–#163）**：公开入口 `dztrader/bar_strategy.h`（`run_bar_strategy<Strategy>()` + `BarStrategy` concept），管理器 `dztrader/bar_strategy/manager.h`（A 的引擎回调、tick 分发、事件适配器、`BarEventSink` / `InstrumentNode`），品种上下文 `dztrader/bar_strategy/context.h`（`InstrumentContext` / `InstrumentInit` / `PriceArrays`），虚拟台账 `dztrader/bar_strategy/position.h`（`PositionState` / `InstrumentSpec` / `apply_intent`），挂靠缩放 `dztrader/bar_strategy/scaling.h`（`AttachedAccount` / `ScalingConfig` / `logical_position`），执行原语 `dztrader/bar_strategy/execution.h`（`TickSnapshot` / `resolve_order_price` / `order_volume`）。#157 实现骨架（配置加载与启动诊断、订阅、tick→bar 分发、`close → open → update` 分发、异常隔离、退化小节表），#158 补齐回调门禁 / 静默填窗 / 用户序列 / 18:00 回卷（`on_schedule` 已实现），#159 补齐**交易意图 + 虚拟台账 + Tier 1**（四个开平意图、即时记账、Tier 1 查询、诊断分级、失败实例拒单），#160 补齐**挂靠缩放 + 差量单 + 逻辑持仓上报**（`L_i = sign(P)×f(scale×|P|)`、每账户差量单、effect 恒 AUTO、涨跌停夹取与 tick 对齐、下单/上报各一个唯一出口、live 门禁与进入 live 时的全量批量上报）。#162 补齐**动态管理命令与参数热更新**：`on_ui_input` 接 `DZ_FRAME_UI_INPUT`（文法见 `bar_strategy/command.h`，12 条命令）、`on_params_update` 参数通知、同步单价格 `resolve_sync_price()`、参数仓 `bar_strategy/params.h`（见 §8/§9）。#163 补齐**竞价相位**：`ctx.is_auction()` 按触发本次回调的 tick 相位在三个 bar 回调入口写入（竞价窗口首笔 tick 触发的 `on_bar_close` 也为 true，ADR 0032 决策 3）；非 bar 回调与静默填窗 / 历史回放期恒 false（回放期由 `InstrumentNode::replaying` 抑制）；退化表下恒 false（§5.5）。骨架期 A 未实现 `on_trade_report` / `on_order_report` / `on_position_info` / `on_trading_account`（引擎按编译期探测跳过）。#161 补齐**历史预热回放 + 缓冲 tick 续接 + 三个内部依赖缝**：依赖缝聚合在 `dztrader/bar_strategy/dependencies.h`（`FrameworkDependencies` / `HistoryBarLoader` / `InstrumentMetadataLoader` / `make_replay_ticks` / `estimate_replay_seconds` / `degraded_trading_session_table`，默认实现 `query_archive_history`、`load_archive_metadata`），A 侧编排在 `manager.h`（`warmup_history_replay` / `replay_instrument_history` / `drain_buffered_ticks` / `should_drop_continuation_tick`；历史自洽性是归档域数据契约，见 §5.3），合约元数据接上后 `volume_multiple` / `price_tick` / `exchange_id` 由默认归档实现或注入的 fake 供给。测试：`libs/strategy_api/tests/bar_strategy_replay_test.cpp`（合成 tick 回放保真、分桶判定的手写期望桶 oracle）、`bar_strategy_dependencies_test.cpp`（元数据四态 / 历史 stub 降级 / 默认依赖口径）、`bar_strategy_framework_test.cpp`（Seam A + Seam B：注入假历史/假元数据的回放事件序、逻辑持仓恢复与一次性上报、final/forming 两条续接规则、锚点独立、冷启动降级、`warmup` 两形态、成本两级 WARN）。
- A 实现引擎侧回调（`on_start` / `on_tick` / `on_schedule` / `on_ui_input` / `on_stop` 等）：`on_start` 里取得 `DzContext*`、读取并校验配置（含挂靠账户表）、注册 A 内部的每日 18:00 回卷定时器（§10）；`on_tick` 里把 tick 转交该品种的 builder（bar 边界由 ADR 0031 组件负责，§5.4）并分发实例；`on_ui_input` 里解析 `instrument *` / `account *` / `param *` / `status` / `quit` 命令；`on_schedule` 里只处理自己的定时器 ID，不转发给 B。**成交/委托/持仓/资金回报 v1 全部忽略**（不建成交台账、不跟踪委托；逻辑账只由 B 的交易意图推进）。
- A 为配置里的每个品种创建一个 B 实例（`instruments` 列表）；品种可按 `instrument add/del` 动态增删；进程故障域独立。
- 账户表由 A 持有（账户标识 + 挂靠比率），运行期可增删改（§8）；策略逻辑不可见账户身份，也不提供 `ctx.account_id()`。0 个账户时策略照常运行（只记逻辑台账、不发实盘单）。
- A 在 `on_start` 开始即对配置里的全部品种调 `dz_subscribe`（订阅先于历史查询，保证切换窗口的 tick 不丢）；运行期 `instrument add/del` 同步增/减订阅。回放与 live 的续接规则见 §5.3。
- B 不继承 `StrategyBase`、不接触引擎回调、不接收交易回报；B 与 A 之间是编译期鸭子类型。

```
进程 stg_ma_ctp01（逻辑账户；挂靠 CTP001 scale=1.0、CTP002 scale=0.5）
└─ run_bar_strategy<double_ma>(config)
    └─ BarStrategyManager<double_ma>（A；引擎侧唯一策略对象）
        ├─ instruments_["rb2601"] → InstrumentNode：InstrumentContext（品种数据 + API 面）+ B 实例
        ├─ instruments_["rb2605"] → InstrumentNode
        ├─ params_{fast,slow,volume}
        └─ accounts_（A 持有：CTP001/1.0、CTP002/0.5）
B 回调（A 调用）：on_init → [静默填窗 → 历史回放（有数据时；虚拟成交、不落地）] → on_bar_open/on_bar_update/on_bar_close → on_destroy
```

## 3. 数据结构与地址稳定性

```cpp
namespace dztrader {

// 类型别名 Volume / LargeVolume 由 data_type.h 的 C++ 段提供（ADR 0029），此处不重复声明

struct WindowState {                        // 每品种一个：价格序列与用户序列共用的窗口几何（纯 POD）
    std::int32_t end = 0;                   // 写入位置 = 窗口右开边界：窗口 = [end - size, end)
    std::int32_t size = 0;                  // 已填充 bar 数（含当前）
    std::int32_t max_lookback = 0;          // 可读历史上限（= 配置 max_lookback）
    std::int32_t buffer_bars = 0;           // 物理缓冲元素数（= max_lookback + reserve_bars）
    std::int32_t advance_seq = 0;           // 单调推进序号（回卷不变）；判「代理是否过期」用
};

template <class T>
struct SeriesStorage {                      // 用户序列的所有权记录：物理数组 + 声明初值（B 经 make_series 注册）
    std::unique_ptr<T[]> base;              // 物理 buffer_bars 个元素
    T init{};
};

template <class T>
class SeriesView {                          // 只读视图：全部读接口的唯一实现（dztrader/series.h）
public:
    T operator[](std::ptrdiff_t n) const;   // TB 口径：n=0 当前，n 往回数；越界抛 std::out_of_range
    std::int32_t size() const noexcept;     // 已填充 bar 数（含当前）；未绑定视图为 0
    std::int32_t max_lookback() const noexcept; // 可回读的历史根数上限（= 配置 max_lookback）
    std::span<const T> span() const noexcept;   // STL 口径：时间正序（最旧→最新）连续窗口，长度 = size，零检查
protected:
    friend struct series_internal::Access;  // 绑定缝：A（ctx）与测试经此构造，B 侧不可凭空绑定数组
    friend class InstrumentContext;         // ctx 的公开价格视图在构造时绑定自身 window_/price_
    SeriesView() = default;
    SeriesView(T* base, const WindowState* window) : base_(base), window_(window) {}
    void check_index(std::ptrdiff_t n) const;  // 越界：Debug 断言（DZTRADER_SERIES_ASSERT）→ 抛 out_of_range
    T* base_ = nullptr;
    const WindowState* window_ = nullptr;
};

template <class T>
class SeriesRef {                           // X[0] 的写入代理：只允许给当前 bar 赋值
public:
    SeriesRef() = default;
    SeriesRef& operator=(T value);          // n != 0 或代理已过期时抛 std::out_of_range
    SeriesRef& operator=(const SeriesRef&); // 值语义 carry（不许逐成员拷贝代理）
    operator T() const;                     // 读取当前 bar（过期代理同样抛，读写同口径）
private:
    template <class U> friend class Series;
    SeriesRef(T* slot, std::ptrdiff_t n, const WindowState* window, std::int32_t seq_snapshot);
    void validate_bound() const;            // 已由 Series::operator[] 绑定
    void validate_writable() const;         // 绑定 + n==0 + 未过期
    void validate_current() const;          // advance_seq 未变（过期即抛 stale）
    T* slot_ = nullptr;                     // 当前 bar 的物理槽位
    std::ptrdiff_t n_ = 0;                  // 索引（仅 0 可写）
    const WindowState* window_ = nullptr;   // 创建时的窗口几何（判过期）
    std::int32_t seq_snapshot_ = 0;         // 创建时的 window.advance_seq
};

template <class T>
class Series : public SeriesView<T> {       // 用户变量：派生只增加「写当前 bar」
public:
    Series() = default;
    using SeriesView<T>::operator[];
    SeriesRef<T> operator[](std::ptrdiff_t n);  // 非 const 对象返回写入代理（仅 n == 0 可赋值）
private:
    friend struct series_internal::Access;  // 绑定缝（同上）
    friend class InstrumentContext;         // make_series 构造视图
    Series(T* base, const WindowState* window) : SeriesView<T>(base, window) {}
};

struct InstrumentSpec {                     // 合约属性（dz_db_query_instruments 解码子集；A 内部，不含 instrument_id）
    std::string exchange_id;                // 交易所（仅诊断/展示；AUTO 下单不需要）
    double volume_multiple = 0;             // 合约乘数（>0；金额/浮动盈亏换算）
    double price_tick = 0;                  // 最小变动价位（>0；下单价格对齐）
    // v1 只用这三项；下单量上下限、期权等字段用到时再加
    // 实现状态：volume_multiple 在 #159 落地（Tier 1 盈亏换算），price_tick 在 #160 落地
    // （下单价格对齐）；三项的**供数**随 #161 的合约元数据接缝落地（默认
    // `load_archive_metadata`；platform.db 缺失时仍为 0/空并打已知限制诊断，§6）
};

struct PositionState {                      // 策略虚拟持仓 + Tier 1 状态（每品种一份；只在「交易意图 → 虚拟成交」漏斗更新）
    LargeVolume net_volume = 0;             // 虚拟净持仓，多正空负（B 看到的持仓）
    double avg_entry_price = 0;             // 当前持仓平均建仓价
    std::int32_t current_entries = 0;       // 当前持仓建仓次数（≥0）
    LargeVolume max_contracts = 0;          // 历史最大持仓手数（|P| 峰值，≥0）
    std::int32_t max_entries = 0;           // 历史最大建仓次数（≥0）
    // 当前持仓的首次建仓 / 最近加仓 / 最近平仓
    // 无参考的日期用 DZ_DATE_NA（= 0）而不是 {}，与 SDK 的「无效日期」约定一致
    double entry_price = 0;  DzDate entry_date = DZ_DATE_NA;  DzTime entry_time = 0;
    double last_entry_price = 0;  DzDate last_entry_date = DZ_DATE_NA;  DzTime last_entry_time = 0;
    double exit_price = 0;  DzDate exit_date = DZ_DATE_NA;  DzTime exit_time = 0;
    std::int64_t entry_bar = -1, last_entry_bar = -1, exit_bar = -1;  // bar 序号（bars_since_* 用）
    double max_position_profit = 0;         // 当前持仓期间浮盈极值（bar 收盘更新）
    double max_position_loss = 0;           // 当前持仓期间浮亏极值（bar 收盘更新）

    std::int32_t market_position() const noexcept;  // -1 / 0 / 1（由 net_volume 符号派生，不存字段）
};

struct PriceArrays {                        // 7 条价格序列的物理数组（A 内部；B 只经只读视图看）
    std::unique_ptr<double[]> open, high, low, close, volume, turnover, open_interest;
};

struct TickSnapshot {                       // 最近一笔已被 builder 接受的 tick（价格夹取 + 同步单定价）
    double upper_limit = 0, lower_limit = 0;  // 涨跌停（价格夹取）
    double last_price = 0;                  // 最新价（同步单无盘口时的兜底，#162）
    double bid_price = 0, ask_price = 0;    // 买一/卖一（同步单的对手价，#162）
    bool valid = false;                     // false = 还没有任何行情，夹取与对手价都跳过
    // 只放**当前有消费者**的字段。回放**不会**置 valid——合成 tick 分发期 A 不挂
    // current_tick_，历史涨跌停/盘口进不了快照（#161）；进入 live 后第一笔真实 tick 才置有效。
    // 三个新字段供 resolve_sync_price()（#162）：对手价 → 最新价兜底 → 跳过该品种。
};

struct AttachedAccount {                    // 挂靠账户（A 持有；运行期可增删改，ADR 0028）
    std::string account_id;
    double scale = 1.0;                     // 挂靠比率：0 < scale <= max_scale
};
using AccountTable = std::vector<AttachedAccount>;  // 只读遍历；命令按账户 id 查找

struct ScalingConfig {                      // 进程级缩放配置（启动加载后不可变；ctx 按值拷贝）
    double max_scale = 1.0;                 // 挂靠比率上限（> 0）
    ScaleRounding rounding = ScaleRounding::Nearest;  // 取整方向（enum 与 ScalingConfig 同在 scaling.h）
    bool min_one = false;                   // 目标非零时至少 1 手
};

enum class SyncMode : std::uint8_t { Align, Flatten };    // 对齐（add/set）/ 删除平仓
enum class PriceMode : std::uint8_t { Counterparty, Market };

struct SyncOutcome {                        // 同步单结果（A 汇总后回显 OUTPUT_UI）
    std::int32_t submitted = 0;
    std::vector<std::string> errors;
};

class ParamStore;                           // 进程级参数存储（定义见 §9；A 持有，ctx 持只读指针）

struct InstrumentInit {                     // ctx 构造参数聚合（按依赖分组，避免位置参数传错）
    DzContext* dz = nullptr;
    const AccountTable* accounts = nullptr; // A 的挂靠账户表（只在两次回调之间修改）
    const ParamStore* params = nullptr;     // 进程级参数
    ScalingConfig scaling;                  // 按值拷贝：启动后不可变
    std::string instrument_id;              // 平台唯一键
    InstrumentSpec spec;                    // 合约属性
    std::int32_t max_lookback = 0;
    std::int32_t buffer_bars = 0;
};

template <class Strategy> class BarStrategyManager;   // 前向：A 通过 friend 访问 ctx
template <class Strategy> struct InstrumentNode;      // 前向：节点就地构造 ctx

class InstrumentContext {                   // 每品种一个：品种数据 + B 的 API 面合并为同一对象
public:
    // ==== B 可见面：只读价格视图、用户序列、参数、交易意图、Tier 1 查询 ====
    SeriesView<double> open, high, low, close, volume, turnover, open_interest;
    template <class T> Series<T> make_series(T init);        // 仅 on_init 可调用
    std::int64_t bar_index() const noexcept;                 // -1 = on_init
    const std::string& instrument_id() const noexcept;       // 合约代码（平台唯一键）
    bool is_auction() const noexcept;                        // 当前回调是否在 Open 竞价窗口内（ADR 0032）
    template <class T> T param(std::string_view name, T default_value) const;
    // 账户身份不暴露：逻辑账户没有独立 id，挂靠账户对策略不可见（ADR 0028）

    // ---- 交易意图（v1 仅限价；返回 void，B 不感知执行结果）----
    // 四个方法是开平意图：buy/sell_short 反向时先平对手仓再建仓，sell/buy_to_cover 只减不增；
    // volume == 0 表示只平不建（买方向平空、卖方向平多）；完整规则见 §6。
    // 每账户手数由挂靠比率与逻辑持仓差量在内部计算（ADR 0030）
    void buy(Volume volume, double price);
    void sell(Volume volume, double price);
    void sell_short(Volume volume, double price);
    void buy_to_cover(Volume volume, double price);

    // ---- Tier 1 持仓/进出场查询（语义与口径见 §6.1；状态在 PositionState）----
    std::int32_t market_position() const noexcept;           // -1 / 0 / 1（由净持仓派生）
    LargeVolume current_contracts() const noexcept;          // 持仓手数（≥0；方向看 market_position）
    double avg_entry_price() const noexcept;
    std::int32_t current_entries() const noexcept;           // 当前持仓建仓段数（≥0）
    LargeVolume max_contracts() const noexcept;              // 本实例历史最大持仓手数（≥0）
    std::int32_t max_entries() const noexcept;               // 本实例历史最大建仓次数（≥0）
    double entry_price() const noexcept;
    DzDate entry_date() const noexcept;
    DzTime entry_time() const noexcept;
    double last_entry_price() const noexcept;
    DzDate last_entry_date() const noexcept;
    DzTime last_entry_time() const noexcept;
    double exit_price() const noexcept;
    DzDate exit_date() const noexcept;
    DzTime exit_time() const noexcept;
    std::int64_t bars_since_entry() const noexcept;          // 无参考值返回 -1
    std::int64_t bars_since_last_entry() const noexcept;
    std::int64_t bars_since_exit() const noexcept;
    double position_profit() const noexcept;
    double contract_profit() const noexcept;
    double max_position_profit() const noexcept;
    double max_position_loss() const noexcept;
private:
    template <class> friend class BarStrategyManager;        // A 通过 friend 访问内部状态
    template <class> friend struct InstrumentNode;           // 节点就地构造（传 InstrumentInit）
    // ==== A 可见面：品种数据 + 运行时状态 ====
    WindowState window_;                    // 价格与用户序列共用
    std::int64_t bar_index_ = -1;           // 当前 bar 的绝对序号（0 起、单调；classic TB CurrentBar 同口径）
    PriceArrays price_;                     // 7 条价格数组（物理 buffer_bars 个元素）
    std::vector<SeriesStorage<double>> series_d_;
    std::vector<SeriesStorage<std::int64_t>> series_i_;
    InstrumentSpec spec_;                   // 合约属性（见上）
    PositionState position_;                // 策略虚拟持仓 + Tier 1（下单意图即时记账）
    TickSnapshot tick_;                     // 最近一笔已接受 tick（价格夹取 + 同步单对手价）
    ScalingConfig scaling_;                 // 缩放配置副本（启动后不可变；按值持有，避免再一层解引用）
    const ParamStore* params_ = nullptr;    // 指向 manager 的参数存储（地址稳定）
    DzContext* dz_ = nullptr;               // 引擎会话句柄（on_start 注入，进程内不变）
    const AccountTable* accounts_ = nullptr; // A 的挂靠账户表只读指针（A 只在两次回调之间修改）
    std::string instrument_id_;             // 合约代码（平台唯一键；注意区分 symbol/name）
    bool live_ = false;                     // false = on_init/回放期：只做虚拟成交、不发单、不上报
    bool failed_ = false;                   // 实例失败：不再回调、不再受理下单意图
    InstrumentContext(const InstrumentInit& init);  // A 内部构造：分配 buffer_bars、绑定 7 个价格视图
    // ==== 执行与记账内部方法（所有路径共用，避免重复实现；实现状态 #160）====
    //   execute_intent(intent, volume, price)：拒单门禁 → 虚拟成交记账 → fanout → 分级诊断（单一漏斗）
    //   fanout_positions(net_before, price)：P_old → P_new 后逐账户算 L_i 差量 → 发单 + 上报
    //   submit_delta_order(account, delta, price)：差量 → 方向 + 手数（差量 0/超范围在这里分流）
    //   submit_order(account, direction, volume, price)：唯一的 dz_place_order 出口
    //   publish_position(account, const char* instrument_id, net_l)：唯一的 dz_set_logical_position 出口
    //     （instrument_id == nullptr = 空品种通配清零；#162 的 account del 经
    //      InstrumentContext::publish_account_wildcard_zero 调用它）
    //   publish_all_positions()：进入 live / 账户表变更后的全量对账上报（不发单）
    //   submit_sync_order(account, delta, market)：生命周期命令的同步单（#162 已落地；价格经
    //   resolve_sync_price 取对手价/最新价/市价，最终仍走 submit_order 出口）
    // A 内部方法：advance_bar / relocate / set_live / set_failed；形成中 bar 与边界判定由每品种的
    // builder 持有（§5.4），ctx 不保存形成中 bar 聚合值；回放游标为 A 的局部变量
public:
    InstrumentContext(const InstrumentContext&) = delete;             // 视图/序列自持指针，
    InstrumentContext& operator=(const InstrumentContext&) = delete;  // 搬移即失效
    InstrumentContext(InstrumentContext&&) = delete;
    InstrumentContext& operator=(InstrumentContext&&) = delete;
};

template <class Strategy>
struct BarEventSink {                      // A 内部：builder 事件 → 窗口推进 + B 回调（§5.4）
    InstrumentContext* ctx = nullptr;      // 本节点的 ctx
    BarStrategyManager<Strategy>* manager = nullptr;  // A：推进窗口 / 门禁 / 失败实例判断
    void on_bar_open(const bar::Bar& bar, bool is_auction);
    void on_bar_update(const bar::Bar& bar, bool is_auction);
    void on_bar_close(const bar::Bar& bar, bool is_auction);
};

template <class Strategy>
struct InstrumentNode {                    // unordered_map 节点内就地构造；ctx/builder/B 一起，地址稳定
    InstrumentNode(BarStrategyManager<Strategy>& manager, const InstrumentInit& init,
                   const bar::TradingSessionTable& table, std::int32_t period_sec);
    InstrumentContext ctx;                 // 品种数据 + B 的 API 面
    BarEventSink<Strategy> sink;           // A 内部：builder 事件 → 窗口推进 + B 回调（§5.4）
    bar::IntradayBarBuilder<BarEventSink<Strategy>> builder;  // tick→bar 唯一实现（ADR 0031）
    Strategy instance;                     // B：默认构造，节点内不再搬移
};

template <class Strategy>
class BarStrategyManager {                    // A：引擎侧唯一策略对象
    // 依赖在前、持有者在后：instruments_ 最后声明 → 最先析构，节点不会看到已销毁的 params_/accounts_
    DzContext* dz_ = nullptr;                // 引擎会话句柄（on_start 取得，注入各 ctx）
    ParamStore params_;                      // 进程级参数（v1 所有品种相同）
    ScalingConfig scaling_;                  // 进程级缩放配置（启动加载一次、盘中不可变）
    AccountTable accounts_;                  // 挂靠账户表（启动读配置，运行期命令修改）
    // 节点地址稳定（#157 实现选 std::map + std::less<>：热路径按 string_view 异构查找
    // 不需要构造 std::string，见 §3 地址稳定性）；条目数 = 品种数（几十量级）
    std::map<std::string, InstrumentNode<Strategy>, std::less<>> instruments_;

public:  // 引擎回调与命令入口（实现见 §2/§8）
    // on_start / on_tick / on_schedule / on_ui_input / on_stop
private:
    // handle_ui_input / account_add / account_set / account_del / account_list
    // publish_all_positions / enter_live
};

}  // namespace dztrader
```

**为什么价格窗口与用户窗口合并成一个**：一个策略进程里每个品种只有一个 B 实例，价格序列与用户序列在每根 bar 上永远同步推进（同一个推进步骤，§4.1），不存在「实例时间线 vs 品种时间线」的相位差；合并成一个 `WindowState` 后，价格视图、用户序列、`bar_index` 三者天然一致，也不需要任何回放期的窗口重绑/临时价格视图。窗口在物理数组内滑动、用尽时整窗搬回头部（§4.4）。

**为什么品种数据与策略上下文合成一个对象**：初稿的 `InstrumentData` / `InstrumentContext` 拆分与 `data_` 指针是为「多账户共享同一份品种数据」而设；采用「单逻辑账户 + 挂靠账户」后每品种恰好一个 B 实例，数据只有一个读者、生命周期与上下文完全重合，指针已不承担任何隔离/共享职责，只剩「指向哪里、何时失效」的不变式。合并后价格数组、用户序列、`bar_index`、合约属性、逻辑持仓都在同一个对象里：B 只见 public 的只读视图/查询/交易意图，A 通过 friend 直接读写内部；地址稳定照旧由有序关联容器的节点提供（`try_emplace` 就地构造，对象显式不可拷贝/移动）。**热路径与拆分版等价**——视图本来就自持 `base_`/`window_` 指针，`ctx.close[0]` 不经过 ctx 之外的任何中转，合并只删代码与生命周期面，不增加间接。

**A 与 ctx 的边界：ctx 自包含执行，没有反向指针**：ctx 是不带模板参数的公开类型（6 个回调签名不暴露 `Strategy`）。下单与记账所需的输入全部在 ctx 本地——`DzContext*`、合约属性（`InstrumentSpec`）、虚拟持仓（`PositionState`）、最近 tick（`TickSnapshot`）、缩放配置副本（`ScalingConfig`）、进程级 `ParamStore` 只读指针，以及 **A 的挂靠账户表只读指针**（账户表是纯数据，A 只在两次回调之间修改，不构成反向控制）。因此 `ctx.buy()` 在 ctx 内完成「逻辑持仓差量 → 每账户手数 → 价格处理 → SDK 调用」与逻辑持仓上报，**不需要 `BarStrategyManager<Strategy>` 指针、虚接口或执行器层**。成交/委托/持仓/资金回报 v1 全部忽略（不建成交台账、不回流逻辑持仓）。账户生命周期命令、回放完成后的批量上报、账户表维护属于 A。未来的委托管理/算法下单属于 A 的后续执行层（§14），同样不需要在下单路径上持有 A 指针。

**WindowState 是什么、不是什么**：每条序列的数据就是一条连续 `T` 数组（`unique_ptr<T[]>`），**物理长度 `buffer_bars = max_lookback + reserve_bars`**，语义等价于每根 bar `push_back` + 写满后 `erase(begin)`，但不增长、不 realloc。读接口分两套口径：`[0]` / `[n]` 是 TB 口径（往回数），`size()` / `max_lookback()` / `span()` 是 STL 口径（`span` 为时间正序，可直接 range-for 或喂算法）。**不是环形缓冲区**：没有取模下标、没有双段拼接，窗口始终物理连续；窗口在数组内滑动，物理缓冲用尽时把窗口整体搬回数组头部（回卷）。`max_lookback` 是**可读历史上限**（= 配置 `max_lookback`），`buffer_bars` 是**物理元素数**（多出的 `reserve_bars` 是回卷余量，B 不可见）。`WindowState` 不是容器，只是这组数组共享的窗口几何，且是**纯 POD**——推进与回卷逻辑在 A 里，B 接触不到。视图只存 `(数组指针, window 指针)`、构造一次后永不更新：A 推进一次 window，所有视图（价格 + 用户序列）同时看到新 bar。

**地址稳定性**：

| 对象 | 存放 | 被谁指向 | 搬移是否安全 |
|---|---|---|---|
| `InstrumentNode`（ctx + builder + B） | 有序关联容器节点（`std::map` + `try_emplace` 就地构造） | ctx 内的视图指向自身 `window_`/`price_`；B 持有的 `Series` 同样指向 ctx 内数组；builder 持节点内 sink 引用 | 安全：节点稳定；ctx 显式不可拷贝/移动，B 在节点内默认构造后不再搬移 |
| 价格数组 / 用户序列数组 | `unique_ptr<T[]>`，堆 | 视图的 `base_`、B 的 `Series` | 安全（回卷只搬内容、不换地址） |
| `SeriesRef`（写入代理） | 栈上临时对象 | 无 | **保存后不可用**：代理记录创建时的 `advance_seq`，跨推进后读/写都抛 `series ref is stale`（`Series` / `SeriesView` 副本可安全保存，代理不行——它是「写这一根」的一次性句柄）。用单调序号而不是 `end` 比较：回卷会把 `end` 置回 `size`，之后推进能让它重新等于旧快照值（ABA），序号不会 |
| `ParamStore` | `BarStrategyManager` 成员 | ctx 的 `params_` | 安全（manager 生命周期内不搬迁；`param set` 原地更新内容） |
| `AccountTable` | `BarStrategyManager` 成员（`std::vector`） | ctx 的 `accounts_` | 表对象地址稳定；**元素地址随增删改变化**——ctx/A 都不缓存元素指针，每次按表遍历 |
| `ScalingConfig` | A 成员 + ctx 按值拷贝 | 无指针 | 天然安全（启动后不可变） |
| `TickSnapshot` / `PositionState` | ctx 内部值成员 | 无外部指针 | 安全（随 ctx 节点稳定） |

`DzContext*` 由引擎在 `on_start` 交给 A、进程内不变，各 ctx 复制该指针；ctx 不持有指向 `BarStrategyManager` 的任何引用；挂靠账户表由 A 持有，ctx 只持只读指针（表的增删改只发生在两次回调之间，事件循环单线程，无并发问题）。类型按生命周期二分：**值语义**（`InstrumentSpec` / `ScalingConfig` / `PositionState` / `TickSnapshot`，随 ctx 持有或按值拷贝）与**指针语义**（`DzContext*` / `ParamStore*` / `AccountTable*`，生命周期由 A 保证且 A 在 ctx 之后析构）。

约束：
- `InstrumentNode` 由 A 在 `unordered_map` 节点内就地构造（`try_emplace` 就地装配 ctx、sink、builder 与 B）：ctx 的 `dz_` / `accounts_` / `params_` / `scaling_` / `spec_` / 窗口几何在构造时注入，公开价格视图绑定自身 `window_` 与 `price_`；sink 持本节点 ctx 与 A 指针，builder 持 sink 引用（构造顺序 ctx → sink → builder，节点地址稳定）。
- **两类状态分清**：值语义状态（`ScalingConfig` / `PositionState` / `TickSnapshot` / `InstrumentSpec`）随 ctx 持有或按值拷贝，没有生命周期指针问题；指针语义状态（`ParamStore` / `AccountTable` / `DzContext`）由 A 持有，A 在 `instruments_` 之前声明（析构顺序保证节点先销毁）。
- **执行门禁只有两个开关**：`live_`（`on_init` 期与回放期 false——`execute_intent` 只做虚拟成交，不发单、不上报）与 `failed_`（实例失败——不再回调、拒单）；两者都在 ctx 内，避免 A 与 ctx 对执行阶段的判断漂移。**实现状态（#160）**：`enter_live()` 在 `on_start` 末尾把全部实例置 true 并做一次全量上报——静默填窗期本来就不调 B（回调门禁），所以 v1 的 false 区间只有 `on_init`；#161 接上回放后，`enter_live()` 的调用点挪到回放之后，回放期自然落进 false 区间，ctx 侧无需改动。
- `InstrumentContext` 不可拷贝、不可移动（视图与 `Series` 自持指针，搬移即失效）；`Strategy` 只要求可默认构造（节点内构造），不再有 move 约束。
- B 不得保存 `InstrumentContext*`（每次回调都传入，保存没有收益且 `instrument del` 后失效）；`make_series` 仅限 `on_init`。
- **用户序列容器只放用户序列**：`series_d_` / `series_i_` 只存 B 在 `on_init` 经 `make_series` 注册的 `SeriesStorage<T>`；价格序列在 `price_`，框架内部状态（形成中 bar 聚合、预热/回放游标等）不进用户序列表；将来若内部需要序列，单独存储、不复用 `make_series` 通道（命名、容量与生命周期语义不同）。
- 价格视图用只读值类型 `SeriesView<double>`：B 可把它复制到局部变量或传给指标函数；视图不拥有数组、不提供写接口。
- **内部面不进 public**：B 的可见面严格限定在 public 区（只读视图、`make_series`、`bar_index/instrument_id/param`、交易意图、Tier 1 查询）；窗口推进、bar 聚合、预热/回放、下单执行与逻辑持仓记账都是 private，由 friend 的 A 驱动或 ctx 内部调用，热路径上不做 virtual dispatch。
- **零开销约束**：`SeriesView` / `Series` 单继承、**禁止虚函数与虚析构**、`Series` 不新增数据成员。静态断言锁定：`sizeof(Series<double>) == sizeof(SeriesView<double>)`、`std::is_trivially_copyable_v<Series<double>>`、`std::is_standard_layout_v<Series<double>>`、`!std::has_virtual_destructor_v<Series<double>>`。
- 指标函数统一签名 `double average(const SeriesView<double>&, int)` 等，`Series<double>` 隐式转基类引用，**不需要模板化算法**。
- 只读保证：`SeriesView` 不能转换为 `Series`（`Series<double> x = ctx.close;` 编译失败），价格数组无法经视图获得写能力。
- **索引类型分工**：对外 `operator[]` 取 `std::ptrdiff_t`；`WindowState` 的几何量取 `std::int32_t`（4 个几何量 + `advance_seq`，有界：`size ≤ end ≤ buffer_bars`、`size ≤ max_lookback ≤ buffer_bars`、`buffer_bars = max_lookback + reserve_bars`；窗口 20 字节 = 5×int32）。转换只发生在 `operator[]` 内部一次宽化 `base_[end - 1 - n]`。`max_lookback ∈ [32, 16384]` 与 `buffer_bars ≤ 24576` 的区间校验已结构性保证二者远小于 `INT32_MAX / 2`，无需额外断言。
- **无 vector 写接口、无扩容**：不提供 `push_back` / `erase` / `resize` / `reserve`（bar 生命周期与窗口推进由 A 管，B 只能写 `[0]`）；容量 = 配置值，在实例创建时一次性分配、之后永不 realloc——B 持有的视图/指针全程有效，热路径上没有分配器；不够用是配置错误（读越界抛异常，§4.2）。用户序列也不做「单条更深回溯」：它们共享品种窗口（见 §14 后置项）。
- **分配不做特化**：每序列一次普通堆分配（`operator new` 的自然对齐，x86-64 至少 16B，足以满足 `double` 与非对齐加载），物理长度就是 `buffer_bars`，不做 2 的幂取整、不做 cache-line padding。理由：热路径只有 8B 自然对齐的标量读写（永不跨 cache line），回卷一天只发生一次（§10），对齐收益在测量噪声内；2 的幂反而有 4KB 同余导致的 L1 cache set 冲突风险。若将来改为 arena/矩阵布局，行步长禁止取 4096 的整数倍（§14 记录）。
- **唯一出口**：所有 `dz_place_order` 调用都走 `ctx.submit_order()`（正常单、同步单、删除平仓共用；差量先经 `ctx.submit_delta_order()` 翻译成方向 + 手数），所有 `dz_set_logical_position` 调用都走 `ctx.publish_position()` / `ctx.publish_all_positions()`；A 不直接拼这两种帧，避免两条路径的上报/价格处理漂移。

**热路径**（每 tick）：

```cpp
auto& node = instruments_.find(tick.instrument_id)->second;  // 一次异构查找（string_view）
node.builder.on_tick(tick);                                  // 唯一 tick→bar 路径：边界/丢弃/事件序由组件负责（§5.4）

// 节点内事件适配器按组件给出的顺序执行（A 内部，与 §5.1 一致）：
//   on_bar_close(prev) → B.on_bar_close(ctx)              // 窗口尚未推进，[0] 仍是上一根
//   on_bar_open(new)   → 回卷兜底 + 推进窗口 + carry     → B.on_bar_open(ctx)
//   on_bar_update      → Bar 字段写入 [0]                → B.on_bar_update(ctx)
```

## 4. Series 层语义

> **实现状态（#155 / #157 / #158）**：Series 层与窗口几何落在两个头文件里——
> `libs/strategy_api/include/dztrader/series.h`（公开面：`WindowState` / `SeriesView` /
> `SeriesRef` / `Series` + 静态断言；`SeriesStorage` 在 `bar_strategy/context.h`，属 ctx
> 内部面）与 `dztrader/series_internal.h`（内部面：
> 容量推导 `resolve_capacity`、窗口驱动 `commit_advance` / `prepare_advance`、回卷
> `relocate_window` / `relocate_array` / `finish_relocate`、绑定缝 `series_internal::Access`）。
> 单元测试 `series_test.cpp`（读/写语义与公开面不变量）与 `series_window_test.cpp`（容量、
> 回卷等价性、随机推进 fuzz）。`InstrumentContext` 的接入（`make_series` / 用户序列 carry /
> 价格视图绑定）由 #157 落地骨架、#158 补齐用户序列与静默填窗初值（§4.1「创建」「推进」）。

### 4.1 读写与推进

| 项 | 语义 |
|---|---|
| 索引 | `[0]` = 当前 bar，`[n]` = 往回 n 根；物理地址 = `base_[end - 1 - n]`，逻辑窗口 = `[end - size, end)` |
| 序号 | `ctx.bar_index()`：**当前回调对应 bar** 在品种 bar 流中的绝对序号（0 起、单调，回卷不重置；classic TB `CurrentBar` 同口径，MC 从 1 起需 −1）。`on_init` 恒为 −1；`on_bar_close` 时指向被关闭的上一根，`on_bar_open` / `on_bar_update` 时指向当前 bar。可读根数以 `ctx.close.size()` 为准 |
| 读 | `n ∈ [0, size)` 返回真实值；**其余一律抛 `std::out_of_range`**（负 n、`n ≥ size` 都抛；无钳位、无 NaN、无初值兜底）。`span()` 是零检查快路径。异常消息含 `n` / `size` / `max_lookback`，A 在派发点补充品种、回调名（§4.6） |
| 写 | 仅 `series[0] = v`；历史位不可写（与 TB/MC 一致）。**口径精确化（#155 实现期）**：「无 API」指没有任何返回 `T&` 的访问入口——`Series::operator[]` 返回写入代理 `SeriesRef`，不存在能拿到历史位可写引用的形状。两条运行期防线：① `X[n>0] = v` 仍能通过编译（迁移代码要在循环里写 `X[n]`），由 `check_index` 在到达代理**之前**抛 `std::out_of_range`；② 代理记住创建时的 `window.advance_seq`（单调序号，回卷不变），**跨 bar 保存后再写会抛** `series ref is stale`——否则被保存的 `X[0]` 代理在下一次推进后会指向历史 `[1]`，静默改写历史。`const Series` / `SeriesView` 上的赋值则是编译期错误。 |
| 推进 | 每根新 bar（由 builder 的 open 事件驱动，§5.4）：① 若 `end == buffer_bars`，先回卷（§4.4；正常由 18:00 定时器提前搬，这里是兜底）；② 新 bar 落在 `base_[end]`，7 条价格字段与量/额/持仓量从 builder 的 `Bar` 写入，用户序列写 carry（首根用 `init`，其后复制上一根 `[0]`）；③ `++end`、`size = min(size+1, max_lookback)`。`on_bar_close` 在 ① 之前触发，故 close 回调内 `[0]` 仍是上一根 |
| 视图 | `span()` 为时间正序（最旧→最新）连续窗口，长度 = `size`，可直接 range-for 或喂 STL 算法；**仅在本次回调内有效**（A 下一根推进/回卷后内容与长度可能变化） |
| 创建 | `ctx.make_series<double>(init)` / `ctx.make_series<std::int64_t>(init)`，仅 `on_init` 可调用 |
| 价格序列 | 只读 `SeriesView<double>`（`ctx.close[0]`），由 A 按 tick/合成 tick 更新 |
| 账户/规模 | B 不可见账户身份（无 `ctx.account_id()`）；交易方法里的手数是**策略手数**，框架按挂靠账户的 `scale` 换算成每账户手数（§6.2），B 不参与换算 |

### 4.2 读语义：一个检查版本，越界必响

- **写历史位的「不可达」是三层**（#155 实现期确认）：① **没有**任何返回 `T&` 的访问入口，拿不到历史位的可写引用——这是结构性不变式；② `X[1] = v` 仍可编译（迁移代码要在循环里写 `X[n]`），由 `check_index` 在到达代理前抛 `std::out_of_range`；③ 写入代理绑定创建时的 `window.advance_seq`（回卷不改序号，故不能比 `end`），**跨 bar 保存的代理再次读写都会抛** `series ref is stale`——B 可以保存 `Series` 副本（§3），但一次推进后那个代理的槽位已是历史，只检查 `n == 0` 会让写历史静默成功。若要求连编译都拒绝，需把下标做成编译期常量（`operator[]` 只接受 `std::integral_constant<0>`），会牺牲 `for (int n = 0; ...) X[n]` 这类迁移写法——与「迁移用户少改写」的目标冲突，故 v1 选择运行期响亮失败。
- **只提供一个检查过的 `operator[]`**：索引不合法就抛 `std::out_of_range`。不提供 `at()` 与 `[]` 双版本——两个入口会让策略作者猜哪个是快路径，且 TB/MC 迁移代码里 `[]` 无处不在，把正确性交给调用者选择没有收益。
- 零检查场景用 `span()`：指标循环、`std::accumulate`、`std::inner_product` 等直接吃连续区间，既没有逐元素分支，也天然对编译器向量化友好。`span()` 不做越界检查（长度就是 `size`），这是**唯一的快路径**。
- 为什么不像 TB 那样钳位/返回 NaN/返回初值：那三种做法都会让「配置错/策略读太深」变成**静默的错误状态**并可能直接产生成交。本框架走相反路线——**不合法就响亮失败**，错误信息直接指出需要调大的配置项。
- **正常策略不会碰到越界**：回调门禁（§5.2）保证任何 B 回调期间价格窗口已填满，即 `size == max_lookback`；此时 `[n]` 对 `0 ≤ n < max_lookback` 全部合法，只有下面两种情况会抛：
  1. 策略按 `n ≥ max_lookback` 回溯 → 配置过小，异常消息给出 `n` 与 `max_lookback`，调大 `max_lookback`；
  2. `on_init` 里读序列（此时 `size == 0`）→ 用法错误，改到回调里读。
- Debug 构建在抛出前加 `assert`（便于直接停在出错点）；Release 只抛异常。异常类型就是标准库 `std::out_of_range`，消息为英文小写、`prose | k=v k=v` 格式（消息本身只含数字，上下文由 A 补充）。
- 用户序列在静默填窗阶段由 A 逐根写入 `init`（carry 语义的起点），因此第一个回调时所有序列 `size == max_lookback`、历史位等于 `init`——不需要读时兜底，也不产生 NaN。

### 4.3 滑动示例

`max_lookback = 4`，`reserve_bars = 2`，`buffer_bars = 6`；`base_` 是连续数组：

```
bar   写入位置   end  size   窗口 [end-size, end)   窗口内容（最旧→最新）
 0    base[0]     1     1    [0,1)                 0
 1    base[1]     2     2    [0,2)                 0 1
 2    base[2]     3     3    [0,3)                 0 1 2
 3    base[3]     4     4    [0,4)                 0 1 2 3        ← 窗口满
 4    base[4]     5     4    [1,5)                 1 2 3 4        ← 淘汰 bar 0，零拷贝
 5    base[5]     6     4    [2,6)                 2 3 4 5        ← 物理缓冲到头（end == buffer_bars）
 6    回卷+base[4] 5    4    [1,5)                 3 4 5 6        ← ① memmove [2,6)→[0,4)、end=size ② 写 base[4] ③ ++end
```

要点：**每根 bar 只改 `end`/`size` 两个整数（不搬数据）**；只有物理缓冲用尽（或 18:00 主动回卷）时才把**当前窗口（≤ max_lookback 根）**整体搬回数组开头一次——不是“每根 bar 移动整个数组”，也不是“移动全部历史”。

### 4.4 回卷（relocation）

- 定义：把窗口 `[end - size, end)` 用一次 `std::memmove` 搬到 `[0, size)`，然后 `end = size`。**纯布局变换**：不改任何元素值、不改 `size`、不改 `bar_index`，B 看不到任何区别；任何时刻执行都合法（包括静默填窗与历史回放中途），但**永远在两次回调之间**执行。
- **v1 主路径：每天 18:00 主动回卷**。A 在 `on_start` 用 `dz_schedule_daily(18:00)` 注册每日定点定时器，触发帧经 `dz_next_event` 进入 `on_schedule`；A 识别自己的 `timer_id`（不转发给 B），对该品种的价格窗口与全部用户序列执行回卷。18:00 位于日盘收盘（15:00–15:15）之后、夜盘开盘（21:00）之前，是天然静默窗：没有 tick 正在处理，回卷只造成事件循环一次 < 0.2ms 的停顿；即使某品种此刻仍有形成中的 bar，回卷也只做布局变换，不关闭、不修改该 bar。
- **兜底：推进前内联回卷**。若定时器因事件积压被延迟/错过，导致某根新 bar 需要写 `base_[buffer_bars]`，推进函数先回卷再写。兜底只影响性能（把搬移拉进盘中路径），不会丢数据、不会改变语义。
- **不做逐小时/每 max_lookback 根定时回卷**：多 24 倍搬移量且没有正确性收益。回卷触发失败（定时器没来）不是错误，只是退化为兜底路径。
- 回卷成本：1 分钟周期（`max_lookback=1440`）每品种搬 7 条价格 + 用户序列（示例 4 条）= 11 条 × 11,520B = 126,720B；50 品种 ≈ 6.3MB，一次性约 60–130µs；15 分钟周期（`max_lookback=96`）每条 768B，50 品种 11 条 ≈ 0.4MB，约 5–10µs。每天一次。对比旧的「每 max_lookback 根收盘时搬」：总搬移量约为其 2.4 倍（每次搬满窗，但每天只攒 600 根新 bar），换来的是**搬移时机确定、永远在静默窗、且不会落在任何 bar/tick 回调路径的 p99 上**。

> **实现状态（#158）**：两条路径都已落地，共用同一个 `InstrumentContext::relocate()`——
> ① 定时器：`on_start` 末尾 `dz_schedule_daily(18:00)` 注册，`on_schedule(const
> DzScheduleEvent&)` 只在 `ev.timer_id == rollback_timer_` 时对全部存活实例回卷（B 自己的
> 定时器帧原样忽略，A 不代 B 处理）；`on_stop` 先 `dz_schedule_cancel` 收回所有权。
> ② 兜底：`advance_bar` 在 `end >= buffer_bars` 时就地搬。
>
> 回卷时刻经管理器构造参数注入（默认 18:00），内部接缝
> `bar_strategy_internal::run_bar_strategy_with(session_table, rollback_clock_ms)` 供 Seam A
> 集成测试在秒级内走通定时器路径；生产入口 `run_bar_strategy()` 恒用 18:00。定时器注册失败不
> 阻断启动（退回兜底路径），打一行 `daily rollback timer unavailable | ...
> fallback=inline_before_advance`；正常触发打一行 `daily rollback | instrument_count=…
> clock_ms=…`（回卷对 B 不可见，这行是运维侧唯一证据）。
>
> 等价性由两层测试锁定：① `bar_strategy_framework_test.cpp` 的
> `RelocationPathsAreInvisibleToStrategy`——同一份 tick 脚本跑两轮（兜底轮 `buffer_bars=138`
> 必然撞上内联回卷；定时器轮 `buffer_bars=224` 使兜底不可能的，只可能来自定时器），比较
> **全部 7 条价格序列 + 两条用户序列**的每根历史、回调序列、`bar_index`、窗口长度；
> ② `series_window_test.cpp` 的 `RelocatePreservesEveryWindowValue`——在**同一个**夹具上做
> 回卷前/后逐位快照比较（跨路径比较抓不到"两条路径同时漏搬"的情形，直接前后比较可以），
> `relocate_window` 与 `relocate_array`+`finish_relocate` 两种形态各验一遍。

### 4.5 容量配比与内存

三个配置量（全部可省略、由框架按周期自动算，见 §7）：

- `max_lookback`（C）：可读历史上限（含当前 bar），`[0] … [C-1]`；
- `reserve_bars`（R）：物理余量，决定两次回卷之间能推进多少根；
- `buffer_bars`（= `max_lookback` + `reserve_bars`）：物理分配元素数（内部字段同名，B 不可见）。

自动值（v1 无交易小节表，按 24 小时保守估算每日 bar 数 `86400 / period_sec`）：

| 周期 | 每日 bar（保守） | `max_lookback` 默认 | `reserve_bars` 下限 | `reserve_bars` 默认 | `buffer_bars` | 每序列字节 |
|---|---|---|---|---|---|---|
| 60s | 1440 | 1440 | 1584 | 2880 | 4320 | 34,560 |
| 180s | 480 | 480 | 528 | 960 | 1440 | 11,520 |
| 300s | 288 | 288 | 317 | 576 | 864 | 6,912 |
| 900s | 96 | 96 | 106 | 192 | 288 | 2,304 |

公式：`max_lookback = clamp(86400 / period_sec, 32, 2048)`；`R_min = max(2, ceil(1.1 × 86400 / period_sec))`；`reserve_bars = max(2 × 86400 / period_sec, R_min)`。校验区间：`max_lookback ∈ [32, 16384]`、`reserve_bars ∈ [R_min, 8192]`、`buffer_bars ≤ 24576`。

实现期确认的两条边界（#155）：

- **`buffer_bars ≤ 24576` 是防御性校验**：两个字段各自合法时其和必然不超过上限（`16384 + 8192 = 24576`），因此这条派生校验当前不可达，为将来放宽单字段区间保留。测试以静态断言锁定「上限 = 两字段上限之和」，避免它退化成无人验证的死分支。
- **周期下界有两个，含义不同**（#155 实现期）：`min_feasible_period()`（= 12）是「存在合法配置」的下界——低于它时 `[R_min, 8192]` 是空区间，报 `period_sec` 非法（不能报一个倒置区间）；`min_auto_reserve_period()`（= 22）是「省略 `reserve_bars` 也合法」的下界——两者之间的周期（如 12–21 秒）必须显式配置 `reserve_bars`，否则按「不静默修正」报错（消息给出 `field=reserve_bars` 的实际值与合法区间，用户据此补上显式值）。错误信息里的合法区间必须真的合法（回归用例解析消息区间并逐点验证，曾把下界写错一根）。v1 的四个周期（60/180/300/900）都远在两个下界之上。

交易小节表落地后（ADR 0018），「每日 bar 数」改用真实 session 总分钟数（1 分钟周期 = 600 根/交易日），默认内存降到约四成（`buffer_bars` 4320 → 1800）：

| 周期 | 每日 bar（真实） | `max_lookback` 默认 | `reserve_bars` 默认 | `buffer_bars` | 每序列字节 |
|---|---|---|---|---|---|
| 60s | 600 | 600 | 1200 | 1800 | 14,400 |
| 180s | 200 | 200 | 400 | 600 | 4,800 |
| 300s | 120 | 120 | 240 | 360 | 2,880 |
| 900s | 40 | 40 | 80 | 120 | 960 |

内存账（**每个进程**，每序列 = `buffer_bars × 8B`）：

- 1 分钟默认：50 品种 ×（7 条价格 + 4 条用户序列）= 550 条 ≈ 19,008,000B（约 18.1MiB）。
- 15 分钟默认：每序列 2,304B；同样 550 条 ≈ 1.21MiB。
- 上限 24,576 根/序列 = 196,608B；极端 2,050 条序列 ≈ 403MB。服务器级内存可接受，启动日志会打印实际上限总量。
- 挂靠账户数不影响内存：多账户共享同一份品种数据与序列，只需账户表（开销可忽略）。
- `max_lookback`（B 可见）与 `buffer_bars`（物理）都按上面分配；多出的 `reserve_bars` 只服务于回卷余量，不增加可读历史。

### 4.6 异常、失败实例与日志

- A 在**每一次 B 回调**外围包 `try/catch`（`std::exception` 与 `...` 都接住），异常绝不穿越引擎/C API 边界。
- 捕获后：记录一行 ERROR（含品种、回调名、异常消息），**该实例标记为失败**——不再接收任何回调、后续下单意图直接拒绝并记诊断；**进程与其它品种继续运行**。失败实例的逻辑持仓保留（不自动平仓），删除品种时才销毁。
- `on_init` 抛异常 = 该品种实例创建失败（不加入 map），记录 ERROR 后继续创建其它品种；配置错误（§7）则在启动阶段直接非 0 退出，不进入运行期。
- 失败实例在 `instrument del` 时仍尝试 `on_destroy`（异常同样捕获并记录）。

## 5. 事件模型

6 个可选回调：`on_init` / `on_bar_open` / `on_bar_update` / `on_bar_close` / `on_destroy` / `on_params_update`，统一参数 `InstrumentContext&`。

### 5.1 实盘（真实 tick）

```
新 bar 首笔 tick：
  on_bar_close(上一根) → 窗口推进（回卷兜底）→ 用本笔 tick 播种新 bar → on_bar_open → on_bar_update
其后每笔 tick：
  更新当前 bar → on_bar_update
```

- `on_bar_open` 触发时新 bar 的 `open/high/low/close` 已等于本笔 tick 价（与 TB 的 OnBarOpen 一致），读 open 不需要等下一次 update。
- 每笔 tick 恰好一次 `on_bar_update`（含新 bar 的首笔）。
- **首根 bar 没有 close**：品种实例的第一笔有效 tick 只触发 `on_bar_open → on_bar_update`（两者携带同一份快照，没有上一根可关闭）；此后每根新 bar 才是 `on_bar_close(上一根) → on_bar_open(新根) → on_bar_update`。回调门禁（§5.2）下 B 的**第一次**回调序列是 `on_bar_close(静默填窗最后一根) → on_bar_open → on_bar_update`，这根 close 属于历史填充。
- **Open 竞价窗口内同构**（[ADR 0032](../adr/0032-auction-window-strategy-events.md)）：窗口首笔 tick 即完成 `on_bar_close(上一根) → on_bar_open(新首根) → on_bar_update`，其后每笔竞价 tick 只 `on_bar_update`；开盘后第一笔连续交易 tick 只 `on_bar_update`（同一根 bar 继续），此时 `ctx.is_auction()` 由 true 变 false。窗口内没有 tick 就没有回调（纯 tick 驱动，不加定时器）。
- **`ctx.is_auction()` 标识触发相位，不是被交出 bar 的属性**（[ADR 0032](../adr/0032-auction-window-strategy-events.md) 决策 3）：竞价窗口首笔 tick 触发的那次 `on_bar_close(上一根)` 同样为 true——被关闭的是上一根普通 bar。因此不能拿它当「跳过整个回调」的开关：**只在 `on_bar_open` / `on_bar_update` 里用它拦交易意图，`on_bar_close` 应照常处理**（否则平仓记账与指标收尾会漏）。静默填窗、历史回放、`on_init`、`on_params_update` 期间恒为 false。

> **实现状态（#163）**：相位写入口径已落地（接线顺序见 §5.4）。竞价窗口首笔 tick 触发的
> `on_bar_close` 为 true、离开窗口的第一笔连续 tick 只 update 且相位转 false；`on_init` /
> `on_params_update` / `on_destroy` 恒 false（不是 bar 事件，没有触发 tick）；**回放期恒
> false**——回放复用 live 的 tick→bar 实现，竞价归档 bar 的合成 tick 会被 builder 标记
> `is_auction=true`，但回放不是实时竞价，合成 tick 的相位不外泄（抑制实现见 §5.4）。框架
> 不因相位拦截下单：窗口内意图与其它时刻走同一条执行路径。测试见 §13。

### 5.2 回调门禁：窗口填满前不回调

> **实现状态（#158）**：本节已落地。门禁判据收敛在 `BarStrategyManager` 的两个静态谓词上——
> `bar_callback_enabled`（`open` / `update`，推进**之后**判 `bar_index >= max_lookback`）与
> `close_callback_enabled`（`close`，推进**之前**判 `size == max_lookback`）。两者的差别正是
> 「填满窗口的那一根」：它的 open/update 属静默期，但它的 close 是新根的前导、必须交付。
> 静默填窗期内 `advance_bar` 照常推进窗口并给用户序列写声明初值。Seam A 集成测试见
> `bar_strategy_framework_test.cpp` 的 `CallbackGateSuppressesEverythingUntilWindowIsFull` /
> `FirstCallbackSequenceAndFullWindowReads`。

- **B 的回调从价格窗口填满（`size == max_lookback`）后的下一根 bar 开始**；此前的 `max_lookback` 根 bar 是**静默填窗**：A 照常把 tick 喂给 builder、推进窗口（用户序列写 `init`），但**不调用任何 B 回调**。
- 第一个回调发生在 `bar_index == max_lookback` 的那根 bar（此前 `max_lookback` 根，索引 `0 … max_lookback-1`，为静默填窗），序列为 `on_bar_close(bar 索引 max_lookback-1) → on_bar_open(bar 索引 max_lookback) → on_bar_update`；此后回调期恒有 `size == max_lookback`。
- 为什么门禁：回溯不足语义是**抛异常**（§4.2），不允许钳位/NaN 参与计算，所以不能像 TB 那样从 bar 0 起就回调；等窗口填满再开始，回调里所有 `[n]` 都是真实历史值，策略不需要自己判预热长度。
- 冷启动（归档域无数据，v1 现状）：从第一根 live bar 起静默填窗，到 `bar_index == max_lookback` 时开始回调。v1 默认 `max_lookback` 是 24 小时 bar 数（1 分钟周期 1440 根），按真实交易时间约 **2.4 个交易日**；归档域落地后缩短为预热回放（§5.3），默认回放 10,000 根。
- `on_init` 在实例创建时调用（早于任何 bar，`bar_index == -1`）；`on_destroy` 在实例销毁时调用；`on_params_update` 只投递给已进入回调阶段的实例（此前挂起并在首根回调后合并投递），避免在 `size == 0` 时读序列。

### 5.3 历史回放（有归档数据时）

回放有两个作用：warmup 收敛指标，以及在进程重启后**恢复逻辑持仓**（ADR 0030）。

> **实现状态（#161）**：启动衔接的四个步骤、final/forming 两条续接规则、合成 tick 回放与
> 一次性上报已全部落地。A 侧编排在 `bar_strategy/manager.h`（`on_start` →
> `create_instances`（查元数据）→ `warmup_history_replay` → `drain_buffered_ticks` →
> `enter_live`），回放原语在 `bar_strategy/dependencies.h`（`make_replay_ticks` /
> `estimate_replay_seconds`）。**历史 bar 的自洽性（升序、OHLC/量额自洽、`last_tick_sec`
> 归属本桶、竞价-only bar 的 `O=H=L=C`）是归档域 #4 的数据契约，本票不做清洗**——数据源
> 还是 stub 时写一套不可达的校验等于没有测试的假防线；归档域落地时在 #4 内实现并测试。
> 归档域本身仍未实现（#4）：默认历史源 `query_archive_history` 把
> `dz_db_query_bar` 的 `DZ_EC_NOT_IMPLEMENTED` 映射为 `Unavailable`，因此 **v1 生产路径
> 实际恒为冷启动**；回放路径由注入的假历史源在测试里完整覆盖（Seam B，§11）。

**启动衔接（订阅 → 查询 → 回放 → 续接）**：

1. A 在 `on_start` 一开始对全部 `instruments` 调 `dz_subscribe`（`replace_previous=true`），让切换窗口的 tick 进入本策略的 md reader；
2. 然后查历史并**逐品种顺序回放**（不是跨品种逐 bar 锁步）；回放期间到达的 tick 留在通道里；
3. 回放完成后 A 主动调 `dz_next_md` 排空缓冲 tick，按品种决定丢弃/续接（见下）；
4. 全部品种处理完 → 一次性上报各账户逻辑持仓 → 进入 live。

**缓冲 tick 的丢弃/续接规则**（`B` = 该品种最后回放 bar 的 `begin_sec`；实现
`should_drop_continuation_tick`，桶判定与 builder 共用 `bar::resolve_tick_bucket`，不是第二套算术）：

- 历史最后一根**已完成（final）**：丢弃 `begin_sec <= B`，保留 `> B`（下一根 bar 从 live 开始）；
- 历史最后一根**未完成（forming）**：丢弃 `begin_sec < B`，保留 `== B` 并继续更新同一根 bar（不触发 close/open），保留 `> B` 按新 bar 推进。
- final/forming 由 bar 数据源的 **`is_final` 字段**给出（定义见 [ADR 0031](../adr/0031-tick-bar-synthesis-component.md)）；归档域数据源须提供该字段（#4），缺失时按「最后一根视为 forming」实现并记 WARN。
- `begin_sec` 跳跃只记 INFO：无成交的品种本来就不生成 bar；`DzTick` 无 seq，无法区分“没成交”与“丢 tick”。

**回放成交口径**：与实时逻辑账完全一致——B 的意图按传入限价**即时虚拟成交**（不按 bar 触碰、不做部分成交/拒单模拟），回放只是在合成 tick 上重放 B 的逻辑账。因此恢复对**bar 边界决策**保真；bar 内按 tick 决策的策略在 4 合成 tick/bar 下可能与实盘分叉（限制，写入迁移表）。

```
加载 K 根历史 bar（K = warmup 深度，K ≥ max_lookback；最后一根允许未走完）
  → 先静默填窗前 max_lookback 根（合成 tick 喂同一个 builder，门禁抑制 B 回调）
  → 对剩余 K - max_lookback 根逐根回放（每根 4 个合成 tick，每 tick 一次 on_bar_update）
       B 照常运行；下单只做虚拟成交（更新逻辑持仓/Tier 1/各账户逻辑持仓），不落地、不逐笔上报
  → 缓冲 tick 续接（规则见上）→ 全部品种完成后一次性上报各账户逻辑持仓
  → 进入 live
```

- 合成 tick 顺序：上涨 `O→L→H→C`，下跌 `O→H→L→C`，平盘 `O→L→H→C`；量/额按 `k/4` 线性累计（合成近似，测试锁定）；**持仓量** 4 个 tick 都取归档的 `open_interest`（归档只有最后一笔的值，回放产出的终值与归档一致，中间 `on_bar_update` 的 OI 是近似）。**时刻以归档的 `last_tick_sec` 为末点**（前三个在 `[begin_sec, last_tick_sec]` 内均匀分布）：这样回放产出的 `last_tick_sec` 与归档一致，且 forming bar 的缓冲续接 tick 一定晚于它、不会被 builder 当乱序丢掉（若把合成 tick 铺到桶末，forming 续接必然失效）。**Open 竞价 bar**（ADR 0032）的 `last_tick_sec` 落在 `begin_sec` 之前（竞价 tick 归后一小节首桶）：4 个合成 tick 都取 `last_tick_sec`，其 tsec 经会话表仍映射到本 bar 桶，回放仍与归档一致；竞价-only bar 的 OHLC 恒为 `O=H=L=C`（builder 的竞价覆盖口径），这是归档域的同一份数据契约。
- 每根回放 bar 的事件序列与实盘同构：`on_bar_close(上一根) → 推进 → on_bar_open → on_bar_update ×4`；回放期间窗口同样保持 `size == max_lookback`（静默填窗已把窗口填满）。回放事件序由 Seam B 集成测试逐根断言，回放产出的 bar 与归档 bar 逐字段一致由纯单元测试锁定（`bar_strategy_replay_test.cpp`）。
- **单一 tick→bar 路径**：静默填窗、回放、live 都走同一个 `IntradayBarBuilder`（§5.4）——回放/静默填窗把归档 bar 拆成合成 tick 逐笔喂入，没有「直接写最终 OHLC」的旁路，否则会出现两套 bar 语义；合成 tick 的 OHLC/量额/持仓构造必须让 builder 产出的 `Bar` 与归档 bar 一致（单元测试锁定）。
- **每个品种的锚点** = 该品种窗口开始接收 bar 的第一根（进程启动时加载历史的最早一根）；回放从锚点开始，按品种顺序推进，各品种锚点独立，不需要跨实例拼接或视图重绑。**实现状态（#161）**：启动路径已落地（锚点由历史第一根 + 窗口推进自然确定，续接过滤只记录**尾部** B）；`instrument add` 的运行期锚点已由 #162 落地（复用 `replay_instrument_history`，锚点 = 该品种本次加载历史的最早一根）。
- **数据源不可用/出错**（实现状态 #161）：`dz_db_query_bar` 返回 NULL 且错误码为 `DZ_EC_NOT_IMPLEMENTED`（v1 现状）→ 默认历史源返回 `Unavailable`，A 打印 `historical bars unavailable | instrument=… status=source_unavailable error="…" fallback=cold_start` 并按冷启动继续（不是错误）；其它查询错误 → `status=query_failed`、同样 WARN + 冷启动（历史是增强项，不阻断启动）。`warmup` 在无历史时被忽略并打上述 WARN；schema 与解码不匹配（必需列缺失）返回 `Failed`，属于归档域落地时必须同步修的接缝。
- **可用历史不足 `max_lookback` 根**（`warmup.from` 太近或归档起始较晚）：先静默填入可用部分，余下从 live bar 继续静默填窗到窗口满再回调，不视为错误；实现打印 `warmup history insufficient | instrument=… available_bars=… max_lookback=… fallback=live_fill`。历史 bar 数据源的缺口/拼接契约归归档域（#4）；回放侧对 `begin_sec` 跳跃只打 INFO（`warmup history gaps | … gap_count=…`）。
- **启动回放阻塞事件派发**：回放发生在 `on_start` 内，期间不派发 SHUTDOWN/UI_INPUT/定时器帧；master Stop 超时默认 3s 强杀。启动成本上限与 WARN 见本节末尾；需要停止时接受“直接重来”（无遗留状态）。
- **回放期成交是虚拟的**（ADR 0030；实现状态 #161）：`live_ == false` 期间意图只更新逻辑台账、不落单、不逐笔上报；`enter_live()` 的调用点已挪到回放与续接之后，进入 live 时一次性上报各账户逻辑持仓（Seam B 断言回放期零订单帧、启动恰一条上报帧且携带恢复后的持仓）。停机期间的真实成交在启动时不可见（SDK ingest 水位过滤），逻辑与实际的差异由操作员一键同步处理。**回放深度就是恢复范围**——持仓周期超出 `warmup` 窗口时恢复不完整（TB 图表同款限制）。
- **v1 无归档数据时是冷启动**：不回放、逻辑持仓从 0 开始；重启后实盘与逻辑的偏差（进程关闭期间的信号）由操作员一键同步处理，框架不做启动校验。
- 回放成本 ∝ 实例数 × `K`（每实例独立回调）：每根回放 bar 有 6 次 B 回调（`on_bar_close` + `on_bar_open` + 4×`on_bar_update`），估算 0.2–0.4µs/回调 → K=10,000、`max_lookback`=1440 时约 5.1 万次回调、约 10–20ms/实例，50 品种约 0.5–1s；10 年 1 分钟（K≈145 万根）约 1.7–3.5s/实例（50 品种约 85–175s）。启动前打印估算（**实现状态 #161**：`warmup replay plan | instrument_count=… warmup_bars=… silent_bars=… replay_bars=… estimated_seconds=…`；估算口径 `estimate_replay_seconds` = 合成 tick × 0.1µs + 回放回调 × 0.3µs，`warmup.from` 模式按日期跨度取上界），预计 > 60s 打 `warmup replay slow | … threshold=60`，> 300s 打 `threshold=300`（只打一条、不阻断启动；可调小 `warmup`）。

### 5.4 bar 边界与 builder 接线

bar 边界由 tick→bar 通用组件（[ADR 0031](../adr/0031-tick-bar-synthesis-component.md)）实现：A 内**每品种一个 `bar::IntradayBarBuilder`**（连同事件适配器作为 `InstrumentNode` 的成员，§3），A 不自行判断边界、不维护形成中 bar 聚合值，`ctx` 只在事件里被动接收结果。口径见 ADR 0031：`DzTick.date` 为真实交易日；`begin_sec` = 18:00 锚点整数网格桶起点；内部边界 tick 归新 bar、小节末端 tick 归前一根；开盘竞价窗口的 tick 归后一小节首桶、窗口内正常回调（相位由 `ctx.is_auction()` 标识，[ADR 0032](../adr/0032-auction-window-strategy-events.md)）；无成交的桶不生成 bar；乱序/非交易时段 tick 丢弃并计数；日线用 tick 当日累计字段。

- **事件适配器（A 内部，B 不可见）**：把 builder 的三个 sink 回调翻成框架动作，动作顺序即回调顺序——
  - `on_bar_close(prev, is_auction)`：把 `is_auction` 写进 ctx → `B.on_bar_close(ctx)`（窗口尚未推进，`[0]` 仍是被关闭的上一根）；
  - `on_bar_open(new, is_auction)`：回卷兜底（§4.4）→ 推进窗口（`Bar` 字段落进 `[0]`、用户序列写 carry、`++bar_index`）→ 写 `is_auction` → `B.on_bar_open(ctx)`；
  - `on_bar_update(bar, is_auction)`：把 `Bar` 的 OHLC/量额/OI/`last_tick_sec` 写进 `[0]` → 写 `is_auction` → `B.on_bar_update(ctx)`。

  每个事件入口先用本笔 tick 刷新 `TickSnapshot`（价格夹取/同步单对手价）；**只有被 builder 接受的 tick 才产生事件**——乱序/非交易时段/量额倒退的 tick 被丢弃并计入 `BarBuilderStats`，不刷新快照；回调门禁（§5.2）、静默填窗/回放（§5.3）、失败实例（§4.6）都在适配器里短路。
- **单一实现**：live、静默填窗、历史回放都喂同一个 builder 的 `on_tick`（回放/静默填窗用合成 tick，§5.3）；不存在第二条 bar 边界实现，`ctx` 也不向 B 暴露边界/聚合类的半成品入口。
- 事件顺序不变：`on_bar_close(上一根) → 推进 → on_bar_open → on_bar_update`；**不使用定时器定义 bar 边界**（回卷定时器只做布局，不参与 bar 生命周期）。
- bar 标识（用于历史对齐）= `(trading_day, begin_sec)`；归档域 schema 与时间标注随归档 spec（#4）对齐该标识。
- 小节末尾不会有独立收盘事件：最后一根 bar 在下一笔更晚 tick 到达时收盘（纯 tick 驱动，无 flush）。

> **实现状态（#163）**：三个入口已按上述顺序接线（`close` / `update` 在写 `[0]` 之后、
> `open` 在推进窗口之后写相位，三者共用 `write_auction_phase`）；回放期由
> `InstrumentNode::replaying` 抑制（语义见 §5.1），`on_params_update` / `on_destroy` 入口
> 显式清零。相位只影响 B 的观测，不改变执行路径（§6）。

### 5.5 v1 退化交易小节表（已知限制）

> **实现状态（#157 / #161）**：`dztrader::degraded_trading_session_table()`（`bar_strategy/dependencies.h`）
> 是代码里的唯一构造点，`run_bar_strategy` 默认用它；启动诊断打印
> `trading session table degraded | mode=builtin sessions=1 auction_windows=0 reason=platform_db_absent`
> 声明处于退化口径。
>
> 真实表的注入点是**内部接缝**
> `bar_strategy_internal::run_bar_strategy_with<Strategy>(FrameworkDependencies)`（完整形态：
> 小节表 + 历史 bar 源 + 合约元数据源 + 回卷时刻）或只需换表的
> `run_bar_strategy_with<Strategy>(table, rollback_clock_ms)` 简化形态——按 #161 的约定，三个外部
> 依赖（历史 bar 源 / 交易小节表 / 合约元数据）都不进公开承诺面，数据源落地时只改框架内部、
> 不改公开头面。是否退化由表自身判定（`is_degraded_session_table()`），不另设标志位——两个独立
> 状态会互相矛盾，那样打印出的诊断就是假的。

交易小节/竞价窗口表的唯一真相源是 platform.db（[ADR 0018](../adr/0018-strategy-side-kline-and-trading-sessions.md) 决策 2/3），v1 不落地该数据源。A 在表数据源缺失时构造内置**退化小节表**喂给 builder：

- 单小节 `[begin_sec = 0, end_sec = 86399]`（18:00:00 → 17:59:59，`TradingSessionTable::valid()` 合法、覆盖全部交易日内秒），`auction_count = 0`（无竞价窗口）。
- 这不是新数据源、也不是配置项（[ADR 0032](../adr/0032-auction-window-strategy-events.md) 决策 8 不引入占位配置）；只是数据源落地前框架内置的临时表，代码里只有一处构造，落地后删除。

由此产生三条与生产口径不同的**已知限制**（迁移与策略必须知晓，platform.db 落地后消失）：

1. **竞价 tick 按普通 tick 处理**：新交易日首根 bar 落在竞价 tick 所在的整数桶（1 分钟周期是 20:59、15 分钟周期是 20:45），随后 21:00 再开一根；ADR 0032 的生产语义是竞价 tick 折入会话首根（`begin_sec` = 21:00）、`ctx.is_auction()` 为 true。退化表下 `ctx.is_auction()` **恒为 false**（#163 的相位通道已实现，但退化表没有竞价窗口可命中，见 §5.1 实现状态）。
2. **小节末端整点 tick 会开新 bar**：15:00:00、10:15:00、23:00:00 等小节边界整秒的 tick 落进下一桶，生成一根 1-tick bar；真实小节表下该 tick 归前一根（ADR 0031 决策 5）。
3. **无交易时段过滤**：非交易时段的异常 tick 不会按 `out_of_session` 丢弃（非法时间、乱序、量额倒退仍会丢弃并计数）。

## 6. 交易与执行

> **实现状态（#159 / #160）**：**虚拟台账 + Tier 1 + 四个开平意图 + 挂靠执行与上报已落地**。
> 台账落 `libs/strategy_api/include/dztrader/bar_strategy/position.h`（`PositionState` /
> `InstrumentSpec` / `apply_intent` 纯转移，可独立单测）；缩放与价格落
> `bar_strategy/scaling.h`（`AttachedAccount` / `ScalingConfig` / `logical_position()`）与
> `bar_strategy/execution.h`（`TickSnapshot` / `resolve_order_price()` / `order_volume()`）——
> 两者都是纯函数，因此逐格单测（`bar_strategy_execution_test.cpp`）而不必搭帧通道。B 可见的
> 四个意图方法、Tier 1 查询、分级诊断、失败实例拒单、**挂靠 fanout 与两个唯一出口**在
> `bar_strategy/context.h`，Seam A 接线在 `bar_strategy_framework_test.cpp`（订单帧与上报帧
> 用独立 probe reader 断言）。
>
> **合约元数据（#161 已接上）**：默认实现在 `bar_strategy/dependencies.h`
> （`load_archive_metadata` → `dz_db_query_instruments`，必需字段 `volume_multiple > 0`、
> `price_tick > 0`、`exchange_id` 非空），测试可注入 fake。四种结局：
> `Loaded` → 实例按元数据执行（盈亏乘乘数、价格按 tick 对齐）；
> `Missing` / `Invalid` → **该实例创建失败**（打印 `instance creation skipped | …`，进程与其它
> 品种继续）；`SourceUnavailable`（v1 无 platform.db 的常态）→ 部署级降级，两项为 0/空且启动
> 诊断声明已知限制（`instrument metadata unavailable | … reason=platform_db_unavailable`）。
> 生命周期命令的同步单（`account add/set/del`、`instrument del` 的 `[sync|nosync]`）已由 #162 落地，
> 价格取自 `resolve_sync_price()`（`execution.h`，对手价 / 最新价回退 / 市价），命令编排见 §8。
> 历史回放的虚拟成交与回放后一次性上报已由 #161 落地：`enter_live()` 的调用点在回放与缓冲
> 续接之后，`publish_all_positions()` 一次性补齐各账户逻辑持仓。

**账户与逻辑持仓**（ADR 0028/0030）：策略进程只有一个逻辑账户；挂靠账户 0..N 个，每个有 `scale`。策略虚拟净持仓 `P` 是 B 可见的唯一持仓口径；挂靠账户 `i` 在某品种上的**逻辑持仓** `L_i = sign(P) × f(scale_i × |P|)`（`f` 为进程级取整策略，§6.2）。正常策略单只改变 `P` 与 `L_i`，不参考实际持仓。

**下单意图**（v1 仅限价单；签名见 §3，四个方法返回 `void`：B 只表达意图，受理/拒单/成交结果不可见，只进框架诊断）：

```cpp
ctx.buy(volume, price);          // 买方向：开/加多，反向时先平全部空再开 volume 多；volume=0 只平空
ctx.sell(volume, price);         // 卖方向：平多（只减不增）；volume=0 全平多
ctx.sell_short(volume, price);   // 卖方向：开/加空，反向时先平全部多再开 volume 空；volume=0 只平多
ctx.buy_to_cover(volume, price); // 买方向：平空（只减不增）；volume=0 全平空
```

- **四函数是开平意图，不是方向别名**（与 TB/MC 默认图表信号语义一致；`P` = 逻辑净持仓，多正空负，`n = volume > 0`）：

| 调用 | `P < 0` | `P = 0` | `P > 0` |
|---|---|---|---|
| `buy(n)` | `P = +n`（平全部空，开 n 多） | `P = +n` | `P += n`（加多） |
| `sell_short(n)` | `P -= n`（加空） | `P = -n` | `P = -n`（平全部多，开 n 空） |
| `sell(n)` | 无操作 | 无操作 | `P = max(P-n, 0)`（平多，超出全平） |
| `buy_to_cover(n)` | `P = min(P+n, 0)`（平空，超出全平） | 无操作 | 无操作 |

- **`volume == 0` = 只平不建，平哪边由调用方向决定**（买方向 `buy`/`buy_to_cover` 平空，卖方向 `sell`/`sell_short` 平多；等价对：`buy(0) ≡ buy_to_cover(0)`、`sell(0) ≡ sell_short(0)`）：

| 调用 | `P < 0` | `P = 0` | `P > 0` |
|---|---|---|---|
| `buy(0)` / `buy_to_cover(0)` | `P = 0` | 无操作 | 无操作 |
| `sell(0)` / `sell_short(0)` | 无操作 | 无操作 | `P = 0` |

  出口函数的 `0 = 全平` 是 TB 约定（`Share` 缺省/0 平掉当前所有持仓）；入口函数的 `0 = 只平反向、不建新仓` 是 MC 口径（`Buy 0`/`SellShort 0`）。两者在净持仓模型下是同一条规则：**`0` 是模式值不是数量**——出口函数 `sell(0)` 与 `sell(1)` 不连续，用计算量调平仓函数时必须自己保证 `> 0`（或显式分支）。
- **参数与诊断**：`volume >= 0`，负数拒单并诊断；`n > 0` 方向无效（如空仓 `sell`）无操作 + WARN 诊断，`n > |P|` 钳制到 `|P|` + WARN 诊断；`0` 且无可平仓无操作、**不诊断**（避免平仓语句逐 bar 刷屏），`0` 实际触发平仓记 INFO 诊断（便于追溯非预期清仓）。日志文本用小写 prose 区分（`intent rejected` / `intent ignored` / `intent clamped` / `intent flat_by_zero`）；注意 header-only 框架只能写 stderr，而 master 把子进程 stderr 一律按 WARN 转发，因此**当前这三档在 master 日志里级别相同、只靠 prose 区分**——真正的级别分流要等 SDK 给策略代码一条日志通道（ADR 0021 现状）。
> **已知门禁缺口**：`scripts/check_log_format.py` 只扫 spdlog / `LastError::set` / `throw std::*` 三类调用，**不扫 `std::cerr`**，因此本组件（以及 `strategy_engine.h`）里的 stderr 诊断没有日志格式机器校验，靠 review 把关（#157 起即如此）。规模：`#160` 新增 7 处（`bar_strategy/context.h` 4→9、`manager.h` 13→15）；`#161` 在 `manager.h` 新增 11 处（15→26，回放/续接/元数据诊断），三个头现共 45 处（`manager.h` 26 + `context.h` 9 + `strategy_engine.h` 10）；全仓 `main` 本有 72 处、`#160` 后 79 处、本票后 90 处。补齐需要给 lint 加一类调用并把既有的 stderr 一并纳入——那是独立改动（会一次暴露全仓既有站点），不在框架票范围内。
- **逻辑台账是虚拟成交（TB 虚拟账户口径）**：B 的每笔交易意图按传入限价**即时**记账——`P` 按上表推进，Tier 1（均价、建仓次数、浮动盈亏、进出场时间）在同一个漏斗里按开平归属更新（跨越 0 时拆成平仓段 + 开仓段）。不等待成交回报、不跟踪在途；拒单/部分成交不回流。B **只接触**这本逻辑账：不接触账户真实持仓/资金/权益，也不接触任何委托与成交明细（`order_id`、委托状态、部分成交、拒单原因都不出现在 B 的 API 面）。
- `on_order_report` / `on_position_info` / `on_trading_account` v1 忽略，不参与任何 B 可见状态。

**执行（ctx 内完成）**：

- 逻辑持仓变化 `P_old → P_new` 后，对每个挂靠账户算 `qty_i = L_i(P_new) − L_i(P_old)`；`qty_i == 0` 跳过。
- 方向 = `sign(qty_i)`，`position_effect = DZ_POSITION_EFFECT_AUTO`（开平/锁仓/今昨由 td 按账户解析，平台专项 issue #82）。本框架固定 AUTO；特定策略/品种若需显式 `OPEN`/`CLOSE_TODAY` 等标准开平指令，走平台标准下单接口，不属于 BarStrategy（ADR 0028 决策 8）。
- 价格 = B 传入的限价：`price_tick` 对齐（买向下取整、卖向上取整，不向不利方向偷价；发生调整时记诊断）；实盘涨跌停：买价高于涨停 → 按涨停价挂，卖价低于跌停 → 按跌停价挂（不拒单）；价格非法（NaN/≤0）或数量非法（负数、或 `|qty_i|` 超出 `DzVolume` 范围）才拒单并诊断。
  **实现状态（#160 / #161）**：涨跌停夹取已生效（涨跌停来自 tick 本身）；tick 对齐在拿到合约元数据后生效（`InstrumentSpec::price_tick > 0`），元数据源不可用时按契约跳过对齐、不拒单。对齐的三种边界（买向下/卖向上/已在网格上不调整）由 `bar_strategy_execution_test.cpp` 用注入的 `price_tick` 覆盖；端到端接线（元数据 → 委托价对齐）由 Seam B 的 `InjectedMetadataRecoversTier1AndPriceAlignment` 在真实帧通道上锁定。
- 调 `dz_place_order(dz_, account_id, instrument_id, direction, DZ_PRICE_LIMIT, price, |qty_i|, AUTO)`；**A 不参与下单、不记录订单路由表**。
- 每账户手数**不保证**等于「策略手数 × scale」，保证的是持仓比例（`L` 的差量，取整误差不随逐单累积）；反手/全平同样按差量发**一张** AUTO 单，不由框架拆平/开（td 按账户持仓解析，issue #82）；不做基于实际持仓的「超出可平量丢弃」——逻辑账层只按 §6 规则钳制到逻辑持仓，实际可平量不足由 td/交易所拒绝，偏差走一键同步。
- **不看实际持仓**：实际仓位不足时单子可能被 td/交易所拒绝；拒单不改变逻辑账，实盘与逻辑的偏差由操作员一键同步处理（§6.2）。

**上报**：每次逻辑持仓变化后，对 `L_i` 发生变化的账户调 `dz_set_logical_position(dz_, account_id, instrument_id, L_i)`；删除账户用「账户 + 空品种」通配清零（§8，命令层经 `InstrumentContext::publish_account_wildcard_zero` 调用同一出口）。策略级 `P` 不单独上报。

两条上报时机与「只报变化的账户」并列，不冲突：

- **逐笔**（上段）：只在 `L_i` 变化时，`L_i` 未变即跳过（否则每笔意图刷 N 条无变化帧）；
- **全量对账**（`publish_all_positions()`：进入 live、回放结束、`instrument add`、账户命令后）：对**全部**账户各报一条当前 `L_i`，**`0` 也报**。这条路径的目的就是让 UI 镜像与台账一致——跳过 0 会让上次运行留下非零镜像的账户在冷启动（`P == 0`）后永远显示旧持仓。回放期不逐笔上报，回放结束后由它一次性补齐（§5.3）。**失败实例也报**（#161）：实例在回放期抛异常后 §4.6 保留其逻辑台账，若启动时跳过上报，UI 镜像会停在旧值；执行门禁对它保持关闭（失败实例本来就拒单）。

**成交回报**：v1 全部忽略（不建成交台账；逻辑账只由 B 的交易意图推进；实际与逻辑的偏差由操作员一键同步处理）。

**失败实例**：不再受理下单意图（ctx 内失败标记，`buy/sell/...` 检查后拒绝并记诊断），也不会自动平仓（§4.6）。

### 6.1 持仓/进出场状态（Tier 1，v1）

> **实现状态（#159）**：本节全部 API 与口径已落地（`context.h` 的 Tier 1 查询 +
> `position.h` 的台账转移），单元测试 `libs/strategy_api/tests/bar_strategy_position_test.cpp`
> 逐条覆盖（8 个 `Tier1Test` 用例 + 全表/即时性/诊断/拒单各组）；Seam A 接线
> （`IntentLedgerIsWiredIntoRealCallbacks`）覆盖「意图在真实回调里即时生效」与「进场时间取
> bar 标准结束时间」；**极值更新时机**由单元测试经注入的 `InstrumentSpec` 覆盖（Seam A 路径
> 上乘数为 0，观察不到极值数值）。**元数据来源（#161 已接上）**：`volume_multiple` 由合约
> 元数据接缝供给；platform.db 不可用时盈亏与极值仍恒为 0（启动诊断声明该已知限制），数据源
> 缺失/非法时该实例创建失败而不是静默按 0 跑（见 §6 实现状态）。

签名与可见性见 §3（`InstrumentContext` public 区）；本节只定义语义与口径。状态落在 §3 的 `PositionState`（`market_position()` 由 `net_volume` 派生，不存字段）；Tier 1 在「交易意图 → 虚拟成交」的单一漏斗上记账（下单即更新，ADR 0030），B 只读；语义对齐 TB 文档但按本项目口径裁剪：

| API | 语义 |
|---|---|
| `ctx.market_position()` | -1 / 0 / 1（净持仓方向） |
| `ctx.current_contracts()` | 持仓手数（≥ 0，绝对值；方向看 `market_position()`） |
| `ctx.avg_entry_price()` | 当前持仓加权平均建仓价；部分平仓不变，反手重置 |
| `ctx.current_entries()` | 当前持仓建仓段数（每次开仓/加仓计 1，反手新开计 1；部分平仓不变；无仓 0） |
| `ctx.max_contracts()` / `max_entries()` | 本实例历史最大持仓手数（`|P|` 峰值）/ 最大建仓段数 |
| `ctx.entry_price()` / `entry_date()` / `entry_time()` | 当前持仓第一笔建仓（日期时间为该 bar 的标准结束时间） |
| `ctx.last_entry_price()` / `last_entry_date()` / `last_entry_time()` | 最近一笔加仓 |
| `ctx.exit_price()` / `exit_date()` / `exit_time()` | 最近一笔平仓（含部分平仓与反手平仓段） |
| `ctx.bars_since_entry()` / `bars_since_last_entry()` / `bars_since_exit()` | 距首次建仓 / 最近加仓 / 最近平仓的 bar 数（同 bar=0；无参考 -1；`on_bar_close` 内用被关闭 bar 的 `bar_index`） |
| `ctx.position_profit()` / `ctx.contract_profit()` | 浮动盈亏 / 每手浮动盈亏：`(最新价 − avg_entry_price) × P × volume_multiple` / `(最新价 − avg_entry_price) × sign(P) × volume_multiple`；不含费用；无仓 0 |
| `ctx.max_position_profit()` / `max_position_loss()` | 当前持仓期间浮盈/浮亏极值；bar 收盘前用该 bar 收盘价更新；开仓重置 0、平仓后归零 |

口径补充：`volume_multiple` 来自 `InstrumentSpec`（必需字段 > 0，缺失则实例创建失败——该判定随元数据接缝在 #161 落地；**platform.db 不可用时**仍为 0、盈亏与极值恒为 0 并由启动诊断声明，见 §6 实现状态）；盈亏单位是钱（不是价格点）；`entry/last_entry/exit` 的日期时间取对应 bar 的标准结束时间（`begin_sec + period_sec`）。与 TB 的差异：`max_contracts` 是本实例历史峰值（TB 的 `MaxContracts`/`MaxContractsHeld` 区分当前持仓期与历史），状态一律由虚拟成交驱动、与真实成交无关。`current_contracts` 返回非负手数（TB 同口径），内部 `PositionState.net_volume` 仍有符号。

返回值类型：`market_position` / `current_entries` / `max_entries` 为 `std::int32_t`（计数类 ≥0）；`current_contracts` / `max_contracts` 为 `LargeVolume`；`bars_since_*` 为 `std::int64_t`（与 `bar_index` 同宽，不做差值窄化；无参考值返回 -1）；价格/盈亏为 `double`；进出场日期/时间为 `DzDate` / `DzTime`。

Tier 2 交易统计（`ctx.stats()`：GrossProfit/GrossLoss/NetProfit/PercentProfit/NumWin|Los|EvenTrades/LargestWin|LosTrade/MaxConsecWinners|Losers/TotalTrades/ExitSize/TotalBars*/AvgBars*）后置到 M5，成交漏斗保留 trade-close 钩子，后续为纯加法。

### 6.2 挂靠账户、缩放与同步

> **实现状态（#160）**：`L_i` 的换算、正常单 fanout、两个唯一出口（下单/上报）、价格对齐与
> 涨跌停夹取**已落地**。纯函数在 `bar_strategy/scaling.h`（`logical_position()` 与
> `ScalingConfig`）与 `bar_strategy/execution.h`（`resolve_order_price()` /
> `order_volume()` / `TickSnapshot`），单元测试 `bar_strategy_execution_test.cpp` 逐格覆盖
> （三种取整 × 正负持仓、`scale_min_one`、非法 `scale` 防御、饱和、差量之和恒等于
> `L(终态) − L(初态)` 因而取整误差不累积、tick 对齐边界、涨跌停夹取、非法价格/数量）；
> Seam A 测试覆盖镜像与反手单、`L_i` 未变化时零帧、0 账户零帧、配置里的取整策略与
> `scale_min_one` 到达执行路径、`on_init` 意图不发单（live 门禁）、涨跌停夹取诊断。
> **同步单（`account add/set/del` /
> `instrument del` 的 `[sync|nosync] [market]`）由 #162 落地**：#160 留出的两个唯一出口
> （`submit_order()` / `publish_position()`）就是命令层的调用面，命令层不另拼帧。价格来源
> 抽成纯函数 `resolve_sync_price()`（`execution.h`，组件文档 §6.2 的口径：`market` → 市价单；
> 默认对手价（买取卖一、卖取买一）；盘口缺档回退最新价；两者都无 → 跳过该品种并诊断，逻辑
> 持仓仍按命令更新）。`TickSnapshot` 因此新增 `last_price` / `bid_price` / `ask_price`
> 三个字段（#160 的注释已预告「随命令层加进来」）。

**账户表与逻辑持仓**：A 持有挂靠账户表（启动读配置，运行期 `account` 命令增删改，§8）。账户 `i` 在某品种上的逻辑持仓：

```
L_i = sign(P) × f(scale_i × |P|)
f = nearest（默认，四舍五入） | floor（向少） | ceil（向多）
scale_min_one：目标非零时至少 1 手（默认关）
```

即「挂靠比率把策略逻辑持仓放大/缩小到该账户」。`max_scale` 在启动时从配置加载、盘中不可变；每个 `scale ∈ (0, max_scale]`。

**正常单的每账户手数**：`qty_i = L_i(P_new) − L_i(P_old)`（§6）。因此账户手数不保证等于「策略手数 × scale」，保证的是持仓比例；取整在目标持仓上做，误差不随逐单累积（ADR 0030）。

**生命周期命令的同步单**：`account add/set/del` 或 `instrument del` 带 `sync` 时，先按命令引起的逻辑持仓变化量发单，再把逻辑持仓更新为目标值；`nosync` 只更新逻辑持仓、不发单。差额按账户/品种计算（`old_l` = 命令前的 `L_i`，`new_l` = 命令后的 `L_i`；新账户 `old_l = 0`）：

- `account add`：`qty = new_l`（该账户的初始逻辑目标）；
- `account set scale`：`qty = new_l − old_l`；
- `account del` / `instrument del`：`qty = −old_l`（平掉本策略在该账户/品种的逻辑贡献，目标为 0）。

**已确认口径（2026-10-21，防后续理解偏差）**：

- `account add` 的差额按 `new_l` 计算，**假设该账户在这些品种上的实际持仓为 0**；若不为 0，sync 单会在已有实际仓上叠加，偏差交给账户级一键同步。
- `instrument del` 与账户命令一样支持 `[sync|nosync] [market]`（默认 `nosync`；`market` 只对 sync 生效）。
- 失败语义：**命令层失败**（校验失败/写配置失败）→ 不发单、逻辑持仓不变；**下单失败（含写帧失败）或未成交** → 逻辑持仓照常更新为目标值，实际差额由一键同步处理。

方向取 `sign(qty)`、`effect=AUTO`；**先发单，然后无论成交与否（含下单失败）都把逻辑持仓更新为 `new_l`**（删除则移除并上报清零）；不查询实际持仓、不确认成交、不建成交台账。价格默认**对手价**：买用卖一、卖用买一（最近一笔 tick；tick 对齐、涨跌停夹取）；无可用行情或价格非有限/≤0 时回退最新价，仍不可用则跳过该品种并报错；命令带 `market` 时用市价单。（`TickSnapshot` 的对手价与最新价字段随本条命令能力在 #162 加进来，见 `execution.h` 的 `resolve_sync_price()`。）

- 无近期行情的品种跳过并输出错误行；逻辑持仓仍按命令更新。
- 删除账户：`sync` 时先发 `−old_l` 平仓单 → 立即移除账户表项并上报清零（ADR 0028）。删除品种：`sync` 时对各账户发 `−old_l` → 移除实例并清零上报（§8）。

**一键同步**（未来 WebUI，平台侧能力）：账户级手工对账，比较账户实际净头寸与各策略上报的逻辑持仓，发单把实际调向系统口径；策略框架不参与（ADR 0028）。v1 没有启动校验：策略启动即认为持仓正常，实盘匹配是操作员责任（ADR 0030）。

**净头寸口径**：逻辑持仓与同步都只按净额；账户内部多空明细/锁仓由 td 的 AUTO 解析，框架不建模，失真可接受（ADR 0028）。

## 7. 配置

> **实现状态（#156）**：配置层已实现，落在 `libs/strategy_api/include/dztrader/bar_strategy/config.h`
> （JSON 解析 / 语义校验 / 自动值推导 / 有效配置日志，纯组件、不碰文件系统）与
> `dztrader/bar_strategy/config_file.h`（原子写、读文件、缺失生成、`ConfigStore` 写盘后改内存）。
> 单元测试 `libs/strategy_api/tests/bar_strategy_config_test.cpp` 与
> `bar_strategy_config_file_test.cpp`。
>
> **接入状态（#157）**：`run_bar_strategy` 已接上四项——`config_path(dz_strategy_home(ctx),
> dz_strategy_id(ctx))` 取路径；`GeneratedDefault` 时打印 `result.message` 并以
> `config_startup_exit_code(status)`（=1）退出；`Loaded` 时用
> `ConfigStore(path, std::move(result.document), std::move(*result.effective))` 构造（**不重读
> 文件**：文件在两次读之间被改动会让内存镜像与磁盘不一致，且这是 `load_config_file` 已读到的
> 那份 document，留给后续票的 `param set` / `account *` 写回用）；
> 读/解析异常（`std::runtime_error`）与配置非法（`std::invalid_argument`）统一转成「打印错误 +
> 非 0 退出」；启动向 stderr 打印一行有效配置 + 一行账户表。接线点在
> `libs/strategy_api/include/dztrader/bar_strategy/manager.h`（`load_startup_config` /
> `print_startup_diagnostics`），集成测试 `bar_strategy_framework_test.cpp` 覆盖缺失与非法
> 配置两条退出路径。实现期确认的口径见本节末「实现期确认」。

**路径与命名**：

- 文件：`$dz_strategy_home()/<strategy_id>.json`；`dz_strategy_home()` 为策略可执行文件目录（SDK 既有接口），`strategy_id` = exe stem。
- 为什么带 `strategy_id` 前缀：部署时同一目录可能放多个策略可执行文件，前缀保证配置不冲突。
- **调试/Release 不区分文件**：配置是数据不是构建产物；Debug/Release 的 exe 目录天然不同，已自动隔离。
- **回测预留**：`<strategy_id>.backtest.json`（可选，覆盖合并到基础配置）；v1 不实现，不建模式子目录。
- 写回：同目录临时文件 + `fsync`（Windows `_commit`）+ 原子替换（POSIX `rename` / Windows `MoveFileEx`）；文件 `fsync` 失败必须报错，POSIX 父目录 `fsync` 为尽力而为（部分文件系统不支持）；任何一步失败都清理临时文件、旧配置保持不变。WebUI 严禁直接读写。
- 缺失：自动生成**最小默认配置**（五个键：`schema_version=1`、`period_sec=60`、`instruments=[]`、`accounts=[]`、`params={}`；不含 `max_scale` / `scale_rounding` / `scale_min_one` / `max_lookback` / `reserve_bars` / `warmup` 这六个可选字段），输出错误提示后以非 0 退出，等用户编辑后重启。生成的配置**故意不是可运行配置**（`instruments` 为空，重启仍会报错退出），避免框架拿一份空配置静默启动。
- `schema_version` 字段用于后续迁移（当前只接受 1；缺失或非 1 一律报错）。

**字段与校验**：

| 字段 | 必填 | 省略时 | 显式提供时 |
|---|---|---|---|
| `schema_version` | ✅ | — | 必须为整数 `1`（其它值/类型报错） |
| `accounts` | ❌ | `[]`（0 个挂靠账户：只记逻辑台账、不发实盘单） | 数组 `{"account_id": 非空字符串, "scale": 有限正数}`；`scale ∈ (0, max_scale]`；账户标识不得重复；账户的登录/凭据属于平台 td 配置 |
| `max_scale` | ❌ | `1.0` | 有限正数；挂靠比率上限，启动加载一次、盘中不可变（运行期编辑无效，重启生效） |
| `scale_rounding` | ❌ | `"nearest"` | `"nearest"` / `"floor"` / `"ceil"`：逻辑持仓 `L_i = f(scale_i × |P|)` 的取整方向（§6.2） |
| `scale_min_one` | ❌ | `false` | 目标非零时至少 1 手（保证小比率账户参与，会放大实际比例） |
| `period_sec` | ✅ | — | ∈ {60, 180, 300, 900}（v1） |
| `max_lookback` | ❌ | `clamp(86400/period_sec, 32, 2048)`（§4.5） | 整数 ∈ [32, 16384]；调大以支持更深回溯 |
| `reserve_bars` | ❌ | `max(2×86400/period_sec, R_min)`；`R_min = max(2, ceil(1.1×86400/period_sec))` | 整数 ∈ [`R_min`, 8192]；允许小于默认值（把回卷兜底拉进盘中，性能自负） |
| `warmup` | ❌ | `{"bars": 10000}`，并自动提升到 ≥ `max_lookback` | `bars` 与 `from` 二选一（互斥）；`bars` 整数 ∈ [`max_lookback`, 10,000,000]；`from` 为 `YYYY-MM-DD` 本地日期且不晚于今天（可用历史不足 `max_lookback` 根时按可用部分静默填窗、余下 live 补足，INFO 记录） |
| `instruments` | ✅ | — | 非空字符串数组（纯合约代码，无品种参数）；元素非空且不得重复 |
| `params` | ❌ | `{}` | 全局参数（每品种相同；策略手数等普通参数由策略自定义，框架不做换算；挂靠比率不属于参数，见上） |

- 派生校验：`buffer_bars = max_lookback + reserve_bars ≤ 24576`；超出 → 报错（提示调小任一值）。
- `instruments` 不得为空（空配置视为未完成初始化，报错退出）；`accounts` 可为空（0 个挂靠账户时策略照常运行，只记逻辑台账、不发实盘单）。
- `warmup.bars` 按 JSON 整数解析（int64，拒绝负数/小数）；`warmup.from` 解析为本地日期。
- 所有“自动值”也走同一套校验（例如某周期的 `R_auto` 越界 → 报错并提示显式配置，不静默越界）。
- **不合法一律报错退出，不静默修正**（与现有配置纪律一致）；错误信息给出字段名、当前值与合法区间（区间类错误）或期望形态/原因（类型、必填、重复、互斥、未知键），多词值按日志格式加双引号。
- 配置可写性：部署目录只读时的覆盖方式待部署需求确认（§14）。

**实现期确认（#156）**：

- **分层校验**：`parse_config` 做 JSON 结构/类型/枚举取值/必填存在性 + schema 版本值与数值表示层（超出 int32 的周期/容量值按字段合法区间报错）；字段取值集合、非空/唯一、数值区间、跨字段关系等语义校验在 `resolve_effective_config`。数值区间错误消息沿用 `series_internal::resolve_capacity` 的「字段名 / 当前值 / 合法区间」形态，不另写一套。
- **未知字段**：顶层未知字段忽略（前向兼容，《帧契约：general》§8）；`accounts` 元素与 `warmup` 对象是**闭集内联对象**，未知键视为拼写错误报错（`field=accounts[0].scales ... reason=unknown_field`）——否则写回重建这两个对象时会静默丢失未知键。已知字段出现 JSON `null` 一律非法。
- **写回后可重新加载**：命令层（#162）写回前必须经 `resolve_effective_config` 全量校验，写回的文件一定还能通过启动加载校验（例：删掉最后一个品种导致 `instruments` 为空 → 该命令失败、内存与文件都不变）。
- **写回复原省略态与未知字段**：`ConfigStore::commit` 以当前文件为基底，只覆盖已知顶层字段；用户省略的可选字段写回后仍省略，文件中未知的顶层字段原样保留。
- **运行期不可变字段**：`ConfigStore::commit` 拒绝改变 `max_scale`（ADR 0027 决策 11：启动加载一次、盘中不可变）与 `period_sec`（启动期架构参数：换周期需重启重建实例），报错且内存与文件都不变。
- **加载结果形状**：`load_config_file` 返回 `{status, optional<EffectiveConfig>, document, message}`——缺失生成默认配置时 `effective` 为空（默认配置故意不合法），`document` 是生成的默认配置；`Loaded` 时 `document` 可直接交给 `ConfigStore(path, document, effective)`，避免重读文件与二次解析。
- **时间注入**：文件层 `load_config_file(path, ConfigLoadOptions{today})` 与 `ConfigStore(..., ConfigLoadOptions)` 可注入「今天」（文件层测试确定性）；`ConfigStore::commit` 重算校验沿用构造时的注入值，不读墙钟；省略 = 本机本地日期（与 #157 启动时机一致）。
- **派生值不标来源**：`buffer_bars` 是纯派生值（= `max_lookback` + `reserve_bars`），日志不单列来源；`max_scale` / `scale_rounding` / `scale_min_one` 省略即默认、非推导值，日志同样不带 source（以上示例为准）。
- **有效配置日志**：`warmup` 用 `from` 模式时把 `warmup_bars=` 换成 `warmup_from=YYYY-MM-DD`；0 个挂靠账户时账户行输出 `accounts effective | -`。框架启动时经 stderr 打印一行有效配置 + 一行账户表（header-only 公开代码不引入 spdlog，ADR 0020/0021）。
- **原子写与既有实现的关系**：SDK 公开头不能依赖 `libs/core` 的 `json_section.h`（分层约束），故文件层自带一份原子写；相对先例额外满足本票 AC 的 `fsync`（文件 `fsync` 硬要求、POSIX 父目录尽力而为），JSON 字节格式沿用 `dump(2)`、**不追加结尾换行**。两份实现并存的取舍见 [ADR 0039](../adr/0039-sdk-config-file-atomic-write.md)。

**C++ 侧配置类型（A 内部）**：

```cpp
enum class ScaleRounding { Nearest, Floor, Ceil };   // 配置写作 "nearest"/"floor"/"ceil"

struct AttachedAccount {
    std::string account_id;                  // 非空、配置内唯一
    double scale = 1.0;                      // (0, max_scale]
};

struct ScalingConfig {                       // 生效值（§3；由落盘 optional 字段解析/取默认得到）
    double max_scale = 1.0;
    ScaleRounding rounding = ScaleRounding::Nearest;
    bool min_one = false;
};

struct WarmupSpec {                          // 二选一（对应 JSON 的 warmup）
    std::optional<std::int64_t> bars;        // 按 JSON 整数 int64 解析；有效区间 [max_lookback, 1e7]
    std::optional<std::string> from;         // YYYY-MM-DD（本地日期）
};

struct BarStrategyConfig {                   // 只描述落盘字段
    std::int32_t schema_version = 1;         // 只接受 1
    std::int32_t period_sec = 60;
    std::vector<std::string> instruments;
    std::vector<AttachedAccount> accounts;   // 可为空（0 个挂靠账户）
    std::optional<double> max_scale;         // 省略 = 1.0（保留省略态供写回）
    std::optional<ScaleRounding> scale_rounding;  // 省略 = Nearest
    std::optional<bool> scale_min_one;       // 省略 = false
    std::optional<std::int32_t> max_lookback;  // 省略 = 自动
    std::optional<std::int32_t> reserve_bars;  // 省略 = 自动
    WarmupSpec warmup;                       // 省略 = {bars: 10000}
    nlohmann::json params = nlohmann::json::object();  // 落盘形态（A 的 ParamStore 见 params.h，#162）
};

struct EffectiveConfig {                     // 校验 + 推导后的有效值（有效配置日志的数据源）
    BarStrategyConfig config;
    ScalingConfig scaling;                   // 生效的 max_scale / rounding / min_one
    std::int32_t max_lookback;               // 显式或按周期自动推导
    std::int32_t reserve_bars;
    std::int32_t buffer_bars;                // = max_lookback + reserve_bars
    ConfigValueSource max_lookback_source;   // explicit / auto
    ConfigValueSource reserve_bars_source;   // explicit / auto
    std::optional<std::int32_t> warmup_bars; // 与 warmup_from 恰有一个有值
    std::optional<std::string> warmup_from;
    ConfigValueSource warmup_source;         // explicit / default（默认值的自动提升也归 default）
    std::int64_t mem_per_series_bytes;       // = buffer_bars × 8B
};
```

`BarStrategyConfig` 只描述落盘字段；运行期的账户表/缩放配置保存在 `BarStrategyManager`（`account` 命令改账户表后原子写回 `accounts`）。

**`max_lookback` vs `warmup` 的分工**（容易混淆，必须分清）：

- `max_lookback`：**可读深度**——策略最多能回溯多少根（内存与 `max_lookback()` 都由它决定；读 `[n]` 且 `n ≥ max_lookback` 抛异常）。对应 TBQuant `SetBackBarMaxCount` / MC `MaxBarsBack` 的“序列变量预分配”角色，但我们是流式回放、不预分配历史。
- `warmup`：**预热深度**——启动时从归档域回放多少根 bar 来收敛指标（`bars` 根数或 `from` 起始日期），不影响内存与 `max_lookback()`，只影响启动时长与指标初始状态。对应 TB 图表“历史起始日期”/ MC “Load additional bars”的语义。

**schema（最小可用）**：

```json
{
  "schema_version": 1,
  "period_sec": 60,
  "instruments": ["rb2601", "rb2605"],
  "accounts": [
    { "account_id": "CTP001", "scale": 1.0 },
    { "account_id": "CTP002", "scale": 0.5 }
  ],
  "max_scale": 1.0,
  "scale_rounding": "nearest",
  "scale_min_one": false,
  "params": { "fast": 10, "slow": 30, "volume": 1 }
}
```

**可选覆盖（示例）**：

```json
{
  "max_lookback": 2000,
  "reserve_bars": 1600,
  "warmup": { "bars": 50000 }
}
```

或按起始日期预热（与 `bars` 互斥）：

```json
{ "warmup": { "from": "2024-01-02" } }
```

**启动有效配置日志**（字段值 + 来源，便于确认框架替用户选了什么）：

```text
config effective | schema_version=1 period_sec=60 accounts=2 max_scale=1.0 scale_rounding=nearest scale_min_one=0 max_lookback=1440 max_lookback_source=auto reserve_bars=2880 reserve_bars_source=auto buffer_bars=4320 warmup_bars=10000 warmup_source=default mem_per_series=34560
accounts effective | CTP001=1.0 CTP002=0.5
```

`warmup` 用 `from` 模式时第一个 `warmup_bars=…` 换成 `warmup_from=YYYY-MM-DD`（来源同为 `explicit`）；0 个挂靠账户时账户行是 `accounts effective | -`。来源枚举为 `explicit` / `auto` / `default`，浮点按最短往返表示并给整数值补 `.0`（`1.0` / `0.5` / `0.333`）。

**UI_INPUT 宿主命令（v1，wire 细节并入 issue #9）**：

```
instrument add <id>
instrument del <id> [sync|nosync] [market]
instrument list
account add <id> <scale> [sync|nosync] [market]
account set <id> scale <scale> [sync|nosync] [market]
account del <id> [sync|nosync] [market]
account list
param get <name> | param set <name> <value> | param list
status | quit
```

- **字符串协议**：`UI_INPUT`/`OUTPUT_UI` 的 payload 都是 UTF-8 文本；命令是空白分隔的 token 行（命令名 + 参数），不引入 JSON/引号语法；参数只接受平台标识/数字/日期（不含空白）；`sync`/`nosync`/`market` 为小写关键字。解析失败、未知命令、参数个数/取值错误 → 单行错误字符串，内存不变；成功 → 结果字符串。
- 结果输出：正常结果与错误行写 `OUTPUT_UI`（字符串），关键错误同时 `dz_notify_ui`；dzweb 的入口、权限、请求关联与展示归 #6/#9，本 spec 只保证解析与执行可被测试帧驱动。
- 增删改账户命令默认 **nosync**；`instrument del` 同样支持 `[sync|nosync] [market]`（默认 nosync）；`sync` 按 §6.2 发同步/平仓单；`market` 只对同步单生效（默认对手价）。命令语义见 §6.2 与 ADR 0028/0030。
- 命令成功后 A 写回配置（原子）；失败则内存不变、返回错误行。`param set` 成功后通过 `on_params_update` 通知策略重读。
- 命令与配置中的账户标识只是平台账户 id 字符串；账户的登录/凭据/交易接口归属属于平台（td 配置），框架不管理。

**JSON 取用（依赖层由 issue #154 落地，消费方随框架票实现）**：框架的配置读写与 `ParamStore` **只经 `dztrader/json.h`** 取得 nlohmann，**不直接包含任何第三方路径**。两种模式：默认用 SDK 头文件目录内置的单头（策略作者不需要 Conan 或额外安装）；定义 `DZTRADER_JSON_EXTERNAL` 时用外部 `<nlohmann/json.hpp>`（策略作者自带版本）。内置单头版本与 `conanfile.py` 的 `nlohmann_json` 固定版本一致，升级**同一次提交**改两边，由 `scripts/check_json_vendor_version.py` 机器校验（版本号与 SHA-256 的唯一记录点是 `libs/strategy_api/include/dztrader/third_party/nlohmann/VENDORED.md`，本文件不重复写版本号）；双模与「聚合头不引入 JSON」由 `ctest -R "json_probe|aggregate_header"` 守住。**`DZTRADER_JSON_EXTERNAL` 是编译期开关，同一进程内所有 TU 必须取值一致**（混用会让 `nlohmann::json` 在不同 TU 成为不同类型 → ODR 违规）。

## 8. 动态管理

> **实现状态（#162）**：运行期命令面**已落地**。入口只有一个——`BarStrategyManager::on_ui_input`
> 接 `DZ_FRAME_UI_INPUT`，文法解析在 `dztrader/bar_strategy/command.h`
> （`command_internal::parse_command` / `CommandKind` / `Command`，纯函数、只依赖标准库），
> 命令实现在 `bar_strategy/manager.h`（`execute_command` 分派到 12 个 `run_*_command`）。
> 执行原语的增量在既有 `ctx` 出口内完成：`bar_strategy/execution.h` 增
> `resolve_sync_price()`（对手价 / 最新价回退 / 市价）与 `TickSnapshot` 的
> `last_price` / `bid_price` / `ask_price`，`bar_strategy/context.h` 增
> `submit_sync_order()` / `submit_sync_close_orders()` / `publish_zero_all_accounts()` /
> `publish_account_wildcard_zero()` / `logical_position_for()` / `logical_position_with_scale()`
> ——同步单仍走**唯一**的 `submit_order` 出口（限价路径照常 tick 对齐 + 涨跌停夹取），清零上报
> 仍走**唯一**的 `publish_position` 出口。**没有帧号、帧结构、导出符号变化**。
> 测试：`bar_strategy_command_test.cpp`（27 例纯文法单元）+ `bar_strategy_framework_test.cpp`
> 的 11 个 Seam A 用例（完整命令面含实例增删后的 ctx 地址稳定性、写盘失败不改内存、
> `instrument del` 顺序、账户命令的
> `sync`/`market` 语义、参数热更新即刻与挂起合并两条路径、`quit` 自退出、超长输出截断）。
> 已知限制：命令的 wire 入口、权限、请求关联归平台侧（#6/#9），本票只保证测试帧可驱动。

- **启动**（**实现状态 #161：以下序列已落地**）：读配置并校验（`period_sec`/`instruments`/`accounts`/`max_scale`/取整策略）→ 立即 `dz_subscribe` 全部品种（§5.3 步骤 1）→ 查合约元数据（每品种一次；`Missing`/`Invalid` 只跳过该品种）→ 对每个品种建 `InstrumentNode`（ctx 注入 `DzContext*`/账户表只读指针/`ParamStore*`，价格数组按 `buffer_bars` 一次分配、B 于 `on_init` 创建用户序列，此时 `bar_index == -1`、窗口为空）→ 逐品种加载历史（`dz_db_query_bar`）并静默填窗 + 回放（§5.3；回放期虚拟成交）→ 排空缓冲 tick 续接 live → 全部完成后一次性上报各账户逻辑持仓 → 进入 live。历史不可用（归档域未实现）时按冷启动：静默填窗从 live 开始，逻辑持仓为 0。
- **instrument add `<id>`**：查合约元数据（必需字段缺失则该命令失败、进程继续）→ 订阅该品种（`dz_subscribe`，追加）→ 建 `InstrumentNode`（`on_init`）→ 该品种自己的锚点 = 本次加载历史的最早一根；有历史则静默填窗 + 回放，无历史则冷启动静默填窗 → 补报该品种各账户逻辑持仓 → 进入 live。
- **instrument del `<id>`**：默认 `nosync`（只销毁实例并清逻辑持仓）；带 `sync` 时先按 §6.2 对各账户发该品种的差额单（`qty = −L_i(old)`；不确认成交、不判断逻辑持仓）→ 退订该品种 → 销毁实例（`on_destroy`）→ 调 `dz_set_logical_position` 逐账户清零上报 → 移除 `InstrumentNode`。
- **account add `<id> <scale>`**：校验（id 非空且未挂靠、`scale ∈ (0, max_scale]`）→ 写回配置 → 入账户表 → 上报该账户各品种逻辑持仓 → 可选 `sync` 发同步单（§6.2）。
- **account set `<id> scale <v>`**：校验并写回配置 → 更新 scale → 重报该账户各品种逻辑持仓 → 可选 `sync` 发同步单。
- **account del `<id>`**：写回配置 → 可选 `sync` 先按命令引起的逻辑变化量发平仓单（`qty = −L_i(old)`，逐品种）→ 立即移除账户表项并上报清零（`SET_LOGICAL_POSITION` 通配清零）；不判断逻辑持仓、不确认成交；不保留任何成交台账。账户不在表内 → 报错。
- **account list**：输出账户 / scale / 各品种逻辑持仓 `L_i`；不含实际持仓与差额。
- **写盘顺序**：先写盘成功，再改内存；写盘失败则命令失败、内存不变（`sync` 发单发生在写盘成功之后）。
- **删除/退出不操作订单**：v1 不撤单、不等待成交；删除账户/品种或进程退出前，操作员需在 td/UI 侧撤单并处理账户实际仓位（ADR 0028）.
- **实现期确认（#162）**：
  - **写回后可重新加载是硬门禁**：所有命令都经 `ConfigStore::commit`（内部 `resolve_effective_config`
    全量校验 + 不可变字段守卫 + 原子写盘）。因此 `instrument del` 删掉最后一个品种会被拒
    （`instruments` 非空）→ 命令失败、实例与订阅保持原样。`account set` 的 `scale` 上界是运行期
    的 `max_scale`（盘中不可变，超限只报错、不顺带放宽上限）。
  - **`instrument add` 的写盘位置在实例创建成功之后**：先建节点（含 `on_init`、回放）再写盘，
    写盘失败时回滚（`on_destroy` + 退订 + 丢弃节点）。反过来（先写盘、创建失败）会让配置里
    留下一个运行期并不存在的品种，下次启动才暴露。
  - **`instrument del` 的顺序不可交换**：先发 `sync` 平仓单（此时该品种的行情快照还在，
    对手价可取）→ 退订 → `on_destroy` → 逐账户清零上报 → 移除节点。先销毁会丢失定价依据，
    先清零会让 B 的 `on_destroy` 读到被清空的镜像。
  - **删账户用通配清零、不逐品种上报**：`account del` 成功后账户已不在表内，逐品种遍历它已不
    可能；且**0 个品种时仍必须清零**。出口是 `InstrumentContext::publish_account_wildcard_zero()`
    （`instrument_id == ""` 的契约语义），上报帧的 `instrument_id` 因此为空串。
  - **`account set` 的幂等性**：`scale` 未变时不发单、不上报、返回 `changed=0`（操作员重放同一
    命令是常态，报错会让脚本化运维很难写）。
  - **命令结果的输出通道**：成功结果与错误行都写 `dz_output_ui`（`DZ_FRAME_OUTPUT_UI`）；
    `quit` 与关键错误（如 `instrument add` 元数据缺失）**额外** `dz_notify_ui`（WARN/ERROR，
    不弹窗）。语法错误不弹通知——那是操作员当场可改的。超长结果由 SDK 按单页上限截断
    （`api.h` 的 `dz_output_ui` 口径），命令层不自行分片。
  - **`status` 的输出字段**：一行 `status | instrument_count=… account_count=… period_sec=…
    max_lookback=… buffer_bars=… net_position=… failed_instances=… param_count=… live=…`。
    比 AC 的最低要求（「`status` 存在」）多几项，理由是运维排障时最常问的就是这几个数；
    `net_position` 是各品种逻辑净持仓之和（策略级 P 不单独上报，这里只是观测汇总），
    `live=0` 表示进程还在预热/回放（正常启动完成后恒为 1）。
  - **同步单没发全时命令结果多一行**：命令本身仍算成功（写盘与逻辑台账都已按命令推进），
    但若同步单因无行情（`skipped_no_market_data`）或手数越界（`rejected`）没发出去，结果文本
    追加一行 `sync orders incomplete | submitted=… skipped_no_market_data=… rejected=…`。
    不把它并成错误行：命令确实成功了，改写成失败会让脚本化运维误判并重试（重试还会撞
    「已存在 / 未挂靠」再次失败）。组件文档 §6.2 的「跳过并输出错误行」指的就是这行。
  - **`quit` 只置标志**：`should_exit()` 返回 true、`exit_code()` 为 0，由引擎走自退出路径
    （`on_stop` → 逐品种 `on_destroy`）。命令实现里**不**销毁实例，否则 `on_destroy` 会被调两次。

## 9. 参数

> **实现状态（#162）**：`ParamStore` 与 `ctx.param<T>` **已落地**，定义在
> `dztrader/bar_strategy/params.h`（**不是** `context.h`：`params.h` 才引入内置 JSON，
> `context.h` 只前向声明 `ParamStore`、持 `const ParamStore*` 并声明 `param<T>`，因此不读参数
> 的策略 TU 不承担 nlohmann 的编译成本——与 `execution.h` / `scaling.h` 的同一条隔离口径）。
> 命令侧在 `manager.h`（`run_param_get` / `run_param_set` / `run_param_list`），测试在
> `bar_strategy_framework_test.cpp` 的三个用例与 `bar_strategy_command_test.cpp` 的文法用例。
> 存值形态：**配置里的 `params`、`ParamStore` 的内存形态、`param set` 写入的形态三者相同**
> （都是 JSON），因此「配置里的参数」与「运行期改过的参数」没有两套表示，写回不需要转换。

**类型**：`class ParamStore`（A 持有唯一实例）——`nlohmann::json` 对象（有序 map）+ typed `get<T>(name, default)` / `set(name, json)`；`ctx.param<T>` 委托给它。**JSON 取用唯一入口是 `dztrader/json.h`**（issue #154）：默认用 SDK 内置单头，定义 `DZTRADER_JSON_EXTERNAL` 时改用外部 `<nlohmann/json.hpp>`；框架代码不得直接包含任何第三方路径（ADR 0026）。

- `ctx.param<double>("fast", 10)` / `param<int64_t>` / `param<bool>` / `param<std::string>`；A 只做运输。
- v1 参数全局统一（`params`，每个品种相同）；品种级参数覆盖后置（硬编码 / 将来扩展 `params` 段）。
- 存储：进程级一份（`BarStrategyManager::params_`），`ctx` 持 `const ParamStore*` 读取。策略手数就是普通参数（如 `volume`）；挂靠比率（`scale`）不是策略参数，属于账户配置（§6.2/§7）。
- 动态修改：UI 命令 → 写盘成功 → A 更新 `params_` 并对受影响实例调用 `on_params_update(ctx)`；未进入回调阶段的实例挂起、首根回调后合并投递；B 重新读取并更新成员，自行决定是否重置派生状态。
- v1 无参数元数据（min/max/step）、无寻优；不读 WebUI。
- **读取口径（实现期确认 #162）**：
  - 名字不存在 → 返回调用方给的默认值（策略不必为可选参数写存在性判断）；
  - 名字存在但**类型不符** → 抛 `std::invalid_argument`（框架按回调异常处理：该实例失败并打
    诊断）。静默退回默认值会让 `fast_run` 这类拼写错误变成「用默认值跑了一整天」，与仓库
    「不静默修正」的配置纪律相悖；
  - 类型映射**不做隐式放宽**：`int64_t` 只接受整数（`10.0` 不算，避免静默截断 `10.5 → 10`）、
    `double` 接受任意数值、`bool` 只接受布尔、`string` 只接受字符串。
- **`param set` 的值 token 类型推断**：命令没有引号语法，值只能是单个 token。规则是「能被 JSON
  解析成**标量**（整数/小数/布尔/字符串）就按该类型存，否则按字符串存」；`null` / 数组 / 对象
  不作为参数值接受（框架只运输标量），同样按字符串存。`param get` 打印时字符串原样输出（不加
  引号：命令层没有引号语法，加引号会让人以为要再输入一次），其余用 JSON dump——因此
  `param set x 10` → `param get x` 打印 `10` → 再 `param set x 10` 是同一个值（往返一致）。
- **通知的投递时机（实现期确认 #162）**：判据是实例「是否**交付过**至少一次 B 回调」
  （`note_callback_delivered()`，在三个 bar 分发入口交付回调之后设置；**不在**
  `if constexpr (HasOnBarXxx)` 里面——它标记的是实例阶段，与 B 实现了哪个回调无关），不是
  `bar_index >= max_lookback`——后者只看窗口几何，把「参数通知该不该投递」与「回调门禁」两件
  事绑在一起，将来门禁口径一变就会改错一处。挂起标记在**每次**回调之后检查一次（挂起可能发生
  在任何时刻，不只在静默填窗期）。合并语义：连续 N 次 `param set` 在挂起位置只留一个标记，
  首根回调后**只投一次** `on_params_update`——B 的职责是「重读参数」，读一次就拿到全部最新值；
  投 N 次会让 B 的派生状态被重置 N 次。投递顺序在「本轮 bar 事件之后、下一轮之前」，避免同一根
  bar 的部分计算用旧参数、部分用新参数。

## 10. 性能预算与基准

目标（Release、单品种、单挂靠账户、基准显式配置 `max_lookback=500`、双均线）分三段验收：

| 范围 | 预算 | 估算 |
|---|---|---|
| A 框架计算路径（tick 聚合 + 回调分发 + 基准 B 代码） | p99 ≤ 1us | 约 100–200ns；不含 SDK 帧解析、shm 写、B 自身复杂逻辑 |
| bar close 框架路径（回调 + 指标 + 下单意图 fanout/上报，不含 shm 写） | p99 ≤ 1us | 约 300–600ns |
| 交易意图 fanout（每账户 `L` 计算 + 1 订单帧 + 1 上报帧） | 先测后定 | 含 shm 写与 `notify_subscribers`，随基准报告给出 |
| 决策 → 订单帧落 shm（端到端） | 纳入项目 30µs 核心路径目标 | 不含网络/柜台往返 |

基准先以报告形式输出，稳定后升为 CI 硬门禁。基准项：`bench_tick_update`、`bench_bar_close_dual_ma`、`bench_series_read`（`[n]` 与 `span()`）、`bench_relocate`（18:00 回卷成本）、`bench_warmup_replay`（bar/s，当前可用测试 fake 源先测路径）；BarBuilder 实现后补 `bench_bar_tick_intraday` / `bench_bar_tick_daily`。

**tick 热路径没有账户级 fanout**：每 tick 只更新一次价格 + 调用一次回调；fanout 只发生在交易意图与命令的冷路径（每个挂靠账户算一次 `L`、发一单、报一条逻辑持仓，N 通常 1–3）。回卷不在热路径（每天 18:00 定时器内一次，成本进基准报告）。

- **交易意图只有一个出口**：`ctx.buy/...` 在 ctx 内完成「逻辑持仓差量 → 每账户手数 → 对齐/夹取 → SDK 调用」；未来 simulated executor 在同一出口内分支（交易意图冷路径），tick/bar 热路径不变。回报 v1 全部忽略。

- 回卷成本（每进程）：1 分钟典型 60–130µs/天，15 分钟 5–10µs/天；记录事件循环停顿上限（< 0.2ms）。
- 定时器错过时兜底回卷会把一次搬移拉进盘中推进路径，成本相同，属可接受的降级。
- 预热回放：K=10,000、50 品种约 0.5–1s/进程；10 年 1 分钟约 85–175s/进程（WARN）；回放期虚拟成交，成本与挂靠账户数无关。

**容量配比与内存**：见 §4.5；启动日志打印 `buffer_bars` 与 `mem_per_series`，按实际序列数可算总内存；挂靠账户数不影响内存。

**备选实现（接口不变，只换 `advance_bar` + `operator[]` 偏移）**：

- **A'（v1 采用）**：连续滑动窗口 + 每天盘后主动回卷。读路径零取模、`span()` 连续、回卷落在静默窗；物理内存 `buffer_bars = max_lookback + reserve_bars`（约 2–3 × `max_lookback`，见 §4.5）。
- **B 镜像环形（升级路径）**：不再整窗搬移，每根 bar 收盘时把该根写到下一圈的影子位，读偏移、`span()` 连续性、内存（2×）与读成本都与 A' 相同，延迟平坦无尖峰；代价是多一条“影子位只在收盘后有效”的不变式（可用参考实现 fuzz 测试锁死）。触发条件：上线 p99.9 / max 门禁、做秒级以下周期、或单实例多周期导致回卷量变不可忽略时。**没有可直接用的成熟第三方库**：Boost.CircularBuffer 不连续（只有 `array_one/array_two`）、nonstd::ring_span 的 `linearize()` 就是搬移（等于 A 的做法）、ETL/Simon Cooke `bip_buffer` 是字节流语义，都不提供「连续 + 反向 `[n]` + 定长窗口」；且 ADR 0020 禁止引入第三方容器依赖。自实现约 30–40 行，风险在不变式测试而不在代码量。
- **C 取模环形**：内存减半（`1×`）、零拷贝、延迟平坦，但失去 `span()` 连续性、指标循环无法向量化（大窗口慢约 4×），读 `[n]` 每次多一次掩码。**否决**——它省下的内存无关紧要，丢掉的连续性是指标库的卖点。
- **D 无界追加（vector 式）**：接口最像 TB 序列，但地址会失效（视图必须每次重取）、内存随运行时长无限增长、用户序列语义无法定义窗口。**否决**。

**对齐说明**：物理数组用普通堆分配（自然对齐），**不做** 2 的幂取整、cache-line padding 或 4KB 同余规避逻辑。定量理由：热路径是 8B 自然对齐的标量读写（永不跨 cache line）；`span()` 扫描用非对齐加载，现代 x86 一行内与对齐加载同速；回卷一天一次，首尾 partial line 的损失 < 1%；而“所有序列地址 mod 4096 同余”这一真实 L1 set 冲突风险只在自建 arena/矩阵布局且行步长恰为 4096 整倍数时才会系统出现，v1 的逐序列独立分配不触发（将来若改 arena，见 §14 约束）。

## 11. 测试与验收

> **已交付的测试（#157–#161）**：`libs/strategy_api/tests/bar_strategy_framework_test.cpp`（Seam A：临时
> `DZTRADER_HOME` + 真实事件/行情通道，测试线程内跑 `run_bar_strategy<FakeStrategy>()`，独立
> writer 注入 TICK/SHUTDOWN 帧）覆盖：回调序列 `close → open → update`、每笔 tick 恰好一次
> update、`bar_index` 0 起单调且 bar 内不变、`on_bar_open` 时 OHLC 等于本笔 tick 价、`on_init`
> 时 `bar_index == -1` 且窗口为空、B 保存的视图副本跨 bar 推进仍有效（节点地址稳定性的可观测
> 证据）、`on_destroy` 每实例一次、异常隔离**五条**路径（`on_init` / `on_bar_open` /
> `on_bar_update` / `on_bar_close` / `on_destroy` 抛异常，均只影响本实例且进程退出码 0）、
> 退化小节表合法、覆盖全
> 交易日内秒并被 `is_degraded_session_table()` 识别、缺失/非法配置的非 0 退出。
>
> **#159 新增**：`bar_strategy_position_test.cpp`（纯单元，21 用例）覆盖四函数开平意图全表
> （含反手目标化、只减不增与钳制、`volume = 0` 的两组等价对逐格比较）、即时记账（同一回调内
> 立刻可见 + `on_init` 期意图的 `bars_since_* == -1`）、Tier 1 每条口径（方向派生与计数非负、
> 加权均价/部分平仓不变/反手重置、建仓段数与历史峰值、进出场时间取 bar 标准结束时间、
> `bars_since_*` 同 bar=0 与无参考 -1、盈亏乘乘数且无仓 0、极值更新/开仓重置/平仓归零）、
> 拒单（负手数与非法价格逐字段不变、失败实例拒单）、诊断分级（四档 prose + 零值静默（含失败
> 实例上）+ 受理路径无声）、账户身份编译期不可见。`bar_strategy_framework_test.cpp` 新增两条 Seam A：
> `IntentLedgerIsWiredIntoRealCallbacks`（意图 → 台账 → Tier 1 在真实回调里的接线，含进场
> 时间断言；极值更新时机由单元测试覆盖，Seam A 路径乘数为 0 观察不到数值）与
> `FailedInstanceStopsAcceptingIntents`（失败实例保留逻辑持仓且不再收回调）。
>
> **#158 新增（同一文件）**：回调门禁（静默填窗 `max_lookback` 根零回调）、首个回调序列
> `close(静默填窗最后一根) → open → update`、回调期恒有 `size == max_lookback` 且 `[n]`
> 全部可读、首根 bar 从未被报告为收盘（`FirstBarIsNeverReportedAsClosed`）、最后一根 bar 不主动收（无 flush）、用户序列静默期写声明初值
> （两条：`double` + `int64_t`）、`make_series` 在 `on_bar_update` 调用响亮失败并经异常边界
> 标记实例失败、回卷两种路径（18:00 定时器 / 推进前兜底）对 B 完全等价（同一 tick 脚本两轮
> 逐字段比较回调序列、`bar_index`、窗口长度、价格历史与用户序列历史）。
> `series_window_test.cpp` 新增逐步回卷（`relocate_array` + `finish_relocate`）与一次性回卷
> （`relocate_window`）的等价性、`finish_relocate` 幂等性、以及**回卷前后窗口逐位不变**的
> 直接不变式（AC6 的正面断言，跨路径比较无法替代）。
>
> **#160 新增**：`bar_strategy_execution_test.cpp`（纯单元，17 用例）覆盖 `L_i` 的三种取整
> × 正负持仓全表、`scale_min_one`（只抬到 1、空仓恒 0）、非法 `scale` 防御（NaN/0/负 →
> 0 而不是 UB）、**取整误差不随逐单累积**（`L` 是当前 `P` 的纯函数 + 逐格数值）、反手一步
> 翻转、幅值饱和（含 `INT64_MIN` 不能直接取负）、tick 对齐不偷价（含 `0.3/0.1` 的浮点边界
> 与「已在网格上不报调整」）、`price_tick` 缺失时跳过对齐、涨跌停夹取（只夹方向对应一侧、
> 快照无效不夹、恰好等界不夹）、对齐与夹取的顺序、非法价格拒单、差量为 0/超范围的手数换算。
> `bar_strategy_framework_test.cpp` 新增八条 Seam A（独立 probe reader 断言订单帧与上报帧）：
> `AttachedAccountsMirrorAndReverseWithSingleDeltaOrder`（scale 1.0/0.5 的镜像手数、反手
> 每账户**一张**差量单、effect=AUTO、上报净手数）、`AccountsWithUnchangedLogicalPositionAreSkipped`
> （`L_i` 未变 → 零帧，同时是「取整在目标持仓上」的可观测后果）、
> `ZeroAttachedAccountsProduceNoFrames`、`ConfiguredRoundingAndMinOneReachExecution` 与
> `ScaleMinOneIsWiredThroughConfig`（配置层到执行路径的接线：默认值断不出接线错误，故用
> `floor` + `scale_min_one` 与足以区分的 scale）、`OnInitIntentIsVirtualOnlyUntilLive`
> （live 门禁 + 进入 live 时的全量批量上报）、`OrderPriceIsClampedToLimitAndDiagnosed`
> （挂涨停价而非拒单 + `reason=limit_up` 诊断）、`BatchReportCarriesTheInstrumentId`
> （逐品种上报绝不带空合约代码——空串是通配清零的形状）。
>
> **#161 新增**：`bar_strategy_replay_test.cpp`（纯单元，10 用例）覆盖合成 tick 回放保真
> （涨/跌/平三种价格顺序、量额 `k/4` 累计、跨交易日累计基准重置、回放产出 bar 与归档 bar
> **逐字段一致**含 `last_tick_sec`、Open 竞价 bar 的负 `last_tick_sec` 与 `is_auction` 相位）与
> **分桶判定的唯一实现**：把 ADR 0031/0032 的口径**手写成期望桶**，同时对齐
> `bar::resolve_tick_bucket` 的结论与 builder 的接受行为（退化表 / 多小节（含小节末端 tick
> 归前一根）/ Open 竞价窗口 / 窗口优先于小节判定）——不能拿 resolver 的返回值当 builder 的
> 期望（builder 内部就调 resolver，两边同错会恒真通过）。`bar_strategy_dependencies_test.cpp`（8 用例，真实 td 库 + 临时目录）覆盖
> 合约元数据默认实现的四种结局（Loaded / Missing / volume_multiple 非法 / price_tick 非法 /
> 源不可用）、归档域 stub 按「无历史」降级（`Unavailable`；错误文案归 SDK 契约，不逐字断言）、
> `FrameworkDependencies` 默认口径（退化表 + 18:00 + 空 loader）。**已知覆盖边界**：
> `query_archive_history` 的行解码分支在归档域 #4 落地前不可达（`dz_db_query_bar` 恒 NULL），
> 端到端解码测试随 #4 补。`bar_strategy_framework_test.cpp` 扩 **Seam B**
> （注入假历史/假元数据 + 「回放即将开始」同步点以确定性复现缓冲 tick）：回放事件序与 live
> 同构且窗口即归档历史（`WarmupReplayDrivesLiveIsomorphicCallbacks`）、回放期零订单帧且启动
> 恰一条恢复后持仓上报（`WarmupReplayKeepsIntentsVirtualAndPublishesRecoveredPositionOnce`）、
> final 尾部丢同桶 tick（`BufferedTicksAreDroppedAgainstFinalTail`）、forming 尾部同桶续接
> 不重复 close/open（`BufferedTicksContinueFormingTailWithoutDuplicateOpenClose`）、冷启动
> 降级（`ColdStartDegradesToLiveWithoutHistory`）、`warmup.bars` 尾部裁剪与 `warmup.from`
> 起始日透传（`WarmupBarsModeCropsToTailOfHistory` / `WarmupFromModeForwardsStartDateToSource`）、
> 锚点按品种独立 + `begin_sec` 跳跃只记 INFO（`WarmupAnchorsAreIndependentPerInstrument`）、
> 历史不足由 live 补足（`InsufficientHistoryFillsRemainderFromLive`）、元数据端到端接线
> （Tier 1 乘数 + 委托价 tick 对齐，`InjectedMetadataRecoversTier1AndPriceAlignment`）、元数据
> 缺失只跳过该实例（`MissingMetadataSkipsOnlyThatInstrument`）、失败实例仍上报台账
> （`FailedInstanceStillPublishesItsLedgerAtStartup`）、成本估算两级 WARN
> （`WarmupPlanWarnsAtSixtySeconds` / `WarmupPlanWarnsAtThreeHundredSeconds`，阈值取生产常量；
> 估算行与实际回放行按行分别断言，防止「拿估算当实测」恒真）。
>
> **#163 新增**：`bar_strategy_framework_test.cpp` 扩 **Seam B/A**（含 Open 竞价窗口的注入
> 小节表 `day_auction_session_table()`，复用 `bar_test_data.h`）：竞价窗口内事件序与相位写入
> （`AuctionWindowDrivesEventOrderAndPhaseWrites`：窗口首笔 tick 的 `close → open → update`
> 三者均为 true、后续竞价 tick 只 update、离开窗口第一笔连续 tick 只 update 且相位转 false；
> 同用例用脚本意图断言窗口内下单立即落地、与连续交易单形状相同——框架不因相位拦截下单）、
> 回放期相位恒 false（`AuctionPhaseIsSuppressedDuringReplay`：用例内先用 builder 做正对照
> ——竞价归档 bar 的合成 tick 标记 true、窗口内启动首笔只 `open → update`——再断言经 A 后
> B 看到 false）、非 bar 回调恒 false
> （`DeferredParamsUpdateSeesPhaseFalseAfterFirstAuctionCallback`：竞价首笔 close 之后补投的
> `on_params_update` 为 false）、退化表下恒 false
> （`DegradedSessionTableKeepsAuctionPhaseFalse`：tick 落在生产口径的竞价窗口内仍为 false）。
> 下列清单是随各票逐步补齐的完整目标。

- 单元：
  - Series：`[n]` 读值/负索引/`n ≥ size` 抛 `std::out_of_range`、消息含 `n/size/max_lookback`；`span()` 长度与顺序；`size/max_lookback` 语义；write-only-`[0]` 的编译期约束（无 `push_back/resize`）；Debug assert。
  - 回卷等价性：随机序列操作前后快照全部 `[n]` 与 `span()` 逐位一致；回卷可发生在静默填窗、回放、live 任意位置；强制 `reserve_bars` 极小时的兜底路径。
  - 容量：各周期自动值、显式覆盖、越界报错、`buffer_bars ≤ 24576`、`buffer_bars == max_lookback + reserve_bars`。
  - 指标函数；合成 tick（涨/跌/平、量分配）；bar 桶计算（`(trading_day, 桶)` 边界）。
  - 竞价相位：`ctx.is_auction()` 在 close/open/update 的写入口径（= 触发 tick 相位，§5.1）；用含 Open 竞价窗口的测试小节表验证窗口内事件序；退化表下恒 false（§5.5）。
  - 配置解析/默认生成/原子写回（含 tmp 占位、rename 目标为目录、父路径不可建目录等失败分支；`warmup.from` 边界含 today 注入）/`warmup` 互斥与范围校验/有效日志；`schema_version` 必填且只接受 1；`accounts`/`max_scale`/取整策略校验（`scale ∈ (0, max_scale]`、账户 id 不重复）；`instruments` 必填、非空、不重复，`accounts` 可空；顶层未知字段忽略、`accounts` 元素与 `warmup` 对象未知键报错；配置路径按 exe stem 推导（带/不带尾分隔符一致）；`ConfigStore` 写盘失败/非法配置/不可变字段（`period_sec`、`max_scale`）时内存与文件都不变、写回保留未知顶层字段与可选字段省略态。
  - 虚拟台账：四函数开平意图全表（`buy`/`sell_short` 反手目标化为 `P=±n`、`sell`/`buy_to_cover` 只减不增与 `n>|P|` 钳制、`volume=0` 的只平不建/全平与两组等价对）；下单意图即时更新 `P`/Tier 1（含跨越 0 的开平归属）；`L_i = f(scale×|P|)` 的三种取整与 `scale_min_one`；每账户手数 = `L_i` 差量；价格对齐与涨跌停夹取；失败实例拒单。
  - 交易意图诊断：`n>0` 方向无效/超仓钳制记 WARN；`volume=0` 且无可平仓不诊断；`volume=0` 实际触发平仓记 INFO；负手数拒单。
  - 交易意图 fanout：N 账户时每单的方向/手数/`effect=AUTO`/价格正确（反手/全平仍是一张差量单）；0 账户时不下单；`L_i` 未变化时跳过发单与上报。
  - 启动衔接：订阅先于查询；最后一根 final → 丢 `begin_sec <= B`；forming → 丢 `begin_sec < B` 且同 `begin_sec` 续接不产生重复 close/open；`begin_sec` 跳跃只记 INFO；多品种按序回放。
  - Tier 1 口径：`current_contracts` 非负、盈亏乘 `volume_multiple`、`current_entries` 段数与反手计数、极值在 bar 收盘前更新且平仓归零、`bars_since_*` 同 bar=0/无参考 -1。
  - 命令字符串：文法解析、未知命令、参数个数/取值错误、`sync|nosync|market` 关键字、错误字符串不改内存；`instrument del` 的 sync/nosync 与 `qty = −L_i`。
  - 删除安全：`instrument del` 不判断逻辑持仓、先平后删（`qty = −L_i`）/直接删两条路径；`account del` 立即移除并清零上报；进程退出不操作订单。
  - 上报：每次逻辑持仓变化上报受影响账户的最新 `L_i`；回放期不逐笔上报、结束后一次性上报；删除账户通配清零。
  - 回报忽略：`on_trade_report` / `on_order_report` / `on_position_info` / `on_trading_account` 不影响逻辑持仓与任何 B 可见状态；框架不建成交台账。
  - 账户命令：add/set/del 的校验与写盘顺序；`sync|nosync` 语义（先按命令引起的 `L_i` 变化量发单，随后无论成交与否更新逻辑持仓；`account add` 差额 = `new_l`（假设实际为 0）；删除时 `qty = −L_i`；默认对手价 / `market`；无 live 行情品种跳过并报错）；`instrument del` 的 `[sync|nosync] [market]`；写配置失败不发单且逻辑不变、下单失败仍更新逻辑；删除立即移除并清零上报。
  - 结构/地址稳定性：`InstrumentContext` 不可拷贝/移动的编译期断言；节点就地构造；`instrument add/del` 后其它品种的视图与 `Series` 仍有效。
- 集成（fake B + fake 历史源）：
  - 回调门禁：冷启动前 `max_lookback` 根零回调、首根回调顺序正确（close→open→update）。
  - 历史回放：静默填窗后 K−C 根按 4 合成 tick/根回放，事件序列与实盘同构；回放期下单只做虚拟成交（不落地、不逐笔上报）、回放后逻辑持仓与一次性上报正确；进入 live 后对实盘发单。
  - 锚点回放：进程启动与 `instrument add` 都在本品种内按序填充/回放，无跨实例依赖；历史不可用 → 降级冷启动 WARN；bar 数据源缺口/拼接语义归归档域（#4）。
  - 异常边界：B 在各回调抛异常 → 实例失败、其它品种继续、进程存活、日志一次；失败实例不再收回调/下单。
  - 回卷定时器触发 + 兜底触发。
- E2E：master 拉起示例策略，真实 shm 喂 tick，验证回调/下单/退出。
- 验收：示例「双均线 + 固定止损」单逻辑账户 + 2 个挂靠账户（scale 1.0 / 0.5）跑通；account add/set/del（sync/nosync）与一键同步前置语义正确；迁移对照表齐备；1us 基准报告达标（回卷/预热单独报告）。

## 12. 迁移对照

| TB/MC | 我们 |
|---|---|
| `OnInit` | `on_init(ctx)`（此时 `bar_index == -1`，不得读序列） |
| `OnBarOpen` | `on_bar_open(ctx)` |
| `OnBar`（实盘每 tick / 历史每 bar） | `on_bar_update(ctx)`（实盘每笔 tick 一次；历史为合成 tick 每根 4 次） |
| `OnBarClose` | `on_bar_close(ctx)` |
| `Symbol`（TBQuant 多数据为 `Data0.Symbol`） | `ctx.instrument_id()`（平台唯一键；注意与 `symbol`（场所原生码）、`name` 区分） |
| `BigPointValue` / `MinMove` 等合约属性函数 | v1 不暴露；`InstrumentSpec` 已收 `volume_multiple` / `price_tick` / `exchange_id`，后续按需加只读查询 |
| `NumericSeries X(0)` / `Variable: X(0)` | `Series<double> X = ctx.make_series<double>(0.0)`；`X[0] = v` 赋值 |
| `X[1]` 回溯 | `X[1]` 同形 |
| 写序列历史位（classic TB 手册明确禁止；TBQuant 官方论坛称禁止；MultiCharts 未核实） | 我们统一禁止（只写 `[0]`）；任意位置写用用户自有容器 |
| `Buy(Size, Price)` / `Buy Next Bar at Market` | `ctx.buy(volume, price)`（限价）；四函数按 TB/MC 开平意图推进 `P`（§6），`volume` 是**策略手数**，每个挂靠账户按 `scale` 换算（AUTO，开平由 td 解析） |
| 反手（平多后立刻开空） | 一次 `ctx.sell_short` 意图即可：`P` 目标化为 `-n`（先平全部多、再开 n 空）；实盘是一张 AUTO 单由 td 拆平/开，不需要等成交回报、不需要拆两次下单 |
| `Sell(0, Price)` / `BuyToCover(0, Price)`（TB：0 = 平该方向全部） | `ctx.sell(0, price)` / `ctx.buy_to_cover(0, price)`：全平对应方向，与 TB 一致 |
| MC `Buy 0` / `SellShort 0`（0 = 只平反向、不建仓） | `ctx.buy(0)` / `ctx.sell_short(0)`：语义相同（买方向平空、卖方向平多） |
| TB `Buy(0, Price)` / `SellShort(0, Price)`（TB：0 = 系统默认手数） | **不支持**：本框架没有系统默认手数；`ctx.buy(0)`/`ctx.sell_short(0)` 是“只平不建”，迁移必须改成显式 `volume` |
| `volume=0` 统一规则 | 买方向平空、卖方向平多；`buy(0) ≡ buy_to_cover(0)`、`sell(0) ≡ sell_short(0)`；0 触发平仓记 INFO（§6） |
| `MarketPosition` / `CurrentContracts` | `ctx.market_position()` / `ctx.current_contracts()`（方向 / 持仓手数 `≥0`；内部净持仓有符号） |
| `MaxContracts` / `MaxContractsHeld` | `ctx.max_contracts()` 只保留「本实例历史峰值」一个口径，不区分当前持仓期/历史（差异见 §6.1） |
| `Time` / `Date` | 后置：Bar 字段已由 [ADR 0031](../adr/0031-tick-bar-synthesis-component.md) 定义（`trading_day`/`begin_sec`/`last_tick_sec`），B 的 `Time`/`Date` 映射随策略 API 定稿 |
| `Q_BidPrice` / `Q_AskPrice` 等报价查询 | 后置：随报价/策略 API spec 定稿（§14） |
| 开平/锁仓由平台自动解析 | 委托 effect 统一 AUTO；td 侧未实现前实盘单会被拒（#82，已知限制） |
| `MaxBarsBack` / `SetBackBarMaxCount` | `max_lookback`（可读深度，实例创建时定死、不扩容；默认按周期，可显式配置） |
| 图表历史起始日 / MC “Load additional bars: From date” | `warmup.from`（起始日期；同时是回放续仓的恢复范围） |
| MC “Load additional bars: N bars” / TB 回放深度 | `warmup.bars`（默认 10,000，与 `max_lookback` 独立） |
| `CurrentBar` | `ctx.bar_index()`（0 起、单调，classic TB 同口径；MC 从 1 起需 −1）。**首个回调的 `bar_index == max_lookback`（此前 `max_lookback` 根静默填窗）**；TB 从 bar 0 起用钳位值回调，我们不这么做（见下一行） |
| `BarCount` | `ctx.bar_index() + 1`；判断“历史够不够”用 `ctx.close.size()`（回调期恒等于 `max_lookback`） |
| 回溯不足（classic TB：`CurrentBar=0` 时 `Close[1]` 取当前 Close 代替） | **抛 `std::out_of_range`**（响亮失败，调大 `max_lookback`）；不钳位、不产生 NaN、不返回初值。用户序列历史位在静默填窗期为 `init` |
| 预热期下单 | 只做虚拟成交（重建逻辑持仓），不落地（TB 图表虚拟账户口径） |
| `BarStatus` | 无（三回调替代） |
| TB 的集合竞价判断（旧版实盘为 `bar.isCallAuction`） | `ctx.is_auction()`：Open 竞价窗口内触发的回调为 true（[ADR 0032](../adr/0032-auction-window-strategy-events.md)）；框架**不拦截**窗口内下单，策略自行过滤（旧版口径）。窗口内事件序见 §5.1；注意它是**触发回调的 tick 相位**——窗口首笔 tick 触发的 `on_bar_close(上一根)` 也为 true（被关的是普通 bar），不要用它跳过 close；v1 无小节数据源时恒为 false（§5.5） |
| `Data0..49` / `DataN` | 后置 |
| 历史：每 bar 一次最终 OHLC | 静默填窗最长 `max_lookback` 根（无回调），其余按合成 tick 回放（`warmup` 深度） |
| 涨跌停拒单 | 挂涨跌停价 |
| 图表绑定一个账户 / 头寸管理器系统倍数 | 一个策略进程 = 一个逻辑账户 + 0..N 个挂靠账户（`scale`）；策略不可见账户；实盘/逻辑差异靠账户级一键同步（平台/WebUI，未开发） |
| 账户资金/权益参与策略逻辑 | 不提供（B 只读逻辑持仓） |

## 13. 功能分期与演进接缝

判据：改这个决定是否要动「数据模型 / 事件模型 / 所有权与生命周期 / 执行边界 / 身份与配置 / 性能路径」。要动 → 架构级，v1 必须定对；只加只读查询、纯函数或配置项 → 功能级，可增量。

| 类别 | 内容 | 演进成本 |
|---|---|---|
| v1 已锁定的架构级 | 单逻辑账户 + N 挂靠账户（ADR 0028）；挂靠缩放/同步/虚拟台账/回放虚拟成交（ADR 0030）；A/B 分工与 ctx 自包含执行（无 A 反向指针）；实例 = 品种；数据组织（ctx 合并）与地址稳定性；Series 语义与抛异常读；容量/回卷模型（A'）；回调门禁；锚点预热；执行边界（B 只表达意图，执行在 ctx 内，effect 恒 AUTO）；配置/身份；热路径零间接 | 改动 = 返工，v1 必须锁定 |
| v1 实现、后续扩展 | executor：v1 有 live + replay（#161 已落地，回放期虚拟成交），simulated（回测撮合）后置；history_source：三个依赖缝与默认实现已落地（#161），默认历史源仍指向 `dz_db_query_bar` stub，归档域实现后生效（同时启用回放续仓）；td AUTO 开平解析（issue #82）为实盘联调前置，不阻塞框架开发与合并 | 纯加法 |
| 功能级后置（加法） | 归档域数据实现；Tier 2 交易统计；指标库扩充；UI 命令面；B 定时器回调（`on_schedule` 转发）；Python 绑定；参数元数据与寻优；策略组编排（跨进程统一启停/参数/UI 分组）；账户级头寸监控与一键同步（平台/WebUI） | 低 |
| 架构级后置（明确不做，升级需新 ADR） | 跨品种/跨周期数据（Data0..N）；单实例多周期（镜像环形触发条件）；long/short 分向持仓模型；撤单/动态委托/算法下单；`Portfolio_*` 组合资金曲线；回测撮合模型；按资金/权益分配手数；锁仓模式多空明细建模 | 高；迁移用户需提前知晓 |

原则：

1. 只为「v1 已有两个具体实现」的地方建接口；v1 的 executor 已有 live + replay 两个模式，simulated（回测撮合）等到回测里程碑再加。
2. 功能以垂直切片交付：接口先冻结（纸面移植验证），实现按里程碑增量，每步跑单测 + 基准。
3. 架构级后置项在开发者指南与迁移表里显式列为「不支持/后置」，避免用户误以为已覆盖。

## 14. 开放问题与后置项

1. 账户级头寸监控与一键同步（平台/WebUI）：比较账户实际净头寸与各策略上报的逻辑持仓、发单调向系统口径；需要「手工仓」与跨策略归属设计（[ADR 0028](../adr/0028-strategy-process-logical-account.md)），v1 不做。
2. 策略组编排：跨进程统一启停、参数同步、UI 分组与汇总；v1 靠部署模板/脚本。
3. 归档域历史数据（#4）：结果集 schema/时间标注对齐 `(trading_day, begin_sec)` 与 ADR 0031 的 Bar 字段、`dz_db_query_bar` 的生产实现；**bar 数据源须提供 `is_final`**（[ADR 0031](../adr/0031-tick-bar-synthesis-component.md)），并支持大 `warmup.bars`（最高 1,000 万根）的分页/LIMIT（随归档 spec）。#161 已给出**暂定解码契约**（`bar_strategy/dependencies.h` 的 `query_archive_history`：结果集列名 = `bar::Bar` 字段名，`last_tick_sec` / `period_sec` / `is_final` 缺列时有默认），归档域 spec 定稿时须与本解码同步；stub 现状下该解码分支不可达。
4. 交易小节表（ADR 0018 决策 2/3）：落地后 `max_lookback` / `reserve_bars` 自动值改用真实 session bar 数（内存降到约四成、冷启动从约 2.4 个交易日缩短到 1 个交易日）；session 收盘与空 bar 口径已在 ADR 0031 定稿。数据源落地前 v1 用内置退化表，三条已知差异见 §5.5。
5. 回卷触发：v1 用 SDK `dz_schedule_daily(18:00)` 定时器（《帧契约：策略》§DZ_FRAME_SCHEDULE）；当前帧体系没有“交易日切换”系统帧，若将来新增则改接该帧（不自行造帧）。
6. 秒级以下周期 / 单实例多周期：改用镜像环形（§10 B 方案），接口与语义不变；触发条件见 §10。
7. 对齐与分配优化：`bench_relocate` 实测后若 partial-line 影响显著再考虑（当前结论是不做）；若改为 arena/矩阵布局，行步长禁止取 4096 的整数倍。
8. B 侧定时器（`on_schedule` 转发 + `ctx.schedule_*` 封装）：v1 只允许 A 内部使用 SDK 定时器。
9. 用户序列单独回溯深度（`make_series<T>(init, lookback)`）：v1 全局 `max_lookback`；单条更深回溯会打破「一组序列共享窗口、一次搬迁一次推进」，属架构级，不在 v1。
10. 启动回放并发化（多品种并行回放）：v1 串行，启动时间随品种数线性增长，见 §5.3。
11. Python 绑定（pybind11）与 API 同形约束。
12. 撤单/委托管理/算法下单（A 的后续执行层；从回报流自建订单簿，不需要 ctx→A 反向指针；若需 A 代理下单另立 ADR）。
13. 参数元数据与寻优。
14. 品种加载次线程化（预留：数据与上下文可整块移交线程，无 ThreadLocal 隐藏状态）。
15. 配置目录可写性：若部署目录只读，需要 `DZTRADER_STRATEGY_HOME` 类覆盖；待部署需求确认。
16. UI_INPUT/OUTPUT_UI 字符串命令协议与 dzweb 入口（#9/#6）：框架侧解析/执行/字符串结果已定（§7）；wire 格式、权限、请求关联、结果展示归 #9/#6，本 spec 只做到“测试帧可驱动”。
17. Tier 1 记账口径：**已定稿**（§6.1：盈亏乘 `volume_multiple`、极值在 bar 收盘前更新并在平仓后归零、`entries` 按开仓段计数、`bars_since_*` 边界、`current_contracts` 非负）；实现按此落测试 oracle。
18. td AUTO 开平解析（issue #82）：v1 直接发 AUTO，td 未修复前实盘单被拒属**已知限制**（框架逻辑账照常推进，实盘/逻辑偏差由操作员一键同步处理）；#82 是实盘联调前置，不阻塞框架开发与合并。实现 td 侧时需新 ADR supersede [ADR 0008](../adr/0008-td-position-model.md) 决策 5。
19. 回放续仓：**机制已落地（#161）**，但默认历史源仍指向归档域 stub（#4），因此 v1 生产路径实际恒冷启动；归档域接上后自动启用。回放深度不足以覆盖持仓周期时恢复不完整；需要明确操作员流程（先一键同步/清仓再启动）。
20. 锁仓模式的净额失真：AUTO 在锁仓账户上可能把「平仓」解析为反向开仓，净额正确但多空明细不干净；删除/同步后可能残留净额为 0 的锁仓腿，需人工处理（ADR 0028 接受该失真）。
21. 多策略共享同一账户：一键同步是账户级、按账户总仓对账，无法区分策略归属；需要「手工仓/策略仓归属」设计（平台侧，与 #6/#9 联动）。
22. **bar 合成已定稿**（[ADR 0031](../adr/0031-tick-bar-synthesis-component.md)，issue #3）：tick→bar 口径、交易小节/竞价数据口径、forming 续接（`is_final`）不再开放；**竞价窗口内事件与 `ctx.is_auction()` 已由 [ADR 0032](../adr/0032-auction-window-strategy-events.md) 定稿**（取代 0031 决策 5 的"窗口内不回调"），框架侧已接入并落地（#163：注入含竞价窗口的小节表即生效；生产入口 v1 退化表下 `ctx.is_auction()` 恒 false，§5.5）。仍后置：B 的 `Date`/`Time`/`Q_BidPrice`/`Q_AskPrice` 查询 API（随策略 API 定稿）与归档域历史 bar 数据源（#4）。
23. 同步单的假设：命令执行前账户实际仓位 ≈ 旧 `L_i`；实际已偏时 sync 后仍需账户级一键同步。框架不建成交台账，无法自行判断偏差。
24. 委托/拒单可见性：v1 不跟踪 `ORDER_REPORT`、不撤单；td 拒单只能从 td 日志排查。后续算法/委托管理层（§13 架构级后置）再接管。
25. 账户存在性/在线校验：`account add` 目前只校验字符串与 `scale`；ACCOUNT_STATUS 广播可用于提示“账户未配置/离线”，但属后置增强，避免拼错账户静默丢单。
26. 性能基准进 CI 的方式：先 report-only 收集固定 Release 机器的 p50/p99；稳定后再把 `bench_tick_update` / `bench_bar_close_dual_ma` 升为 p99 门禁（BarBuilder 实现后补 `bench_bar_tick_intraday` / `bench_bar_tick_daily`）。
