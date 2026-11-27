// 消息中心「查看全部」抽屉 + 顶栏未读徽标（ADR 0060 决策 6）
// 覆盖：全量列表、按级别筛选、×N 折叠计数、全部已读、空态、打开即已读。
import { describe, it, expect, beforeEach, vi } from 'vitest'
import { mount } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import { nextTick } from 'vue'
import NotifyDrawer from '../NotifyDrawer.vue'
import { useNotifyStore } from '@/stores/notify'

vi.mock('@/stores/toast', () => ({
  useToastStore: () => ({
    error: vi.fn(), warning: vi.fn(), info: vi.fn(), persistent: vi.fn(),
    success: vi.fn(), show: vi.fn(), dismiss: vi.fn(), clear: vi.fn(),
  }),
}))

const stubs = {
  Modal: {
    props: ['open', 'title'],
    template: '<div v-if="open" class="modal-stub"><slot /></div>',
  },
  Icon: { props: ['name', 'size'], template: '<span class="icon" />' },
}

function mountDrawer(open = true) {
  return mount(NotifyDrawer, {
    props: { open },
    global: { stubs },
  })
}

describe('NotifyDrawer 消息中心抽屉', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
  })

  it('关闭时不渲染内容', () => {
    const w = mountDrawer(false)
    expect(w.find('.notify-drawer').exists()).toBe(false)
  })

  it('显示全部消息，新的在前', () => {
    const store = useNotifyStore()
    store.push('第一条', 'dztd_ctp', 'info')
    store.push('第二条', 'dztd_ctp', 'warning')
    const w = mountDrawer()
    const rows = w.findAll('.notify-drawer__row')
    expect(rows).toHaveLength(2)
    // 倒序：新的（第二条）在前
    expect(rows[0].text()).toContain('第二条')
    expect(rows[1].text()).toContain('第一条')
  })

  it('按级别筛选只显示该级别', async () => {
    const store = useNotifyStore()
    store.push('信息消息', 'dztd_ctp', 'info')
    store.push('错误消息', 'dztd_ctp', 'error')
    store.push('警告消息', 'dztd_ctp', 'warning')
    const w = mountDrawer()
    expect(w.findAll('.notify-drawer__row')).toHaveLength(3)

    // 点「错误」筛选按钮
    const errorBtn = w.findAll('.notify-drawer__filters button').find(b => b.text().includes('错误'))
    expect(errorBtn).toBeTruthy()
    await errorBtn!.trigger('click')
    await nextTick()
    const rows = w.findAll('.notify-drawer__row')
    expect(rows).toHaveLength(1)
    expect(rows[0].text()).toContain('错误消息')
  })

  it('显示的计数包含各级别条数', () => {
    const store = useNotifyStore()
    store.push('e1', 'a', 'error')
    store.push('e2', 'a', 'error')
    store.push('w1', 'a', 'warning')
    const w = mountDrawer()
    const text = w.find('.notify-drawer__filters').text()
    expect(text).toContain('全部 (3)')
    expect(text).toContain('错误 (2)')
    expect(text).toContain('警告 (1)')
  })

  it('count > 1 时显示 ×N 折叠计数', () => {
    const store = useNotifyStore()
    store.push('平今仓位不足', 'dztd_ctp', 'warning', false, 100, 5)
    const w = mountDrawer()
    expect(w.find('.notify-drawer__row').text()).toContain('×5')
  })

  it('count = 1 时不显示 ×N', () => {
    const store = useNotifyStore()
    store.push('单次事件', 'dztd_ctp', 'warning', false, 100, 1)
    const w = mountDrawer()
    expect(w.find('.notify-drawer__row').text()).not.toContain('×')
  })

  it('空态显示「暂无消息」', () => {
    const w = mountDrawer()
    expect(w.find('[data-test="notify-drawer-empty"]').exists()).toBe(true)
    expect(w.findAll('.notify-drawer__row')).toHaveLength(0)
  })

  it('打开即把消息标为已读', async () => {
    const store = useNotifyStore()
    store.push('未读消息', 'dztd_ctp', 'error')
    expect(store.unreadCount).toBe(1)
    mountDrawer(true)
    await nextTick()
    expect(store.unreadCount).toBe(0)
  })

  it('「全部已读」按钮清未读', async () => {
    const store = useNotifyStore()
    store.push('未读消息', 'dztd_ctp', 'error')
    const w = mountDrawer(true)
    store.push('新来的', 'dztd_ctp', 'error')
    expect(store.unreadCount).toBeGreaterThan(0)
    await w.get('[data-test="notify-drawer-mark-read"]').trigger('click')
    expect(store.unreadCount).toBe(0)
  })
})
