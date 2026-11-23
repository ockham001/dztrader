import { describe, it, expect } from 'vitest'
import {
  CLOCK_OFFSET_UNAVAILABLE_TEXT,
  CLOCK_TIME_ZONE_FALLBACK,
  clockDeviation,
  clockTextParts,
  topbarClockModel,
} from '../topbarClock'
import {
  CLOCK_DEFAULT_OFFSET_ERROR_MS,
  CLOCK_DEFAULT_OFFSET_WARN_MS,
  resolveClockThresholds,
} from '../sysmonClock'
import {
  SYSMON_CLOCK_ALL_FAILED,
  SYSMON_CLOCK_LIVE,
  SYSMON_CLOCK_UNPROBED,
} from '@/test-utils/sysmonSnapshot'

// #452 / ADR 0054 顶栏墙钟的纯函数单测：四段文本、偏差括号四态、阈值边界（含「恰好等于」）、
// 配置缺失回退默认值、title / aria 字段。组件只负责每秒 tick 与渲染，这里锁死口径。

const THRESHOLDS = resolveClockThresholds(null)  // 契约默认 100 / 1000 / 200

/** 固定时刻：2026-01-01（周四）13:25:36 本地时间 */
const FIXED_NOW = new Date(2026, 0, 1, 13, 25, 36)

function model(overrides: Partial<Parameters<typeof topbarClockModel>[0]> = {}) {
  return topbarClockModel({
    now: FIXED_NOW,
    timeZone: 'Asia/Shanghai',
    clock: SYSMON_CLOCK_LIVE,
    thresholds: THRESHOLDS,
    snapshotPresent: true,
    probeAgeText: '30 秒',
    ...overrides,
  })
}

describe('clockTextParts：本机墙钟拆段（24h、本地时区、补零）', () => {
  it('拆出完整日期 / 短日期 / 周几 / 时分秒 与 <time datetime> 属性', () => {
    expect(clockTextParts(FIXED_NOW)).toEqual({
      dateFull: '2026-01-01',
      dateShort: '01-01',
      weekday: '周四',
      time: '13:25:36',
      dateTime: '2026-01-01T13:25:36',
    })
  })

  it('个位数月 / 日 / 时 / 分 / 秒一律补足两位（固定宽度扫读不跳位）', () => {
    const parts = clockTextParts(new Date(2026, 8, 7, 9, 5, 6))
    expect(parts.dateFull).toBe('2026-09-07')
    expect(parts.dateShort).toBe('09-07')
    expect(parts.time).toBe('09:05:06')
  })

  it('星期日 → 周日、星期三 → 周三（中文星期表）', () => {
    expect(clockTextParts(new Date(2026, 0, 4)).weekday).toBe('周日')
    expect(clockTextParts(new Date(2026, 0, 7)).weekday).toBe('周三')
  })
})

describe('clockDeviation：偏差括号四态（契约 sysmon §2.9 的空态一一对应）', () => {
  it('快照尚未到达 → `(—)` + 中性 + 等待说明（此时连配置是否关闭都不知道）', () => {
    for (const clock of [null, SYSMON_CLOCK_LIVE, SYSMON_CLOCK_UNPROBED]) {
      const deviation = clockDeviation(clock, THRESHOLDS, false, null)
      expect(deviation).toEqual({
        text: `(${CLOCK_OFFSET_UNAVAILABLE_TEXT})`,
        level: null,
        aria: '时钟偏差等待快照',
        detail: '时钟偏差（等待 dzsysmon 快照）',
      })
    }
  })

  it('配置关闭（clock = null）→ 返回 null，整段括号不渲染', () => {
    expect(clockDeviation(null, THRESHOLDS, true, null)).toBeNull()
  })

  it('尚未探测 → `(—)` + 中性，detail 说明尚未探测（不得用 0 冒充）', () => {
    const deviation = clockDeviation(SYSMON_CLOCK_UNPROBED, THRESHOLDS, true, null)
    expect(deviation?.text).toBe(`(${CLOCK_OFFSET_UNAVAILABLE_TEXT})`)
    expect(deviation?.level).toBeNull()
    expect(deviation?.detail).toContain('尚未探测')
  })

  it('本轮全部失败 → `(—)` + 中性 + 失败原因汇总 + 上次探测时间', () => {
    const deviation = clockDeviation(SYSMON_CLOCK_ALL_FAILED, THRESHOLDS, true, '30 秒')
    expect(deviation?.text).toBe(`(${CLOCK_OFFSET_UNAVAILABLE_TEXT})`)
    expect(deviation?.level).toBeNull()
    expect(deviation?.detail).toContain('不可用')
    expect(deviation?.detail).toContain('超时未响应')
    expect(deviation?.detail).toContain('无法解析或连接失败')
    expect(deviation?.detail).toContain('上次探测 30 秒前')
    expect(deviation?.text).not.toContain('0')
  })

  it('正常态 → 紧凑带符号整数毫秒 + 按 |offset| 定档 + 完整 detail', () => {
    const deviation = clockDeviation(SYSMON_CLOCK_LIVE, THRESHOLDS, true, '30 秒')
    // 12.5 ms → `+13ms`（Math.round 到最近整毫秒；精确值在 title 里）
    expect(deviation?.text).toBe('(+13ms)')
    expect(deviation?.level).toBe('normal')
    expect(deviation?.detail).toContain('偏差 +12.5 ms')
    expect(deviation?.detail).toContain('误差界 18.3 ms')
    expect(deviation?.detail).toContain('来源 ntp.aliyun.com（stratum 2）')
    expect(deviation?.detail).toContain('上次探测 30 秒前')
    // 误差界 18.3 < 200：不带测量质量提示
    expect(deviation?.detail).not.toContain('测量质量')
  })

  it.each([
    [99, 'normal'],
    [100, 'warn'],        // 恰好等于 warn 阈值算进入（阈值语义 ≥）
    [-100, 'warn'],       // 负偏差按绝对值定档
    [999, 'warn'],
    [1000, 'danger'],     // 恰好等于 error 阈值算进入
    [-1000, 'danger'],
  ] as Array<[number, string]>)('|offset|=%s → 括号等级 %s', (offsetMs, expected) => {
    const deviation = clockDeviation({ ...SYSMON_CLOCK_LIVE, offset_ms: offsetMs }, THRESHOLDS, true, null)
    expect(deviation?.level).toBe(expected)
    expect(deviation?.aria).toContain(expected === 'normal' ? '正常' : expected === 'warn' ? '偏大' : '过大')
  })

  it('负偏差文本保留负号（方向是语义的一部分），并按绝对值着色', () => {
    const deviation = clockDeviation({ ...SYSMON_CLOCK_LIVE, offset_ms: -1200 }, THRESHOLDS, true, null)
    expect(deviation?.text).toBe('(-1200ms)')
    expect(deviation?.level).toBe('danger')
    expect(deviation?.aria).toContain('负 = 本地慢于参考源')
  })

  it('恰好 0 偏差显示 `(0ms)`（无方向可表，不加符号）且为 normal', () => {
    const deviation = clockDeviation({ ...SYSMON_CLOCK_LIVE, offset_ms: 0 }, THRESHOLDS, true, null)
    expect(deviation?.text).toBe('(0ms)')
    expect(deviation?.level).toBe('normal')
    expect(deviation?.aria).toContain('本地与参考源一致')
  })

  it('阈值取生效配置（不是写死的 100 / 1000）：同一读数在不同阈值下档位不同', () => {
    const live = { ...SYSMON_CLOCK_LIVE, offset_ms: 150 }
    expect(clockDeviation(live, { warn_ms: 200, error_ms: 1000, bound_alarm_ms: 400 }, true, null)?.level).toBe('normal')
    expect(clockDeviation(live, { warn_ms: 100, error_ms: 1000, bound_alarm_ms: 400 }, true, null)?.level).toBe('warn')
    expect(clockDeviation(live, { warn_ms: 100, error_ms: 150, bound_alarm_ms: 400 }, true, null)?.level).toBe('danger')
  })

  it('配置镜像缺失时回退契约默认值 100 / 1000（resolveClockThresholds 的既有口径）', () => {
    const fallback = resolveClockThresholds(undefined)
    expect(fallback.warn_ms).toBe(CLOCK_DEFAULT_OFFSET_WARN_MS)
    expect(fallback.error_ms).toBe(CLOCK_DEFAULT_OFFSET_ERROR_MS)
    expect(clockDeviation({ ...SYSMON_CLOCK_LIVE, offset_ms: CLOCK_DEFAULT_OFFSET_WARN_MS }, fallback, true, null)?.level)
      .toBe('warn')
    expect(clockDeviation({ ...SYSMON_CLOCK_LIVE, offset_ms: CLOCK_DEFAULT_OFFSET_WARN_MS - 1 }, fallback, true, null)?.level)
      .toBe('normal')
    expect(clockDeviation({ ...SYSMON_CLOCK_LIVE, offset_ms: CLOCK_DEFAULT_OFFSET_ERROR_MS }, fallback, true, null)?.level)
      .toBe('danger')
  })

  it('误差界越界只进 detail 的「测量质量」提示，不改变偏差着色（契约 §2.8）', () => {
    const deviation = clockDeviation(
      { ...SYSMON_CLOCK_LIVE, offset_ms: 20, bound_ms: 500 },
      THRESHOLDS,
      true,
      null,
    )
    expect(deviation?.level).toBe('normal')
    expect(deviation?.detail).toContain('测量质量')
  })

  it('无法表达上次探测时间时，detail 不出现「上次探测」片段（不编造相对时间）', () => {
    expect(clockDeviation(SYSMON_CLOCK_LIVE, THRESHOLDS, true, null)?.detail).not.toContain('上次探测')
  })
})

describe('topbarClockModel：四档文本片段 + title / aria 文案', () => {
  it('四段文本片段恒在模型里（可见性由 CSS 断点决定，秒永远保留）', () => {
    const m = model()
    expect(m.dateFull).toBe('2026-01-01')
    expect(m.dateShort).toBe('01-01')
    expect(m.weekday).toBe('周四')
    expect(m.time).toBe('13:25:36')
    expect(m.deviation?.text).toBe('(+13ms)')
  })

  it('title 含时区 / 偏差（1 位小数带符号）/ 误差界 / 来源 / stratum / 上次探测相对时间', () => {
    const title = model().title
    expect(title).toContain('时区 Asia/Shanghai')
    expect(title).toContain('偏差 +12.5 ms')
    expect(title).toContain('误差界 18.3 ms')
    expect(title).toContain('来源 ntp.aliyun.com')
    expect(title).toContain('stratum 2')
    expect(title).toContain('上次探测 30 秒前')
  })

  it('偏差不可用时 title 给出原因（失败原因汇总 / 尚未探测）', () => {
    expect(model({ clock: SYSMON_CLOCK_ALL_FAILED }).title).toContain('超时未响应')
    expect(model({ clock: SYSMON_CLOCK_ALL_FAILED }).title).toContain('不可用')
    expect(model({ clock: SYSMON_CLOCK_UNPROBED }).title).toContain('尚未探测')
  })

  it('配置关闭（clock = null）：括号不渲染，title / aria 说明监控已关闭', () => {
    const m = model({ clock: null })
    expect(m.deviation).toBeNull()
    expect(m.title).toContain('时区 Asia/Shanghai')
    expect(m.title).toContain('关闭')
    expect(m.ariaLabel).toContain('关闭')
  })

  it('aria-label = 完整日期星期时间 + 偏差状态（含档位词，色标之外的冗余信号）', () => {
    const m = model()
    expect(m.ariaLabel).toContain('2026-01-01 周四 13:25:36')
    expect(m.ariaLabel).toContain('时钟偏差 +12.5 ms')
    expect(m.ariaLabel).toContain('正常')
  })

  it('aria-label 的偏差状态随档位变化（偏大 / 过大）', () => {
    expect(model({ clock: { ...SYSMON_CLOCK_LIVE, offset_ms: 100 } }).ariaLabel).toContain('偏大')
    expect(model({ clock: { ...SYSMON_CLOCK_LIVE, offset_ms: 1000 } }).ariaLabel).toContain('过大')
  })

  it('时区取不到时回退兜底文案（tooltip 不得出现「时区 」空值）', () => {
    expect(model({ timeZone: '' }).title).toContain(CLOCK_TIME_ZONE_FALLBACK)
    expect(model({ timeZone: '   ' }).title).toContain(CLOCK_TIME_ZONE_FALLBACK)
  })
})
