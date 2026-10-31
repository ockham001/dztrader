import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest'
import { mount, flushPromises, type VueWrapper } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import { useAuthStore } from '@/stores/auth'
import Sidebar from '../Sidebar.vue'

// 侧边栏用户菜单键盘可达（#206 修复项 2）
// 覆盖：菜单项是原生 <button role="menuitem">（落在 Tab 序列、Enter/Space 由浏览器原生激活）、
//       触发按钮 aria-haspopup="menu" / aria-expanded 随开关变化、菜单 role=menu、
//       Esc 关闭并把焦点还给触发按钮、点击外部关闭的既有逻辑不回退。
//
// 关于键盘断言：jsdom **不实现** <button> 的原生键盘激活（Enter/Space 按下时不派发 click），
// 因此这里按真实浏览器的行为顺序模拟——先 keydown，再由「浏览器」派发 click。
// 顺便钉住「模板里不额外绑 keydown.enter」：否则真实浏览器会 keydown→自派发 click 双触发。

const routerPush = vi.hoisted(() => vi.fn())

vi.mock('vue-router', () => ({
  useRouter: () => ({ push: routerPush }),
  RouterLink: { template: '<a><slot /></a>' },
}))

const stubs = {
  Icon: { props: ['name', 'size'], template: '<span class="icon" :data-icon="name" />' },
  Modal: {
    props: ['open', 'title'],
    template: '<div v-if="open" class="modal-stub"><slot /><slot name="footer" /></div>',
  },
  // 只保留 open，用 data-test 断言「修改密码」确实开了弹窗
  ChangePasswordModal: {
    props: ['open'],
    template: '<div v-if="open" data-test="change-pwd-stub" />',
  },
  RouterLink: { template: '<a><slot /></a>' },
}

/** 侧边栏必须挂到 document.body：Esc 后焦点还原的断言依赖真实 document.activeElement */
async function mountSidebar(): Promise<VueWrapper> {
  const auth = useAuthStore()
  auth.user = {
    id: 1, username: 'admin', display_name: 'Admin', email: 'a@x.com',
    role: 'admin', status: 'online', created_at: '2026-08-01',
  }
  const wrapper = mount(Sidebar, {
    props: { collapsed: false, mobileOpen: false, isMobile: false },
    global: { stubs },
    attachTo: document.body,
  })
  await flushPromises()
  return wrapper
}

function triggerButton(wrapper: VueWrapper) {
  const btn = wrapper.find('.user-section-btn')
  expect(btn.exists(), '缺少用户菜单触发按钮').toBe(true)
  return btn
}

async function openMenu(wrapper: VueWrapper) {
  await triggerButton(wrapper).trigger('click')
  await flushPromises()
}

/** 按文案取菜单项（避免依赖顺序） */
function menuItem(wrapper: VueWrapper, label: string) {
  const item = wrapper.findAll('[role="menuitem"]').find(el => el.text().includes(label))
  expect(item, `缺少菜单项「${label}」`).toBeTruthy()
  return item!
}

/**
 * 原生 button 的键盘激活：真实浏览器先派发 keydown，再由 UA 派发 click。
 * jsdom 缺后半段，这里手动补上，以断言「键盘路径能触发、且只触发一次」。
 */
async function keyboardActivate(el: ReturnType<typeof menuItem>) {
  await el.trigger('keydown', { key: 'Enter' })
  await el.trigger('click')
  await flushPromises()
}

let wrapper: VueWrapper | null = null

afterEach(() => {
  wrapper?.unmount()
  wrapper = null
  routerPush.mockClear()
  sessionStorage.clear()
})

describe('Sidebar 用户菜单键盘可达（#206）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
  })

  it('菜单项是原生 <button type="button">，role=menuitem，落在 Tab 序列里', async () => {
    wrapper = await mountSidebar()
    await openMenu(wrapper)

    const items = wrapper.findAll('[role="menuitem"]')
    expect(items).toHaveLength(2)
    for (const item of items) {
      // 原生 button 是「Tab 可达 + Enter/Space 触发」的前提：div + @click 键盘永远到不了
      expect(item.element.tagName).toBe('BUTTON')
      expect(item.attributes('type')).toBe('button')
      // 未被排除出 Tab 序列（tabindex=-1 会让菜单项键盘不可达）
      expect((item.element as HTMLButtonElement).tabIndex).toBe(0)
    }
    // 文案与既有语义保持不变
    expect(items[0].text()).toBe('修改密码')
    expect(items[1].text()).toBe('登出')
  })

  it('「修改密码」项经键盘（keydown Enter + UA click）打开改密弹窗并收起菜单', async () => {
    wrapper = await mountSidebar()
    await openMenu(wrapper)
    expect(wrapper.find('[data-test="change-pwd-stub"]').exists()).toBe(false)

    // 只按 keydown 不应有动作：绑了 @keydown.enter 才会在真实浏览器里双触发
    await menuItem(wrapper, '修改密码').trigger('keydown', { key: 'Enter' })
    await flushPromises()
    expect(wrapper.find('[data-test="change-pwd-stub"]').exists()).toBe(false)

    await keyboardActivate(menuItem(wrapper, '修改密码'))

    expect(wrapper.find('[data-test="change-pwd-stub"]').exists()).toBe(true)
    expect(wrapper.find('[role="menuitem"]').exists()).toBe(false)
  })

  it('「登出」项经键盘触发后清会话并跳登录页，且只触发一次', async () => {
    wrapper = await mountSidebar()
    const auth = useAuthStore()
    const logoutSpy = vi.spyOn(auth, 'logout')
    await openMenu(wrapper)

    await keyboardActivate(menuItem(wrapper, '登出'))

    expect(logoutSpy).toHaveBeenCalledTimes(1)
    expect(routerPush).toHaveBeenCalledTimes(1)
    expect(routerPush).toHaveBeenCalledWith('/login')
    // 登出前记下用户名供登录页预填（既有行为，不能被按钮化改坏）
    expect(sessionStorage.getItem('last_username')).toBe('admin')
  })

  it('触发按钮有 aria-haspopup="menu"，aria-expanded 随菜单开关变化', async () => {
    wrapper = await mountSidebar()
    const btn = triggerButton(wrapper)
    expect(btn.attributes('aria-haspopup')).toBe('menu')
    expect(btn.attributes('aria-expanded')).toBe('false')

    await btn.trigger('click')
    await flushPromises()
    expect(triggerButton(wrapper).attributes('aria-expanded')).toBe('true')

    await triggerButton(wrapper).trigger('click')
    await flushPromises()
    expect(triggerButton(wrapper).attributes('aria-expanded')).toBe('false')
  })

  it('菜单容器 role=menu，Escape 关闭并把焦点还给触发按钮', async () => {
    wrapper = await mountSidebar()
    await openMenu(wrapper)
    expect(wrapper.find('[role="menu"]').exists()).toBe(true)

    // 焦点落在菜单项上（Tab 进入菜单后的典型位置）
    const first = menuItem(wrapper, '修改密码')
    ;(first.element as HTMLElement).focus()
    expect(document.activeElement).toBe(first.element)

    const esc = new KeyboardEvent('keydown', { key: 'Escape', bubbles: true, cancelable: true })
    document.dispatchEvent(esc)
    await flushPromises()

    expect(wrapper.find('[role="menu"]').exists()).toBe(false)
    expect(wrapper.find('[role="menuitem"]').exists()).toBe(false)
    expect(document.activeElement).toBe(triggerButton(wrapper).element)
  })

  it('点击外部仍关闭菜单（既有逻辑不回退）', async () => {
    wrapper = await mountSidebar()
    await openMenu(wrapper)
    expect(wrapper.find('[role="menu"]').exists()).toBe(true)

    document.body.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    await flushPromises()

    expect(wrapper.find('[role="menu"]').exists()).toBe(false)
    expect(triggerButton(wrapper).attributes('aria-expanded')).toBe('false')
  })

  // 焦点不在这两者上时，Esc 不应被本菜单吃掉：
  // 否则对话框自己的 Esc 会连带关掉下拉菜单并无故抢走焦点。
  it('焦点不在菜单内时 Escape 不关闭菜单（不与对话框的 Esc 抢）', async () => {
    wrapper = await mountSidebar()
    await openMenu(wrapper)
    expect(wrapper.find('[role="menu"]').exists()).toBe(true)

    const outside = document.createElement('button')
    document.body.appendChild(outside)
    outside.focus()
    expect(document.activeElement).toBe(outside)

    document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true, cancelable: true }))
    await flushPromises()

    expect(wrapper.find('[role="menu"]').exists()).toBe(true)
    expect(document.activeElement).toBe(outside)
    outside.remove()
  })

  // 回归点：先开改密弹窗再关它，焦点不能掉到 body。
  // 若 openChangePassword 没有先把焦点移回触发按钮，焦点陷阱记录到的还原目标
  // 是即将被 v-if 移除的菜单项，isConnected 保护会跳过还原 → document.activeElement 变 body。
  it('从菜单打开改密弹窗后，焦点还原目标是触发按钮而不是 body', async () => {
    wrapper = await mountSidebar()
    await openMenu(wrapper)

    const item = menuItem(wrapper, '修改密码')
    ;(item.element as HTMLElement).focus()
    // 原生 button 的键盘激活：keydown → UA click
    await item.trigger('click')
    await flushPromises()

    // 弹窗打开时焦点应已移入弹窗内容（此处 stub 无焦点元素，故只断言不是 body）
    expect(document.activeElement).not.toBe(document.body)
    expect(wrapper.find('[data-test="change-pwd-stub"]').exists()).toBe(true)
  })
})
