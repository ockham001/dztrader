import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest'
import { mount, flushPromises } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import { useTdAccountsStore, tdAccountsPending } from '@/stores/tdAccounts'
import { useAuthStore } from '@/stores/auth'
import { useProcessStore } from '@/stores/process'
import { useToastStore } from '@/stores/toast'
import { tdApi } from '@/api/td'
import TradingAccountsView from '../TradingAccountsView.vue'

vi.mock('@/api/td', () => ({
  tdApi: {
    listAccounts: vi.fn(),
    addAccount: vi.fn(),
    updateAccount: vi.fn(),
    removeAccount: vi.fn(),
  },
}))

/**
 * 交易账户配置页（#251；契约 rest §2.9.7/§2.9.9/§4）。
 *
 * 页面级契约事实（store 已覆盖的不重复，这里固定「用户看到什么」）：
 * - 降级 `available:false` → 展示降级提示，**不得**当作空列表；
 * - 非 admin → 写操作禁用并给出原因（契约 rest §2.9.7 预检 1）；
 * - 删除必须二次确认（确认框未确认不发请求）；
 * - 冲突提示可见且不发请求；密码不展示明文（回报恒为 ****）；
 * - 细状态/交易日/登录时间来自 td_rtn_status，未上报显示占位。
 */
const stubs = {
  Icon: { props: ['name', 'size'], template: '<span class="icon" :data-icon="name" />' },
  Modal: {
    props: ['open', 'title'],
    template: '<div v-if="open" class="modal-stub" :data-title="title"><slot /><div class="modal-footer-stub"><slot name="footer" /></div></div>',
  },
  Dropdown: {
    props: ['items', 'modelValue', 'id', 'placeholder', 'disabled'],
    template: '<select class="dropdown-stub"><option v-for="i in items" :key="i.value" :value="i.value">{{ i.label }}</option></select>',
  },
}

/** 管理员登录态（页面写操作前置） */
function loginAsAdmin(): void {
  const auth = useAuthStore()
  auth.user = {
    id: 1, username: 'admin', display_name: 'Admin', email: 'a@x.com',
    role: 'admin', status: 'online', created_at: '2026-08-01',
  }
}

function loginAsUser(): void {
  const auth = useAuthStore()
  auth.user = {
    id: 2, username: 'bob', display_name: 'Bob', email: 'b@x.com',
    role: 'user', status: 'online', created_at: '2026-08-01',
  }
}

function registerGateway(name: string, running = true): void {
  const process = useProcessStore()
  process.applyProcessConfig({ [name]: { exe: `${name}.exe` } })
  process.applyProcessStatus({
    name,
    state: running ? 'Running' : 'Stopped',
    pid: running ? 100 : 0,
  })
}

function accountRow(overrides: Record<string, unknown> = {}) {
  return { account_id: 'account_001', gateway: 'dztd_ctp', enabled: true, ...overrides }
}

describe('TradingAccountsView · 列表与状态（契约 rest §2.9.9、契约 td-status）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    vi.resetAllMocks()
    tdAccountsPending.__resetForTests()
    loginAsAdmin()
    registerGateway('dztd_ctp')
    vi.mocked(tdApi.listAccounts).mockResolvedValue({ available: true, accounts: [] })
  })

  afterEach(() => {
    useTdAccountsStore().dispose()
  })

  it('挂载即拉注册表并渲染账户行与聚合数', async () => {
    vi.mocked(tdApi.listAccounts).mockResolvedValue({
      available: true,
      accounts: [accountRow(), accountRow({ account_id: 'account_002', enabled: false })],
    })
    const w = mount(TradingAccountsView, { global: { stubs } })
    await flushPromises()

    expect(tdApi.listAccounts).toHaveBeenCalledTimes(1)
    expect(w.text()).toContain('交易账户')
    expect(w.find('[data-test="td-account-count"]').text()).toContain('共 2 个账户')
    expect(w.find('[data-test="td-account-count"]').text()).toContain('启用 1')
    expect(w.find('[data-test="td-account-row-account_001"]').exists()).toBe(true)
    expect(w.find('[data-test="td-account-row-account_002"]').exists()).toBe(true)
  })

  it('细状态/交易日/登录时间来自 td_rtn_status；未上报显示占位', async () => {
    vi.mocked(tdApi.listAccounts).mockResolvedValue({
      available: true,
      accounts: [accountRow(), accountRow({ account_id: 'account_002' })],
    })
    const store = useTdAccountsStore()
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
    const w = mount(TradingAccountsView, { global: { stubs } })
    await flushPromises()

    const ready = w.find('[data-test="td-account-row-account_001"]')
    expect(ready.find('[data-test="td-account-state"]').text()).toBe('就绪')
    expect(ready.text()).toContain('2026-08-08')
    expect(ready.text()).toContain('2026-08-08 08:45:32')

    // 未上报：占位而不是"未启动/今天"之类的伪造值
    const unknown = w.find('[data-test="td-account-row-account_002"]')
    expect(unknown.find('[data-test="td-account-state"]').text()).toBe('—')
    expect(unknown.text()).toContain('—')
    expect(w.find('[data-test="td-account-count"]').text()).toContain('就绪 1')
  })

  it('注册表 available:false → 展示降级提示，且不当作空列表', async () => {
    vi.mocked(tdApi.listAccounts).mockResolvedValue({
      available: false, reason: 'no_gateway_snapshot',
    })
    const w = mount(TradingAccountsView, { global: { stubs } })
    await flushPromises()

    expect(w.find('[data-test="td-accounts-degraded"]').exists()).toBe(true)
    expect(w.find('[data-test="td-accounts-degraded"]').text()).toContain('账户注册表暂不可用')
    // 关键契约：降级不显示"还没有配置任何交易账户"（用户会以为账户被删了）
    expect(w.find('[data-test="td-accounts-empty"]').exists()).toBe(false)
  })

  it('成功读到空注册表 → 空态提示（与降级可区分）', async () => {
    const w = mount(TradingAccountsView, { global: { stubs } })
    await flushPromises()

    expect(w.find('[data-test="td-accounts-empty"]').exists()).toBe(true)
    expect(w.find('[data-test="td-accounts-degraded"]').exists()).toBe(false)
  })

  it('降级但配置镜像有账户 → 仍展示账户行（降级不等于隐藏事实）', async () => {
    vi.mocked(tdApi.listAccounts).mockResolvedValue({
      available: false, reason: 'no_gateway_snapshot',
    })
    const store = useTdAccountsStore()
    store.applyTdConfig('dztd_ctp', {
      qry_account_interval_s: 5,
      qry_position_interval_s: 60,
      qry_flush_interval_ms: 1500,
      accounts: [{
        account_id: 'account_001', enabled: true,
        ext: { broker_id: '9999', user_id: 'u1', password: '****' },
      }],
      ext: {},
    })
    const w = mount(TradingAccountsView, { global: { stubs } })
    await flushPromises()

    expect(w.find('[data-test="td-accounts-degraded"]').exists()).toBe(true)
    expect(w.find('[data-test="td-account-row-account_001"]').exists()).toBe(true)
  })

  it('密码不展示明文（配置镜像里的值恒为 ****，页面也不提供明文入口）', async () => {
    vi.mocked(tdApi.listAccounts).mockResolvedValue({ available: true, accounts: [accountRow()] })
    const store = useTdAccountsStore()
    store.applyTdConfig('dztd_ctp', {
      qry_account_interval_s: 5,
      qry_position_interval_s: 60,
      qry_flush_interval_ms: 1500,
      accounts: [{
        account_id: 'account_001', enabled: true,
        ext: { broker_id: '9999', user_id: 'u1', password: '****' },
      }],
      ext: {},
    })
    const w = mount(TradingAccountsView, { global: { stubs } })
    await flushPromises()

    // 列表不渲染任何密码文本；打开编辑表单也不把 **** 当作用户可读的密码回显
    expect(w.text()).not.toContain('password')
  })
})

describe('TradingAccountsView · 权限与写操作（契约 rest §2.9.7）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    vi.resetAllMocks()
    tdAccountsPending.__resetForTests()
    registerGateway('dztd_ctp')
    vi.mocked(tdApi.listAccounts).mockResolvedValue({ available: true, accounts: [accountRow()] })
  })

  afterEach(() => {
    useTdAccountsStore().dispose()
  })

  it('非 admin：添加/编辑/删除/启停全部禁用，并展示只读原因', async () => {
    loginAsUser()
    const w = mount(TradingAccountsView, { global: { stubs } })
    await flushPromises()

    expect(w.find('[data-test="td-accounts-readonly"]').exists()).toBe(true)
    expect(w.find('[data-test="td-add-account"]').attributes('disabled')).toBeDefined()
    expect(w.find('[data-test="td-edit-account_001"]').attributes('disabled')).toBeDefined()
    expect(w.find('[data-test="td-toggle-account_001"]').attributes('disabled')).toBeDefined()
    expect(w.find('[data-test="td-remove-account_001"]').attributes('disabled')).toBeDefined()
  })

  it('删除必须先二次确认：未确认不发请求，确认后才发 DELETE', async () => {
    loginAsAdmin()
    vi.mocked(tdApi.removeAccount).mockResolvedValue({ ok: true })
    const w = mount(TradingAccountsView, { global: { stubs } })
    await flushPromises()

    await w.find('[data-test="td-remove-account_001"]').trigger('click')
    await flushPromises()

    // 确认框出现且说明影响范围
    expect(w.find('[data-test="td-remove-confirm-text"]').exists()).toBe(true)
    expect(w.text()).toContain('已存在的交易数据不会被删除')
    expect(tdApi.removeAccount).not.toHaveBeenCalled()

    await w.find('[data-test="td-remove-confirm"]').trigger('click')
    await flushPromises()

    expect(tdApi.removeAccount).toHaveBeenCalledWith('account_001')
  })

  it('启停走 UpdateAccount 的 {enabled}（契约 rest §2.9.7 唯一写端点）', async () => {
    loginAsAdmin()
    vi.mocked(tdApi.updateAccount).mockResolvedValue({ ok: true })
    const w = mount(TradingAccountsView, { global: { stubs } })
    await flushPromises()

    await w.find('[data-test="td-toggle-account_001"]').trigger('click')
    await flushPromises()

    expect(tdApi.updateAccount).toHaveBeenCalledWith('account_001', { enabled: false })
  })

  it('写操作成功后提示「等待回报」，不宣称已生效（契约 webui-ws §5 第 1 条）', async () => {
    loginAsAdmin()
    vi.mocked(tdApi.updateAccount).mockResolvedValue({ ok: true })
    const toast = useToastStore()
    const successSpy = vi.spyOn(toast, 'success')
    const w = mount(TradingAccountsView, { global: { stubs } })
    await flushPromises()

    await w.find('[data-test="td-toggle-account_001"]').trigger('click')
    await flushPromises()

    expect(successSpy).toHaveBeenCalled()
    expect(successSpy.mock.calls[0][0]).toContain('等待交易网关回报')
  })

  it('唯一性冲突 → 表单内提示「已被占用」且不发请求（契约 rest §2.9.7 预检 4）', async () => {
    loginAsAdmin()
    vi.mocked(tdApi.listAccounts).mockResolvedValue({
      available: true,
      accounts: [accountRow({ account_id: 'taken', gateway: 'dztd_ctp_2' })],
    })
    vi.mocked(tdApi.addAccount).mockResolvedValue({ ok: true })
    const w = mount(TradingAccountsView, { global: { stubs } })
    await flushPromises()

    await w.find('[data-test="td-add-account"]').trigger('click')
    await flushPromises()

    // 填必填字段：account_id 与注册表中 dztd_ctp_2 已占用的值相同
    await w.find('input[placeholder="账户唯一标识，跨全部交易网关唯一"]').setValue('taken')
    await w.find('input[placeholder="broker_id"]').setValue('9999')
    await w.find('input[placeholder="user_id"]').setValue('00001')
    await w.find('input[placeholder="登录密码"]').setValue('secret')
    await flushPromises()

    const submit = w.find('[data-test="td-form-submit"]')
    expect(submit.attributes('disabled')).toBeUndefined()
    await submit.trigger('click')
    await flushPromises()

    // 冲突是前端预检：明确提示归属网关，且**不发请求**（权威校验在 dzweb）
    const error = w.find('[data-test="td-form-error"]')
    expect(error.exists()).toBe(true)
    expect(error.text()).toContain('占用')
    expect(error.text()).toContain('dztd_ctp_2')
    expect(tdApi.addAccount).not.toHaveBeenCalled()
  })

  it('表单校验拦住缺失必填项：不填 broker_id 时提交按钮禁用（本地预检）', async () => {
    loginAsAdmin()
    const w = mount(TradingAccountsView, { global: { stubs } })
    await flushPromises()

    await w.find('[data-test="td-add-account"]').trigger('click')
    await flushPromises()
    await w.find('input[placeholder="账户唯一标识，跨全部交易网关唯一"]').setValue('account_new')
    await flushPromises()

    const submit = w.find('[data-test="td-form-submit"]')
    expect(submit.attributes('disabled')).toBeDefined()
    expect(w.find('[data-test="td-form-validation"]').text()).toContain('经纪公司代码')
  })
})
