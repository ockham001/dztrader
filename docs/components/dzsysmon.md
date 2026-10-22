# dzsysmon（系统监控进程）架构设计

本文档描述 dzsysmon 内部进程的架构设计与核心机制。帧语义、payload 与校验以[《帧契约：系统监控》](../frame_contracts/sysmon.md)为准；本文只写契约不规定、但一旦写错代价高的实现结构。

## 职责与范围

- **内部进程**（ADR 0025）：既非策略、亦非行情/交易网关，由主进程 `dztraderd` 按配置拉起、监护与关闭；7×24 采集本机资源指标并推送实时快照。
- 已实现（#70/#72/#73/#74/#75）：系统信息、CPU 总使用率、每核使用率与饱和核标记、系统负载、上下文切换/中断速率、温度（Linux hwmon）与每核频率（Linux cpufreq）、内存与交换（含缓存/缓冲）、卷容量与关注路径落卷、共享内存目录占用。快照形状一次定稿：已列出的指标域后续只填值、不改 payload 形状；新增指标类别按 additive 扩展并同步契约/类型/前端 schema（契约 sysmon §2）。
- 前端（#71/#72/#73/#74/#75）：WebUI「资源监控」页展示系统信息与完整 CPU 区块（总使用率、每核含饱和标记、系统负载、上下文切换/中断、每核频率），温度在可用时以独立卡片展示；内存卡片接实时快照（使用率、已用/总量、可用、缓存、缓冲）并含交换子区块（无交换分区显示明确文案）；磁盘区块展示各卷容量与使用率（关注卷高亮、读不到的卷显示「不可用」+ 原因）与共享内存目录占用（大小、页文件数、最旧页文件年龄）；网络区块仍按可用性映射显示明确空态（不得显示 0）。温度与每核频率区块**仅在可用时出现，不可用时整块不渲染**（与内存等区块的「不可用」空态不同）。顶栏 CPU / 内存 / 各盘卷徽章接实时快照，写死的 CPU / 内存 / 磁盘假数字与 NTP 徽章已删除。刷新基线由发送方 `realtime.push_interval_sec`（默认 5s）决定，前端不轮询、不改 WS 契约。
- 后续票：磁盘 I/O #76、网络 #77、历史写入 #78、历史查询 #79、阈值告警 #80、运行期配置 #81。
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

- [`SystemCollector`](../../libs/platform/include/dztrader/platform/system_collector.h) 是唯一采集接缝，平台无关：只返回**原始累计计数器**、原始系统信息与原始瞬时读数（`RawCpuTimes` / `RawSystemInfo` / `RawLoadAverage` / `RawTemperature[]` / `RawCoreFrequency[]` / `RawMemoryInfo` / `RawSwapInfo`），不含百分比、增量、速率计算与平台条件分支。`RawCpuTimes` 携带全局与每核累计时间、上下文切换与中断累计计数；`RawLoadAverage` 携带平台语义标识；温度与频率由采集器做**格式归一**（摄氏度、整数 MHz、稳定 sensor 标识）但不做聚合与平滑；内存/交换由采集器做**口径归一**（`used = total − available/free` 所需的原始量、异常值钳制）。
- 两平台实现经 `make_system_collector()` 按编译平台选择；可在单线程事件循环内调用、不抛异常（系统调用失败返回 `nullopt` / 空值）。Windows 负载代理实现持有 PDH 查询句柄与指数加权状态（进程内单线程访问）。
- 速率与百分比由**纯函数**从原始计数器差值计算（契约 sysmon §2.7）：首次采样输出不可用（`null`，不得用 0 冒充）；32 位计数器回绕仅当两次值均落在 32 位范围内且本次小于上次时处理，其余下降视为计数器重置、本次输出不可用；Linux `iowait` 不计入忙，`guest` / `guest_nice` 不重复计入总量；CPU 使用率 = `1 − Δidle/Δtotal`，钳制到 `[0,1]`。
- 采集失败（如 `/proc` 读取失败）跳过本次计量并累加 `sampling.missed_ticks`。
- Windows 系统信息必须取真实 NT 版本：用 `RtlGetVersion`，禁止 `GetVersionEx` / `VerifyVersionInfo` / `IsWindows*`（manifest 兼容性垫片会谎报版本）；`platform.build` 用于区分 Windows 10 与 11（两者都报 NT 10.0）。
- 每核/速率/负载来源（实现细节；契约只规定 payload 语义）：Linux 单次读 `/proc/stat` 得全局 `cpu `、每核 `cpuN`、`ctxt`、`intr`，负载用 `getloadavg()`；Windows 全局时间用 `GetSystemTimes`，每核时间与中断用 `NtQuerySystemInformation(SystemProcessorPerformanceInformation)`，上下文切换用 `NtQuerySystemInformation(SystemPerformanceInformation)`，负载用 PDH `\System\Processor Queue Length` 做 60/300/900 秒窗口的指数加权移动平均。
- 温度与每核频率来源（#73；Linux）：温度扫描 `/sys/class/hwmon/hwmon*/temp*_input`（毫摄氏度/1000 得 ℃，sensor 标识 `<hwmon name>#<hwmonN>:<label>`，无 label 时 `<hwmon name>#<hwmonN>:tempN`（含 hwmon 目录名，稳定且不随同名芯片增减变化；病态重复 label 追加 `-<n>` 兜底）），**不按设备类型过滤**——厂商芯片经 hwmon 暴露的 GPU/网卡温度也展示（仅温度读数，不扩张 C 组指标）；无 hwmon 或全部读取失败时报告不可用（`no_sensor`）；每核频率读 `/sys/devices/system/cpu/cpuN/cpufreq/scaling_cur_freq`（kHz 四舍五入到 MHz），无 cpufreq（含多数容器/部分虚拟机）时报告不可用（`unsupported`）。单个传感器/核读取失败只跳过该项，不使整类失败。Windows 两个采集器方法恒返回 `std::nullopt`（无可靠 API，本版本也不提供整体频率）；默认配置又关闭了对应的 B 组开关，故通常为 `disabled_by_config`，用户显式开启后为 `unsupported`。
- Windows 中断数是各核 32 位 `InterruptCount` 之和：合计可超过 2^32，单核回绕会使合计下降并被判定为计数器重置（该帧中断速率 `null`，下一采样重建基线）。中断没有独立可用性键（`cpu.context_switches` 覆盖两项），中断单独不可用时该帧 `interrupts_per_sec=null`、上下文切换照常。
- Windows >64 逻辑核时 NT 每核查询只覆盖调用线程所在处理器组：序列数与逻辑核数不一致时每核标记不可用（`collect_failed`）、中断累计计数输出 `null`；上下文切换来自系统级查询仍可用。不得用组内数据冒充全系统每核序列。
- 内存与交换来源（#74；实现细节；契约只规定 payload 语义）：Linux 读 `/proc/meminfo`——`MemTotal` / `MemAvailable` / `Buffers` / `Cached` / `SReclaimable` / `SwapTotal` / `SwapFree`；`MemAvailable` 缺失、为 0 或大于总量时回退 `MemFree + Buffers + Cached + SReclaimable` 并钳制到 `[0, total]`，异常与回退记 WARN 日志，回退也无法计算时本次内存采集失败（该域 `null`，可用性粘性）；`cached_bytes = Cached + SReclaimable`，`buffers_bytes = Buffers`，两者均钳制到总量。Windows 读 `GetPerformanceInfo`（`psapi`）：`total = PhysicalTotal × PageSize`、`available = PhysicalAvailable × PageSize`（Windows 上"空闲"与"可用"同义，不单列 `free`）、`cached = SystemCache × PageSize`、`buffers` 不提供；交换为页面文件口径——`total = CommitLimit − PhysicalTotal`、`used = CommitTotal − PhysicalTotal` 并钳制（无页面文件时 `total = 0` → 可用性 `unsupported`）。
- 内存唯一口径：`used_bytes = total_bytes − available_bytes`（Linux 取 `MemAvailable`；Windows 取 `PhysicalAvailable`），不得使用「总量 − 空闲」（ADR 0024）；交换同理 `used = total − free`。
- 卷、关注路径与共享内存目录来源（#75；实现细节；契约只规定 payload 语义）：Linux 解析 `/proc/mounts`（还原八进制转义的挂载点、过滤伪文件系统与内存文件系统、同挂载点去重），`statvfs` 逐卷取容量；容量读不到（`EACCES`/`EPERM` → `no_permission`，其它 → `collect_failed`）的卷仍进数组、`total_bytes`/`free_bytes` 留 `nullopt`。Windows 用 `GetLogicalDrives` + `GetDriveTypeW` 只取 `DRIVE_FIXED` 固定盘，`GetDiskFreeSpaceExW` 取容量；`ERROR_ACCESS_DENIED` → `no_permission`。关注路径落卷：Linux 先 `weakly_canonical` 规范化（跟随符号链接、保留不存在的尾部）再按**路径分量边界**做最长前缀匹配（`/home` 不匹配 `/home2`）；Windows 用 `GetFullPathNameW` 规范化后由 `GetVolumePathNameW` 给出挂载点（正确处理目录挂载点与卷 GUID）。路径不存在 ⇒ `path_not_found`，解析不到卷 ⇒ 不得猜卷、不得回退到 `/`（契约 sysmon §2.4）。共享内存目录占用递归统计字节数（含 `meta.dat`）、页文件数（`<数字>.dat`，与 `libs/shm` 的页文件判定同口径）与最旧**页文件**年龄（`meta.dat` 不参与年龄计算——它不会随页清理消失，参与会把"页清理失效"掩盖成"有个旧文件"）；目录不可读返回 `nullopt` 而不是全 0 的对象（0 是"目录为空"这一真实事实）。

## 快照与可用性

- `DZ_FRAME_RTN_SYSMON_SNAPSHOT`：形态 4 纯上行推送，无 `instance_id`，发送方固定 `dzsysmon`（payload `source` 显式携带来源，通用规则 §5）；dzweb 消费后更新镜像并 WS 广播 `sysmon_snapshot`，REST `GET /api/sysmon/snapshot` 读取镜像（契约 sysmon §5/§6、契约 rest、契约 webui-ws）。两个 sysmon 域均进快照域，WS 建连立即拿到当前值，供顶栏全局使用。
- 类型层真相源：[`sysmon.h`](../../libs/platform/include/dztrader/platform/sysmon.h)（`SysmonSnapshot` 及 `validate`）。`null` **仅允许**出现在契约 §2 标注可空的位置，其余位置出现 `null` 为校验失败；dzweb 侧校验失败记日志并忽略（不更新镜像、不广播）。
- **可用性描述**（关键设计）：每个快照必须携带 `availability`——`platform_description`（Windows 必含 build 号）、`metrics`（固定键集合，每帧全覆盖；`available=false` 时 `reason` 必填）、`semantic_hints`。前端按可用性渲染，**不按 OS 名硬编码分支**；不可用一律显示「不可用」，不显示 0。
- 当前可用性（#75）：`system`、`cpu.total`、`cpu.cores`、`cpu.load_average`、`cpu.context_switches`、`memory`、`swap`、`volumes`、`shm_dir` 可用；Linux 上按宿主能力实现 `cpu.frequency`（cpufreq）与 `temperature`（hwmon），Windows 上两者不提供（默认配置关闭对应 B 组开关 → `disabled_by_config`；显式开启后采集器恒报 `unsupported`）；无交换分区/无页面文件时 `swap` 为 `unsupported`；`disk_io` / `network` 固定 `available=false` + `reason="not_implemented"`，对应域输出 `null`（契约 sysmon §2.5）。采集能力不可用时用 `reason="collect_failed"`（温度无传感器用 `no_sensor`、无 cpufreq/无交换用 `unsupported`）；能力可用但本帧数值未就绪时为 `available=true`、数值 `null`；配置关闭对应指标组时为 `available=false` + `reason="disabled_by_config"`。**数值不沿用上一帧**：本帧未采到即输出 `null`/空数组（能力标记粘性），避免把旧数据当当前值展示。
- 渲染语义分两档：内存/交换/磁盘/网络等区块按可用性显示明确空态——不可用时显示「不可用」+ 原因（`swap` 的 `unsupported` 说明为「本机无交换分区/页面文件」），能力可用但本帧无数据时显示「暂无数据」；温度与每核频率区块**仅在可用时渲染，不可用时整块不出现**（不在前端按平台名分支，只看 `availability.metrics`）。后者避免在 Windows 等平台长期占据版面显示无意义空态。内存的 `used_bytes` / `used_ratio` 由前端按唯一口径重算，保证字节与百分比一致。
- 饱和核判定（契约 sysmon §2.7）：单核使用率连续 `alerts.cpu_sustain_samples` 次采样 ≥ `alerts.cpu_sustain_used_pct / 100` 标记饱和，任一采样严格低于阈值即解除；不可用样本不改变计数；核序列变化（热插拔）重置计数。阈值/次数取当前配置，运行期编辑入口随 #81。
- 跨平台语义差异：Windows 无原生 load average，以处理器队列长度做指数加权移动平均作为代理量（`semantic="processor_queue"`），与 Linux load average 不是同一个量；口径说明由 `availability.semantic_hints["cpu.load_average"]` 承载，前端只按该字段展示、不得同轴比较（不按 OS 名分支）。

## 配置

- 配置文件：`$DZTRADER_HOME/configs/dzsysmon.json`。进程启动时缺失则**自动生成完整默认值**，零配置可运行；仓库不提交模板文件（AGENTS 门禁 6）。
- 内容：`log`（日志，走《帧契约：日志》的 `SET/RTN_LOG_CONFIG`，不在 sysmon 配置帧 payload 内）与 `sampling` / `disk` / `network` / `history` / `realtime` / `alerts`（字段、默认值与约束见契约 sysmon §3.1）。`alerts.cpu_sustain_used_pct` / `cpu_sustain_samples` 已由 #72 的饱和核判定消费；`sampling.groups.temperature` / `sampling.groups.cpu_frequency` 由 #73 消费（Linux 默认开、Windows 默认关，关闭时不采集并把可用性标为 `disabled_by_config`）；`sampling.groups.memory` / `sampling.groups.swap` 由 #74 消费（关闭时跳过采集并标 `disabled_by_config`）；历史、阈值告警等配置项的消费方随对应后续票落地（#75–#81）。
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

- #80 阈值告警：内存、交换、关注卷剩余空间（百分比与绝对容量两种形态）越限时复用既有 `DZ_FRAME_NOTIFY_UI`（不新增通知帧），带冷却防抖与恢复信号；路径感知只关注共享内存目录与数据库目录所在卷——落卷解析与 `volumes[].watched` 已由 #75 提供（`watch_paths` 不受 `sampling.groups.volumes` 开关影响，关掉卷容量采集仍能定位关注卷）。
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
