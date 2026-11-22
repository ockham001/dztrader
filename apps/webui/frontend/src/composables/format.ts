/**
 * 数值格式化唯一实现（#126；契约 sysmon §2.4 的展示口径）
 *
 * 为什么收敛成一份：同一**单位族**的数值此前在监控页 / 历史图表 / 配置页 / 日志页各自格式化
 * （字节数、比率等），于是同一量在不同页面显示不同精度与不同单位档（同一块 22 TiB 的卷，
 * 旧实现按 GiB 定标会渲染成 `22528.0 GiB`），切换单位档或精度时也必然漏掉某一页。
 * 本模块是这些数值的**唯一**实现：本文件之外不得再为字节/比率/延迟/温度/计数/毫秒自行写
 * `toFixed`。
 *
 * 明确的例外（不是遗漏）：
 * - Topbar 徽章的整数百分比是刻意的窄条紧凑显示（见该组件注释），不走 `formatPercent`。
 *
 * 时长（秒 → 可读时长）于 #142 迁入本模块：sparkline 的「实际覆盖时长」与历史视图的
 * 「实际覆盖最近 X」是同一个换算，两处各写一份必然在改口径时漂移。
 * 「多久以前」口径由 {@link formatAge} 承担（页文件年龄与顶栏时钟的上次探测时间共用），
 * 与覆盖时长不是同一件事（前者向下取整到整档，后者带小数）。
 *
 * 单位与精度（写在契约外的展示约定，改这里就是改全部页面）：
 * - 字节：IEC 1024 进制 `B` / `KiB` / `MiB` / `GiB` / `TiB`；`B` 档取整、其余档固定 1 位小数。
 *   定标 + 固定小数位的先例是 btop 的 `shorten` 模式：本场景是按列对齐的实时表格，
 *   **宽度稳定优先于单值精度**（Grafana / Netdata 的有效数字自适应会让每秒刷新的列宽跳动）。
 * - 速率 = 字节值 + `/s`。
 * - 百分比 1 位、负载 2 位、延迟 2 位、温度 1 位、IOPS 与计数取整、计数带千分位。
 * - 毫秒（时钟偏差 / 误差界，契约 sysmon §2.9）1 位；偏差带符号（正 = 本地快）。
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

/**
 * 毫秒量（时钟偏差 / 误差界）：固定 1 位 + `ms`（`12.3 ms`）；不可用时为不可用占位。
 *
 * 与 {@link formatLatency} 的 2 位精度**刻意不同**：延迟是磁盘 I/O 的派生量，抖动在小数第二
 * 位有意义；时钟偏差是 NTP 采样给出的量，1 位毫秒已是判据分辨率（契约 sysmon §2.8 的阈值
 * 都以整毫秒给出），多给一位只会让每秒刷新的数字无谓跳变。
 */
export function formatMilliseconds(milliseconds: number | null | undefined): string {
  if (!isFiniteNumber(milliseconds)) return UNAVAILABLE_TEXT
  return `${milliseconds.toFixed(1)} ms`
}

/**
 * 带符号毫秒量（时钟偏差）：正值加 `+`（`+12.3 ms` / `-12.3 ms` / `0.0 ms`）。
 *
 * 符号是**语义的一部分**，不是排版偏好：正 = 本地墙钟快于参考源，负 = 本地慢，方向决定该往
 * 哪边修（契约 sysmon §2.9）。恰好 0 无方向可表，按既有 `formatSignedVolume` 的先例不加符号。
 */
export function formatSignedMilliseconds(milliseconds: number | null | undefined): string {
  if (!isFiniteNumber(milliseconds)) return UNAVAILABLE_TEXT
  const text = formatMilliseconds(milliseconds)
  return milliseconds > 0 ? `+${text}` : text
}

/**
 * **紧凑**带符号毫秒量（顶栏 NTP 徽章，ADR 0048）：取整 + 无空格单位（`+12ms` / `-3ms` / `0ms`）。
 *
 * 与 {@link formatSignedMilliseconds} 的取舍**刻意不同**：顶栏是 32px 常驻窄条，`+12.3 ms`
 * 比 CPU 徽章宽一倍多，而偏差判据是 100ms / 1000ms 档（整数分辨率足够）；1 位小数的精确值
 * 仍在徽章的悬停 title 里（那里用 `formatSignedMilliseconds`，不丢精度）。这与「顶栏整数百分比」
 * 是同一类取舍（见本文件头「明确的例外」与组件文档）。取整用 `Math.round`，四舍五入到最近整毫秒。
 */
export function formatCompactSignedMilliseconds(milliseconds: number | null | undefined): string {
  if (!isFiniteNumber(milliseconds)) return UNAVAILABLE_TEXT
  const rounded = Math.round(milliseconds)
  // 恰好 -0.4 取整后是 0，「负数但显示 0」是误导，符号只跟取整后的值走
  const text = `${Math.abs(rounded)}ms`
  if (rounded > 0) return `+${text}`
  if (rounded < 0) return `-${text}`
  return text
}

/**
 * 「多久以前」口径（秒 → 整档时长）：`30 秒` / `8 分钟` / `2 小时` / `1 天`。
 *
 * 向下取整的整档口径，**不给小数**：它回答的是「大致多久之前」，不是精确时长；四舍五入会
 * 把 14 分 59 秒说成「15 分钟」（与 {@link formatDuration} 的分钟向下取整同一理由）。
 * 由监控页（页文件年龄）与顶栏（时钟上次探测时间）共用，**不得**在视图里各写一份。
 */
export function formatAge(seconds: number | null | undefined): string {
  if (!isFiniteNumber(seconds) || seconds < 0) return UNAVAILABLE_TEXT
  const total = Math.floor(seconds)
  if (total < 60) return `${total} 秒`
  if (total < 3_600) return `${Math.floor(total / 60)} 分钟`
  if (total < 86_400) return `${Math.floor(total / 3_600)} 小时`
  return `${Math.floor(total / 86_400)} 天`
}

/** 去掉小数部分的尾随零（`60.0` → `60`），整数部分不动（`600` 不是 `6`） */
function trimFraction(text: string): string {
  if (!text.includes('.')) return text
  return text.replace(/0+$/, '').replace(/\.$/, '')
}

/**
 * 毫秒形态能留原样的最大字符数（#382）。
 *
 * **16 是实测边界**：11px tabular-nums 下 15 字符 = 84.86px（88px 列装得下）、
 * 16 字符 = 91.16px（装不下）。不是估的——在真实台账单元格里量的，字号/字体/字距都取自
 * 那一个元素的 `getComputedStyle`。列宽若以后变了，这个数要跟着重测（测试里有对应断言）。
 */
const MS_FORM_MAX_CHARS = 15

/**
 * 秒值 → **3 位有效数字**的紧凑文本（尾随零去掉）：`0.001` / `1.5` / `59.9` / `600`。
 *
 * 3 位有效数字是**显示精度**的取舍，和 {@link formatBytes} 的「档位 + 1 位小数」同一类：
 * 阈值列的列宽是定值（88px），而契约允许的阈值要到 6 位数毫秒。逐毫秒精确的「判据分辨率」
 * 只在默认档（100 / 1000 ms）上有意义，那一档由 {@link formatClockThreshold} 原样保留。
 *
 * 为什么不是「固定 1 位小数」：`0.001 s` 会被抹成 `0.0 s`（阈值写成 0 是编造）；
 * 为什么不是「固定 3 位小数」：`±59.999 / ±599.999 s` 实测 105.6px，仍撑爆 88px 列。
 *
 * 舍入方向由调用方给：**warn 向下、error 向上**。两个值用相反方向，保证 `warn < error`
 * 的配置在显示上不会塌成同一个数——`floor(w) ≤ w < e ≤ ceil(e)` 使两侧相等不可能成立，
 * 否则 `warn=59999 / error=60000` 会显示成 `±60 / ±60 s`，「两档」这个事实就没了。
 * 方向上还有一层意思：warn 宁可报低（早示警不是坏消息），error 宁可报高（不虚报弹窗线）。
 */
function compactSeconds(seconds: number, direction: 'floor' | 'ceil' | 'nearest'): string {
  const magnitude = Math.abs(seconds)
  if (magnitude === 0) return '0'
  // 2 − 数量级 = 「保留 3 位有效数字」所需的小数位；下界 0、上界 3（毫秒输入最多到 0.001 s）
  const decimals = Math.min(3, Math.max(0, 2 - Math.floor(Math.log10(magnitude))))
  // 先按小数位放大再取整：直接在浮点值上 floor 会在 0.001 × 1000 这类位置上掉一档。
  // 取整只作用在**绝对值**上，符号单独还原——否则负值的「向下取整」会变成向零取整。
  const scale = 10 ** decimals
  const scaled = magnitude * scale
  const rounded = direction === 'floor'
    ? Math.floor(scaled + 1e-6)
    : direction === 'ceil' ? Math.ceil(scaled - 1e-6) : Math.round(scaled)
  const text = trimFraction((rounded / scale).toFixed(decimals))
  return seconds < 0 && rounded !== 0 ? `-${text}` : text
}

/**
 * 时钟偏差的两档阈值 → 阈值列文案（#382）。**唯一实现**：台账阈值列与将来的第二个消费方都走这里。
 *
 * 折算规则（三条，按序命中）：
 * 1. **原样毫秒装得下就不折算**（判据见下）：`±100 / ±1000 ms`、`±200 / ±2000 ms`、
 *    `±500 / ±2000 ms`、`±1600 / ±2000 ms` 全部保持原形。**契约默认值逐字符不变**：这是最常见的
 *    配置，改它等于改所有人的肌肉记忆，也会让 #363 对默认宽度的实测白做。
 * 2. 两档都能被整分钟整除 → 分钟：`±1 / ±10 min`（契约上限 `60000 / 600000` 即此形）。
 * 3. 其余按秒、3 位有效数字：`±0.001 / ±600 s`、`±1.23 / ±500 s`。
 *
 * **边界为什么按「位数」而不是按 `error ≤ 1000`**（审查 finding，第一版就是后者）：
 * `error ≤ 1000` 既不是默认值边界（默认值只需 `(100,1000)` 保持原样），也不是宽度边界 ——
 * 实测 `±200 / ±2000 ms` 与默认档**同宽 84.86px**，却会被那条边界推去秒档变成 `±0.2 / ±2 s`，
 * 在用户没要求的情况下改了他读到的东西。而仓内后端测试自用的 `500/2000`、`250/3000`、
 * `1600/2000`、`300/1200` 全在那条边界外。真正放不下 88px 的是**两档各 4 位及以上**：
 * `±999 / ±9999 ms` 84.86px（装得下）、`±9999 / ±9999 ms` 91.16px（装不下）、
 * `±1000 / ±10000 ms` 97.44px（装不下）。所以判据取「整数部分位数之和 ≤ 8」——
 * 它同时覆盖上面那批真实配置，又把真放不下的那批送去折算。
 *
 * 为什么放开到 3 位有效数字而不是逐毫秒：契约允许的两档都是**毫秒整数**，折算只发生在
 * 秒/分钟档；此时 ±0.1% 的相对误差对「离弹窗线还有多远」没有影响，而精确值始终在「配置」
 * tab 里以整数毫秒给出（`offset_warn_ms`），台账只承担扫读。反过来若坚持逐毫秒精确，
 * 契约上限附近的合法配置（`59999 / 599999`）要 105.6px，只能改列宽——那会挤掉七列布局的
 * 弹性列（#382 方案 1 的代价）。
 *
 * 不采纳「去掉重复的 ± / 单位只写一次」（#382 方案 2）：它要么改掉默认值文案（硬约束），
 * 要么按分支切换符号，同一列出现两种句式。
 *
 * 两条边界性质（#382 单测锁定）：
 * - **默认值逐字符不变**：`(100, 1000) → '±100 / ±1000 ms'`；
 * - **两档在显示上永不相等**：ms 档两值本身就是整数；min 档只有 `1 / 10`；s 档两侧方向相反。
 *
 * @param warnMs 生效配置的 `clock.offset_warn_ms`（毫秒整数）
 * @param errorMs 生效配置的 `clock.offset_error_ms`（毫秒整数）
 */
export function formatClockThreshold(warnMs: number, errorMs: number): string {
  if (!isFiniteNumber(warnMs) || !isFiniteNumber(errorMs)) return UNAVAILABLE_TEXT
  // 判据直接取**生成的文本长度**，不猜「位数关系」：实测（11px tabular-nums，真实台账单元格）
  // 15 字符 = 84.86px（88px 列装得下）、16 字符 = 91.16px（装不下）。所以毫秒形态能不能留，
  // 等价于它自己有多长 —— 数长度不会因为换个数字组合就失效，数位数会
  // （`9999/9999` 位数和 8 但 16 字符装不下；`1/600000` 位数和 7 且 15 字符装得下）。
  const asMilliseconds = `±${warnMs} / ±${errorMs} ms`
  if (asMilliseconds.length <= MS_FORM_MAX_CHARS) return asMilliseconds
  // 分钟档：两档都是整分钟（`warn` 上限恰为 60000，故实际只可能是 `1 min` 这一档）
  if (warnMs % 60_000 === 0 && errorMs % 60_000 === 0) {
    return `±${warnMs / 60_000} / ±${errorMs / 60_000} min`
  }
  // 秒档：warn 向下、error 向上（见 compactSeconds 的方向论证）
  return `±${compactSeconds(warnMs / 1_000, 'floor')} / ±${compactSeconds(errorMs / 1_000, 'ceil')} s`
}

/**
 * 时钟偏差**余量**（毫秒，非负）→ 余量列文案（#382）。
 *
 * 与 {@link formatClockThreshold} 同一套有效数字策略，但保留「1 位小数毫秒」的既有档
 * （`987.5 ms`，误差界判据就在这个量级上），只有 `≥ 1 s` 才换秒——余量按 error 档算，
 * 契约上限是 600 s，而定宽余量列（全档统一 88px）装不下 6 位毫秒
 * （`599984.7 ms` 实测 65.4px，加上「已超 」前缀 96.6px 更放不下）。舍入取**就近**：这里是读数不是阈值，
 * 不承担「两档不得塌在一起」的义务。
 *
 * **负值保留负号**（`-12.5 ms` / `-1 s`），不在这里加「已超」前缀：本函数是纯格式化，
 * 前缀属于「这是越线」的语义判断，由调用方按各自口径决定——字节/点数余量走
 * `monitorLedger` 的 `headroomText`（判符号后改写为「已超 X」），时钟行则直接把带符号值
 * 交给本函数（负号即越线，与 `formatSignedMilliseconds` 的符号口径一致）。
 */
export function formatClockHeadroom(milliseconds: number): string {
  if (!isFiniteNumber(milliseconds)) return UNAVAILABLE_TEXT
  // 先按毫秒的展示精度（1 位小数）取整，再决定单位：否则 999.95 ms 会四舍五入成
  // `1000.0 ms`——跨过了 1 s 却仍停在毫秒档，比换成 `1 s` 更难读。
  // 取整后可能出现 `-0`（如 error 1000 − |offset| 1000.04 → `Math.round(-0.4)/10`）。
  // **不需要单独兜住**：`(-0).toFixed(1)` 在 V8 里就是 `"0.0"`（实测），负零天然不会漏出来。
  // 这里不加分支——加了也测不出来（审查 finding：那是一条不可观测的死代码），
  // 留一句说明比留一段永远不进的分支诚实。
  const rounded = Math.round(milliseconds * 10) / 10
  if (Math.abs(rounded) < 1_000) return `${rounded.toFixed(1)} ms`
  // 极端情形：`offset_ms` 在契约里**没有上界**（§2.9 只说「有符号偏差」），所以余量理论上
  // 可以到 `1e9 s` 量级（`-1e12 ms`）。那时 `compactSeconds` 的 3 位有效数字退化为原样输出，
  // `已超 1000000000 s` 约 96px、会溢出 88px 列。这是需要时钟坏到「偏差几十年」才会出现的
  // 路径，本函数不做上限截断——截断意味着编造一个契约没有的上限。若真要兜，正确位置是
  // 数据侧（把这种偏差当成「时钟不可信」而不是「余量很大」），属另一票。
  return `${compactSeconds(rounded / 1_000, 'nearest')} s`
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

/**
 * 平台描述 → 发行版名：剥掉末尾的 `(kernel …)` / `(build …)` 括号段（#430）。
 *
 * 为什么需要派生：契约 §2.5 的 `platform_description` 在 Linux 上形如
 * `Ubuntu 24.04.1 LTS (kernel 6.8.0-45-generic)`、Windows 上形如 `Windows 10 Pro (build 19045)`，
 * 而主机条把「内核」单列（`platform.kernel`），若系统字段直接用原值，同一事实会在一行里出现两次
 * （#373 的重复根因）。派生只发生在展示层：不改契约、不改发送方，原始值仍在 title 与
 * 历史/配置视图的副标题里可见。
 *
 * 规则：只剥**末尾**的 `(kernel …)` / `(build …)`（两段之间可以有空格），剥完为空则返回空串，
 * 由调用方决定占位（视图侧统一显示 `—`）。其它括号段（如发行版名自带的代号）不动。
 */
export function formatPlatformName(description: string | null | undefined): string {
  if (typeof description !== 'string') return ''
  const trimmed = description.trim()
  if (trimmed === '') return ''
  return trimmed.replace(/\s*\((?:kernel|build)\s[^)]*\)\s*$/i, '').trim()
}

/**
 * 运行时长（秒 → 参考图口径的「已运行」文案，#430）：`1 天 23 小时` / `22 小时 1 分` / `3 分钟`。
 *
 * 与 {@link formatDuration} 刻意不同：那个是「覆盖时长」（天/小时保留 1 位小数，用于
 * 「最近 15 分钟」这类口径），这里是**人类读的机器运行时长**——不保留小数、不显示秒
 * （秒级跳动对「这台机器跑了多久」没有信息量，反而让主机条每秒都在变）。向下取整。
 */
export function formatUptime(seconds: number | null | undefined): string {
  if (!isFiniteNumber(seconds) || seconds < 0) return UNAVAILABLE_TEXT
  const total = Math.floor(seconds)
  if (total >= 86_400) return `${Math.floor(total / 86_400)} 天 ${Math.floor((total % 86_400) / 3_600)} 小时`
  if (total >= 3_600) return `${Math.floor(total / 3_600)} 小时 ${Math.floor((total % 3_600) / 60)} 分`
  if (total >= 60) return `${Math.floor(total / 60)} 分钟`
  return `${total} 秒`
}

/**
 * 墙钟时刻（Unix 毫秒 → `HH:MM:SS`；与参考时刻不是同一天时带 `MM-DD`，#430）。
 *
 * 主机条的状态槽是「更新于 …」：日常盯盘只关心今天几点几分几秒，日期是噪声；而页面跨夜常开时
 * 只给时分秒会让人误读成今天——所以只在跨日时补 `MM-DD`。参考时刻由调用方给（组件里是每秒
 * 推进的 `nowMs`），本函数不读系统时钟，便于单测固定时间点。
 */
export function formatClockTime(unixMs: number | null | undefined, referenceNowMs: number): string {
  if (!isFiniteNumber(unixMs) || !isFiniteNumber(referenceNowMs)) return UNAVAILABLE_TEXT
  const date = new Date(unixMs)
  if (Number.isNaN(date.getTime())) return UNAVAILABLE_TEXT
  const now = new Date(referenceNowMs)
  const pad = (n: number): string => String(n).padStart(2, '0')
  const clock = `${pad(date.getHours())}:${pad(date.getMinutes())}:${pad(date.getSeconds())}`
  const sameDay = date.getFullYear() === now.getFullYear()
    && date.getMonth() === now.getMonth()
    && date.getDate() === now.getDate()
  return sameDay ? clock : `${pad(date.getMonth() + 1)}-${pad(date.getDate())} ${clock}`
}
