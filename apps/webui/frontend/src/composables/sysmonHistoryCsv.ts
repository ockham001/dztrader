import type { SysmonHistoryPoint } from '@/types/api'

/**
 * 历史序列导出（#327）：纯前端把「当前选中指标 + 当前时间范围」的全部记录窗口转成 CSV 文本。
 *
 * 列取《帧契约：系统监控》§6.1 桶字段的子集：`ts`（导出列名为 AC 指定的 `timestamp`，
 * Unix 秒，与接口同轴）、`min` / `avg` / `max`、`n`（有效样本数）。`last` 是展示用末值、不导出。
 *
 * `null`（该桶内无有效样本）导出为**空字段**，不写 0 —— 与全仓「不可用 ≠ 0」口径一致。
 * 数值按接口原值导出（不做展示档位四舍五入），导出是数据交换格式而不是页面文本。
 */
export const SYSMON_HISTORY_CSV_HEADER = ['timestamp', 'min', 'avg', 'max', 'n'] as const

/** 单个字段的 CSV 转义：含逗号 / 双引号 / 换行时用双引号包裹，内部双引号翻倍（RFC 4180）。 */
export function escapeCsvField(value: string): string {
  if (!/[",\r\n]/.test(value)) {
    return value
  }
  return `"${value.replace(/"/g, '""')}"`
}

/**
 * 把一组历史桶导出为 CSV 文本（行尾 CRLF，便于 Excel 直接打开）。
 * 空序列返回仅含表头的文本（调用方据 `state === 'ready'` 决定按钮可用性）。
 */
export function historyCsv(points: readonly SysmonHistoryPoint[]): string {
  const lines = [SYSMON_HISTORY_CSV_HEADER.join(',')]
  for (const point of points) {
    lines.push(
      [
        String(point.ts),
        point.min === null ? '' : String(point.min),
        point.avg === null ? '' : String(point.avg),
        point.max === null ? '' : String(point.max),
        point.n === null ? '' : String(point.n),
      ]
        .map(escapeCsvField)
        .join(','),
    )
  }
  return lines.join('\r\n') + '\r\n'
}

/**
 * 导出文件名：带指标 key 与查询区间，避免多次导出互相覆盖。
 * 指标 key 含 `.`（如 `cpu.usage_ratio`），替换为 `-` 以兼容 Windows 文件名。
 */
export function historyCsvFilename(metricKey: string, rangeSeconds: number): string {
  const metric = metricKey.replace(/[^a-z0-9]+/gi, '-').replace(/^-+|-+$/g, '')
  return `sysmon-history-${metric}-${rangeSeconds}s.csv`
}
