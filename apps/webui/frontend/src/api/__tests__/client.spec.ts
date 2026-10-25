import { describe, it, expect, vi, afterEach } from 'vitest'
import { api, ApiError, shouldRedirectOn401 } from '../client'

// 401 处理的两条回归线：
// 1) 登录接口的 401 是凭据错误，不能清会话（否则输错密码会丢登录态）；
// 2) 已在登录页时不再跳转（否则「请求 401 → 跳 /login → 重载 → 再 401」整页刷新循环）。
// 跳转本身在 jsdom 里不可观测，故决策抽成 shouldRedirectOn401 直接断言。

describe('api client 401 处理', () => {
  afterEach(() => {
    vi.unstubAllGlobals()
    localStorage.clear()
  })

  function stub401(): void {
    vi.stubGlobal('fetch', vi.fn(async () => new Response(
      JSON.stringify({ error: 'unauthorized' }),
      { status: 401, headers: { 'Content-Type': 'application/json' } },
    )))
  }

  it('登录接口 401 不清会话，只把错误交给调用方', async () => {
    localStorage.setItem('jwt_token', 'token-1')
    localStorage.setItem('user_info', '{"username":"admin"}')
    stub401()

    await expect(api.post('/api/login', { username: 'admin', password: 'bad' }))
      .rejects.toBeInstanceOf(ApiError)

    expect(localStorage.getItem('jwt_token')).toBe('token-1')
    expect(localStorage.getItem('user_info')).toBe('{"username":"admin"}')
  })

  it('其他接口 401 清掉会话', async () => {
    // 停在登录页：避免触发 jsdom 未实现的页面跳转（跳转决策另有用例覆盖）
    window.history.replaceState({}, '', '/login')
    localStorage.setItem('jwt_token', 'token-1')
    localStorage.setItem('user_info', '{"username":"admin"}')
    stub401()

    await expect(api.get('/api/user/me')).rejects.toBeInstanceOf(ApiError)

    expect(localStorage.getItem('jwt_token')).toBeNull()
    expect(localStorage.getItem('user_info')).toBeNull()
  })

  it('跳转决策：登录页内不跳转，登录接口的 401 不跳转，其余跳转', () => {
    expect(shouldRedirectOn401('/api/user/me', '/login')).toBe(false)
    expect(shouldRedirectOn401('/api/sysmon/snapshot', '/login')).toBe(false)
    expect(shouldRedirectOn401('/api/login', '/')).toBe(false)
    expect(shouldRedirectOn401('/api/user/me', '/')).toBe(true)
  })
})
