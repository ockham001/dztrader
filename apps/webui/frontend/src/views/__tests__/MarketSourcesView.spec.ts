import { describe, it, expect, vi, beforeEach } from 'vitest'
import { mount, flushPromises } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import { useMarketSourcesStore } from '@/stores/marketSources'
import { useProgressStore } from '@/stores/progress'
import { useToastStore } from '@/stores/toast'
import { marketSourcesApi } from '@/api/marketSources'
import { resolveCard } from '@/stores/marketSourcesCardRegistry'
import MarketSourcesView from '../MarketSourcesView.vue'

// mock 卡片注册表：避免加载真实 CtpCard/GenericCard（重依赖）。
// resolveCard 用 vi.fn 以便校验卡片类型入参（契约 md-config ui_card 机制）；
// stub 渲染 source 的显示名与登录态，用于断言聚合结果确实传给卡片。
vi.mock('@/stores/marketSourcesCardRegistry', () => ({
  resolveCard: vi.fn(() => ({
    name: 'CardStub',
    props: ['source'],
    template: '<article class="source-card-stub">{{ source.display_name }}:{{ source.loginState }}</article>',
  })),
}))

// stub 子组件
const sharedStubs = {
  Icon: { template: '<span class="icon" />' },
  Modal: { props: ['open', 'title'], emits: ['close'], template: '<div class="modal-stub"><slot /></div><div class="modal-footer-stub"><slot name="footer" /></div>' },
  SyncSelect: { props: ['modelValue'], template: '<button type="button" class="sync-select">{{ String(modelValue) }}</button>' },
}

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

describe('MarketSourcesView', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    vi.mocked(resolveCard).mockClear()
    const store = useMarketSourcesStore()
    // 阻断真实 API 拉取；baseSources 未对外暴露，经真实 loadSources 注入 DB 列表
    vi.spyOn(marketSourcesApi, 'list').mockResolvedValue([])
    vi.spyOn(store, 'refreshAvailable').mockResolvedValue()
  })

  it('渲染页面标题（行情源）与空列表占位（共 0 个行情源）', async () => {
    const w = mount(MarketSourcesView, { global: { stubs: sharedStubs } })
    await flushPromises()
    expect(w.text()).toContain('行情源')
    expect(w.text()).toContain('共 0 个行情源')
    expect(w.find('.source-card-stub').exists()).toBe(false)
  })

  it('按 ui_card 解析并渲染行情源卡片', async () => {
    vi.mocked(marketSourcesApi.list).mockResolvedValue([ctpSource])
    useProgressStore().progress.dzmd_ctp = { min: 0, max: 1, current: 1 }
    const w = mount(MarketSourcesView, { global: { stubs: sharedStubs } })
    await flushPromises()

    expect(useMarketSourcesStore().sourceCount).toBe(1)
    expect(w.find('.source-card-stub').text()).toBe('CTP主行情:online')
    expect(resolveCard).toHaveBeenCalledWith('ctp')
  })

  it('store.error 非空时触发 toast 并清空 error', async () => {
    const store = useMarketSourcesStore()
    const toast = useToastStore()
    const toastSpy = vi.spyOn(toast, 'error')
    const w = mount(MarketSourcesView, { global: { stubs: sharedStubs } })
    await flushPromises()
    // 触发 store.error → 组件 watch → toast + clearError
    store.error = '测试错误'
    await w.vm.$nextTick()
    expect(toastSpy).toHaveBeenCalledWith('测试错误')
    expect(store.error).toBeNull()
    w.unmount()
  })
})
