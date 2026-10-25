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

/**
 * 401 后是否跳转登录页。
 * - 登录接口的 401 是凭据错误（不是会话失效）：交给表单展示，不清会话
 * - 已在登录页时不再跳转：否则「请求 401 → 跳 /login → 重载 → 再 401」会形成整页刷新循环
 */
export function shouldRedirectOn401(path: string, currentPath: string): boolean {
  return path !== '/api/login' && currentPath !== '/login'
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
    // 登录接口的 401 是凭据错误而非会话失效：不清会话（否则输错密码会把登录态清掉）
    if (path !== '/api/login') {
      localStorage.removeItem('jwt_token')
      localStorage.removeItem('user_info')
    }
    // 跳转决策见 shouldRedirectOn401（登录页内不重复跳转，避免整页刷新循环）
    if (shouldRedirectOn401(path, window.location.pathname)) {
      window.location.replace('/login')
    }
    throw new ApiError(401, { error: 'unauthorized' })
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
