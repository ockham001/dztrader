/**
 * 顶栏墙钟与时钟偏差括号的展示模型（#452 / ADR 0054）
 *
 * 契约是唯一语义真相源：sysmon §2.9 定义 `clock` 域的 payload 与三种空态、§2.8 定义两个
 * 时间判定对象、§3.1 定义 `clock` 配置段。本模块只承载**顶栏墙钟**的展示口径：
 *
 * 1. **墙钟取本机 `Date`**（每秒 tick 由组件负责），**不用快照 `timestamp`** —— 快照时间是
 *    「数据龄」口径，墙钟是「现在几点」口径，两者混用会让停推时时钟跟着停（#452 拍板记录 5）。
 * 2. **偏差括号只有一档状态色，且只作用于括号内文本**：正常中性、`|offset|` 达
 *    `clock.offset_warn_ms` 变黄、达 `offset_error_ms` 变红。整块时钟不变色——时钟本身永远
 *    可读，偏差是它的一个附属事实（DESIGN「The One Job Rule」：颜色只在括号上标注偏差状态）。
 * 3. **空态不得用 0 冒充**：尚未探测 / 本轮全部失败显示 `(—)`；`clock = null`（配置关闭）
 *    整段括号不渲染（配置关闭是运维事实，不放 0 或空壳）。四态分类复用
 *    `sysmonClock.clockDomainState` 的口径，只是形态从「独立 NTP 徽章」改为「墙钟右侧的括号」。
 *
 * 纯函数、不读 store / DOM / Date.now（时刻由调用方传入），因此边界可以在单测里逐条锁死；
 * 组件只负责每秒 tick、visibility 暂停与渲染。
 */
import {
  CLOCK_DISABLED_TITLE,
  CLOCK_QUALITY_HINT,
  CLOCK_UNPROBED_DESC,
  CLOCK_UNPROBED_TITLE,
  clockBoundAlarm,
  clockFailureSummary,
  clockOffsetLevel,
  type ClockThresholds,
} from '@/composables/sysmonClock'
import {
  UNAVAILABLE_TEXT,
  formatCompactSignedMilliseconds,
  formatMilliseconds,
  formatSignedMilliseconds,
} from '@/composables/format'
import type { ResourceLevel } from '@/composables/useResourceLevel'
import type { SysmonClockInfo } from '@/types/generated'

/** 星期日 → 星期六（`Date.getDay()` 的值域）；中文星期用「周四」形态（#452 拍板记录 1） */
const WEEKDAY_NAMES = ['周日', '周一', '周二', '周三', '周四', '周五', '周六'] as const

/** 无法从 `Intl` 解析出 IANA 时区时的兜底文案（tooltip 必须能说明「按哪个时区显示」） */
export const CLOCK_TIME_ZONE_FALLBACK = '本地时区'

/** 括号内「没有可用读数」的唯一占位（全角破折号，与监控页不可用占位同一形态） */
export const CLOCK_OFFSET_UNAVAILABLE_TEXT = '—'

/** `HH` / `mm` / `ss` / 月 / 日 一律补足两位：固定宽度扫读，不因个位数跳位 */
function pad2(value: number): string {
  return String(value).padStart(2, '0')
}

/**
 * 墙钟的四段文本（四档格式按 CSS 断点只显示其中一部分，秒永远保留）：
 * - `dateFull`：`2026-01-02`（≥1280 档）
 * - `dateShort`：`01-02`（1024–1279 档，只丢年份）
 * - `weekday`：`周四`（768–1023 档保留，<768 丢）
 * - `time`：`13:25:36`（所有宽度都保留）
 * - `dateTime`：`<time datetime>` 属性值（本地时间，ISO 形态不含时区偏移）
 */
export interface ClockTextParts {
  dateFull: string
  dateShort: string
  weekday: string
  time: string
  dateTime: string
}

/** 本机墙钟拆段（24 小时制、本地时区；`now` 由调用方给，组件每秒刷新） */
export function clockTextParts(now: Date): ClockTextParts {
  const year = now.getFullYear()
  const month = pad2(now.getMonth() + 1)
  const day = pad2(now.getDate())
  const hour = pad2(now.getHours())
  const minute = pad2(now.getMinutes())
  const second = pad2(now.getSeconds())
  const dateFull = `${year}-${month}-${day}`
  const time = `${hour}:${minute}:${second}`
  return {
    dateFull,
    dateShort: `${month}-${day}`,
    weekday: WEEKDAY_NAMES[now.getDay()],
    time,
    dateTime: `${dateFull}T${time}`,
  }
}

/**
 * 偏差括号的展示模型：
 * - `text`：括号内文本（**含括号**）——`(+13ms)` / `(—)`；
 * - `level`：只有括号文本用的状态等级（`null` = 中性，不套色标）；
 * - `aria`：偏差状态整句（读屏用，含方向与档位）；
 * - `detail`：完整说明（tooltip 片段，含原因 / 误差界 / 来源 / stratum / 上次探测）。
 */
export interface ClockDeviation {
  text: string
  level: ResourceLevel | null
  aria: string
  detail: string
}

/** 偏差档位 → 中文状态词（aria / 读屏用；色标之外的冗余信号，WCAG 1.4.1） */
function deviationLevelText(level: ResourceLevel): string {
  if (level === 'danger') return '过大'
  if (level === 'warn') return '偏大'
  return '正常'
}

/**
 * 偏差括号模型（四态与契约 §2.9 的域内空态一一对应）：
 *
 * - `snapshotPresent=false`（快照尚未到达）→ `(—)` + 中性 + 「等待 dzsysmon 快照」；
 * - `clock = null`（该域被配置关闭）→ 返回 `null`，**括号不渲染**；
 * - `last_probe_unix_ms = 0`（尚未探测）→ `(—)` + 中性 + 「尚未探测」；
 * - `> 0` 且 `offset_ms = null`（本轮全部失败）→ `(—)` + 中性 + 失败原因汇总；
 * - `live` → 紧凑带符号整数毫秒 + 按 `|offset_ms|` 定档（只有括号文本着色）。
 *
 * 三条硬约束（与 ADR 0048 的 NTP 徽章相同，形态不同）：
 * 1. **不得用 0 冒充**：三种空态的括号文本都不是数字；
 * 2. **色标只来自 `clockOffsetLevel`**（阈值取生效配置），本模块不另写比较；
 * 3. **误差界越界只进 detail**：`bound_ms > bound_alarm_ms` 追一句「测量质量」提示，
 *    不改变偏差着色（契约 §2.8：两个判定对象独立）。
 *
 * @param clock 已由 store 净化的域（`null` = 配置关闭）
 * @param thresholds 生效配置的三个阈值（配置镜像未到达时由调用方回退契约默认值）
 * @param snapshotPresent 快照是否已到达（未到达时连「配置是否关闭」都不知道，只能显示等待态）
 * @param probeAgeText 「上次探测多久以前」的现成文案（`null` = 无法表达）；由调用方用
 *        `clockProbeAgeSec` + `formatAge` 算出——本模块不复制第二份相对时间口径
 */
export function clockDeviation(
  clock: SysmonClockInfo | null,
  thresholds: ClockThresholds,
  snapshotPresent: boolean,
  probeAgeText: string | null,
): ClockDeviation | null {
  if (!snapshotPresent) {
    return {
      text: `(${CLOCK_OFFSET_UNAVAILABLE_TEXT})`,
      level: null,
      aria: '时钟偏差等待快照',
      detail: '时钟偏差（等待 dzsysmon 快照）',
    }
  }
  if (clock === null) return null

  const ageSuffix = probeAgeText === null ? '' : `；上次探测 ${probeAgeText}前`
  if (clock.last_probe_unix_ms === 0) {
    return {
      text: `(${CLOCK_OFFSET_UNAVAILABLE_TEXT})`,
      level: null,
      aria: '时钟偏差尚未探测',
      detail: `${CLOCK_UNPROBED_TITLE}：${CLOCK_UNPROBED_DESC}`,
    }
  }
  if (clock.offset_ms === null) {
    // store 已保证偏移与误差界同生共死；这里按「无偏差读数」处理，不用 0 冒充
    const reason = clockFailureSummary(clock.servers)
    return {
      text: `(${CLOCK_OFFSET_UNAVAILABLE_TEXT})`,
      level: null,
      aria: '时钟偏差不可用',
      detail: `时钟偏差${UNAVAILABLE_TEXT}：${reason}${ageSuffix}`,
    }
  }

  const level = clockOffsetLevel(clock.offset_ms, thresholds)
  const signed = formatSignedMilliseconds(clock.offset_ms)
  const direction = clock.offset_ms > 0 ? '正 = 本地快于参考源' : clock.offset_ms < 0 ? '负 = 本地慢于参考源' : '本地与参考源一致'
  const levelText = level === null ? '' : deviationLevelText(level)
  const measured = [
    `偏差 ${signed}`,
    `误差界 ${formatMilliseconds(clock.bound_ms)}`,
    `来源 ${clock.source || UNAVAILABLE_TEXT}（stratum ${clock.source_stratum ?? UNAVAILABLE_TEXT}）`,
  ].join('；')
  const quality = clockBoundAlarm(clock.bound_ms, thresholds) ? `；${CLOCK_QUALITY_HINT}` : ''
  return {
    // Math.round 到整毫秒：顶栏是常驻窄条，判据分辨率（100ms / 1000ms）远粗于 1ms；
    // 完整的 1 位小数在 tooltip 里（formatSignedMilliseconds），不丢精度。
    text: `(${formatCompactSignedMilliseconds(clock.offset_ms)})`,
    level,
    aria: `时钟偏差 ${signed}（${levelText}，${direction}）`,
    detail: `${measured}${ageSuffix}${quality}`,
  }
}

/** `topbarClockModel` 的入参：时刻 / 时区 / 偏差域 / 生效阈值 / 快照在场 / 上次探测文案 */
export interface TopbarClockInput {
  /** 当前本机时刻（组件每秒更新；不得用快照 timestamp） */
  now: Date
  /** IANA 时区名（`Intl.DateTimeFormat().resolvedOptions().timeZone`）；空串回退兜底文案 */
  timeZone: string
  /** store 净化后的时钟域（`null` = 配置关闭） */
  clock: SysmonClockInfo | null
  /** 生效配置的三个阈值（配置镜像未到达时用契约默认值） */
  thresholds: ClockThresholds
  /** 快照是否已到达 */
  snapshotPresent: boolean
  /** 「上次探测多久以前」的现成文案（相对快照 timestamp 计算） */
  probeAgeText: string | null
}

/** 顶栏墙钟的完整展示模型（模板只做四段 + 括号的渲染） */
export interface TopbarClockModel extends ClockTextParts {
  /** 偏差括号；`null` = 配置关闭，整段括号不渲染 */
  deviation: ClockDeviation | null
  /** `<time>` 的 title：时区 + 偏差完整说明（含误差界 / 来源 / stratum / 上次探测） */
  title: string
  /** `<time>` 的 aria-label：完整日期星期时间 + 偏差状态（色标之外的冗余信号） */
  ariaLabel: string
}

/**
 * 顶栏墙钟展示模型：四档文本片段 + 偏差括号 + title / aria 文案。
 *
 * `title` 与 `ariaLabel` 只在此处拼装（模板不散写字面量），因此「tooltip 含哪些字段」与
 * 「读屏念什么」在同一处可测、可改。
 */
export function topbarClockModel(input: TopbarClockInput): TopbarClockModel {
  const parts = clockTextParts(input.now)
  const timeZone = input.timeZone.trim() || CLOCK_TIME_ZONE_FALLBACK
  const deviation = clockDeviation(
    input.clock,
    input.thresholds,
    input.snapshotPresent,
    input.probeAgeText,
  )
  const deviationDetail = deviation === null ? CLOCK_DISABLED_TITLE : deviation.detail
  const deviationAria = deviation === null ? CLOCK_DISABLED_TITLE : deviation.aria
  return {
    ...parts,
    deviation,
    title: `时区 ${timeZone}；${deviationDetail}`,
    ariaLabel: `${parts.dateFull} ${parts.weekday} ${parts.time}，${deviationAria}`,
  }
}
