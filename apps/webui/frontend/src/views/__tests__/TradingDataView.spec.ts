import { describe, it, expect, beforeEach } from 'vitest'
import { mount } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import TradingDataView from '../TradingDataView.vue'

// 子 tab 各自有独立测试（OrdersTab.spec.ts 覆盖委托列表；成交占位由本文件覆盖），
// 这里只验证页壳与 tab 切换。断言用面板的内联 display 而非 VTU 的 isVisible：
// jsdom 无布局，isVisible 依赖 offsetWidth 回退，跨用例不稳定。
const stubs = {
  OrdersTab: { template: '<div class="orders-tab-stub">委托面板</div>' },
  TradesTab: { template: '<div class="trades-tab-stub">成交面板</div>' },
}

/** 面板在当前 tab 下是否显示（v-show 以内联 display:none 隐藏） */
function panelVisible(wrapper: ReturnType<typeof mount>, stubSelector: string): boolean {
  const panel = wrapper.find(stubSelector).element.closest('.trading-data-view__panel')
  expect(panel, `找不到 ${stubSelector} 所在面板`).toBeTruthy()
  return panel!.getAttribute('style')?.includes('display: none') !== true
}

describe('TradingDataView（#246 交易数据页壳）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
  })

  it('渲染页头「交易数据」与委托/成交两个 tab', () => {
    const wrapper = mount(TradingDataView, { global: { stubs } })

    expect(wrapper.find('h1').text()).toBe('交易数据')
    expect(wrapper.find('[data-test="td-tab-orders"]').text()).toBe('委托')
    expect(wrapper.find('[data-test="td-tab-trades"]').text()).toBe('成交')
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
})
