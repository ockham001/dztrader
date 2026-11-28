import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest'
import { nextTick } from 'vue'
import { setActivePinia, createPinia } from 'pinia'
import {
  useTdAccountsStore,
  tdAccountsPending,
  tdGatewayPending,
  tdInterfaceType,
  tdStateText,
  tdLastErrorText,
  parseTdLastError,
  TD_ACCOUNT_OP_TIMEOUT_MS,
  TD_GATEWAY_OP_TIMEOUT_MS,
  TD_SESSION_LOGIN_TIMEOUT_MS,
  AUTO_LOGIN_PENDING_TIMEOUT_MS,
  tdAutoLoginPending,
} from '../tdAccounts'
import { tdApi } from '@/api/td'
import { tdGatewaysApi } from '@/api/tdGateways'
import { useProgressStore } from '@/stores/progress'
import { useProcessStore } from '@/stores/process'
import { useToastStore } from '@/stores/toast'
import { TD_ACCOUNT_ERROR_TEXT } from '@/composables/tdAccountErrors'
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
    loginAccount: vi.fn(),
    logoutAccount: vi.fn(),
  },
}))

// 交易接口生命周期端点（契约 rest §2.10）：只替身 API，错误文案映射保留真实实现
vi.mock('@/api/tdGateways', async (importOriginal) => {
  const actual = await importOriginal<typeof import('@/api/tdGateways')>()
  return {
    ...actual,
    tdGatewaysApi: {
      listAvailable: vi.fn(),
      start: vi.fn(),
      stop: vi.fn(),
      remove: vi.fn(),
      // #524 auto-login 端点（#530 M6 的 pending 兜底用例用）
      getAutoLogin: vi.fn(),
      setAutoLogin: vi.fn(),
    },
  }
})

/**
 * 交易账户配置 store 用例（#251；契约 rest §2.9.7/§2.9.9、契约 td-config §1/§2/§4/§5、
 * 契约 td-status §2.1/§2.2、契约 webui-ws §2.1/§2.2/§2.3）。
 *
 * 固定这些契约事实：
 * - 注册表是账户列表的唯一视角；`available:false` 是**降级**，不得当空列表；
 * - 配置镜像按网关全量覆盖，到达时清该网关全部 pending（不是「账户消失」判定）；
 * - 细状态/进度按账户 ID（账户级帧 instance_id）覆盖，不从配置镜像推导；
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
  return { account_id: 'account_001', account_name: '', gateway: 'dztd_ctp', enabled: true, ...overrides }
}

function configPayload(accounts: TdConfigPayload['accounts'] = []): TdConfigPayload {
  return {
    qry_min_interval_ms: 1100,
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
    // 显式收口：`Partial<TdStatusPayload>` 的 last_error 是可选，展开后会带 undefined，
    // 而线上契约恒为 `TdLastError | null`（契约 td-status §2.5：缺键与 null 同义）。
    last_error: overrides.last_error ?? null,
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

describe('tdStateText（契约 td-status §2.2 十五态）', () => {
  it('15 个状态值都有中文文案，且互不相同（不漏映射、不撞文案）', () => {
    const states = ['Idle', 'Connecting', 'Connected', 'Authenticating', 'AuthFailed',
      'Authenticated', 'LoggingIn', 'LoginFailed', 'LoggedIn', 'Confirming',
      'SettlementConfirmFailed', 'LoadingInstruments', 'InstrumentsLoadFailed', 'Ready',
      'Disconnected'] as const
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

  it('AuthFailed 有独立文案（与 Authenticating 不同，用户能看出「在认证」还是「认证失败」）', () => {
    expect(tdStateText('AuthFailed')).toBe('认证失败')
    expect(tdStateText('AuthFailed')).not.toBe(tdStateText('Authenticating'))
  })

  it('LoginFailed 有独立文案（与 LoggingIn 不同，用户能看出「在登录」还是「登录失败」）', () => {
    // ADR 0059 决策 1: 登录失败停在 5/10, 界面必须能区分「登录进行中」与「登录失败」——
    // 两者进度同为 5, 只有 state 与文案不同。
    expect(tdStateText('LoginFailed')).toBe('登录失败')
    expect(tdStateText('LoginFailed')).not.toBe(tdStateText('LoggingIn'))
    expect(tdStateText('LoginFailed')).not.toBe(tdStateText('AuthFailed'))
  })

  it('SettlementConfirmFailed / InstrumentsLoadFailed 各自有独立文案（停在 7/10、8/10 的失败态）', () => {
    // #496 / ADR 0059 决策 1: 结算确认与合约加载失败停在各自失败态, 不再回退「已登录」。
    // 界面必须能区分「正在确认/正在加载」与「那一段失败了」—— 各自进度同格, 只有
    // state 与文案不同。
    expect(tdStateText('SettlementConfirmFailed')).toBe('结算单确认失败')
    expect(tdStateText('SettlementConfirmFailed')).not.toBe(tdStateText('Confirming'))
    expect(tdStateText('SettlementConfirmFailed')).not.toBe(tdStateText('LoggedIn'))
    expect(tdStateText('InstrumentsLoadFailed')).toBe('合约加载失败')
    expect(tdStateText('InstrumentsLoadFailed')).not.toBe(tdStateText('LoadingInstruments'))
    expect(tdStateText('InstrumentsLoadFailed')).not.toBe(tdStateText('LoggedIn'))
    // 两个失败态互不相同: 用户要能看出卡在哪一段 (两段恢复动作相同, 根因不同)
    expect(tdStateText('SettlementConfirmFailed')).not.toBe(tdStateText('InstrumentsLoadFailed'))
  })
})

// ============================================================================
// last_error 解析与文案（契约 td-status §2.5 前端义务③）
// ============================================================================

describe('parseTdLastError / tdLastErrorText（契约 td-status §2.5）', () => {
  it('对象且 code 为 string 才采纳；其余字段逐字段防御回落', () => {
    expect(parseTdLastError({
      code: 'auth_rejected', message: 'CTP:用户认证失败', error_id: 3, timestamp: 1_700_000_000,
    })).toEqual({ code: 'auth_rejected', message: 'CTP:用户认证失败', errorId: 3, timestamp: 1_700_000_000 })

    // message 非 string → ''；error_id/timestamp 非有限 number → 0
    expect(parseTdLastError({ code: 'auth_timeout', message: 7, error_id: 'x', timestamp: NaN }))
      .toEqual({ code: 'auth_timeout', message: '', errorId: 0, timestamp: 0 })
  })

  it('null / 非对象 / code 非 string → null（不构成一次失败）', () => {
    expect(parseTdLastError(null)).toBeNull()
    expect(parseTdLastError(undefined)).toBeNull()
    expect(parseTdLastError('auth_rejected')).toBeNull()
    expect(parseTdLastError({ message: 'x' })).toBeNull()
    expect(parseTdLastError({ code: 7, message: 'x', timestamp: 1 })).toBeNull()
  })

  it('message 优先；空则按 code 映射固定中文，绝不展示 snake_case 机器码', () => {
    expect(tdLastErrorText({ code: 'auth_rejected', message: 'CTP:用户认证失败', errorId: 3, timestamp: 1 }))
      .toBe('CTP:用户认证失败')
    expect(tdLastErrorText({ code: 'auth_rejected', message: '', errorId: 0, timestamp: 1 }))
      .toBe('认证被柜台拒绝')
    expect(tdLastErrorText({ code: 'auth_timeout', message: '', errorId: 0, timestamp: 1 }))
      .toBe('认证超时（10 秒内未收到响应）')
    // 未知 code 也不外泄机器码
    expect(tdLastErrorText({ code: 'some_future_code', message: '', errorId: 0, timestamp: 1 }))
      .toBe('交易登录链失败')
    expect(tdLastErrorText(null)).toBe('')
  })

  it('登录段五个码各有中文兜底（契约 td-status §2.5 码表）', () => {
    // 机器码不展示；每种原因说得清是哪一步出错，用户才知道该改什么。
    expect(tdLastErrorText({ code: 'login_rejected', message: '', errorId: 3, timestamp: 1 }))
      .toBe('登录被柜台拒绝')
    expect(tdLastErrorText({ code: 'login_timeout', message: '', errorId: 0, timestamp: 1 }))
      .toBe('登录超时（10 秒内未收到响应）')
    expect(tdLastErrorText({ code: 'login_parse_error', message: '', errorId: 0, timestamp: 1 }))
      .toBe('登录响应解析失败')
    expect(tdLastErrorText({ code: 'day_load_failed', message: '', errorId: 0, timestamp: 1 }))
      .toBe('当日订单/成交基准装载失败')
    expect(tdLastErrorText({ code: 'login_send_failed', message: '', errorId: 0, timestamp: 1 }))
      .toBe('登录请求发送失败')
  })

  it('结算确认 / 合约加载段四码各有中文兜底（契约 td-status §2.5 码表）', () => {
    // #496: 后两段的码补进前端兜底映射 —— 后端 message 为空时用户仍能看到是哪一步出错。
    expect(tdLastErrorText({ code: 'settlement_rejected', message: '', errorId: 3, timestamp: 1 }))
      .toBe('结算单确认被拒')
    expect(tdLastErrorText({ code: 'settlement_timeout', message: '', errorId: 0, timestamp: 1 }))
      .toBe('结算单确认超时（60 秒内未收到响应）')
    expect(tdLastErrorText({ code: 'instruments_rejected', message: '', errorId: 7, timestamp: 1 }))
      .toBe('合约加载被拒')
    expect(tdLastErrorText({ code: 'instruments_timeout', message: '', errorId: 0, timestamp: 1 }))
      .toBe('合约加载超时（5 分钟内未收到最后一批响应）')
  })

  it('connect_timeout 文案说「连接超时」而不是「登录失败」（它不是失败态）', () => {
    // ADR 0059 决策 7: 连接超时回 Idle(未登录), 不是失败态。卡片状态显示「未启动」,
    // 原因行若说「登录失败」会把「从未连上」误导成一登录就失败。
    const text = tdLastErrorText({ code: 'connect_timeout', message: '', errorId: 0, timestamp: 1 })
    expect(text).toBe('连接超时（30 秒内未连接上前置）')
    expect(text).not.toContain('登录失败')
  })
})

describe('useTdAccountsStore · 账户行的 lastError（契约 td-status §2.5）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    vi.resetAllMocks()
    tdAccountsPending.__resetForTests()
    registerGateway('dztd_ctp')
  })

  afterEach(() => {
    useTdAccountsStore().dispose()
  })

  it('status.last_error 解析进 row.lastError；无失败/镜像未到达为 null', async () => {
    const store = useTdAccountsStore()
    vi.mocked(tdApi.listAccounts).mockResolvedValue({
      available: true,
      accounts: [{ account_id: 'account_001', account_name: '', gateway: 'dztd_ctp', enabled: true }],
    })
    await store.init()

    // 镜像未到达：null
    expect(store.rows[0].lastError).toBeNull()

    store.applyTdStatus('account_001', statusPayload({
      state: 'AuthFailed', progress_current: 3, progress_desc: '认证失败', last_error: null,
    }))
    await nextTick()
    expect(store.rows[0].lastError).toBeNull()

    store.applyTdStatus('account_001', statusPayload({
      state: 'AuthFailed', progress_current: 3, progress_desc: '认证失败',
      last_error: { code: 'auth_timeout', message: '', error_id: 0, timestamp: 1_700_000_000 },
    }))
    await nextTick()
    expect(store.rows[0].lastError).toEqual({
      code: 'auth_timeout', message: '', errorId: 0, timestamp: 1_700_000_000,
    })
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

  it('available:false → 进降级态（"无账户"不可信，页面不得当空展示）', async () => {
    vi.mocked(tdApi.listAccounts).mockResolvedValue({
      available: false,
      reason: 'no_gateway_snapshot',
    } satisfies TdAccountsDegradedResponse)
    const store = useTdAccountsStore()

    await store.init()

    expect(store.available).toBe(false)
    expect(store.degradedReason).toBe('no_gateway_snapshot')
    expect(store.degradedText).toContain('账户注册表暂不可用')
    // 关键契约：降级 ≠ 空列表（用户会以为账户被删了）。空态判定已随卡片化上移到页面
    // （接口级空态由进程镜像就绪 + 卡片数决定），store 不再提供 account-level isEmpty。
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
      { account_id: 'account_001', enabled: true, ext: { broker_id: '9999', password: '****' } },
    ]))
    store.applyTdConfig('dztd_ctp_2', configPayload([
      { account_id: 'account_009', enabled: false, ext: { broker_id: '8888', password: '****' } },
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
      { account_id: 'account_001', enabled: false, ext: { broker_id: '9999', password: '****' } },
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

describe('useTdAccountsStore · 账户展示名解析（ADR 0056 决策 9 / 契约 td-config §8）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    vi.resetAllMocks()
    tdAccountsPending.__resetForTests()
  })

  afterEach(() => {
    useTdAccountsStore().dispose()
  })

  it('注册表有名字 → displayName 返回名字；空名/未登记 → 回落 account_id', async () => {
    vi.mocked(tdApi.listAccounts).mockResolvedValue({
      available: true,
      accounts: [
        registryEntry({ account_id: '00001', account_name: '我的主账户' }),
        registryEntry({ account_id: '00002', account_name: '' }),
      ],
    } satisfies TdAccountsListResponse)
    const store = useTdAccountsStore()
    await store.init()

    expect(store.displayName('00001')).toBe('我的主账户')
    expect(store.displayName('00002')).toBe('00002')
    // 解析不到（账户已删 / 未登记）同样回落 id：展示层不得因缺名字失败（ADR 0056 决策 5）
    expect(store.displayName('unknown')).toBe('unknown')
  })

  it('配置镜像覆盖注册表：改名只经 td_rtn_config 到达，注册表缓存要等重拉', async () => {
    vi.mocked(tdApi.listAccounts).mockResolvedValue({
      available: true,
      accounts: [registryEntry({ account_id: '00001', account_name: '旧名字' })],
    } satisfies TdAccountsListResponse)
    const store = useTdAccountsStore()
    await store.init()
    expect(store.displayName('00001')).toBe('旧名字')

    store.applyTdConfig('dztd_ctp', configPayload([
      { account_id: '00001', account_name: '新名字', enabled: true, ext: { broker_id: '9999', password: '****' } },
    ]))

    // 改名不广播 data_changed（契约 webui-ws §2.3 只覆盖账户集合增减），
    // 所以注册表会停留在旧名——解析必须以配置镜像为准，改名才能立刻全站生效（含历史行）
    expect(store.displayName('00001')).toBe('新名字')
  })

  it('帧里名字清空 = 未设置 → 回落 id（不保留注册表旧名）', async () => {
    vi.mocked(tdApi.listAccounts).mockResolvedValue({
      available: true,
      accounts: [registryEntry({ account_id: '00001', account_name: '旧名字' })],
    } satisfies TdAccountsListResponse)
    const store = useTdAccountsStore()
    await store.init()

    store.applyTdConfig('dztd_ctp', configPayload([
      { account_id: '00001', account_name: '', enabled: true, ext: { broker_id: '9999', password: '****' } },
    ]))

    expect(store.displayName('00001')).toBe('00001')
    expect(store.accountName('00001')).toBe('')
  })

  it('注册表降级时仍可从配置镜像解析名字（降级不丢展示名）', async () => {
    // 先让注册表进入降级态（available:false，契约 rest §2.9.9），再到达配置帧
    vi.mocked(tdApi.listAccounts).mockResolvedValue({
      available: false,
      reason: 'no_gateway_snapshot',
    } satisfies TdAccountsDegradedResponse)
    const store = useTdAccountsStore()
    await store.init()
    expect(store.available).toBe(false)

    store.applyTdConfig('dztd_ctp', configPayload([
      { account_id: '00001', account_name: '主账户', enabled: true, ext: { broker_id: '9999', password: '****' } },
    ]))

    // 降级只影响「账户全集是否可信」，不影响已到达配置镜像里的名字（ADR 0056 决策 9）
    expect(store.displayName('00001')).toBe('主账户')
  })

  it('accountName 返回原值（表单回填不回落 id），displayName 去首尾空白且全空白视为未设置', async () => {
    vi.mocked(tdApi.listAccounts).mockResolvedValue({
      available: true,
      accounts: [
        registryEntry({ account_id: '00001', account_name: '  我的主账户  ' }),
        registryEntry({ account_id: '00002', account_name: '   ' }),
      ],
    } satisfies TdAccountsListResponse)
    const store = useTdAccountsStore()
    await store.init()

    // 表单回填必须能区分「没起名字」与「名字恰好等于 id」，因此原值不回落
    expect(store.accountName('00001')).toBe('  我的主账户  ')
    expect(store.displayName('00001')).toBe('我的主账户')
    // 全空白名字 = 契约 §2.2「trim 后为空 = 未设置」
    expect(store.accountName('00002')).toBe('   ')
    expect(store.displayName('00002')).toBe('00002')
  })

  it('名字是纯展示字段：解析结果不参与 gatewayOf / 唯一性预检', async () => {
    vi.mocked(tdApi.listAccounts).mockResolvedValue({
      available: true,
      accounts: [registryEntry({ account_id: '00001', account_name: '主账户' })],
    } satisfies TdAccountsListResponse)
    const store = useTdAccountsStore()
    await store.init()

    // 按名字查不到任何东西：账户身份始终是 account_id（契约 td-config §8）
    expect(store.gatewayOf('主账户')).toBeNull()
    expect(store.accountIdConflict('主账户')).toBeNull()
    expect(store.gatewayOf('00001')).toBe('dztd_ctp')
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

  it('td_rtn_status 按账户 ID（instance_id）覆盖，并进入行展示；配置帧不携带细状态', async () => {
    vi.mocked(tdApi.listAccounts).mockResolvedValue({
      available: true,
      accounts: [registryEntry()],
    } satisfies TdAccountsListResponse)
    const store = useTdAccountsStore()
    await store.init()
    registerGateway('dztd_ctp')

    store.applyTdConfig('dztd_ctp', configPayload([
      { account_id: 'account_001', enabled: true, ext: { broker_id: '9999', password: '****' } },
    ]))
    // 配置帧不含状态字段：细状态必须来自 td_rtn_status
    expect(store.rows[0].status).toBeNull()

    store.applyTdStatus('account_001', statusPayload())
    expect(store.rows[0].status?.state).toBe('Ready')
    expect(store.rows[0].status?.date).toBe('20260808')

    // 后到覆盖先到（全量语义，含 Idle 清空）
    store.applyTdStatus('account_001', statusPayload({
      state: 'Idle', date: '', login_time: '', progress_current: 0,
    }))
    expect(store.rows[0].status?.state).toBe('Idle')
    expect(store.rows[0].status?.date).toBe('')
  })

  it('非法帧忽略：缺 instance_id / 缺 account_id / instance_id 与 payload.account_id 不一致 不覆盖已有镜像', async () => {
    vi.mocked(tdApi.listAccounts).mockResolvedValue({
      available: true,
      accounts: [registryEntry()],
    } satisfies TdAccountsListResponse)
    const store = useTdAccountsStore()
    await store.init()
    store.applyTdStatus('account_001', statusPayload())

    store.applyTdStatus(undefined, statusPayload({ state: 'Idle' }))
    store.applyTdStatus('account_001', undefined)
    store.applyTdStatus('account_001', { ...statusPayload({ state: 'Idle' }), account_id: '' })
    // 自校验（契约 td-status §3：payload.account_id 供消费方定位与自校验）：不一致 = 来源错乱
    store.applyTdStatus('account_002', statusPayload({ account_id: 'account_001', state: 'Idle' }))
    // 旧的复合键实例标识已废弃（ADR 0056 决策 7）：不再被拆键匹配，整帧忽略
    store.applyTdStatus('dztd_ctp:account_001', statusPayload({ state: 'Idle' }))

    expect(store.rows[0].status?.state).toBe('Ready')
  })

  it('进度取契约 progress 镜像（唯一入口），不取 td_rtn_status 的 progress_*', async () => {
    vi.mocked(tdApi.listAccounts).mockResolvedValue({
      available: true,
      accounts: [registryEntry()],
    } satisfies TdAccountsListResponse)
    const store = useTdAccountsStore()
    await store.init()
    store.applyTdStatus('account_001', statusPayload({ progress_current: 3 }))

    useProgressStore().applyProgress('account_001', { min: 0, max: 10, current: 7, desc: '加载合约' })

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
      account_id: 'dup', ext: { broker_id: '9999', password: 'p' },
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
      account_id: 'account_001', ext: { broker_id: '9999', password: 'p' },
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
      ext: { broker_id: '9999', password: 'p' },
    })

    expect(result.ok).toBe(true)
    expect(tdApi.addAccount).toHaveBeenCalledWith({
      gateway: 'dztd_ctp',
      account: {
        account_id: 'account_001',  // 去首尾空白后再提交
        enabled: true,
        ext: { broker_id: '9999', password: 'p' },
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
      account_id: 'account_001', ext: { broker_id: '9999', password: 'p' },
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

describe('useTdAccountsStore · 会话控制（ADR 0051 / 契约 td-connection）', () => {
  const SESSION_KEY = (op: 'login' | 'logout') => `td_sess:dztd_ctp:account_001:${op}`

  beforeEach(async () => {
    setActivePinia(createPinia())
    vi.resetAllMocks()
    tdAccountsPending.__resetForTests()
    vi.mocked(tdApi.listAccounts).mockResolvedValue({
      available: true, accounts: [registryEntry()],
    } satisfies TdAccountsListResponse)
    vi.mocked(tdApi.loginAccount).mockResolvedValue({ ok: true })
    vi.mocked(tdApi.logoutAccount).mockResolvedValue({ ok: true })
    registerGateway('dztd_ctp')
    await useTdAccountsStore().init()
  })

  afterEach(() => {
    useTdAccountsStore().dispose()
    vi.useRealTimers()
  })

  it('登录 pending 由 progress 到 max 清除', async () => {
    const store = useTdAccountsStore()
    const pending = store.login('account_001')
    expect(isPending(SESSION_KEY('login'))).toBe(true)

    useProgressStore().applyProgress('account_001', { min: 0, max: 10, current: 10 })
    await nextTick()

    expect(isPending(SESSION_KEY('login'))).toBe(false)
    expect(await pending).toEqual({ ok: true })
    expect(tdApi.loginAccount).toHaveBeenCalledWith('account_001')
  })

  it('进程级 progress（key = 进程名）不清账户会话 pending；账户级 instance_id = 账户 ID（ADR 0056 决策 7）', async () => {
    const store = useTdAccountsStore()
    const pending = store.login('account_001')
    expect(isPending(SESSION_KEY('login'))).toBe(true)

    // 网关进程自身的进度与账户登录无关：instance_id 是进程名，解析不出账户 → 不清 pending
    useProgressStore().applyProgress('dztd_ctp', { min: 0, max: 10, current: 10 })
    await nextTick()
    expect(isPending(SESSION_KEY('login'))).toBe(true)

    // 账户级进度到 max 才清登录 pending
    useProgressStore().applyProgress('account_001', { min: 0, max: 10, current: 10 })
    await nextTick()
    expect(isPending(SESSION_KEY('login'))).toBe(false)
    await pending
  })

  it('登录在途时点登出：progress 到 min 时登录/登出 pending 都清（不挂兜底超时）', async () => {
    const store = useTdAccountsStore()
    const login = store.login('account_001')
    const logout = store.logout('account_001')
    expect(isPending(SESSION_KEY('login'))).toBe(true)
    expect(isPending(SESSION_KEY('logout'))).toBe(true)

    useProgressStore().applyProgress('account_001', { min: 0, max: 10, current: 0 })
    await nextTick()

    expect(isPending(SESSION_KEY('login'))).toBe(false)
    expect(isPending(SESSION_KEY('logout'))).toBe(false)
    await Promise.all([login, logout])
  })

  it('progress 回退（主动中止）清登录 pending，中间态停留不清', async () => {
    const store = useTdAccountsStore()
    const pending = store.login('account_001')

    // 建立中（5/10）：中间态但未回退 → 保持 pending
    useProgressStore().applyProgress('account_001', { min: 0, max: 10, current: 5 })
    await nextTick()
    expect(isPending(SESSION_KEY('login'))).toBe(true)

    // 回退到 2/10：主动中止（ADR 0059 把回退收窄为登出/前置断线；链内失败停在失败阶段、
    // 不回退，那条路径由上面的 last_error 用例覆盖）
    useProgressStore().applyProgress('account_001', { min: 0, max: 10, current: 2 })
    await nextTick()
    expect(isPending(SESSION_KEY('login'))).toBe(false)
    await pending
  })

  it('配置帧到达不清会话 pending（td: 与 td_sess: 前缀隔离）', async () => {
    const store = useTdAccountsStore()
    const pending = store.login('account_001')

    store.applyTdConfig('dztd_ctp', configPayload([
      {
        account_id: 'account_001', enabled: true, risk_control_enabled: false, currency_id: 'CNY',
        ext: { broker_id: '9999', password: '****' },
      },
    ]))
    // 配置帧的清除粒度只覆盖配置写 pending（td:），不得误清登录 pending（td_sess:）
    expect(isPending(SESSION_KEY('login'))).toBe(true)

    // 收尾：progress 到 min 清掉，避免留下定时器
    useProgressStore().applyProgress('account_001', { min: 0, max: 10, current: 0 })
    await nextTick()
    await pending
  })

  // ===== 链内失败：pending 由 status.last_error.timestamp 清除（契约 td-status §2.5）=====
  // ADR 0059 起链内失败停在失败阶段、current 不回退 —— 进度 watcher 的回退判据在失败路径上
  // 不再触发，本组用例钉住「失败仍能立即清 pending、不等兜底超时」。

  function failedStatus(timestamp: number): TdStatusPayload {
    return statusPayload({
      state: 'AuthFailed',
      progress_desc: '认证失败',
      progress_current: 3,
      login_time: '',
      date: '',
      last_error: { code: 'auth_rejected', message: 'CTP:用户认证失败', error_id: 3, timestamp },
    })
  }

  it('登录 pending 在 last_error 到达时立即清除（进度停在 3/10、不回退）', async () => {
    const store = useTdAccountsStore()
    const pending = store.login('account_001')
    expect(isPending(SESSION_KEY('login'))).toBe(true)

    // 进度停在失败阶段（3/10），不回退：回退判据在此不成立
    useProgressStore().applyProgress('account_001', { min: 0, max: 10, current: 3 })
    await nextTick()
    expect(isPending(SESSION_KEY('login'))).toBe(true)

    // status 帧带 last_error 到达 → 立即清，不等兜底超时
    store.applyTdStatus('account_001', failedStatus(1_700_000_000))
    await nextTick()
    expect(isPending(SESSION_KEY('login'))).toBe(false)
    await pending
  })

  it('last_error.timestamp 变化再次清除（重试后再失败）；同一次失败重复推送不重复清', async () => {
    const store = useTdAccountsStore()
    const first = store.login('account_001')
    store.applyTdStatus('account_001', failedStatus(1_700_000_000))
    await nextTick()
    expect(isPending(SESSION_KEY('login'))).toBe(false)
    await first

    // 用户按恢复路径重新登录（先登出再登录 → 这里直接再发一次登录）
    const second = store.login('account_001')
    expect(isPending(SESSION_KEY('login'))).toBe(true)
    // 同一次失败重复推送（订阅统计等无关字段变化会重推整帧）：timestamp 未变 → 不清新 pending
    store.applyTdStatus('account_001', failedStatus(1_700_000_000))
    await nextTick()
    expect(isPending(SESSION_KEY('login'))).toBe(true)

    // 新的失败（新 timestamp）→ 清
    store.applyTdStatus('account_001', failedStatus(1_700_000_100))
    await nextTick()
    expect(isPending(SESSION_KEY('login'))).toBe(false)
    await second
  })

  it('last_error 变 null（下一次尝试被受理 / 转 Ready）不清 pending', async () => {
    const store = useTdAccountsStore()
    // 先制造一次已见过的失败，使去重记录存在
    store.applyTdStatus('account_001', failedStatus(1_700_000_000))
    await nextTick()

    const pending = store.login('account_001')
    expect(isPending(SESSION_KEY('login'))).toBe(true)
    // 登录尝试被受理 → 网关清空 last_error 并推帧：这不是「一次尝试结束」，不得清 pending
    store.applyTdStatus('account_001', statusPayload({
      state: 'Connecting', progress_desc: '前置连接...', progress_current: 1, last_error: null,
    }))
    await nextTick()
    expect(isPending(SESSION_KEY('login'))).toBe(true)

    // 收尾：progress 到 min 清掉，避免留下定时器
    useProgressStore().applyProgress('account_001', { min: 0, max: 10, current: 0 })
    await nextTick()
    await pending
  })

  it('同一秒内原因种类变化也清 pending（时间戳只有秒级精度）', async () => {
    // 后端重推键与前端去重键都是 code@timestamp（td_status_publish.h / 本 store），
    // 只比 timestamp 会让「同一秒内先拒绝、后超时」既不重推也不清 pending。
    const store = useTdAccountsStore()
    const first = store.login('account_001')
    store.applyTdStatus('account_001', failedStatus(1_700_000_000))
    await nextTick()
    expect(isPending(SESSION_KEY('login'))).toBe(false)
    await first

    const second = store.login('account_001')
    expect(isPending(SESSION_KEY('login'))).toBe(true)
    // 同一秒、不同原因码 → 是一次新的失败，必须再清
    store.applyTdStatus('account_001', statusPayload({
      state: 'AuthFailed', progress_desc: '认证失败', progress_current: 3, login_time: '', date: '',
      last_error: { code: 'auth_timeout', message: '', error_id: 0, timestamp: 1_700_000_000 },
    }))
    await nextTick()
    expect(isPending(SESSION_KEY('login'))).toBe(false)
    await second
  })

  it('last_error.timestamp 非法（<=0）不清 pending（前端义务③：非法时间不构成一次失败）', async () => {
    const store = useTdAccountsStore()
    const pending = store.login('account_001')
    store.applyTdStatus('account_001', failedStatus(0))
    await nextTick()
    expect(isPending(SESSION_KEY('login'))).toBe(true)

    useProgressStore().applyProgress('account_001', { min: 0, max: 10, current: 0 })
    await nextTick()
    await pending
  })

  it('progress 回退仍能清登录 pending（主动中止路径不回归）', async () => {
    // ADR 0059 把回退收窄为「主动中止」（登出 / 前置断线），该路径必须继续有效。
    const store = useTdAccountsStore()
    const pending = store.login('account_001')
    useProgressStore().applyProgress('account_001', { min: 0, max: 10, current: 5 })
    await nextTick()
    expect(isPending(SESSION_KEY('login'))).toBe(true)
    useProgressStore().applyProgress('account_001', { min: 0, max: 10, current: 1 })
    await nextTick()
    expect(isPending(SESSION_KEY('login'))).toBe(false)
    await pending
  })

  it('登录 pending 兜底超时清除（无进展 330s，不是总时长）', async () => {
    vi.useFakeTimers()
    const store = useTdAccountsStore()
    // 永不 resolve：只能靠超时兜底
    vi.mocked(tdApi.loginAccount).mockImplementation(() => new Promise(() => {}))
    const pending = store.login('account_001')
    expect(TD_SESSION_LOGIN_TIMEOUT_MS).toBe(330_000)

    await vi.advanceTimersByTimeAsync(TD_SESSION_LOGIN_TIMEOUT_MS)
    expect(await pending).toEqual({ ok: false, kind: 'timeout' })
    expect(isPending(SESSION_KEY('login'))).toBe(false)
  })

  it('登录 pending 由进度推进续命：慢但活着的登录不误报超时（#508）', async () => {
    vi.useFakeTimers()
    const store = useTdAccountsStore()
    // HTTP 200 立即返回（真实时序）：`keepPendingOnSuccess` 让 pending 保持到 WS 清，
    // 因此这里挂起的是「等进度回报」而非「等 HTTP」——正是线上误报超时的场景。
    vi.mocked(tdApi.loginAccount).mockResolvedValue({ ok: true })
    const pending = store.login('account_001')
    expect(await pending).toEqual({ ok: true })
    expect(isPending(SESSION_KEY('login'))).toBe(true)

    // 每 20 秒推进一格：1→2→…→8（加载合约）。累计 160 秒，远超旧的 45s 绝对超时。
    for (let current = 1; current <= 8; current++) {
      await vi.advanceTimersByTimeAsync(20_000)
      useProgressStore().applyProgress('account_001', { min: 0, max: 10, current })
      await nextTick()
      // 进度推进到 N/10 时 pending 必须仍在（不能误报超时）
      expect(isPending(SESSION_KEY('login')), `进度推进到 ${current}/10 时 pending 必须仍在`).toBe(true)
    }
    // 160 秒过去、全程有推进 → 登录 pending 仍未超时（旧实现到 60s 就被清掉了）
    expect(isPending(SESSION_KEY('login'))).toBe(true)

    // 真正到 Ready → 清 pending（不是超时清的）
    useProgressStore().applyProgress('account_001', { min: 0, max: 10, current: 10 })
    await nextTick()
    expect(isPending(SESSION_KEY('login'))).toBe(false)
  })

  it('进度停滞满 330s 才判超时（后端最长静默段 300s 之后才兜底）', async () => {
    vi.useFakeTimers()
    const toast = useToastStore()
    const errorSpy = vi.spyOn(toast, 'error')
    const store = useTdAccountsStore()
    vi.mocked(tdApi.loginAccount).mockResolvedValue({ ok: true })
    void store.login('account_001')
    await nextTick()
    expect(isPending(SESSION_KEY('login'))).toBe(true)

    // 推进到 8/10 后彻底卡住（模拟流控死循环 / 柜台不回 is_last）
    await vi.advanceTimersByTimeAsync(20_000)
    useProgressStore().applyProgress('account_001', { min: 0, max: 10, current: 8 })
    await nextTick()

    // 从最后一次推进起算：329s 内仍 pending（合约加载段可静默到后端 300s 看门狗）
    await vi.advanceTimersByTimeAsync(329_000)
    expect(isPending(SESSION_KEY('login'))).toBe(true)
    // 超过 330s 无推进 → 兜底超时清 pending 并提示（后端看门狗也失灵时的最后一道信号）
    await vi.advanceTimersByTimeAsync(2_000)
    expect(isPending(SESSION_KEY('login'))).toBe(false)
    expect(errorSpy).toHaveBeenCalledWith('登录交易账户超时')
  })

  it('sessionPending：登出 pending 由 progress 到 min 清除', async () => {
    const store = useTdAccountsStore()
    const pending = store.logout('account_001')
    expect(store.sessionPending('account_001', 'logout')).toBe(true)
    expect(store.sessionPending('account_001', 'login')).toBe(false)

    useProgressStore().applyProgress('account_001', { min: 0, max: 10, current: 0 })
    await nextTick()
    expect(store.sessionPending('account_001', 'logout')).toBe(false)
    await pending
  })

  it('登录失败按 code 映射中文并清 pending（不回显英文 error）', async () => {
    const store = useTdAccountsStore()
    vi.mocked(tdApi.loginAccount).mockRejectedValue({
      status: 409, body: { error: 'td gateway not running', code: 'gateway_not_running' },
    })
    const result = await store.login('account_001')
    expect(result).toEqual({
      ok: false, kind: 'failed', message: TD_ACCOUNT_ERROR_TEXT.gateway_not_running,
    })
    expect(isPending(SESSION_KEY('login'))).toBe(false)
  })
})

describe('useTdAccountsStore · 交易接口卡片派生（#439 / ADR 0053 / 契约 rest §4）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    vi.resetAllMocks()
    tdAccountsPending.__resetForTests()
  })

  afterEach(() => {
    useTdAccountsStore().dispose()
  })

  it('按 process 镜像派生卡片：display_name / 进程状态 / 账户分组 / 排序', async () => {
    const process = useProcessStore()
    process.applyProcessConfig({
      dztd_ctp: { exe: 'dztd_ctp.exe', display_name: 'CTP 交易接口' },
      dztd_xtp: { exe: 'dztd_xtp.exe', display_name: 'XTP 交易接口' },
    })
    process.applyProcessStatus({ name: 'dztd_ctp', state: 'Running', pid: 100 })
    process.applyProcessStatus({ name: 'dztd_xtp', state: 'Crashed', pid: 0 })
    vi.mocked(tdApi.listAccounts).mockResolvedValue({
      available: true,
      accounts: [
        registryEntry({ account_id: 'account_002', gateway: 'dztd_ctp' }),
        registryEntry({ account_id: 'account_001', gateway: 'dztd_ctp' }),
        registryEntry({ account_id: 'account_009', gateway: 'dztd_xtp' }),
      ],
    } satisfies TdAccountsListResponse)
    const store = useTdAccountsStore()
    await store.init()

    expect(store.interfaces.map(i => i.name)).toEqual(['dztd_ctp', 'dztd_xtp'])
    const ctp = store.interfaces[0]
    expect(ctp.displayName).toBe('CTP 交易接口')
    expect(ctp.state).toBe('Running')
    expect(ctp.running).toBe(true)
    expect(ctp.accountCount).toBe(2)
    expect(ctp.enabledCount).toBe(2)
    // 账户行按 account_id 升序、只含本接口账户
    expect(ctp.accounts.map(row => row.account_id)).toEqual(['account_001', 'account_002'])
    expect(ctp.accounts[0].gatewayRunning).toBe(true)

    const xtp = store.interfaces[1]
    expect(xtp.running).toBe(false)
    expect(xtp.state).toBe('Crashed')
    expect(xtp.accounts[0].gatewayRunning).toBe(false)
  })

  it('接口可零账户：账户注册表为空也照样出卡片（ADR 0053 决策 2）', async () => {
    const process = useProcessStore()
    process.applyProcessConfig({ dztd_ctp: { exe: 'dztd_ctp.exe', display_name: 'CTP 交易接口' } })
    process.applyProcessStatus({ name: 'dztd_ctp', state: 'Running', pid: 100 })
    vi.mocked(tdApi.listAccounts).mockResolvedValue({ available: true, accounts: [] } satisfies TdAccountsListResponse)
    const store = useTdAccountsStore()
    await store.init()

    expect(store.interfaces).toHaveLength(1)
    expect(store.interfaces[0].accountCount).toBe(0)
    expect(store.interfaces[0].running).toBe(true)
  })

  it('display_name：配置镜像优先于状态帧；两者都缺失回退进程名（契约 process）', async () => {
    const process = useProcessStore()
    process.applyProcessConfig({ dztd_ctp: { exe: 'dztd_ctp.exe', display_name: '配置显示名' } })
    process.applyProcessStatus({
      name: 'dztd_ctp', state: 'Running', pid: 1, display_name: '状态帧显示名',
    })
    vi.mocked(tdApi.listAccounts).mockResolvedValue({ available: true, accounts: [] } satisfies TdAccountsListResponse)
    const store = useTdAccountsStore()
    await store.init()

    // 契约 process：配置镜像为唯一真相源，状态帧 display_name 仅用于展示
    expect(store.interfaces[0].displayName).toBe('配置显示名')
  })

  it('process_status 先于 process_config 到达（快照时序）也不丢卡片，配置到达后以配置为准', async () => {
    const process = useProcessStore()
    // status 先到（configsInitialized 尚为 false，process store 放行写入）
    process.applyProcessStatus({
      name: 'dztd_ctp', state: 'Starting', pid: 0, display_name: 'CTP 交易接口',
    })
    vi.mocked(tdApi.listAccounts).mockResolvedValue({ available: true, accounts: [] } satisfies TdAccountsListResponse)
    const store = useTdAccountsStore()
    await store.init()

    expect(store.interfaces.map(i => i.name)).toEqual(['dztd_ctp'])
    // 竞态窗口内用状态帧的 display_name 兜底展示，避免闪一下进程名
    expect(store.interfaces[0].displayName).toBe('CTP 交易接口')
    // 配置帧到达后显示名以配置为准
    process.applyProcessConfig({ dztd_ctp: { exe: 'dztd_ctp.exe', display_name: '配置显示名' } })
    await nextTick()
    expect(store.interfaces[0].displayName).toBe('配置显示名')
  })

  it('display_name 两个镜像都缺失时回退进程名（契约 process）', async () => {
    const process = useProcessStore()
    process.applyProcessConfig({ dztd_ctp: { exe: 'dztd_ctp.exe' } })
    process.applyProcessStatus({ name: 'dztd_ctp', state: 'Running', pid: 1 })
    vi.mocked(tdApi.listAccounts).mockResolvedValue({ available: true, accounts: [] } satisfies TdAccountsListResponse)
    const store = useTdAccountsStore()
    await store.init()

    expect(store.interfaces[0].displayName).toBe('dztd_ctp')
  })
})

describe('useTdAccountsStore · 交易接口生命周期（契约 rest §2.10 / ADR 0053）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    vi.resetAllMocks()
    tdAccountsPending.__resetForTests()
    tdGatewayPending.__resetForTests()
    vi.mocked(tdApi.listAccounts).mockResolvedValue({ available: true, accounts: [] } satisfies TdAccountsListResponse)
  })

  afterEach(() => {
    useTdAccountsStore().dispose()
    vi.useRealTimers()
  })

  function apiError(code: string, status = 400): Error & { body: { error: string; code: string }; status: number } {
    return Object.assign(new Error('english message'), {
      body: { error: 'english message', code }, status,
    })
  }

  it('refreshAvailableGateways 写入可用列表（添加下拉数据源）', async () => {
    vi.mocked(tdGatewaysApi.listAvailable).mockResolvedValue([
      { name: 'dztd_ctp', display_name: 'CTP 交易接口', added: false, running: false },
    ])
    const store = useTdAccountsStore()

    await store.refreshAvailableGateways()

    expect(store.gatewayAvailable).toHaveLength(1)
    expect(store.gatewayAvailable[0].display_name).toBe('CTP 交易接口')
    expect(store.gatewayAvailableLoading).toBe(false)
  })

  it('添加接口 = start + display_name；pending 由 process_config 条目出现清除', async () => {
    vi.mocked(tdGatewaysApi.start).mockResolvedValue({ ok: true })
    const store = useTdAccountsStore()

    const result = await store.addGateway('dztd_ctp', ' CTP 交易接口 ')

    expect(result).toEqual({ ok: true })
    expect(tdGatewaysApi.start).toHaveBeenCalledWith('dztd_ctp', { display_name: 'CTP 交易接口' })
    // HTTP 200 不清 pending（契约 rest §2.10：生效信号是 WS 进程镜像）
    expect(store.gatewayStartPending('dztd_ctp')).toBe(true)

    useProcessStore().applyProcessConfig({ dztd_ctp: { exe: 'dztd_ctp.exe', display_name: 'CTP 交易接口' } })
    await nextTick()

    expect(store.gatewayStartPending('dztd_ctp')).toBe(false)
    expect(store.interfaces.map(i => i.name)).toEqual(['dztd_ctp'])
  })

  it('恢复接口 = start 无 body；pending 由 process_status 到 Running 清除', async () => {
    const process = useProcessStore()
    process.applyProcessConfig({ dztd_ctp: { exe: 'dztd_ctp.exe' } })
    process.applyProcessStatus({ name: 'dztd_ctp', state: 'Crashed', pid: 0 })
    vi.mocked(tdGatewaysApi.start).mockResolvedValue({ ok: true })
    const store = useTdAccountsStore()

    const result = await store.startGateway('dztd_ctp')

    expect(result).toEqual({ ok: true })
    expect(tdGatewaysApi.start).toHaveBeenCalledWith('dztd_ctp')
    expect(store.gatewayStartPending('dztd_ctp')).toBe(true)

    // 无关配置帧（该接口仍在配置中）不得提前清掉在途 start：只认 未注册 → 已注册 的迁移
    process.applyProcessConfig({ dztd_ctp: { exe: 'dztd_ctp.exe', display_name: '改名' } })
    await nextTick()
    expect(store.gatewayStartPending('dztd_ctp')).toBe(true)

    process.applyProcessStatus({ name: 'dztd_ctp', state: 'Running', pid: 100 })
    await nextTick()

    expect(store.gatewayStartPending('dztd_ctp')).toBe(false)
    expect(store.interfaces[0].running).toBe(true)
  })

  it('start 失败事件（StartFailed）清 pending（失败反馈由 NOTIFY_UI 承载，不额外抛错）', async () => {
    const process = useProcessStore()
    process.applyProcessConfig({ dztd_ctp: { exe: 'dztd_ctp.exe' } })
    process.applyProcessStatus({ name: 'dztd_ctp', state: 'Crashed', pid: 0 })
    vi.mocked(tdGatewaysApi.start).mockResolvedValue({ ok: true })
    const store = useTdAccountsStore()

    await store.startGateway('dztd_ctp')
    expect(store.gatewayStartPending('dztd_ctp')).toBe(true)

    process.applyProcessStatus({
      name: 'dztd_ctp', state: 'Crashed', pid: 0, event: 'StartFailed',
    })
    await nextTick()

    expect(store.gatewayStartPending('dztd_ctp')).toBe(false)
  })

  it('移除接口 = DELETE；pending 由 process_config 条目消失（权威信号）清除', async () => {
    const process = useProcessStore()
    process.applyProcessConfig({ dztd_ctp: { exe: 'dztd_ctp.exe' } })
    process.applyProcessStatus({ name: 'dztd_ctp', state: 'Running', pid: 100 })
    vi.mocked(tdGatewaysApi.remove).mockResolvedValue({ ok: true })
    const store = useTdAccountsStore()

    const result = await store.removeGateway('dztd_ctp')

    expect(result).toEqual({ ok: true })
    expect(tdGatewaysApi.remove).toHaveBeenCalledWith('dztd_ctp')
    expect(store.gatewayRemovePending('dztd_ctp')).toBe(true)

    // 条目仍在配置里时，任何配置帧都不算「移除完成」（只认 已注册 → 未注册 的迁移）
    process.applyProcessConfig({
      dztd_ctp: { exe: 'dztd_ctp.exe' }, dztd_xtp: { exe: 'dztd_xtp.exe' },
    })
    await nextTick()
    expect(store.gatewayRemovePending('dztd_ctp')).toBe(true)

    // 契约 process：process_config 条目消失 = 移除完成
    process.applyProcessConfig({})
    await nextTick()

    expect(store.gatewayRemovePending('dztd_ctp')).toBe(false)
    expect(store.interfaces).toHaveLength(0)
  })

  it('RemoveFailed 也清 pending（不留 30s 悬挂）', async () => {
    const process = useProcessStore()
    process.applyProcessConfig({ dztd_ctp: { exe: 'dztd_ctp.exe' } })
    process.applyProcessStatus({ name: 'dztd_ctp', state: 'Running', pid: 100 })
    vi.mocked(tdGatewaysApi.remove).mockResolvedValue({ ok: true })
    const store = useTdAccountsStore()

    await store.removeGateway('dztd_ctp')
    process.applyProcessStatus({
      name: 'dztd_ctp', state: 'Running', pid: 100, event: 'RemoveFailed',
    })
    await nextTick()

    expect(store.gatewayRemovePending('dztd_ctp')).toBe(false)
    // 失败不清配置条目：卡片仍在
    expect(store.interfaces).toHaveLength(1)
  })

  it('失败文案按 code 映射中文，绝不回显英文 error', async () => {
    vi.mocked(tdGatewaysApi.remove).mockRejectedValue(apiError('gateway_has_accounts', 409))
    const store = useTdAccountsStore()

    const result = await store.removeGateway('dztd_ctp')

    expect(result.ok).toBe(false)
    if (!result.ok && result.kind === 'failed') {
      expect(result.message).toContain('先删除全部账户')
    }
    expect(JSON.stringify(result)).not.toContain('english')
    expect(store.gatewayRemovePending('dztd_ctp')).toBe(false)
  })

  it('invalid_gateway / gateway_not_found 各映射到不同中文文案', async () => {
    const store = useTdAccountsStore()
    const texts = new Set<string>()
    for (const code of ['invalid_gateway', 'gateway_not_found'] as const) {
      tdGatewayPending.__resetForTests()
      vi.mocked(tdGatewaysApi.start).mockRejectedValue(apiError(code))
      const result = await store.startGateway('dztd_ctp')
      expect(result.ok).toBe(false)
      if (!result.ok && result.kind === 'failed') texts.add(result.message)
    }
    expect(texts.size).toBe(2)
    expect([...texts].some(t => t.includes('不是可用的交易接口'))).toBe(true)
    expect([...texts].some(t => t.includes('不存在'))).toBe(true)
  })

  it('503（事件通道未就绪，无 code）→ 基础设施文案，不冒充网络/预检失败', async () => {
    vi.mocked(tdGatewaysApi.start).mockRejectedValue(
      Object.assign(new Error('shm writer not available'), { status: 503, body: { error: 'shm writer not available' } }))
    const store = useTdAccountsStore()

    const result = await store.startGateway('dztd_ctp')

    expect(result.ok).toBe(false)
    if (!result.ok && result.kind === 'failed') {
      expect(result.message).toContain('暂不可用')
      expect(result.message).not.toContain('无法连接服务器')
      expect(result.message).not.toContain('不是可用的交易接口')
    }
  })

  it('网络层错误 → 统一无法连接服务器文案', async () => {
    vi.mocked(tdGatewaysApi.start).mockRejectedValue(new Error('Failed to fetch'))
    const store = useTdAccountsStore()

    const result = await store.startGateway('dztd_ctp')

    expect(result.ok).toBe(false)
    if (!result.ok && result.kind === 'failed') {
      expect(result.message).toContain('无法连接服务器')
      expect(result.message).not.toContain('Failed to fetch')
    }
  })

  it('非法进程名本地拒绝（不发出必然 400 的请求）', async () => {
    const store = useTdAccountsStore()

    const result = await store.addGateway('dzmd_ctp', 'x')

    expect(result.ok).toBe(false)
    if (!result.ok && result.kind === 'failed') {
      expect(result.message).toContain('不是可用的交易接口')
    }
    expect(tdGatewaysApi.start).not.toHaveBeenCalled()
  })

  it('同接口在途时再次提交返回 busy（防重入），只发一个请求', async () => {
    let release: (() => void) | null = null
    vi.mocked(tdGatewaysApi.start).mockImplementation(() =>
      new Promise(resolve => { release = () => resolve({ ok: true }) }))
    const store = useTdAccountsStore()

    const first = store.startGateway('dztd_ctp')
    const second = await store.startGateway('dztd_ctp')

    expect(second).toEqual({ ok: false, kind: 'busy' })
    expect(tdGatewaysApi.start).toHaveBeenCalledTimes(1)
    release!()
    expect((await first).ok).toBe(true)
  })

  it('超时兜底清 pending 并返回 timeout（30s 进程操作口径）', async () => {
    vi.useFakeTimers()
    vi.mocked(tdGatewaysApi.start).mockImplementation(() => new Promise(() => {}))
    const store = useTdAccountsStore()

    const pending = store.startGateway('dztd_ctp')
    expect(TD_GATEWAY_OP_TIMEOUT_MS).toBe(30_000)
    await vi.advanceTimersByTimeAsync(TD_GATEWAY_OP_TIMEOUT_MS)

    expect(await pending).toEqual({ ok: false, kind: 'timeout' })
    expect(store.gatewayStartPending('dztd_ctp')).toBe(false)
  })

  it('refreshAvailableGateways 失败写 error（中文、无英文），不清空已有列表', async () => {
    vi.mocked(tdGatewaysApi.listAvailable).mockResolvedValueOnce([
      { name: 'dztd_ctp', display_name: 'CTP 交易接口', added: true, running: true },
    ])
    const store = useTdAccountsStore()
    await store.refreshAvailableGateways()

    vi.mocked(tdGatewaysApi.listAvailable).mockRejectedValue(new Error('Failed to fetch'))
    await store.refreshAvailableGateways()

    expect(store.error).toContain('无法连接服务器')
    expect(store.error).not.toContain('Failed to fetch')
    expect(store.gatewayAvailable).toHaveLength(1)
    expect(store.gatewayAvailableLoading).toBe(false)
    store.clearError()
  })
})

describe('useTdAccountsStore · 添加账户 pending 按网关隔离（#439 审查收敛）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    vi.resetAllMocks()
    tdAccountsPending.__resetForTests()
    vi.mocked(tdApi.listAccounts).mockResolvedValue({ available: true, accounts: [] } satisfies TdAccountsListResponse)
  })

  afterEach(() => {
    useTdAccountsStore().dispose()
  })

  it('addPendingFor 只看本网关：一台接口添加中不锁另一张卡片，配置帧按网关清除', async () => {
    registerGateway('dztd_ctp')
    registerGateway('dztd_ctp_2')
    vi.mocked(tdApi.addAccount).mockResolvedValue({ ok: true })
    const store = useTdAccountsStore()
    await store.init()

    await store.addAccount('dztd_ctp', { account_id: 'account_a1' })
    expect(store.addPendingFor('dztd_ctp')).toBe(true)
    expect(store.addPendingFor('dztd_ctp_2')).toBe(false)

    await store.addAccount('dztd_ctp_2', { account_id: 'account_a2' })
    expect(store.addPendingFor('dztd_ctp_2')).toBe(true)

    // 配置帧是整网关全量：只清自己网关的 pending
    store.applyTdConfig('dztd_ctp', configPayload([]))
    expect(store.addPendingFor('dztd_ctp')).toBe(false)
    expect(store.addPendingFor('dztd_ctp_2')).toBe(true)
  })
})

// #530 M6：排程写操作是非乐观的（pending 等 RTN），必须给兜底超时；
// 否则 RTN 丢失 / 网关停机时开关与增删按钮永久禁用、变更静默丢失（md 侧同能力有超时）。
describe('useTdAccountsStore · 自动登录排程 pending 兜底（#530 M6）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    vi.resetAllMocks()
    vi.useFakeTimers()
    tdAutoLoginPending.__resetForTests()
  })

  afterEach(() => {
    tdAutoLoginPending.__resetForTests()
    vi.useRealTimers()
    useTdAccountsStore().dispose()
  })

  it('RTN 到达：清 pending 与兜底定时器，超时不再误报错误', async () => {
    vi.mocked(tdGatewaysApi.setAutoLogin).mockResolvedValue({ ok: true })
    const store = useTdAccountsStore()
    const pending = store.setAutoLogin('dztd_ctp', { enabled: false, schedules: [] })
    expect(store.autoLoginPending.has('dztd_ctp')).toBe(true)

    store.applyAutoLogin('dztd_ctp', { enabled: false, schedules: [] })
    await pending
    expect(store.autoLoginPending.has('dztd_ctp')).toBe(false)

    // 定时器必须被真正 clearTimeout（而不只是回调里早退）：越过原超时点后不得再弹超时提示。
    const toast = useToastStore()
    const errSpy = vi.spyOn(toast, 'error')
    vi.advanceTimersByTime(AUTO_LOGIN_PENDING_TIMEOUT_MS + 1)
    expect(errSpy).not.toHaveBeenCalled()
    expect(store.error).toBeNull()
  })

  it('RTN 丢失：10s 超时后清 pending 并提示（控件不会永久锁死）', async () => {
    vi.mocked(tdGatewaysApi.setAutoLogin).mockResolvedValue({ ok: true })
    const store = useTdAccountsStore()
    const toast = useToastStore()
    const errSpy = vi.spyOn(toast, 'error')
    await store.setAutoLogin('dztd_ctp', { enabled: false, schedules: [] })
    expect(store.autoLoginPending.has('dztd_ctp')).toBe(true)

    vi.advanceTimersByTime(AUTO_LOGIN_PENDING_TIMEOUT_MS)
    expect(store.autoLoginPending.has('dztd_ctp')).toBe(false)
    // usePending 统一出超时提示（与 md 侧同口径）
    expect(errSpy).toHaveBeenCalledWith('排程保存超时')
  })

  it('dispose 清掉排程 pending 定时器（视图卸载不残留）', async () => {
    vi.mocked(tdGatewaysApi.setAutoLogin).mockResolvedValue({ ok: true })
    const store = useTdAccountsStore()
    await store.setAutoLogin('dztd_ctp', { enabled: false, schedules: [] })
    expect(store.autoLoginPending.has('dztd_ctp')).toBe(true)

    store.dispose()
    expect(store.autoLoginPending.has('dztd_ctp')).toBe(false)
  })

  it('HTTP 失败：立即清 pending（不等到兜底超时）', async () => {
    vi.mocked(tdGatewaysApi.setAutoLogin).mockRejectedValue(new Error('boom'))
    const store = useTdAccountsStore()
    const ok = await store.setAutoLogin('dztd_ctp', { enabled: false, schedules: [] })
    expect(ok).toBe(false)
    expect(store.autoLoginPending.has('dztd_ctp')).toBe(false)
  })

  it('过期 GET 的 unavailable 不覆盖已有 mirror 镜像（并发守卫）', async () => {
    const store = useTdAccountsStore()
    store.applyAutoLogin('dztd_ctp', {
      enabled: true,
      schedules: [{ login_time: '08:45', logout_time: '15:45' }],
    })
    vi.mocked(tdGatewaysApi.getAutoLogin).mockResolvedValue({
      enabled: false,
      schedules: [],
      source: 'unavailable',
    })

    await store.loadAutoLogin('dztd_ctp')
    expect(store.autoLogins.dztd_ctp?.source).toBe('mirror')
    expect(store.autoLogins.dztd_ctp?.schedules).toHaveLength(1)
  })
})
