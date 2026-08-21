import { describe, it, expect } from 'vitest'
import GenericCard from '@/components/marketSources/GenericCard.vue'
import CtpCard from '@/components/marketSources/CtpCard.vue'
import { resolveCard } from '../marketSourcesCardRegistry'

// P3 任务 3 领域功能模板：注册表契约的机器强制门禁。
// 保证「新增行情源类型 = 注册表 key + 卡片组件」这条唯一路径不会写错：
//   - 键契约：key 恒小写（与后端 extract_ui_card 输出一致），resolveCard 归一化防误回退；
//   - 命中：ctp → CtpCard；未注册类型 → GenericCard 兜底。

describe('marketSourcesCardRegistry', () => {
  it('resolveCard 对小写 key 命中注册卡片', () => {
    expect(resolveCard('ctp')).toBe(CtpCard)
  })

  it('resolveCard 归一化大小写/空白（类名脏值不悄悄回退 GenericCard）', () => {
    expect(resolveCard('CTP')).toBe(CtpCard)
    expect(resolveCard(' Ctp ')).toBe(CtpCard)
    expect(resolveCard('  ctp\n')).toBe(CtpCard)
  })

  it('组件名（CtpCard 等）不是 ui_card 值，按未命中回退 GenericCard', () => {
    expect(resolveCard('CtpCard')).toBe(GenericCard)
    expect(resolveCard(' CtpCard ')).toBe(GenericCard)
  })

  it('未注册类型回退 GenericCard 兜底', () => {
    expect(resolveCard('')).toBe(GenericCard)
    expect(resolveCard('xtp')).toBe(GenericCard)
    expect(resolveCard('femas')).toBe(GenericCard)
    expect(resolveCard('unknown')).toBe(GenericCard)
  })
})