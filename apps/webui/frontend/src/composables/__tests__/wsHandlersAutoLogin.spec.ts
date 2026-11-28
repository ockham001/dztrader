import { describe, it, expect, vi, beforeEach } from 'vitest'
import { setActivePinia, createPinia } from 'pinia'

// 捕获 wsHandlers 注册的处理函数（不启动真实 WS 连接）：本套验证 auto_login 的
// **快照与增量同源分流**（契约 auto-login / 契约 webui-ws §6；#530 M5）。
const registered = new Map<string, (payload: never, instanceId?: string) => void>()

vi.mock('@/composables/wsClient', () => ({
  registerHandler: (type: string, handler: (payload: never, instanceId?: string) => void) => {
    registered.set(type, handler)
  },
  useWebSocket: () => ({ connectionState: { value: 'connected' } }),
}))

// 模块级副作用：import 即注册（与 main.ts 的真实加载路径一致）
await import('@/composables/wsHandlers')
import { useMdConfigStore } from '@/stores/mdConfig'
import { useTdAccountsStore } from '@/stores/tdAccounts'
import type { AutoLoginPayload } from '@/types/mirror'

function dispatch(type: string, payload: unknown, instanceId?: string): void {
  const handler = registered.get(type)
  if (!handler) throw new Error(`no handler registered for ${type}`)
  handler(payload as never, instanceId)
}

const TD_SCHED: AutoLoginPayload = {
  enabled: true,
  schedules: [{ login_time: '08:45', logout_time: '15:45' }],
}
const MD_SCHED: AutoLoginPayload = { enabled: false, schedules: [] }

describe('wsHandlers · auto_login 分流（快照与增量同源；#530 M5）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
  })

  it('snapshot：dztd_* 进 td store、其余进 md store，互不覆盖', () => {
    dispatch('snapshot', {
      dztd_ctp: { auto_login: TD_SCHED },
      dzmd_ctp: { auto_login: MD_SCHED },
    })

    const td = useTdAccountsStore()
    const md = useMdConfigStore()
    expect(td.autoLogins.dztd_ctp).toMatchObject({ enabled: true, source: 'mirror' })
    expect(td.autoLogins.dzmd_ctp).toBeUndefined()
    expect(md.autoLogins.dzmd_ctp).toMatchObject({ enabled: false })
    expect(md.autoLogins.dztd_ctp).toBeUndefined()
  })

  it('增量 auto_login：按 instance_id 前缀分流（与 snapshot 同一条路径）', () => {
    dispatch('auto_login', TD_SCHED, 'dztd_ctp')
    dispatch('auto_login', MD_SCHED, 'dzmd_ctp')

    const td = useTdAccountsStore()
    const md = useMdConfigStore()
    expect(td.autoLogins.dztd_ctp?.enabled).toBe(true)
    expect(md.autoLogins.dzmd_ctp?.enabled).toBe(false)
    expect(td.autoLogins.dzmd_ctp).toBeUndefined()
    expect(md.autoLogins.dztd_ctp).toBeUndefined()
  })

  it('无 instance_id 的增量帧被忽略（不写任何镜像）', () => {
    dispatch('auto_login', TD_SCHED, undefined)
    expect(useTdAccountsStore().autoLogins).toEqual({})
    expect(useMdConfigStore().autoLogins).toEqual({})
  })
})
