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
    localStorage.removeItem('jwt_token')
    localStorage.removeItem('user_info')
    window.location.href = '/login'
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
