import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest'
import { mount, flushPromises } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import TradingDataView from '../TradingDataView.vue'
import { __resetTdStatsSchedulerForTests } from '@/stores/tdStats'
import { __resetTdBatchSchedulerForTests } from '@/stores/tdTrades'
import { tdApi } from '@/api/td'

// 子 tab 各自有独立测试（OrdersTab.spec.ts / TradesTab.spec.ts 覆盖各自列表），
// 这里只验证页壳、统计条位置与 tab 切换。断言用面板的内联 display 而非 VTU 的 isVisible：
// jsdom 无布局，isVisible 依赖 offsetWidth 回退，跨用例不稳定。
//
// #248 起成交面板是**真实组件**（不再是占位）：stub 掉只为隔离其 store/API 依赖，
// 并额外断言「挂的是真实列表组件而不是占位文案」。
const stubs = {
  OrdersTab: { template: '<div class="orders-tab-stub">委托面板</div>' },
  TradesTab: { template: '<div class="trades-tab-stub">成交面板</div>' },
}

// 统计条挂载即拉 REST 基线（#247）：mock 掉 API，避免真实 fetch
vi.mock('@/api/td', () => ({
  tdApi: { getStats: vi.fn(), getOrders: vi.fn(), getTrades: vi.fn() },
}))

/** 面板在当前 tab 下是否显示（v-show 以内联 display:none 隐藏） */
function panelVisible(wrapper: ReturnType<typeof mount>, stubSelector: string): boolean {
  const panel = wrapper.find(stubSelector).element.closest('.trading-data-view__panel')
  expect(panel, `找不到 ${stubSelector} 所在面板`).toBeTruthy()
  return panel!.getAttribute('style')?.includes('display: none') !== true
}

/** 某个选择器在页壳直接子节点中的位置（用于断言统计条在 tab 栏上方） */
function childIndex(wrapper: ReturnType<typeof mount>, selector: string): number {
  const root = wrapper.element as HTMLElement
  const target = root.querySelector(selector)
  expect(target, `找不到 ${selector}`).toBeTruthy()
  return [...root.children].indexOf(target as Element)
}

describe('TradingDataView（#246/#247/#248 交易数据页壳）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    __resetTdStatsSchedulerForTests()
    __resetTdBatchSchedulerForTests()
    vi.useFakeTimers()
    vi.resetAllMocks()
    vi.mocked(tdApi.getStats).mockResolvedValue({
      available: true as const,
      trading_day: '20260808',
      online_accounts: ['acc_a'],
      stats: { total_orders: 1240, total_volume: 2480, unfilled_orders: 12, unfilled_volume: 21 },
    })
  })
  afterEach(() => {
    vi.useRealTimers()
    __resetTdStatsSchedulerForTests()
    __resetTdBatchSchedulerForTests()
  })

  it('渲染页头「交易数据」与委托/成交两个 tab', () => {
    const wrapper = mount(TradingDataView, { global: { stubs } })

    expect(wrapper.find('h1').text()).toBe('交易数据')
    expect(wrapper.find('[data-test="td-tab-orders"]').text()).toBe('委托')
    expect(wrapper.find('[data-test="td-tab-trades"]').text()).toBe('成交')
    wrapper.unmount()
  })

  it('统计条存在且位于 tab 栏上方（spec：UI 在交易数据页显示统计条）', async () => {
    const wrapper = mount(TradingDataView, { global: { stubs } })
    await flushPromises()

    expect(wrapper.find('[data-test="td-stats-bar"]').exists()).toBe(true)
    expect(childIndex(wrapper, '[data-test="td-stats-bar"]'))
      .toBeLessThan(childIndex(wrapper, '.ds-tabs'))
    // 统计条是页级口径：切 tab 不重建（它在 tab 栏之外）
    expect(wrapper.findAll('[data-test="td-stats-values"]')).toHaveLength(1)
    wrapper.unmount()
  })

  it('默认选中「委托」tab，成交面板隐藏', () => {
    const wrapper = mount(TradingDataView, { global: { stubs } })

    expect(wrapper.find('[data-test="td-tab-orders"]').classes()).toContain('is-active')
    expect(wrapper.find('[data-test="td-tab-trades"]').classes()).not.toContain('is-active')
    expect(panelVisible(wrapper, '.orders-tab-stub')).toBe(true)
    expect(panelVisible(wrapper, '.trades-tab-stub')).toBe(false)
    wrapper.unmount()
  })

  it('点击「成交」切到成交 tab（委托面板保留在 DOM 但隐藏）', async () => {
    const wrapper = mount(TradingDataView, { global: { stubs } })

    await wrapper.find('[data-test="td-tab-trades"]').trigger('click')

    expect(wrapper.find('[data-test="td-tab-trades"]').classes()).toContain('is-active')
    expect(wrapper.find('[data-test="td-tab-orders"]').classes()).not.toContain('is-active')
    expect(panelVisible(wrapper, '.trades-tab-stub')).toBe(true)
    // v-show：切换不销毁委托面板（保留滚动位置与 store 窗口，不重拉基线）
    expect(wrapper.find('.orders-tab-stub').exists()).toBe(true)
    expect(panelVisible(wrapper, '.orders-tab-stub')).toBe(false)

    await wrapper.find('[data-test="td-tab-orders"]').trigger('click')
    expect(panelVisible(wrapper, '.orders-tab-stub')).toBe(true)
    wrapper.unmount()
  })

  it('成交面板挂的是真实成交列表组件，不再是「后续版本提供」占位', async () => {
    // 不 stub TradesTab：断言真实组件挂载（占位实现会有 td-trades-placeholder 标记与
    // 「成交列表将在后续版本提供」文案；真实列表在无数据时给出「当日暂无成交」空态）。
    vi.mocked(tdApi.getTrades).mockResolvedValue({
      available: true as const,
      trading_day: '20260808',
      online_accounts: ['acc_a'],
      items: [],
      next_cursor: null,
    })
    const wrapper = mount(TradingDataView, { global: { stubs: { OrdersTab: stubs.OrdersTab } } })

    await wrapper.find('[data-test="td-tab-trades"]').trigger('click')
    await flushPromises()

    expect(wrapper.find('[data-test="td-trades-placeholder"]').exists()).toBe(false)
    expect(wrapper.text()).not.toContain('成交列表将在后续版本提供')
    expect(wrapper.find('[data-test="td-trades-empty"]').exists()).toBe(true)
    // 真实成交列表走成交端点，不误用委托端点
    expect(tdApi.getTrades).toHaveBeenCalledTimes(1)
    expect(tdApi.getOrders).not.toHaveBeenCalled()
    wrapper.unmount()
  })
})
