import { describe, it, expect } from 'vitest'
import { tdPanelEmptyText, tdPanelState, tdPanelUnknownBannerText } from '../tdPanelState'
import type { TdPanelStateInput } from '../tdPanelState'

/**
 * 仪表盘交易面板空态状态机（#358）。
 *
 * 这里固定的是「无法确认数据 ≠ 暂无数据」的判定口径：
 * - 五态互斥，优先级固定 degraded → loading → error → no-accounts → empty；
 * - 账户全集为空 = 无法确认（未知态），不得落到「暂无」；
 * - 只有「账户集合已知且确实无行」才是正常空态。
 */
function input(overrides: Partial<TdPanelStateInput> = {}): TdPanelStateInput {
  return {
    available: true,
    loading: false,
    initialized: true,
    error: null,
    hasAccounts: true,
    ...overrides,
  }
}

describe('tdPanelState（五态互斥，优先级固定）', () => {
  it('degraded：!available 压过其余一切状态（降级横幅优先）', () => {
    expect(tdPanelState(input({ available: false }))).toBe('degraded')
    expect(tdPanelState(input({
      available: false,
      loading: true,
      initialized: false,
      error: 'HTTP 500',
      hasAccounts: false,
    }))).toBe('degraded')
  })

  it('loading：请求在途或尚未初始化 → loading（优先于 error 与 no-accounts）', () => {
    expect(tdPanelState(input({ loading: true }))).toBe('loading')
    expect(tdPanelState(input({ initialized: false }))).toBe('loading')
    expect(tdPanelState(input({ loading: true, error: 'HTTP 500', hasAccounts: false }))).toBe('loading')
    expect(tdPanelState(input({ initialized: false, error: 'HTTP 500', hasAccounts: false }))).toBe('loading')
  })

  it('error：请求失败 → error（优先于 no-accounts：此时账户全集不可信）', () => {
    expect(tdPanelState(input({ error: 'HTTP 500' }))).toBe('error')
    expect(tdPanelState(input({ error: 'HTTP 500', hasAccounts: false }))).toBe('error')
  })

  it('no-accounts：成功读到基线但账户全集为空 → 未知态（不得落到 empty）', () => {
    expect(tdPanelState(input({ hasAccounts: false }))).toBe('no-accounts')
  })

  it('empty：已知账户集合下确实没有数据行（正常空态）', () => {
    expect(tdPanelState(input({ hasAccounts: true }))).toBe('empty')
  })
})

describe('tdPanelEmptyText（空态槽位文案，按面板种类区分「未知」与「暂无」）', () => {
  it('降级 / 加载 / 失败三态三个面板同文案（与降级横幅、错误行不冲突）', () => {
    for (const kind of ['position', 'fund', 'order'] as const) {
      expect(tdPanelEmptyText('degraded', kind)).toBe('交易数据暂不可用')
      expect(tdPanelEmptyText('loading', kind)).toBe('加载中...')
      expect(tdPanelEmptyText('error', kind)).toBe('加载失败')
    }
  })

  it('no-accounts 明确说「未知」，绝不说「暂无」', () => {
    expect(tdPanelEmptyText('no-accounts', 'position')).toBe('持仓未知')
    expect(tdPanelEmptyText('no-accounts', 'fund')).toBe('资金未知')
    expect(tdPanelEmptyText('no-accounts', 'order')).toBe('委托未知')
  })

  it('empty 才是正常的「暂无」（已知账户集合下的空数据）', () => {
    expect(tdPanelEmptyText('empty', 'position')).toBe('暂无持仓')
    expect(tdPanelEmptyText('empty', 'fund')).toBe('暂无资金数据')
    expect(tdPanelEmptyText('empty', 'order')).toBe('暂无委托')
  })
})

describe('tdPanelUnknownBannerText（面板级未知态提示条文案）', () => {
  it('持仓未知提示条：说明原因与排查方向（交易网关进程 / 账户配置）', () => {
    const text = tdPanelUnknownBannerText('position')
    expect(text).toContain('持仓未知')
    expect(text).toContain('交易网关')
    expect(text).toContain('账户')
    expect(text).not.toContain('暂无')
  })

  it('资金未知提示条：同样的原因与排查方向', () => {
    const text = tdPanelUnknownBannerText('fund')
    expect(text).toContain('资金未知')
    expect(text).toContain('交易网关')
    expect(text).toContain('账户')
    expect(text).not.toContain('暂无')
  })

  it('委托未知提示条：同样的原因与排查方向', () => {
    const text = tdPanelUnknownBannerText('order')
    expect(text).toContain('委托未知')
    expect(text).toContain('交易网关')
    expect(text).toContain('账户')
    expect(text).not.toContain('暂无')
  })
})
