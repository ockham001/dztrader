/**
 * 资金的展示纯函数（契约 rest §2.9.4，契约 webui-ws §5 第 12/13 条）。
 *
 * 为什么单独成模块：金额格式化、出入金文案、锚点日标签与在线状态都是**纯函数**，store 与
 * 视图共用同一实现，视图里不得再写一份（同一单元格两种格式 = 同一事实两种说法）。
 *
 * 与持仓展示的差异：资金行是**每账户一行**（业务键 account_id），金额字段全为 `number|null`
 * ——`null` 表示「无记录/不可用」，必须显示占位而不是 0（契约 webui-ws §5 第 13 条）。
 */
import { formatTdDate } from './tdPositionView'
import { TD_UNAVAILABLE_TEXT } from './tdOrderView'

/**
 * 金额显示：固定 2 位小数 + 千分位（`1234567.89` → `1,234,567.89`）。
 *
 * `null` 与**非有限值**（NaN / ±Infinity）显示占位 `—`：它们都不是资金事实，用 `0` 冒充
 * 会把「无数据」说成「余额为零」（契约 webui-ws §5 第 13 条）。
 *
 * 负号按数值符号输出；`-0` 按 `0` 显示（不是负余额）。
 */
export function formatTdMoney(value: number | null): string {
  if (value === null || !Number.isFinite(value)) return TD_UNAVAILABLE_TEXT
  const fixed = Math.abs(value).toFixed(2)
  const [integer, fraction] = fixed.split('.')
  const grouped = integer.replace(/\B(?=(\d{3})+(?!\d))/g, ',')
  return `${value < 0 ? '-' : ''}${grouped}.${fraction}`
}

/**
 * 出入金单侧文案：`null` → 占位（该段无数据）；`0` → 带符号的 `0`（当日无出入金是**真实值**，
 * 不得显示成占位）；其余按金额口径格式化（2 位小数 + 千分位）。
 */
function fundFlowText(sign: '+' | '-', value: number | null): string {
  if (value === null || !Number.isFinite(value)) return TD_UNAVAILABLE_TEXT
  if (value === 0) return `${sign}0`
  return `${sign}${formatTdMoney(value)}`
}

/**
 * 出入金文案：`入 +{deposit} / 出 -{withdraw}`（契约 rest §2.9.4 的入金/出金金额）。
 *
 * 两个金额分别格式化：任一为 `null` 时该段显示占位（不拿另一侧的数值脑补）；
 * `0` 显示 `+0` / `-0`——零是合法真实值，与「无数据」必须可区分。
 */
export function formatDepositWithdraw(deposit: number | null, withdraw: number | null): string {
  return `入 ${fundFlowText('+', deposit)} / 出 ${fundFlowText('-', withdraw)}`
}

/**
 * 面板级数据日期标签：`数据日期 YYYY-MM-DD / YYYY-MM-DD`。
 *
 * 资金行的 `date` 是**各账户锚点日**（契约 rest §2.9.1），多账户时可能不同：去重升序后
 * 全部列出，不得只标一个日期（那会把「旧快照」说成「当天数据」）。空集合返回空串
 * （调用方据此不渲染标签，而不是渲染一个空的「数据日期」）。
 */
export function formatFundDateLabel(dates: readonly string[]): string {
  const unique = [...new Set(dates)].sort()
  if (unique.length === 0) return ''
  return `数据日期 ${unique.map(formatTdDate).join(' / ')}`
}

/**
 * 账户是否在线（信封 `online_accounts` 判定，契约 rest §2.9.2）。
 *
 * 唯一谓词：状态文案（{@link fundStatusText}）与面板状态标签的样式都从它推导——
 * 两处各写一份 `onlineAccounts.includes` 时，改一处忘另一处会出现「文案=在线、样式=离线」
 * 这种自相矛盾的单元格。
 */
export function isTdAccountOnline(accountId: string, onlineAccounts: readonly string[]): boolean {
  return onlineAccounts.includes(accountId)
}

/** 账户在线状态文案（由 {@link isTdAccountOnline} 推导）：在线 / 离线 */
export function fundStatusText(accountId: string, onlineAccounts: readonly string[]): string {
  return isTdAccountOnline(accountId, onlineAccounts) ? '在线' : '离线'
}
