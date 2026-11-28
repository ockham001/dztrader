// 交易接口自动登录/登出排程面板（#524；契约 rest §2.10 / 契约 auto-login）。
//
// 固定这些事实：
// - 排程是**进程级**：面板放在接口卡片级，不按账户行渲染；
// - 非乐观提交：HTTP 成功后仍 pending，直到 `RTN_AUTO_LOGIN`（WS auto_login）到达；
// - `source=unavailable`（网关未运行/镜像未到）时如实说「当前值未知」，不假装空排程；
// - 非 admin 只读：开关与增删按钮禁用并给原因；
// - 提交前挡「登录时间 == 登出时间」（契约：会话区间必须非空），后端仍会权威校验。
import { describe, it, expect, vi, beforeEach } from 'vitest'
import { mount, flushPromises } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import TdAutoLoginPanel from '../TdAutoLoginPanel.vue'
import { useTdAccountsStore } from '@/stores/tdAccounts'
import { useToastStore } from '@/stores/toast'
import { tdGatewaysApi } from '@/api/tdGateways'

vi.mock('@/api/td', () => ({ tdApi: {} }))

vi.mock('@/api/tdGateways', async (importOriginal) => {
  const actual = await importOriginal<typeof import('@/api/tdGateways')>()
  return {
    ...actual,
    tdGatewaysApi: {
      getAutoLogin: vi.fn(),
      setAutoLogin: vi.fn(),
    },
  }
})

const stubs = {
  Icon: { props: ['name', 'size'], template: '<span class="icon" />' },
  Modal: {
    props: ['open', 'title'],
    template: '<div v-if="open" class="modal-stub"><slot /></div>',
  },
  // TimePicker 用原生 input 模拟：暴露 v-model
  TimePicker: {
    props: ['modelValue', 'id'],
    emits: ['update:modelValue'],
    template: '<input class="time-picker-stub" :value="modelValue" @input="$emit(\'update:modelValue\', $event.target.value)" />',
  },
}

const GATEWAY = 'dztd_ctp'

function mountPanel(canWrite = true) {
  return mount(TdAutoLoginPanel, {
    props: { gateway: GATEWAY, canWrite },
    global: { stubs },
  })
}

function seedAutoLogin(overrides: Partial<{ enabled: boolean; schedules: unknown[]; source: string }> = {}) {
  const store = useTdAccountsStore()
  store.autoLogins[GATEWAY] = {
    enabled: true,
    schedules: [
      { login_time: '08:45', logout_time: '15:45' },
      { login_time: '20:45', logout_time: '02:33' },
    ],
    source: 'mirror',
    ...overrides,
  } as never
}

describe('TdAutoLoginPanel 交易接口排程', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    vi.resetAllMocks()
  })

  it('显示已启用状态与时段列表', () => {
    seedAutoLogin()
    const w = mountPanel()
    expect(w.get('[data-test="td-auto-login-toggle"]').text()).toBe('已启用')
    const items = w.findAll('.td-auto-login__item')
    expect(items).toHaveLength(2)
    expect(items[0].text()).toContain('08:45')
    expect(items[0].text()).toContain('15:45')
  })

  it('未启用时开关显示「未启用」', () => {
    seedAutoLogin({ enabled: false })
    const w = mountPanel()
    expect(w.get('[data-test="td-auto-login-toggle"]').text()).toBe('未启用')
  })

  it('跨午夜时段标注「跨午夜」', () => {
    seedAutoLogin({
      schedules: [{ login_time: '20:45', logout_time: '02:33' }],
    })
    const w = mountPanel()
    expect(w.get('.td-auto-login__item').text()).toContain('跨午夜')
  })

  it('source=unavailable 时如实提示「当前值未知」，不渲染可编辑列表', () => {
    seedAutoLogin({ source: 'unavailable', enabled: false, schedules: [] })
    const w = mountPanel()
    expect(w.find('[data-test="td-auto-login-unavailable"]').exists()).toBe(true)
    expect(w.findAll('.td-auto-login__item')).toHaveLength(0)
    expect(w.find('[data-test="td-auto-login-add"]').exists()).toBe(false)
  })

  it('镜像尚未到达（无条目）也走「当前值未知」', () => {
    const w = mountPanel()
    expect(w.find('[data-test="td-auto-login-unavailable"]').exists()).toBe(true)
  })

  it('切开关走全量提交（enabled 取反 + 现有 schedules）', async () => {
    seedAutoLogin()
    vi.mocked(tdGatewaysApi.setAutoLogin).mockResolvedValue({ ok: true })
    const w = mountPanel()
    await w.get('[data-test="td-auto-login-toggle"]').trigger('click')
    expect(tdGatewaysApi.setAutoLogin).toHaveBeenCalledWith(GATEWAY, {
      enabled: false,
      schedules: [
        { login_time: '08:45', logout_time: '15:45' },
        { login_time: '20:45', logout_time: '02:33' },
      ],
    })
  })

  it('非乐观：HTTP 成功后 pending 保持，直到 RTN_AUTO_LOGIN 到达才清', async () => {
    seedAutoLogin()
    vi.mocked(tdGatewaysApi.setAutoLogin).mockResolvedValue({ ok: true })
    const store = useTdAccountsStore()
    const w = mountPanel()
    await w.get('[data-test="td-auto-login-toggle"]').trigger('click')
    await flushPromises()
    // 已提交，但镜像未变、pending 仍在
    expect(store.autoLoginPending.has(GATEWAY)).toBe(true)

    // RTN 到达（WS auto_login）→ 权威覆盖 + 清 pending
    store.applyAutoLogin(GATEWAY, {
      enabled: false,
      schedules: [{ login_time: '08:45', logout_time: '15:45' }],
    })
    expect(store.autoLoginPending.has(GATEWAY)).toBe(false)
    expect(store.autoLogins[GATEWAY].enabled).toBe(false)
  })

  it('删除时段提交剩余列表', async () => {
    seedAutoLogin()
    vi.mocked(tdGatewaysApi.setAutoLogin).mockResolvedValue({ ok: true })
    const w = mountPanel()
    await w.findAll('.td-auto-login__remove')[0].trigger('click')
    expect(tdGatewaysApi.setAutoLogin).toHaveBeenCalledWith(GATEWAY, {
      enabled: true,
      schedules: [{ login_time: '20:45', logout_time: '02:33' }],
    })
  })

  // #530 M4：契约 auto-login 的 `schedules: []` = 回落网关内置默认排程，不是「清空」。
  // 删掉最后一条时段后列表会自己长回来，必须给出提示，否则用户会认为保存失败。
  it('删光最后一个时段：提交空表 + 提示「回落到内置默认排程」', async () => {
    seedAutoLogin({ schedules: [{ login_time: '08:45', logout_time: '15:45' }] })
    vi.mocked(tdGatewaysApi.setAutoLogin).mockResolvedValue({ ok: true })
    const toast = useToastStore()
    const infoSpy = vi.spyOn(toast, 'info')
    const w = mountPanel()

    await w.get('.td-auto-login__remove').trigger('click')
    await flushPromises()

    expect(tdGatewaysApi.setAutoLogin).toHaveBeenCalledWith(GATEWAY, {
      enabled: true,
      schedules: [],
    })
    expect(infoSpy).toHaveBeenCalledTimes(1)
    expect(String(infoSpy.mock.calls[0][0])).toContain('内置默认排程')
  })

  it('mirror 为空表时文案说明「使用内置默认排程」，不写「不会自动登录」', () => {
    seedAutoLogin({ schedules: [] })
    const w = mountPanel()
    const empty = w.get('[data-test="td-auto-login-empty"]').text()
    expect(empty).toContain('内置默认排程')
    expect(empty).not.toContain('不会自动登录')
  })

  it('添加时段：登录时间等于登出时间被前端挡住，不发请求', async () => {
    seedAutoLogin({ schedules: [] })
    const w = mountPanel()
    await w.get('[data-test="td-auto-login-add"]').trigger('click')
    // 默认 08:45 / 15:45，把登出改成同一个时间
    const inputs = w.findAll('.time-picker-stub')
    expect(inputs).toHaveLength(2)
    await inputs[1].setValue('08:45')
    await w.get('[data-test="td-auto-login-submit"]').trigger('click')
    expect(tdGatewaysApi.setAutoLogin).not.toHaveBeenCalled()
    expect(w.find('.td-auto-login__error').text()).toContain('不能等于')
  })

  it('非 admin：开关与增删按钮禁用并给原因', () => {
    seedAutoLogin()
    const w = mountPanel(false)
    expect(w.get('[data-test="td-auto-login-toggle"]').attributes('disabled')).toBeDefined()
    expect(w.get('[data-test="td-auto-login-toggle"]').attributes('title')).toContain('管理员')
    expect(w.get('[data-test="td-auto-login-add"]').attributes('disabled')).toBeDefined()
    expect(w.findAll('.td-auto-login__remove')[0].attributes('disabled')).toBeDefined()
  })

  it('提交失败时弹出错误提示', async () => {
    seedAutoLogin()
    vi.mocked(tdGatewaysApi.setAutoLogin).mockRejectedValue(new Error('boom'))
    const w = mountPanel()
    await w.get('[data-test="td-auto-login-toggle"]').trigger('click')
    await flushPromises()
    const { useToastStore } = await import('@/stores/toast')
    expect(useToastStore().items.some(t => t.level === 'error')).toBe(true)
  })
})

describe('tdAccounts.applyAutoLogin 分流与守卫', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
  })

  it('只接受 dztd_* 网关（md 的排程不得写进 td 镜像）', () => {
    const store = useTdAccountsStore()
    store.applyAutoLogin('dzmd_ctp', { enabled: true, schedules: [] })
    expect(store.autoLogins['dzmd_ctp']).toBeUndefined()
  })

  it('载荷缺 enabled / schedules 非数组时整条丢弃，不半覆盖', () => {
    const store = useTdAccountsStore()
    store.applyAutoLogin(GATEWAY, { enabled: true, schedules: [{ login_time: '08:45', logout_time: '15:45' }] })
    const before = store.autoLogins[GATEWAY]
    store.applyAutoLogin(GATEWAY, { schedules: [] })   // 缺 enabled
    store.applyAutoLogin(GATEWAY, { enabled: false, schedules: 'x' })  // schedules 非数组
    expect(store.autoLogins[GATEWAY]).toBe(before)
  })

  it('RTN 到达清 pending', () => {
    const store = useTdAccountsStore()
    store.autoLoginPending.add(GATEWAY)
    store.applyAutoLogin(GATEWAY, { enabled: false, schedules: [] })
    expect(store.autoLoginPending.has(GATEWAY)).toBe(false)
  })
})
