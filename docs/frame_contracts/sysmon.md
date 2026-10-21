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

**语义**：推送当前实时快照。首版只填系统信息与 CPU 总使用率，其余域按本文档定稿的形状输出 `null` 并在可用性映射中标记不可用；已列出的指标域后续只填值、**不改形状**（新增指标类别属 additive 扩展，按总则 §11.3 同步契约、类型与前端 schema；消费方忽略未知字段）。

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
| `memory` | object \| null | 内存；`null` = 本版本未采集/不可用（见可用性映射） |
| `swap` | object \| null | 交换分区/页文件 |
| `volumes` | array \| null | 卷容量 |
| `disk_io` | array \| null | 每物理盘 I/O |
| `network` | array \| null | 每网卡收发 |
| `shm_dir` | object \| null | 共享内存目录占用 |
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
| `logical_cores` | int | 逻辑核数（> 0） |
| `boot_time_unix_sec` | int | 启动时间（Unix 秒） |
| `uptime_sec` | int | 运行时长（秒），由快照生成时刻与启动时间计算 |

### 2.3 cpu

| 字段 | 类型 | 说明 |
|------|------|------|
| `total.usage_ratio` | number \| null | 总使用率 `[0,1]`；`null` = 首次采样/计数器不可用（不得用 0 冒充） |
| `cores` | array | 每核使用率（不可用时为空数组）：`{index:int, usage_ratio:number\|null, saturated:bool}` |
| `saturated_cores` | array\<int\> | 当前处于持续饱和状态的核 index 集合 |
| `load_average` | object \| null | `{one,five,fifteen: number\|null, semantic: string}`；`semantic` 说明口径（Linux `"load_average"`；Windows `"processor_queue"` 代理量）。**两者不是同一个量，前端不得同轴比较** |
| `context_switches_per_sec` | number \| null | 每秒上下文切换（速率型，不可用为 null） |
| `interrupts_per_sec` | number \| null | 每秒中断（速率型，不可用为 null） |

### 2.4 其余指标域

`memory`：`{total_bytes:int, available_bytes:int, used_bytes:int, used_ratio:number|null}`。
**唯一口径：`used_bytes = total_bytes − available_bytes`**（不得使用"总量 − 空闲"）；`used_ratio = used_bytes / total_bytes`，`total_bytes = 0` 时为 `null`。

`swap`：`{total_bytes:int, used_bytes:int, used_ratio:number|null}`。无交换分区时 `total_bytes = 0` 且可用性映射标记不可用。

`volumes[]`：`{mount:string, total_bytes:int, used_bytes:int, used_ratio:number|null}`。卷级"不可读/无权限"当前不进数组；若后续票需要区分"无此卷"与"读不到"，以 additive 字段（如 `available`/`reason`）扩展并同步契约、类型与前端 schema。

`disk_io[]`：`{device:string, read_bytes_per_sec, write_bytes_per_sec, read_iops, write_iops, latency_ms, queue_depth, utilization}`，速率/计量字段均为 `number | null`（计数器不可用时为 `null` 而非 0）。

`network[]`：`{interface:string, rx_bytes_total:int, tx_bytes_total:int, rx_bytes_per_sec, tx_bytes_per_sec, rx_errors_per_sec, tx_errors_per_sec, rx_drops_per_sec, tx_drops_per_sec (均 number|null), is_virtual:bool}`。

`shm_dir`：`{path:string, total_bytes:int, page_files:int, oldest_file_age_sec:int|null}`。

### 2.5 availability（关键设计）

前端按本字段渲染，**不得按 OS 名硬编码分支**；不可用一律显示"不可用"而非 0。

| 字段 | 类型 | 说明 |
|------|------|------|
| `platform_description` | string | 人类可读平台描述，**Windows 必须含 build 号**（如 `"Windows 10 Pro (build 19045)"`） |
| `metrics` | object | 按指标可用性映射，键集合见下；每个值为 `{available:bool, reason?:string}`；`available=false` 时 `reason` 必填 |
| `semantic_hints` | object | 跨平台语义提示（key → 文案）；当前版本为空对象 |

`metrics` 键集合（固定，发送方每帧必须覆盖全部键）：

`system`、`cpu.total`、`cpu.cores`、`cpu.load_average`、`cpu.context_switches`、`memory`、`swap`、`volumes`、`disk_io`、`network`、`shm_dir`。

`reason` 为自由文案（英文、小写开头、无句号），建议取值：
- `not_implemented` —— 本版本尚未采集该指标
- `unsupported` —— 本平台不支持（如 Windows 无 load average、无交换分区）
- `no_permission` —— 计数器需特权/未开启
- `no_sensor` —— 传感器不存在（如温度）

> 当前版本实现 `system` 与 `cpu.total`；其余键固定为 `available=false` + `reason="not_implemented"`，对应域输出 `null`/空数组。

### 2.6 快照示例（首版节选）

```json
{
  "source": "dzsysmon",
  "timestamp": 1790000005000,
  "sampling": {"measure_interval_ms": 1000, "last_sample_unix_ms": 1790000005123, "missed_ticks": 0},
  "platform": {"family": "linux", "version": "Ubuntu 24.04.1 LTS", "kernel": "6.8.0-45-generic", "arch": "x86_64", "build": 0},
  "system": {"hostname": "trade-01", "cpu_model": "AMD Ryzen 9 7950X 16-Core Processor", "logical_cores": 32, "boot_time_unix_sec": 1789990000, "uptime_sec": 10005},
  "cpu": {"total": {"usage_ratio": 0.187}, "cores": [], "saturated_cores": [], "load_average": null, "context_switches_per_sec": null, "interrupts_per_sec": null},
  "memory": null,
  "swap": null,
  "volumes": null,
  "disk_io": null,
  "network": null,
  "shm_dir": null,
  "availability": {
    "platform_description": "Ubuntu 24.04.1 LTS (kernel 6.8.0-45-generic)",
    "metrics": {
      "system": {"available": true},
      "cpu.total": {"available": true},
      "cpu.cores": {"available": false, "reason": "not_implemented"},
      "cpu.load_average": {"available": false, "reason": "not_implemented"},
      "cpu.context_switches": {"available": false, "reason": "not_implemented"},
      "memory": {"available": false, "reason": "not_implemented"},
      "swap": {"available": false, "reason": "not_implemented"},
      "volumes": {"available": false, "reason": "not_implemented"},
      "disk_io": {"available": false, "reason": "not_implemented"},
      "network": {"available": false, "reason": "not_implemented"},
      "shm_dir": {"available": false, "reason": "not_implemented"}
    },
    "semantic_hints": {}
  }
}
```

### 2.7 计算语义（发送方义务）

- 速率与百分比均由纯函数从**原始累计计数器**差值计算（`SystemCollector` 只返回原始计数器，不含增量/百分比）。
- **首次采样返回不可用**（`null`），不得返回 0。
- 计数器回绕：仅当上次与本次值均落在 32 位范围内且本次小于上次时按 32 位回绕计算；其余下降视为计数器重置，本次输出不可用（下一采样周期重建基线）。
- Linux 忙闲口径：`iowait` 不计入忙；`guest`/`guest_nice` 不重复计入总量（已含于 user/nice）。
- 两次采样间隔内 CPU 使用率 = `1 − Δidle/Δtotal`，结果钳制到 `[0,1]`。
- 采集失败（如 `/proc` 读取失败）跳过本次计量并累加 `sampling.missed_ticks`。

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
| `groups` | object\<string,bool\> | 全部 true | 值必须 bool | 启用的指标大类：`cpu`/`memory`/`swap`/`volumes`/`disk_io`/`network`；平台相关默认（Windows 上需要额外权限或不可靠的指标，如温度/每核频率）由对应指标票在默认配置生成器落地 |

`disk`：`{watch_paths:string[], devices:string[]}`（元素非空；`watch_paths` 为空表示使用默认关注路径——共享内存目录与数据库目录所在卷）。

`network`：`{hide_virtual:bool}`，默认 `true`（隐藏 Hyper-V/WSL/Docker 虚拟网卡）。

`history`：`{enabled:bool, raw_retention_hours:int, max_retention_days:int, max_bytes:int}`，默认 `true/24/30/2147483648`；`raw_retention_hours ∈ [1,720]`、`max_retention_days ∈ [1,3650]`、`max_bytes ≥ 0`（0 = 不限制）。

`realtime`：`{push_interval_sec:int}`，默认 `5`，`[1, 3600]`；与计量频率解耦（事件通道每次写入会唤醒全部注册进程）。

`alerts`：`{cpu_sustain_used_pct:number, cpu_sustain_samples:int, memory_used_pct:number, swap_used_pct:number, volume_free_pct:number, volume_free_bytes:int, cooldown_sec:int}`，默认 `90/10/90/50/10/1073741824/600`；百分比字段 `[0,100]`、`cpu_sustain_samples ∈ [1,3600]`、`cooldown_sec ∈ [0,86400]`。

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
- `used_bytes` 必须等于 `total_bytes − available_bytes`（`memory`）；`used_bytes ≤ total_bytes`（`memory`/`swap`/`volumes`）。
- 比率为 `[0,1]`；字节/计数为非负整数；`boot_time_unix_sec`/`uptime_sec` 非负。
- `availability.metrics` 中 `available=false` 时 `reason` 必须存在且非空；`available=true` 时不得出现 `reason`。
- 未知字段忽略（前向兼容）。

---

## 8. 范围与明确不做

- **不支持容器内采集**：容器内 `/proc` 报宿主容量，会给出错误的容量基线；部署文档写明（`docs/components/dzsysmon.md`），不做 cgroup v1/v2 感知。
- 历史查询不走帧（dzweb 只读历史库）；告警复用 `NOTIFY_UI`；进程级指标、GPU、SMART、风扇、电池、登录用户列表不在本契约范围。
