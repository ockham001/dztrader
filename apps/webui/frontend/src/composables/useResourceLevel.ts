/**
 * 资源使用率色标（顶栏 CPU / 内存 / 卷徽章、监控页卷列表与内存 / 交换使用率共用；
 * 阈值只在本模块定义）
 *
 * 90% 已用等价于「10% 空闲」，与契约 sysmon §2.8 的 `thresholds.volume_free_pct` 默认值 10
 * 同源（按空闲百分比判定：`free_pct = 100 − used_ratio × 100`）。主流实现同在
 * 90% used / 10% free 附近取判据（Zabbix 80/90% used、Netdata 90/98% used、PRTG 25/10% free、
 * Datadog 10% free、GNOME 5% free）；既有 Windows 口径亦然——`LowDiskSpaceMinimum` 400 MB
 * 只是绝对容量抑制项，`DiskSpaceThreshold` 的 10% 与此处「10% 空闲」同口径
 *（完整外部证据见 ADR 0034）。
 *
 * 色标是**独立展示规则，不是越限判定**（契约 §2.8、ADR 0034）：越限判据是
 * `free_pct < volume_free_pct`（默认 <10%），故恰好 90% 已用显示红色但不构成越限。
 * 常量集中于此、各处不得各自定义，让「同一份使用率在顶栏与监控页颜色不一致」的漂移只有一个修正点。
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

/** 等级排序：danger > warn > normal（顶栏资源图标 / 服务徽章的「取最差」共用） */
const RESOURCE_LEVEL_RANK: Record<ResourceLevel, number> = { normal: 0, warn: 1, danger: 2 }

/**
 * 多读数取「最差」展示等级（danger > warn > normal），与读数顺序无关。
 * null = 不可判读（无快照 / 指标不可用 / 进程状态未知），不参与聚合：
 * 全部不可判读时返回 null，由调用方决定中性展示，不得默认 normal 冒充「一切正常」。
 */
export function worstResourceLevel(levels: Iterable<ResourceLevel | null>): ResourceLevel | null {
  let worst: ResourceLevel | null = null
  for (const level of levels) {
    if (level === null) continue
    if (worst === null || RESOURCE_LEVEL_RANK[level] > RESOURCE_LEVEL_RANK[worst]) worst = level
  }
  return worst
}
