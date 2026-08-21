import { describe, it, expect, vi, beforeEach } from 'vitest'
import { mount, flushPromises } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import { useMarketSourcesStore } from '@/stores/marketSources'
import { useToastStore } from '@/stores/toast'
import MarketSourcesView from '../MarketSourcesView.vue'

// mock 卡片注册表：避免加载真实 CtpCard/GenericCard（重依赖），返回简单渲染
vi.mock('@/stores/marketSourcesCardRegistry', () => ({
  resolveCard: () => ({
    name: 'CardStub',
    template: '<article class="source-card-stub">行情卡片</article>',
  }),
}))

// stub 子组件
const sharedStubs = {
  Icon: { template: '<span class="icon" />' },
  Modal: { props: ['open', 'title'], emits: ['close'], template: '<div class="modal-stub"><slot /></div><div class="modal-footer-stub"><slot name="footer" /></div>' },
  SyncSelect: { props: ['modelValue'], template: '<button type="button" class="sync-select">{{ String(modelValue) }}</button>' },
}

describe('MarketSourcesView', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    const store = useMarketSourcesStore()
    // 阻断真实 API 拉取
    vi.spyOn(store, 'loadSources').mockResolvedValue()
    vi.spyOn(store, 'refreshAvailable').mockResolvedValue()
  })

  it('渲染页面标题（行情源）', async () => {
    const w = mount(MarketSourcesView, { global: { stubs: sharedStubs } })
    await flushPromises()
    expect(w.text()).toContain('行情源')
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