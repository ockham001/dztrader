// Process state display helpers — 从 CtpCard.vue:259-277 / GenericCard.vue:26-42 原样提取
// 契约 process ChildState 为 PascalCase（Starting/Running/Stopping/Stopped/Crashed）：
// processStateText 与 processStateColor 均按 PascalCase 匹配。未映射/空值回退逻辑保留.
export function processStateText(state: string | null): string {
  if (!state) return '未知'
  const map: Record<string, string> = {
    Starting: '启动中',
    Running: '运行中',
    Stopping: '停止中',
    Stopped: '已停止',
    Crashed: '已崩溃',
  }
  return map[state] || state
}

// 进程状态展示等级（顶栏服务徽章聚合用，#199）：unknown = 中性「未知」，
// 不套状态色、也不得命中 layout.css 的 warn / danger 闪烁规则。
export type ProcessLevel = 'normal' | 'warn' | 'danger' | 'unknown'

/**
 * 进程状态 → 展示等级。Running 正常、Starting/Stopping 过渡（warn）、
 * Stopped/Crashed 需关注（danger，服务已离线）；空值/未映射 → unknown。
 * 注意与 processStateColor 的口径差异：卡片上的状态文字对 Stopped 用中性 tertiary，
 * 而顶栏徽章是常驻在线状态指示，Stopped 必须显性告警（#199 AC）。
 */
export function processStateLevel(state: string | null): ProcessLevel {
  if (state === 'Running') return 'normal'
  if (state === 'Starting' || state === 'Stopping') return 'warn'
  if (state === 'Stopped' || state === 'Crashed') return 'danger'
  return 'unknown'
}

export function processStateColor(state: string | null): string {
  if (!state) return 'var(--text-tertiary)'
  if (state === 'Running') return 'var(--status-success-default)'
  if (state === 'Crashed') return 'var(--status-error-default)'
  if (state === 'Starting' || state === 'Stopping') return 'var(--status-alert-default)'
  return 'var(--text-tertiary)'
}

// 空闲判断 (契约 F-C4 状态保护, 与 CtpCard.vue:212-214 / marketSources.ts:150 语义一致):
// 进程运行中 (Running) 且未登录 (offline) 时为 Idle, 可修改连接参数.
// 泛化为双字符串参数, 调用点迁移在 P5.
export function isStateIdle(processState: string | null, loginState: string): boolean {
  return processState === 'Running' && loginState === 'offline'
}
