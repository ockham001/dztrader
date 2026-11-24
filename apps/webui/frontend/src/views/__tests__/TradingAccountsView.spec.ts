import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest'
import { mount, flushPromises } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import { useTdAccountsStore, tdAccountsPending, tdGatewayPending } from '@/stores/tdAccounts'
import { useAuthStore } from '@/stores/auth'
import { useProcessStore } from '@/stores/process'
import { useToastStore } from '@/stores/toast'
import { tdApi } from '@/api/td'
import { tdGatewaysApi } from '@/api/tdGateways'
import type { ProcessState } from '@/types/api'
import TradingAccountsView from '../TradingAccountsView.vue'

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
 * 交易账户页（#439 / ADR 0053；契约 rest §2.9.7/§2.9.9/§2.10/§4）。
 *
 * 页面级契约事实（卡片细节已在 TdInterfaceCard.spec 覆盖，这里固定「页面给出什么」）：
 * - 页面 = 交易接口卡片列表，导航/页头文案仍是「交易账户」；空系统 → 空态 + 添加入口；
 * - 卡片来源 = WS process_config / process_status 的 dztd_* 条目（不拉列表端点）；
 * - 页面级只有 1 个入口「添加交易接口」：available 下拉 + 显示名弹窗 + 添加即 start；
 * - 非 admin：无接口级按钮、账户写按钮禁用（现有权限行为保持）；
 * - 账户注册表降级 → 展示降级提示，不把降级当空列表。
 */
type TdAccountsStoreView = ReturnType<typeof useTdAccountsStore>

const stubs = {
  Icon: { props: ['name', 'size'], template: '<span class="icon" :data-icon="name" />' },
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

/** 登记一个 dztd_* 进程（卡片来源 = process 镜像） */
function registerGateway(
  name: string,
  options: { running?: boolean; displayName?: string; state?: ProcessState } = {},
): void {
  const running = options.running ?? true
  const process = useProcessStore()
  process.applyProcessConfig({
    ...process.configs,
    [name]: { exe: `${name}.exe`, display_name: options.displayName ?? name },
  })
  process.applyProcessStatus({
    name,
    state: options.state ?? (running ? 'Running' : 'Stopped'),
    pid: running ? 100 : 0,
  })
}

function mockRegistry(
  accounts: { account_id: string; account_name?: string; gateway: string; enabled?: boolean }[],
): void {
  vi.mocked(tdApi.listAccounts).mockResolvedValue({
    available: true,
    accounts: accounts.map(account => ({ enabled: true, account_name: '', ...account })),
  })
}

function mountPage() {
  return mount(TradingAccountsView, { global: { stubs } })
}

describe('TradingAccountsView · 空态与卡片渲染（ADR 0053 / 契约 rest §4）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    vi.resetAllMocks()
    tdAccountsPending.__resetForTests()
    tdGatewayPending.__resetForTests()
    loginAsAdmin()
    mockRegistry([])
    vi.mocked(tdGatewaysApi.listAvailable).mockResolvedValue([])
  })

  afterEach(() => {
    useTdAccountsStore().dispose()
  })

  it('进程配置快照未到达：先给加载占位，不闪「还没有交易接口」空态', async () => {
    const w = mountPage()
    await flushPromises()

    expect(w.find('[data-test="td-interfaces-loading"]').exists()).toBe(true)
    expect(w.find('[data-test="td-interfaces-loading"]').text()).toContain('正在读取交易接口进程配置')
    expect(w.find('[data-test="td-interfaces-empty"]').exists()).toBe(false)
  })

  it('无任何交易接口（镜像已就绪）：空态 + 页面级唯一入口「添加交易接口」，标题仍是「交易账户」', async () => {
    // process_config 全量帧到达 = 镜像就绪（空配置也是有效快照，之后才允许判空）
    useProcessStore().applyProcessConfig({})
    const w = mountPage()
    await flushPromises()

    expect(w.text()).toContain('交易账户')
    expect(w.find('[data-test="td-interface-count"]').text()).toContain('共 0 个交易接口')
    expect(w.find('[data-test="td-interfaces-empty"]').exists()).toBe(true)
    // 页面级只有 1 个入口：SyncSelect 触发器（带 Plus 前缀），没有第二个按钮
    expect(w.find('[data-test="td-add-interface"]').exists()).toBe(true)
    expect(w.find('[data-test="td-add-interface"]').text()).toContain('添加交易接口')
    expect(w.findAll('button')).toHaveLength(1)
  })

  it('卡片来自 process 镜像：显示名/进程名/状态/账户；页头聚合计数', async () => {
    registerGateway('dztd_ctp', { displayName: 'CTP 交易接口' })
    registerGateway('dztd_xtp', { displayName: 'XTP 交易接口', running: false })
    mockRegistry([
      { account_id: 'account_001', gateway: 'dztd_ctp' },
      { account_id: 'account_009', gateway: 'dztd_xtp' },
    ])
    const w = mountPage()
    await flushPromises()

    expect(w.find('[data-test="td-interface-count"]').text()).toContain('共 2 个交易接口 · 2 个账户')
    expect(w.find('[data-test="td-interface-card-dztd_ctp"]').exists()).toBe(true)
    expect(w.find('[data-test="td-interface-card-dztd_xtp"]').exists()).toBe(true)

    const ctp = w.find('[data-test="td-interface-card-dztd_ctp"]')
    expect(ctp.find('[data-test="td-interface-name"]').text()).toBe('CTP 交易接口')
    expect(ctp.find('[data-test="td-interface-process"]').text()).toBe('dztd_ctp')
    expect(ctp.find('[data-test="td-interface-state"]').text()).toContain('运行中')
    expect(ctp.find('[data-test="td-account-row-account_001"]').exists()).toBe(true)

    const xtp = w.find('[data-test="td-interface-card-dztd_xtp"]')
    expect(xtp.find('[data-test="td-interface-state"]').text()).toContain('已停止')
    expect(xtp.find('[data-test="td-account-row-account_009"]').exists()).toBe(true)
  })

  it('账户注册表降级 → 展示降级提示，配置镜像里的账户照常进卡片（不当作无账户）', async () => {
    registerGateway('dztd_ctp', { displayName: 'CTP 交易接口' })
    vi.mocked(tdApi.listAccounts).mockResolvedValue({
      available: false, reason: 'no_gateway_snapshot',
    })
    const store = useTdAccountsStore()
    store.applyTdConfig('dztd_ctp', {
      qry_min_interval_ms: 1100,
      accounts: [{
        account_id: 'account_001',
        enabled: true,
        ext: { broker_id: '9999', password: '****' },
      }],
      ext: {},
    })
    const w = mountPage()
    await flushPromises()

    expect(w.find('[data-test="td-accounts-degraded"]').exists()).toBe(true)
    expect(w.find('[data-test="td-accounts-degraded"]').text()).toContain('账户注册表暂不可用')
    expect(w.find('[data-test="td-account-row-account_001"]').exists()).toBe(true)
  })

  it('非 admin：无添加交易接口入口，显示只读提示；卡片内无接口级按钮', async () => {
    loginAsUser()
    registerGateway('dztd_ctp', { displayName: 'CTP 交易接口' })
    mockRegistry([{ account_id: 'account_001', gateway: 'dztd_ctp' }])
    const w = mountPage()
    await flushPromises()

    expect(w.find('[data-test="td-add-interface"]').exists()).toBe(false)
    expect(w.find('[data-test="td-accounts-readonly"]').exists()).toBe(true)
    // 契约 AC5：非 admin 无删除/启停接口按钮（不是禁用）
    expect(w.find('[data-test="td-interface-remove"]').exists()).toBe(false)
    expect(w.find('[data-test="td-interface-start"]').exists()).toBe(false)
    // 账户写按钮禁用（现有权限行为保持）
    expect(w.find('[data-test="td-card-add-account"]').attributes('disabled')).toBeDefined()
    expect(w.find('[data-test="td-edit-account_001"]').attributes('disabled')).toBeDefined()
  })
})

describe('TradingAccountsView · 添加交易接口（契约 rest §2.10，与行情源页同构）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    vi.resetAllMocks()
    tdAccountsPending.__resetForTests()
    tdGatewayPending.__resetForTests()
    loginAsAdmin()
    mockRegistry([])
  })

  afterEach(() => {
    useTdAccountsStore().dispose()
  })

  function mockAvailable(): void {
    vi.mocked(tdGatewaysApi.listAvailable).mockResolvedValue([
      { name: 'dztd_ctp', display_name: 'CTP 交易接口', added: false, running: false },
      { name: 'dztd_xtp', display_name: 'XTP 交易接口', added: true, running: true },
    ])
  }

  async function openDropdown(w: ReturnType<typeof mountPage>): Promise<void> {
    await w.find('[data-test="td-add-interface"] .ds-sync-select__trigger').trigger('click')
    await flushPromises()
  }

  it('打开下拉即刷新 available，已添加项标注「(已添加)」', async () => {
    mockAvailable()
    const w = mountPage()
    await flushPromises()

    await openDropdown(w)

    expect(tdGatewaysApi.listAvailable).toHaveBeenCalled()
    const items = w.findAll('.ds-sync-select__item').map(item => item.text())
    expect(items).toEqual(['CTP 交易接口', 'XTP 交易接口 (已添加)'])
  })

  it('选择未添加项 → 显示名弹窗 → 确认调 start；WS 条目到达后卡片出现', async () => {
    mockAvailable()
    vi.mocked(tdGatewaysApi.start).mockResolvedValue({ ok: true })
    const w = mountPage()
    await flushPromises()
    await openDropdown(w)

    await w.findAll('.ds-sync-select__item')[0].trigger('click')
    await flushPromises()

    const modal = w.find('.modal-stub')
    expect(modal.attributes('data-title')).toContain('添加交易接口：dztd_ctp')
    const displayInput = w.find('input[placeholder="例如：CTP 交易接口"]')
    expect((displayInput.element as HTMLInputElement).value).toBe('CTP 交易接口')

    await displayInput.setValue('CTP 主交易接口')
    await w.find('[data-test="td-add-interface-confirm"]').trigger('click')
    await flushPromises()

    // 添加即 start（body 带显示名）；HTTP 200 不伪造卡片
    expect(tdGatewaysApi.start).toHaveBeenCalledWith('dztd_ctp', { display_name: 'CTP 主交易接口' })
    expect(w.find('.modal-stub').exists()).toBe(false)
    expect(w.find('[data-test="td-interface-card-dztd_ctp"]').exists()).toBe(false)

    // WS process_config / process_status 到达 → 卡片出现（接口可零账户）
    registerGateway('dztd_ctp', { displayName: 'CTP 主交易接口' })
    await flushPromises()
    expect(w.find('[data-test="td-interface-card-dztd_ctp"]').exists()).toBe(true)
    expect(w.find('[data-test="td-interface-empty"]').exists()).toBe(true)
  })

  it('选择已添加项被忽略：不弹窗、不发 start（恢复走卡片「启动进程」）', async () => {
    mockAvailable()
    const w = mountPage()
    await flushPromises()
    await openDropdown(w)

    await w.findAll('.ds-sync-select__item')[1].trigger('click')
    await flushPromises()

    expect(w.find('.modal-stub').exists()).toBe(false)
    expect(tdGatewaysApi.start).not.toHaveBeenCalled()
  })

  it('start 失败（invalid_gateway）→ 弹窗内展示中文原因，不回显英文 error', async () => {
    mockAvailable()
    vi.mocked(tdGatewaysApi.start).mockRejectedValue(Object.assign(
      new Error('invalid gateway'),
      { status: 400, body: { error: 'invalid gateway', code: 'invalid_gateway' } },
    ))
    const w = mountPage()
    await flushPromises()
    await openDropdown(w)

    await w.findAll('.ds-sync-select__item')[0].trigger('click')
    await flushPromises()
    await w.find('[data-test="td-add-interface-confirm"]').trigger('click')
    await flushPromises()

    const error = w.find('[data-test="td-add-interface-error"]')
    expect(error.exists()).toBe(true)
    expect(error.text()).toContain('不是可用的交易接口')
    expect(error.text()).not.toContain('invalid gateway')
    // 失败停在弹窗内可重试，不会留下幽灵卡片
    expect(w.find('.modal-stub').exists()).toBe(true)
  })
})

describe('TradingAccountsView · 非 Running 与删除 guard（ADR 0053 / 契约 rest §4）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    vi.resetAllMocks()
    tdAccountsPending.__resetForTests()
    tdGatewayPending.__resetForTests()
    loginAsAdmin()
    vi.mocked(tdGatewaysApi.listAvailable).mockResolvedValue([])
  })

  afterEach(() => {
    useTdAccountsStore().dispose()
  })

  it('非 Running 卡片显示「启动进程」；点击调 start 恢复（不得删除重加）', async () => {
    registerGateway('dztd_ctp', { displayName: 'CTP 交易接口', running: false })
    mockRegistry([])
    vi.mocked(tdGatewaysApi.start).mockResolvedValue({ ok: true })
    const w = mountPage()
    await flushPromises()

    expect(w.find('[data-test="td-interface-state"]').text()).toContain('已停止')
    const start = w.find('[data-test="td-interface-start"]')
    expect(start.exists()).toBe(true)

    await start.trigger('click')
    await flushPromises()

    expect(tdGatewaysApi.start).toHaveBeenCalledWith('dztd_ctp')
  })

  it('有账户时删除接口按钮禁用 + title「先删除账户」；清空后确认删除才发 DELETE', async () => {
    registerGateway('dztd_ctp', { displayName: 'CTP 交易接口' })
    mockRegistry([{ account_id: 'account_001', gateway: 'dztd_ctp' }])
    vi.mocked(tdGatewaysApi.remove).mockResolvedValue({ ok: true })
    const store: TdAccountsStoreView = useTdAccountsStore()
    const w = mountPage()
    await flushPromises()

    const removeButton = w.find('[data-test="td-interface-remove"]')
    expect(removeButton.attributes('disabled')).toBeDefined()
    expect(removeButton.attributes('title')).toBe('先删除账户')
    await removeButton.trigger('click')
    await flushPromises()
    expect(tdGatewaysApi.remove).not.toHaveBeenCalled()

    // 清空账户（注册表重拉）→ 删除按钮恢复可用
    mockRegistry([])
    await store.loadRegistry()
    await flushPromises()
    expect(w.find('[data-test="td-interface-remove"]').attributes('disabled')).toBeUndefined()

    await w.find('[data-test="td-interface-remove"]').trigger('click')
    await flushPromises()
    expect(w.find('[data-test="td-interface-remove-confirm-text"]').text()).toContain('CTP 交易接口')
    await w.find('[data-test="td-interface-remove-confirm"]').trigger('click')
    await flushPromises()

    expect(tdGatewaysApi.remove).toHaveBeenCalledWith('dztd_ctp')
  })

  it('删除成功提示「等待进程配置回报」；卡片由 WS 条目消失移除（不本地伪造）', async () => {
    registerGateway('dztd_ctp', { displayName: 'CTP 交易接口' })
    mockRegistry([])
    vi.mocked(tdGatewaysApi.remove).mockResolvedValue({ ok: true })
    const toast = useToastStore()
    const successSpy = vi.spyOn(toast, 'success')
    const w = mountPage()
    await flushPromises()

    await w.find('[data-test="td-interface-remove"]').trigger('click')
    await flushPromises()
    await w.find('[data-test="td-interface-remove-confirm"]').trigger('click')
    await flushPromises()

    expect(successSpy).toHaveBeenCalledWith(expect.stringContaining('已提交移除请求') as unknown as string)
    // HTTP 200 不代表已生效：卡片仍在，等 process_config 条目消失
    expect(w.find('[data-test="td-interface-card-dztd_ctp"]').exists()).toBe(true)

    useProcessStore().applyProcessConfig({})
    await flushPromises()
    expect(w.find('[data-test="td-interface-card-dztd_ctp"]').exists()).toBe(false)
    expect(w.find('[data-test="td-interfaces-empty"]').exists()).toBe(true)
  })

  it('store.error 非空时触发 toast 并清空（页面不重复展示）', async () => {
    const store = useTdAccountsStore()
    const toast = useToastStore()
    const toastSpy = vi.spyOn(toast, 'error')
    const w = mountPage()
    await flushPromises()

    store.error = '测试错误'
    await w.vm.$nextTick()

    expect(toastSpy).toHaveBeenCalledWith('测试错误')
    expect(store.error).toBeNull()
  })
})
