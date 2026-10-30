import { describe, it, expect, beforeEach, vi } from 'vitest'
import type { MockInstance } from 'vitest'
import { createPinia, setActivePinia } from 'pinia'

// #207 C2：后端 log_tail_unsubscribed 的 reason 必须转成一次性可见提示（旧实现只 console.info）。
// wsHandlers 在 import 时把处理器注册进 wsClient 的注册表单例，因此这里 mock wsClient 的
// registerHandler 把处理器捕获下来，直接按契约载荷调用。
const captured = vi.hoisted(() => new Map<string, (payload: unknown) => void>())

vi.mock('@/composables/wsClient', () => ({
  registerHandler: (type: string, handler: (payload: unknown) => void) => {
    captured.set(type, handler)
  },
  useWebSocket: () => ({ connectionState: { value: 'connected' } }),
}))

// wsHandlers 还会注册其他处理器并 import 大量 store；这里只为 log_tail_unsubscribed 做准备。
await import('@/composables/wsHandlers')
import { useLogsStore } from '@/stores/logs'
import { useToastStore } from '@/stores/toast'
import { LOG_TAIL_SEND_FAILURES_TEXT, SELF_LOG_TAIL_DISABLED_TEXT } from '@/composables/logTailNotice'

describe('log_tail_unsubscribed 的可见提示（#207 C2）', () => {
  let warning: MockInstance

  beforeEach(() => {
    setActivePinia(createPinia())
    warning = vi.spyOn(useToastStore(), 'warning').mockReturnValue(0)
  })

  function dispatch(payload: unknown): void {
    const handler = captured.get('log_tail_unsubscribed')
    expect(handler, '未注册 log_tail_unsubscribed 处理器').toBeTruthy()
    handler!(payload)
  }

  it('self_logs_cannot_be_tailed：关闭实时开关并给出一条解释（不再只写 console）', () => {
    const logs = useLogsStore()
    logs.setTailEnabled(true)

    dispatch({ file: 'logs/dzweb/dzweb.log', reason: 'self_logs_cannot_be_tailed' })

    expect(logs.tailEnabled).toBe(false)
    expect(warning).toHaveBeenCalledWith(SELF_LOG_TAIL_DISABLED_TEXT)
  })

  it('send failures：同样关闭实时开关，提示文案说明是推送失败（不是被拒绝）', () => {
    const logs = useLogsStore()
    logs.setTailEnabled(true)

    dispatch({ file: 'logs/dztraderd/dztraderd.log', reason: 'send failures' })

    expect(logs.tailEnabled).toBe(false)
    expect(warning).toHaveBeenCalledWith(LOG_TAIL_SEND_FAILURES_TEXT)
  })

  it('未知 reason：仍然停止跟随（服务端已退订），但不编造解释', () => {
    const logs = useLogsStore()
    logs.setTailEnabled(true)

    dispatch({ file: 'logs/x.log', reason: 'something_new' })

    expect(logs.tailEnabled).toBe(false)
    expect(warning).not.toHaveBeenCalled()
  })
})
