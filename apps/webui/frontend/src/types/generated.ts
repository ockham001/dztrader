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
    /** 操作结果事件；缺失=自发状态变化 */
    event?: ProcessEvent
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
