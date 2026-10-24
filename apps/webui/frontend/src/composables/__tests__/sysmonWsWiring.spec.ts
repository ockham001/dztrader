import { describe, it, expect, vi, beforeEach } from 'vitest'
import { setActivePinia, createPinia } from 'pinia'
import type { WsDataByType, WsServerMessageType } from '@/types/ws'
import { SYSMON_NETWORK_CONFIG_KEY, sysmonPending, useSysmonStore } from '@/stores/sysmon'
import { makeSysmonConfig, makeSysmonSnapshot } from '@/test-utils/sysmonSnapshot'

// 捕获 wsHandlers 注册的处理函数（不启动真实 WS 连接）：本套只验证 #77 新增的
// sysmon_config 增量帧与 snapshot 快照域分发都落到 sysmon store 的 applyConfig 上。
const registered = new Map<string, (payload: never, instanceId?: string) => void>()

vi.mock('@/composables/wsClient', () => ({
  registerHandler: (type: string, handler: (payload: never, instanceId?: string) => void) => {
    registered.set(type, handler)
  },
}))

// 模块级副作用：import 即注册（与 main.ts 的真实加载路径一致）
await import('@/composables/wsHandlers')

type Handler<T extends WsServerMessageType> = (payload: WsDataByType[T]) => void

function dispatch<T extends WsServerMessageType>(type: T, payload: WsDataByType[T]): void {
  const handler = registered.get(type)
  if (!handler) throw new Error(`no handler registered for ${type}`)
  handler(payload as never)
}

describe('wsHandlers · sysmon 网络配置（#77）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    sysmonPending.__resetForTests()
  })

  it('sysmon_config 增量帧写入生效配置镜像（hide_virtual 供「显示全部网卡」开关读取）', () => {
    const store = useSysmonStore()
    expect(store.hideVirtual).toBe(true)

    dispatch('sysmon_config', makeSysmonConfig({ network: { hide_virtual: false } }) as WsDataByType['sysmon_config'])

    expect(store.config?.network.hide_virtual).toBe(false)
    expect(store.hideVirtual).toBe(false)
  })

  it('sysmon_config 到达清除「显示全部网卡」pending（REST 只表示已下发，本帧才是生效信号）', () => {
    const store = useSysmonStore()
    sysmonPending.pending[SYSMON_NETWORK_CONFIG_KEY] = true

    dispatch('sysmon_config', makeSysmonConfig({ network: { hide_virtual: false } }) as WsDataByType['sysmon_config'])

    expect(sysmonPending.pending[SYSMON_NETWORK_CONFIG_KEY]).toBe(false)
    expect(store.hideVirtual).toBe(false)
  })

  it('snapshot 快照域分发复用同一 apply：sysmon_config 域恢复开关、sysmon_snapshot 域恢复快照', () => {
    const store = useSysmonStore()
    const handler = registered.get('snapshot') as unknown as Handler<'snapshot'>

    sysmonPending.pending[SYSMON_NETWORK_CONFIG_KEY] = true
    handler({
      dzsysmon: {
        sysmon_config: makeSysmonConfig({ network: { hide_virtual: false } }),
        sysmon_snapshot: makeSysmonSnapshot({ resourceData: true }),
      },
    } as WsDataByType['snapshot'])

    expect(store.hideVirtual).toBe(false)
    expect(sysmonPending.pending[SYSMON_NETWORK_CONFIG_KEY]).toBe(false)
    expect(store.network.map(nic => nic.interface)).toEqual(['eth0'])
  })

  it('sysmon_snapshot 增量帧刷新网卡数组（hide_virtual 过滤后为空数组时 available 仍为 true）', () => {
    const store = useSysmonStore()

    dispatch('sysmon_snapshot', makeSysmonSnapshot({ network: [] }) as WsDataByType['sysmon_snapshot'])

    expect(store.availabilityOf('network')).toEqual({ available: true })
    expect(store.network).toEqual([])
    expect(store.networkFilteredAll).toBe(true)
  })
})
