const BASE_URL = ''  // Same-origin, no baseURL needed

export interface ApiErrorBody {
  error: string
  [key: string]: unknown
}

export class ApiError extends Error {
  constructor(public status: number, public body: ApiErrorBody | null) {
    super(body?.error || `HTTP ${status}`)
    this.name = 'ApiError'
  }
}

/** 当前是否在登录页（参数化 currentPath 便于测试） */
export function isOnLoginPage(currentPath: string = window.location.pathname): boolean {
  return currentPath === '/login'
}

/**
 * 会话失效后硬跳转登录页；已在登录页时 no-op。
 * 这是**唯一**的登录页跳转点：不 no-op 就会形成「请求 401 → 跳 /login → 重载 → 再 401」
 * 的整页刷新循环（issue #118）。
 */
export function redirectToLogin(
  currentPath: string = window.location.pathname,
  navigate: (url: string) => void = url => window.location.replace(url),
): void {
  if (!isOnLoginPage(currentPath)) navigate('/login')
}

async function request<T>(
  path: string,
  options: RequestInit = {}
): Promise<T> {
  const token = localStorage.getItem('jwt_token')
  const headers: Record<string, string> = {
    'Content-Type': 'application/json',
    ...(options.headers as Record<string, string> | undefined),
  }
  if (token) {
    headers['Authorization'] = `Bearer ${token}`
  }

  const resp = await fetch(`${BASE_URL}${path}`, { ...options, headers })

  if (resp.status === 401) {
    // 401 的响应体可能是凭据错误（invalid_credentials / account_locked + locked_until），
    // 必须原样交给调用方展示，否则登录表单只能看到笼统的 unauthorized
    let body: ApiErrorBody | null = null
    try { body = await resp.json() as ApiErrorBody } catch { /* ignore parse error */ }
    // 登录接口的 401 是凭据错误而非会话失效：不清会话、不跳转
    if (path !== '/api/login') {
      localStorage.removeItem('jwt_token')
      localStorage.removeItem('user_info')
      redirectToLogin()  // 已在登录页时 no-op（断掉整页刷新循环）
    }
    throw new ApiError(401, body ?? { error: 'unauthorized' })
  }

  if (!resp.ok) {
    let body: ApiErrorBody | null = null
    try { body = await resp.json() as ApiErrorBody } catch { /* ignore parse error */ }
    throw new ApiError(resp.status, body)
  }

  // 204 No Content
  if (resp.status === 204) return undefined as T
  return resp.json() as Promise<T>
}

export const api = {
  get: <T>(path: string) => request<T>(path),
  post: <T>(path: string, body?: unknown) =>
    request<T>(path, { method: 'POST', body: body ? JSON.stringify(body) : undefined }),
  put: <T>(path: string, body?: unknown) =>
    request<T>(path, { method: 'PUT', body: body ? JSON.stringify(body) : undefined }),
  del: <T>(path: string, body?: unknown) =>
    request<T>(path, { method: 'DELETE', body: body ? JSON.stringify(body) : undefined }),
}
