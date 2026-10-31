import { describe, it, expect, vi, beforeEach } from 'vitest'
import { nextTick, type Component } from 'vue'
import { mount } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import CtpCard from '../CtpCard.vue'
import GenericCard from '../GenericCard.vue'
import MarketSourceCardTemplate from '../MarketSourceCardTemplate.vue'
import BrokerCard from '../BrokerCard.vue'
import { marketSourcesApi } from '@/api/marketSources'
import { useMarketSourcesStore } from '@/stores/marketSources'
import { makeSource } from '@/composables/marketSourceView'
import type { BrokerEntry } from '@/types/api'

// =====================================================================
// #206 修复项 2「键盘不可达」——行情源卡片头可键盘展开/收起。
//
// 断言口径（刻意不只是断言 DOM 属性）：
//   - 按键后断言 **store 的 expanded 状态**确实翻转（GenericCard / CtpCard / 模板卡）；
//   - aria-expanded 双向都覆盖：切换后把新的聚合 view 用 setProps 回灌（生产环境由父级
//     MarketSourcesView 随 store 重渲染传入），再断言 DOM 与 src.expanded 一致；
//   - BrokerCard 的展开态由父级手风琴持有（不是 store），故断言原生 button 的
//     aria-expanded 双向 + toggle 事件；原生 button 的 Enter/Space 由浏览器合成为 click，
//     jsdom 不合成，所以键盘可达性以「原生 button 语义」为断言对象（额外挂 keydown 会与
//     浏览器合成 click 双重触发，反而一次按键切两次）。
// =====================================================================

const dbSource = {
  id: 1,
  source_type: 'CTP',
  source_name: 'dzmd_ctp',
  display_name: 'CTP',
  ui_card: 'ctp',
  is_added: true,
  auto_login: false,
  created_at: '2026-01-01T00:00:00Z',
  updated_at: '2026-01-01T00:00:00Z',
}

const brokerA: BrokerEntry = {
  name: 'A券商',
  broker_id: '0001',
  user_id: 'u1',
  password: '****',
  product_info: '',
  frontends: [{ address: 'tcp://1.2.3.4:10211', label: '电信', enabled: true }],
}

const stubs = {
  Icon: { template: '<span class="icon" />' },
  Modal: { props: ['open', 'title'], template: '<div v-if="open"><slot /><slot name="footer" /></div>' },
  TimePicker: { template: '<span />' },
  StatusIndicator: { template: '<span class="status" />' },
  LoginPanel: { template: '<div class="lp" />' },
  ScheduleManager: { template: '<div class="sm" />' },
  FrontendTable: { props: ['source', 'broker'], template: '<div class="fe-stub" />' },
  ProcessStartButton: { template: '<span class="psb-stub" />' },
}

/// 把一条 DB 源灌进 store：expanded 是 store 的本地 UI 状态（expandedIds），
/// 组件只能通过 store.toggleExpand 改它，因此断言 store.sources[0].expanded 才有效。
async function seedStore(): Promise<void> {
  vi.spyOn(marketSourcesApi, 'list').mockResolvedValue([dbSource])
  await useMarketSourcesStore().loadSources()
}

/// 把 store 当前聚合 view 回灌给卡片（生产环境由父级重渲染传入新对象）。
async function syncSource(w: ReturnType<typeof mount>): Promise<void> {
  await w.setProps({ source: useMarketSourcesStore().sources[0] })
  await nextTick()
}

describe('#206 三张行情源卡片头：Enter/Space 可切换 store.expanded 且 aria-expanded 双向一致', () => {
  beforeEach(() => {
    vi.restoreAllMocks()
    setActivePinia(createPinia())
  })

  const cards: [string, Component][] = [
    ['CtpCard', CtpCard],
    ['GenericCard', GenericCard],
    ['MarketSourceCardTemplate', MarketSourceCardTemplate],
  ]

  for (const [name, card] of cards) {
    it(`${name}: role=button + tabindex=0，Enter 展开、Space 收起，aria-expanded 始终跟随 src.expanded`, async () => {
      await seedStore()
      const store = useMarketSourcesStore()
      const w = mount(card, { props: { source: store.sources[0] }, global: { stubs } })
      await nextTick()

      const header = () => w.find('.source-card__header')
      expect(store.sources[0].expanded).toBe(false)
      expect(header().attributes('role')).toBe('button')
      expect(header().attributes('tabindex')).toBe('0')
      expect(header().attributes('aria-expanded')).toBe('false')

      // Enter → 展开（store 状态翻转，而非只改 DOM）
      await header().trigger('keydown', { key: 'Enter' })
      expect(store.sources[0].expanded).toBe(true)
      await syncSource(w)
      expect(header().attributes('aria-expanded')).toBe('true')

      // Space → 收起（.space 修饰键对应 key === ' '）
      await header().trigger('keydown', { key: ' ' })
      expect(store.sources[0].expanded).toBe(false)
      await syncSource(w)
      expect(header().attributes('aria-expanded')).toBe('false')
    })

    it(`${name}: 点击头部仍可切换（鼠标路径不回归，aria-expanded 同步为 true）`, async () => {
      await seedStore()
      const store = useMarketSourcesStore()
      const w = mount(card, { props: { source: store.sources[0] }, global: { stubs } })
      await nextTick()

      await w.find('.source-card__header').trigger('click')
      expect(store.sources[0].expanded).toBe(true)
      await syncSource(w)
      expect(w.find('.source-card__header').attributes('aria-expanded')).toBe('true')
    })
  }
})

describe('#206 BrokerCard：折叠开关为原生 button（头部含 radio/删除按钮不能整体做 disclosure）', () => {
  beforeEach(() => {
    vi.restoreAllMocks()
    setActivePinia(createPinia())
  })

  function mountBroker(expanded: boolean) {
    const src = makeSource({
      id: 1,
      source_type: 'CTP',
      source_name: 'dzmd_ctp',
      display_name: 'CTP',
      brokers: [brokerA],
      process_state: 'Running', // 进程运行+未登录(isStateIdle=true), radio/删除按钮可用
    })
    return mount(BrokerCard, { props: { source: src, broker: brokerA, expanded }, global: { stubs } })
  }

  it('折叠开关是原生 type=button，aria-expanded 与 expanded 双向一致，展开/收起切换 body', async () => {
    const w = mountBroker(false)
    await nextTick()

    const disclosure = () => w.find('.broker-card__disclosure')
    expect(disclosure().element.tagName).toBe('BUTTON')
    expect(disclosure().attributes('type')).toBe('button')
    expect(disclosure().attributes('aria-expanded')).toBe('false')
    expect(w.find('.broker-card__body').exists()).toBe(false)

    // 点击 → 抛 toggle（生产环境父级手风琴翻 expanded）
    await disclosure().trigger('click')
    expect(w.emitted('toggle')).toHaveLength(1)
    await w.setProps({ expanded: true })
    await nextTick()
    expect(disclosure().attributes('aria-expanded')).toBe('true')
    expect(w.find('.broker-card__body').exists()).toBe(true)

    await disclosure().trigger('click')
    await w.setProps({ expanded: false })
    await nextTick()
    expect(disclosure().attributes('aria-expanded')).toBe('false')
    expect(w.find('.broker-card__body').exists()).toBe(false)
  })

  it('header 自身不再是点击区（无 role/tabindex），radio 与删除按钮仍是其兄弟节点', async () => {
    const w = mountBroker(false)
    await nextTick()

    const header = w.find('.broker-card__header')
    expect(header.attributes('role')).toBeUndefined()
    expect(header.attributes('tabindex')).toBeUndefined()
    // 点击空白处不误切换
    await header.trigger('click')
    expect(w.emitted('toggle')).toBeUndefined()

    // radio 与删除按钮是 disclosure 的兄弟节点，未被 button 包裹（否则非法嵌套交互元素）
    expect(disclosureIsSiblingOfInteractive(w)).toBe(true)
  })

  function disclosureIsSiblingOfInteractive(w: ReturnType<typeof mount>): boolean {
    const header = w.find('.broker-card__header').element
    const radio = w.find('input[type="radio"]').element
    const deleteBtn = w.findAll('button').find(b => b.text() === '删除经纪商')!.element
    return radio.parentElement!.parentElement === header && deleteBtn.parentElement === header
  }
})
