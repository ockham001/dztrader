import { describe, it, expect, vi, beforeEach } from 'vitest'
import { setActivePinia, createPinia } from 'pinia'

// 捕获 wsHandlers 注册的处理函数（不启动真实 WS 连接）：本套只验证
// `data_changed{scope:"td"}` 同时触发委托与统计两条重拉链路（契约 webui-ws §2.3/§5 第 10 条）。
const registered = new Map<string, (payload: never, instanceId?: string) => void>()

vi.mock('@/composables/wsClient', () => ({
  registerHandler: (type: string, handler: (payload: never, instanceId?: string) => void) => {
    registered.set(type, handler)
  },
  useWebSocket: () => ({ connectionState: { value: 'connected' } }),
}))

vi.mock('@/api/td', () => ({
  tdApi: { getOrders: vi.fn(), getStats: vi.fn() },
}))

// 模块级副作用：import 即注册（与 main.ts 的真实加载路径一致）
await import('@/composables/wsHandlers')
import { useTdOrdersStore, __resetTdBatchSchedulerForTests } from '@/stores/tdOrders'
import { useTdStatsStore, __resetTdStatsSchedulerForTests } from '@/stores/tdStats'

function dispatch(payload: unknown): void {
  const handler = registered.get('data_changed')
  if (!handler) throw new Error('no handler registered for data_changed')
  handler(payload as never)
}

describe('wsHandlers · data_changed{scope:"td"} 同时重拉委托与统计（#247 契约 webui-ws §2.3/§5 第 10 条）',
  () => {
    beforeEach(() => {
      setActivePinia(createPinia())
      __resetTdBatchSchedulerForTests()
      __resetTdStatsSchedulerForTests()
      vi.useFakeTimers()
      vi.resetAllMocks()
    })

    it('scope=td：委托 store 与统计 store 各收到一次 handleDataChanged', () => {
      const ordersHandle = vi.spyOn(useTdOrdersStore(), 'handleDataChanged').mockResolvedValue()
      const statsHandle = vi.spyOn(useTdStatsStore(), 'handleDataChanged').mockResolvedValue()

      dispatch({ scope: 'td' })

      expect(ordersHandle).toHaveBeenCalledTimes(1)
      expect(ordersHandle).toHaveBeenCalledWith('td')
      expect(statsHandle).toHaveBeenCalledTimes(1)
      // 统计无 WS 推送（§5 第 10 条）：这条消息只负责「标记失效 + 立即重拉」
      expect(statsHandle).toHaveBeenCalledWith()
      vi.useRealTimers()
    })

    it('其他 scope 不触碰统计（只走原有分支）', () => {
      const statsHandle = vi.spyOn(useTdStatsStore(), 'handleDataChanged').mockResolvedValue()
      const ordersHandle = vi.spyOn(useTdOrdersStore(), 'handleDataChanged').mockResolvedValue()

      dispatch({ scope: 'market_sources' })

      expect(statsHandle).not.toHaveBeenCalled()
      expect(ordersHandle).not.toHaveBeenCalled()
      vi.useRealTimers()
    })

    it('stats 重拉后写入新基线（接线真的到达 store，不只是 spy 被调用）', async () => {
      const { tdApi } = await import('@/api/td')
      vi.mocked(tdApi.getStats).mockResolvedValue({
        available: true as const,
        trading_day: '20260808',
        online_accounts: ['acc_a'],
        stats: { total_orders: 7, total_volume: 9, unfilled_orders: 2, unfilled_volume: 3 },
      })
      const stats = useTdStatsStore()
      // 先初始化，handleDataChanged 在未挂载时不产生流量（store 语义）
      await stats.init()
      vi.mocked(tdApi.getStats).mockResolvedValue({
        available: true as const,
        trading_day: '20260809',
        online_accounts: ['acc_a'],
        stats: { total_orders: 4, total_volume: 5, unfilled_orders: 1, unfilled_volume: 1 },
      })

      dispatch({ scope: 'td' })
      await vi.advanceTimersByTimeAsync(0)

      expect(tdApi.getStats).toHaveBeenCalledTimes(2)
      expect(stats.tradingDay).toBe('20260809')
      expect(stats.values).toEqual({
        total_orders: 4, total_volume: 5, unfilled_orders: 1, unfilled_volume: 1 })
      vi.useRealTimers()
    })
  })
