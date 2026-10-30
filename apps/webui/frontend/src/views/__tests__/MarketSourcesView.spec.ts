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

// #207 C3：批量操作必须先确认、列出影响范围；无目标时按钮禁用
describe('MarketSourcesView 批量操作确认（#207 C3）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    vi.mocked(resolveCard).mockClear()
    const store = useMarketSourcesStore()
    vi.spyOn(store, 'refreshAvailable').mockResolvedValue()
  })

  const secondSource = {
    ...ctpSource,
    id: 2,
    source_name: 'dzmd_xtp',
    display_name: 'XTP备行情',
  }

  it('无目标时批量按钮禁用（点击不弹确认框、不发请求）', async () => {
    vi.mocked(marketSourcesApi.list).mockResolvedValue([])
    const store = useMarketSourcesStore()
    const loginSpy = vi.spyOn(store, 'batchLogin')
    const w = mount(MarketSourcesView, { global: { stubs: sharedStubs } })
    await flushPromises()

    const btn = w.find('[data-test="batch-login"]')
    expect(btn.attributes('disabled')).toBeDefined()
    expect(btn.text()).toBe('批量登录')  // 无目标时不显示（0）
    expect(btn.attributes('title')).toContain('没有需要登录的行情源')

    await btn.trigger('click')
    await flushPromises()
    expect(w.find('[data-test="batch-confirm"]').exists()).toBe(false)
    expect(loginSpy).not.toHaveBeenCalled()
  })

  it('点击批量登录先弹确认框列出目标源，确认后才执行（取消则不下发）', async () => {
    vi.mocked(marketSourcesApi.list).mockResolvedValue([ctpSource, secondSource])
    const store = useMarketSourcesStore()
    const loginSpy = vi.spyOn(store, 'batchLogin').mockResolvedValue()
    const w = mount(MarketSourcesView, { global: { stubs: sharedStubs } })
    await flushPromises()

    const btn = w.find('[data-test="batch-login"]')
    // 两个源都离线 → 都在目标集合内，按钮标出数量
    expect(btn.text()).toBe('批量登录（2）')
    expect(btn.attributes('disabled')).toBeUndefined()

    await btn.trigger('click')
    await flushPromises()
    const summary = w.find('[data-test="batch-confirm-summary"]')
    expect(summary.exists()).toBe(true)
    expect(summary.text()).toContain('2')
    expect(summary.text()).toContain('登录')
    const targets = w.find('[data-test="batch-confirm-targets"]').text()
    expect(targets).toContain('CTP主行情')
    expect(targets).toContain('XTP备行情')
    // 弹框阶段不得下发
    expect(loginSpy).not.toHaveBeenCalled()

    // 取消：关闭且不下发
    await w.find('[data-test="batch-confirm-cancel"]').trigger('click')
    await flushPromises()
    expect(w.find('[data-test="batch-confirm"]').exists()).toBe(false)
    expect(loginSpy).not.toHaveBeenCalled()

    // 再点开并确认：只下发确认时算出的目标集合
    await btn.trigger('click')
    await flushPromises()
    await w.find('[data-test="batch-confirm-ok"]').trigger('click')
    await flushPromises()
    expect(loginSpy).toHaveBeenCalledTimes(1)
    expect(loginSpy).toHaveBeenCalledWith([expect.objectContaining({ id: 1 }), expect.objectContaining({ id: 2 })])
  })

  it('批量登出/自动登录同模式：确认框文案对应动作，目标集合来自镜像状态', async () => {
    vi.mocked(marketSourcesApi.list).mockResolvedValue([ctpSource, secondSource])
    const store = useMarketSourcesStore()
    const logoutSpy = vi.spyOn(store, 'batchLogout').mockResolvedValue()
    const w = mount(MarketSourcesView, { global: { stubs: sharedStubs } })
    await flushPromises()

    // 无在线源 → 批量登出禁用
    expect(w.find('[data-test="batch-logout"]').attributes('disabled')).toBeDefined()

    // 自动登录：两者都是关闭态 → 「开启」有 2 个目标，「关闭」无目标
    expect(w.find('[data-test="batch-auto-login-on"]').text()).toBe('批量开启自动登录（2）')
    expect(w.find('[data-test="batch-auto-login-off"]').attributes('disabled')).toBeDefined()

    await w.find('[data-test="batch-auto-login-on"]').trigger('click')
    await flushPromises()
    expect(w.find('[data-test="batch-confirm-summary"]').text()).toContain('开启自动登录')
    await w.find('[data-test="batch-confirm-ok"]').trigger('click')
    await flushPromises()
    expect(logoutSpy).not.toHaveBeenCalled()
  })
})
