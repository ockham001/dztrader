/**
 * 资源使用率色标共享常量（顶栏卷徽章 / 监控页卷列表两处合一，禁止各自定义）
 *
 * 90% 已用等价于「10% 空闲」，与《帧契约：系统监控》`thresholds.volume_free_pct` 的默认值 10
 * 同源（该阈值按空闲百分比判定：`free_pct = 100 − used_ratio × 100`）。主流实现同样以
 * 百分比为主判据、绝对容量最多作抑制项：Zabbix 官方模板 used 80%/90%、Netdata used 90%/98%、
 * PRTG free 25%/10%、Datadog 建议 free 10%、GNOME 默认 free 5%。既有 Windows 口径同理：
 * `LowDiskSpaceMinimum` 的 400 MB 是绝对容量抑制项（容量本身太小时不再告警），
 * 历史上 `DiskSpaceThreshold` 的 10% 与这里的「10% 空闲」同口径。
 *
 * 注意：色标（≥70% 黄、≥90% 红）是**独立的展示规则，不是越限判定**——越限判据是
 * `free_pct < volume_free_pct`（默认 free_pct < 10%）。因此恰好 90% 已用显示红色，
 * 但 `free_pct = 10` 不满足严格小于，不构成越限：色标用于扫读，越限用于日志，
 * 两者的边界不必逐点重合。两处组件共用本模块而不是各自抄常量，正是为了让这类
 * 「同一卷在顶栏与监控页颜色不一致」的漂移只有一个修正点。
 */
export const RESOURCE_WARN_RATIO = 0.7
export const RESOURCE_DANGER_RATIO = 0.9

/** 资源使用率展示等级：normal（正常）/ warn（警告）/ danger（危险） */
export type ResourceLevel = 'normal' | 'warn' | 'danger'

/**
 * 比率 → 展示等级（边界含：恰好 0.7 → warn、恰好 0.9 → danger）。
 * 入参须是已净化的有限比率；不可用（null / NaN）由调用方自行分支，
 * 不得用 0 冒充数据后再套色标。
 */
export function resourceLevel(ratio: number): ResourceLevel {
  if (ratio >= RESOURCE_DANGER_RATIO) return 'danger'
  if (ratio >= RESOURCE_WARN_RATIO) return 'warn'
  return 'normal'
}
