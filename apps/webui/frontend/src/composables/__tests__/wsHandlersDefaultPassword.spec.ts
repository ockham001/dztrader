// 默认口令告警的 WS 消费（契约 webui-ws §2.3 / 契约 rest §2.1）。
// #202 核心：告警文案的唯一真相源在后端（payload.message），前端不得硬编码——
// 旧实现忽略 payload 自写文案，改一端必漂移。
import { describe, it, expect, beforeEach, vi } from 'vitest'
import { createPinia, setActivePinia } from 'pinia'

const captured = vi.hoisted(() => new Map<string, (payload: unknown) => void>())

vi.mock('@/composables/wsClient', () => ({
  registerHandler: (type: string, handler: (payload: unknown) => void) => {
    captured.set(type, handler)
  },
  useWebSocket: () => ({ connectionState: { value: 'connected' } }),
}))

await import('@/composables/wsHandlers')
import { useAuthStore } from '@/stores/auth'

describe('default_password_warning（#202 文案单一来源）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
  })

  function dispatch(payload: unknown): void {
    const handler = captured.get('default_password_warning')
    expect(handler, '未注册 default_password_warning 处理器').toBeTruthy()
    handler!(payload)
  }

  it('直接采用后端下发的文案（不拼接、不硬编码）', () => {
    const auth = useAuthStore()
    dispatch({ message: '后端文案：系统正在使用默认管理员密码' })
    expect(auth.defaultPasswordWarning).toBe('后端文案：系统正在使用默认管理员密码')
  })

  it('文案里不出现具体口令值（前端只透传后端文案，不做二次加工）', () => {
    const auth = useAuthStore()
    // 后端常量 kDefaultPasswordWarningMessage 的真实文案形态：只说事实、不含口令值
    const backendText = '系统正在使用默认管理员密码，存在安全风险，请尽快修改密码'
    dispatch({ message: backendText })
    // 双向断言：既是原文透传（不是被改写/截断），又确实不含口令值
    expect(auth.defaultPasswordWarning).toBe(backendText)
    expect(auth.defaultPasswordWarning).not.toContain('88888888')
  })

  it('payload 缺 message / 非字符串时保持无告警（不写假文案）', () => {
    const auth = useAuthStore()
    auth.defaultPasswordWarning = 'previous'
    dispatch({})
    expect(auth.defaultPasswordWarning).toBeNull()
    auth.defaultPasswordWarning = 'previous'
    dispatch({ message: 42 })
    expect(auth.defaultPasswordWarning).toBeNull()
  })
})
