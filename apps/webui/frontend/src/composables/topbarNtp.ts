/**
 * 顶栏「时钟偏差」图标徽章的展示模型（#452 / ADR 0054）
 *
 * 契约是唯一语义真相源：sysmon §2.9 定义 `clock` 域的 payload 与三种空态、§2.8 定义两个
 * 时间判定对象、§3.1 定义 `clock` 配置段。本模块只承载**顶栏那一枚时钟偏差图标徽章**的
 * 展示口径（顶栏没有日期时间 / 墙钟，也没有独立文字 `NTP` 徽章）：
 *
 * 1. **数值只有一档状态色，且只作用于数值文本**：正常中性、`|offset|` 达
 *    `clock.offset_warn_ms` 变黄、达 `offset_error_ms` 变红；图标本身永远中性色。
 *    颜色只是冗余信号（DESIGN「The One Job Rule」：一个状态色只表达一种事实），读屏另有
 *    档位词（正常 / 偏大 / 过大）。
 * 2. **空态不得用 0 冒充**：快照未到达 / 尚未探测 / 本轮全部失败显示 `—`；`clock = null`
 *    （配置关闭）返回 `null`，整枚徽章不渲染（配置关闭是运维事实，不放 0 或空壳）。
 * 3. **定档只有一个实现**：偏差等级来自 `sysmonClock.clockOffsetLevel`（阈值取生效配置，
 *    配置镜像缺失由调用方回退契约默认值），本模块不另写比较。
 *
 * 纯函数、不读 store / DOM / 时间：组件只负责把 store 里的快照域与配置阈值传进来。
 */
import {
  CLOCK_QUALITY_HINT,
  CLOCK_UNPROBED_DESC,
  CLOCK_UNPROBED_TITLE,
  clockBoundAlarm,
  clockDomainState,
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

/** 没有可用读数时的唯一占位（全角破折号，与监控页不可用占位同一形态） */
export const NTP_OFFSET_UNAVAILABLE_TEXT = '—'

/**
 * 顶栏时钟偏差徽章的展示模型：
 * - `value`：徽章上的数值文本（紧凑带符号整数毫秒 `+13ms` / 空态 `—`）；
 * - `level`：数值文本的状态等级（`null` = 中性；normal 也用中性色，只有 warn / danger 着色）；
 * - `ariaLabel`：读屏名（含档位词，颜色之外的冗余信号）；
 * - `title`：悬停完整说明（含误差界 / 来源 / stratum / 上次探测 / 测量质量提示）。
 */
export interface NtpBadgeModel {
  value: string
  level: ResourceLevel | null
  ariaLabel: string
  title: string
}

/** 偏差档位 → 中文状态词（读屏用；色标之外的冗余信号，WCAG 1.4.1） */
function deviationLevelText(level: ResourceLevel): string {
  if (level === 'danger') return '过大'
  if (level === 'warn') return '偏大'
  return '正常'
}

/**
 * 时钟偏差徽章模型（四态与契约 §2.9 的域内空态一一对应）：
 *
 * - `snapshotPresent=false`（快照尚未到达）→ `—` + 中性 + 「等待 dzsysmon 快照」；
 * - `clock = null`（该域被配置关闭）→ 返回 `null`，**整枚徽章不渲染**；
 * - `last_probe_unix_ms = 0`（尚未探测）→ `—` + 中性 + 「尚未探测」；
 * - `> 0` 且 `offset_ms = null`（本轮全部失败）→ `—` + 中性 + 失败原因汇总；
 * - `live` → 紧凑带符号整数毫秒 + 按 `|offset_ms|` 定档（只有数值文本着色）。
 *
 * 三条硬约束：
 * 1. **不得用 0 冒充**：三种空态的 value 都不是数字；
 * 2. **色标只来自 `clockOffsetLevel`**（阈值取生效配置），本模块不另写比较；
 * 3. **误差界越界只进 title**：`bound_ms > bound_alarm_ms` 追一句「测量质量」提示，
 *    不改变偏差着色（契约 §2.8：两个判定对象独立）。
 *
 * @param clock 已由 store 净化的域（`null` = 配置关闭）
 * @param thresholds 生效配置的三个阈值（配置镜像未到达时由调用方回退契约默认值）
 * @param snapshotPresent 快照是否已到达（未到达时连「配置是否关闭」都不知道，只能显示等待态）
 * @param probeAgeText 「上次探测多久以前」的现成文案（`null` = 无法表达）；由调用方用
 *        `clockProbeAgeSec` + `formatAge` 算出——本模块不复制第二份相对时间口径
 */
export function ntpBadgeModel(
  clock: SysmonClockInfo | null,
  thresholds: ClockThresholds,
  snapshotPresent: boolean,
  probeAgeText: string | null,
): NtpBadgeModel | null {
  if (!snapshotPresent) {
    return {
      value: NTP_OFFSET_UNAVAILABLE_TEXT,
      level: null,
      ariaLabel: '时钟偏差等待快照',
      title: '时钟偏差（等待 dzsysmon 快照）',
    }
  }
  if (clock === null) return null
  // 域状态分类的唯一实现是 sysmonClock.clockDomainState（本模块不重推同一判定）
  const domain = clockDomainState(clock)
  const ageSuffix = probeAgeText === null ? '' : `；上次探测 ${probeAgeText}前`
  if (domain === 'unprobed') {
    return {
      value: NTP_OFFSET_UNAVAILABLE_TEXT,
      level: null,
      ariaLabel: '时钟偏差尚未探测',
      title: `时钟偏差（${CLOCK_UNPROBED_TITLE}）：${CLOCK_UNPROBED_DESC}`,
    }
  }
  if (domain === 'unavailable' || clock.offset_ms === null) {
    // store 已保证偏移与误差界同生共死；这里按「无偏差读数」处理，不用 0 冒充
    const reason = clockFailureSummary(clock.servers)
    return {
      value: NTP_OFFSET_UNAVAILABLE_TEXT,
      level: null,
      ariaLabel: '时钟偏差不可用',
      title: `时钟偏差${UNAVAILABLE_TEXT}：${reason}${ageSuffix}`,
    }
  }

  const level = clockOffsetLevel(clock.offset_ms, thresholds)
  const signed = formatSignedMilliseconds(clock.offset_ms)
  const levelText = level === null ? '' : deviationLevelText(level)
  const direction = clock.offset_ms > 0
    ? '正 = 本地快于参考源'
    : clock.offset_ms < 0 ? '负 = 本地慢于参考源' : '本地与参考源一致'
  const quality = clockBoundAlarm(clock.bound_ms, thresholds) ? `；${CLOCK_QUALITY_HINT}` : ''
  // 完整 1 位小数在 title 里（formatSignedMilliseconds），徽章上按整数毫秒（判据分辨率足够）
  const value = formatCompactSignedMilliseconds(clock.offset_ms)
  return {
    value,
    level,
    ariaLabel: `时钟偏差 ${value}，${levelText}`,
    title: `时钟偏差 ${signed}（${levelText}，${direction}）`
      + `；误差界 ${formatMilliseconds(clock.bound_ms)}`
      + `；来源 ${clock.source || UNAVAILABLE_TEXT}（stratum ${clock.source_stratum ?? UNAVAILABLE_TEXT}）`
      + `${ageSuffix}${quality}`,
  }
}
