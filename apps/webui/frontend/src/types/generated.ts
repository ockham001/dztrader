// 由 scripts/gen-types.mjs 从 schema/domain-payloads.schema.json 生成 — 请勿手改
// 真源：docs/frame_contracts/*.md 与 libs/platform 头文件；改 schema 后重跑 npm run gen:types

// 进程状态（契约 process）
export type ProcessState = 'Starting' | 'Running' | 'Stopping' | 'Stopped' | 'Crashed'

// 进程操作结果事件（契约 process）
export type ProcessEvent = 'StartSucceeded' | 'StartFailed' | 'StopSucceeded' | 'StopFailed' | 'RemoveSucceeded' | 'RemoveFailed'

export interface ProcessStatusPayload {
    /** 进程名 */
    name: string
    state: ProcessState
    /** 进程 PID，未运行时为 0 */
    pid: number
    /** 用户可读显示名；缺省为空串 */
    display_name?: string
    /** 状态说明/失败原因；缺省为空串 */
    message?: string
    /** 操作结果事件；缺失或 null 均为自发状态变化（契约 webui-ws §2.2） */
    event?: ProcessEvent | null
}

export interface ProgressStatus {
    min: number
    /** max <= min（含 max==0）表示不确定进度 */
    max: number
    current: number
    /** 简短文本说明；空串/省略=无文本 */
    desc?: string
}

export interface LogConfigPayload {
    /** 当前生效级别，始终为规范全称（契约 log） */
    level: string
    /** 当前生效 flush 阈值，始终为规范全称 */
    flush_on: string
}

export interface AutoLoginSchedule {
    /** 登录时间 HH:MM（进程本地时间） */
    login_time: string
    /** 登出时间 HH:MM（进程本地时间） */
    logout_time: string
}

export interface AutoLoginPayload {
    /** 是否启用自动登录排程 */
    enabled: boolean
    /** 排程整体覆盖（契约 auto-login） */
    schedules: AutoLoginSchedule[]
}

export interface NotifyUiPayload {
    /** 通知来源（进程名或策略 ID） */
    source: string
    /** 通知级别 */
    level: string
    /** 通知正文 */
    message: string
    /** Unix 秒级时间戳 */
    timestamp: number
    /** 是否弹窗打断用户 */
    popup: boolean
}

export interface BrokerFrontend {
    /** 前置地址，非空（契约 md-config） */
    address: string
    /** 可选人类可读标签，可为空 */
    label: string
    /** 是否启用（注册到 CTP） */
    enabled: boolean
}

export interface BrokerEntry {
    /** 经纪商不可变 key */
    name: string
    /** 经纪公司代码 */
    broker_id: string
    /** 用户代码 */
    user_id: string
    /** 登录密码（生产/RTN 脱敏为 ****） */
    password: string
    /** 产品信息 */
    product_info: string
    /** 前置地址列表 */
    frontends: BrokerFrontend[]
}

export interface MdConfigPayload {
    /** 经纪商列表（RTN 全量） */
    brokers: BrokerEntry[]
    /** 当前选中经纪商名，空或指向存在的经纪商 */
    current_broker_name: string
    /** 每批订阅数量 */
    subscribe_batch_size?: number
    /** 批间延迟（毫秒） */
    subscribe_batch_delay_ms?: number
    /** 补订检查间隔（毫秒） */
    sub_check_interval_ms?: number
    /** 补订最大重试 */
    sub_max_retry?: number
}

export interface MdRtnConfigPayload {
    /** 行情源进程名 */
    source: string
    /** 脱敏行情配置 */
    config: MdConfigPayload
}

export interface PreloadPoint {
    /** 预加载页数，范围 [0,8] */
    pages: number
    /** 预加载字节数，范围 [0,2^40] */
    bytes: number
}

export interface ShmConfigView {
    /** 页大小（MB），启动后不可变 */
    page_size_mb: number
    /** 预加载点，key=HH:MM */
    preload_points: Record<string, PreloadPoint>
    /** 周期检查间隔（分钟），0=不检查 */
    check_interval_min: number
    /** 周期检查预加载页数 */
    check_pages: number
    /** 周期检查预加载字节数 */
    check_bytes: number
}

export interface LogLine {
    /** 行号 */
    n: number
    /** 时间戳 */
    ts: string
    /** 日志级别 */
    level: string
    /** logger 名 */
    logger: string
    /** 函数名 */
    func: string
    /** 源文件名 */
    file: string
    /** 源文件行号 */
    line: number
    /** 进程 ID */
    pid: string
    /** 线程 ID */
    tid: string
    /** 日志消息 */
    msg: string
    /** 原始行 */
    raw: string
    /** 是否已结构化解析 */
    parsed: boolean
}

export interface MdRtnStatusPayload {
    /** 行情源进程名 */
    source: string
    /** 网关状态（结构随接口类型） */
    status: Record<string, unknown>
}

export interface SysmonSamplingMeta {
    /** 当前计量间隔（毫秒） */
    measure_interval_ms: number
    /** 最近一次计量成功的 Unix 毫秒时间；0=尚无样本 */
    last_sample_unix_ms: number
    /** 累计错过的计量 tick 数（缺口标记） */
    missed_ticks: number
}

export interface SysmonPlatformInfo {
    /** 平台家族：windows / linux */
    family: string
    /** 真实 OS 版本 */
    version: string
    /** Linux 内核 release；Windows 为 NT 版本号 */
    kernel: string
    /** 架构标识 */
    arch: string
    /** Windows build 号（区分 Win10/Win11）；Linux 为 0 */
    build: number
}

export interface SysmonSystemInfo {
    /** 主机名 */
    hostname: string
    /** CPU 型号；取不到时为空串 */
    cpu_model: string
    /** 逻辑核数 */
    logical_cores: number
    /** 启动时间（Unix 秒） */
    boot_time_unix_sec: number
    /** 运行时长（秒） */
    uptime_sec: number
}

export interface SysmonCpuTotal {
    /** 总使用率 [0,1]；null=首次采样/计数器不可用（不得用 0 冒充） */
    usage_ratio: number | null
}

export interface SysmonCpuCore {
    /** 核 index */
    index: number
    /** 单核使用率 [0,1]；null=不可用 */
    usage_ratio: number | null
    /** 是否处于持续饱和状态 */
    saturated: boolean
}

export interface SysmonCoreFrequency {
    /** OS CPU 编号（不保证连续，与 cpu.cores[].index 同源） */
    index: number
    /** 当前频率（MHz，> 0） */
    mhz: number
}

export interface SysmonLoadAverage {
    one: number | null
    five: number | null
    fifteen: number | null
    /** 口径：load_average（Linux）/ processor_queue（Windows 代理量） */
    semantic: string
}

export interface SysmonCpuInfo {
    total: SysmonCpuTotal
    /** 每核使用率（不可用时为空数组） */
    cores: SysmonCpuCore[]
    /** 每核当前频率；null=本平台不提供/不可用（不可用时区块整块不渲染） */
    core_frequencies: SysmonCoreFrequency[] | null
    /** 持续饱和的核 index 集合 */
    saturated_cores: number[]
    /** 系统负载；null=不提供/不可用（Windows 为处理器队列长度代理量，语义不同） */
    load_average: SysmonLoadAverage | null
    context_switches_per_sec: number | null
    interrupts_per_sec: number | null
}

export interface SysmonMemoryInfo {
    total_bytes: number
    available_bytes: number
    /** 唯一口径：total_bytes - available_bytes */
    used_bytes: number
    used_ratio: number | null
    /** 可回收文件页缓存（Linux Cached+SReclaimable / Windows SystemCache）；null = 本平台不提供/本帧未读到 */
    cached_bytes?: number | null
    /** 内核文件系统元数据缓存（Linux Buffers）；仅 Linux；Windows 恒 null */
    buffers_bytes?: number | null
}

export interface SysmonSwapInfo {
    /** 0 = 本机无交换分区 */
    total_bytes: number
    used_bytes: number
    used_ratio: number | null
}

export interface SysmonVolumeInfo {
    /** 规范化挂载点（Linux 挂载目录；Windows 盘符根） */
    mount: string
    /** 容量是否可读；false 时三个容量字段必须为 null（不得用 0 冒充） */
    available: boolean
    /** available=false 时为 null */
    total_bytes: number | null
    /** available=false 时为 null */
    used_bytes: number | null
    /** available=false 或 total=0 时为 null */
    used_ratio: number | null
    /** 不可用原因；available=true 时省略 */
    reason?: string
}

export interface SysmonDiskIoInfo {
    /** 物理盘标识（Linux 为 sda/nvme0n1 等内核设备名；Windows 为 PhysicalDrive0 等物理盘号） */
    device: string
    /** 读吞吐（字节/秒）；首次采样/计数器重置/本帧未读到时为 null（不得用 0 冒充） */
    read_bytes_per_sec: number | null
    /** 写吞吐（字节/秒）；首次采样/计数器重置/本帧未读到时为 null（不得用 0 冒充） */
    write_bytes_per_sec: number | null
    /** 读 IOPS（每秒读请求数）；首次采样/计数器重置/本帧未读到时为 null（不得用 0 冒充） */
    read_iops: number | null
    /** 写 IOPS（每秒写请求数）；首次采样/计数器重置/本帧未读到时为 null（不得用 0 冒充） */
    write_iops: number | null
    /** 平均每请求延迟（毫秒）；首次采样/计数器重置/本帧未读到时为 null（不得用 0 冒充） */
    latency_ms: number | null
    /** 采样时刻在途请求数（瞬时 gauge，不是累计计数器）；计数器不可用时为 null（不得用 0 冒充） */
    queue_depth: number | null
    /** 设备利用率（[0,1] 比率）；计数器不可用时为 null（不得用 0 冒充） */
    utilization: number | null
}

// 每网卡累计收发与速率（契约 sysmon §2.4 network[]）。八个 *_total 为累计计数器、必填非负整数（累计量没有不可用语义）；六个 *_per_sec 为速率，首次采样/本帧新出现/计数器重置时为 null（不得用 0 冒充）。hide_virtual=true 时发送方已过滤 is_virtual 条目。
export interface SysmonNetworkInfo {
    /** 网卡名（Linux 内核接口名；Windows 适配器别名 Alias）；单帧内唯一 */
    interface: string
    /** 累计接收字节数 */
    rx_bytes_total: number
    /** 累计发送字节数 */
    tx_bytes_total: number
    /** 累计接收包数（含单播与组播/广播） */
    rx_packets_total: number
    /** 累计发送包数（含单播与组播/广播） */
    tx_packets_total: number
    /** 累计接收错误数 */
    rx_errors_total: number
    /** 累计发送错误数 */
    tx_errors_total: number
    /** 累计接收丢弃数 */
    rx_drops_total: number
    /** 累计发送丢弃数 */
    tx_drops_total: number
    /** 接收吞吐（字节/秒）；首次采样/本帧新出现/计数器重置时为 null（不得用 0 冒充） */
    rx_bytes_per_sec: number | null
    /** 发送吞吐（字节/秒）；首次采样/本帧新出现/计数器重置时为 null（不得用 0 冒充） */
    tx_bytes_per_sec: number | null
    /** 接收错误速率（个/秒）；不可用时为 null（不得用 0 冒充） */
    rx_errors_per_sec: number | null
    /** 发送错误速率（个/秒）；不可用时为 null（不得用 0 冒充） */
    tx_errors_per_sec: number | null
    /** 接收丢弃速率（个/秒）；不可用时为 null（不得用 0 冒充） */
    rx_drops_per_sec: number | null
    /** 发送丢弃速率（个/秒）；不可用时为 null（不得用 0 冒充） */
    tx_drops_per_sec: number | null
    /** 是否虚拟网卡（不由硬件设备支撑）；hide_virtual=true 时发送方不输出 true 的条目 */
    is_virtual: boolean
}

export interface SysmonShmDirInfo {
    path: string
    total_bytes: number
    page_files: number
    oldest_file_age_sec: number | null
}

export interface SysmonTemperatureInfo {
    /** 传感器稳定标识（Linux hwmon：<name>#<hwmonN>:<label>，无 label 时用 tempN）；也是历史库维度键，值不因新增 chip/label 而改变 */
    sensor: string
    /** 芯片名（hwmon name，如 coretemp / amdgpu / acpitz）；additive 字段，旧版本发送方缺失时消费方从 sensor 回退解析 */
    chip?: string
    /** 通道标签（tempN_label）；additive 字段，缺失时为空串（该通道没有标签，不是不可用） */
    label?: string
    /** 摄氏温度（瞬时 gauge，不是累计计数器） */
    celsius: number
}

export interface SysmonAvailabilityEntry {
    available: boolean
    /** 不可用原因；available=true 时省略 */
    reason?: string
}

export interface SysmonAvailability {
    /** 人类可读平台描述（Windows 含 build 号） */
    platform_description: string
    /** 按指标可用性映射 */
    metrics: Record<string, SysmonAvailabilityEntry>
    /** 跨平台语义提示 */
    semantic_hints: Record<string, string>
}

export interface SysmonSnapshotPayload {
    /** 发送方进程名（固定 dzsysmon；无 instance_id 帧来源显式携带，general §5） */
    source: string
    /** 快照生成时刻（Unix 毫秒） */
    timestamp: number
    sampling: SysmonSamplingMeta
    platform: SysmonPlatformInfo
    system: SysmonSystemInfo
    cpu: SysmonCpuInfo
    /** 温度传感器读数；null=本平台不提供/无传感器/不可用（不可用时区块整块不渲染） */
    temperature: SysmonTemperatureInfo[] | null
    memory: SysmonMemoryInfo | null
    swap: SysmonSwapInfo | null
    volumes: SysmonVolumeInfo[] | null
    disk_io: SysmonDiskIoInfo[] | null
    network: SysmonNetworkInfo[] | null
    shm_dir: SysmonShmDirInfo | null
    availability: SysmonAvailability
}

export interface SysmonSamplingConfigPayload {
    /** 计量间隔 [100,60000] */
    measure_interval_ms: number
    /** 记录间隔 [1,86400] */
    record_interval_sec: number
    /** 记录相位偏移 [0,86400] */
    phase_offset_sec: number
    /** 启用的指标大类 */
    groups: Record<string, boolean>
}

export interface SysmonDiskConfigPayload {
    /** 设备范围（空=全部物理盘） */
    devices: string[]
}

export interface SysmonNetworkConfigPayload {
    /** 是否隐藏虚拟网卡 */
    hide_virtual: boolean
}

export interface SysmonHistoryConfigPayload {
    enabled: boolean
    raw_retention_hours: number
    max_retention_days: number
    /** 总量上限；0=不限制 */
    max_bytes: number
}

export interface SysmonRealtimeConfigPayload {
    /** 推送间隔 [1,3600] */
    push_interval_sec: number
}

export interface SysmonThresholdsConfigPayload {
    cpu_sustain_used_pct: number
    cpu_sustain_samples: number
    memory_used_pct: number
    swap_used_pct: number
    volume_free_pct: number
    log_repeat_sec: number
}

export interface SysmonConfigPayload {
    sampling: SysmonSamplingConfigPayload
    disk: SysmonDiskConfigPayload
    network: SysmonNetworkConfigPayload
    history: SysmonHistoryConfigPayload
    realtime: SysmonRealtimeConfigPayload
    thresholds: SysmonThresholdsConfigPayload
}
