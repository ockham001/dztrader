import { describe, it, expect, vi, beforeEach } from 'vitest'
import { mount } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import { useMarketSourcesStore } from '@/stores/marketSources'
import { useProgressStore } from '@/stores/progress'
import { marketSourcesApi } from '@/api/marketSources'
import DashboardView from '../DashboardView.vue'

// Dashboard 引用真实 StatusIndicator；断言页面展示的聚合数字与行情卡片，
// 而不是测试自己刚写进 store 的值。
//
// issue #200：账户 / 持仓 / 委托 / 策略 / 消息五个面板仍是演示数据，
// 必须永久可见地标注演示，且聚合数与展示行自洽（禁止硬编码矛盾数字）。

const ctpSource = {
  id: 1,
  source_type: 'CTP',
  source_name: 'dzmd_ctp',
  display_name: 'CTP主行情',
  ui_card: 'ctp',
  is_added: true,
  auto_login: false,
  created_at: '2026-08-01T00:00:00Z',
  updated_at: '2026-08-01T00:00:00Z',
}

describe('DashboardView', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
  })

  it('无行情源时展示在线/总数 0/0', () => {
    const w = mount(DashboardView, { attachTo: document.body })
    expect(w.text()).toContain('0/0')
    w.unmount()
  })

  it('行情源在线时在线数/总数随聚合视图更新', async () => {
    const store = useMarketSourcesStore()
    const progress = useProgressStore()
    vi.spyOn(marketSourcesApi, 'list').mockResolvedValue([ctpSource])
    await store.loadSources()
    // 契约 progress: min=0,max=1,current=1 → 登录完成
    progress.progress.dzmd_ctp = { min: 0, max: 1, current: 1 }

    const w = mount(DashboardView, { attachTo: document.body })
    await w.vm.$nextTick()

    expect(w.text()).toContain('1/1')
    expect(w.text()).toContain('CTP主行情')
    expect(w.text()).toContain('已登录')

    w.unmount()
  })

  // ---- issue #200：演示数据标识与自洽 ----

  it('首屏即标注演示数据并点名未接真实链路的面板', () => {
    const w = mount(DashboardView, { attachTo: document.body })
    const banner = w.find('[data-test="dashboard-demo-banner"]')
    expect(banner.exists()).toBe(true)
    const text = banner.text()
    expect(text).toContain('演示数据')
    // 横幅正文要点名哪些面板尚未接入真实链路，读者不必猜测
    for (const panel of ['账户', '持仓', '委托', '策略', '消息']) {
      expect(text).toContain(panel)
    }
    w.unmount()
  })

  it('五个演示面板逐面板标注演示，行情面板（真实数据）不标注', () => {
    const w = mount(DashboardView, { attachTo: document.body })
    const tags = w.findAll('[data-test="dashboard-demo-tag"]')
    expect(tags).toHaveLength(5)
    expect(tags.every(tag => tag.text() === '演示')).toBe(true)
    w.unmount()
  })

  it('不出现真实券商名与真实合约代码', () => {
    const w = mount(DashboardView, { attachTo: document.body })
    const text = w.text()
    const realWorldValues = [
      '华鑫期货', '山金期货',
      'rb2510', 'hc2510', 'i2509', 'j2509', 'JM2510', 'rb2511', 'hc2511', 'i2510',
      'i2609', 'hc2501', 'j2601',
    ]
    for (const real of realWorldValues) {
      expect(text).not.toContain(real)
    }
    w.unmount()
  })

  it('聚合数与展示行自洽', () => {
    const w = mount(DashboardView, { attachTo: document.body })
    // 账户：3 个账户中仅 1 个已登录（旧值 2/3 与展示行矛盾）
    expect(w.get('[data-test="account-aggregate"]').text()).toBe('1/3')
    // 持仓：8 行；「隐藏匹配行」筛选尚未实现，命中数=总数
    expect(w.get('[data-test="position-aggregate"]').text()).toBe('8/8')
    // 委托：7 笔中 2 笔未成交（旧值 2/12 与 7 行矛盾）
    expect(w.get('[data-test="orders-aggregate"]').text()).toBe('2/7 笔')
    // 策略：5 个策略中 4 个运行中（旧值 5/5 与「1 已停止」矛盾）
    expect(w.get('[data-test="strategy-aggregate"]').text()).toBe('4/5')
    expect(w.get('[data-test="strategy-footer"]').text()).toBe('总计: 5 已加载, 1 已停止')
    w.unmount()
  })
})
