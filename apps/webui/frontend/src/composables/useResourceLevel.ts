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
 * 数值 + 双档阈值 → 展示等级（边界含：恰好 warn 阈值 → warn、恰好 danger 阈值 → danger）。
 *
 * 这是本仓**唯一**的「数值定档」实现：资源类使用率由 {@link resourceLevel} 转发（阈值是
 * 本模块常量），时钟偏差由调用方传入配置阈值（契约 sysmon §3.1 `clock.offset_warn_ms` /
 * `offset_error_ms`）——阈值来源不同，但「≥ 阈值即进入该档」这一判定语义与等级词表只有一份
 *（review-checklist §2.4：同一事实两处定义必然在改口径时漂移）。
 *
 * 入参须是已净化的有限值；不可用（null / NaN）由调用方自行分支，不得用 0 冒充后再套色标。
 * 调用方负责取绝对值等口径转换（阈值恒为正向：时钟偏差按绝对值定档，契约 sysmon §2.8）。
 */
export function thresholdLevel(value: number, warnThreshold: number, dangerThreshold: number): ResourceLevel {
  if (value >= dangerThreshold) return 'danger'
  if (value >= warnThreshold) return 'warn'
  return 'normal'
}

/**
 * 比率 → 展示等级（边界含：恰好 0.7 → warn、恰好 0.9 → danger）。
 * 入参须是已净化的有限比率；不可用（null / NaN）由调用方自行分支，
 * 不得用 0 冒充数据后再套色标。
 */
export function resourceLevel(ratio: number): ResourceLevel {
  return thresholdLevel(ratio, RESOURCE_WARN_RATIO, RESOURCE_DANGER_RATIO)
}

/** 等级排序：danger > warn > normal（顶栏资源图标 / 服务徽章的「取最差」共用） */
const RESOURCE_LEVEL_RANK: Record<ResourceLevel, number> = { normal: 0, warn: 1, danger: 2 }

/**
 * 展示等级 → CSS 修饰类（顶栏徽章的**唯一**等级类映射）。
 *
 * 为什么放在本模块：等级类把「等级词表」与 `layout.css` 的着色规则绑在一起，分散在
 * 徽章组件 / 服务徽章 / 窄屏图标 / 浮层条目四处各自拼字符串，改词表（例如新增 caution 档）
 * 就会漏改一处、静默少一个颜色。`null` = 不可判读（快照未到 / 指标不可用 / 状态未知），
 * 走中性 `--unavailable` 类，不得用 normal 冒充「一切正常」。
 */
export function resourceLevelClass(level: ResourceLevel | null): string {
  return level === null ? 'topbar__resource--unavailable' : `topbar__resource--${level}`
}

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
