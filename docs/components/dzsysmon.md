# dzsysmon（系统监控进程）架构设计

本文档描述 dzsysmon 内部进程的架构设计与核心机制。帧语义、payload 与校验以[《帧契约：系统监控》](../frame_contracts/sysmon.md)为准；本文只写契约不规定、但一旦写错代价高的实现结构。

## 职责与范围

- **内部进程**（ADR 0025）：既非策略、亦非行情/交易网关，由主进程 `dztraderd` 按配置拉起、监护与关闭；7×24 采集本机资源指标并推送实时快照。
- 首版（#70）只实现系统信息与 CPU 总使用率，但快照形状一次定稿：已列出的指标域后续只填值、不改 payload 形状；新增指标类别按 additive 扩展并同步契约/类型/前端 schema（契约 sysmon §2）。
- 前端（#71）：WebUI「资源监控」页替换占位页，展示系统信息与 CPU 总使用率，内存 / 磁盘 / 网络区块按可用性映射显示明确空态（不得显示 0）；顶栏 CPU 徽章接实时快照，写死的 CPU / 内存 / 磁盘假数字与 NTP 徽章已删除。刷新基线由发送方 `realtime.push_interval_sec`（默认 5s）决定，前端不轮询、不改 WS 契约。
- 后续票：每核使用率与饱和核 #72、温度与每核频率 #73、内存与交换 #74、卷与共享内存目录占用 #75、磁盘 I/O #76、网络 #77、历史写入 #78、历史查询 #79、阈值告警 #80、运行期配置 #81。
- 本进程不采集：进程级指标、GPU、SMART、风扇、电池、登录用户列表、NTP 时间偏移；不支持容器内采集（见「部署限制」）。

## 进程与编排

- 主进程在 `dztraderd.json` 的 `internal` 段登记并拉起本进程（段形制同 `md`/`td`；段缺失 = 无内部进程）。可执行文件名精确匹配 `dzsysmon`（不做 `dz*` 通配，ADR 0025），`instance_id` 即进程名（可执行文件名，通用规则 §5）。
- 属非策略子进程：第二趟启动、不等待行情源就绪；关闭批次在交易网关、行情网关之后、`dzweb` 之前——监控要活到能记录其它进程的退出过程，但无需活过 UI（ADR 0025，编排细节见 [dztraderd.md](dztraderd.md)）。
- 重启策略沿用网关默认：启用、最多 5 次、退避 5s（主进程退避为线性；spawn 失败不重启、退出码 0 不重启）。
- 自动重启时主进程统一追加 `--recover`。本进程接受并记录该参数；历史连续性语义（不重置数据文件、重启窗口标记为「采集中断」、绝不伪造连续数据）随历史票 #78 落地，当前不改变采集行为。
- 优雅退出：收到定向 `DZ_FRAME_SHUTDOWN`（契约 process）后停止定时器并退出；历史落盘接入后（#78）还须 flush 队列并干净关库。
- 平台基线检查失败 ⇒ 记录错误、以非 0 退出，不静默降级；主进程按重启策略重试后停在 `Crashed`（ADR 0025，预期行为）。

## 并发与调度

### 线程模型

- **io_context 单线程**：采集、帧读写、实时推送与配置加载全部在这一条 io_context 线程上串行，无业务锁。
- **信号量监听线程**（唯一额外线程）：阻塞在事件通道命名信号量上；唤醒后只投递「排空通道」任务给 io_context，不读取、不解析数据（与 [dztraderd.md](dztraderd.md) 同构）。
- 历史落盘线程：后续票 #78 引入（有界队列 + 批量提交，采集线程只入队、永不阻塞）；当前无此线程。

### 双定时器

| 定时器 | 基线 | 职责 |
|---|---|---|
| 计量 | 1000ms（`sampling.measure_interval_ms`） | 读取原始累计计数器、更新速率基线 |
| 推送 | `realtime.push_interval_sec`，默认 5s | 编码快照帧并广播；计量时刻不推送 |

- **计量与推送解耦**：事件通道是广播介质，任何写入都会唤醒全部已注册进程（含策略），推送不得跟随 1s 计量（契约 sysmon §4、ADR 0023）。
- **时钟纪律**：asio `steady_timer`（单调时钟，免疫 NTP 跳变）；deadline 按「上次 deadline + interval」推进做漂移校正；错过 tick 跳过并累加 `sampling.missed_ticks`（区分机器空闲与监控停顿），不补采；handler 内不做阻塞 I/O。
- 初始化完成后立即推送一次；收到 `QUERY_FULL_SNAPSHOT`（通用规则 §7）响应推送一次。推送失败（如事件通道写满）只记日志、不重试，下一刻度继续（契约 sysmon §4）。
- 运行期修改间隔（`SET_SYSMON_CONFIG`）立即生效：取消当前 pending wait 并按新间隔重排，不等待当前周期结束；配置展示与 UI 编辑入口随 #81 前端落地。

### 事件驱动边界

与主进程、dzweb 之间保持 100% 事件驱动（信号量唤醒、子进程退出回调、定向 SHUTDOWN）。进程内周期采样是当前本仓唯一被允许的例外，边界见 ADR 0023；不得扩散到主进程或网关进程。

## 采集接缝

- [`SystemCollector`](../../libs/platform/include/dztrader/platform/system_collector.h) 是唯一采集接缝，平台无关：只返回**原始累计计数器**与原始系统信息（`RawCpuTimes` / `RawSystemInfo`），不含百分比、增量、速率计算与平台条件分支。
- 两平台实现经 `make_system_collector()` 按编译平台选择；实现无状态、可在单线程事件循环内调用、不抛异常（系统调用失败返回 `nullopt` / 空值）。
- 速率与百分比由**纯函数**从原始计数器差值计算（契约 sysmon §2.7）：首次采样输出不可用（`null`，不得用 0 冒充）；32 位计数器回绕仅当两次值均落在 32 位范围内且本次小于上次时处理，其余下降视为计数器重置、本次输出不可用；Linux `iowait` 不计入忙，`guest` / `guest_nice` 不重复计入总量；CPU 使用率 = `1 − Δidle/Δtotal`，钳制到 `[0,1]`。
- 采集失败（如 `/proc` 读取失败）跳过本次计量并累加 `sampling.missed_ticks`。
- Windows 系统信息必须取真实 NT 版本：用 `RtlGetVersion`，禁止 `GetVersionEx` / `VerifyVersionInfo` / `IsWindows*`（manifest 兼容性垫片会谎报版本）；`platform.build` 用于区分 Windows 10 与 11（两者都报 NT 10.0）。
- 内存唯一口径：`used_bytes = total_bytes − available_bytes`（Linux 取 `MemAvailable`），不得使用「总量 − 空闲」（ADR 0024）。

## 快照与可用性

- `DZ_FRAME_RTN_SYSMON_SNAPSHOT`：形态 4 纯上行推送，无 `instance_id`，发送方固定 `dzsysmon`（payload `source` 显式携带来源，通用规则 §5）；dzweb 消费后更新镜像并 WS 广播 `sysmon_snapshot`，REST `GET /api/sysmon/snapshot` 读取镜像（契约 sysmon §5/§6、契约 rest、契约 webui-ws）。两个 sysmon 域均进快照域，WS 建连立即拿到当前值，供顶栏全局使用。
- 类型层真相源：[`sysmon.h`](../../libs/platform/include/dztrader/platform/sysmon.h)（`SysmonSnapshot` 及 `validate`）。`null` **仅允许**出现在契约 §2 标注可空的位置，其余位置出现 `null` 为校验失败；dzweb 侧校验失败记日志并忽略（不更新镜像、不广播）。
- **可用性描述**（关键设计）：每个快照必须携带 `availability`——`platform_description`（Windows 必含 build 号）、`metrics`（固定键集合，每帧全覆盖；`available=false` 时 `reason` 必填）、`semantic_hints`。前端按可用性渲染，**不按 OS 名硬编码分支**；不可用一律显示「不可用」，不显示 0。
- 首版可用性：`system`、`cpu.total` 可用；其余键固定 `available=false` + `reason="not_implemented"`，对应域输出 `null` / 空数组（契约 sysmon §2.5）。
- 跨平台语义差异：Windows 无原生 load average，只能给处理器队列长度代理量（`semantic="processor_queue"`），与 Linux load average 不是同一个量、前端不得同轴比较；该指标随每核票 #72 落地。

## 配置

- 配置文件：`$DZTRADER_HOME/configs/dzsysmon.json`。进程启动时缺失则**自动生成完整默认值**，零配置可运行；仓库不提交模板文件（AGENTS 门禁 6）。
- 内容：`log`（日志，走《帧契约：日志》的 `SET/RTN_LOG_CONFIG`，不在 sysmon 配置帧 payload 内）与 `sampling` / `disk` / `network` / `history` / `realtime` / `alerts`（字段、默认值与约束见契约 sysmon §3.1）。历史、告警、指标大类等配置项当前只定义形状，消费方随对应后续票落地（#72–#81）。
- 运行期修改：`SET_SYSMON_CONFIG` 为 RFC 7386 merge patch（任意位置 `null` 非法；空对象无操作但仍回 RTN），成功回推全量 `RTN_SYSMON_CONFIG`；失败按四件套处理——回滚旧值 → 日志 → `NOTIFY_UI` error（`popup=true`）→ 回推旧值（契约 sysmon §3.3、通用规则 §8）。
- 触发场景：初始化完成主动 RTN 一次；每次 SET 后回推；`QUERY_FULL_SNAPSHOT` 响应时回推（契约 sysmon §3.2）。
- 进程内已落地（#70）：`SET_SYSMON_CONFIG` 接收 → 校验 → 整文件原子持久化（保留 `log`/未知 section）→ 内存镜像更新 → 定时器即时重排 → 全量 RTN；配置展示与 UI 编辑入口随 #81 前端落地。

## 平台基线

| 平台 | 基线 |
|---|---|
| Windows | x86_64；NT 10.0 及以上（Windows 10 build 10240 / Server 2016 build 14393）；真实版本经 `RtlGetVersion` 取得 |
| Linux | x86_64；内核 ≥ 4.18、glibc ≥ 2.28（RHEL 8+ / Debian 10+ / Ubuntu 20.04+）；GCC 13 工具链 |

- 编译期用 `#error`（非目标平台 / 32 位 / Linux 工具链与 glibc 下限）拒绝构建；启动期 `check_platform_baseline()` 复核运行期基线（Windows NT 版本、Linux 内核/glibc/架构），失败记录错误并以非 0 退出。Windows 的 NT 版本无法在编译期可靠断言（构建侧 `WINVER`/`_WIN32_WINNT` 是 API 可用性声明，不是运行期判据），由启动期检查承担。禁止静默回退（AGENTS 跨平台门禁）。
- 真相源：[`platform_requirements.h`](../../libs/platform/include/dztrader/platform/platform_requirements.h)。

## 历史（后续票）

- #78 历史写入：独立 SQLite（WAL）历史库，位于 `$DZTRADER_HOME/db/`，与交易库、WebUI 库分离；**dzsysmon 唯一写者、dzweb 只读**。schema 与行编解码放在共享模块（唯一一份定义），宽表存固定标量、长表存按核 / 盘 / 网卡展开的维度；每条记录保留窗口 min/max/avg/last（尖峰由 max 保留）；原始记录默认保留 24 小时，超期数据在记录间隔小于 1 分钟时压成一分钟聚合、否则原样保留，最长保留 30 天，另有总量上限兜底；采集中断与时钟跳变写入标记，绝不伪造连续数据；落盘在独立线程（有界队列 + 批量提交，队列满丢最旧并计数）。
- #79 历史查询：dzweb 只读打开历史库并经 REST 提供；库缺失、schema 版本不符或文件损坏时优雅降级为「历史不可用」，不崩溃、不返回伪造数据。
- 当前状态：不写历史、无历史查询端点（规划）。

## 告警（后续票）

- #80 阈值告警：内存、交换、关注卷剩余空间（百分比与绝对容量两种形态）越限时复用既有 `DZ_FRAME_NOTIFY_UI`（不新增通知帧），带冷却防抖与恢复信号；路径感知只关注共享内存目录与数据库目录所在卷。
- 当前状态：未实现（规划）；`alerts` 配置段已定义形状。

## 第三方归属

- 采集的规格与算法参考 psutil（BSD-3-Clause）：只用于确定「该调哪个 OS API、必须处理哪些边界」与测试不变量（ADR 0024）；不翻译其 C 代码、不嵌入 CPython 运行时、不引入第三方系统信息库。
- 归属声明与 BSD-3 全文见仓库根 [THIRD_PARTY_NOTICES.md](../../THIRD_PARTY_NOTICES.md)；文档、UI 与发布物不得出现暗示 psutil 背书的措辞。

## 部署限制：不支持容器内采集

- 容器内 `/proc` 报告的是宿主容量（cgroup 配额不会反映到这些接口），会给出错误的容量与使用率基线；本版本不做 cgroup v1/v2 感知。
- 因此 `dzsysmon` 必须运行在宿主机上；需要监控容器所在机器时，在宿主机部署本进程（契约 sysmon §8 明确本条）。

## 设计原则

- **可观测性诚实**：不可用标不可用并给 reason，首次采样 / 计数器重置输出 `null`，不拿 0 冒充。
- **采集与推送解耦**：短计量窗口服务精度，低频推送服务广播成本。
- **定时器纪律**：单调时钟、漂移校正、错过即记缺口、handler 不阻塞。
- **事件驱动边界清晰**：进程内周期采样是当前唯一例外（ADR 0023），跨进程通信保持事件驱动。
- **单一真相源**：payload 形状与校验在[契约](../frame_contracts/sysmon.md)，类型在 `sysmon.h`，采集在 `system_collector.h`；本文不复制字段表。

## 参考

- [《帧契约：系统监控》](../frame_contracts/sysmon.md)（帧、payload、校验、范围）
- 平台头文件：[sysmon.h](../../libs/platform/include/dztrader/platform/sysmon.h)、[system_collector.h](../../libs/platform/include/dztrader/platform/system_collector.h)、[platform_requirements.h](../../libs/platform/include/dztrader/platform/platform_requirements.h)
- ADR：[0023](../adr/0023-sysmon-in-process-periodic-sampling.md)（进程内周期采样）、[0024](../adr/0024-sysmon-psutil-as-reference-only.md)（psutil 仅作参考）、[0025](../adr/0025-internal-process-category.md)（Internal 进程类别）
- [dztraderd.md](dztraderd.md)（进程编排、启动 / 关闭顺序、内部进程）
