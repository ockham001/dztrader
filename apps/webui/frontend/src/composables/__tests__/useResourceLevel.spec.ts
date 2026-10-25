import { describe, it, expect } from 'vitest'
import {
  RESOURCE_DANGER_RATIO,
  RESOURCE_WARN_RATIO,
  resourceLevel,
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
