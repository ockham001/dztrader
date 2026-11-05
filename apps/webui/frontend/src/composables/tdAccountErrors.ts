// 交易账户配置写端点的失败码 → 用户可读文案（单一真相源）。
//
// 契约来源：契约 rest §2.9.7 预检清单 + §2.9.7 响应段。写端点失败体为
// `{"error": <human message>, "code": <snake_case code>}`：
// `error` 是英文人类可读消息（排障用），`code` 是机器码、也是前端**唯一**的分支依据
// （不解析 `error` 文本，也**不回显**英文消息给用户——与登录页 loginErrors.ts 同一口径）。
//
// code 全集（契约 rest §2.9.7 预检 1–5，与 HTTP 状态无关地按 code 映射，避免状态码与语义漂移）：
//   admin_required       403  非 admin 角色
//   invalid_gateway      400  目标网关不是已注册的 dztd_* 进程
//   gateway_not_running  409  目标网关当前不是 Running
//   account_exists       409  account_id 已在注册表（任何网关，含已禁用/离线）
//   account_not_found    404  注册表无该 account_id（PUT/DELETE）
//
// 为什么本模块是 composable 目录下的独立模块：账户配置页与 store 的失败提示必须同源，
// 与 `loginErrors.ts` / `marketSourceView.ts` 等既有跨 store 文案模块保持同一组织方式。

export type TdAccountErrorCode =
  | 'admin_required'
  | 'invalid_gateway'
  | 'gateway_not_running'
  | 'account_exists'
  | 'account_not_found'

/** 机器码 → 中文文案（契约 rest §2.9.7） */
export const TD_ACCOUNT_ERROR_TEXT: Record<TdAccountErrorCode, string> = {
  admin_required: '需要管理员权限才能修改交易账户',
  invalid_gateway: '目标交易网关不存在或未注册',
  gateway_not_running: '目标交易网关未运行，请先启动网关',
  account_exists: '该账户 ID 已被占用（可能属于其它交易网关），请换一个账户 ID',
  account_not_found: '账户不存在（可能已被删除或未同步），请刷新后重试',
}

/** 通用失败兜底文案（无 code / 未知 code / HTTP 层失败） */
export const TD_ACCOUNT_ERROR_FALLBACK = '交易账户操作失败'
/** HTTP 层失败（网络/服务不可达，无响应体）文案 */
export const TD_ACCOUNT_NETWORK_ERROR_TEXT = '无法连接服务器，请检查网络后重试'

/**
 * 预检之外的「基础设施失败」文案（服务端有响应但不带 code）。
 *
 * 具体形态：dzweb 写事件通道失败回 **503 且不带 code**（契约 rest §2.9.7 的预检 code 族之外，
 * 属"请求未被受理"）。这类失败**不能**说成"无法连接服务器"（服务端明明回应了），也不能
 * 套用任何预检码文案（会误导用户去改账户 ID / 权限）。
 */
export function tdAccountServerErrorText(status: number | undefined): string {
  if (status === 503) return '交易网关服务暂不可用（事件通道未就绪），请稍后重试'
  if (status === 401) return '登录已失效，请重新登录'
  if (status === 403) return TD_ACCOUNT_ERROR_TEXT.admin_required
  return TD_ACCOUNT_ERROR_FALLBACK
}

/**
 * 失败码 → 用户可读文案；未知/非法码统一兜底文案（绝不回显 snake_case 或英文 error）。
 * @param code 后端错误体的 `code` 字段（运行时不可信，故入参为 unknown）
 */
export function tdAccountErrorText(code: unknown): string {
  if (typeof code === 'string' && Object.prototype.hasOwnProperty.call(TD_ACCOUNT_ERROR_TEXT, code)) {
    return TD_ACCOUNT_ERROR_TEXT[code as TdAccountErrorCode]
  }
  return TD_ACCOUNT_ERROR_FALLBACK
}

/**
 * 从运行时错误对象取出机器码（`ApiError.body.code`）。
 *
 * store 侧捕获的是任意 Error，不能假定形状：非 `ApiError`（网络中断等）返回 undefined，
 * 由调用方回落到 {@link TD_ACCOUNT_NETWORK_ERROR_TEXT}（无 HTTP 响应）
 * 或 {@link tdAccountServerErrorText}（有响应但不带 code，如 503）。
 */
export function tdAccountErrorCodeOf(error: unknown): string | undefined {
  const body = (error as { body?: { code?: unknown } } | null | undefined)?.body
  const code = body?.code
  return typeof code === 'string' ? code : undefined
}

/**
 * 从运行时错误对象取出 HTTP 状态码（`ApiError.status`）。
 * 非 `ApiError`（网络中断/超时）没有状态码 → undefined，据此区分「服务端拒绝」与「没连上」。
 */
export function tdAccountErrorStatusOf(error: unknown): number | undefined {
  const status = (error as { status?: unknown } | null | undefined)?.status
  return typeof status === 'number' && Number.isFinite(status) ? status : undefined
}
