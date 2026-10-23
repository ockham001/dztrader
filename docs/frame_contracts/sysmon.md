# 帧契约：系统监控

本文件覆盖 `DZ_FRAME_RTN_SYSMON_SNAPSHOT`、`DZ_FRAME_SET_SYSMON_CONFIG`、`DZ_FRAME_RTN_SYSMON_CONFIG` 三个帧。总则见《帧契约：通用规则》。

发送方固定为内部进程 `dzsysmon`（进程名即 `instance_id`，见总则 §5）。告警复用既有 `DZ_FRAME_NOTIFY_UI`（契约 notify-ui），**不新增通知帧**；历史查询**不走帧**（dzweb 以只读方式直接打开历史库，schema 由历史模块承载）。

类型层真相源：`libs/platform/include/dztrader/platform/sysmon.h`（`SysmonSnapshot`/`SysmonConfig` 及校验）；采集侧 `SystemCollector` 接缝见 `libs/platform/include/dztrader/platform/system_collector.h`。

> 参考实现 psutil（BSD-3）仅作规格与算法参考（该调哪个 OS API、必须处理哪些边界），不翻译其代码、不嵌入 CPython 运行时；归属声明见仓库根 `THIRD_PARTY_NOTICES.md`。本契约与实现不得出现暗示 psutil 背书的措辞。

---

## 1. 帧一览

| 帧名 | 形态（总则 §4.2） | 路由 | Payload |
|------|------------------|------|---------|
| `DZ_FRAME_RTN_SYSMON_SNAPSHOT` | 形态 4 纯上行推送 | 无 `instance_id`；发送方固定 `dzsysmon` | 快照（本文件 §2） |
| `DZ_FRAME_SET_SYSMON_CONFIG` | 形态 1 请求 | `instance_id = dzsysmon` | 配置 merge patch（本文件 §3） |
| `DZ_FRAME_RTN_SYSMON_CONFIG` | 形态 1 响应 / 形态 4 自发 | `instance_id = dzsysmon` | 全量生效配置（本文件 §3） |

三帧 payload 均为 JSON。帧头使用 `DzExtFrameHeader`（快照）或 `DzExtInstFrameHeader`（SET/RTN 配置），布局见总则 §2。

---

## 2. 快照（RTN_SYSMON_SNAPSHOT）

**语义**：推送当前实时快照。当前版本（#76）填系统信息与 CPU 区块（总量、每核与饱和标记、系统负载、上下文切换/中断速率、每核频率）、温度、内存与交换、卷容量与关注路径落卷、共享内存目录占用、每物理盘 I/O；网络域按本文档定稿的形状输出 `null` 并在可用性映射中标记不可用。已列出的指标域后续只填值、**不改形状**（新增指标类别属 additive 扩展，按总则 §11.3 同步契约、类型与前端 schema；消费方忽略未知字段）。

**来源识别**：帧头无 `instance_id`（形态 4、发送方固定 `dzsysmon`），按总则 §5 在 payload 中显式携带 `source`。

**数据流**：形态 4——`dzsysmon` → 事件通道 → dzweb 事件监听 → 领域服务更新镜像 + WS 广播 `sysmon_snapshot`。无前端入口。

**Payload（SysmonSnapshot）**：

| 字段 | 类型 | 说明 |
|------|------|------|
| `source` | string | 发送方进程名，固定 `"dzsysmon"`（无 instance_id 帧来源显式携带，总则 §5） |
| `timestamp` | int | 快照生成时刻（Unix 毫秒） |
| `sampling` | object | 采样元信息（见下） |
| `platform` | object | 平台信息（见下） |
| `system` | object | 系统信息（见下） |
| `cpu` | object | CPU 区块（见下） |
| `temperature` | array \| null | 温度传感器读数；`null` = 本平台不提供/无传感器/不可用（见 §2.4、可用性映射） |
| `memory` | object \| null | 内存（见 §2.4）；`null` = 本帧未采集/不可用（见可用性映射） |
| `swap` | object \| null | 交换分区/页文件（见 §2.4）；无交换时为 `total_bytes = 0` 的对象，读取失败可为 `null` |
| `volumes` | array \| null | 卷容量与可用性（见 §2.4） |
| `watch_paths` | array \| null | 关注路径（共享内存目录 / 数据库目录）的落卷解析（见 §2.4） |
| `disk_io` | array \| null | 每物理盘 I/O |
| `network` | array \| null | 每网卡收发 |
| `shm_dir` | object \| null | 共享内存目录占用（见 §2.4） |
| `availability` | object | 可用性描述（见下），**每个快照必须携带** |

### 2.1 sampling

| 字段 | 类型 | 说明 |
|------|------|------|
| `measure_interval_ms` | int | 当前计量间隔（毫秒）；基线 1000 |
| `last_sample_unix_ms` | int | 最近一次计量成功的墙钟时间（Unix 毫秒）；0 = 尚无成功样本 |
| `missed_ticks` | int | 累计错过的计量 tick 数（缺口标记，单调不减）；用于区分"机器空闲"与"监控停顿" |

### 2.2 platform 与 system

`platform`：

| 字段 | 类型 | 说明 |
|------|------|------|
| `family` | string | `"windows"` / `"linux"` |
| `version` | string | 真实 OS 版本（Windows 不得被 manifest 兼容性垫片欺骗；Linux 取发行版版本） |
| `kernel` | string | Linux 内核 release；Windows 为 NT 版本号（如 `"10.0.19045"`） |
| `arch` | string | 架构标识（`"x86_64"`） |
| `build` | int | Windows build 号（区分 Windows 10 与 11：两者都报 NT 10.0）；Linux 为 0 |

`system`：

| 字段 | 类型 | 说明 |
|------|------|------|
| `hostname` | string | 主机名（非空） |
| `cpu_model` | string | CPU 型号；取不到时为空串 |
| `logical_cores` | int | 逻辑核数（> 0）；每核序列可用时等于 `cpu.cores` 长度（热插拔/虚拟机缩放后以本次序列为准） |
| `boot_time_unix_sec` | int | 启动时间（Unix 秒） |
| `uptime_sec` | int | 运行时长（秒），由快照生成时刻与启动时间计算 |

### 2.3 cpu

| 字段 | 类型 | 说明 |
|------|------|------|
| `total.usage_ratio` | number \| null | 总使用率 `[0,1]`；`null` = 首次采样/计数器不可用（不得用 0 冒充） |
| `cores` | array | 每核使用率（不可用时为空数组）：`{index:int, usage_ratio:number\|null, saturated:bool}`。`index` 为 OS CPU 编号（Linux `cpuN` 的 N；Windows 处理器序号），**不保证连续**；可用时数组长度 == `system.logical_cores` |
| `core_frequencies` | array \| null | 每核当前频率：`{index:int, mhz:int}`；`index` 与 `cores[].index` 同源（OS CPU 编号，不保证连续），`mhz` 为四舍五入到整数的 MHz（> 0）。`null` = 本平台不提供/无 cpufreq/不可用（Windows 恒为 `null`）；读取失败的单个核不进入数组 |
| `saturated_cores` | array\<int\> | 当前处于持续饱和状态的核 index 集合（判定规则见 §2.7） |
| `load_average` | object \| null | `{one,five,fifteen: number\|null, semantic: string}`。Linux：`semantic="load_average"`，三值为真实 1/5/15 分钟负载；Windows：`semantic="processor_queue"`，以处理器队列长度做指数加权移动平均后填三值（均为代理量）。**两者不是同一个量，前端不得同轴比较**；口径说明由 `availability.semantic_hints["cpu.load_average"]` 承载 |
| `context_switches_per_sec` | number \| null | 每秒上下文切换：由累计计数器差值与实际采样间隔计算（§2.7）；首次采样为 null |
| `interrupts_per_sec` | number \| null | 每秒中断：同上下文切换口径；首次采样为 null |

### 2.4 其余指标域

`memory`：`{total_bytes:int, available_bytes:int, used_bytes:int, used_ratio:number|null, cached_bytes:int|null, buffers_bytes:int|null}`。
**唯一口径：`used_bytes = total_bytes − available_bytes`**（不得使用"总量 − 空闲"）；`used_ratio = used_bytes / total_bytes`，`total_bytes = 0` 时为 `null`（总量为 0 视为本次采集失败，不产出该对象）。
`cached_bytes` 为可回收文件页缓存（Linux `Cached + SReclaimable`；Windows `SystemCache`），`buffers_bytes` 为内核文件系统元数据缓存（仅 Linux `Buffers`；Windows 不提供，恒为 `null`）。两者是信息项，**不参与** `used_bytes` 口径，也不得用于反推可用量；字段为 `null` = 本平台不提供/本帧未读到。
Windows 上"空闲"与"可用"是同一个量（`PhysicalAvailable`），不得与 Linux `MemFree` 混用：快照**不单列** `free`，`available_bytes` 在 Windows 即系统报告的可用物理内存。

`swap`：`{total_bytes:int, used_bytes:int, used_ratio:number|null}`。无交换分区/无页面文件时 `total_bytes = 0`、`used_bytes = 0`、`used_ratio = null`，可用性映射标记不可用；读取失败时该对象也可以为 `null`（见可用性映射的 `collect_failed`）。**Windows 口径**：总量 = 由页面文件支撑的提交上限（`CommitLimit − PhysicalTotal`），已用 = 物理内存之外已提交的量（`CommitTotal − PhysicalTotal`，钳制到 `[0, total]`）；语义是"页面文件支撑的虚拟内存"，不是原始页面文件大小。

`volumes[]`：`{mount:string, watched:bool, available:bool, total_bytes:int|null, used_bytes:int|null, used_ratio:number|null, reason?:string}`。

- `mount` 为规范化挂载点：Linux 取挂载目录（`/`、`/home`），Windows 取盘符根（`C:\`）。同一帧内唯一。
- `watched` 表示该卷承载了任一关注路径（§2.4 `watch_paths`），消费方据此高亮。
- `available=false`（`reason` 必填）表示**卷存在但容量读不到**（无权限、卷不可访问）。此时 `total_bytes`/`used_bytes`/`used_ratio` **必须为 `null`**，不得用 0 冒充；卷仍然进数组，因为"卷存在但读不到"与"根本没有这个卷"是不同事实，消费方都要能显示。
- `available=true` 时 `total_bytes`/`used_bytes` 为非负整数且 `used_bytes ≤ total_bytes`；`used_ratio = used_bytes / total_bytes`，`total_bytes = 0`（异常/特殊卷）时为 `null`。
- 采集范围：Linux 过滤伪文件系统（`proc`/`sysfs`/`devtmpfs`/`devpts`/`tmpfs`/`cgroup*`/`debugfs`/`tracefs`/`configfs`/`mqueue`/`hugetlbfs`/`autofs`/`binfmt_misc`/`nsfs`/`pstore`/`securityfs`/`fusectl`/`bpf` 等），同一挂载点只保留一条；Windows 覆盖固定盘（`GetDriveType` = `DRIVE_FIXED`）。

`watch_paths[]`：`{path:string, mount:string|null, available:bool, reason?:string}`。关注路径为默认的共享内存目录与数据库目录（配置 `disk.watch_paths` 非空时以其为准，见 §3.1）。

- `path` 为解析用的路径（绝对路径，符号链接已跟随）；`mount` 为其所在卷的挂载点，与同帧 `volumes[].mount` 可直接相等比较；`available=false` 时 `mount = null` 且 `reason` 必填。
- 解析义务：先规范化路径（`weakly_canonical`：解析 `.`/`..` 与符号链接，保留不存在的尾部用于诊断），再对卷挂载点做最长前缀匹配；Windows 匹配大小写不敏感、分隔符统一（`/` 与 `\` 等价），Linux 大小写敏感。
- 路径不存在或解析不到任何卷 ⇒ `available=false` + `reason`（`path_not_found`）；权限不足 ⇒ `no_permission`；路径所在卷不在采集范围（网络盘、可移动盘、光驱等非固定盘）⇒ `unsupported`；其它解析失败 ⇒ `collect_failed`。**不得**在解析失败时猜一个卷或回退到 `/`。

`disk_io[]`：每物理盘 I/O 派生值。字段为 `{device:string, read_bytes_per_sec, write_bytes_per_sec, read_iops, write_iops, latency_ms, queue_depth, utilization}`；除 `device` 外均为 `number | null`——**计数器不可用时为 `null` 而非 0**（契约 §2.7）。

- `device` 为设备稳定标识：Linux 为内核块设备名（`sda` / `nvme0n1` 等**整盘**名，不含分区）；Windows 为 `PhysicalDrive<N>`（物理盘编号）。帧内唯一，可在帧间变化（热插拔/盘号增减）。
- 采集范围只含**整块物理盘**：Linux 以 `/sys/block` 的整盘条目为准（分区天然不在其中），并排除 loop/ram/zram 内存与回环设备、`dm-*`/`md*`/bcache 聚合设备（其 I/O 已计入底层物理盘，单列会重复计数）与 `nbd`/`rbd`/`drbd` 网络块设备、`sr`/`sg`/`fd`/`pktcdvd` 光学/软驱/包写入等非存储设备；Windows 遍历 `PhysicalDrive0..PhysicalDrive32`，不存在的盘号与不支持计数器的盘（含无权限）跳过。**盘符/卷 × 物理盘的映射不在本版本范围**（Windows `device` 只有物理盘编号，见 §8）。
- `read_bytes_per_sec` / `write_bytes_per_sec`：吞吐（字节/秒）；`read_iops` / `write_iops`：每秒完成的读/写请求数。四者 = Δ累计计数器 / Δ实际经过秒数（§2.7）。
- `latency_ms`：**平均每完成请求的驻留时间（含排队）**，单位毫秒 = Δ(读服务时间 + 写服务时间) / Δ(读完成数 + 写完成数)。窗口内没有完成的请求时为 `null`（无定义，不是 0）。
- `queue_depth`：**采样时刻的在途请求数**（瞬时 gauge，如 Linux `ios_in_progress` / Windows `QueueDepth`），**不是区间平均队列深度**；平台不提供该读数时为 `null`。
- `utilization`：设备忙时间占比 = Δ忙时间 / Δt，钳制到 `[0,1]`；平台不提供忙时间、或 Δ忙时间超过计量窗口容忍上限（计数器口径异常，如系统时间跳变）时为 `null`（不钳成 100% 冒充满载）。
- 首帧（无基线）**全部派生字段为 `null`**（含瞬时 gauge `queue_depth`：首次采样整块不可用，不得用 0 冒充）。本帧新出现的设备同样在没有基线时除 `device` 外全部 `null`；消失的设备不再输出。
- 采集失败与不可用：Linux `/proc/diskstats` 或 `/sys/block` 不可读 ⇒ 整域 `null` + `collect_failed`；Windows 一个盘的性能计数器都读不到（无管理员权限、性能计数器未开启或盘不支持）⇒ 整域 `null` + `no_permission`（`no_permission` 的既有语义即"计数器需特权/未开启"，**Windows 上不可用是常态而非异常**）。单个盘读不到只是不进入数组，不改写整体可用性；空数组 = 采集成功但本机无物理盘（仅 Linux 会出现；Windows 上 `CreateFile` 对"盘不存在"与"无权限"都返回同一个失败，无法区分，故恒以 `null` + `no_permission` 表示不可用）。

`network[]`：`{interface:string, rx_bytes_total:int, tx_bytes_total:int, rx_bytes_per_sec, tx_bytes_per_sec, rx_errors_per_sec, tx_errors_per_sec, rx_drops_per_sec, tx_drops_per_sec (均 number|null), is_virtual:bool}`。

`temperature[]`：`{sensor:string, celsius:number}`。`sensor` 为稳定标识且在单帧内唯一（Linux hwmon：`<name>#<hwmonN>:<label>`，无 label 时为 `<name>#<hwmonN>:tempN`；`<hwmonN>` 为 sysfs hwmon 目录名，保证同一启动周期内稳定、不同设备不冲突；病态驱动同一芯片重复 label 时追加 `-<n>` 兜底），`celsius` 为摄氏温度。温度为瞬时 gauge，不是累计计数器；传感器集合可在帧间变化。**不可用不是 0**：传感器缺失/读取失败/该指标未采集时输出 `null`（能力可用但本帧未读到，见 §2.7），不得输出 0。Windows 恒为 `null`（不提供温度）。Linux 读取**全部 hwmon 温度传感器**，不按设备类型过滤（厂商芯片经 hwmon 暴露的 GPU/网卡温度也会出现，但仅温度读数，不扩张 C 组指标范围）。

`shm_dir`：`{path:string, total_bytes:int, page_files:int, oldest_file_age_sec:int|null}`。共享内存目录占用，用于确认页清理机制没有失效。

- `path` 为共享内存根目录的绝对路径（`$DZTRADER_HOME/shm`）；`total_bytes` 为该目录下全部普通文件字节数之和（递归，含 `meta.dat` 与页文件）。
- `page_files` 为页文件数量（递归统计文件名形如 `<数字>.dat` 的页文件；通道目录内的页文件按目录展开，`meta.dat` 不计）。
- `oldest_file_age_sec` 为**最旧页文件**的文件年龄（秒，`0` 表示刚写入）；没有页文件时为 `null`。口径明确取页文件而非目录内任意文件（#75）：`meta.dat` 不随页清理消失，若把它算进来，"页清理失效"会被长期存在的元数据文件掩盖成正常。页文件年龄是页清理是否生效的直接信号。
- 目录不存在/不可读/无权限 ⇒ `shm_dir = null` + 可用性 `collect_failed`（能力粘性），**不得**输出全 0 的对象（0 是"目录为空"这一真实事实，不是读取失败）。

### 2.5 availability（关键设计）

前端按本字段渲染，**不得按 OS 名硬编码分支**；不可用一律显示"不可用"而非 0。

| 字段 | 类型 | 说明 |
|------|------|------|
| `platform_description` | string | 人类可读平台描述，**Windows 必须含 build 号**（如 `"Windows 10 Pro (build 19045)"`） |
| `metrics` | object | 按指标可用性映射，键集合见下；每个值为 `{available:bool, reason?:string}`；`available=false` 时 `reason` 必填 |
| `semantic_hints` | object | 跨平台语义提示（key → 文案），key 取指标键。当前版本定义 `cpu.load_average`（Windows 处理器队列长度代理量不是 Unix load average，不得同轴比较）与 `disk_io`（延迟是每完成请求的平均驻留时间、含排队；队列深度是采样时刻的在途请求数，不是区间平均；该提示在 `disk_io` 能力可用时携带）。无提示时为空对象 |

`metrics` 键集合（固定，发送方每帧必须覆盖全部键）：

`system`、`cpu.total`、`cpu.cores`、`cpu.load_average`、`cpu.context_switches`、`cpu.frequency`、`memory`、`swap`、`volumes`、`disk_io`、`network`、`shm_dir`、`temperature`。

`reason` 为自由文案（英文、小写开头、无句号），建议取值：
- `not_implemented` —— 本版本尚未采集该指标
- `unsupported` —— 本平台不支持（如无交换分区、Windows 上的温度/频率）
- `no_permission` —— 计数器需特权/未开启
- `no_sensor` —— 传感器不存在（如温度）
- `collect_failed` —— 本次采集失败（能力存在但读不到；下一次采样可能恢复）
- `disabled_by_config` —— 该指标类别已被配置关闭（`sampling.groups` 中对应开关为 false）

> 当前版本（#76）实现 `system`、`cpu.total`、`cpu.cores`、`cpu.load_average`、`cpu.context_switches`、`memory`、`swap`、`volumes`、`shm_dir`、`disk_io`；Linux 上按宿主能力实现 `cpu.frequency`（cpufreq）与 `temperature`（hwmon）——无 cpufreq 时 `cpu.frequency` 为 `unsupported`，无 hwmon 传感器时为 `temperature` = `no_sensor`；Windows 上两者不提供（默认配置关闭对应 B 组开关，故为 `disabled_by_config`；用户显式开启后采集器恒报 `unsupported`），也不提供整体频率。无交换分区/无页面文件的机器上 `swap` 为 `unsupported`。`network` 固定为 `available=false` + `reason="not_implemented"`，对应域输出 `null`。
>
> `disk_io` 的可用性同为**能力粘性**：一次成功枚举到物理盘后 `available` 保持 `true`，此后单帧读取失败只把域置 `null`；**单个盘读不到不改写整体可用性**（读不到的盘不进数组）。一个盘都读不到时：Linux 为 `collect_failed`，Windows 为 `no_permission`（计数器需特权/未开启，契约 §2.4）。配置关闭 `sampling.groups.disk_io` 时优先输出 `available=false` + `reason="disabled_by_config"`、`disk_io` 为 `null`。
>
> `memory` / `swap` 的可用性同为**能力粘性**：一次成功读到后可用性保持 `true`；`swap` 还须确认本机确有交换/页面文件（`total_bytes > 0`），否则为 `available=false` + `reason="unsupported"` 且 payload 输出 `total_bytes=0` 的对象。此后单帧读取失败只把对应域置 `null`，不改写可用性。配置关闭 `sampling.groups.memory` / `sampling.groups.swap` 时优先输出 `available=false` + `reason="disabled_by_config"`，对应域为 `null`；前端内存/交换区块按可用性显示明确空态（不显示 0）。
>
> `cpu.cores` / `cpu.load_average` / `cpu.context_switches` 的可用性表示**能力**（不是本帧数值是否就绪）：能力一旦确认，可用性为粘性 `true`，此后的采样失败只把数值置 `null`，不改写可用性。能力从未确认或平台不支持时可为 `available=false` + `reason="collect_failed"`（平台不支持也可用 `unsupported`；当前版本无法区分能力缺失与暂时失败，统一用 `collect_failed`；`reason` 为自由文案，本版本新增建议取值 `collect_failed`）。
>
> `volumes` / `shm_dir` 的可用性同为**能力粘性**：一次成功枚举到卷 / 读到目录后可用性保持 `true`，此后单帧失败只把对应域置 `null`，不改写可用性。配置关闭 `sampling.groups.volumes` 时优先 `available=false` + `reason="disabled_by_config"`、`volumes` 为 `null`；配置关闭后 `watch_paths` 仍输出（落卷解析不受卷容量采集开关影响，告警需要它，见 #80）。`shm_dir` 没有独立配置组（页清理有效性是基础可观测项），始终采集。**单个卷不可读**不改写 `volumes` 的整体可用性——该卷以 `available=false` 进数组（§2.4），整体仍为 `available=true`；只有当一次卷都枚举不到时才整体标记 `collect_failed`。
>
> `cpu.frequency` / `temperature` 的可用性同为**能力粘性**：一次成功读到后可用性保持 `true`，此后单帧读取失败只把数值置 `null`（温度允许空数组/`null`、频率允许 `null`），消费方据此保留区块并显示「暂无数据」而不是「不可用」。配置关闭对应组时优先输出 `available=false` + `reason="disabled_by_config"`，数值为 `null`。温度/频率不可用时前端**整块不渲染**（不是显示 0，也不是显示「不可用」占位）。

### 2.6 快照示例（当前版本节选）

```json
{
  "source": "dzsysmon",
  "timestamp": 1790000005000,
  "sampling": {"measure_interval_ms": 1000, "last_sample_unix_ms": 1790000005123, "missed_ticks": 0},
  "platform": {"family": "linux", "version": "Ubuntu 24.04.1 LTS", "kernel": "6.8.0-45-generic", "arch": "x86_64", "build": 0},
  "system": {"hostname": "trade-01", "cpu_model": "AMD Ryzen 9 7950X 16-Core Processor", "logical_cores": 4, "boot_time_unix_sec": 1789990000, "uptime_sec": 10005},
  "cpu": {
    "total": {"usage_ratio": 0.187},
    "cores": [
      {"index": 0, "usage_ratio": 0.21, "saturated": false},
      {"index": 1, "usage_ratio": 0.95, "saturated": true},
      {"index": 2, "usage_ratio": 0.05, "saturated": false},
      {"index": 3, "usage_ratio": null, "saturated": false}
    ],
    "core_frequencies": [
      {"index": 0, "mhz": 4200},
      {"index": 1, "mhz": 3600},
      {"index": 2, "mhz": 3600},
      {"index": 3, "mhz": 4200}
    ],
    "saturated_cores": [1],
    "load_average": {"one": 0.42, "five": 0.51, "fifteen": 0.63, "semantic": "load_average"},
    "context_switches_per_sec": 1234.5,
    "interrupts_per_sec": 567.0
  },
  "temperature": [
    {"sensor": "coretemp#hwmon3:Package id 0", "celsius": 45.5},
    {"sensor": "coretemp#hwmon3:Core 0", "celsius": 44.0}
  ],
  "memory": {
    "total_bytes": 34359738368,
    "available_bytes": 17179869184,
    "used_bytes": 17179869184,
    "used_ratio": 0.5,
    "cached_bytes": 8589934592,
    "buffers_bytes": 268435456
  },
  "swap": {"total_bytes": 8589934592, "used_bytes": 2147483648, "used_ratio": 0.25},
  "volumes": [
    {"mount": "/", "watched": false, "available": true, "total_bytes": 107374182400, "used_bytes": 42949672960, "used_ratio": 0.4},
    {"mount": "/data", "watched": true, "available": true, "total_bytes": 1099511627776, "used_bytes": 274877906944, "used_ratio": 0.25},
    {"mount": "/backup", "watched": false, "available": false, "total_bytes": null, "used_bytes": null, "used_ratio": null, "reason": "no_permission"}
  ],
  "watch_paths": [
    {"path": "/srv/dztrader/shm", "mount": "/data", "available": true},
    {"path": "/srv/dztrader/db", "mount": "/data", "available": true},
    {"path": "/srv/dztrader/missing", "mount": null, "available": false, "reason": "path_not_found"}
  ],
  "disk_io": [
    {"device": "sda", "read_bytes_per_sec": 1048576.0, "write_bytes_per_sec": 524288.0,
     "read_iops": 15.0, "write_iops": 5.0, "latency_ms": 1.25, "queue_depth": 3,
     "utilization": 0.5},
    {"device": "nvme0n1", "read_bytes_per_sec": 0.0, "write_bytes_per_sec": 0.0,
     "read_iops": 0.0, "write_iops": 0.0, "latency_ms": null, "queue_depth": 0,
     "utilization": 0.0}
  ],
  "network": null,
  "shm_dir": {"path": "/srv/dztrader/shm", "total_bytes": 754974720, "page_files": 12, "oldest_file_age_sec": 480},
  "availability": {
    "platform_description": "Ubuntu 24.04.1 LTS (kernel 6.8.0-45-generic)",
    "metrics": {
      "system": {"available": true},
      "cpu.total": {"available": true},
      "cpu.cores": {"available": true},
      "cpu.load_average": {"available": true},
      "cpu.context_switches": {"available": true},
      "cpu.frequency": {"available": true},
      "memory": {"available": true},
      "swap": {"available": true},
      "volumes": {"available": true},
      "disk_io": {"available": true},
      "network": {"available": false, "reason": "not_implemented"},
      "shm_dir": {"available": true},
      "temperature": {"available": true}
    },
    "semantic_hints": {
      "disk_io": "latency is the average time per completed request, queue wait included; queue depth is the instantaneous in-flight request count at the measurement tick, not an interval average"
    }
  }
}
```

### 2.7 计算语义（发送方义务）

- 速率与百分比均由纯函数从**原始累计计数器**差值计算（`SystemCollector` 只返回原始计数器，不含增量/百分比）。
- **首次采样返回不可用**（`null`），不得返回 0。
- 计数器回绕：仅当上次与本次值均落在该计数器的位宽跨度内且本次小于上次时按跨度回绕计算（默认 32 位；磁盘 I/O 的时间计数器是「32 位毫秒计数器换算为微秒」，跨度见下条）；其余下降视为计数器重置，本次输出不可用（下一采样周期重建基线）。
- Linux 忙闲口径：`iowait` 不计入忙；`guest`/`guest_nice` 不重复计入总量（已含于 user/nice）。
- 两次采样间隔内 CPU 使用率 = `1 − Δidle/Δtotal`，结果钳制到 `[0,1]`。
- 每核使用率：与总量同公式；核序列长度或 index 序列变化（热插拔 / 处理器组变化）视为基线失效，本次每核使用率全部 `null` 并重建基线。
- 速率型指标（上下文切换 / 中断）：`Δcount / 实际经过秒数`（采样间隔用单调时钟测量），首次采样 `null`；`Δ时间 ≤ 0` 本次不可用。
- **饱和核判定**：单核使用率连续 `alerts.cpu_sustain_samples` 次采样 **≥** `alerts.cpu_sustain_used_pct / 100` 即标记 `saturated=true`；任一采样严格低于阈值即清零解除；不可用样本（`usage_ratio=null`）不改变连续计数；核序列变化重置全部计数。阈值与连续次数取自当前配置（运行期修改语义随配置票 #81）。
- Windows >64 逻辑核的机器上每核序列可能不可用（处理器组限制）：此时 `cpu.cores` 标记不可用、中断累计计数输出 `null`；不得用组内数据冒充全系统每核序列。
- **本帧数值不得沿用上一帧**：某项能力可用但本帧数值未就绪或采集失败时，对应字段输出 `null`——每核序列整体未采到时 `cpu.cores` 保留已知核 index 条目、`usage_ratio` 全部 `null`、`saturated` 全部 `false`（从未获得过序列时 `cpu.cores` / `saturated_cores` 为空数组）；`availability` 的能力标记保持粘性，消费方据此显示"不可用"而不是旧数据。单个核的计数器不可用时该核 `usage_ratio=null`，不改变饱和判定的连续计数。
- **每物理盘 I/O 的计算语义**（契约 §2.4 `disk_io[]`）：
  - 采集层只给原始累计计数器：Linux 读 `/proc/diskstats`（扇区数按内核 ABI 固定 ×512 得字节；`ms_reading` / `ms_writing` / `io_ticks` 是 32 位毫秒计数器，换算为微秒后上报）；Windows 读 `IOCTL_DISK_PERFORMANCE` 的 `DISK_PERFORMANCE`（`ReadTime`/`WriteTime`/`IdleTime` 为 100ns 累计值，换算为微秒；`QueryTime` 是查询时刻的系统时间锚点，`QueryTime − IdleTime` 的差值才是累计忙时间）。
  - 派生值全部由纯函数从两次采样差值计算：吞吐/IOPS = Δ计数器 / Δ实际经过秒数（单调时钟测量）；`latency_ms` = Δ(读+写服务时间) / Δ(读+写完成数)；`utilization` = Δ忙时间 / Δt；`queue_depth` 直接取本帧瞬时读数（不做差值）。
  - **首次采样（无基线）输出全部派生字段 `null`**，不得用 0 冒充。基线按 `device` 匹配：本帧新出现的设备等同首次采样；消失的设备不再输出。
  - **计数器回绕按原生位宽处理**：采集层在每条原始计数器上携带该设备的回绕跨度（数据驱动，消费方不按平台分支）。计数类：Windows `ReadCount`/`WriteCount` 为 32 位 DWORD（跨度 `2^32`），Linux 为 64 位（跨度 0 = 不回绕，下降即重置）；时间类（微秒）：Linux 的 `ms_reading`/`ms_writing`/`io_ticks` 是 32 位毫秒计数器（跨度 `2^32 × 1000`），Windows 的 100ns 计数器为 64 位（0）；字节类：两平台均为 64 位（0）。仅当上次与本次值均落在跨度内且本次更小时按跨度回绕计算；其余下降视为计数器重置，本帧该字段 `null` 并在下一采样周期重建基线。
  - **采样窗口**：速率的分母是两次**成功采样**之间实际经过的时间；采集失败期间基线不推进，因此恢复后的首帧速率是跨越该缺口的窗口平均（口径明确，非伪造瞬时值）。磁盘 I/O 采集失败只把该域置 `null`，**不累加** `sampling.missed_ticks`（该标记表示计量 tick 整体被跳过，不是单个指标域失败）。
  - `utilization` 结果钳制到 `[0,1]`；若 Δ忙时间超过窗口的 1.5 倍，视为计数器口径异常（如 Windows `QueryTime` 受系统时间跳变影响），本帧输出 `null`，不得钳成 100%。窗口内完成请求数为 0 时 `latency_ms` 为 `null`（无定义）。
- **温度 / 每核频率采集语义**：两者都是瞬时 gauge，不做差值或聚合（温度单位为摄氏度，频率四舍五入到整数 MHz）；传感器集合与核集合可在帧间变化，按本次读数输出，不沿用上一帧。读取失败的单个传感器/核不进入数组；能力可用但本帧一个都没读到时输出 `null`。配置 `sampling.groups.temperature=false` / `sampling.groups.cpu_frequency=false` 时跳过采集，可用性为 `disabled_by_config`、数值 `null`。Windows 不提供温度与每核频率（恒 `null`），也不提供整体频率。
- **内存 / 交换采集语义**：两者都是瞬时 gauge，每次计量刷新，不做差值或聚合，不沿用上一帧。Linux 读 `/proc/meminfo`（`MemTotal`/`MemAvailable`/`Buffers`/`Cached`/`SReclaimable`/`SwapTotal`/`SwapFree`）；`MemAvailable` 缺失、为 0 或大于总量时按 `MemFree + Buffers + Cached + SReclaimable` 回退并钳制到 `[0, total]`，异常与回退均记日志；回退也无法计算时本次内存采集失败（该域 `null`，可用性按能力粘性）。Windows 读 `GetPerformanceInfo`（`PhysicalTotal`/`PhysicalAvailable`/`SystemCache`/`CommitLimit`/`CommitTotal`）。
- **计量 tick 级**采集失败（如 `/proc/stat` 读取失败）跳过本次计量并累加 `sampling.missed_ticks`；单个指标域（如磁盘 I/O）采集失败只把该域置 `null`，不累加（该标记表示 tick 整体被跳过）。

---

## 3. 配置（SET/RTN_SYSMON_CONFIG）

**语义**：运行期修改系统监控配置（RFC 7386 merge patch），并回推**全量**生效配置。配置持久化为 `$DZTRADER_HOME/configs/dzsysmon.json`（进程缺失时自动生成默认值；仓库不提交模板文件）。日志配置走《帧契约：日志》的 `SET/RTN_LOG_CONFIG`，**不在本帧 payload 内**。

**数据流**：形态 1——dzweb → `dzsysmon`（`instance_id=dzsysmon`）→ 校验/应用/持久化 → `RTN_SYSMON_CONFIG` 全量回推 → dzweb 镜像 `sysmon_config` 域 → WS 消息 `sysmon_config`。前端入口（REST `PUT /api/sysmon/config`）随运行期配置实现登记到契约 rest；本契约落帧语义与接收方处理（校验/持久化/定时器即时重排）。

**SET payload**：JSON object（merge patch；object 内嵌套对象按键合并、数组整体替换）。任意位置出现 `null` 为校验失败；空对象 `{}` 为无操作但**仍回 RTN**。未知字段忽略（总则 §8）。

**RTN payload**：全量配置（`SysmonConfig`，即当前生效值；无 `error` 字段，失败经四件套表达）。

### 3.1 配置 schema

`sampling`：

| 字段 | 类型 | 默认 | 约束 | 说明 |
|------|------|------|------|------|
| `measure_interval_ms` | int | 1000 | `[100, 60000]` | 计量间隔；速率型指标的采样窗口 |
| `record_interval_sec` | int | 60 | `[1, 86400]` | 记录间隔（历史写入用；当前版本未消费） |
| `phase_offset_sec` | int | 3 | `[0, 86400]` | 记录 tick 相位偏移，避开整点附近其它任务 |
| `groups` | object\<string,bool\> | Linux：全部 true；Windows：`temperature`/`cpu_frequency` 为 false | 值必须 bool | 启用的指标大类：`cpu`/`memory`/`swap`/`volumes`/`disk_io`/`network`/`temperature`/`cpu_frequency`。`temperature` 与 `cpu_frequency` 属 B 组：Linux 默认开（有 hwmon / cpufreq 时）；Windows 默认关（平台上不可靠或不存在，需显式开启；本版本 Windows 采集器仍报告 `unsupported`）。缺少的键按 true 处理 |

`disk`：`{watch_paths:string[], devices:string[]}`（元素非空；`watch_paths` 为空表示使用默认关注路径——共享内存目录与数据库目录；非空时逐条解析并作为 `watch_paths[]` 输出，同时决定 `volumes[].watched`）。`watch_paths` 影响快照 `watch_paths` / `volumes[].watched`，运行期修改随 #81 生效。

`network`：`{hide_virtual:bool}`，默认 `true`（隐藏 Hyper-V/WSL/Docker 虚拟网卡）。

`history`：`{enabled:bool, raw_retention_hours:int, max_retention_days:int, max_bytes:int}`，默认 `true/24/30/2147483648`；`raw_retention_hours ∈ [1,720]`、`max_retention_days ∈ [1,3650]`、`max_bytes ≥ 0`（0 = 不限制）。

`realtime`：`{push_interval_sec:int}`，默认 `5`，`[1, 3600]`；与计量频率解耦（事件通道每次写入会唤醒全部注册进程）。

`alerts`：`{cpu_sustain_used_pct:number, cpu_sustain_samples:int, memory_used_pct:number, swap_used_pct:number, volume_free_pct:number, volume_free_bytes:int, cooldown_sec:int}`，默认 `90/10/90/50/10/1073741824/600`；百分比字段 `[0,100]`、`cpu_sustain_samples ∈ [1,3600]`、`cooldown_sec ∈ [0,86400]`。`cpu_sustain_used_pct` / `cpu_sustain_samples` 由发送方用于饱和核判定（§2.7）；阈值告警的触发与冷却由 #80 消费。

### 3.2 触发场景（RTN_SYSMON_CONFIG）

1. 进程启动初始化完成后主动推送一次（总则 §7 完整性收敛）
2. 每次 `SET_SYSMON_CONFIG` 成功/失败后回推（失败回推旧值，四件套见总则 §8）
3. `QUERY_FULL_SNAPSHOT` 响应时推送一次

### 3.3 失败处理（四件套，总则 §8）

校验失败/持久化失败：回滚旧值（内存镜像不变）→ 日志（英文）→ `NOTIFY_UI` 错误级别弹窗（`popup=true`）→ 回推旧值。

---

## 4. 快照触发场景（RTN_SYSMON_SNAPSHOT）

1. **进程启动初始化完成后立即推送一次**（首帧 `cpu.total.usage_ratio` 可为 `null`）
2. **周期推送**：间隔 = `realtime.push_interval_sec`（默认 5 秒）；与计量频率解耦，计量时刻不推送
3. **`QUERY_FULL_SNAPSHOT` 响应**：收到总则 §7 的广播查询帧后立即推送一次（该帧无 `instance_id`）

推送失败（事件通道写满等）只记日志，不重试；下一刻度继续推送。

---

## 5. 镜像与 WS 映射（dzweb）

| 帧 | 镜像实例 | 镜像 domain | WS 消息 | WS `instance_id` |
|----|---------|------------|---------|------------------|
| `RTN_SYSMON_SNAPSHOT` | `dzsysmon`（固定；帧头无实例标识、payload `source` 显式携带发送方） | `sysmon_snapshot` | `sysmon_snapshot` | `dzsysmon` |
| `RTN_SYSMON_CONFIG` | `dzsysmon` | `sysmon_config` | `sysmon_config` | `dzsysmon` |

- 两个 domain 均进**快照域**：新建 WS 连接立即拿到当前值（顶栏需要全局可用）。
- dzweb 读取时必须校验快照（`SysmonSnapshot::validate`）与配置（`SysmonConfig::validate`）；非法 payload 记日志并忽略（不更新镜像、不广播）。

> 总则 §9 的"无 `instance_id` 帧挂固定 `dztraderd`"适用于 master 广播帧；快照帧按本契约固定挂 `dzsysmon`（发送方唯一且固定）。

---

## 6. REST

`GET /api/sysmon/snapshot`：读取 dzweb 镜像中的当前快照（任意已认证用户）。镜像尚未就绪（dzsysmon 未上报）→ `503 {"error":"sysmon snapshot not available"}`。端点细节见契约 rest。

---

## 7. 校验（除总则 §8 外的特殊规则）

- 快照中 `null` **仅允许**出现在本文档 §2 明确标注 `| null` 的位置；其余位置出现 `null` 为校验失败。
- `used_bytes` 必须等于 `total_bytes − available_bytes`（`memory`）；`used_bytes ≤ total_bytes`（`memory`/`swap`/`volumes`）；`memory.cached_bytes` / `memory.buffers_bytes` 缺失或 `null` 合法，出现时须为非负整数且不大于 `total_bytes`。
- `volumes[]`：`mount` 非空且帧内唯一；`available=true` 时 `total_bytes`/`used_bytes` 必须为非负整数且 `used_bytes ≤ total_bytes`、`used_ratio` 为 `[0,1]` 或 `null`，且**不得**出现 `reason`；`available=false` 时 `reason` 必须非空、`total_bytes`/`used_bytes`/`used_ratio` **必须**为 `null`（禁止用 0 冒充）。
- `watch_paths[]`：`path` 非空且帧内唯一；`available=true` 时 `mount` 必须非空且不得出现 `reason`，且**当同帧 `volumes` 非 `null` 时** `mount` 必须为 `volumes[].mount` 之一（`volumes` 为 `null` 时不交叉校验——配置关闭卷采集后落卷信息仍要能给告警用）；`available=false` 时 `mount` 必须为 `null` 且 `reason` 必须非空。
- `disk_io[]`：`device` 非空且帧内唯一；除 `device` 外的七个字段为 `null` 或数字——吞吐/IOPS/延迟/队列深度为 `>= 0`，`utilization` 为 `[0,1]` 内的比率或 `null`（首帧全 null 的条目合法）。
- 比率为 `[0,1]`；字节/计数为非负整数；`boot_time_unix_sec`/`uptime_sec` 非负。
- `availability.metrics` 中 `available=false` 时 `reason` 必须存在且非空；`available=true` 时不得出现 `reason`。
- 未知字段忽略（前向兼容）。

---

## 8. 范围与明确不做

- **不支持容器内采集**：容器内 `/proc` 报宿主容量，会给出错误的容量基线；部署文档写明（`docs/components/dzsysmon.md`），不做 cgroup v1/v2 感知。
- **Windows 不提供温度与每核频率**（也不提供整体频率）：Windows 上温度需 WMI/厂商驱动、每核频率无可靠 API，两者恒为不可用；本版本不实现「整体频率 + 口径标注」形态。
- **磁盘 I/O 为整盘粒度**：不提供分区级 I/O。本版本的 `device` 只有物理盘标识（Windows `PhysicalDrive<N>`）；**卷/盘符 ↔ 物理盘映射**由后续票 #103 提供（ADR 0024 Consequences 的映射要求，本票不交付）。Windows 依赖性能计数器（diskperf）与访问权限，读不到时按 §2.4 报告不可用，不做提权或反复重试。
- **磁盘 I/O 无历史/告警消费**：本契约只定义实时快照域；历史写入（#78）与阈值告警（#80）的消费范围随各自票定义。
- 历史查询不走帧（dzweb 只读历史库）；告警复用 `NOTIFY_UI`；进程级指标、GPU、SMART、风扇、电池、登录用户列表不在本契约范围。
