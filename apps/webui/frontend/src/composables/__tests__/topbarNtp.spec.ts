import { describe, it, expect } from 'vitest'
import { NTP_OFFSET_UNAVAILABLE_TEXT, ntpBadgeModel } from '../topbarNtp'
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

// #452 / ADR 0054 顶栏「时钟偏差」图标徽章的纯函数单测：四态、阈值边界（含「恰好等于」）、
// 配置缺失回退默认值、数值文本 / 读屏名 / tooltip 字段。组件只负责渲染，这里锁死口径。

const THRESHOLDS = resolveClockThresholds(null)  // 契约默认 100 / 1000 / 200

function badge(clock: Parameters<typeof ntpBadgeModel>[0], overrides: {
  thresholds?: Parameters<typeof ntpBadgeModel>[1]
  snapshotPresent?: boolean
  probeAgeText?: string | null
} = {}) {
  return ntpBadgeModel(
    clock,
    overrides.thresholds ?? THRESHOLDS,
    overrides.snapshotPresent ?? true,
    overrides.probeAgeText ?? null,
  )
}

describe('ntpBadgeModel：四态（契约 sysmon §2.9 的空态一一对应）', () => {
  it('快照尚未到达 → `—` + 中性 + 等待说明（此时连配置是否关闭都不知道）', () => {
    for (const clock of [null, SYSMON_CLOCK_LIVE, SYSMON_CLOCK_UNPROBED]) {
      expect(badge(clock, { snapshotPresent: false, probeAgeText: null })).toEqual({
        value: NTP_OFFSET_UNAVAILABLE_TEXT,
        level: null,
        ariaLabel: '时钟偏差等待快照',
        title: '时钟偏差（等待 dzsysmon 快照）',
      })
    }
  })

  it('配置关闭（clock = null）→ 返回 null，整枚徽章不渲染', () => {
    expect(badge(null, { snapshotPresent: true })).toBeNull()
  })

  it('尚未探测 → `—` + 中性 + 尚未探测说明（不得用 0 冒充）', () => {
    const model = badge(SYSMON_CLOCK_UNPROBED, { probeAgeText: null })
    expect(model?.value).toBe(NTP_OFFSET_UNAVAILABLE_TEXT)
    expect(model?.level).toBeNull()
    expect(model?.ariaLabel).toBe('时钟偏差尚未探测')
    expect(model?.title).toContain('尚未探测')
  })

  it('本轮全部失败 → `—` + 中性 + 失败原因汇总 + 上次探测时间', () => {
    const model = badge(SYSMON_CLOCK_ALL_FAILED, { probeAgeText: '30 秒' })
    expect(model?.value).toBe(NTP_OFFSET_UNAVAILABLE_TEXT)
    expect(model?.level).toBeNull()
    expect(model?.ariaLabel).toBe('时钟偏差不可用')
    expect(model?.title).toContain('不可用')
    expect(model?.title).toContain('超时未响应')
    expect(model?.title).toContain('无法解析或连接失败')
    expect(model?.title).toContain('上次探测 30 秒前')
    expect(model?.value).not.toContain('0')
  })

  it('正常态 → 紧凑带符号整数毫秒 + 按 |offset| 定档 + 完整 title / aria', () => {
    const model = badge(SYSMON_CLOCK_LIVE, { probeAgeText: '30 秒' })
    // 12.5 ms → `+13ms`（Math.round 到最近整毫秒；精确值在 title 里）
    expect(model?.value).toBe('+13ms')
    expect(model?.level).toBe('normal')
    expect(model?.ariaLabel).toBe('时钟偏差 +13ms，正常')
    expect(model?.title).toContain('时钟偏差 +12.5 ms')
    expect(model?.title).toContain('正 = 本地快于参考源')
    expect(model?.title).toContain('误差界 18.3 ms')
    expect(model?.title).toContain('来源 ntp.aliyun.com（stratum 2）')
    expect(model?.title).toContain('上次探测 30 秒前')
    // 误差界 18.3 < 200：不带测量质量提示
    expect(model?.title).not.toContain('测量质量')
  })
})

describe('ntpBadgeModel：阈值边界（契约 §2.8：绝对值 ≥ 阈值；边界含）', () => {
  it.each([
    [99, 'normal', '正常'],
    [100, 'warn', '偏大'],        // 恰好等于 warn 阈值算进入
    [-100, 'warn', '偏大'],       // 负偏差按绝对值定档
    [999, 'warn', '偏大'],
    [1000, 'danger', '过大'],     // 恰好等于 error 阈值算进入
    [-1000, 'danger', '过大'],
  ] as Array<[number, string, string]>)('|offset|=%s → %s（aria 含「%s」）', (offsetMs, level, levelText) => {
    const model = badge({ ...SYSMON_CLOCK_LIVE, offset_ms: offsetMs })
    expect(model?.level).toBe(level)
    expect(model?.ariaLabel).toContain(levelText)
  })

  it('负偏差数值文本保留负号（方向是语义的一部分），并按绝对值着色', () => {
    const model = badge({ ...SYSMON_CLOCK_LIVE, offset_ms: -1200 })
    expect(model?.value).toBe('-1200ms')
    expect(model?.level).toBe('danger')
    expect(model?.ariaLabel).toContain('过大')
    expect(model?.title).toContain('负 = 本地慢于参考源')
  })

  it('恰好 0 偏差显示 `0ms`（无方向可表，不加符号）且为 normal', () => {
    const model = badge({ ...SYSMON_CLOCK_LIVE, offset_ms: 0 })
    expect(model?.value).toBe('0ms')
    expect(model?.level).toBe('normal')
    expect(model?.title).toContain('本地与参考源一致')
  })

  it('阈值取生效配置（不是写死的 100 / 1000）：同一读数在不同阈值下档位不同', () => {
    const live = { ...SYSMON_CLOCK_LIVE, offset_ms: 150 }
    expect(badge(live, { thresholds: { warn_ms: 200, error_ms: 1000, bound_alarm_ms: 400 } })?.level).toBe('normal')
    expect(badge(live, { thresholds: { warn_ms: 100, error_ms: 1000, bound_alarm_ms: 400 } })?.level).toBe('warn')
    expect(badge(live, { thresholds: { warn_ms: 100, error_ms: 150, bound_alarm_ms: 400 } })?.level).toBe('danger')
  })

  it('配置镜像缺失时回退契约默认值 100 / 1000（resolveClockThresholds 的既有口径）', () => {
    const fallback = resolveClockThresholds(undefined)
    expect(fallback.warn_ms).toBe(CLOCK_DEFAULT_OFFSET_WARN_MS)
    expect(fallback.error_ms).toBe(CLOCK_DEFAULT_OFFSET_ERROR_MS)
    expect(badge({ ...SYSMON_CLOCK_LIVE, offset_ms: CLOCK_DEFAULT_OFFSET_WARN_MS }, { thresholds: fallback })?.level)
      .toBe('warn')
    expect(badge({ ...SYSMON_CLOCK_LIVE, offset_ms: CLOCK_DEFAULT_OFFSET_WARN_MS - 1 }, { thresholds: fallback })?.level)
      .toBe('normal')
    expect(badge({ ...SYSMON_CLOCK_LIVE, offset_ms: CLOCK_DEFAULT_OFFSET_ERROR_MS }, { thresholds: fallback })?.level)
      .toBe('danger')
  })

  it('误差界越界只进 title 的「测量质量」提示，不改变偏差着色（契约 §2.8）', () => {
    const model = badge({ ...SYSMON_CLOCK_LIVE, offset_ms: 20, bound_ms: 500 })
    expect(model?.level).toBe('normal')
    expect(model?.title).toContain('测量质量')
  })

  it('无法表达上次探测时间时，title 不出现「上次探测」片段（不编造相对时间）', () => {
    expect(badge(SYSMON_CLOCK_LIVE)?.title).not.toContain('上次探测')
  })
})
