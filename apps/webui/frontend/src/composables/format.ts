/**
 * 数值格式化唯一实现（#126；契约 sysmon §2.4 的展示口径）
 *
 * 为什么收敛成一份：同一**单位族**的数值此前在监控页 / 历史图表 / 配置页 / 日志页各自格式化
 * （字节数、比率等），于是同一量在不同页面显示不同精度与不同单位档（同一块 22 TiB 的卷，
 * 旧实现按 GiB 定标会渲染成 `22528.0 GiB`），切换单位档或精度时也必然漏掉某一页。
 * 本模块是这些数值的**唯一**实现：本文件之外不得再为字节/比率/延迟/温度/计数自行写
 * `toFixed`。
 *
 * 明确的例外（不是遗漏）：
 * - Topbar 徽章的整数百分比是刻意的窄条紧凑显示（见该组件注释），不走 `formatPercent`。
 *
 * 时长（秒 → 可读时长）于 #142 迁入本模块：sparkline 的「实际覆盖时长」与历史视图的
 * 「实际覆盖最近 X」是同一个换算，两处各写一份必然在改口径时漂移。
 * 例外：`MonitorView.formatAge` 的「文件年龄」是向下取整的整档口径（多久以前），与覆盖时长的
 * 展示精度不同，不共用本函数。
 *
 * 单位与精度（写在契约外的展示约定，改这里就是改全部页面）：
 * - 字节：IEC 1024 进制 `B` / `KiB` / `MiB` / `GiB` / `TiB`；`B` 档取整、其余档固定 1 位小数。
 *   定标 + 固定小数位的先例是 btop 的 `shorten` 模式：本场景是按列对齐的实时表格，
 *   **宽度稳定优先于单值精度**（Grafana / Netdata 的有效数字自适应会让每秒刷新的列宽跳动）。
 * - 速率 = 字节值 + `/s`。
 * - 百分比 1 位、负载 2 位、延迟 2 位、温度 1 位、IOPS 与计数取整、计数带千分位。
 *
 * 不可用语义（契约 sysmon §2.5）：`null` / `NaN` / `±Infinity` 一律返回
 * {@link UNAVAILABLE_TEXT}——**不得用 0 冒充**。非法负值只在**字节与速率**格式化器里视为
 * 不可用（负容量/负吞吐没有物理意义）；其余格式化器不臆断取值范围（比率/温度/延迟是否合法
 * 属于上层采集与校验语义）。「不可用」（单元格值缺失）与「暂无数据」（整块无数据）是两种
 * 不同事实，后者由视图的空态承担，不经过本模块。
 */

/**
 * 不可用占位：**数值不可用**的场景共用同一文案（契约 sysmon §2.5「不可用一律显示不可用而非 0」）。
 *
 * 它不覆盖其它占位语义：快照尚未到达的等待态（顶栏 `—`）与「总量为 0」这类合法事实
 * （监控页折叠组里的 `—`）是不同事实，仍由各处自行表达。
 */
export const UNAVAILABLE_TEXT = '不可用'

/** IEC 字节单位档（1024 进制）；顺序即进位顺序，索引 = 档位 */
export const BYTE_UNITS = ['B', 'KiB', 'MiB', 'GiB', 'TiB'] as const

/** 有限数值判定：`null` / `undefined` / `NaN` / `±Infinity` 均不可用 */
function isFiniteNumber(value: number | null | undefined): value is number {
  return typeof value === 'number' && Number.isFinite(value)
}

/**
 * 字节容量格式化：1024 进制自动定标到 `B` / `KiB` / `MiB` / `GiB` / `TiB`。
 *
 * `B` 档取整（`1023 B`），`≥KiB` 固定 1 位小数（`1.0 KiB`、`22.0 TiB`）。
 * 负值不是合法字节数，返回不可用而不是显示负容量。
 *
 * @param bytes 字节数；`null` / 非有限值 / 负值返回不可用占位
 */
export function formatBytes(bytes: number | null | undefined): string {
  if (!isFiniteNumber(bytes) || bytes < 0) return UNAVAILABLE_TEXT
  let value = bytes
  let unitIndex = 0
  // 进位判据是「按本档精度四舍五入后的显示值」，不是原始值：1023.6 B 在 B 档会显示成 1024，
  // 那等于跨过了档位边界，应先升档——否则出现 `1024 B` 这种看起来像 bug 的读数。
  // 循环到最后一档为止：TiB 是展示上限，更大的值仍以 TiB 呈现（不再进 PiB）。
  while (unitIndex < BYTE_UNITS.length - 1) {
    const digits = unitIndex === 0 ? 0 : 1
    if (Number(value.toFixed(digits)) < 1024) break
    value /= 1024
    unitIndex += 1
  }
  const unit = BYTE_UNITS[unitIndex]
  // B 档取整：字节是最小单位，小数没有意义（也避免 260.42857142857144 这类浮点原值）。
  if (unitIndex === 0) return `${Math.round(value)} ${unit}`
  return `${value.toFixed(1)} ${unit}`
}

/**
 * 字节速率格式化：字节值 + `/s`（如 `260 B/s`、`1.0 MiB/s`）。
 *
 * 与 {@link formatBytes} 共用同一套定标与精度，因此容量与吞吐量在相邻列上单位档一致。
 * 不可用时返回**整串** `不可用`（不是 `不可用/s`）——后缀只在有数值时才有意义。
 */
export function formatByteRate(bytesPerSec: number | null | undefined): string {
  if (!isFiniteNumber(bytesPerSec) || bytesPerSec < 0) return UNAVAILABLE_TEXT
  return `${formatBytes(bytesPerSec)}/s`
}

/**
 * 百分比格式化（入参为 `[0,1]` 的比率）：固定 1 位小数。
 *
 * 比率非法时返回不可用；恰好 0 是合法真实值（显示 `0.0%`，与「不可用」区分）。
 */
export function formatPercent(ratio: number | null | undefined): string {
  if (!isFiniteNumber(ratio)) return UNAVAILABLE_TEXT
  return `${(ratio * 100).toFixed(1)}%`
}

/**
 * 定标小数格式化（系统负载 / 处理器队列长度 / 磁盘队列深度）：固定 2 位。
 *
 * 同类数字纵向比较需要稳定小数位，因此不做「整数就不带小数」的自适应。
 */
export function formatDecimal(value: number | null | undefined, digits = 2): string {
  if (!isFiniteNumber(value)) return UNAVAILABLE_TEXT
  return value.toFixed(digits)
}

/** 平均每请求延迟：固定 2 位 + `ms`（`1.25 ms`）；无定义（窗口内无完成请求）时为不可用 */
export function formatLatency(milliseconds: number | null | undefined): string {
  if (!isFiniteNumber(milliseconds)) return UNAVAILABLE_TEXT
  return `${milliseconds.toFixed(2)} ms`
}

/** 温度：固定 1 位 + `°C`（`45.5 °C`）；调用方只在该指标可用时传入有效值 */
export function formatTemperature(celsius: number | null | undefined): string {
  if (!isFiniteNumber(celsius)) return UNAVAILABLE_TEXT
  return `${celsius.toFixed(1)} °C`
}

/** 每核频率：整数 + `MHz`（`3600 MHz`） */
export function formatFrequency(mhz: number | null | undefined): string {
  if (!isFiniteNumber(mhz)) return UNAVAILABLE_TEXT
  return `${Math.round(mhz)} MHz`
}

/** 每秒 I/O 操作数（IOPS）：取整 + `IOPS`（`2048 IOPS`） */
export function formatIops(value: number | null | undefined): string {
  if (!isFiniteNumber(value)) return UNAVAILABLE_TEXT
  return `${Math.round(value)} IOPS`
}

/**
 * 每秒事件数（通用档）：取整或指定小数位 + `/s`。
 *
 * 后缀 `/s` 只在有数值时拼接——不可用时返回整串 `不可用`，否则会得到 `不可用/s`。
 * 调用方按指标语义选小数位（上下文切换/中断取整、错误/丢弃速率 1 位）。
 */
export function formatPerSec(value: number | null | undefined, digits = 0): string {
  if (!isFiniteNumber(value)) return UNAVAILABLE_TEXT
  return `${value.toFixed(digits)}/s`
}

/** 每秒事件数（上下文切换 / 中断）：取整 + `/s`（`1234/s`） */
export function formatCountPerSec(value: number | null | undefined): string {
  return formatPerSec(value, 0)
}

/** 计数（包 / 错误 / 丢弃 / 页文件等累计量）：取整 + 千分位（`12,000,000`） */
export function formatCount(value: number | null | undefined): string {
  if (!isFiniteNumber(value)) return UNAVAILABLE_TEXT
  return Math.round(value).toLocaleString('en-US')
}

/**
 * 时长格式化（秒 → 可读时长）：天 / 小时保留 1 位小数、分钟**向下取整**、不足 1 分钟按秒。
 *
 * 同一换算用于历史视图的「实际覆盖最近 X」与内联趋势的时间窗口径（#142 迁入本模块）。
 * 分钟向下取整是口径要求，不是精度取舍：四舍五入会把 14 分 59 秒（899 秒）说成
 * 「15 分钟」，恰好违反「不足 15 分钟不得标注最近 15 分钟」；宁少不多。
 * 非有限值 / 负值返回不可用占位（负时长没有意义，不用负号冒充）。
 */
export function formatDuration(seconds: number | null | undefined): string {
  if (!isFiniteNumber(seconds) || seconds < 0) return UNAVAILABLE_TEXT
  if (seconds >= 86_400) return `${(seconds / 86_400).toFixed(1)} 天`
  if (seconds >= 3_600) return `${(seconds / 3_600).toFixed(1)} 小时`
  if (seconds >= 60) return `${Math.floor(seconds / 60)} 分钟`
  return `${seconds} 秒`
}

/**
 * 整数或 2 位小数的自适应格式化（历史图表数值轴的通用档）。
 *
 * 历史图表一条轴上会同时出现计数（页文件数 12）与小数负载（0.42），因此这里按值本身取形态：
 * 整数原样、非整数固定 2 位。**与 {@link formatDecimal} 的区别是刻意的**——列式实时面板要
 * 稳定位宽，而图表 tooltip 要如实体现实测值。
 */
export function formatNumberAuto(value: number | null | undefined): string {
  if (!isFiniteNumber(value)) return UNAVAILABLE_TEXT
  return Number.isInteger(value) ? String(value) : value.toFixed(2)
}
