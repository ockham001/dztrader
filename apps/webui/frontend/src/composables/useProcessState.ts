// Process state display helpers — 从 CtpCard.vue:259-277 / GenericCard.vue:26-42 原样提取
// 契约 04 ChildState 为 PascalCase（Starting/Running/Stopping/Stopped/Crashed）：
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
