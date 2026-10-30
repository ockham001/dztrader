import { describe, it, expect, beforeEach, vi } from 'vitest'
import { mount, flushPromises } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import { useSystemStore } from '@/stores/system'
import { useAuthStore } from '@/stores/auth'
import Sidebar from '../Sidebar.vue'

// 侧边栏（#207 A3/A4）：关于页版本号只来自后端 /api/system/info，前端不得硬编码。
// vue-router 依赖由 mock 提供（Sidebar 只用 useRouter().push 与 router-link 渲染导航）。
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

/** 打开「关于」弹窗（按钮文案为「关于」） */
async function openAbout(wrapper: Awaited<ReturnType<typeof mountSidebar>>) {
  const about = wrapper.findAll('button').find(b => b.text().trim() === '关于')
  expect(about, '缺少「关于」按钮').toBeTruthy()
  await about!.trigger('click')
  await flushPromises()
  return wrapper.find('[data-test="about-version"]')
}

describe('Sidebar 关于页版本号（#207 A3）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
  })

  it('显示后端自述的工程版本，而不是硬编码值', async () => {
    useSystemStore().version = '0.1.0'
    const el = await openAbout(await mountSidebar())
    expect(el.exists()).toBe(true)
    expect(el.text()).toBe('Version 0.1.0')
    // 旧实现硬编码 0.0.1（与工程版本漂移）；该值不得再出现
    expect(el.text()).not.toContain('0.0.1')
  })

  it('版本随 store 变化（后端返回什么就显示什么），不缓存第二份', async () => {
    useSystemStore().version = '1.2.3-rc1'
    const el = await openAbout(await mountSidebar())
    expect(el.text()).toBe('Version 1.2.3-rc1')
  })

  it('后端信息尚未到达时显示「未知」，不编造版本号', async () => {
    useSystemStore().version = ''
    const el = await openAbout(await mountSidebar())
    expect(el.text()).toBe('Version 未知')
  })
})

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
