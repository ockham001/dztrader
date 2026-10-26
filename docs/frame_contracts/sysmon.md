# 帧契约：系统监控

本文件覆盖 `DZ_FRAME_RTN_SYSMON_SNAPSHOT`、`DZ_FRAME_SET_SYSMON_CONFIG`、`DZ_FRAME_RTN_SYSMON_CONFIG` 三个帧。总则见《帧契约：通用规则》。

发送方固定为内部进程 `dzsysmon`（进程名即 `instance_id`，见总则 §5）。**资源越限（内存使用率 / 交换使用率 / 卷剩余空间 / CPU 饱和）只写日志与 UI 色标，不产生 UI 通知**（§2.8）；`dzsysmon` 的 `NOTIFY_UI` 只保留真故障（历史库不可用、配置下发失败），不新增通知帧。历史查询**不走帧**（dzweb 以只读方式直接打开历史库，schema 由历史模块承载）。

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

**语义**：推送当前实时快照。当前版本（#80）填系统信息与 CPU 区块（总量、每核与饱和标记、系统负载、上下文切换/中断速率、每核频率）、温度、内存与交换、卷容量（本机全部卷）、共享内存目录占用、每物理盘 I/O、每网卡收发速率与累计量（默认隐藏虚拟网卡，见 §2.4）。资源越限**不改变快照形状**——它只写日志与 UI 色标，不携带越限集合（§2.8）。已列出的指标域后续只填值、**不改形状**（新增指标类别属 additive 扩展，按总则 §11.3 同步契约、类型与前端 schema；消费方忽略未知字段）。

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
| `volumes` | array \| null | 卷容量与可用性，范围恒为本机全部卷（见 §2.4） |
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

`volumes[]`：`{mount:string, available:bool, total_bytes:int|null, used_bytes:int|null, used_ratio:number|null, reason?:string}`。

- `mount` 为规范化挂载点：Linux 取挂载目录（`/`、`/home`），Windows 取盘符根（`C:\`）。同一帧内唯一。
- `available=false`（`reason` 必填）表示**卷存在但容量读不到**（无权限、卷不可访问）。此时 `total_bytes`/`used_bytes`/`used_ratio` **必须为 `null`**，不得用 0 冒充；卷仍然进数组，因为"卷存在但读不到"与"根本没有这个卷"是不同事实，消费方都要能显示。
- `available=true` 时 `total_bytes`/`used_bytes` 为非负整数且 `used_bytes ≤ total_bytes`；`used_ratio = used_bytes / total_bytes`，`total_bytes = 0`（异常/特殊卷）时为 `null`。
- 采集范围恒为**本机全部卷**，不存在"关注哪一卷"的概念（#121 取消关注路径）：Linux 过滤伪文件系统（`proc`/`sysfs`/`devtmpfs`/`devpts`/`tmpfs`/`cgroup*`/`debugfs`/`tracefs`/`configfs`/`mqueue`/`hugetlbfs`/`autofs`/`binfmt_misc`/`nsfs`/`pstore`/`securityfs`/`fusectl`/`bpf`/裸 `fuse` 等；`fuse.*` 家族按前缀过滤），同一挂载点只保留一条；Windows 覆盖固定盘（`GetDriveType` = `DRIVE_FIXED`）。
- **伪文件系统清单口径（#122）**：裸 `fuse`（`/proc/mounts` 中 `fstype` 字面量即 `fuse`，不带实现后缀，如 GNOME remote desktop 的 `cliprdr-*` 临时挂载）与 `fuse.*` 前缀匹配并列过滤——它们 `statvfs` 返回 0 块，`df` 默认也不列。**`fuseblk` 不在此列**：它由内核 fuse 模块承载块设备上的真实文件系统（典型是 ntfs-3g 挂载的 NTFS 卷），过滤掉会隐藏真实存储。
- 卷容量不参与 `NOTIFY_UI`：使用率只用于 UI 色标与日志（§2.8）。
- **无容量挂载的折叠是纯显示行为（#122）**：`available=false` 或 `total_bytes = 0` 的挂载**照常进 `volumes[]`**（`available=true` + `total_bytes=0` 是合法事实，不得在采集层抹掉）；监控页正文只列容量可读且总量 > 0 的卷，其余折叠进「无容量挂载（N）」分组、组内逐条显示挂载点与不可用原因。顶栏徽章是全局常驻条，但**不逐个平铺**（#127）：`≥lg` 显示使用率最高的 2 个卷 + 「其余 N 个」（悬停标题列出其余卷的挂载点与使用率/不可用原因，点击展开全部、再点收起；展开状态不持久化），`<lg` 收敛为**单个状态图标**（图标颜色取 CPU / 内存 / 全部卷中最差的使用率色标；无快照时为中性等待态）。收敛**不丢事实**：`≥lg` 显示使用率最高的 2 个卷；不可读卷（`used_ratio = null`）按挂载点升序排在可读卷之后，**可读卷足够 2 个时**它进入「其余 N 个」清单，可读卷不足 2 个时补位直接显示（以「不可用」呈现）——两种情况下事实都不丢；`<lg` 时整组资源徽章收起为单个状态图标，其 title 汇总各资源读数（悬停可见），完整信息始终在监控页正文（「无容量挂载」是监控页正文的分组口径，与顶栏的收敛是两件事）。
- **卷列表的展示顺序（#122）**：数组本身**不要求顺序**（发送端按枚举顺序输出即可）；消费方渲染时按 `used_ratio` 降序、同值按 `mount` 升序排列，`used_ratio = null` 的排在末尾并按 `mount` 升序。展示顺序因此不依赖发送端顺序。

`disk_io[]`：每物理盘 I/O 派生值。字段为 `{device:string, read_bytes_per_sec, write_bytes_per_sec, read_iops, write_iops, latency_ms, queue_depth, utilization}`；除 `device` 外均为 `number | null`——**计数器不可用时为 `null` 而非 0**（契约 §2.7）。

- `device` 为设备稳定标识：Linux 为内核块设备名（`sda` / `nvme0n1` 等**整盘**名，不含分区）；Windows 为 `PhysicalDrive<N>`（物理盘编号）。帧内唯一，可在帧间变化（热插拔/盘号增减）。
- 采集范围只含**整块物理盘**：Linux 以 `/sys/block` 的整盘条目为准（分区天然不在其中），并排除 loop/ram/zram 内存与回环设备、`dm-*`/`md*`/bcache 聚合设备（其 I/O 已计入底层物理盘，单列会重复计数）与 `nbd`/`rbd`/`drbd` 网络块设备、`sr`/`sg`/`fd`/`pktcdvd` 光学/软驱/包写入等非存储设备；Windows 遍历 `PhysicalDrive0..PhysicalDrive32`，不存在的盘号与不支持计数器的盘（含无权限）跳过。**盘符/卷 × 物理盘的映射不在本版本范围**（Windows `device` 只有物理盘编号，见 §8）。配置 `disk.devices` 非空时只输出匹配的设备（§3.1；过滤在快照构造阶段，不改变采集与速率基线）。
- `read_bytes_per_sec` / `write_bytes_per_sec`：吞吐（字节/秒）；`read_iops` / `write_iops`：每秒完成的读/写请求数。四者 = Δ累计计数器 / Δ实际经过秒数（§2.7）。
- `latency_ms`：**平均每完成请求的驻留时间（含排队）**，单位毫秒 = Δ(读服务时间 + 写服务时间) / Δ(读完成数 + 写完成数)。窗口内没有完成的请求时为 `null`（无定义，不是 0）。
- `queue_depth`：**采样时刻的在途请求数**（瞬时 gauge，如 Linux `ios_in_progress` / Windows `QueueDepth`），**不是区间平均队列深度**；平台不提供该读数时为 `null`。
- `utilization`：设备忙时间占比 = Δ忙时间 / Δt，钳制到 `[0,1]`；平台不提供忙时间、或 Δ忙时间超过计量窗口容忍上限（计数器口径异常，如系统时间跳变）时为 `null`（不钳成 100% 冒充满载）。
- 首帧（无基线）**全部派生字段为 `null`**（含瞬时 gauge `queue_depth`：首次采样整块不可用，不得用 0 冒充）。本帧新出现的设备同样在没有基线时除 `device` 外全部 `null`；消失的设备不再输出。
- 采集失败与不可用：Linux `/proc/diskstats` 或 `/sys/block` 不可读 ⇒ 整域 `null` + `collect_failed`；Windows 一个盘的性能计数器都读不到（无管理员权限、性能计数器未开启或盘不支持）⇒ 整域 `null` + `no_permission`（`no_permission` 的既有语义即"计数器需特权/未开启"，**Windows 上不可用是常态而非异常**）。单个盘读不到只是不进入数组，不改写整体可用性；空数组 = 采集成功但本机无物理盘（仅 Linux 会出现；Windows 上 `CreateFile` 对"盘不存在"与"无权限"都返回同一个失败，无法区分，故恒以 `null` + `no_permission` 表示不可用）。

`network[]`：每网卡累计收发与速率。字段为
`{interface:string, rx_bytes_total:int, tx_bytes_total:int, rx_packets_total:int, tx_packets_total:int, rx_errors_total:int, tx_errors_total:int, rx_drops_total:int, tx_drops_total:int, rx_bytes_per_sec:number|null, tx_bytes_per_sec:number|null, rx_errors_per_sec:number|null, tx_errors_per_sec:number|null, rx_drops_per_sec:number|null, tx_drops_per_sec:number|null, is_virtual:bool}`。

- `interface` 为网卡名（Linux 内核接口名；Windows 适配器别名 `Alias`），单帧内唯一，可在帧间变化（热插拔 / 重命名 / 驱动重载）。
- 八个 `*_total` 为**累计计数器**（收/发 × 字节/包/错误/丢弃），非负整数；包计数含单播与组播/广播。
- 六个 `*_per_sec` 为**速率**（= Δ累计计数器 / Δ实际经过秒数，见 §2.7）；首次采样、本帧新出现的网卡或计数器重置时为 `null`，不得用 0 冒充。`0` 表示该窗口内确实没有增量。
- `is_virtual` 表示该网卡**不由硬件设备支撑**（虚拟网卡、回环、桥接、隧道、聚合与 VLAN 逻辑接口）。判定必须用结构性依据而非名字前缀：Linux 以 `/sys/class/net/<if>` 是否由硬件设备支撑（存在 `device` 符号链接）为准；Windows 以 `MIB_IF_ROW2.InterfaceAndOperStatusFlags.HardwareInterface` 为准。
- **默认隐藏虚拟网卡**：`network.hide_virtual = true`（默认值）时发送方**不输出** `is_virtual = true` 的条目；为 `false` 时全部输出。隐藏发生在**快照构造**而非采集：采集层始终返回全部网卡（含 `is_virtual`），因此改开关不需要重新采集、也不需要重启进程（§2.7）。
- 采集范围：全部已存在的网络接口，**含当前无流量的网卡与已断开的适配器**（"存在但为 0"与"不存在"是不同事实）。单个网卡读取失败只跳过该网卡，不使整域失败。
- 采集失败：Linux `/proc/net/dev` 不可读 ⇒ 整域 `null` + `collect_failed`；Windows `GetIfTable2` 失败 ⇒ 整域 `null` + `collect_failed`。空数组 = 采集成功但本机无网络接口（罕见，容器内可能）。

`temperature[]`：`{sensor:string, chip?:string, label?:string, celsius:number}`。`sensor` 为稳定标识且在单帧内唯一（Linux hwmon：`<name>#<hwmonN>:<label>`，无 label 时为 `<name>#<hwmonN>:tempN`；`<hwmonN>` 为 sysfs hwmon 目录名，保证同一启动周期内稳定、不同设备不冲突；病态驱动同一芯片重复 label 时追加 `-<n>` 兜底），`celsius` 为摄氏温度。温度为瞬时 gauge，不是累计计数器；传感器集合可在帧间变化。**不可用不是 0**：传感器缺失/读取失败/该指标未采集时输出 `null`（能力可用但本帧未读到，见 §2.7），不得输出 0。Windows 恒为 `null`（不提供温度）。Linux 读取**全部 hwmon 温度传感器**，不按设备类型过滤（厂商芯片经 hwmon 暴露的 GPU/网卡温度也会出现，但仅温度读数，不扩张 C 组指标范围）。

- `chip` 为**芯片名**（hwmon 的 `name`，即 `sensor` 中 `#` 之前的部分，如 `coretemp` / `amdgpu` / `iwlwifi_1` / `acpitz`），能力可用时非空。`label` 为**通道标签**（`tempN_label` 的内容），缺失时为空串——空串是「该通道没有标签」这一合法事实，不是不可用（#126）。
- 两者是 **additive 扩展**（#126）：`sensor` 的**值保持不变**——它是历史库 `temperature` 域的维度键，改格式会让已落库历史全部失配。消费方用二者做展示分组与友好命名，`sensor` 只用于标识与原始标识 tooltip。
- **发送方义务与消费方容忍是两件事，不矛盾**：本版本发送方**每次都必须给出两者**（`chip` 非空、`label` 可为空串）；而消费方**必须容忍它们缺失**（旧版本发送方的载荷不带这两个键）——缺失时从 `sensor` 回退解析（`#` 前为芯片名、`:` 后为标签或通道名），新旧版本短暂并存不会渲染失败。`label` 为空串时展示层回退注入通道名（`sensor` 冒号后的 `tempN`），不退化为空。

`shm_dir`：`{path:string, total_bytes:int, page_files:int, oldest_file_age_sec:int|null}`。共享内存目录占用，用于确认页清理机制没有失效。

- `path` 为共享内存根目录的绝对路径（`$DZTRADER_HOME/shm`）；`total_bytes` 为该目录下全部普通文件字节数之和（递归，含 `meta.dat` 与页文件）。
- `page_files` 为页文件数量（递归统计文件名形如 `<数字>.dat` 的页文件；通道目录内的页文件按目录展开，`meta.dat` 不计）。
- `oldest_file_age_sec` 为**最旧页文件**的文件年龄（秒，`0` 表示刚写入）；没有页文件时为 `null`。口径明确取页文件而非目录内任意文件（#75）：`meta.dat` 不随页清理消失，若把它算进来，"页清理失效"会被长期存在的元数据文件掩盖成正常。页文件年龄是页清理是否生效的直接信号。
- 目录不存在/不可读/无权限 ⇒ `shm_dir = null` + 可用性 `collect_failed`（能力粘性），**不得**输出全 0 的对象（0 是"目录为空"这一真实事实，不是读取失败）。

本域**没有**越限集合字段：阈值越限只写日志与 UI 色标，不进快照（§2.8）。

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

> 当前版本（#77）实现 `system`、`cpu.total`、`cpu.cores`、`cpu.load_average`、`cpu.context_switches`、`memory`、`swap`、`volumes`、`shm_dir`、`disk_io`、`network`；Linux 上按宿主能力实现 `cpu.frequency`（cpufreq）与 `temperature`（hwmon）——无 cpufreq 时 `cpu.frequency` 为 `unsupported`，无 hwmon 传感器时为 `temperature` = `no_sensor`；Windows 上两者不提供（默认配置关闭对应 B 组开关，故为 `disabled_by_config`；用户显式开启后采集器恒报 `unsupported`），也不提供整体频率。无交换分区/无页面文件的机器上 `swap` 为 `unsupported`。
>
> `network` 的可用性同为**能力粘性**：一次成功枚举到网卡后 `available` 保持 `true`，此后单帧读取失败只把域置 `null`（窗口不推进，恢复后首帧速率跨缺口平均，见 §2.7）。**单个网卡读不到不改写整体可用性**（该网卡不进数组）。配置关闭 `sampling.groups.network` 时优先输出 `available=false` + `reason="disabled_by_config"`、`network` 为 `null`。`hide_virtual` 只影响输出条目、不影响可用性：开关生效时若所有网卡均为虚拟网卡，`network` 为**空数组**且 `available=true`（"全部被过滤"与"采集失败"是不同事实，消费方据此显示「已隐藏全部虚拟网卡」而不是「不可用」）。
>
> `disk_io` 的可用性同为**能力粘性**：一次成功枚举到物理盘后 `available` 保持 `true`，此后单帧读取失败只把域置 `null`；**单个盘读不到不改写整体可用性**（读不到的盘不进数组）。一个盘都读不到时：Linux 为 `collect_failed`，Windows 为 `no_permission`（计数器需特权/未开启，契约 §2.4）。配置关闭 `sampling.groups.disk_io` 时优先输出 `available=false` + `reason="disabled_by_config"`、`disk_io` 为 `null`。
>
> `memory` / `swap` 的可用性同为**能力粘性**：一次成功读到后可用性保持 `true`；`swap` 还须确认本机确有交换/页面文件（`total_bytes > 0`），否则为 `available=false` + `reason="unsupported"` 且 payload 输出 `total_bytes=0` 的对象。此后单帧读取失败只把对应域置 `null`，不改写可用性。配置关闭 `sampling.groups.memory` / `sampling.groups.swap` 时优先输出 `available=false` + `reason="disabled_by_config"`，对应域为 `null`；前端内存/交换区块按可用性显示明确空态（不显示 0）。
>
> `cpu.cores` / `cpu.load_average` / `cpu.context_switches` 的可用性表示**能力**（不是本帧数值是否就绪）：能力一旦确认，可用性为粘性 `true`，此后的采样失败只把数值置 `null`，不改写可用性。能力从未确认或平台不支持时可为 `available=false` + `reason="collect_failed"`（平台不支持也可用 `unsupported`；当前版本无法区分能力缺失与暂时失败，统一用 `collect_failed`；`reason` 为自由文案，本版本新增建议取值 `collect_failed`）。
>
> `volumes` / `shm_dir` 的可用性同为**能力粘性**：一次成功枚举到卷 / 读到目录后可用性保持 `true`，此后单帧失败只把对应域置 `null`，不改写可用性。配置关闭 `sampling.groups.volumes` 时优先 `available=false` + `reason="disabled_by_config"`、`volumes` 为 `null`。`shm_dir` 没有独立配置组（页清理有效性是基础可观测项），始终采集。**单个卷不可读**不改写 `volumes` 的整体可用性——该卷以 `available=false` 进数组（§2.4），整体仍为 `available=true`；只有当一次卷都枚举不到时才整体标记 `collect_failed`。
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
    {"sensor": "coretemp#hwmon3:Package id 0", "chip": "coretemp", "label": "Package id 0", "celsius": 45.5},
    {"sensor": "coretemp#hwmon3:Core 0", "chip": "coretemp", "label": "Core 0", "celsius": 44.0},
    {"sensor": "acpitz#hwmon0:temp1", "chip": "acpitz", "label": "", "celsius": 27.8}
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
    {"mount": "/", "available": true, "total_bytes": 107374182400, "used_bytes": 42949672960, "used_ratio": 0.4},
    {"mount": "/data", "available": true, "total_bytes": 1099511627776, "used_bytes": 274877906944, "used_ratio": 0.25},
    {"mount": "/backup", "available": false, "total_bytes": null, "used_bytes": null, "used_ratio": null, "reason": "no_permission"}
  ],
  "disk_io": [
    {"device": "sda", "read_bytes_per_sec": 1048576.0, "write_bytes_per_sec": 524288.0,
     "read_iops": 15.0, "write_iops": 5.0, "latency_ms": 1.25, "queue_depth": 3,
     "utilization": 0.5},
    {"device": "nvme0n1", "read_bytes_per_sec": 0.0, "write_bytes_per_sec": 0.0,
     "read_iops": 0.0, "write_iops": 0.0, "latency_ms": null, "queue_depth": 0,
     "utilization": 0.0}
  ],
  "network": [
    {"interface": "eno1", "rx_bytes_total": 55647207322, "tx_bytes_total": 32143786457,
     "rx_packets_total": 60705115, "tx_packets_total": 48562128,
     "rx_errors_total": 0, "tx_errors_total": 0,
     "rx_drops_total": 47594, "tx_drops_total": 4,
     "rx_bytes_per_sec": 1048576.0, "tx_bytes_per_sec": 2097152.0,
     "rx_errors_per_sec": 0.0, "tx_errors_per_sec": 0.0,
     "rx_drops_per_sec": 0.0, "tx_drops_per_sec": 0.0, "is_virtual": false}
  ],
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
      "network": {"available": true},
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
- **饱和核判定**：单核使用率连续 `thresholds.cpu_sustain_samples` 次采样 **≥** `thresholds.cpu_sustain_used_pct / 100` 即标记 `saturated=true`；任一采样严格低于阈值即清零解除；不可用样本（`usage_ratio=null`）不改变连续计数；核序列变化重置全部计数。阈值与连续次数取自当前配置，运行期修改经 `SET_SYSMON_CONFIG` 即时生效（§3.1.1）；`sampling.groups.cpu=false` 时饱和判定暂停并从空状态开始。
- Windows >64 逻辑核的机器上每核序列可能不可用（处理器组限制）：此时 `cpu.cores` 标记不可用、中断累计计数输出 `null`；不得用组内数据冒充全系统每核序列。
- **本帧数值不得沿用上一帧**：某项能力可用但本帧数值未就绪或采集失败时，对应字段输出 `null`——每核序列整体未采到时 `cpu.cores` 保留已知核 index 条目、`usage_ratio` 全部 `null`、`saturated` 全部 `false`（从未获得过序列时 `cpu.cores` / `saturated_cores` 为空数组）；`availability` 的能力标记保持粘性，消费方据此显示"不可用"而不是旧数据。单个核的计数器不可用时该核 `usage_ratio=null`，不改变饱和判定的连续计数。
- **每物理盘 I/O 的计算语义**（契约 §2.4 `disk_io[]`）：
  - 采集层只给原始累计计数器：Linux 读 `/proc/diskstats`（扇区数按内核 ABI 固定 ×512 得字节；`ms_reading` / `ms_writing` / `io_ticks` 是 32 位毫秒计数器，换算为微秒后上报）；Windows 读 `IOCTL_DISK_PERFORMANCE` 的 `DISK_PERFORMANCE`（`ReadTime`/`WriteTime`/`IdleTime` 为 100ns 累计值，换算为微秒；`QueryTime` 是查询时刻的系统时间锚点，`QueryTime − IdleTime` 的差值才是累计忙时间）。
  - 派生值全部由纯函数从两次采样差值计算：吞吐/IOPS = Δ计数器 / Δ实际经过秒数（单调时钟测量）；`latency_ms` = Δ(读+写服务时间) / Δ(读+写完成数)；`utilization` = Δ忙时间 / Δt；`queue_depth` 直接取本帧瞬时读数（不做差值）。
  - **首次采样（无基线）输出全部派生字段 `null`**，不得用 0 冒充。基线按 `device` 匹配：本帧新出现的设备等同首次采样；消失的设备不再输出。
  - **计数器回绕按原生位宽处理**：采集层在每条原始计数器上携带该设备的回绕跨度（数据驱动，消费方不按平台分支）。计数类：Windows `ReadCount`/`WriteCount` 为 32 位 DWORD（跨度 `2^32`），Linux 为 64 位（跨度 0 = 不回绕，下降即重置）；时间类（微秒）：Linux 的 `ms_reading`/`ms_writing`/`io_ticks` 是 32 位毫秒计数器（跨度 `2^32 × 1000`），Windows 的 100ns 计数器为 64 位（0）；字节类：两平台均为 64 位（0）。仅当上次与本次值均落在跨度内且本次更小时按跨度回绕计算；其余下降视为计数器重置，本帧该字段 `null` 并在下一采样周期重建基线。32 位计数器上「回绕」与「重置」数值形态相同、无法区分，统一按回绕处理（与 CPU 计数器同一口径）；由此产生的单帧尖峰由下一帧的自然恢复收敛。
  - **采样窗口**：速率的分母是两次**成功采样**之间实际经过的时间；采集失败期间基线不推进，因此恢复后的首帧速率是跨越该缺口的窗口平均（口径明确，非伪造瞬时值）。磁盘 I/O 采集失败只把该域置 `null`，**不累加** `sampling.missed_ticks`（该标记表示计量 tick 整体被跳过，不是单个指标域失败）。
  - `utilization` 结果钳制到 `[0,1]`；若 Δ忙时间超过窗口的 1.5 倍，视为计数器口径异常（如 Windows `QueryTime` 受系统时间跳变影响），本帧输出 `null`，不得钳成 100%。窗口内完成请求数为 0 时 `latency_ms` 为 `null`（无定义）。
- **温度 / 每核频率采集语义**：两者都是瞬时 gauge，不做差值或聚合（温度单位为摄氏度，频率四舍五入到整数 MHz）；传感器集合与核集合可在帧间变化，按本次读数输出，不沿用上一帧。读取失败的单个传感器/核不进入数组；能力可用但本帧一个都没读到时输出 `null`。配置 `sampling.groups.temperature=false` / `sampling.groups.cpu_frequency=false` 时跳过采集，可用性为 `disabled_by_config`、数值 `null`。Windows 不提供温度与每核频率（恒 `null`），也不提供整体频率。`chip` 与 `label` 由采集层在读取传感器时一并给出（hwmon `name` 与 `tempN_label`），发送方不重新解析 `sensor`——单帧标识的拼接规则仍只在采集层一处（§2.4）。
- **内存 / 交换采集语义**：两者都是瞬时 gauge，每次计量刷新，不做差值或聚合，不沿用上一帧。Linux 读 `/proc/meminfo`（`MemTotal`/`MemAvailable`/`Buffers`/`Cached`/`SReclaimable`/`SwapTotal`/`SwapFree`）；`MemAvailable` 缺失、为 0 或大于总量时按 `MemFree + Buffers + Cached + SReclaimable` 回退并钳制到 `[0, total]`，异常与回退均记日志；回退也无法计算时本次内存采集失败（该域 `null`，可用性按能力粘性）。Windows 读 `GetPerformanceInfo`（`PhysicalTotal`/`PhysicalAvailable`/`SystemCache`/`CommitLimit`/`CommitTotal`）。
- **每网卡的计算语义**（契约 §2.4 `network[]`）：
  - 采集层只给原始累计计数器（`RawNetworkInfo`：收/发字节、包、错误、丢弃 + `is_virtual`），速率由纯函数从两次采样差值计算。
  - 六个速率字段 = Δ计数器 / Δ实际经过秒数（单调时钟测量）。**基线按 `interface` 匹配**：本帧新出现的网卡等同首次采样、速率全部 `null`；消失的网卡不再输出（热插拔/虚拟网卡增删因此天然不错位，不依赖数组下标）。
  - 计数器回绕按**原生位宽跨度**处理（数据驱动）：Linux `/proc/net/dev` 的计数为 64 位（跨度 0 = 不回绕，下降即重置）；Windows `MIB_IF_ROW2` 的计数为 64 位 `ULONG64`（跨度 0）。仅当上次与本次值均落在跨度内且本次更小时按跨度回绕；其余下降视为计数器重置，本帧该字段 `null` 并在下一采样周期重建基线。
  - **采样窗口**：分母是两次**成功采样**之间实际经过的时间；采集失败期间基线不推进，恢复后的首帧速率是跨越该缺口的窗口平均（口径明确，非伪造瞬时值）。网络采集失败只把该域置 `null`，**不累加** `sampling.missed_ticks`。
  - `hide_virtual` 在**快照构造**阶段按 `is_virtual` 过滤（不重新采集）：改动即时体现在下一帧快照（`SET_SYSMON_CONFIG` 后立即推送一次）。

### 2.8 阈值越限与日志

**语义**：资源读数与生效配置的阈值比较，得出一个**持续的越限状态**；该状态只通过**日志**与**UI 色标**表达，**不产生 `DZ_FRAME_NOTIFY_UI`**。理由（ADR 0034）：资源占用高在本机不专属本项目时是常态而非故障（swap 长期高位、其他服务占 CPU 均在预期内），而弹窗通道要留给需要立刻决策的故障——资源占用混入后使用者会对弹窗脱敏。

**判定对象与阈值**（阈值取当前生效配置 `thresholds` 段，§3.1；默认值即可零配置工作）：

| 判定对象 | 指标来源 | 进入越限 | 恢复 |
|---------|----------|---------|------|
| `memory_used` | 快照 `memory.used_ratio` | `used_ratio ≥ memory_used_pct / 100` | `used_ratio < memory_used_pct / 100` |
| `swap_used` | 快照 `swap.used_ratio` | `used_ratio ≥ swap_used_pct / 100` | `used_ratio < swap_used_pct / 100` |
| `volume_free` | 快照 `volumes[].used_ratio`（**全部卷**，逐卷一条） | `free_pct < volume_free_pct` | `free_pct ≥ volume_free_pct` |
| `cpu_saturated` | 快照 `cpu.saturated_cores` | 非空（存在至少一个饱和核，判定见 §2.7） | 空**且**本帧每核使用率可读（见下条，读不到不算恢复） |

- **卷的判定范围恒为本机全部容量可读的卷**，不存在「关注卷 / 关注路径」这一层：所有卷使用率一视同仁，非固定盘不入数组（§2.4）即不参与判定。`volume_free` 按 `mount` **逐卷**是一个独立判定对象，日志用挂载点标识（与 `volumes[].mount` 同口径）；`memory_used` / `swap_used` / `cpu_saturated` 每类各一个判定对象，日志不需要对象标识。`total_bytes = 0` 的卷无法计算百分比，不参与 `volume_free` 判定（它是**容量可读但总量为 0** 的合法事实，由快照 `available=true` + `total_bytes=0` 如实表达，与 `available=false` 的「读不到」是两件事）。
- `volume_free_pct` 是**空闲**百分比阈值：`free_pct = 100 − used_ratio × 100`。默认 10 表示「剩余低于 10% 即越限」，即使用率**严格高于** 90%（ADR 0034：主流实现均以百分比为主判据，绝对容量只作抑制项，故不设 `volume_free_bytes`）。**UI 色标是独立的展示规则，不是本判定的派生**：卷使用率 ≥70% 黄、≥90% 红，与顶栏徽章共用同一组常量（由前端共享模块导出，两处不得各自定义）；因此「恰好 90% 已用」会显示红色但不构成越限——色标用于扫读，越限用于日志，两者的边界不必逐点重合。**色标落点**：监控页对卷、内存、交换的使用率百分比共用同一组色标，顶栏 CPU / 内存 / 卷徽章同源；CPU 饱和另以每核 `saturated` 标记表达（§2.3）。
- `cpu_saturated` 是**整机一个**判定对象（不是每核一个），当前值取饱和核的 index 集合——每核的 `saturated` 标记本身仍在快照里（§2.3），日志只需能看出"何时开始/结束整机饱和"以及当时涉及哪些核。它的日志**不含单值阈值**：判定条件是 §2.7 的连续采样规则（见下表注），越限层只看「有没有饱和核」。
- 判定在每个**计量 tick** 进行（与历史记录同频），不跟随推送频率：默认 1s 判定、5s 推送，越限被发现的时间不超过一个计量间隔。
- **相等按表处理**：内存/交换"恰好等于阈值"算进入越限（`≥`）；卷"恰好等于阈值"不算越限（`free_pct < volume_free_pct`），且满足恢复（`≥`）——不在边界做额外容差。

**日志（走《帧契约：日志》）**：payload 与格式按契约 log，本条只规定级别与内容义务。级别用 spdlog 规范全称（`warn` 只是输入别名，日志输出为 `warning`）。

| 状态 | 级别 | 内容义务 |
|------|------|---------|
| 进入越限 | `warning` | 判定对象、当前值、阈值 |
| 仍越限（周期重记） | `info` | 判定对象、当前值、阈值、活动期已持续秒数 |
| 恢复 | `info` | 判定对象、当前值、阈值、活动期持续秒数 |

**注（`cpu_saturated`）**：该判定对象的判定条件是「每核使用率连续 `thresholds.cpu_sustain_samples` 次采样 ≥ `thresholds.cpu_sustain_used_pct`」（§2.7），没有可比的单值阈值；其日志三态的内容义务以 `saturated_cores=<indices>` 表达当前值，不再列阈值（其余判定对象照常给出当前值与阈值）。

- **进入与恢复各记一条**：一次活动期（进入 → 恢复）有明确的起点与终点，事后可从日志判断它何时开始、何时结束。
- **持续期按 `thresholds.log_repeat_sec`（默认 600s）重记**：同一次活动期内每满该间隔写一条「仍越限」记录，用于看出高水位持续了多久，而不是只有一条孤立的进入记录。重记计时用**单调时钟**（不受系统时间调整影响），基准是该活动期的进入时刻；`log_repeat_sec = 0` 表示不重记（只有进入与恢复两条）。
- 日志文本是英文、小写开头、无句号、`prose | k=v`（AGENTS 日志门禁），不是协议语义；消费方不得解析。

**静默清除（不是恢复）**：以下情况移除活动状态但**不写恢复日志**——清除原因不是"指标回到阈值以下"：

- 对应指标类别被配置关闭（`disabled_by_config`），如 `sampling.groups.volumes=false` 时的全部卷判定对象、`sampling.groups.swap=false` 时的 `swap_used`、`sampling.groups.cpu=false` 时的 `cpu_saturated`。
- 判定对象本身不再存在：某卷从 `volumes[]` 中消失（卸载/热拔），或本机交换分区被移除。
- **本帧读不到不伪造恢复**：能力仍在但本帧数值不可用（`memory`/`swap`/`volumes` 为 `null`，或 `used_ratio = null`）时**保留**活动状态、不写恢复日志——"读不到"不是"已恢复"；恢复只在真正读到低于阈值时发生。恢复日志的持续秒数按活动期起点计算，因此跨越采集缺口的恢复记录仍如实反映总持续时长。
- **`cpu_saturated` 不得按「`saturated_cores` 为空」判恢复**：§2.7 规定每核序列整体未采到时 `saturated` 全部 `false`，若直接按「空即恢复」处理，CPU 采集失败会被误记成恢复。判据必须是**本帧每核使用率确实可读**（至少一个核 `usage_ratio != null`）：读不到时保留活动状态，`saturated_cores` 为空只作为"本帧无信息"而非"已恢复"。`sampling.groups.cpu=false` 仍按静默清除处理（配置关闭不是恢复）。

**没有的东西（明确不做）**：

- 不产生 `DZ_FRAME_NOTIFY_UI`，不弹窗、不发 toast（ADR 0034 否决了 toast：它仍是主动推送，盯盘时同样干扰专注）。
- 不进快照：没有 `alerts[]` 之类的活动集合字段（§2.4），活动状态不跨帧可见，只有日志与 UI 色标。
- 不写历史库：历史只记录指标事实，越限可由历史事实重新推导；`thresholds` 也不进 `availability.metrics` 键集合（它不是指标能力）。
- 没有冷却与恢复限流：不再有通知可限流，防刷屏由「只在进入/恢复各一条 + 按间隔重记」保证。

**`dzsysmon` 的 `NOTIFY_UI` 保留范围**：只有**真故障**——历史库不可用（§3.1 `history` 段降级语义）与配置下发失败（§3.3 四件套）。两者都是一次性、需人处理的故障，级别与弹窗语义按契约 notify-ui。

---

## 3. 配置（SET/RTN_SYSMON_CONFIG）

**语义**：运行期修改系统监控配置（RFC 7386 merge patch），并回推**全量**生效配置。配置持久化为 `$DZTRADER_HOME/configs/dzsysmon.json`（进程缺失时自动生成默认值；仓库不提交模板文件）。日志配置走《帧契约：日志》的 `SET/RTN_LOG_CONFIG`，**不在本帧 payload 内**。

**数据流**：形态 1——dzweb → `dzsysmon`（`instance_id=dzsysmon`）→ 校验/应用/持久化 → `RTN_SYSMON_CONFIG` 全量回推 → dzweb 镜像 `sysmon_config` 域 → WS 消息 `sysmon_config`。前端入口为 REST `PUT /api/sysmon/config`（写入）与 `GET /api/sysmon/config`（读取镜像中的当前生效配置），端点细节见契约 rest §2.8；监控页「配置」区展示当前生效配置并提供 admin 编辑入口。本契约落帧语义与接收方处理（校验/持久化/运行期副作用/定时器即时重排）。

**SET payload**：JSON object（merge patch；object 内嵌套对象按键合并、数组整体替换）。任意位置出现 `null` 为校验失败；空对象 `{}` 为无操作但**仍回 RTN**。未知字段忽略（总则 §8）。

**RTN payload**：全量配置（`SysmonConfig`，即当前生效值；无 `error` 字段，失败经四件套表达）。

### 3.1 配置 schema

`sampling`：

| 字段 | 类型 | 默认 | 约束 | 说明 |
|------|------|------|------|------|
| `measure_interval_ms` | int | 1000 | `[100, 60000]` | 计量间隔；速率型指标的采样窗口 |
| `record_interval_sec` | int | 60 | `[1, 86400]` | 记录间隔（历史写入窗口宽度，由 #78 消费；记录窗口同时受 `phase_offset_sec` 影响，见 §3.1.1 第 3 步） |
| `phase_offset_sec` | int | 3 | `[0, 86400]` | 记录 tick 相位偏移，避开整点附近其它任务（#78 消费；桶计算时按 `phase_offset_sec % record_interval_sec` 取模对齐——**存储、RTN 与 UI 保留配置原值，不在配置层钳制**；默认配置下窗口起点为每分钟第 3 秒，**不落在整数分钟边界**） |
| `groups` | object\<string,bool\> | Linux：全部 true；Windows：`temperature`/`cpu_frequency` 为 false | 值必须 bool | 启用的指标大类：`cpu`/`memory`/`swap`/`volumes`/`disk_io`/`network`/`temperature`/`cpu_frequency`。`temperature` 与 `cpu_frequency` 属 B 组：Linux 默认开（有 hwmon / cpufreq 时）；Windows 默认关（平台上不可靠或不存在，需显式开启；本版本 Windows 采集器仍报告 `unsupported`）。缺少的键按 true 处理。各键语义见 §3.1.1 |

`disk`：`{devices:string[]}`（元素非空）。

- `devices` 为**磁盘 I/O 设备范围**：空数组 = 全部整物理盘；非空时只输出 `device` 与列表**精确匹配**的 `disk_io[]` 条目（标识与快照 `disk_io[].device` 同源：Linux 整盘名如 `sda` / `nvme0n1`，Windows `PhysicalDrive<N>`）。列表中的名字匹配不到任何设备只是不输出，**不导致校验失败**。过滤发生在**快照构造**阶段（与 `network.hide_virtual` 同款）：运行期修改不触发重新采集、不重置速率基线，改后立即推送的一帧即体现新范围；历史记录的维度数据由同一快照构造路径观测，与实时快照同口径。过滤后为空数组仍 `available=true`（"范围内没有盘"与"采集失败"是不同事实）。
- `disk` 段**不含** `watch_paths`：卷容量的监控范围恒为本机全部卷，不存在「关注哪一卷」的配置层（§2.4、ADR 0034）。

`network`：`{hide_virtual:bool}`，默认 `true`（隐藏虚拟网卡：回环、桥接、veth、容器网桥、隧道与 VLAN 逻辑接口等——判定依据是「不由硬件设备支撑」，见 §2.4）。该开关在**快照构造**阶段过滤，`SET_SYSMON_CONFIG` 成功后立即推送一帧使改动即时可见（§4 触发 4）；历史记录的维度数据由同一快照构造路径观测，与实时快照同口径。

`history`：`{enabled:bool, raw_retention_hours:int, max_retention_days:int, max_bytes:int}`，默认 `true/24/30/2147483648`；`raw_retention_hours ∈ [1,720]`、`max_retention_days ∈ [1,3650]`、`max_bytes ≥ 0`（0 = 不限制）。由 #78 消费：`enabled=false` 时不写历史（仅实时数据，不建库文件、不起落盘线程）；保留与压缩规则见 #41 spec「历史存储」。历史库 schema 与行编解码的唯一真相源是 `libs/sysmonstore`，**不在本契约范围内**（历史查询不走帧，dzweb 以只读方式直接打开历史库）。`enabled` **支持运行期切换**（§3.1.1）：true→false 时先 flush 当前记录窗口再关库；false→true 时建库并从空窗口开始（**不**写 `restart` 标记——进程没有重启）；建库失败只记日志并 `NOTIFY_UI` 报错、实时链路继续，生效配置保持 `true`（配置是意图，失败是运行期降级，不伪造回滚）。`raw_retention_hours` / `max_retention_days` / `max_bytes` 运行期修改在**下一维护周期**生效。

`realtime`：`{push_interval_sec:int}`，默认 `5`，`[1, 3600]`；与计量频率解耦（事件通道每次写入会唤醒全部注册进程）。

`thresholds`（**越限判定阈值，只产生日志与 UI 色标，不产生 UI 通知**，§2.8；本段原名 `alerts`，旧配置文件自动迁移见下）：

| 字段 | 类型 | 默认 | 约束 | 说明 |
|------|------|------|------|------|
| `cpu_sustain_used_pct` | number | 90 | `[0,100]` | 单核持续饱和阈值（§2.7） |
| `cpu_sustain_samples` | int | 10 | `[1,3600]` | 持续饱和的连续采样次数（§2.7） |
| `memory_used_pct` | number | 90 | `[0,100]` | 内存使用率越限阈值 |
| `swap_used_pct` | number | 50 | `[0,100]` | 交换使用率越限阈值 |
| `volume_free_pct` | number | 10 | `[0,100]` | 卷**空闲**百分比阈值；低于该值即越限 |
| `log_repeat_sec` | int | 600 | `[0,86400]` | 持续越限期的日志重记间隔；0 = 不重记 |

- 全部阈值键都住在**同一段**，使用者一次就能看全所有阈值，不必分辨"哪些会打扰我"（§2.8：它们都不打扰）。
- `cpu_sustain_used_pct` / `cpu_sustain_samples` 用于饱和核判定（§2.7）；其余键用于越限判定与日志节奏（§2.8）。所有键**运行期修改即时生效**（`SET_SYSMON_CONFIG`，§3.1.1），默认值即零配置可用。
- **没有 `volume_free_bytes`**：绝对容量阈值已删除（ADR 0034）。主流实现均以百分比为主判据，绝对容量只作抑制项；`OR` 语义下的 1 GiB 阈值会让总容量小于 1 GiB 的卷（ESP、`/boot`）在空闲时也永久越限。
- **没有 `cooldown_sec`**：不再有通知可限流，防刷屏由「进入/恢复各一条 + 按 `log_repeat_sec` 重记」保证（§2.8）。
- **旧配置迁移**：读到旧 `alerts` 段时把可映射的键值搬到 `thresholds`、丢弃不可映射的 `volume_free_bytes`、写回配置文件、记一条日志，**不报错、不阻塞启动**。默认配置生成器直接产出 `thresholds` 形状（仓库不提交模板）。

### 3.1.1 运行期生效语义

配置经 §3.3 校验与原子持久化成功后，接收方按**固定顺序**应用副作用（顺序是语义的一部分）：

1. **内存镜像整体替换**为新配置（失败路径不发生这一步，见 §3.3）。
2. **定时器即时重排**：取消当前 pending 的计量与推送等待，从**当前时刻**按新间隔重新排定，不等待当前周期结束。`measure_interval_ms` 与 `push_interval_sec` 因此立即生效；下一个 tick 的时刻由重排时刻决定（不是"旧 deadline + 新间隔"）。
3. **记录窗口重配**：`record_interval_sec` / `phase_offset_sec` 从**下一窗口**起生效；进行中的窗口按它开始时的宽度落库（不切碎历史，避免同一桶宽度在同一窗口内变化）。
4. **保留策略参数发布**：`history` 的保留/上限参数交给落盘线程，下一维护周期生效。
5. **`history.enabled` 切换**：true→false 先 flush 当前窗口再关库/停线程，并重置时钟跳变检测基线（重开后的首个 tick 不得把关闭时长误报成 `clock_jump`）；false→true 建库并起线程（不回写 `restart` 标记）。切换失败不阻止其它副作用，按本节 `history` 段的降级语义处理；**开启失败后配置保持 `true`，后续任意 SET 会重试建库**（无需先关再开或重启进程）。
6. 回推全量 `RTN_SYSMON_CONFIG`（成功与失败都回推，见 §3.2/§3.3），成功时再按 §4 触发 4 立即推送一帧快照——因此**只影响快照构造**的配置（`network.hide_virtual`、`disk.devices`）无需等到下一个推送周期。`thresholds` 的改动在下一次计量 tick 起作用（越限判定与历史记录同频），最多一个计量间隔后可见新的日志行为。

**`sampling.groups` 各键语义**（关闭后重新开启一律从**新基线**开始，不得跨关闭期计算使用率/速率；关闭期间**不累加** `sampling.missed_ticks`——配置关闭不是采集缺口）：

| 键 | 覆盖的指标 | 关闭后 |
|---|---|---|
| `cpu` | `cpu.total` / `cpu.cores` / `cpu.load_average` / `cpu.context_switches`（含 `interrupts_per_sec`）与饱和核判定 | 跳过 CPU 采样与负载读取；上述可用性 `disabled_by_config`，`cpu` 区块数值为 `null`/空数组，`saturated_cores` 为空；`system.logical_cores` 保持系统信息原值。`cpu.frequency` 由独立键控制，不受影响。`cpu_saturated` 越限状态静默清除（§2.8） |
| `memory` | `memory` | 跳过采集；可用性 `disabled_by_config`、`memory` 为 `null`；`memory_used` 越限状态静默清除（§2.8） |
| `swap` | `swap` | 跳过采集；可用性 `disabled_by_config`、`swap` 为 `null`；`swap_used` 越限状态静默清除（§2.8） |
| `volumes` | `volumes` | 跳过卷容量采集；可用性 `disabled_by_config`、`volumes` 为 `null`；对应卷越限判定静默清除（§2.8） |
| `disk_io` | `disk_io` | 跳过采集；可用性 `disabled_by_config`、`disk_io` 为 `null` |
| `network` | `network` | 跳过采集；可用性 `disabled_by_config`、`network` 为 `null`（`hide_virtual` 只影响输出条目，见 §2.5） |
| `temperature` | `temperature` | 跳过采集；可用性 `disabled_by_config`、数值 `null`（前端整块不渲染） |
| `cpu_frequency` | `cpu.core_frequencies` / `cpu.frequency` | 跳过采集；可用性 `disabled_by_config`、数值 `null` |

配置关闭某大类时**实时数据其余部分照常**：快照继续按周期推送、历史继续记录（该域落 NULL，读取方显示"不可用"而不是 0）、其它指标不受影响。

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
4. **`SET_SYSMON_CONFIG` 成功后立即推送一次**（失败不推）：只影响快照构造的配置（如 `network.hide_virtual`）必须在下一次周期推送之前生效，否则一次开关操作最长要等一个推送周期（默认 5 秒）才可见。定时器重排只影响计量与推送节奏，不代表快照内容已刷新。

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

`GET /api/sysmon/snapshot`：读取 dzweb 镜像中的当前快照（任意已认证用户）。镜像尚未就绪（dzsysmon 未上报）→ `503 {"error":"sysmon snapshot not available"}`。

`GET /api/sysmon/config`：读取 dzweb 镜像中的**当前生效配置**（任意已认证用户；`sysmon_config` 域本来就是广播给所有 WS 客户端的，读权限与快照一致）。未就绪 → `503 {"error":"sysmon config not available"}`；就绪 → `200` + 全量配置（`SysmonConfig`，形状与 RTN payload 相同）。本端点只读镜像、不产生帧往返。

`PUT /api/sysmon/config`：运行期配置 merge patch（admin）。请求 body 原样透传为 `SET_SYSMON_CONFIG` 的 payload，响应 `200 {"ok":true,"message":"dispatched"}` 只表示"已写事件通道"——校验结果与生效值以 WS `sysmon_config`（`RTN_SYSMON_CONFIG` 全量回推，失败回推旧值）为准。`shm` 未就绪 → `503 {"error":"shm not available"}`；body 非 object/非法 JSON → `400 {"error":"bad request"}`；非 admin → `403 {"error":"admin required"}`。

前端消费方式：监控页「配置」区以 WS `sysmon_config` 镜像为唯一生效信号，首次进入时用 `GET /api/sysmon/config` 补拉镜像；表单提交只发差异 patch，提交后保持 pending 直到收到 WS `sysmon_config`（REST 200 ≠ 已生效）。

端点细节见契约 rest。

### 6.1 历史查询（GET /api/sysmon/history）

**语义**：`dzweb` 只读打开历史库并经 REST 提供历史曲线，**不走帧**——历史库由 `dzsysmon` 唯一写入、`dzweb` 只读，schema 与行编解码的唯一真相源是 `libs/sysmonstore`。因此本端点无帧、无镜像、无 WS 广播：每次请求即一次只读查询，不产生跨进程交互。

**Query 参数**（全部可选，除 `metric` 外均有默认值）：

| 参数 | 类型 | 默认 | 约束 | 说明 |
|------|------|------|------|------|
| `metric` | string | `cpu.usage_ratio` | 必须是已注册指标键：`kScalarMetrics` 的逻辑键（如 `cpu.usage_ratio`、`memory.used_ratio`），或 `<domain>.<metric>` 形式的维度指标（`domain` 取 `schema.h` 的 `k*Domain`，`metric` 属于该域，如 `network.rx_bytes_per_sec`） | 要查询的指标 |
| `from` | int | `to − 6h` | `0 < from < to`（Unix 秒） | 时间范围起点（含） |
| `to` | int | 当前墙钟秒 | 同上 | 时间范围终点（**不含**），与历史桶的 `[since, until)` 语义一致 |
| `level` | string | `auto` | `auto` / `raw` / `minute` | 桶层级：`raw` = 原始记录（`level=0`）、`minute` = 一分钟聚合（`level=1`）；`auto` 按 §6.1.1 选择 |
| `max_points` | int | `1500` | `[10, 20000]` | 每个序列的点数上限；超出时按 §6.1.2 前移窗口 |
| `dimension` | string | 空 | 维度指标**必填**；标量指标必须为空/省略 | 维度标识（网卡名 / 盘符 / 挂载点 / 核序号 / sensor 标识）。本版本每次查询返回**单条**序列（多维度叠加不在本版本范围） |

参数非法（`metric` 未注册、维度指标缺 `dimension`、标量指标给了 `dimension`、`from >= to`、非数字/溢出、`level` 不在枚举内、`max_points` 越界）一律 `400 {"error":"bad request"}`——**不做静默钳制**：前端拿到的 200 必然对应一个语义正确的查询。

**响应**（`200`）：

| 字段 | 类型 | 说明 |
|------|------|------|
| `available` | bool | `true` = 历史库可读；`false` = 不可用（§6.1.3），此时除 `reason` 外不返回任何数据字段 |
| `reason` | string \| 省略 | 仅在 `available=false` 时出现，取值见 §6.1.3 |
| `level` | string | 实际使用的层级（`raw` / `minute`），回显 `auto` 的选择结果 |
| `metric` | string | 回显请求的指标键 |
| `from` / `to` | int | 回显**生效**的时间范围（Unix 秒，`[from, to)`）；`truncated=true` 时 `from` 为实际返回首点的桶时刻 |
| `interval_sec` | int \| null | 返回序列的桶宽（秒）：`minute` 层级恒为 `60`；`raw` 层级的**标量**指标取最新一行记录的 `interval_sec`（记录间隔可运行期调整，最新行代表当前生效桶宽），**维度**指标的原始行没有该列，取相邻桶时刻的最小正间隔（桶网格等距，缺口只会拉大间隔；**仅两点且两点之间恰为缺口时无法与真实桶宽区分**，该段缺口不会被检出——这是从数据反推桶宽的固有上限，补齐需要给长表加 `interval_sec` 列，属后续 schema 版本）；范围内无任何桶时为 `null`（无桶即无桶宽，不得猜一个值） |
| `truncated` | bool | `true` = 因 `max_points` 上限窗口被前移（§6.1.2），此时 `from` 是前移后的生效起点 |
| `series` | array | 恒 1 条序列（`series[0]`）；保留数组形状以便将来多维度叠加是纯 additive 扩展 |
| `marks` | array | 时间范围内的历史标记（§6.1.4），按 `ts` 升序；**不受 `max_points` 约束**（见下） |
| `gaps` | array | 已归并的序列内不连续区间（§6.1.4），按 `from` 升序、互不重叠 |

`series[]`：`{dimension:string, points:array}`；`dimension` 回显请求的维度（标量指标为空串）。

`points[]`：`{ts:int, min:number|null, max:number|null, avg:number|null, last:number|null, n:int|null}`。

- `ts` 为该点所属桶的起始时刻（Unix 秒，UTC）；宽表与长表同一口径。
- `min`/`max`/`avg`/`last` 为该桶内的聚合量，`n` 为有效样本数。**尖峰只由 `max` 保留**（与写入侧同一语义），因此前端「最小 / 最大包络」直接取 `min`/`max`。
- **该桶内该指标无有效样本时四值与 `n` 全为 `null`**，不得用 0 冒充（与实时快照同一纪律：0 是合法的真实读数，不可用不是 0）。

**`available=true` 的充分条件**：库文件存在、只读打开成功、schema 版本等于 `kSchemaVersion`。

**空结果**：范围内没有任何桶时 `series[].points` 为空数组、`available=true`、`reason` 省略——「范围内没有数据」与「历史不可用」是不同事实，前者由前端显示「该时间范围无数据」。

#### 6.1.1 层级自动选择（`level=auto`）

按 `to − from`（范围宽度）选取：**≤ 1 小时**选 `raw`（原始记录最细，1 小时内点数有界），**> 1 小时**选 `minute`（一分钟聚合是唯一压缩层级；更宽范围用原始记录会超点数上限，聚合正是为此存在）。

`auto` 的**回退规则**：选中的层级在范围内无任何行时，回退到另一层级再查一次，并把**实际使用的层级**回显在 `level` 字段中（回退对使用方可见，前端摘要显示「粒度 …」，不静默掩盖）。

必须回退的原因：一分钟聚合**只在 `sampling.record_interval_sec < 60` 时发生**（默认 60 ⇒ 默认部署从不压缩 ⇒ `level=1` 永远为空）。若死守「> 1 小时选 `minute`」而不回退，默认配置下所有超过 1 小时的范围——包括默认的 6 小时视图与 24 小时 / 7 天预设——都会返回空序列，功能整体失效。

显式 `level`（`raw` / `minute`）**不回退**：用户明确要的就是该层级，无数据就如实返回空序列（回退只属于 `auto` 的层级选择）。

#### 6.1.2 点数上限（`max_points`）

`max_points` 限制**每个序列**的点数。请求范围内的桶数超过该上限时，服务端**保留最新**的 `max_points` 个桶，回 `truncated=true`，`from` 回显为**实际返回首点的桶时刻**（生效起点）。

- 保留最新而非最早：使用者看的是「最近一段时间发生了什么」，截掉旧数据比截掉新数据有用。
- 截断判定必须**精确**：读取方按时间降序多取一行（`max_points + 1`）以判定「范围内是否还有更旧的桶」。**不得**按「范围宽度 ÷ 桶宽」估算——那会把「起点不与桶边界对齐」和「该段本就没有数据」误判成截断，从而丢掉本可完整返回的数据。
- **不**跨桶合并、**不**插值、**不**按时间重采样：合并会造出历史库里不存在的桶边界，`min`/`max` 包络在跨桶合并后语义改变（不再是记录窗口的包络）。宁可如实返回更短的窗口，也不伪造粒度。
- 前端据 `truncated=true` 显示「已显示最近一段」并把时间范围控件同步到生效 `from`；需要完整范围时由前端缩小时间范围或显式请求更粗层级。

#### 6.1.3 不可用降级

历史库缺失、schema 版本不符或文件损坏时**不得崩溃、不得返回伪造数据、不得返回 5xx**：回 `200` + `{"available":false,"reason":<code>}`。`reason` 取自 `libs/sysmonstore` 的 `HistoryAvailability` 稳定 ASCII 码：

| `reason` | 触发条件 | 前端文案 |
|----------|----------|----------|
| `not_found` | 库文件不存在（`history.enabled=false` 或尚未落过盘） | 历史不可用 · 历史记录未启用或尚无数据 |
| `schema_mismatch` | 库内 schema 版本 ≠ `kSchemaVersion` | 历史不可用 · 历史库版本与当前程序不符 |
| `open_failed` | 文件存在但打开失败（权限、损坏、非 SQLite 文件） | 历史不可用 · 历史库无法读取 |

三者与 `HistoryAvailability` 一一对应；`ok` 不出现在 `reason` 中（可用时该字段省略）。**查询期间**的 SQL 失败（库被外部改动等）同样降级为 `available=false` + `open_failed`，不把半截数据当结果返回。

#### 6.1.4 标记与缺口（`marks` / `gaps`）

`marks[]`：`{ts:int, kind:string, detail:string}`，直接投影历史库的 `sysmon_mark` 表（`kind` 取 `sample_gap` / `restart` / `clock_jump`，见 `schema.h`；`detail` 为自由文本，可为空串）。`marks` 是事实清单，供前端渲染标记与 tooltip 文案。

`gaps[]`：`{from:int, to:int, kind:string}`，把**序列内部的不连续**归并成区间，便于前端在图上直接画缺口底纹：

- 判定依据是**点之间的连续性**（外部可观察事实，不依赖标记）：相邻两点满足 `cur.ts − (prev.ts + interval_sec) > 0` 时，二者之间存在一段无桶区间，即一个 `gap`：`from = prev.ts + interval_sec`、`to = cur.ts`。这是「采集中断 / 进程重启 / 时钟跳变」在数据上的统一表现——标记解释原因，缺口描述事实。
- 归并：同一段不连续里出现多条标记时合并为一个 `gap`，不按标记逐条拆分。
- `kind` 取解释该缺口的原因：落在 `[from, to)` 内的标记中 `restart` 优先于 `sample_gap`（进程重启是更强的解释）；无标记落在其中时为 `unmarked`——「数据确实缺了，但没有标记解释」，这正是需要暴露给使用者的异常，**不得**默默当作连续。
- 范围边界处（首个点之前 / 末个点之后）**不**生成 `gap`——那是请求范围之外，不是序列内不连续。
- `clock_jump` **不**产生 `gap`（跳变期间数据可能仍连续，只是时间轴被步进），前端按 `marks` 显示跳变标记而不是缺口。
- `available=false` 时 `gaps` 为空数组（无数据即无从判定不连续）。

`series[].points` 为空或单点时 `gaps` 为空数组。

`marks` **不随 `max_points` 截断**：标记解释缺口原因，若与记录点一起被截断，缺口会被错报成 `unmarked`——把「有解释的停机」误报成「无人解释的异常」，正好与本节目的相反。标记数量由写入侧决定（每次重启/跳变/缺口一条），量级远小于记录点，因此给独立上限，且该上限必须显著高于 `max_points` 的合法上界（否则用户把 `max_points` 调到上限附近时标记仍会被静默截断，缺口又被错报成 `unmarked`，与本节目的相反）。

---

## 7. 校验（除总则 §8 外的特殊规则）

- 快照中 `null` **仅允许**出现在本文档 §2 明确标注 `| null` 的位置；其余位置出现 `null` 为校验失败。
- `used_bytes` 必须等于 `total_bytes − available_bytes`（`memory`）；`used_bytes ≤ total_bytes`（`memory`/`swap`/`volumes`）；`memory.cached_bytes` / `memory.buffers_bytes` 缺失或 `null` 合法，出现时须为非负整数且不大于 `total_bytes`。
- `volumes[]`：`mount` 非空且帧内唯一；`available=true` 时 `total_bytes`/`used_bytes` 必须为非负整数且 `used_bytes ≤ total_bytes`、`used_ratio` 为 `[0,1]` 或 `null`，且**不得**出现 `reason`；`available=false` 时 `reason` 必须非空、`total_bytes`/`used_bytes`/`used_ratio` **必须**为 `null`（禁止用 0 冒充）。
- 发送方**不得**输出 `alerts` 或 `watch_paths` 字段（ADR 0034 已删除两者）。消费方无需校验其缺席：旧版本发送方仍携带时按未知字段忽略（见本节「未知字段忽略」条），新旧版本短暂并存不报错。
- `disk_io[]`：`device` 非空且帧内唯一；除 `device` 外的七个字段为 `null` 或数字——吞吐/IOPS/延迟/队列深度为 `>= 0`，`utilization` 为 `[0,1]` 内的比率或 `null`（首帧全 null 的条目合法）。
- `network[]`：`interface` 非空且帧内唯一；八个 `*_total` 必须为非负整数且**必须存在**（累计计数器没有"不可用"语义）；六个 `*_per_sec` 为 `null` 或 `>= 0` 的数字；`is_virtual` 必须为 bool。`hide_virtual = true` 时若出现 `is_virtual = true` 的条目为校验失败（发送方义务：过滤在发送端完成）；`hide_virtual` 的值不在快照内，故该条校验由发送方自查、消费方只做形状校验。
- `temperature[]`：`sensor` 非空且帧内唯一、`celsius` 为数字；`chip` 与 `label` 是 **additive 可选字段**（#126）——存在时 `chip` 必须为**非空**字符串、`label` 必须为字符串（允许空串，表示该通道没有标签），缺失合法（旧版本发送方）。`sensor` 的值不因新增两者而改变（历史库维度键）。
- 比率为 `[0,1]`；字节/计数为非负整数；`boot_time_unix_sec`/`uptime_sec` 非负。
- `availability.metrics` 中 `available=false` 时 `reason` 必须存在且非空；`available=true` 时不得出现 `reason`。
- 未知字段忽略（前向兼容）。

---

## 8. 范围与明确不做

- **不支持容器内采集**：容器内 `/proc` 报宿主容量，会给出错误的容量基线；部署文档写明（`docs/components/dzsysmon.md`），不做 cgroup v1/v2 感知。
- **Windows 不提供温度与每核频率**（也不提供整体频率）：Windows 上温度需 WMI/厂商驱动、每核频率无可靠 API，两者恒为不可用；本版本不实现「整体频率 + 口径标注」形态。
- **磁盘 I/O 为整盘粒度**：不提供分区级 I/O。本版本的 `device` 只有物理盘标识（Windows `PhysicalDrive<N>`）；**卷/盘符 ↔ 物理盘映射**由后续票 #103 提供（ADR 0024 Consequences 的映射要求，本票不交付）。Windows 依赖性能计数器（diskperf）与访问权限，读不到时按 §2.4 报告不可用，不做提权或反复重试。
- **磁盘 I/O 无阈值越限消费**：本契约只定义实时快照域；历史写入（#78）与阈值越限（§2.8，只覆盖内存/交换/全部卷剩余空间/CPU 饱和）不消费磁盘 I/O。磁盘 I/O 的维度指标（每盘吞吐/IOPS/延迟等）**可经 §6.1 历史查询**（`metric=disk_io.<metric>`），但阈值越限不在本版本。
- **网络为整网卡粒度**：不提供每进程/每连接/每协议流量；不做虚拟网卡的自动隐藏策略推断（`is_virtual` 只用结构性依据，不看名字前缀），也不提供 Linux 网卡流量分类（`tc` 队列）与 Windows 适配器绑定细节。
- **网络无阈值越限消费**：本契约只定义实时快照域。`hide_virtual` 与完整配置编辑入口见 §3（REST `PUT /api/sysmon/config` → `SET_SYSMON_CONFIG`；监控页「配置」区下发差异 patch）。网络的维度指标可经 §6.1 历史查询（`metric=network.<metric>`）。
- **阈值越限范围**：只判定内存使用率、交换使用率、全部卷剩余空间以及 CPU 饱和四类（§2.8），且**只写日志 + UI 色标**，不产生 `NOTIFY_UI`、不进快照、不写历史库；**不做**越限历史/越限管理界面、确认与静默、外部通道（邮件/webhook）、进程级指标越限，也不为磁盘 I/O 与网络出越限。历史库只记录指标事实。
- 历史查询不走帧（dzweb 只读历史库，§6.1）；进程级指标、GPU、SMART、风扇、电池、登录用户列表不在本契约范围。
- **历史查询不做跨桶重采样**：服务端只按真实桶返回，点数超限时前移窗口（§6.1.2），不在服务端合并桶或插值。历史视图的时间范围/粒度选择、包络与标记渲染在前端，视觉细节不在本契约。
- **历史视图为独立视图**：监控页在「实时」与「历史」之间切换，切换只影响 REST 查询与渲染，**不影响 WS 连接与实时快照镜像**（实时 store 继续接收 `sysmon_snapshot`）——历史视图不订阅任何新 WS 消息。
- **实时内联趋势（sparkline）不新增接口（#127）**：监控页在标量指标旁展示最近 15 分钟趋势，数据源**只**是 §6.1 的历史查询（每次进入页面按 `max_points=60` 补齐，历史库在运行期才建立时下次进入会重试）+ 既有 WS `sysmon_snapshot` 增量推进，不新增帧、不新增 REST 端点、不改快照形状。**末点与旁边的实时数字同源**（同一次快照、同一个值，不是另一次采样）。
  - **时间窗口径如实（#142）**：只有实际覆盖满 15 分钟才标注「最近 15 分钟」；历史范围内无桶（`interval_sec = null`，如新装机器 / 刚启用历史）时按**实时点跨度**（首个实时点至最新点）标注实际覆盖；历史有桶但首个桶晚于生效 `from` **超过一个桶宽**（历史刚启用、窗口内只有后半段桶）时，按**首个桶起点**计实际覆盖（首个桶滞后不超过一个桶宽属桶粒度，仍按生效 `from` 计）；`truncated = true`（§6.1.2：`record_interval_sec < 15` 时 `max_points=60` 只能覆盖 `60 × interval_sec` 秒）时按**生效 `from` / `to`** 标注实际覆盖，并明确「点数超限，仅显示最近一段」（与历史视图同口径）——**不**通过放大 `max_points` 或跨桶重采样填满窗口（§6.1.2 的「不做跨桶重采样」同样适用于 sparkline）。
  - **单指标补拉降级（#142）**：补拉失败或 `available=false` 时，若该指标上一轮已有可用序列则**保留序列并标记未刷新**（不清空），实时增量继续推进；**首次进入即不可用**时仍**静默不渲染**，数值照常显示（降级不是错误提示）。补拉**成功**时同一时段以历史桶为准，历史**首桶之前 / 末桶之后**的实时点保留（补拉不得清空或缩短已推进的实时趋势）。
  - 维度指标（每核 / 每盘 / 每网卡 / 每传感器）第一版不做 sparkline——维度查询需要 `dimension` 参数，与「维度历史 UI 选择器不在本版本」同一范围。
- **维度历史的 UI 选择器不在本版本**：§6.1 的 REST 端点支持全部指标（含 `<domain>.<metric>` 维度指标，需显式 `dimension`），但监控页的指标下拉只列标量指标（无需维度即可查询）。维度指标（每核 / 每卷 / 每盘 / 每网卡 / 每传感器）已可经 REST 查询，UI 的维度选择器随后续票；选择器不出现**不得**被理解为「维度历史未实现」。
- **时间范围选择为相对预设**：监控页提供「最近 N」预设（15 分钟 ~ 7 天），不提供绝对起止时间选择器；§6.1 的 `from` / `to` 参数支持绝对时刻，供 REST 调用方使用。
