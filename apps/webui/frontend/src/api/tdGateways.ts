import { api } from './client'

/**
 * 交易接口（td 网关进程）生命周期端点（契约 rest §2.10、ADR 0053）。
 *
 * 与行情源域（`/api/market-sources/*`）刻意分成两套：行情源的 `source_id` 是 DB 主键与
 * 卡片机制都绑定的 md 专属概念，交易接口没有任何 DB 实体——接口身份 = 进程名（= exe stem），
 * 生命周期操作直接写进程控制帧（契约 process）。因此这里只有四个薄端点，没有「创建条目」
 * 与「列表」：卡片列表由 WS `process_config` / `process_status` 派生（契约 rest §4）。
 *
 * 响应语义（契约 rest §2.10）：200 `{ok:true}` 只表示**已写入事件通道**，不代表已生效——
 * 成败以 WS `process_config`（条目增删）与 `process_status`（带 event）为准。
 */

/** 可用交易接口（`GET /api/td/gateways/available`，按 name 升序） */
export interface TdGatewayAvailable {
  /** 进程名（= exe stem = 接口身份，不可改名） */
  name: string
  /** 已注册时取进程配置 display_name；未注册时回退 name */
  display_name: string
  /** 进程配置镜像中存在该 dztd_* 条目 */
  added: boolean
  /** process_status 为 Running */
  running: boolean
}

/** `POST .../start` 可选 body：display_name 走配置 patch（进程配置字段，不是独立实体） */
export interface StartTdGatewayBody {
  display_name?: string
}

/** 写端点统一响应：只表示已写入事件通道（契约 rest §2.10） */
export interface TdGatewayWriteOk {
  ok: boolean
}

export const tdGatewaysApi = {
  /**
   * 可用交易接口列表（任意已认证用户）：扫描 App Root 的 `dztd_*` exe + 进程配置镜像。
   * 添加流程的下拉数据源；`added` / `running` 只用于标注，放行/拦截由写端点裁决。
   */
  listAvailable: () => api.get<TdGatewayAvailable[]>('/api/td/gateways/available'),
  /**
   * 启动/添加交易接口（admin）：未注册时 master 动态注册并持久化（契约 process）。
   * 卡片上的「启动进程」与页面级「添加交易接口」共用本端点（区别只在 body 是否带显示名）。
   */
  start: (name: string, body?: StartTdGatewayBody) =>
    api.post<TdGatewayWriteOk>(`/api/td/gateways/${encodeURIComponent(name)}/start`, body ?? {}),
  /** 停止交易接口进程（admin）：瞬态，不是持久期望态（ADR 0053；UI 不提供该按钮） */
  stop: (name: string) =>
    api.post<TdGatewayWriteOk>(`/api/td/gateways/${encodeURIComponent(name)}/stop`),
  /** 移除交易接口（admin）：停止进程 + 删除进程配置条目；有账户时服务端 409 guard */
  remove: (name: string) =>
    api.del<TdGatewayWriteOk>(`/api/td/gateways/${encodeURIComponent(name)}`),
}

/**
 * 交易接口写端点失败码（契约 rest §2.10 预检清单）。
 *
 * 与账户写端点（`composables/tdAccountErrors.ts`）分开维护：两边共享 `admin_required`，
 * 但其余 code 的语义与用户动作不同（例如 `gateway_has_accounts` 只在本域出现），
 * 合并成一张表会让两份文案互相污染。
 */
export type TdGatewayErrorCode =
  | 'admin_required'
  | 'invalid_gateway'
  | 'gateway_not_found'
  | 'gateway_has_accounts'

/** 机器码 → 中文文案（契约 rest §2.10；`invalid_gateway` 同时覆盖 start 的「扫描不到 exe」预检） */
export const TD_GATEWAY_ERROR_TEXT: Record<TdGatewayErrorCode, string> = {
  admin_required: '需要管理员权限才能管理交易接口',
  invalid_gateway: '目标不是可用的交易接口（未注册且 App Root 下找不到对应程序）',
  gateway_not_found: '交易接口不存在（可能已被移除），请刷新后重试',
  gateway_has_accounts: '该交易接口下还有账户，请先删除全部账户再移除接口',
}

/** 未知/无 code 的兜底文案（绝不回显英文 error 或 snake_case code） */
export const TD_GATEWAY_ERROR_FALLBACK = '交易接口操作失败'
/** 无 HTTP 响应（网络中断）文案 */
export const TD_GATEWAY_NETWORK_ERROR_TEXT = '无法连接服务器，请检查网络后重试'

/**
 * 失败码 → 用户可读文案；未知码兜底（入参 unknown：运行时错误体不可信）。
 */
export function tdGatewayErrorText(code: unknown): string {
  if (typeof code === 'string' && Object.prototype.hasOwnProperty.call(TD_GATEWAY_ERROR_TEXT, code)) {
    return TD_GATEWAY_ERROR_TEXT[code as TdGatewayErrorCode]
  }
  return TD_GATEWAY_ERROR_FALLBACK
}

/**
 * 「有响应但不带 code」的基础设施失败文案（契约 rest §2.10 预检 6）：
 * dzweb 写事件通道不可用回 503 且不带 code——不能说成网络错误，也不能套预检码文案。
 */
export function tdGatewayServerErrorText(status: number | undefined): string {
  if (status === 503) return '交易接口服务暂不可用（事件通道未就绪），请稍后重试'
  if (status === 401) return '登录已失效，请重新登录'
  if (status === 403) return TD_GATEWAY_ERROR_TEXT.admin_required
  return TD_GATEWAY_ERROR_FALLBACK
}
