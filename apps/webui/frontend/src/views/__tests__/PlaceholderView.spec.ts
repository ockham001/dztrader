import { describe, it, expect, vi, beforeEach } from 'vitest'
import { mount } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import PlaceholderView from '../PlaceholderView.vue'

// mock useRoute：返回带 meta 的路由对象
const routeMetaMock = vi.fn()
vi.mock('vue-router', () => ({
  useRoute: () => routeMetaMock(),
}))

vi.mock('@/components/shared/Icon.vue', () => ({
  default: { name: 'Icon', props: { name: String, size: [Number, String] }, template: '<span class="icon" />' },
}))

describe('PlaceholderView', () => {
  beforeEach(() => { setActivePinia(createPinia()) })

  it('渲染路由 meta 的标题与描述', () => {
    routeMetaMock.mockReturnValue({ meta: { title: '交易账户', description: '交易功能开发中' } })
    const w = mount(PlaceholderView)
    expect(w.text()).toContain('交易账户')
    expect(w.text()).toContain('交易功能开发中')
    expect(w.text()).toContain('功能开发中')
  })

  it('路由 meta 缺失时回退默认文案', () => {
    routeMetaMock.mockReturnValue({ meta: {} })
    const w = mount(PlaceholderView)
    expect(w.text()).toContain('占位页面')
    expect(w.text()).toContain('该功能正在开发中，敬请期待。')
  })
})