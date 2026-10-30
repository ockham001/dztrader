import { describe, it, expect, beforeEach, vi } from 'vitest'
import { mount, flushPromises } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import { useAuthStore } from '@/stores/auth'
import Sidebar from '../Sidebar.vue'

// 侧边栏导航「开发中」标识（#207 C4）：占位页功能在点进去之前就能看出来。
// 与 A3/A4 分开成文件以保持每项「独立可回滚」。

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

describe('Sidebar 导航「开发中」标识（#207 C4）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
  })

  it('五个占位页导航项带「开发中」标签，已实现页面不带', async () => {
    const wrapper = await mountSidebar()

    // 每个导航项一个 .nav-link；按文案定位，避免依赖顺序
    function navByLabel(label: string) {
      const link = wrapper.findAll('.nav-link').find(l => l.text().includes(label))
      expect(link, `缺少导航项 ${label}`).toBeTruthy()
      return link!
    }

    for (const label of ['交易账户', '策略管理', '数据管理', '结算', '风控']) {
      const tag = navByLabel(label).find('[data-test="nav-wip-tag"]')
      expect(tag.exists(), `${label} 缺「开发中」标签`).toBe(true)
      expect(tag.text()).toBe('开发中')
    }
    for (const label of ['仪表盘', '行情源', '日志', '资源监控', '访问控制', '系统设置']) {
      expect(navByLabel(label).find('[data-test="nav-wip-tag"]').exists(), `${label} 不应有标签`).toBe(false)
    }
    // 仍可点入：标签只是标识，不改变 link 目标（路由未安装时 router-link 保留 to 属性）
    expect(navByLabel('交易账户').attributes('to')).toBe('/trading')
  })
})
