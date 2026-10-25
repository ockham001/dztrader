import { describe, it, expect, vi } from 'vitest'
import { mountWhenRouterReady } from '../bootstrap'

// issue #118 根因 1 的回归：路由就绪前 mount 会先渲染 AppShell → 未登录时触发 401 →
// 整页跳转 → 重载 → 再次先渲染 AppShell，形成整页刷新循环。

function deferred(): { promise: Promise<void>; resolve: () => void; reject: (e: unknown) => void } {
  let resolve!: () => void
  let reject!: (e: unknown) => void
  const promise = new Promise<void>((res, rej) => {
    resolve = res
    reject = rej
  })
  return { promise, resolve, reject }
}

describe('mountWhenRouterReady', () => {
  it('路由就绪前不挂载，就绪后挂载 #app', async () => {
    const ready = deferred()
    const app = { mount: vi.fn() }
    const router = { isReady: () => ready.promise }

    const done = mountWhenRouterReady(app, router)
    await Promise.resolve()
    expect(app.mount).not.toHaveBeenCalled()

    ready.resolve()
    await done
    expect(app.mount).toHaveBeenCalledWith('#app')
  })

  it('初次导航失败也挂载，避免白屏与未处理 rejection', async () => {
    const app = { mount: vi.fn() }
    const router = { isReady: () => Promise.reject(new Error('chunk load failed')) }

    await mountWhenRouterReady(app, router)

    expect(app.mount).toHaveBeenCalledWith('#app')
  })
})
