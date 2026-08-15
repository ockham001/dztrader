import { api } from './client'
import type { MarketSource, BrokerEntry, BrokerFrontend } from '@/types/api'

export interface CreateMarketSourceBody {
  source_type: string
  source_name: string
  display_name: string
}

export interface AvailableMarketSource {
  name: string          // 真实进程名（dzmd_ctp 等），即单实例模式下的 source_name
  display_name: string  // 友好显示名（Ctp 等）
  ui_card: string       // UI 卡片大类（去掉 dzmd_/dztd_ 前缀后的第一段，契约 06）
  added: boolean        // dztraderd.json 中是否已写入 [gateways.<name>] 段（"运行中"或"待运行"）
  in_db: boolean        // SQLite 中是否已有该 source_name 记录（用于复用 DB 记录）
}

export interface UpdateMarketSourceBody {
  display_name: string
}

// 自动登录/登出排程请求体 (契约 04: 全量提交, 直发 SET_AUTO_LOGIN)
export interface AutoLoginBody {
  enabled: boolean
  schedules: { login_time: string; logout_time: string }[]
}

// 添加经纪商请求体: name 为唯一 key (创建后不可修改), 其余字段为连接参数
export interface AddBrokerBody {
  name: string
  broker_id: string
  user_id: string
  password: string
  product_info: string
}

export interface AddFrontendBody {
  address: string
  label: string
}

export interface SetCurrentBrokerBody {
  name: string  // 空字符串表示清空选中
}

// 订阅参数请求体 (契约 08 SetSubscribeParams): 全部可选, 缺失=保留旧值, 无状态保护
export interface SubscribeParamsBody {
  subscribe_batch_size?: number
  subscribe_batch_delay_ms?: number
  sub_check_interval_ms?: number
  sub_max_retry?: number
}

// SHM 行情通道配置请求体 (契约 02 SET_MD_SHM_CONFIG): RFC 7386 递归合并 patch。
// preload_points 内 key 的 value 为 null = 删除该时间点 (契约 02 唯一合法 null 位置);
// page_size_mb 不可变, 网关跳过 (前端不下发该字段)
export interface ShmConfigPatch {
  preload_points?: Record<string, { pages: number; bytes: number } | null>
  check_interval_min?: number
  check_pages?: number
  check_bytes?: number
}

export interface StartMarketSourceBody {
  display_name?: string      // 显示名（覆盖 DB 中的值）
}

export const marketSourcesApi = {
  list: () => api.get<MarketSource[]>('/api/market-sources'),
  listAvailable: () => api.get<AvailableMarketSource[]>('/api/market-sources/available'),
  refresh: () => api.post<{ ok: boolean }>('/api/market-sources/refresh', {}),
  create: (data: CreateMarketSourceBody) => api.post<MarketSource>('/api/market-sources', data),
  update: (id: number, data: UpdateMarketSourceBody) => api.put<MarketSource>(`/api/market-sources/${id}`, data),
  remove: (id: number) => api.del<{ ok: boolean; id: number }>(`/api/market-sources/${id}`),
  // login 不再在 body 中携带 credentials: credentials 现存储在子进程配置的 broker entry 中
  // (Wave 2B commit 3617e30). 后端通过 source_id -> 镜像 current_broker_name 取 credentials.
  login: (id: number) => api.post<{ ok: boolean }>(`/api/market-sources/${id}/login`),
  logout: (id: number) => api.post<{ ok: boolean }>(`/api/market-sources/${id}/logout`),
  start: (id: number, data?: StartMarketSourceBody) =>
    api.post<{ ok: boolean; source: string }>(`/api/market-sources/${id}/start`, data ?? {}),
  stop: (id: number) => api.post<{ ok: boolean }>(`/api/market-sources/${id}/stop`),
  // 自动登录/登出排程 (契约 04): 全量提交 {enabled, schedules}, 直发 SET_AUTO_LOGIN 帧,
  // HTTP 同步响应仅表示已下发, 最终状态由 WS auto_login 推送 (RTN_AUTO_LOGIN) 决定
  setAutoLogin: (id: number, data: AutoLoginBody) =>
    api.put<{ ok: boolean }>(`/api/market-sources/${id}/auto-login`, data),

  // ===== Broker CRUD (Wave 2B: SHM dispatch, 不再有 DB 表) =====
  // 添加经纪商 (扩展候选列表, 不改变当前连接, 不受状态保护)
  addBroker: (sourceId: number, data: AddBrokerBody) =>
    api.post<{ ok: boolean }>(`/api/market-sources/${sourceId}/brokers`, data),
  // 删除经纪商 (连接参数变更, 受状态保护, 需二次确认)
  removeBroker: (sourceId: number, brokerName: string) =>
    api.del<{ ok: boolean }>(`/api/market-sources/${sourceId}/brokers/${encodeURIComponent(brokerName)}`),
  // 编辑经纪商字段 (name 不可变, 其余字段全量下发)
  updateBroker: (sourceId: number, brokerName: string, data: BrokerEntry) =>
    api.put<{ ok: boolean }>(`/api/market-sources/${sourceId}/brokers/${encodeURIComponent(brokerName)}`, data),
  // 替换前置地址列表 (整体下发, 用于增删改切换 enabled)
  updateFrontends: (sourceId: number, brokerName: string, frontends: BrokerFrontend[]) =>
    api.put<{ ok: boolean }>(`/api/market-sources/${sourceId}/brokers/${encodeURIComponent(brokerName)}/frontends`, { frontends }),
  // 切换当前选中经纪商 (空字符串清空选中)
  setCurrentBroker: (sourceId: number, brokerName: string) =>
    api.put<{ ok: boolean }>(`/api/market-sources/${sourceId}/current-broker`, { name: brokerName }),
  // 修改订阅参数 (契约 08): 可选字段 patch, 直发 SET_MD_CONFIG{SetSubscribeParams},
  // HTTP 同步响应仅表示已下发, 最终状态由 WS md_rtn_config 推送决定
  setSubscribeParams: (sourceId: number, data: SubscribeParamsBody) =>
    api.put<{ ok: boolean }>(`/api/market-sources/${sourceId}/subscribe-params`, data),
  // 修改 SHM 行情通道配置 (契约 02): merge patch 直发 SET_MD_SHM_CONFIG,
  // 最终状态由 WS md_shm_config 推送 (RTN_MD_SHM_CONFIG) 决定
  setShmConfig: (sourceId: number, data: ShmConfigPatch) =>
    api.put<{ ok: boolean }>(`/api/market-sources/${sourceId}/shm-config`, data),
}
