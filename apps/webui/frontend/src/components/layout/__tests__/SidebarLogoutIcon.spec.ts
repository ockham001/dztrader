import { describe, it, expect, beforeEach, vi } from 'vitest'
import { mount, flushPromises } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import { useAuthStore } from '@/stores/auth'
import Sidebar from '../Sidebar.vue'

// 侧边栏登出图标语义（#207 A4）：用 logout 图标，不再是下载箭头 download2。
// 与 A3/C4 分开成文件以保持每项「独立可回滚」。

vi.mock('vue-router', () => ({
  useRouter: () => ({ push: vi.fn() }),
  RouterLink: { template: '<a><slot /></a>' },
}))

const stubs = {
  Icon: { props: ['name', 'size'], template: '<span class="icon" :data-icon="name" />' },
  Modal: {
    props: ['open', 'title'],
    template: '<div v-if="open" class="modal-stub"><slot /><slot name="footer" /></div>',
  },
  RouterLink: { template: '<a><slot /></a>' },
}

async function mountSidebar() {
  const auth = useAuthStore()
  auth.user = {
    id: 1, username: 'admin', display_name: 'Admin', email: 'a@x.com',
    role: 'admin', status: 'online', created_at: '2026-08-01',
  }
  const wrapper = mount(Sidebar, {
    props: { collapsed: false, mobileOpen: false, isMobile: false },
    global: { stubs },
  })
  await flushPromises()
  return wrapper
}

describe('Sidebar 登出图标语义（#207 A4）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
  })

  it('用户菜单的登出项使用 logout 图标，不再是下载箭头 download2', async () => {
    const wrapper = await mountSidebar()
    // 打开用户下拉菜单（按钮含显示名）
    const userBtn = wrapper.findAll('button').find(b => b.text().includes('Admin'))
    expect(userBtn, '缺少用户菜单按钮').toBeTruthy()
    await userBtn!.trigger('click')
    await flushPromises()

    const icons = wrapper.findAll('[data-icon]').map(i => i.attributes('data-icon'))
    expect(icons).toContain('logout')
    expect(icons).not.toContain('download2')
  })
})
