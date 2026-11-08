import { describe, it, expect } from 'vitest'
import {
  RESOURCE_DANGER_RATIO,
  RESOURCE_WARN_RATIO,
  resourceLevel,
  thresholdLevel,
  worstResourceLevel,
} from '../useResourceLevel'

describe('resourceLevel 阈值边界（含边界）', () => {
  // 与契约一致：≥70% 黄、≥90% 红；边界点是「恰好等于」也进入上一级，
  // 这样与顶栏徽章 / 监控页卷列表共用同一条规则，避免两侧各自比较时
  // 一处用 > 一处用 ≥ 而在阈值点给出不同颜色。
  const cases: Array<[number, string]> = [
    [0, 'normal'],
    [0.6999, 'normal'],
    [RESOURCE_WARN_RATIO, 'warn'],       // 恰好 70%
    [0.75, 'warn'],
    [0.8999, 'warn'],
    [RESOURCE_DANGER_RATIO, 'danger'],   // 恰好 90%
    [1, 'danger'],
  ]

  it.each(cases)('ratio=%s → %s', (ratio, expected) => {
    expect(resourceLevel(ratio)).toBe(expected)
  })
})

describe('thresholdLevel 数值 + 双档阈值定档（#302：资源类与时钟偏差共用同一语义）', () => {
  // 时钟偏差的阈值来自配置（契约 sysmon §3.1 `offset_warn_ms` / `offset_error_ms`），
  // 资源类来自本模块常量。两者共用同一份「≥ 阈值即进入该档」判定与等级词表。
  const cases: Array<[number, number, number, string]> = [
    [0, 100, 1000, 'normal'],
    [99.9, 100, 1000, 'normal'],
    [100, 100, 1000, 'warn'],        // 恰好等于 warn 阈值算进入（契约 §2.8：≥）
    [999, 100, 1000, 'warn'],
    [1000, 100, 1000, 'danger'],     // 恰好等于 error 阈值算进入（契约 §2.8：≥）
    [5000, 100, 1000, 'danger'],
  ]

  it.each(cases)('value=%s warn=%s danger=%s → %s', (value, warn, danger, expected) => {
    expect(thresholdLevel(value, warn, danger)).toBe(expected)
  })

  it('resourceLevel 的阈值点定档与 thresholdLevel 同口径（字面等级，不拿转发目标当 oracle）', () => {
    // 用字面等级做期望：`resourceLevel` 只是转发 `thresholdLevel`，拿后者当期望等于
    // 「自己等于自己」——转发写坏时两边同时变，这条断言就永远不会红。
    expect(resourceLevel(RESOURCE_WARN_RATIO)).toBe('warn')
    expect(resourceLevel(RESOURCE_DANGER_RATIO)).toBe('danger')
    expect(resourceLevel(0.5)).toBe('normal')
  })
})

describe('worstResourceLevel 多读数取最差等级', () => {
  it('取 danger > warn > normal 中最差的等级（与顺序无关）', () => {
    expect(worstResourceLevel(['normal', 'warn'])).toBe('warn')
    expect(worstResourceLevel(['warn', 'normal'])).toBe('warn')
    expect(worstResourceLevel(['danger', 'warn', 'normal'])).toBe('danger')
    expect(worstResourceLevel(['normal'])).toBe('normal')
  })

  it('null（不可判读）不参与聚合', () => {
    expect(worstResourceLevel([null, 'normal'])).toBe('normal')
    expect(worstResourceLevel(['warn', null, 'normal'])).toBe('warn')
  })

  it('空集合 / 全部 null → null（调用方决定中性展示，不得默认 normal）', () => {
    expect(worstResourceLevel([])).toBeNull()
    expect(worstResourceLevel([null, null])).toBeNull()
  })
})
