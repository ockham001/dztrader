import { describe, it, expect, vi, beforeEach } from 'vitest'
import { setActivePinia, createPinia } from 'pinia'
import type { WsDataByType, WsServerMessageType } from '@/types/ws'
import type { TdConfigPayload, TdStatusPayload } from '@/types/api'

/**
 * wsHandlers · 交易账户配置/细状态接线（#251；契约 webui-ws §2.1/§2.2/§2.3）。
 *
 * 固定四件事：
 * - `td_rtn_config` 只写 tdAccounts store 的配置镜像（按 instance_id = 网关进程名）；
 * - `td_rtn_status` 只写细状态镜像（按 instance_id = 网关名:账户ID）；
 * - `data_changed{scope:"td"}` 追加触发账户注册表重拉（契约 rest §2.9.7 可观察效果）；
 * - `snapshot` 的 td_config / td_status 两个新域分发进同一个 store（幂等覆盖）。
 */
const registered = new Map<string, (payload: never, instanceId?: string) => void>()

vi.mock('@/composables/wsClient', () => ({
  registerHandler: (type: string, handler: (payload: never, instanceId?: string) => void) => {
    registered.set(type, handler)
  },
  useWebSocket: () => ({ connectionState: { value: 'connected' } }),
}))

vi.mock('@/api/td', () => ({
  tdApi: {
    getOrders: vi.fn(),
    getTrades: vi.fn(),
    getPositions: vi.fn(),
    getStats: vi.fn(),
    getTradingAccounts: vi.fn(),
    listAccounts: vi.fn(),
    addAccount: vi.fn(),
    updateAccount: vi.fn(),
    removeAccount: vi.fn(),
  },
}))

// 模块级副作用：import 即注册（与 main.ts 的真实加载路径一致）
await import('@/composables/wsHandlers')
import { useTdAccountsStore, tdAccountsPending } from '@/stores/tdAccounts'
import { tdApi } from '@/api/td'

function dispatch<T extends WsServerMessageType>(
  type: T, payload: WsDataByType[T], instanceId?: string,
): void {
  const handler = registered.get(type)
  if (!handler) throw new Error(`no handler registered for ${type}`)
  handler(payload as never, instanceId)
}

function config(accounts: TdConfigPayload['accounts'] = []): TdConfigPayload {
  return {
    qry_account_interval_s: 5,
    qry_position_interval_s: 60,
    qry_flush_interval_ms: 1500,
    accounts,
    ext: { enable_lock_mode: true },
  }
}

function status(overrides: Partial<TdStatusPayload> = {}): TdStatusPayload {
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

describe('wsHandlers · 交易账户配置/细状态（#251）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    vi.resetAllMocks()
    tdAccountsPending.__resetForTests()
  })

  it('两个新 handler 均已注册（漏注册 = 帧到达后镜像永不更新）', () => {
    expect(registered.has('td_rtn_config')).toBe(true)
    expect(registered.has('td_rtn_status')).toBe(true)
  })

  it('td_rtn_config 按 instance_id（网关进程名）写配置镜像，并清该网关 pending', async () => {
    vi.mocked(tdApi.listAccounts).mockResolvedValue({
      available: true,
      accounts: [{ account_id: 'account_001', gateway: 'dztd_ctp', enabled: true }],
    })
    const store = useTdAccountsStore()
    await store.init()
    vi.mocked(tdApi.updateAccount).mockResolvedValue({ ok: true })
    await store.updateAccount('account_001', { enabled: false })
    expect(tdAccountsPending.pending['td:dztd_ctp:account_001:update']).toBe(true)

    dispatch('td_rtn_config', config([
      { account_id: 'account_001', enabled: false, ext: { broker_id: '9999', user_id: 'u1', password: '****' } },
    ]), 'dztd_ctp')

    // 镜像 key 是网关进程名（契约 webui-ws §2.2），不是固定值
    expect(store.configs['dztd_ctp'].accounts[0].account_id).toBe('account_001')
    // 帧到达即该网关 pending 的清除信号
    expect(tdAccountsPending.pending['td:dztd_ctp:account_001:update'] === true).toBe(false)
  })

  it('td_rtn_status 按 instance_id（网关名:账户ID）写细状态镜像，且不影响配置镜像', async () => {
    const store = useTdAccountsStore()

    dispatch('td_rtn_status', status(), 'dztd_ctp:account_001')

    expect(store.statuses['dztd_ctp:account_001']?.state).toBe('Ready')
    expect(store.statuses['dztd_ctp:account_001']?.date).toBe('20260808')
    expect(store.configs).toEqual({})
  })

  it('缺 instance_id 的两类帧被忽略（不写镜像、不抛错）', () => {
    const store = useTdAccountsStore()

    dispatch('td_rtn_config', config([]), undefined)
    dispatch('td_rtn_status', status(), undefined)

    expect(store.configs).toEqual({})
    expect(store.statuses).toEqual({})
  })

  it('data_changed{scope:"td"} 同时重拉账户注册表（契约 rest §2.9.7 可观察效果）', async () => {
    vi.mocked(tdApi.listAccounts).mockResolvedValue({ available: true, accounts: [] })
    const store = useTdAccountsStore()
    await store.init()
    vi.mocked(tdApi.listAccounts).mockClear()
    vi.mocked(tdApi.listAccounts).mockResolvedValue({
      available: true,
      accounts: [{ account_id: 'account_009', gateway: 'dztd_ctp_2', enabled: true }],
    })

    dispatch('data_changed', { scope: 'td' } as never)
    await Promise.resolve()
    await Promise.resolve()

    expect(tdApi.listAccounts).toHaveBeenCalledTimes(1)
    expect(store.registry.map(e => e.account_id)).toEqual(['account_009'])
  })

  it('snapshot 的 td_config / td_status 域分发进同一个 store（幂等覆盖）', async () => {
    const store = useTdAccountsStore()

    dispatch('snapshot', {
      dztd_ctp: {
        td_config: config([
          { account_id: 'account_001', enabled: true, ext: { broker_id: '9999', user_id: 'u1', password: '****' } },
        ]),
      },
      'dztd_ctp:account_001': {
        td_status: status(),
      },
    } as never)

    expect(store.configs['dztd_ctp'].accounts).toHaveLength(1)
    expect(store.statuses['dztd_ctp:account_001'].state).toBe('Ready')
  })
})
