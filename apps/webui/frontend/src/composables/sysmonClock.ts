/**
 * 时钟偏差域的展示口径（#302；契约 sysmon §2.9 / §2.8）
 *
 * 契约是唯一语义真相源：§2.9 定义 `clock` 域的 payload 与三种空态、§2.8 定义两个时间判定
 * 对象（`clock_offset` 双档、`clock_bound` 独立质量判据）、§3.1 定义 `clock` 配置段。本模块
 * 只承载**展示侧的口径**（阈值解析、空态分类、失败原因的中文文案、每台服务器的指标列），
 * 不含任何协议判断——payload 净化在 `stores/sysmon.ts`，渲染在 `views/MonitorView.vue`。
 *
 * 三条不可动摇的约定：
 * 1. **定档语义只有一份**。偏差的两档阈值来自配置（不是本模块常量），但「≥ 阈值即进入该档」
 *    与等级词表（`normal` / `warn` / `danger`，与资源类色标同一套）由
 *    `useResourceLevel.thresholdLevel` 提供，本模块只做「取绝对值 + 传阈值」。
 * 2. **空态三分支不得混用**。`clock = null` 只表示配置关闭（契约 §2.9 注：前端须区分「尚未
 *    探测」与「不可用」，两种含义共用一个 null 无法区分）；`last_probe_unix_ms = 0` 是尚未探测；
 *    `> 0` 且 `offset_ms = null` 是全部失败。任何分支都不得用 0 冒充。
 * 3. **质量判据与偏差定档互不影响**。`clock_bound` 越界只写日志 + 卡片内「测量质量」提示，
 *    不改变偏差着色（契约 §2.8：「测不准」与「真的偏」是两件事）。
 */
import {
  UNAVAILABLE_TEXT,
  formatCompactSignedMilliseconds,
  formatMilliseconds,
  formatSignedMilliseconds,
} from '@/composables/format'
import { thresholdLevel, type ResourceLevel } from '@/composables/useResourceLevel'
import type {
  SysmonClockConfigPayload,
  SysmonClockInfo,
  SysmonClockServerInfo,
} from '@/types/generated'

/**
 * 契约 sysmon §3.1 `clock` 段的默认值（**副本，用于配置镜像尚未到达时的展示**）。
 *
 * 发送方是唯一真相源：`dzsysmon` 未收到配置帧时也按这些默认值工作，前端在「配置尚未到达」
 * 的窗口内必须与发送方给出同一结论，否则卡片会先按一套阈值着色、配置到达后跳变。只复制
 * 非数组的三个阈值 + 手动限流（数组字段卡片不消费）。
 */
export const CLOCK_DEFAULT_OFFSET_WARN_MS = 100
export const CLOCK_DEFAULT_OFFSET_ERROR_MS = 1000
export const CLOCK_DEFAULT_BOUND_ALARM_MS = 200
export const CLOCK_DEFAULT_MANUAL_MIN_INTERVAL_SEC = 10

/** 卡片消费的三个阈值（契约 §3.1：它们与探测参数同属一个功能单元，不在 `thresholds` 段） */
export interface ClockThresholds {
  /** 偏差 warning 档阈值（毫秒，绝对值 ≥ 它即 warn） */
  warn_ms: number
  /** 偏差 error 档阈值（毫秒，绝对值 ≥ 它即 danger） */
  error_ms: number
  /** 误差界质量判据（毫秒，`bound_ms >` 它即提示测量质量） */
  bound_alarm_ms: number
}

/** 只认正有限值：畸形配置字段回退契约默认值（不让一个坏字段把整张卡片变成不显示） */
function positiveOr(value: unknown, fallback: number): number {
  return typeof value === 'number' && Number.isFinite(value) && value > 0 ? value : fallback
}

/**
 * 从生效配置镜像解析三个阈值（配置镜像未到达 / 该段缺失时按契约默认值）。
 *
 * 不校验 `offset_warn_ms < offset_error_ms`：**校验责任在发送方**（`SysmonConfig::validate`
 * 拒绝非法组合，契约 §3.1 也这样规定）；前端再造一份判据会与后端漂移，且畸形配置本就到不了
 * 镜像（`isFullConfigPayload` 之外的字段即便如此也只会让定档顺序按字面值生效）。
 */
export function resolveClockThresholds(
  config: SysmonClockConfigPayload | null | undefined,
): ClockThresholds {
  return {
    warn_ms: positiveOr(config?.offset_warn_ms, CLOCK_DEFAULT_OFFSET_WARN_MS),
    error_ms: positiveOr(config?.offset_error_ms, CLOCK_DEFAULT_OFFSET_ERROR_MS),
    bound_alarm_ms: positiveOr(config?.bound_alarm_ms, CLOCK_DEFAULT_BOUND_ALARM_MS),
  }
}

/**
 * 手动查询的最小间隔（秒，契约 §3.1 `manual_min_interval_sec`；0 = 不限流）。
 * 非整数 / 负数 / 缺失回退契约默认 10 秒——0 是合法值，只能由「恰为 0」表达，不能被兜底吞掉。
 */
export function resolveClockManualIntervalSec(
  config: SysmonClockConfigPayload | null | undefined,
): number {
  const value = config?.manual_min_interval_sec
  if (typeof value !== 'number' || !Number.isFinite(value) || value < 0) {
    return CLOCK_DEFAULT_MANUAL_MIN_INTERVAL_SEC
  }
  return Math.floor(value)
}

/**
 * 域状态（契约 §2.9 的三种空态 + 正常态）：
 * - `disabled`：`clock` 为 null，**仅**表示该域被配置关闭；
 * - `unprobed`：`last_probe_unix_ms = 0`（尚未探测）；
 * - `unavailable`：已探测过但本轮无可用参考源（`offset_ms` 为 null）；
 * - `live`：有可用的聚合偏差。
 */
export type ClockDomainState = 'disabled' | 'unprobed' | 'unavailable' | 'live'

/** 域状态分类的唯一实现（`clock` 已由 store 净化；null 即配置关闭） */
export function clockDomainState(clock: SysmonClockInfo | null): ClockDomainState {
  if (clock === null) return 'disabled'
  if (clock.last_probe_unix_ms === 0) return 'unprobed'
  return clock.offset_ms === null ? 'unavailable' : 'live'
}

/**
 * 偏差的展示等级：`null`（本轮无可用参考源）不套任何状态色——不可用不是 0 毫秒，
 * 不得用 `normal` 冒充「时钟很准」。有值时按**绝对值**定档（契约 §2.8：绝对值 ≥ 阈值）。
 */
export function clockOffsetLevel(
  offsetMs: number | null,
  thresholds: ClockThresholds,
): ResourceLevel | null {
  if (offsetMs === null) return null
  return thresholdLevel(Math.abs(offsetMs), thresholds.warn_ms, thresholds.error_ms)
}

/**
 * 测量质量是否劣化（契约 §2.8：`bound_ms > bound_alarm_ms` 进入，「恰好等于」不算）。
 * 与 {@link clockOffsetLevel} **相互独立**：它只决定「测量质量」提示，不参与偏差着色。
 */
export function clockBoundAlarm(boundMs: number | null, thresholds: ClockThresholds): boolean {
  return boundMs !== null && boundMs > thresholds.bound_alarm_ms
}

/** 探测失败原因的中文文案（契约 §2.9 `error` 的五个取值；非约定值回退原文，不吞信息） */
export const CLOCK_FAILURE_TEXT: Record<string, string> = {
  timeout: '超时未响应',
  kod_rate: '服务端限流（KoD RATE）',
  kod_deny: '服务端拒绝（KoD DENY）',
  bad_echo: '报文回显不匹配',
  unreachable: '无法解析或连接失败',
}

/**
 * 失败原因的展示文案。`null`（该台没有失败原因，或本轮尚无结果）显示占位破折号——
 * 这是「不存在失败原因」而不是「不可用」，两种事实不同。
 */
export function clockFailureText(error: string | null): string {
  if (error === null || error === '') return '—'
  return CLOCK_FAILURE_TEXT[error] ?? error
}

/** 每台服务器一行指标（主机名在行标题，故此处不含 host） */
export interface ClockServerCell {
  /** 稳定英文键（用于 `data-test` / `v-for :key`，不用中文标签做标识） */
  key: string
  /** 中文列标题 */
  label: string
  /** 已格式化的展示值 */
  value: string
}

/**
 * 每台服务器的指标列（AC：主机、偏差、误差界、RTT、stratum、失败原因）。
 *
 * 失败台的四项测量按契约 §2.9 恒为 null（失败样本不产出测量值），因此统一走各格式化器的
 * 不可用分支——**不得**显示 0。列集合对成功台与失败台一致，便于纵向对照。
 */
export function clockServerCells(server: SysmonClockServerInfo): ClockServerCell[] {
  return [
    { key: 'offset', label: '偏差', value: formatSignedMilliseconds(server.offset_ms) },
    { key: 'bound', label: '误差界', value: formatMilliseconds(server.bound_ms) },
    { key: 'rtt', label: 'RTT', value: formatMilliseconds(server.rtt_ms) },
    { key: 'stratum', label: 'Stratum', value: server.stratum === null ? UNAVAILABLE_TEXT : String(server.stratum) },
    { key: 'error', label: '失败原因', value: clockFailureText(server.error) },
  ]
}

/**
 * 全部失败时的原因汇总（卡片正文的「不可用」说明）。
 *
 * 逐台原因去重后并列：两台超时 + 一台无法解析是**同一个故障的不同表现**，合并成一句
 * 「探测失败」会把可诊断信息抹掉（AC：失败态要给出原因）。`servers` 为空（畸形 payload /
 * 尚未探测却标了探测时刻）时只能给出不带原因的中性说明，不编造原因。
 */
export function clockFailureSummary(servers: readonly SysmonClockServerInfo[]): string {
  const reasons = [...new Set(
    servers
      .filter(server => !server.ok)
      .map(server => clockFailureText(server.error))
      .filter(text => text !== '—'),
  )]
  if (reasons.length === 0) return '最近一轮探测未获得可用参考源'
  return `最近一轮探测全部失败：${reasons.join('、')}`
}

/** 卡片内空态 / 提示的固定文案（视图只引用，不在模板里散写字面量） */
export const CLOCK_DISABLED_TITLE = '时钟监控已在配置中关闭'
export const CLOCK_DISABLED_DESC = '在「配置」tab 的「时钟偏差」段启用后，dzsysmon 会立即发起一轮探测'
export const CLOCK_UNPROBED_TITLE = '尚未探测'
export const CLOCK_UNPROBED_DESC = 'dzsysmon 尚未完成首轮 NTP 探测（进程启动后会立即探测一次）'
export const CLOCK_NO_DAEMON_TEXT = '未检测到运行中的时间同步服务'
export const CLOCK_NO_DAEMON_DESC = '本机可能没有启动任何常见时间同步服务（检测不到不影响偏差测量）'
export const CLOCK_DAEMON_CONFLICT_TEXT = '检测到多个时间同步服务同时运行，请确认只保留一个'
/** 「测量质量」提示正文（契约 §2.8：只提示，不改偏差着色） */
export const CLOCK_QUALITY_HINT = '测量质量偏弱：误差界较大，本次偏差读数的可信度下降（网络或服务器响应慢）'

/**
 * 上次探测距当前的秒数；快照未到达 / 尚未探测（`last_probe_unix_ms = 0`）时返回 null。
 *
 * 只算**秒数**，「多久以前」的文案交给 `format.formatAge`——相对时间只允许一种呈现实现
 * （页文件年龄与上次探测时间共用同一份「多久以前」口径；系统启动时长用的 `formatUptime`
 * 是另一种形态，不做「多久以前」的换算）。
 *
 * 负值（快照里的探测时刻晚于同一帧的 `timestamp`）钳到 0：这说明本机时钟本身可能有问题，
 * 展示成「-3 秒前」只会让人以为界面坏了；真正的信号在偏差数字上。
 */
export function clockProbeAgeSec(lastProbeUnixMs: number, nowUnixMs: number): number | null {
  if (!Number.isFinite(lastProbeUnixMs) || lastProbeUnixMs <= 0) return null
  if (!Number.isFinite(nowUnixMs)) return null
  return Math.max(0, Math.floor((nowUnixMs - lastProbeUnixMs) / 1_000))
}

/**
 * 同步服务列表的展示序（`running` 优先，再按名称升序）：先看「谁在管」，再看看到齐了哪些。
 * 不按 payload 顺序——发送端各平台的枚举顺序不同（Linux 进程表扫描 / Windows 单条查询）。
 */
export function sortClockDaemons<T extends { name: string; running: boolean }>(daemons: readonly T[]): T[] {
  return [...daemons].sort((a, b) => {
    if (a.running !== b.running) return a.running ? -1 : 1
    return a.name.localeCompare(b.name)
  })
}

// ===== 顶栏 NTP 徽章（ADR 0048，契约 sysmon §2.9 的「顶栏徽章」条）=====

/**
 * 顶栏徽章的展示模型（纯数据，组件只负责渲染）：
 * - `text`：徽章上的短文本（紧凑毫秒 / `—` / `不可用`）；
 * - `level`：状态色等级（`null` = 中性等待态，不套色标）；
 * - `title`：悬停完整说明。
 */
export interface ClockBadge {
  text: string
  level: ResourceLevel | null
  title: string
}

/** 徽章标签文本（顶栏窄条用缩写，与 CPU / MEM 同层） */
export const CLOCK_BADGE_LABEL = 'NTP'

/**
 * 顶栏 NTP 徽章的展示模型（四态一一对应契约 §2.9 的域内空态）：
 *
 * - `snapshotMissing`（快照尚未到达）→ `—` + 中性色 + 「等待 dzsysmon 快照」；
 * - `disabled`（`clock = null`，该域被配置关闭）→ 返回 `null`，**整枚徽章不渲染**
 *   （配置关闭是「这项监控被关掉了」这一运维事实，顶栏不放无信息空壳）；
 * - `unprobed`（`last_probe_unix_ms = 0`）→ `—` + 中性色 + 「尚未探测」；
 * - `unavailable`（探测过但本轮无可用参考源）→ `不可用` + 中性色 + 失败原因汇总；
 * - `live` → 紧凑带符号毫秒 + 按 `|offset_ms|` 定档的状态色。
 *
 * 三条硬约束：
 * 1. **不得用 0 冒充**：三种空态的文本都不是数字（`—` / `不可用`）。
 * 2. **色标只来自 `clockOffsetLevel`**（阈值取生效配置），组件里不另写比较。
 * 3. **误差界越界只进 title**：`bound_ms > bound_alarm_ms` 追一句「测量质量」提示，
 *    不改变偏差着色（契约 §2.8：两个判定对象独立）。
 *
 * @param clock 已由 store 净化的域（`null` = 配置关闭）
 * @param thresholds 生效配置的三个阈值（含 `bound_alarm_ms`）
 * @param snapshotPresent 快照是否已到达（未到达时连「配置是否关闭」都不知道，只能显示等待态）
 * @param probeAgeText 「上次探测多久以前」的现成文案（`null` = 无法表达），由调用方用
 *        `clockProbeAgeSec` + `formatAge` 算出——本模块不复制第二份相对时间口径
 */
export function clockBadge(
  clock: SysmonClockInfo | null,
  thresholds: ClockThresholds,
  snapshotPresent: boolean,
  probeAgeText: string | null,
): ClockBadge | null {
  if (!snapshotPresent) {
    return { text: '—', level: null, title: '时钟偏差（等待 dzsysmon 快照）' }
  }
  if (clock === null) return null
  const state = clockDomainState(clock)
  if (state === 'unprobed') {
    return { text: '—', level: null, title: `${CLOCK_UNPROBED_TITLE}：${CLOCK_UNPROBED_DESC}` }
  }

  const ageSuffix = probeAgeText === null ? '' : `；上次探测 ${probeAgeText}前`
  if (clock.offset_ms === null || clock.bound_ms === null) {
    // store 已保证两键同生共死；这里仍按「无偏差读数」处理，不用 0 冒充
    return {
      text: UNAVAILABLE_TEXT,
      level: null,
      title: `时钟偏差${UNAVAILABLE_TEXT}：${clockFailureSummary(clock.servers)}${ageSuffix}`,
    }
  }

  const level = clockOffsetLevel(clock.offset_ms, thresholds)
  const quality = clockBoundAlarm(clock.bound_ms, thresholds) ? `；${CLOCK_QUALITY_HINT}` : ''
  const title = `时钟偏差 ${formatSignedMilliseconds(clock.offset_ms)}`
    + `；误差界 ${formatMilliseconds(clock.bound_ms)}`
    + `；来源 ${clock.source}（stratum ${clock.source_stratum ?? UNAVAILABLE_TEXT}）`
    + `${ageSuffix}${quality}`
  return {
    text: formatCompactSignedMilliseconds(clock.offset_ms),
    level,
    title,
  }
}
