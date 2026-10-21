import { describe, it, expect, vi, beforeEach } from 'vitest'
import { mount } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import { useMarketSourcesStore } from '@/stores/marketSources'
import { useProgressStore } from '@/stores/progress'
import { marketSourcesApi } from '@/api/marketSources'
import DashboardView from '../DashboardView.vue'

// Dashboard 引用真实 StatusIndicator；断言页面展示的聚合数字与行情卡片，
// 而不是测试自己刚写进 store 的值。

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
})
