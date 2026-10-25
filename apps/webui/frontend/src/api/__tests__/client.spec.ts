import { describe, it, expect, vi, afterEach } from 'vitest'
import { api, ApiError, isOnLoginPage, redirectToLogin } from '../client'

// 401 处理的三条回归线（issue #118）：
// 1) 登录接口的 401 是凭据错误：不清会话、不跳转，且响应体原样交给表单（否则只会显示 unauthorized）；
// 2) 其他接口 401 清会话；
// 3) 跳转收口在 redirectToLogin：已在登录页时 no-op（否则「401 → 跳 /login → 重载 → 再 401」整页刷新循环）。

describe('api client 401 处理', () => {
  afterEach(() => {
    vi.unstubAllGlobals()
    localStorage.clear()
    window.history.replaceState({}, '', '/')
  })

  function stub401(body: unknown = { error: 'unauthorized' }): void {
    vi.stubGlobal('fetch', vi.fn(async () => new Response(
      JSON.stringify(body),
      { status: 401, headers: { 'Content-Type': 'application/json' } },
    )))
  }

  it('登录接口 401 不清会话，并把响应体原样交给调用方', async () => {
    localStorage.setItem('jwt_token', 'token-1')
    localStorage.setItem('user_info', '{"username":"admin"}')
    stub401({ error: 'invalid_credentials' })

    const error = await api.post('/api/login', { username: 'admin', password: 'bad' })
      .catch((e: unknown) => e)

    expect(error).toBeInstanceOf(ApiError)
    expect((error as ApiError).body).toEqual({ error: 'invalid_credentials' })
    expect(localStorage.getItem('jwt_token')).toBe('token-1')
    expect(localStorage.getItem('user_info')).toBe('{"username":"admin"}')
  })

  it('其他接口 401 清会话（跳转决策由 redirectToLogin 用例覆盖）', async () => {
    // 停在登录页：避免触发 jsdom 未实现的页面跳转
    window.history.replaceState({}, '', '/login')
    localStorage.setItem('jwt_token', 'token-1')
    localStorage.setItem('user_info', '{"username":"admin"}')
    stub401()

    await expect(api.get('/api/user/me')).rejects.toBeInstanceOf(ApiError)

    expect(localStorage.getItem('jwt_token')).toBeNull()
    expect(localStorage.getItem('user_info')).toBeNull()
  })

  it('redirectToLogin：非登录页跳转，登录页 no-op（整页刷新循环的断点）', () => {
    const navigate = vi.fn()

    redirectToLogin('/', navigate)
    expect(navigate).toHaveBeenCalledWith('/login')

    navigate.mockClear()
    redirectToLogin('/login', navigate)
    expect(navigate).not.toHaveBeenCalled()

    expect(isOnLoginPage('/login')).toBe(true)
    expect(isOnLoginPage('/monitor')).toBe(false)
  })
})
