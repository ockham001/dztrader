import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest'
import { mount, flushPromises } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import TdInterfaceCard from '../TdInterfaceCard.vue'
import {
  useTdAccountsStore,
  tdAccountsPending,
  tdGatewayPending,
} from '@/stores/tdAccounts'
import { useAuthStore } from '@/stores/auth'
import { useProcessStore } from '@/stores/process'
import { useProgressStore } from '@/stores/progress'
import { useToastStore } from '@/stores/toast'
import { tdApi } from '@/api/td'
import { tdGatewaysApi } from '@/api/tdGateways'
import type { ProcessState } from '@/types/api'

type TdAccountsStoreView = ReturnType<typeof useTdAccountsStore>

vi.mock('@/api/td', () => ({
  tdApi: {
    listAccounts: vi.fn(),
    addAccount: vi.fn(),
    updateAccount: vi.fn(),
    removeAccount: vi.fn(),
    loginAccount: vi.fn(),
    logoutAccount: vi.fn(),
  },
}))

vi.mock('@/api/tdGateways', async (importOriginal) => {
  const actual = await importOriginal<typeof import('@/api/tdGateways')>()
  return {
    ...actual,
    tdGatewaysApi: {
      listAvailable: vi.fn(),
      start: vi.fn(),
      stop: vi.fn(),
      remove: vi.fn(),
    },
  }
})

/**
 * 交易接口卡片（#439 / ADR 0053；契约 rest §2.10/§4）。
 *
 * 固定这些页面级契约事实：
 * - 卡片头 = 显示名 + 进程名 + 进程状态；账户表无「交易网关」列（归属由卡片决定）；
 * - 接口非 Running：显示「启动进程」，账户写按钮禁用并给原因（ADR 0053 不得删除重加）；
 * - 删除接口：有账户时前端禁用 + title「先删除账户」；无账户时二次确认（停止进程 +
 *   移除启动配置 + 不可撤销），确认才发 DELETE；
 * - 非 admin：接口级按钮不渲染，账户写按钮禁用并给原因；
 * - 账户表单不再选网关（gateway 由卡片传入）。
 */
const stubs = {
  Icon: { props: ['name', 'size'], template: '<span class="icon" :data-icon="name" />' },
  // 若账户表单重新引入网关下拉，Dropdown 会以 .dropdown-stub 出现，使「无网关下拉」断言有效
  Dropdown: { template: '<div class="dropdown-stub" />' },
  Modal: {
    props: ['open', 'title'],
    template: '<div v-if="open" class="modal-stub" :data-title="title"><slot /><div class="modal-footer-stub"><slot name="footer" /></div></div>',
  },
}

function loginAsAdmin(): void {
  useAuthStore().user = {
    id: 1, username: 'admin', display_name: 'Admin', email: 'a@x.com',
    role: 'admin', status: 'online', created_at: '2026-08-01',
  }
}

function loginAsUser(): void {
  useAuthStore().user = {
    id: 2, username: 'bob', display_name: 'Bob', email: 'b@x.com',
    role: 'user', status: 'online', created_at: '2026-08-01',
  }
}

interface PrepareOptions {
  running?: boolean
  state?: ProcessState
  accounts?: string[]
  /** 额外注册表条目（用于唯一性冲突/跨网关场景） */
  registryAccounts?: { account_id: string; gateway: string }[]
}

/** 登记一个 dztd_* 进程 + 账户注册表，返回 store（卡片数据由 store.interfaces 派生） */
async function prepare(options: PrepareOptions = {}): Promise<TdAccountsStoreView> {
  const running = options.running ?? true
  const process = useProcessStore()
  process.applyProcessConfig({ dztd_ctp: { exe: 'dztd_ctp.exe', display_name: 'CTP 交易接口' } })
  process.applyProcessStatus({
    name: 'dztd_ctp',
    state: options.state ?? (running ? 'Running' : 'Stopped'),
    pid: running ? 100 : 0,
  })
  vi.mocked(tdApi.listAccounts).mockResolvedValue({
    available: true,
    accounts: [
      ...(options.accounts ?? ['account_001']).map(accountId => ({
        account_id: accountId, gateway: 'dztd_ctp', enabled: true,
      })),
      ...(options.registryAccounts ?? []).map(entry => ({ enabled: true, ...entry })),
    ],
  })
  const store = useTdAccountsStore()
  await store.init()
  return store
}

function mountCard(store: TdAccountsStoreView) {
  const wrapper = mount(TdInterfaceCard, {
    props: { tdInterface: store.interfaces[0] },
    global: { stubs },
  })
  return { wrapper, store }
}

describe('TdInterfaceCard · 渲染（ADR 0053 / 契约 rest §4）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    vi.resetAllMocks()
    tdAccountsPending.__resetForTests()
    tdGatewayPending.__resetForTests()
    loginAsAdmin()
  })

  afterEach(() => {
    useTdAccountsStore().dispose()
  })

  it('头部渲染显示名 / 进程名 / 进程状态；账户表无「交易网关」列', async () => {
    const store = await prepare()
    const { wrapper } = mountCard(store)

    expect(wrapper.find('[data-test="td-interface-name"]').text()).toBe('CTP 交易接口')
    expect(wrapper.find('[data-test="td-interface-process"]').text()).toBe('dztd_ctp')
    expect(wrapper.find('[data-test="td-interface-state"]').text()).toContain('运行中')
    expect(wrapper.find('[data-test="td-interface-account-count"]').text()).toContain('账户 1')
    expect(wrapper.find('[data-test="td-account-row-account_001"]').exists()).toBe(true)

    const headers = wrapper.findAll('th').map(th => th.text())
    expect(headers).toEqual(['账户 ID', '启用', '连接状态', '交易日', '登录时间', '进度', '操作'])
    // 账户归属由卡片决定，表格不再有「交易网关」列
    expect(headers).not.toContain('交易网关')
  })

  it('接口可零账户运行：显示卡片内空态提示，不渲染停止运行的表格', async () => {
    const store = await prepare({ accounts: [] })
    const { wrapper } = mountCard(store)

    expect(wrapper.find('[data-test="td-interface-empty"]').exists()).toBe(true)
    expect(wrapper.find('[data-test="td-interface-empty"]').text()).toContain('还没有账户')
    expect(wrapper.find('table').exists()).toBe(false)
    expect(wrapper.find('[data-test="td-card-add-account"]').attributes('disabled')).toBeUndefined()
    // 零账户 → 删除接口可用（无「先删除账户」guard）
    expect(wrapper.find('[data-test="td-interface-remove"]').attributes('disabled')).toBeUndefined()
  })

  it('细状态/交易日/登录时间/进度来自 td_rtn_status 与 progress；未上报显示占位', async () => {
    const store = await prepare({ accounts: ['account_001', 'account_002'] })
    store.applyTdStatus('dztd_ctp:account_001', {
      account_id: 'account_001',
      state: 'Ready',
      date: '20260808',
      login_time: '2026-08-08 08:45:32',
      progress_desc: 'ready',
      progress_min: 0,
      progress_max: 10,
      progress_current: 10,
      ext: {},
    })
    useProgressStore().applyProgress('dztd_ctp:account_001', { min: 0, max: 10, current: 10, desc: '就绪' })
    const { wrapper } = mountCard(store)

    const ready = wrapper.find('[data-test="td-account-row-account_001"]')
    expect(ready.find('[data-test="td-account-state"]').text()).toBe('就绪')
    expect(ready.text()).toContain('2026-08-08')
    expect(ready.text()).toContain('2026-08-08 08:45:32')
    expect(ready.text()).toContain('10/10')

    const unknown = wrapper.find('[data-test="td-account-row-account_002"]')
    expect(unknown.find('[data-test="td-account-state"]').text()).toBe('—')
  })
})

describe('TdInterfaceCard · 接口非 Running（ADR 0053 / 契约 rest §4）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    vi.resetAllMocks()
    tdAccountsPending.__resetForTests()
    tdGatewayPending.__resetForTests()
    loginAsAdmin()
  })

  afterEach(() => {
    useTdAccountsStore().dispose()
  })

  it('显示进程状态与「启动进程」；账户写按钮全部禁用并给出原因', async () => {
    const store = await prepare({ running: false })
    const { wrapper } = mountCard(store)

    expect(wrapper.find('[data-test="td-interface-state"]').text()).toContain('已停止')
    expect(wrapper.find('[data-test="td-interface-start"]').exists()).toBe(true)

    for (const testId of [
      'td-card-add-account', 'td-edit-account_001', 'td-toggle-account_001', 'td-remove-account_001',
    ]) {
      const button = wrapper.find(`[data-test="${testId}"]`)
      expect(button.exists()).toBe(true)
      expect(button.attributes('disabled')).toBeDefined()
      expect(button.attributes('title')).toContain('未运行')
    }
  })

  it('点击「启动进程」调 start 恢复；pending 等进程状态回报，不得以删除重加代替', async () => {
    const store = await prepare({ running: false })
    vi.mocked(tdGatewaysApi.start).mockResolvedValue({ ok: true })
    const { wrapper } = mountCard(store)

    await wrapper.find('[data-test="td-interface-start"]').trigger('click')
    await flushPromises()

    expect(tdGatewaysApi.start).toHaveBeenCalledWith('dztd_ctp')
    // HTTP 200 只表示已写通道：按钮进入 pending（等 process_status），不是「已恢复」
    await wrapper.setProps({ tdInterface: store.interfaces[0] })
    expect(store.gatewayStartPending('dztd_ctp')).toBe(true)
    expect(wrapper.find('[data-test="td-interface-start"]').attributes('disabled')).toBeDefined()

    // WS 进程状态到 Running → pending 清除、按钮消失
    useProcessStore().applyProcessStatus({ name: 'dztd_ctp', state: 'Running', pid: 100 })
    await flushPromises()
    await wrapper.setProps({ tdInterface: store.interfaces[0] })
    expect(store.gatewayStartPending('dztd_ctp')).toBe(false)
    expect(wrapper.find('[data-test="td-interface-start"]').exists()).toBe(false)
  })

  it('Running 时不渲染「启动进程」按钮', async () => {
    const store = await prepare()
    const { wrapper } = mountCard(store)

    expect(wrapper.find('[data-test="td-interface-start"]').exists()).toBe(false)
  })
})

describe('TdInterfaceCard · 删除交易接口（ADR 0053 决策 3 / 契约 rest §2.10 预检 5）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    vi.resetAllMocks()
    tdAccountsPending.__resetForTests()
    tdGatewayPending.__resetForTests()
    loginAsAdmin()
  })

  afterEach(() => {
    useTdAccountsStore().dispose()
  })

  it('有账户时删除按钮禁用 + title「先删除账户」，点击不发请求', async () => {
    const store = await prepare({ accounts: ['account_001'] })
    const { wrapper } = mountCard(store)

    const button = wrapper.find('[data-test="td-interface-remove"]')
    expect(button.attributes('disabled')).toBeDefined()
    expect(button.attributes('title')).toBe('先删除账户')

    await button.trigger('click')
    await flushPromises()

    expect(wrapper.find('[data-test="td-interface-remove-confirm-text"]').exists()).toBe(false)
    expect(tdGatewaysApi.remove).not.toHaveBeenCalled()
  })

  it('零账户：二次确认写明停止进程 + 从启动配置移除 + 不可撤销；取消不删', async () => {
    const store = await prepare({ accounts: [] })
    vi.mocked(tdGatewaysApi.remove).mockResolvedValue({ ok: true })
    const { wrapper } = mountCard(store)

    await wrapper.find('[data-test="td-interface-remove"]').trigger('click')
    await flushPromises()

    const confirmText = wrapper.find('[data-test="td-interface-remove-confirm-text"]')
    expect(confirmText.exists()).toBe(true)
    expect(confirmText.text()).toContain('CTP 交易接口')
    const dialogText = wrapper.find('.modal-stub').text()
    expect(dialogText).toContain('停止')
    expect(dialogText).toContain('启动配置中移除')
    expect(dialogText).toContain('不可撤销')

    // 取消：不发请求
    await wrapper.find('.modal-footer-stub .ds-btn--secondary').trigger('click')
    await flushPromises()
    expect(tdGatewaysApi.remove).not.toHaveBeenCalled()

    // 重新打开并确认 → 发 DELETE，成功后提示已提交
    await wrapper.find('[data-test="td-interface-remove"]').trigger('click')
    const successSpy = vi.spyOn(useToastStore(), 'success')
    await wrapper.find('[data-test="td-interface-remove-confirm"]').trigger('click')
    await flushPromises()

    expect(tdGatewaysApi.remove).toHaveBeenCalledWith('dztd_ctp')
    expect(successSpy).toHaveBeenCalledWith(expect.stringContaining('已提交移除请求') as unknown as string)
    expect(wrapper.find('[data-test="td-interface-remove-confirm-text"]').exists()).toBe(false)
  })

  it('服务端 409 gateway_has_accounts（前端状态过期）→ 确认框内展示中文原因', async () => {
    const store = await prepare({ accounts: [] })
    vi.mocked(tdGatewaysApi.remove).mockRejectedValue(Object.assign(
      new Error('gateway has accounts'),
      { status: 409, body: { error: 'gateway has accounts', code: 'gateway_has_accounts' } },
    ))
    const { wrapper } = mountCard(store)

    await wrapper.find('[data-test="td-interface-remove"]').trigger('click')
    await wrapper.find('[data-test="td-interface-remove-confirm"]').trigger('click')
    await flushPromises()

    const error = wrapper.find('[data-test="td-interface-remove-error"]')
    expect(error.exists()).toBe(true)
    expect(error.text()).toContain('先删除全部账户')
    expect(error.text()).not.toContain('gateway')
    // 失败已清 pending（usePending 失败路径），用户可修正后重试
    expect(store.gatewayRemovePending('dztd_ctp')).toBe(false)
  })
})

  it('移除请求在途：启动/添加/编辑/启停/删除账户全部禁用并提示原因（堵同页竞态）', async () => {
    const store = await prepare({ running: false, accounts: [] })
    vi.mocked(tdGatewaysApi.remove).mockResolvedValue({ ok: true })
    await store.removeGateway('dztd_ctp')
    expect(store.gatewayRemovePending('dztd_ctp')).toBe(true)

    const { wrapper } = mountCard(store)

    // 启动进程：remove 在途时禁用并提示原因，点击不下发
    const start = wrapper.find('[data-test="td-interface-start"]')
    expect(start.attributes('disabled')).toBeDefined()
    expect(start.attributes('title')).toContain('移除请求已下发')
    await start.trigger('click')
    await flushPromises()
    expect(tdGatewaysApi.start).not.toHaveBeenCalled()

    // 添加账户：禁用并提示原因，点击不打开表单
    const addAccount = wrapper.find('[data-test="td-card-add-account"]')
    expect(addAccount.attributes('disabled')).toBeDefined()
    expect(addAccount.attributes('title')).toContain('移除请求已下发')
    await addAccount.trigger('click')
    await flushPromises()
    expect(wrapper.find('[data-test="td-form-gateway"]').exists()).toBe(false)

    // 删除接口按钮本身也处于 pending 禁用态
    expect(wrapper.find('[data-test="td-interface-remove"]').attributes('disabled')).toBeDefined()
  })

describe('TdInterfaceCard · 账户操作（契约 rest §2.9.7/§2.9.10、ADR 0051）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    vi.resetAllMocks()
    tdAccountsPending.__resetForTests()
    tdGatewayPending.__resetForTests()
    loginAsAdmin()
  })

  afterEach(() => {
    useTdAccountsStore().dispose()
  })

  it('卡片内添加账户：表单无网关下拉，提交带卡片 gateway', async () => {
    const store = await prepare({ accounts: [] })
    vi.mocked(tdApi.addAccount).mockResolvedValue({ ok: true })
    const { wrapper } = mountCard(store)

    await wrapper.find('[data-test="td-card-add-account"]').trigger('click')
    await flushPromises()

    // gateway 由卡片固定传入并只读展示，不再有下拉选择（ADR 0053）
    expect(wrapper.find('[data-test="td-form-gateway"]').text()).toBe('dztd_ctp')
    expect(wrapper.find('.dropdown-stub').exists()).toBe(false)
    expect(wrapper.find('.ds-dropdown').exists()).toBe(false)
    expect(wrapper.find('select').exists()).toBe(false)

    await wrapper.find('input[placeholder="账户唯一标识，全部交易接口间唯一"]').setValue('account_new')
    await wrapper.find('input[placeholder="broker_id"]').setValue('9999')
    await wrapper.find('input[placeholder="user_id"]').setValue('00001')
    await wrapper.find('input[placeholder="登录密码"]').setValue('secret')
    await flushPromises()

    await wrapper.find('[data-test="td-form-submit"]').trigger('click')
    await flushPromises()

    expect(tdApi.addAccount).toHaveBeenCalledWith(expect.objectContaining({
      gateway: 'dztd_ctp',
      account: expect.objectContaining({ account_id: 'account_new', enabled: true }),
    }))
  })

  it('删除账户必须先二次确认：未确认不发 DELETE，确认后才发', async () => {
    const store = await prepare()
    vi.mocked(tdApi.removeAccount).mockResolvedValue({ ok: true })
    const { wrapper } = mountCard(store)

    await wrapper.find('[data-test="td-remove-account_001"]').trigger('click')
    await flushPromises()

    expect(wrapper.find('[data-test="td-remove-confirm-text"]').exists()).toBe(true)
    expect(wrapper.find('.modal-stub').text()).toContain('已存在的交易数据不会被删除')
    expect(tdApi.removeAccount).not.toHaveBeenCalled()

    await wrapper.find('[data-test="td-remove-confirm"]').trigger('click')
    await flushPromises()

    expect(tdApi.removeAccount).toHaveBeenCalledWith('account_001')
  })

  it('会话控制双按钮（ADR 0051）：未登录仅登录可点，就绪仅登出可点；点击下发对应 REST', async () => {
    const store = await prepare()
    useProgressStore().applyProgress('dztd_ctp:account_001', { min: 0, max: 10, current: 0 })
    vi.mocked(tdApi.loginAccount).mockResolvedValue({ ok: true })
    vi.mocked(tdApi.logoutAccount).mockResolvedValue({ ok: true })
    const { wrapper } = mountCard(store)

    const login = wrapper.find('[data-test="td-login-account_001"]')
    const logout = wrapper.find('[data-test="td-logout-account_001"]')
    expect(login.attributes('disabled')).toBeUndefined()
    expect(logout.attributes('disabled')).toBeDefined()

    await login.trigger('click')
    await flushPromises()
    expect(tdApi.loginAccount).toHaveBeenCalledWith('account_001')

    useProgressStore().applyProgress('dztd_ctp:account_001', { min: 0, max: 10, current: 10 })
    await flushPromises()
    await wrapper.setProps({ tdInterface: store.interfaces[0] })
    expect(wrapper.find('[data-test="td-login-account_001"]').attributes('disabled')).toBeDefined()
    const readyLogout = wrapper.find('[data-test="td-logout-account_001"]')
    expect(readyLogout.attributes('disabled')).toBeUndefined()

    await readyLogout.trigger('click')
    await flushPromises()
    expect(tdApi.logoutAccount).toHaveBeenCalledWith('account_001')
  })

  it('启停走 UpdateAccount 的 {enabled}（契约 rest §2.9.7 唯一写端点）并提示等待回报', async () => {
    const store = await prepare()
    vi.mocked(tdApi.updateAccount).mockResolvedValue({ ok: true })
    const successSpy = vi.spyOn(useToastStore(), 'success')
    const { wrapper } = mountCard(store)

    await wrapper.find('[data-test="td-toggle-account_001"]').trigger('click')
    await flushPromises()

    expect(tdApi.updateAccount).toHaveBeenCalledWith('account_001', { enabled: false })
    expect(successSpy).toHaveBeenCalledWith(
      expect.stringContaining('等待交易网关回报配置') as unknown as string)
  })

  it('progress 中间态（current < max）登出可点 = 强制中断，登录禁用（ADR 0051）', async () => {
    const store = await prepare()
    useProgressStore().applyProgress('dztd_ctp:account_001', { min: 0, max: 10, current: 2 })
    const { wrapper } = mountCard(store)

    expect(wrapper.find('[data-test="td-login-account_001"]').attributes('disabled')).toBeDefined()
    expect(wrapper.find('[data-test="td-logout-account_001"]').attributes('disabled')).toBeUndefined()
  })

  it('网关未运行：登录/登出双按钮禁用并提示先启动接口进程', async () => {
    const store = await prepare({ running: false })
    const { wrapper } = mountCard(store)

    const login = wrapper.find('[data-test="td-login-account_001"]')
    const logout = wrapper.find('[data-test="td-logout-account_001"]')
    expect(login.attributes('disabled')).toBeDefined()
    expect(logout.attributes('disabled')).toBeDefined()
    expect(login.attributes('title')).toContain('交易接口未运行，请先点击「启动进程」')
    expect(logout.attributes('title')).toContain('交易接口未运行，请先点击「启动进程」')
  })

  it('本卡添加在途时添加按钮禁用（spinner），不影响其他卡片入口（addPendingFor 按网关）', async () => {
    const store = await prepare({ accounts: [] })
    vi.mocked(tdApi.addAccount).mockResolvedValue({ ok: true })
    const { wrapper } = mountCard(store)

    await wrapper.find('[data-test="td-card-add-account"]').trigger('click')
    await flushPromises()
    await wrapper.find('input[placeholder="账户唯一标识，全部交易接口间唯一"]').setValue('account_new')
    await wrapper.find('input[placeholder="broker_id"]').setValue('9999')
    await wrapper.find('input[placeholder="user_id"]').setValue('00001')
    await wrapper.find('input[placeholder="登录密码"]').setValue('secret')
    await flushPromises()
    await wrapper.find('[data-test="td-form-submit"]').trigger('click')
    await flushPromises()

    expect(store.addPendingFor('dztd_ctp')).toBe(true)
    expect(wrapper.find('[data-test="td-card-add-account"]').attributes('disabled')).toBeDefined()
  })

  it('表单校验拦住缺失必填项：不填 broker_id 时提交按钮禁用（本地预检）', async () => {
    const store = await prepare({ accounts: [] })
    const { wrapper } = mountCard(store)

    await wrapper.find('[data-test="td-card-add-account"]').trigger('click')
    await flushPromises()
    await wrapper.find('input[placeholder="账户唯一标识，全部交易接口间唯一"]').setValue('account_new')
    await flushPromises()

    expect(wrapper.find('[data-test="td-form-submit"]').attributes('disabled')).toBeDefined()
    expect(wrapper.find('[data-test="td-form-validation"]').text()).toContain('经纪公司代码')
  })

  it('唯一性冲突 → 表单内提示归属网关且不发请求（契约 rest §2.9.7 预检 4）', async () => {
    const store = await prepare({
      accounts: [],
      registryAccounts: [{ account_id: 'taken', gateway: 'dztd_xtp' }],
    })
    vi.mocked(tdApi.addAccount).mockResolvedValue({ ok: true })
    const { wrapper } = mountCard(store)

    await wrapper.find('[data-test="td-card-add-account"]').trigger('click')
    await flushPromises()
    await wrapper.find('input[placeholder="账户唯一标识，全部交易接口间唯一"]').setValue('taken')
    await wrapper.find('input[placeholder="broker_id"]').setValue('9999')
    await wrapper.find('input[placeholder="user_id"]').setValue('00001')
    await wrapper.find('input[placeholder="登录密码"]').setValue('secret')
    await flushPromises()
    await wrapper.find('[data-test="td-form-submit"]').trigger('click')
    await flushPromises()

    const error = wrapper.find('[data-test="td-form-error"]')
    expect(error.exists()).toBe(true)
    expect(error.text()).toContain('占用')
    expect(error.text()).toContain('dztd_xtp')
    expect(tdApi.addAccount).not.toHaveBeenCalled()
  })

  it('编辑账户：UpdateAccount patch 不含 account_id，密码留空不提交（契约 td-config §2.1/§2.3）', async () => {
    const store = await prepare()
    // 配置镜像提供 ext（密码恒脱敏），编辑表单据此回填 broker/user
    store.applyTdConfig('dztd_ctp', {
      qry_account_interval_s: 5,
      qry_flush_interval_ms: 1500,
      accounts: [{
        account_id: 'account_001',
        enabled: true,
        ext: { broker_id: '9999', user_id: 'u1', password: '****' },
      }],
      ext: {},
    })
    vi.mocked(tdApi.updateAccount).mockResolvedValue({ ok: true })
    const { wrapper } = mountCard(store)

    await wrapper.find('[data-test="td-edit-account_001"]').trigger('click')
    await flushPromises()
    // 编辑态密码留空 = 不修改（占位说明），不把 **** 当密码回填
    expect(wrapper.find('input[placeholder="留空 = 不修改密码"]').exists()).toBe(true)
    expect((wrapper.find('input[placeholder="留空 = 不修改密码"]').element as HTMLInputElement).value).toBe('')

    await wrapper.find('[data-test="td-form-submit"]').trigger('click')
    await flushPromises()

    expect(tdApi.updateAccount).toHaveBeenCalledTimes(1)
    const [accountId, patch] = vi.mocked(tdApi.updateAccount).mock.calls[0]
    expect(accountId).toBe('account_001')
    expect(patch).not.toHaveProperty('account_id')
    // 密码留空不得提交（避免把掩码/空串写成新密码）
    expect(patch.ext).not.toHaveProperty('password')
  })

  it('非 admin：接口级按钮不渲染，账户写按钮禁用并给出原因', async () => {
    loginAsUser()
    const store = await prepare({ running: false })
    const { wrapper } = mountCard(store)

    // 契约 AC5：非 admin 无添加/删除/启停接口按钮（不是禁用）
    expect(wrapper.find('[data-test="td-interface-start"]').exists()).toBe(false)
    expect(wrapper.find('[data-test="td-interface-remove"]').exists()).toBe(false)
    // 账户写按钮保留在页面上但禁用（现有权限行为保持）
    const addAccount = wrapper.find('[data-test="td-card-add-account"]')
    expect(addAccount.exists()).toBe(true)
    expect(addAccount.attributes('disabled')).toBeDefined()
    expect(addAccount.attributes('title')).toContain('仅管理员')
    expect(wrapper.find('[data-test="td-edit-account_001"]').attributes('disabled')).toBeDefined()
    expect(wrapper.find('[data-test="td-login-account_001"]').attributes('disabled')).toBeDefined()
  })
})
