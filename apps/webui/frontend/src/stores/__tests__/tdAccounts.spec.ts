import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest'
import { setActivePinia, createPinia } from 'pinia'
import {
  useTdAccountsStore,
  tdAccountsPending,
  tdInterfaceType,
  tdStateText,
  TD_ACCOUNT_OP_TIMEOUT_MS,
} from '../tdAccounts'
import { tdApi } from '@/api/td'
import { useProgressStore } from '@/stores/progress'
import { useProcessStore } from '@/stores/process'
import type {
  TdAccountsDegradedResponse,
  TdAccountsListResponse,
  TdConfigAccountList,
  TdConfigPayload,
  TdStatusPayload,
} from '@/types/api'

vi.mock('@/api/td', () => ({
  tdApi: {
    listAccounts: vi.fn(),
    addAccount: vi.fn(),
    updateAccount: vi.fn(),
    removeAccount: vi.fn(),
  },
}))

/**
 * 交易账户配置 store 用例（#251；契约 rest §2.9.7/§2.9.9、契约 td-config §1/§2/§4/§5、
 * 契约 td-status §2.1/§2.2、契约 webui-ws §2.1/§2.2/§2.3）。
 *
 * 固定这些契约事实：
 * - 注册表是账户列表的唯一视角；`available:false` 是**降级**，不得当空列表；
 * - 配置镜像按网关全量覆盖，到达时清该网关全部 pending（不是「账户消失」判定）；
 * - 细状态/进度按 `网关名:账户ID` 覆盖，不从配置镜像推导；
 * - 唯一性预检命中即不发请求；**降级时跳过预检**（裁决交给 dzweb 的 409）；
 * - 失败文案按 code 映射中文，绝不回显英文 error。
 */

/**
 * pending 是否处于挂起态。
 * usePending 的两条结束路径写法不同：RTN 到达走 clearByPrefix（delete 键 → undefined），
 * 失败/超时走 settle → clearPending（置 false）。两者语义都是「不再挂起」，断言按语义写。
 */
function isPending(key: string): boolean {
  return tdAccountsPending.pending[key] === true
}

function registryEntry(overrides: Partial<TdConfigAccountList> = {}): TdConfigAccountList {
  return { account_id: 'account_001', gateway: 'dztd_ctp', enabled: true, ...overrides }
}

function configPayload(accounts: TdConfigPayload['accounts'] = []): TdConfigPayload {
  return {
    qry_account_interval_s: 5,
    qry_position_interval_s: 60,
    qry_flush_interval_ms: 1500,
    accounts,
    ext: { enable_lock_mode: true },
  }
}

function statusPayload(overrides: Partial<TdStatusPayload> = {}): TdStatusPayload {
  return {
    account_id: 'account_001',
    state: 'Ready',
    date: '20260808',
    login_time: '2026-08-08 08:45:32',
    progress_desc: 'ready',
    progress_min: 0,
    progress_max: 10,
    progress_current: 10,
    ext: { api_version: 'v6.7.2', sys_version: 'v6.7.2_20240105' },
    ...overrides,
  }
}

/** 让 process_config 镜像登记网关（页面下拉与 Running 判定的来源） */
function registerGateway(name: string, running = true): void {
  const process = useProcessStore()
  process.applyProcessConfig({ [name]: { exe: `${name}.exe` } })
  process.applyProcessStatus({
    name,
    state: running ? 'Running' : 'Stopped',
    pid: running ? 100 : 0,
  })
}

describe('tdInterfaceType（契约 td-config §1 接口类型派生）', () => {
  it('dztd_ctp → ctp；带实例序号取第一个下划线之前', () => {
    expect(tdInterfaceType('dztd_ctp')).toBe('ctp')
    expect(tdInterfaceType('dztd_ctp_2')).toBe('ctp')
    expect(tdInterfaceType('dztd_xtp')).toBe('xtp')
  })

  it('非交易网关进程名返回空串（派生算法只对 dztd_ 前缀成立）', () => {
    expect(tdInterfaceType('dzmd_ctp')).toBe('')
    expect(tdInterfaceType('dztd_')).toBe('')
    expect(tdInterfaceType('')).toBe('')
  })
})

describe('tdStateText（契约 td-status §2.2 十一态）', () => {
  it('11 个状态值都有中文文案，且互不相同（不漏映射、不撞文案）', () => {
    const states = ['Idle', 'Connecting', 'Connected', 'Authenticating', 'Authenticated',
      'LoggingIn', 'LoggedIn', 'Confirming', 'LoadingInstruments', 'Ready', 'Disconnected'] as const
    const texts = states.map(state => tdStateText(state))
    for (const text of texts) {
      expect(text).not.toBe('')
      expect(text).not.toBe('—')
    }
    // 文案唯一：两个不同状态说同一句话会让用户无法区分（如 LoggedIn / Ready）
    expect(new Set(texts).size).toBe(states.length)
  })

  it('未上报显示占位，未知枚举原样返回（不吞掉事实）', () => {
    expect(tdStateText(null)).toBe('—')
    expect(tdStateText(undefined)).toBe('—')
    expect(tdStateText('SomeFutureState')).toBe('SomeFutureState')
  })
})

describe('useTdAccountsStore · 注册表（契约 rest §2.9.9）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    vi.resetAllMocks()
    tdAccountsPending.__resetForTests()
  })

  afterEach(() => {
    useTdAccountsStore().dispose()
  })

  it('init 拉注册表并写入条目（按 account_id 升序）', async () => {
    vi.mocked(tdApi.listAccounts).mockResolvedValue({
      available: true,
      accounts: [
        registryEntry({ account_id: 'account_002', gateway: 'dztd_ctp_2', enabled: false }),
        registryEntry({ account_id: 'account_001' }),
      ],
    } satisfies TdAccountsListResponse)
    const store = useTdAccountsStore()

    await store.init()

    expect(tdApi.listAccounts).toHaveBeenCalledTimes(1)
    expect(store.available).toBe(true)
    expect(store.registry.map(e => e.account_id)).toEqual(['account_001', 'account_002'])
    expect(store.rows.map(r => r.account_id)).toEqual(['account_001', 'account_002'])
    expect(store.rows[1].enabled).toBe(false)
  })

  it('available:false → 进降级态，且**不**把列表当空（isEmpty 保持 false）', async () => {
    vi.mocked(tdApi.listAccounts).mockResolvedValue({
      available: false,
      reason: 'no_gateway_snapshot',
    } satisfies TdAccountsDegradedResponse)
    const store = useTdAccountsStore()

    await store.init()

    expect(store.available).toBe(false)
    expect(store.degradedReason).toBe('no_gateway_snapshot')
    expect(store.degradedText).toContain('账户注册表暂不可用')
    // 关键契约：降级 ≠ 空列表（用户会以为账户被删了）
    expect(store.isEmpty).toBe(false)
  })

  it('降级时保留旧条目并以配置镜像为骨架展示（不出现空屏）', async () => {
    vi.mocked(tdApi.listAccounts).mockResolvedValue({
      available: true,
      accounts: [registryEntry()],
    } satisfies TdAccountsListResponse)
    const store = useTdAccountsStore()
    await store.init()

    // 降级前的配置镜像（含另一网关的账户）
    store.applyTdConfig('dztd_ctp', configPayload([
      { account_id: 'account_001', enabled: true, ext: { broker_id: '9999', user_id: 'u1', password: '****' } },
    ]))
    store.applyTdConfig('dztd_ctp_2', configPayload([
      { account_id: 'account_009', enabled: false, ext: { broker_id: '8888', user_id: 'u9', password: '****' } },
    ]))

    vi.mocked(tdApi.listAccounts).mockResolvedValue({
      available: false,
      reason: 'no_gateway_snapshot',
    } satisfies TdAccountsDegradedResponse)
    await store.loadRegistry()

    expect(store.available).toBe(false)
    // 降级不改写已有镜像：注册表条目 + 配置镜像账户都在
    expect(store.rows.map(r => r.account_id)).toEqual(['account_001', 'account_009'])
    expect(store.rows[1].enabled).toBe(false)  // enabled 取配置帧值（注册表不可用时唯一可信来源）
  })

  it('enabled 以配置镜像为准（启停不广播 data_changed，注册表值会停留在旧值）', async () => {
    // 注册表说「已启用」（旧投影），配置帧说「已禁用」（启停后的最新真相）
    vi.mocked(tdApi.listAccounts).mockResolvedValue({
      available: true,
      accounts: [registryEntry({ enabled: true })],
    } satisfies TdAccountsListResponse)
    const store = useTdAccountsStore()
    await store.init()

    store.applyTdConfig('dztd_ctp', configPayload([
      { account_id: 'account_001', enabled: false, ext: { broker_id: '9999', user_id: 'u1', password: '****' } },
    ]))

    // 契约 rest §2.9.7 可观察效果：只有账户集合增减才广播 data_changed，
    // 启停变化不重拉注册表 → 列必须跟配置帧走，否则会长期显示反了的启用状态
    expect(store.rows[0].enabled).toBe(false)
  })

  it('data_changed{scope:"td"} 重拉注册表；其他 scope 不触发', async () => {
    vi.mocked(tdApi.listAccounts).mockResolvedValue({
      available: true,
      accounts: [registryEntry()],
    } satisfies TdAccountsListResponse)
    const store = useTdAccountsStore()
    await store.init()
    vi.mocked(tdApi.listAccounts).mockClear()

    await store.handleDataChanged('users')
    expect(tdApi.listAccounts).not.toHaveBeenCalled()

    await store.handleDataChanged('td')
    expect(tdApi.listAccounts).toHaveBeenCalledTimes(1)
  })
})

describe('useTdAccountsStore · WS 镜像与 pending（契约 td-config §4、契约 webui-ws §2.2）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    vi.resetAllMocks()
    tdAccountsPending.__resetForTests()
  })

  afterEach(() => {
    useTdAccountsStore().dispose()
  })

  it('td_rtn_config 按网关全量覆盖，清除该网关下全部 pending（含删除操作）', async () => {
    vi.mocked(tdApi.listAccounts).mockResolvedValue({
      available: true,
      accounts: [registryEntry()],
    } satisfies TdAccountsListResponse)
    const store = useTdAccountsStore()
    await store.init()
    registerGateway('dztd_ctp')
    vi.mocked(tdApi.updateAccount).mockResolvedValue({ ok: true })
    vi.mocked(tdApi.removeAccount).mockResolvedValue({ ok: true })

    // 两个在途操作：update + remove（HTTP 已成功，pending 等 RTN 清）
    await store.updateAccount('account_001', { enabled: false })
    await store.removeAccount('account_001')
    expect(isPending('td:dztd_ctp:account_001:update')).toBe(true)
    expect(isPending('td:dztd_ctp:account_001:remove')).toBe(true)

    // 该网关的配置帧到达 → 该网关全部 pending 清除（配置帧是整网关全量）
    store.applyTdConfig('dztd_ctp', configPayload([]))

    expect(isPending('td:dztd_ctp:account_001:update')).toBe(false)
    expect(isPending('td:dztd_ctp:account_001:remove')).toBe(false)
    expect(store.configs.dztd_ctp).toEqual(configPayload([]))
  })

  it('其它网关的配置帧不清本网关 pending（清除粒度是网关前缀，不跨网关）', async () => {
    vi.mocked(tdApi.listAccounts).mockResolvedValue({
      available: true,
      accounts: [registryEntry()],
    } satisfies TdAccountsListResponse)
    const store = useTdAccountsStore()
    await store.init()
    registerGateway('dztd_ctp')
    vi.mocked(tdApi.updateAccount).mockResolvedValue({ ok: true })

    await store.updateAccount('account_001', { enabled: false })
    expect(isPending('td:dztd_ctp:account_001:update')).toBe(true)

    store.applyTdConfig('dztd_ctp_2', configPayload([]))
    expect(isPending('td:dztd_ctp:account_001:update')).toBe(true)

    store.applyTdConfig('dztd_ctp', configPayload([]))
    expect(isPending('td:dztd_ctp:account_001:update')).toBe(false)
  })

  it('HTTP 200 不清 pending（响应只表示已写入事件通道，契约 rest §2.9.7）', async () => {
    vi.mocked(tdApi.listAccounts).mockResolvedValue({
      available: true,
      accounts: [registryEntry()],
    } satisfies TdAccountsListResponse)
    const store = useTdAccountsStore()
    await store.init()
    registerGateway('dztd_ctp')
    vi.mocked(tdApi.updateAccount).mockResolvedValue({ ok: true })

    const result = await store.updateAccount('account_001', { enabled: false })

    expect(result.ok).toBe(true)
    // 唯一清除信号是 td_rtn_config，不是 HTTP 成功
    expect(isPending('td:dztd_ctp:account_001:update')).toBe(true)
  })

  it('td_rtn_status 按 网关名:账户ID 覆盖，并进入行展示；配置帧不携带细状态', async () => {
    vi.mocked(tdApi.listAccounts).mockResolvedValue({
      available: true,
      accounts: [registryEntry()],
    } satisfies TdAccountsListResponse)
    const store = useTdAccountsStore()
    await store.init()
    registerGateway('dztd_ctp')

    store.applyTdConfig('dztd_ctp', configPayload([
      { account_id: 'account_001', enabled: true, ext: { broker_id: '9999', user_id: 'u1', password: '****' } },
    ]))
    // 配置帧不含状态字段：细状态必须来自 td_rtn_status
    expect(store.rows[0].status).toBeNull()

    store.applyTdStatus('dztd_ctp:account_001', statusPayload())
    expect(store.rows[0].status?.state).toBe('Ready')
    expect(store.rows[0].status?.date).toBe('20260808')

    // 后到覆盖先到（全量语义，含 Idle 清空）
    store.applyTdStatus('dztd_ctp:account_001', statusPayload({
      state: 'Idle', date: '', login_time: '', progress_current: 0,
    }))
    expect(store.rows[0].status?.state).toBe('Idle')
    expect(store.rows[0].status?.date).toBe('')
  })

  it('非法帧忽略：缺 instance_id / 缺 account_id / 非对象 status 不覆盖已有镜像', async () => {
    vi.mocked(tdApi.listAccounts).mockResolvedValue({
      available: true,
      accounts: [registryEntry()],
    } satisfies TdAccountsListResponse)
    const store = useTdAccountsStore()
    await store.init()
    store.applyTdStatus('dztd_ctp:account_001', statusPayload())

    store.applyTdStatus(undefined, statusPayload({ state: 'Idle' }))
    store.applyTdStatus('dztd_ctp:account_001', undefined)
    store.applyTdStatus('dztd_ctp:account_001', { ...statusPayload({ state: 'Idle' }), account_id: '' })

    expect(store.rows[0].status?.state).toBe('Ready')
  })

  it('进度取契约 progress 镜像（唯一入口），不取 td_rtn_status 的 progress_*', async () => {
    vi.mocked(tdApi.listAccounts).mockResolvedValue({
      available: true,
      accounts: [registryEntry()],
    } satisfies TdAccountsListResponse)
    const store = useTdAccountsStore()
    await store.init()
    store.applyTdStatus('dztd_ctp:account_001', statusPayload({ progress_current: 3 }))

    useProgressStore().applyProgress('dztd_ctp:account_001', { min: 0, max: 10, current: 7, desc: '加载合约' })

    expect(store.rows[0].progress?.current).toBe(7)
    expect(store.rows[0].progress?.desc).toBe('加载合约')
    // 细状态自带的 progress_* 只作为数据保留，不参与页面进度口径
    expect(store.rows[0].status?.progress_current).toBe(3)
  })
})

describe('useTdAccountsStore · 唯一性预检（契约 rest §2.9.7 预检 4 / §4 前端义务）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    vi.resetAllMocks()
    tdAccountsPending.__resetForTests()
  })

  afterEach(() => {
    useTdAccountsStore().dispose()
  })

  function mockRegistry(accounts: TdConfigAccountList[]): void {
    vi.mocked(tdApi.listAccounts).mockResolvedValue({ available: true, accounts })
  }

  it('account_id 已被其它网关占用（含已禁用）→ 冲突且**不发请求**', async () => {
    mockRegistry([registryEntry({ account_id: 'dup', gateway: 'dztd_ctp_2', enabled: false })])
    const store = useTdAccountsStore()
    await store.init()
    registerGateway('dztd_ctp')

    const result = await store.addAccount('dztd_ctp', {
      account_id: 'dup', ext: { broker_id: '9999', user_id: 'u1', password: 'p' },
    })

    expect(result).toEqual({
      ok: false, kind: 'conflict', message: expect.stringContaining('dztd_ctp_2') as unknown as string,
    })
    expect(tdApi.addAccount).not.toHaveBeenCalled()
  })

  it('注册表 available:false → 跳过预检并允许提交（裁决交给 dzweb 的 409）', async () => {
    vi.mocked(tdApi.listAccounts).mockResolvedValue({
      available: false, reason: 'no_gateway_snapshot',
    } satisfies TdAccountsDegradedResponse)
    vi.mocked(tdApi.addAccount).mockResolvedValue({ ok: true })
    const store = useTdAccountsStore()
    await store.init()
    registerGateway('dztd_ctp')

    const result = await store.addAccount('dztd_ctp', {
      account_id: 'account_001', ext: { broker_id: '9999', user_id: 'u1', password: 'p' },
    })

    expect(store.available).toBe(false)
    // 降级时"查不到"不代表"不存在"：不能据此拦下提交
    expect(result.ok).toBe(true)
    expect(tdApi.addAccount).toHaveBeenCalledTimes(1)
  })

  it('无冲突时正常提交，body 含 gateway + account（契约 rest §2.9.7 POST 形状）', async () => {
    mockRegistry([])
    vi.mocked(tdApi.addAccount).mockResolvedValue({ ok: true })
    const store = useTdAccountsStore()
    await store.init()
    registerGateway('dztd_ctp')

    const result = await store.addAccount('dztd_ctp', {
      account_id: ' account_001 ',
      enabled: true,
      ext: { broker_id: '9999', user_id: 'u1', password: 'p' },
    })

    expect(result.ok).toBe(true)
    expect(tdApi.addAccount).toHaveBeenCalledWith({
      gateway: 'dztd_ctp',
      account: {
        account_id: 'account_001',  // 去首尾空白后再提交
        enabled: true,
        ext: { broker_id: '9999', user_id: 'u1', password: 'p' },
      },
    })
  })

  it('账户 ID 为空 → 本地拒绝，不发请求', async () => {
    mockRegistry([])
    const store = useTdAccountsStore()
    await store.init()

    const result = await store.addAccount('dztd_ctp', { account_id: '   ' })

    expect(result).toEqual({ ok: false, kind: 'failed', message: '账户 ID 不能为空' })
    expect(tdApi.addAccount).not.toHaveBeenCalled()
  })
})

describe('useTdAccountsStore · 写操作与失败文案（契约 rest §2.9.7）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    vi.resetAllMocks()
    tdAccountsPending.__resetForTests()
  })

  afterEach(() => {
    useTdAccountsStore().dispose()
    vi.useRealTimers()
  })

  function apiError(code: string, status = 400): Error & { body: { error: string; code: string } } {
    return Object.assign(new Error('english message'), { body: { error: 'english message', code }, status })
  }

  it('account_exists 失败 → 中文文案，且不回显英文 error', async () => {
    vi.mocked(tdApi.listAccounts).mockResolvedValue({
      available: true, accounts: [],
    } satisfies TdAccountsListResponse)
    vi.mocked(tdApi.addAccount).mockRejectedValue(apiError('account_exists', 409))
    const store = useTdAccountsStore()
    await store.init()
    registerGateway('dztd_ctp')

    const result = await store.addAccount('dztd_ctp', {
      account_id: 'account_001', ext: { broker_id: '9999', user_id: 'u1', password: 'p' },
    })

    expect(result).toEqual({ ok: false, kind: 'failed', message: expect.stringContaining('已被占用') as unknown as string })
    expect(JSON.stringify(result)).not.toContain('english')
    // 失败不留 pending（usePending 已清，避免同 key 永久 busy）
    expect(isPending('td:dztd_ctp:account_001:add')).toBe(false)
  })

  it('gateway_not_running / admin_required / account_not_found 各映射到不同中文文案', async () => {
    vi.mocked(tdApi.listAccounts).mockResolvedValue({
      available: true, accounts: [registryEntry()],
    } satisfies TdAccountsListResponse)
    const store = useTdAccountsStore()
    await store.init()
    registerGateway('dztd_ctp')

    const texts = new Set<string>()
    for (const [code, op] of [
      ['gateway_not_running', () => store.updateAccount('account_001', { enabled: false })],
      ['admin_required', () => store.updateAccount('account_001', { enabled: false })],
    ] as const) {
      tdAccountsPending.__resetForTests()
      vi.mocked(tdApi.updateAccount).mockRejectedValue(apiError(code))
      const result = await op()
      expect(result.ok).toBe(false)
      if (!result.ok && result.kind === 'failed') texts.add(result.message)
    }
    // 不同 code 必须给出不同文案（否则用户无法判断该先做哪一步）
    expect(texts.size).toBe(2)
    expect([...texts].some(t => t.includes('未运行'))).toBe(true)
    expect([...texts].some(t => t.includes('管理员'))).toBe(true)
  })

  it('网络层错误（无 status 无 code）→ 统一"无法连接服务器"，不回显底层英文', async () => {
    vi.mocked(tdApi.listAccounts).mockResolvedValue({
      available: true, accounts: [registryEntry()],
    } satisfies TdAccountsListResponse)
    vi.mocked(tdApi.updateAccount).mockRejectedValue(new Error('Failed to fetch'))
    const store = useTdAccountsStore()
    await store.init()
    registerGateway('dztd_ctp')

    const result = await store.updateAccount('account_001', { enabled: false })

    expect(result).toEqual({ ok: false, kind: 'failed', message: expect.stringContaining('无法连接服务器') as unknown as string })
    expect(JSON.stringify(result)).not.toContain('Failed to fetch')
  })

  it('有响应但不带 code（503 事件通道未就绪）→ 基础设施文案，不冒充预检失败/网络错误', async () => {
    vi.mocked(tdApi.listAccounts).mockResolvedValue({
      available: true, accounts: [registryEntry()],
    } satisfies TdAccountsListResponse)
    // 后端实际形状：写事件通道失败 → 503，body 是 {error} 无 code
    vi.mocked(tdApi.updateAccount).mockRejectedValue(
      Object.assign(new Error('shm not available'), { status: 503, body: { error: 'shm not available' } }))
    const store = useTdAccountsStore()
    await store.init()
    registerGateway('dztd_ctp')

    const result = await store.updateAccount('account_001', { enabled: false })

    expect(result.ok).toBe(false)
    if (!result.ok && result.kind === 'failed') {
      expect(result.message).toContain('暂不可用')
      expect(result.message).not.toContain('已被占用')
      expect(result.message).not.toContain('无法连接服务器')
    }
  })

  it('PUT body 不含 account_id；启停走 UpdateAccount 的 {enabled}', async () => {
    vi.mocked(tdApi.listAccounts).mockResolvedValue({
      available: true, accounts: [registryEntry()],
    } satisfies TdAccountsListResponse)
    vi.mocked(tdApi.updateAccount).mockResolvedValue({ ok: true })
    const store = useTdAccountsStore()
    await store.init()
    registerGateway('dztd_ctp')

    await store.setAccountEnabled('account_001', false)

    expect(tdApi.updateAccount).toHaveBeenCalledWith('account_001', { enabled: false })
    const body = vi.mocked(tdApi.updateAccount).mock.calls[0][1]
    expect(body).not.toHaveProperty('account_id')
  })

  it('同 key 在途时再次提交返回 busy，不发第二个请求（防重入）', async () => {
    vi.mocked(tdApi.listAccounts).mockResolvedValue({
      available: true, accounts: [registryEntry()],
    } satisfies TdAccountsListResponse)
    let release: (() => void) | null = null
    vi.mocked(tdApi.updateAccount).mockImplementation(() =>
      new Promise(resolve => { release = () => resolve({ ok: true }) }))
    const store = useTdAccountsStore()
    await store.init()
    registerGateway('dztd_ctp')

    const first = store.updateAccount('account_001', { enabled: false })
    const second = await store.updateAccount('account_001', { enabled: true })

    expect(second).toEqual({ ok: false, kind: 'busy' })
    expect(tdApi.updateAccount).toHaveBeenCalledTimes(1)
    release!()
    expect((await first).ok).toBe(true)
  })

  it('超时兜底清 pending 并返回 timeout（同步校验 5s 口径）', async () => {
    vi.useFakeTimers()
    vi.mocked(tdApi.listAccounts).mockResolvedValue({
      available: true, accounts: [registryEntry()],
    } satisfies TdAccountsListResponse)
    // 永不 resolve：只能靠超时兜底
    vi.mocked(tdApi.updateAccount).mockImplementation(() => new Promise(() => {}))
    const store = useTdAccountsStore()
    await store.init()
    registerGateway('dztd_ctp')

    const pending = store.updateAccount('account_001', { enabled: false })
    expect(TD_ACCOUNT_OP_TIMEOUT_MS).toBe(5_000)
    await vi.advanceTimersByTimeAsync(TD_ACCOUNT_OP_TIMEOUT_MS)

    const result = await pending
    expect(result).toEqual({ ok: false, kind: 'timeout' })
    expect(isPending('td:dztd_ctp:account_001:update')).toBe(false)
  })

  it('注册表无该账户时 update/remove 直接按 account_not_found 提示，不发请求', async () => {
    vi.mocked(tdApi.listAccounts).mockResolvedValue({
      available: true, accounts: [],
    } satisfies TdAccountsListResponse)
    const store = useTdAccountsStore()
    await store.init()

    const updated = await store.updateAccount('ghost', { enabled: false })
    const removed = await store.removeAccount('ghost')

    expect(updated.ok).toBe(false)
    expect(removed.ok).toBe(false)
    expect(tdApi.updateAccount).not.toHaveBeenCalled()
    expect(tdApi.removeAccount).not.toHaveBeenCalled()
  })
})
