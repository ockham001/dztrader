/**
 * 日志级别颜色共享常量（LogViewerTab / LogLevelControlTab 两处合一）
 * 设计 §5.4/§8.2：LEVEL_COLORS 两处 → 一处
 */
export const LEVEL_COLORS: Record<string, string> = {
  trace: 'var(--text-tertiary)',
  debug: 'var(--text-secondary)',
  info: 'var(--status-success-default)',
  warning: 'var(--status-alert-default)',
  error: 'var(--status-error-default)',
  critical: '#b000ff',
}

export function levelColor(level: string): string {
  return LEVEL_COLORS[level] ?? 'var(--text-tertiary)'
}