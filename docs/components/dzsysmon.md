# dzsysmon（系统监控进程）架构设计

本文档描述 dzsysmon 内部进程的架构设计与核心机制。帧语义、payload 与校验以[《帧契约：系统监控》](../frame_contracts/sysmon.md)为准；本文只写契约不规定、但一旦写错代价高的实现结构。

## 职责与范围

- **内部进程**（ADR 0025）：既非策略、亦非行情/交易网关，由主进程 `dztraderd` 按配置拉起、监护与关闭；7×24 采集本机资源指标并推送实时快照。
- 已实现（#70/#72/#73/#74/#75/#76/#77）：系统信息、CPU 总使用率、每核使用率与饱和核标记、系统负载、上下文切换/中断速率、温度（Linux hwmon）与每核频率（Linux cpufreq）、内存与交换（含缓存/缓冲）、卷容量（本机全部卷）、共享内存目录占用、每物理盘 I/O（吞吐/IOPS/延迟/队列深度/利用率）、每网卡收发速率与累计量（默认隐藏虚拟网卡）。快照形状一次定稿：已列出的指标域后续只填值、不改 payload 形状；新增指标类别按 additive 扩展并同步契约/类型/前端 schema（契约 sysmon §2）。
- 前端（#71/#72/#73/#74/#75/#76/#77）：WebUI「资源监控」页展示系统信息与完整 CPU 区块（总使用率、每核含饱和标记、系统负载、上下文切换/中断、每核频率），温度在可用时以独立卡片展示；内存卡片接实时快照（使用率、已用/总量、可用、缓存、缓冲）并含交换子区块（无交换分区显示明确文案）；磁盘区块展示各卷容量与使用率（正文按使用率降序列出容量可读且总量 > 0 的卷，读不到容量与总量为 0 的挂载折叠进「无容量挂载（N）」分组、组内显示挂载点与原因；卷按使用率着色（≥70% 黄 / ≥90% 红），与顶栏卷徽章共用同一组阈值常量（前端共享模块 `useResourceLevel.ts`）；卷枚举口径与无容量折叠见 #122）与共享内存目录占用（大小、页文件数、最旧页文件年龄）；磁盘 I/O 区块按盘展示读/写吞吐、读/写 IOPS、平均延迟、队列深度与利用率，首次采样/计数器重置的字段显示「不可用」而不是 0，指标不可用时整块显示「不可用」+ 原因（Windows 计数器未开启/无权限时给出对应文案），并展示发送方 `semantic_hints["disk_io"]` 的口径提示；网络区块按网卡展示收/发速率与累计字节、错误、丢弃，逐项 `null`（首次采样/新网卡/计数器重置）显示「不可用」而不是 0，并提供 admin 可用的「显示全部网卡」开关（下发 `SET_SYSMON_CONFIG` 的 `network.hide_virtual`，即时生效），全部虚拟网卡被隐藏时显示「已隐藏全部虚拟网卡」而不是「不可用」。温度与每核频率区块**仅在可用时出现，不可用时整块不渲染**（与内存等区块的「不可用」空态不同）。顶栏 CPU / 内存 / 各盘卷徽章接实时快照，写死的 CPU / 内存 / 磁盘假数字与 NTP 徽章已删除。资源越限的日志形态由 #123 落地（`NOTIFY_UI` 通知已删除，判定结果只写日志）；快照 `alerts[]` 字段、前端「资源告警」横幅与顶栏 `ALERT` 徽章已删除（#125）。监控页另有「配置」tab（#81）：展示当前生效配置（所有已认证用户可读），admin 可编辑全部可改项并以 WS `sysmon_config` 回推为生效信号。刷新基线由发送方 `realtime.push_interval_sec`（默认 5s）决定，前端不轮询、不改 WS 契约。
- 已实现历史写入（#78）：独立 SQLite（WAL）历史库、min/max/avg/last 聚合、保留与压缩、缺口/跳变/重启标记、落盘线程。
- 已实现历史查询（#79）：`dzweb` 只读打开历史库并经 REST `GET /api/sysmon/history` 提供（契约 sysmon §6.1），监控页提供「实时 / 历史」视图切换；历史查询不走帧、历史视图不订阅 WS。
- **阈值越限（#123 改为日志；ADR 0034）**：判定对象为内存使用率、交换使用率、全部卷剩余空间与 CPU 饱和；进入越限写 `warning`、恢复写 `info`、持续期每 `thresholds.log_repeat_sec`（默认 600s）重记一条 `info`。资源类**不产生任何 `NOTIFY_UI`**，快照也不携带越限集合（契约 sysmon §2.8）；UI 上的使用率色标由 #125 落地。
- 已实现运行期配置与配置展示（#81）：配置帧 SET/RTN 全量生效、运行期切换历史记录、监控页「配置」区展示当前生效配置并提供 admin 编辑入口（契约 sysmon §3）。
- 本进程不采集：进程级指标、GPU、SMART、风扇、电池、登录用户列表、NTP 时间偏移；不支持容器内采集（见「部署限制」）。

## 进程与编排

- 主进程在 `dztraderd.json` 的 `internal` 段登记并拉起本进程（段形制同 `md`/`td`；段缺失 = 无内部进程）。可执行文件名精确匹配 `dzsysmon`（不做 `dz*` 通配，ADR 0025），`instance_id` 即进程名（可执行文件名，通用规则 §5）。
- 属非策略子进程：第二趟启动、不等待行情源就绪；关闭批次在交易网关、行情网关之后、`dzweb` 之前——监控要活到能记录其它进程的退出过程，但无需活过 UI（ADR 0025，编排细节见 [dztraderd.md](dztraderd.md)）。
- 重启策略沿用网关默认：启用、最多 5 次、退避 5s（主进程退避为线性；spawn 失败不重启、退出码 0 不重启）。
- 自动重启时主进程统一追加 `--recover`。本进程接受该参数并把它写进历史的 `restart` 标记（`detail` 为 `recover` / `manual`），供读取方区分异常重启与人工重启。**历史连续性优先**：重启不重置数据文件、不丢已有聚合，重启窗口标记为「采集中断」（绝不伪造连续数据）。
- 优雅退出：收到定向 `DZ_FRAME_SHUTDOWN`（契约 process）后停止定时器并退出；退出前先 flush 当前未结束窗口 → 冲空队列 → WAL checkpoint → 关库（`shutdown_history`）。
- 平台基线检查失败 ⇒ 记录错误、以非 0 退出，不静默降级；主进程按重启策略重试后停在 `Crashed`（ADR 0025，预期行为）。

## 并发与调度

### 线程模型

- **io_context 单线程**：采集、帧读写、实时推送与配置加载全部在这一条 io_context 线程上串行，无业务锁。
- **信号量监听线程**（#70）：阻塞在事件通道命名信号量上；唤醒后只投递「排空通道」任务给 io_context，不读取、不解析数据（与 [dztraderd.md](dztraderd.md) 同构）。
- **历史落盘线程**（#78）：有界队列 + 批量提交，采集线程只入队、永不阻塞。数据库提交与 fsync 在磁盘繁忙时可阻塞数十至数百毫秒，放 io_context 上会同时拖住采集与帧处理（详见「历史写入与保留」）。

### 双定时器

| 定时器 | 基线 | 职责 |
|---|---|---|
| 计量 | 1000ms（`sampling.measure_interval_ms`） | 读取原始累计计数器、更新速率基线 |
| 推送 | `realtime.push_interval_sec`，默认 5s | 编码快照帧并广播；计量时刻不推送 |

历史记录不占独立定时器：每个**计量** tick 顺带观测一次快照（记录窗口的 min/max 需要全部样本），窗口边界由墙钟桶对齐决定（`sampling.record_interval_sec` + 相位偏移）。
阈值越限判定同样不占独立定时器：每个计量 tick 用「内存/交换/全部卷/CPU 饱和」的当前事实推进一次状态机并写日志（1s 判定、5s 推送，越限被发现的时间不超过一个计量间隔）。

- **计量与推送解耦**：事件通道是广播介质，任何写入都会唤醒全部已注册进程（含策略），推送不得跟随 1s 计量（契约 sysmon §4、ADR 0023）。
- **时钟纪律**：asio `steady_timer`（单调时钟，免疫 NTP 跳变）；deadline 按「上次 deadline + interval」推进做漂移校正；错过 tick 跳过并累加 `sampling.missed_ticks`（区分机器空闲与监控停顿），不补采；handler 内不做阻塞 I/O。
- 初始化完成后立即推送一次；收到 `QUERY_FULL_SNAPSHOT`（通用规则 §7）响应推送一次。推送失败（如事件通道写满）只记日志、不重试，下一刻度继续（契约 sysmon §4）。
- 运行期修改间隔（`SET_SYSMON_CONFIG`）立即生效：取消当前 pending wait 并按新间隔重排，不等待当前周期结束；配置展示与编辑入口见「配置」章节。

### 事件驱动边界

与主进程、dzweb 之间保持 100% 事件驱动（信号量唤醒、子进程退出回调、定向 SHUTDOWN）。进程内周期采样是当前本仓唯一被允许的例外，边界见 ADR 0023；不得扩散到主进程或网关进程。

## 采集接缝

- [`SystemCollector`](../../libs/platform/include/dztrader/platform/system_collector.h) 是唯一采集接缝，平台无关：只返回**原始累计计数器**、原始系统信息与原始瞬时读数（`RawCpuTimes` / `RawSystemInfo` / `RawLoadAverage` / `RawTemperature[]` / `RawCoreFrequency[]` / `RawMemoryInfo` / `RawSwapInfo`），不含百分比、增量、速率计算与平台条件分支。`RawCpuTimes` 携带全局与每核累计时间、上下文切换与中断累计计数；`RawDiskIoInfo` 携带每物理盘的累计读/写次数、字节数、服务时间、忙时间与采样时刻在途请求数（时间统一微秒，扇区统一 512 字节换算）；`RawLoadAverage` 携带平台语义标识；温度与频率由采集器做**格式归一**（摄氏度、整数 MHz、稳定 sensor 标识）但不做聚合与平滑；内存/交换由采集器做**口径归一**（`used = total − available/free` 所需的原始量、异常值钳制）。
- 两平台实现经 `make_system_collector()` 按编译平台选择；可在单线程事件循环内调用、不抛异常（系统调用失败返回 `nullopt` / 空值）。Windows 负载代理实现持有 PDH 查询句柄与指数加权状态（进程内单线程访问）。
- 速率与百分比由**纯函数**从原始计数器差值计算（契约 sysmon §2.7）：首次采样输出不可用（`null`，不得用 0 冒充）；32 位计数器回绕仅当两次值均落在 32 位范围内且本次小于上次时处理，其余下降视为计数器重置、本次输出不可用；Linux `iowait` 不计入忙，`guest` / `guest_nice` 不重复计入总量；CPU 使用率 = `1 − Δidle/Δtotal`，钳制到 `[0,1]`。磁盘 I/O 的派生值同样全部由纯函数计算（`compute_disk_io`）：吞吐/IOPS = Δ计数器/Δt；平均延迟 = Δ(读+写服务时间)/Δ(读+写完成数)；利用率 = Δ忙时间/Δt（钳制 `[0,1]`，Δ忙时间超过窗口 1.5 倍视为口径异常输出 `null`）；队列深度取本帧瞬时在途请求数。计数器回绕按采样携带的**原生位宽跨度**处理（数据驱动，纯函数不按平台分支）：Linux 计数/字节为 64 位（不按回绕，下降即重置）、时间字段来自 32 位毫秒计数器（微秒跨度 `2^32 × 1000`）；Windows 计数为 32 位 DWORD（跨度 `2^32`）、字节与 100ns 时间为 64 位。
- **计量 tick 级**采集失败（如 `/proc/stat` 读取失败）跳过本次计量并累加 `sampling.missed_ticks`；单个指标域（如磁盘 I/O）采集失败只把该域置 `null`，不累加（该标记表示 tick 整体被跳过）。
- Windows 系统信息必须取真实 NT 版本：用 `RtlGetVersion`，禁止 `GetVersionEx` / `VerifyVersionInfo` / `IsWindows*`（manifest 兼容性垫片会谎报版本）；`platform.build` 用于区分 Windows 10 与 11（两者都报 NT 10.0）。
- 每核/速率/负载来源（实现细节；契约只规定 payload 语义）：Linux 单次读 `/proc/stat` 得全局 `cpu `、每核 `cpuN`、`ctxt`、`intr`，负载用 `getloadavg()`；Windows 全局时间用 `GetSystemTimes`，每核时间与中断用 `NtQuerySystemInformation(SystemProcessorPerformanceInformation)`，上下文切换用 `NtQuerySystemInformation(SystemPerformanceInformation)`，负载用 PDH `\System\Processor Queue Length` 做 60/300/900 秒窗口的指数加权移动平均。
- 温度与每核频率来源（#73；Linux）：温度扫描 `/sys/class/hwmon/hwmon*/temp*_input`（毫摄氏度/1000 得 ℃，sensor 标识 `<hwmon name>#<hwmonN>:<label>`，无 label 时 `<hwmon name>#<hwmonN>:tempN`（含 hwmon 目录名，稳定且不随同名芯片增减变化；病态重复 label 追加 `-<n>` 兜底）），**不按设备类型过滤**——厂商芯片经 hwmon 暴露的 GPU/网卡温度也展示（仅温度读数，不扩张 C 组指标）；无 hwmon 或全部读取失败时报告不可用（`no_sensor`）；每核频率读 `/sys/devices/system/cpu/cpuN/cpufreq/scaling_cur_freq`（kHz 四舍五入到 MHz），无 cpufreq（含多数容器/部分虚拟机）时报告不可用（`unsupported`）。单个传感器/核读取失败只跳过该项，不使整类失败。Windows 两个采集器方法恒返回 `std::nullopt`（无可靠 API，本版本也不提供整体频率）；默认配置又关闭了对应的 B 组开关，故通常为 `disabled_by_config`，用户显式开启后为 `unsupported`。
- Windows 中断数是各核 32 位 `InterruptCount` 之和：合计可超过 2^32，单核回绕会使合计下降并被判定为计数器重置（该帧中断速率 `null`，下一采样重建基线）。中断没有独立可用性键（`cpu.context_switches` 覆盖两项），中断单独不可用时该帧 `interrupts_per_sec=null`、上下文切换照常。
- Windows >64 逻辑核时 NT 每核查询只覆盖调用线程所在处理器组：序列数与逻辑核数不一致时每核标记不可用（`collect_failed`）、中断累计计数输出 `null`；上下文切换来自系统级查询仍可用。不得用组内数据冒充全系统每核序列。
- 内存与交换来源（#74；实现细节；契约只规定 payload 语义）：Linux 读 `/proc/meminfo`——`MemTotal` / `MemAvailable` / `Buffers` / `Cached` / `SReclaimable` / `SwapTotal` / `SwapFree`；`MemAvailable` 缺失、为 0 或大于总量时回退 `MemFree + Buffers + Cached + SReclaimable` 并钳制到 `[0, total]`，异常与回退记 WARN 日志，回退也无法计算时本次内存采集失败（该域 `null`，可用性粘性）；`cached_bytes = Cached + SReclaimable`，`buffers_bytes = Buffers`，两者均钳制到总量。Windows 读 `GetPerformanceInfo`（`psapi`）：`total = PhysicalTotal × PageSize`、`available = PhysicalAvailable × PageSize`（Windows 上"空闲"与"可用"同义，不单列 `free`）、`cached = SystemCache × PageSize`、`buffers` 不提供；交换为页面文件口径——`total = CommitLimit − PhysicalTotal`、`used = CommitTotal − PhysicalTotal` 并钳制（无页面文件时 `total = 0` → 可用性 `unsupported`）。
- 内存唯一口径：`used_bytes = total_bytes − available_bytes`（Linux 取 `MemAvailable`；Windows 取 `PhysicalAvailable`），不得使用「总量 − 空闲」（ADR 0024）；交换同理 `used = total − free`。
- 卷与共享内存目录来源（#75/#122；实现细节；契约只规定 payload 语义）：Linux 解析 `/proc/mounts`（还原八进制转义的挂载点、过滤伪文件系统与内存文件系统、同挂载点去重），`statvfs` 逐卷取容量；容量读不到（`EACCES`/`EPERM` → `no_permission`，其它 → `collect_failed`）的卷仍进数组、`total_bytes`/`free_bytes` 留 `nullopt`。伪文件系统清单按**精确类型名**匹配（`cgroup` 与 `cgroup2` 各列一条），`fuse.*` 按前缀；#122 把**裸 `fuse`** 一并过滤（`/proc/mounts` 里 GNOME remote desktop 的 `cliprdr-*` 临时挂载 `fstype` 就是 `fuse`，`statvfs` 返回 0 块）——**`fuseblk` 不在清单内**：它由内核 fuse 模块承载块设备上的真实文件系统（典型是 ntfs-3g 挂载的 NTFS 卷）。Windows 用 `GetLogicalDrives` + `GetDriveTypeW` 只取 `DRIVE_FIXED` 固定盘，`GetDiskFreeSpaceExW` 取容量；`ERROR_ACCESS_DENIED` → `no_permission`。卷的监控范围恒为**本机全部卷**（契约 sysmon §2.4），不存在「关注哪一卷」这一层。共享内存目录占用递归统计字节数（含 `meta.dat`）、页文件数（`<数字>.dat`，与 `libs/shm` 的页文件判定同口径）与最旧**页文件**年龄（`meta.dat` 不参与年龄计算——它不会随页清理消失，参与会把"页清理失效"掩盖成"有个旧文件"）；目录不可读返回 `nullopt` 而不是全 0 的对象（0 是"目录为空"这一真实事实）。**无容量挂载的折叠发生在显示层**（#122：`readableVolumes` / `zeroCapacityVolumes` 两个 store computed），采集语义不变——`available=true` + `total_bytes=0` 仍是合法事实，完整保留在 `volumes[]` 里。
- 每物理盘 I/O 来源（#76；实现细节；契约只规定 payload 语义）：Linux 读 `/proc/diskstats`（内核 2.6+ 整盘行格式；4.18+ 的额外 discard/flush 字段不影响前 11 项语义），候选集取 `/sys/block` 的整盘条目（分区天然不在其中），并排除 `loop`/`ram`/`zram`/`dm-`/`md`/`bcache`/`nbd`/`rbd`/`drbd`/`sr`/`sg`/`fd`/`pktcdvd`/`ublk`/`vduse`/`zd` 等虚拟、聚合与光学设备（聚合设备的 I/O 已计在底层物理盘，单列会重复计数）；扇区按内核 ABI 固定 ×512 换算字节，`ms_reading`/`ms_writing`/`io_ticks` 毫秒 ×1000 换算微秒，`ios_in_progress` 作为瞬时在途数。Windows 遍历 `\\.\PhysicalDrive0..32`，对每个盘 `CreateFileW`（0 访问权限、共享读写）+ `IOCTL_DISK_PERFORMANCE` 读 `DISK_PERFORMANCE`：不存在的盘号、`ERROR_INVALID_FUNCTION`/`ERROR_NOT_SUPPORTED`（盘不支持计数器）、`ERROR_ACCESS_DENIED`（无权限）与其它读取失败的盘一律跳过；缓冲区不足时按驱动实际结构扩大重试（上限 1 MiB）。`ReadTime`/`WriteTime`/`IdleTime` 的 100ns 累计值 ÷10 换算微秒；`QueryTime − IdleTime` 的差值作为累计忙时间（`QueryTime` 是查询时刻的系统时间锚点，消费方只用差值）；`QueueDepth` 作为瞬时在途数。`QueryTime` 为 0 说明计数器未填充（diskperf 关闭/驱动不支持），此时其余字段的全 0 不是"读到了 0"，该盘按不可用跳过。采集失败时该盘不进数组；一个盘都读不到时采集结果携带 `no_permission`（Windows 上不可用是常态）。

- 每网卡来源（#77；实现细节；契约只规定 payload 语义）：Linux 解析 `/proc/net/dev` 全部接口行（跳过两行表头；每行 `if: rx_bytes rx_packets rx_errs rx_drop ... tx_bytes tx_packets tx_errs tx_drop ...`），`is_virtual` 由 `/sys/class/net/<if>/device` 是否存在判定——该链接只由硬件设备驱动创建，因此 bond/bridge/vlan/tun/veth/docker0/loopback 等逻辑接口一律判为虚拟，**不用名字前缀白名单**（名字随发行版与容器运行时变化，前缀表必然漏）。Windows 用 `GetIfTable2`（IP Helper）遍历所有接口：`Alias` 作网卡名（转 UTF-8）、`InOctets`/`OutOctets`、包数取 `InUcastPkts + InNUcastPkts` 与 `OutUcastPkts + OutNUcastPkts`（合并单播与组播/广播即契约的"包数"）、`InErrors`/`OutErrors`、`InDiscards`/`OutDiscards`，`is_virtual = !InterfaceAndOperStatusFlags.HardwareInterface`（结构性标志，不是别名推断）。采集结果包含虚拟网卡与回环，**隐藏发生在快照构造阶段**——这样 `hide_virtual` 开关改动不需要重新采集、也不影响速率基线，改开关后立即推送一帧即生效。速率由 `compute_network_rates` 纯函数按 `interface` 匹配基线计算：新出现的网卡速率全 `null`（等同首次采样）、消失的网卡不再输出，因此网卡热插拔/删除**不会让序列错位串行**（不依赖数组下标）；计数器两平台均为 64 位，下降一律视为重置（该帧速率 `null`，下一采样重建基线）。网络采集失败只把该域置 `null`、不累加 `missed_ticks`。

## 快照与可用性

- `DZ_FRAME_RTN_SYSMON_SNAPSHOT`：形态 4 纯上行推送，无 `instance_id`，发送方固定 `dzsysmon`（payload `source` 显式携带来源，通用规则 §5）；dzweb 消费后更新镜像并 WS 广播 `sysmon_snapshot`，REST `GET /api/sysmon/snapshot` 读取镜像（契约 sysmon §5/§6、契约 rest、契约 webui-ws）。两个 sysmon 域均进快照域，WS 建连立即拿到当前值，供顶栏全局使用。
- 类型层真相源：[`sysmon.h`](../../libs/platform/include/dztrader/platform/sysmon.h)（`SysmonSnapshot` 及 `validate`）。`null` **仅允许**出现在契约 §2 标注可空的位置，其余位置出现 `null` 为校验失败；dzweb 侧校验失败记日志并忽略（不更新镜像、不广播）。
- **可用性描述**（关键设计）：每个快照必须携带 `availability`——`platform_description`（Windows 必含 build 号）、`metrics`（固定键集合，每帧全覆盖；`available=false` 时 `reason` 必填）、`semantic_hints`。前端按可用性渲染，**不按 OS 名硬编码分支**；不可用一律显示「不可用」，不显示 0。
- 当前可用性（#77）：`system`、`cpu.total`、`cpu.cores`、`cpu.load_average`、`cpu.context_switches`、`memory`、`swap`、`volumes`、`shm_dir`、`disk_io`、`network` 可用；Linux 上按宿主能力实现 `cpu.frequency`（cpufreq）与 `temperature`（hwmon），Windows 上两者不提供（默认配置关闭对应 B 组开关 → `disabled_by_config`；显式开启后采集器恒报 `unsupported`）；无交换分区/无页面文件时 `swap` 为 `unsupported`。`disk_io` 为能力粘性：单个盘读不到只是不进数组，一个盘都读不到时 Linux 报 `collect_failed`、Windows 报 `no_permission`（性能计数器需特权/未开启，不可用是常态而非异常）。`network` 同样能力粘性：单个网卡读不到只是不进数组，整域读取失败报 `collect_failed`；`hide_virtual` 只影响输出条目、不影响可用性——全部网卡都是虚拟网卡且被隐藏时为**空数组 + `available=true`**（「全部被过滤」与「采集失败」是不同事实）。采集能力不可用时用 `reason="collect_failed"`（温度无传感器用 `no_sensor`、无 cpufreq/无交换用 `unsupported`）；能力可用但本帧数值未就绪时为 `available=true`、数值 `null`；配置关闭对应指标组时为 `available=false` + `reason="disabled_by_config"`。**数值不沿用上一帧**：本帧未采到即输出 `null`/空数组（能力标记粘性），避免把旧数据当当前值展示。
- 渲染语义分两档：内存/交换/磁盘/网络等区块按可用性显示明确空态——不可用时显示「不可用」+ 原因（`swap` 的 `unsupported` 说明为「本机无交换分区/页面文件」、`disk_io` 的 `no_permission` 说明为「磁盘性能计数器不可用（需管理员权限或性能计数器未开启）」），能力可用但本帧无数据时显示「暂无数据」；磁盘 I/O 与网络的派生字段为 `null`（首次采样/新网卡/计数器重置）时逐项显示「不可用」而不是 0；网络区块在「可用但数组为空」时显示「已隐藏全部虚拟网卡」并提供切换入口（与「暂无数据」区分）。温度与每核频率区块**仅在可用时渲染，不可用时整块不出现**（不在前端按平台名分支，只看 `availability.metrics`）。后者避免在 Windows 等平台长期占据版面显示无意义空态。内存的 `used_bytes` / `used_ratio` 由前端按唯一口径重算，保证字节与百分比一致。
- 饱和核判定（契约 sysmon §2.7）：单核使用率连续 `thresholds.cpu_sustain_samples` 次采样 ≥ `thresholds.cpu_sustain_used_pct / 100` 标记饱和，任一采样严格低于阈值即解除；不可用样本不改变计数；核序列变化（热插拔）重置计数。阈值/次数取当前配置，运行期修改经 `SET_SYSMON_CONFIG` 即时生效；`sampling.groups.cpu=false` 时判定暂停（监控页「配置」区可编辑）。
- 跨平台语义差异：Windows 无原生 load average，以处理器队列长度做指数加权移动平均作为代理量（`semantic="processor_queue"`），与 Linux load average 不是同一个量；口径说明由 `availability.semantic_hints["cpu.load_average"]` 承载，前端只按该字段展示、不得同轴比较（不按 OS 名分支）。磁盘 I/O 的 `availability.semantic_hints["disk_io"]` 说明「延迟是每完成请求的平均驻留时间（含排队）、队列深度是采样时刻的在途请求数（不是区间平均）」，两平台同一口径；该提示只在 `disk_io` 能力可用时携带，前端随块展示。

## 配置

- 配置文件：`$DZTRADER_HOME/configs/dzsysmon.json`。进程启动时缺失则**自动生成完整默认值**，零配置可运行；仓库不提交模板文件（AGENTS 门禁 6）。
- 内容：`log`（日志，走《帧契约：日志》的 `SET/RTN_LOG_CONFIG`，不在 sysmon 配置帧 payload 内）与 `sampling` / `disk` / `network` / `history` / `realtime` / `thresholds`（字段、默认值与约束见契约 sysmon §3.1）。消费关系（#81 收口后全部可运行期修改）：
  - `sampling.groups.cpu`（#81）：关闭时跳过 CPU 计时/负载/速率采集与饱和判定，`cpu.total` / `cpu.cores` / `cpu.load_average` / `cpu.context_switches` 标 `disabled_by_config`，重新开启从新基线开始；`cpu.frequency` 由 `sampling.groups.cpu_frequency` 独立控制（#73，Linux 默认开、Windows 默认关）。
  - `sampling.groups.memory` / `sampling.groups.swap`（#74）、`volumes`（#75）、`disk_io`（#76）、`network`（#77）：关闭时跳过对应采集并标 `disabled_by_config`，重新开启从新基线开始，关闭期不冒充速率窗口。
  - `sampling.measure_interval_ms` 与 `realtime.push_interval_sec`：SET 成功后取消 pending wait 并按新间隔重排（即时生效）；`sampling.record_interval_sec` / `sampling.phase_offset_sec`：记录窗口重配，从**下一窗口**生效（进行中窗口按旧宽度落库，避免切碎历史）。
  - `disk.devices`（#81，`disk` 段只含此项）：磁盘 I/O 设备范围——空 = 全部整物理盘；非空按 `disk_io[].device` 精确匹配（Linux 整盘名 / Windows `PhysicalDrive<N>`），匹配不到只是不输出、不是校验错误。过滤在快照构造阶段（不重新采集、不重置速率基线），SET 后立即推送的一帧即体现；历史维度数据与实时快照同口径。
  - `network.hide_virtual`（#77）：快照构造阶段过滤虚拟网卡，SET 后立即推送一帧；历史维度数据与实时快照同口径。
  - `history.enabled`（#81 起支持运行期切换）：true→false 先 flush 当前记录窗口再关库/停落盘线程；false→true 建库并起线程，从空窗口开始且**不写 `restart` 标记**（进程没重启）；建库失败记 ERROR + `NOTIFY_UI`，实时链路继续，生效配置保持 `true`（配置表达意图，失败是运行期降级，不伪造回滚）；后续任意 SET 会重试建库（无需先关再开或重启）。
  - `history.raw_retention_hours` / `max_retention_days` / `max_bytes`：保留策略参数发布给落盘线程，**下一维护周期**生效。
  - `thresholds.*`（#80 原 `alerts` 段，#123 改名）：饱和阈值、内存/交换/卷阈值与 `log_repeat_sec` 重记间隔，每个计量 tick 读取当前值。卷越限只按空闲百分比判定，没有绝对容量形态的阈值。
  - 旧配置迁移：读到旧 `alerts` 段时把可映射的键值搬到 `thresholds`（不可映射的键丢弃）、写回配置文件、记一条日志，**不报错、不阻塞启动**；默认配置生成器直接产出 `thresholds` 形状（契约 sysmon §3.1）。
- 运行期修改：`SET_SYSMON_CONFIG` 为 RFC 7386 merge patch（任意位置 `null` 非法；空对象无操作但仍回 RTN），成功回推全量 `RTN_SYSMON_CONFIG`；失败按四件套处理——回滚旧值 → 日志 → `NOTIFY_UI` error（`popup=true`）→ 回推旧值（契约 sysmon §3.3、通用规则 §8）。
- 生效顺序（契约 sysmon §3.1.1，顺序是语义的一部分）：校验 → 原子持久化（保留 `log`/未知 section）→ 内存镜像替换 → 重排双定时器 → 记录窗口重配 → 保留策略发布 → 历史开关切换 → 回推 RTN → 成功时立即推送一帧快照。因此只影响快照构造的配置（`hide_virtual` / `devices`）在下一个推送周期之前就可见（不必等 5 秒基线）；`thresholds` 的改动在下一次计量 tick 起作用（越限判定与历史记录同频）。
- 配置展示与编辑入口（#81）：监控页「配置」tab 展示当前生效配置（所有已认证用户可读），admin 经表单下发**差异 patch** 并保持 pending 直到 WS `sysmon_config` 回推（REST 200 只表示已写事件通道）；非 admin 只读。读取镜像配置也可用 REST `GET /api/sysmon/config`（未就绪 503）。
- 触发场景：初始化完成主动 RTN 一次；每次 SET 后回推；`QUERY_FULL_SNAPSHOT` 响应时回推（契约 sysmon §3.2）。

## 平台基线

| 平台 | 基线 |
|---|---|
| Windows | x86_64；NT 10.0 及以上（Windows 10 build 10240 / Server 2016 build 14393）；真实版本经 `RtlGetVersion` 取得 |
| Linux | x86_64；内核 ≥ 4.18、glibc ≥ 2.28（RHEL 8+ / Debian 10+ / Ubuntu 20.04+）；GCC 13 工具链 |

- 编译期用 `#error`（非目标平台 / 32 位 / Linux 工具链与 glibc 下限）拒绝构建；启动期 `check_platform_baseline()` 复核运行期基线（Windows NT 版本、Linux 内核/glibc/架构），失败记录错误并以非 0 退出。Windows 的 NT 版本无法在编译期可靠断言（构建侧 `WINVER`/`_WIN32_WINNT` 是 API 可用性声明，不是运行期判据），由启动期检查承担。禁止静默回退（AGENTS 跨平台门禁）。
- 真相源：[`platform_requirements.h`](../../libs/platform/include/dztrader/platform/platform_requirements.h)。

## 历史写入与保留（#78）

### 存储层

- **共享模块** [`libs/sysmonstore`](../../libs/sysmonstore/)：历史库 schema 的**唯一**定义 + 行编解码 + 写入方/只读读取方门面。dzsysmon 与 dzweb 共用它——schema 只此一份，不存在第二份建表语句。
- 库文件 `$DZTRADER_HOME/db/sysmon.db`，与交易库（`td.db`）、WebUI 库（`webui.db`）分离。WAL + `synchronous=FULL`（历史写入是低频批量，不在交易路径上，抗断电优先）；**dzsysmon 是唯一写者**，dzweb 走只读连接。
- 表结构分两类（快照形状一次定稿，见[契约 §2](../frame_contracts/sysmon.md)）：
  - **宽表** `sysmon_scalar`：固定标量指标，一行一个时间桶，每指标 5 列（`_min` / `_max` / `_avg` / `_last` / `_n`）。复合主键 `(level, bucket_ts)` + `WITHOUT ROWID`：原始记录与分钟聚合同表共存、互不覆盖，按主键范围扫描即时间范围查询。
  - **长表** `sysmon_dimension`：按维度展开（CPU 核 / 卷 / 物理盘 / 网卡 / 温度传感器），一行一个 `(桶, 域, 维度, 指标)`。维度元数可变（核数、盘数、网卡数随机器与热插拔变化），固定列会把表结构钉死在某一台机器上；CPU 核序号不保证连续，因此维度标识是**不透明文本**。
  - **标记表** `sysmon_mark`：采集中断 / 重启 / 时钟跳变，与数据分表存放（避免「有标记的桶」被误当正常数据）。
- `_n` 是该指标在桶内的**有效样本数**：压缩时 `avg` 按 `n` 加权合并，否则「部分样本不可用」的桶会把均值算偏。`avg` 列恒为 `REAL`（整型指标的窗口均值几乎必为小数，`INTEGER` 亲和性会截断）。

### 聚合（`HistoryRecorder`）

- 记录窗口 = `sampling.record_interval_sec`（默认 60s）+ `sampling.phase_offset_sec`（默认 3s）。桶边界按**墙钟**对齐（使历史能与交易事件对照），相位偏移让 tick 避开整点附近的其它任务（K 线切分、页清理、预加载）——默认配置下桶起点是每分钟第 3 秒，**不落在整数分钟边界**。
- **每个计量 tick 都观测一次**（不是只观测推送时刻）：记录间隔与推送间隔无关，只观测推送时刻会让窗口内 11/12 的样本丢失，尖峰被系统性漏掉。聚合为纯逻辑类，不碰 I/O、不读时钟，便于确定性测试。
- `null` 值**不观测**：`cpu.total.usage_ratio = null`（首次采样）或整个域为 `null`（本帧未采集）时跳过，因此该指标在桶内可能 `count = 0`（落库 NULL，读取方显示「不可用」而不是 0）。用 0 填充会把均值拉偏，且 0 是「看起来正常」的假数据。

### 缺口与跳变标记（绝不伪造连续数据）

- **窗口样本不足 / 错过 tick**：满窗应有 `interval_sec * 1000 / measure_interval_ms` 个样本（默认 60）。以下任一成立即写 `sample_gap`：实际样本数不足（进程停顿、被强杀），或计量定时器明确报告跳过了 tick（`sampling.missed_ticks` 的增量，由 `note_missed_ticks` 注入）。`detail` 为 `<实际样本数>/<明确跳过的 tick 数>`。允许 ±1 的调度抖动（定时器按「上一 deadline + 间隔」推进，边界样本归属可能相差一个 tick）。
- **时钟跳变**：采样定时器走单调时钟，墙钟与单调时钟的推进差异超过 `kClockJumpThresholdMs`（5s）即判定 NTP 步进/回拨/虚拟机恢复，写 `clock_jump` 标记（`detail` 为带符号毫秒数）。不标记会让读取方把 NTP 步进误读成负载突变。
- **进程重启**：`init_history` 成功后立即写 `restart` 标记（`detail` 为 `recover|manual`，能算出缺口时附 `/<缺口秒数>`——用只读连接查库中最后一条记录的时刻，查不到就不写数字）。恢复参数语义是**历史连续性优先**——不重置数据文件、不丢已有聚合，重启窗口标记为采集中断。
- 标记与它所描述的数据**同批次提交**（同一事务），不存在「标记已落库、对应窗口还没落」的中间状态。

### 保留与压缩

| 阶段 | 规则 |
|---|---|
| 压缩 | `record_interval_sec < 60` 时，超过 `raw_retention_hours` 的原始桶按墙钟整分钟聚合成 `level=1` 分钟桶；记录间隔已 ≥ 60 时**不压缩**（原始记录本就是分钟级或更粗，压不出更细的粒度） |
| 删除 | 原始按 `raw_retention_hours`（默认 24h）、分钟聚合与标记按 `max_retention_days`（默认 30 天） |
| 总量上限 | 库的**逻辑占用**（已用页 × 页大小，不含 freelist）超过 `max_bytes` 时按 `bucket_ts` 从**全库最旧**的桶开始删（跨层级统一排序，不按层级固定先后）；删够后 `VACUUM` 把 freelist 还给操作系统。用逻辑占用而非文件大小判断：`DELETE` 不缩文件，按文件大小会「删光数据仍超限」 |

- 压缩跨维护周期分批进行时，新聚合必须与**已有分钟桶合并**（`MetricAggregate::merge` 按 `n` 加权），直接覆盖会丢掉前一批的 `min`/`max`/`n`。
- 维护按 `maintenance_batch` 分段、每段一个事务：避免一次长事务把只读连接（dzweb 查询）挡住整个维护周期。压缩只删**本批真正压过的行**（标量与维度的输入各自 LIMIT，按桶整删会连带删掉本批未压缩的维度行 = 静默丢历史）；`compacted_buckets` 按整轮维护的分钟桶去重计数。
- 删除宽表与长表**同桶**数据必须一起做，否则按域查询会读到孤儿行。
- **已删除的维度指标遗留行**（#124；决策与「不提升 `kSchemaVersion` / 不做主动清理」的理由见 [ADR 0034](../adr/0034-sysmon-resource-thresholds-log-only.md)）：已落库的旧行去向随记录间隔而异——记录间隔 ≥ 60 秒（默认，不压缩）时随 `max_retention_days`（30 天）总保留期按 `bucket_ts` 删除；< 60 秒（压缩分支）时压缩先聚合、再删原始行，而 `write_dimension_row` 对未注册指标记一条 `reason=not_registered` warning 并跳过，旧行不会被压成分钟聚合，在 `raw_retention_hours`（24 小时）原始水位即删除。两条路径都不留永久孤儿行（`resolve_metric` 对未注册指标返回 invalid，前端从不查询）。
- 维护周期由落盘线程执行（`maintenance_interval_sec`，默认 300s），配置项在运行期修改后**下一个周期即按新值执行**。保留参数通过互斥保护的小快照从 io_context 线程发布给落盘线程，落盘线程**不直接读**主配置对象（那是跨线程数据竞争）。

### 落盘线程（`HistoryWriter`）

- 采集线程（io_context）只调 `enqueue()`：**不阻塞、不抛异常**。数据库提交与 fsync 在磁盘繁忙时可阻塞数十至数百毫秒，放在 io_context 上会同时拖住采集（历史出现缺口）与帧处理（优雅退出请求无法及时响应 ⇒ 被主进程强杀 ⇒ 重启循环）。
- 有界队列 + 批量提交：落盘线程一次取走整批、合并成**一个事务**写入（单事务上限 `max_batch_records`，超出留给下一轮，避免长事务占写锁）。队列满时丢**最旧**批次并计数（丢旧优于丢新：新数据还能解释刚刚发生了什么，缺口会由维护周期记成标记）。
- 落盘失败**不杀进程**：历史是观测辅助，采集与实时推送必须继续；错误记 ERROR 并计入 `failed_records`（与「队列满丢弃」的 `dropped_records` 分开计：前者说明磁盘/库有问题，后者说明消费跟不上）。
- 优雅退出顺序（`shutdown_history`）：先把当前未结束窗口的样本交给落盘线程（不丢「退出前发生了什么」），再 `flush()` 冲空队列 → WAL checkpoint → 关库。

### 配置与降级

- `history.enabled = false`：完全不碰库（不建文件、不起线程），仅实时数据。
- **`history.enabled` 支持运行期切换**（#81）：true→false 先 flush 当前记录窗口、再冲空队列并关库/停落盘线程（与优雅退出同一条 `shutdown_history` 路径，io 线程会短暂等待落盘线程 join——配置切换是低频人工操作，可接受）；false→true 重新建库并起线程，保留策略参数同时发布。运行期开启**不写 `restart` 标记**（进程没有重启）；关闭后重新开启从空窗口开始（关闭期间没有数据，不伪造连续）。建库失败只记 ERROR + `NOTIFY_UI`，实时链路继续，生效配置保持 `true`；后续任意 SET 会重试建库（无需先关再开或重启）。关闭时同时重置时钟跳变检测基线，避免把关闭时长误报成 `clock_jump`。其余 `history.*` 参数（保留时长、上限）**运行期即时生效**（下一维护周期）。
- 库打开失败（磁盘满、权限、版本不符）**不阻止进程启动**：记 ERROR + `NOTIFY_UI`，降级为「不写历史」。7×24 进程不应因观测辅助链路起不来。
- schema 版本比当前新时写入方拒绝打开（旧写入方不得覆盖新结构）；读取方在库缺失 / 版本不符 / 文件损坏时降级为「历史不可用」（见 #79）。

## 历史查询（#79）

查询语义与帧无关：`dzweb` 以只读连接直接打开历史库，`dzsysmon` 侧**不需要任何改动**（唯一写者的角色不变）。契约见[《帧契约：系统监控》](../frame_contracts/sysmon.md) §6.1，此处只写实现结构与该契约不规定但写错代价高的部分。

### 分层

| 层 | 位置 | 职责 |
|---|---|---|
| 查询语义 | `libs/sysmonstore/history_query.{h,cpp}` | 指标键解析、层级选择、点数上限裁剪、缺口归并——全部是纯函数 |
| 行读取 | `HistoryReader`（`history_store.h`） | 只读连接 + 升序/降序查询；降级判定（`HistoryAvailability`） |
| HTTP 装配 | `apps/webui/sysmon_history_service.{h,cpp}` | query 参数解析/校验、序列化成 REST JSON |
| 端点 | `SysmonCtrl::get_history`（`sysmon_controller.cpp`） | 400 / 200 语义、注入历史库路径 |

**为什么查询语义放进共享模块**：这些规则全部由 schema 的形状决定（哪些逻辑键存在、宽表与长表怎么区分、桶宽从哪来）。放进 `dzweb` 会让 schema 的知识出现第二份，与「schema 与行编解码只有一份定义」（ADR 0033）冲突。

### 精确截断（一个容易写错的点）

点数上限要求「保留最新 N 个桶」。判定「是否还有更旧的桶」**必须由实际取回的行数决定**：读取方按时间**降序**取 `max_points + 1` 行，多取的那一行就是证据。

按「范围宽度 ÷ 桶宽」估算会误判两类情况，且两种都会丢掉本可完整返回的数据：
- 起点不与桶边界对齐（例如请求 `from` 落在分钟中间）；
- 该段时间本来就没有数据（机器停机、历史被压缩走）。

因此 `HistoryReader` 提供成对的升序/降序查询：升序给既有消费方（重启缺口查询等），降序专供 `max_points` 裁剪。

### 缺口归并的依据

`gaps[]` 只看**相邻桶时刻的连续性**（`cur.ts − (prev.ts + interval_sec) > 0`），标记只用来解释原因。这样「有标记但数据连续」（时钟跳变）与「数据断了但没有标记」（`unmarked`）都不被掩盖——后者恰恰是最需要暴露的异常，绝不能默默当连续序列。

### 降级的三条路径

读取方把三种「历史不可用」区分开（`HistoryAvailability`）：`not_found`（库不存在 = 历史未启用）、`schema_mismatch`（版本不符）、`open_failed`（损坏/权限）。

**`sqlite3_open` 是惰性的**：对非 SQLite 文件它照样返回成功，失败只在实际读页时出现。若不加区分，「库文件是垃圾」会被「查 `sysmon_meta` 抛异常 ⇒ 无表 ⇒ 未初始化」的路径错报成 `not_found`，让使用者以为「历史没开」而不是「库坏了」。因此读取方：
1. 打开前先验 16 字节 SQLite 文件头（`has_valid_sqlite_header`）——不是 SQLite 直接 `open_failed`；
2. 打开后用 `database_is_readable`（查 `sqlite_master`）确认库结构可读——读不了是 `open_failed`；
3. 最后才查 `sysmon_meta` 的版本——表/行不存在才是 `not_found`。

0 字节文件是合法的空库（SQLite 首次写入才建头），走第 3 步报 `not_found`。

### 已知限制

- 逐请求打开只读连接（`HistoryReader` 非线程安全，而 REST 回调在单 IO 线程串行）。历史查询是低频人工操作，连接开销可忽略；确需优化时按线程缓存连接，不要跨线程共享。
- 每次查询返回**单条**序列：维度指标需显式给 `dimension`，不提供「全部维度叠加」。
- 不做跨桶重采样：点数超限时前移窗口并回 `truncated=true`（契约 §6.1.2），不在服务端合并桶或插值。

## 阈值越限与日志

帧语义与越限/恢复条件见[契约 sysmon §2.8](../frame_contracts/sysmon.md)；本节只写实现结构与该契约不规定但写错代价高的部分。资源越限不产生 `NOTIFY_UI`，改为日志 + UI 色标（#80 实现、#123 改造，ADR 0034）。

### 分层

| 层 | 位置 | 职责 |
|---|---|---|
| 判定状态机 | `apps/sysmon/thresholds.{h,cpp}`（`ThresholdEvaluator`） | 进入 / 周期重记 / 恢复；纯逻辑，注入墙钟与单调时钟，不读全局配置、不写帧、不写日志 |
| 输入构造 | 同文件 `make_threshold_inputs` | 把一次计量的 CPU 饱和/内存/交换/全部卷事实转成判定输入 |
| 日志记录 | `main.cpp` `evaluate_thresholds()` | 把纯函数产出的「待写日志记录」（级别、判定对象、状态、当前值、阈值、活动期持续秒数）落成日志 |

**为什么先构造输入再做判定**：「`used = total − available`」口径与「全部卷逐卷一条判定对象」都属于输入转换；把它做成纯函数后，`thresholds_test` 可以用假采集结果确定性验证边界语义（等于阈值算越限、`total_bytes = 0` 的卷不参与判定、读不到不等于恢复），而不必起真实进程或真实磁盘。

### 状态机语义（容易写错的点）

- 每个判定对象维护**活动期**：进入时记 `since`（墙钟），恢复时结束；`volume_free` 逐卷一条（按挂载点区分），其余每类一个。
- **进入与恢复各记一条**（级别与内容义务见契约 sysmon §2.8）——一次活动期有明确起点与终点，事后可从日志判断它何时开始、何时结束。
- **持续期按 `thresholds.log_repeat_sec` 重记**（含活动期已持续秒数；`0` 表示不重记）。重记计时用**单调时钟**（NTP 步进不影响节奏），基准是该活动期的进入时刻：里程碑恒为 `进入时刻 + N × 间隔`，中间漏掉的里程碑不补发（一条「仍越限」已足够表达"它还在持续"）；运行期改动间隔后按新间隔重排，后续里程碑仍落在 `进入时刻 + N × 新间隔` 上。`since` 用墙钟，写在**进入**那条日志里——UI 不再呈现"高水位始于何时"（ADR 0034），这条日志是唯一能回答它的地方。
- **"能力被配置关闭 / 判定对象消失"不是恢复**：对应指标类别被配置关闭（`disabled_by_config`）、交换分区被移除、某卷从 `volumes[]` 消失时，静默清除活动状态、**不写恢复日志**（清除原因不是"指标回到阈值以下"）。
- 本帧读不到数值（能力仍在，`used_ratio = null` 或整个指标域为 `null`）→ **保留**活动状态、不写恢复日志——"读不到"不是"已恢复"。跨越采集缺口后真正恢复时，恢复日志的持续秒数按活动期起点计算，如实反映总时长。
- **卷的判定范围恒为本机全部卷**：逐卷一条判定对象，不存在「关注哪一卷」这一层；`total_bytes = 0` 的卷无法计算百分比，不参与判定。卷从 `volumes[]` 消失（卸载/热拔）时静默清除；卷域整体读不到时**不**清（无法区分"消失"与"暂时枚举不到"）。
- **`cpu_saturated` 不按「空集」判恢复**：每核序列整体未采到时 `saturated` 全部为 false（契约 §2.7），那只是"本帧无信息"；只有本帧每核使用率确实可读且没有饱和核才算恢复。
- 日志文本是英文、小写开头、无句号、`prose | k=v`（AGENTS 日志门禁），不构成协议语义，消费方不得解析。

### 与 `NOTIFY_UI` 的边界

- 四类资源判定（内存使用率、交换使用率、卷剩余空间、CPU 饱和）**不产生任何 `NOTIFY_UI`**。`dzsysmon` 的 `NOTIFY_UI` 此后只剩真故障：历史库不可用与配置下发失败——两者都是一次性、需人处理的故障，级别与弹窗语义按契约 notify-ui。
- **不按判定对象额外限流**：防刷屏完全由「进入/恢复各一条 + 按 `log_repeat_sec` 重记」保证——同一活动期内不会重复记进入，也不会连记多条恢复。
- 不写历史库、不进快照：历史只记录指标事实，越限可由历史事实重新推导；活动状态不跨帧可见，只有日志与 UI 色标。
- 默认阈值（内存 90%、交换 50%、卷空闲空间低于 10%、CPU 单核连续 10 次采样 ≥ 90%）足以零配置工作；全部阈值与 `log_repeat_sec` 取自当前生效配置 `thresholds`，运行期修改经 `SET_SYSMON_CONFIG` 即时生效（监控页「配置」tab 提供 admin 编辑入口）。

### 已知限制

- 资源高水位没有「始于何时」的 UI 呈现：需要时查日志（`logs/dzsysmon_<date>.log`），持续时长由间隔重记的条目以及恢复日志的持续秒数推断。这是有意取舍（ADR 0034）。
- 日志是唯一的状态载体：日志被清空或超出保留期后，无法再还原某段活动期的起止时刻（快照不携带活动状态，前端也没有可恢复的副本）。

## 第三方归属

- 采集的规格与算法参考 psutil（BSD-3-Clause）：只用于确定「该调哪个 OS API、必须处理哪些边界」与测试不变量（ADR 0024）；不翻译其 C 代码、不嵌入 CPython 运行时、不引入第三方系统信息库。
- 归属声明与 BSD-3 全文见仓库根 [THIRD_PARTY_NOTICES.md](../../THIRD_PARTY_NOTICES.md)；文档、UI 与发布物不得出现暗示 psutil 背书的措辞。

## 部署限制：不支持容器内采集

- 容器内 `/proc` 报告的是宿主容量（cgroup 配额不会反映到这些接口），会给出错误的容量与使用率基线；本版本不做 cgroup v1/v2 感知。
- 因此 `dzsysmon` 必须运行在宿主机上；需要监控容器所在机器时，在宿主机部署本进程（契约 sysmon §8 明确本条）。

## 已知限制

- 磁盘 I/O 为**整盘粒度**：不提供分区级 I/O；Windows 的 `device` 只有 `PhysicalDrive<N>`。**卷/盘符 ↔ 物理盘映射**（ADR 0024 Consequences 要求，Windows 走 `IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS`）由后续票 [#103](https://github.com/ockham001/dztrader/issues/103) 提供，不在本票范围。
- Windows 磁盘性能计数器依赖 diskperf 已开启与访问权限，读不到时整域报不可用（`no_permission`）而不是 0；不做提权或反复重试。
- Windows 物理盘编号按 `0..32` 枚举（沿用参考实现口径），超过 33 块盘的机器会被截断。
- 网络为**整网卡粒度**：不提供每进程/每连接/每协议流量。`is_virtual` 只用结构性依据（Linux `device` 链接、Windows `HardwareInterface`），因此硬件网卡上的 VLAN 子接口、网卡绑定（bond/team）逻辑接口会被判为虚拟并在默认配置下隐藏；排查这类拓扑时用「显示全部网卡」开关。
- Windows 上 `GetIfTable2` 的 `Alias` 是用户可重命名的适配器别名：网卡在帧间的身份依赖该别名稳定（改名会让该网卡在下一帧被当作新网卡、速率一帧为 `null`），不做「按 ifIndex 追踪」的额外映射。

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
