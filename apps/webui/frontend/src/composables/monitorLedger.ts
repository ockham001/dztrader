/**
 * 资源监控台账的展示口径（#361；契约 sysmon §2、§2.8、§3.1）
 *
 * 契约是唯一语义真相源：§2 定义各域的 payload 与可用性映射，§2.8 定义资源类阈值与两档色标，
 * §3.1 定义 `thresholds` / `clock` 两个配置段。本模块只承载**展示侧的口径**——把已净化的
 * store 数据翻译成「分组 → 行 → 七列取值」，不含协议判断（净化在 `stores/sysmon.ts`）、
 * 不含渲染（在 `views/MonitorView.vue`）。
 *
 * 五条不可动摇的约定：
 *
 * 1. **阈值列只写契约里真实存在的阈值。** `thresholds` 段只有内存 / 交换 / 卷空闲三个资源类阈值
 *    （CPU 持续饱和那两个判的是核，不是台账行），时钟的两个在 `clock` 段。磁盘 I/O 的忙碌占比、
 *    网卡、共享内存目录**没有**契约阈值，一律写 `—`。不得为了让表格整齐而编造阈值。
 *
 * 2. **余量按契约阈值算，不按 70/90 算。** `RESOURCE_WARN_RATIO` / `RESOURCE_DANGER_RATIO` 是
 *    前端展示分档（ADR 0034 明文：色标是独立展示规则，不是越限判定），只决定 `level` 与轨道
 *    缺口位置，**不参与** `headroomText`。两套数字混用会让「余量 1.2 GiB」与「阈值 90%」互相
 *    对不上。
 *
 * 3. **行序是固定顺序，不是余量顺序。** 与 ADR 0048 同口径：卷按挂载深度升序 → 挂载点字符串
 *    升序（store 的 `readableVolumes` 已排好，本模块**保持传入顺序**）。每 5 秒推送重排一次会让
 *    同一块盘在行与行之间来回换位，扫读位置不稳定。
 *
 * 4. **四类空态互不混淆，任何一类都不产出数字。** 不可用（能力缺失，带 reason）/ 暂无数据
 *    （能力可用但本帧无值）/ 配置关闭（`disabled_by_config`）/ 无此维度（如本机无交换分区）。
 *    用 0 冒充任何一种都是撒谎（契约 §2 的「不得用 0 冒充」贯穿全篇）。
 *
 * 5. **`level` 为 null 表示「无法定档」，不得冒充 `normal`。** 没有比率分母的行（网卡 / 共享
 *    内存 / 时钟）不套任何状态色——「没法判」与「一切正常」是两件事。
 */
import {
  UNAVAILABLE_TEXT,
  formatByteRate,
  formatBytes,
  formatCount,
  formatDecimal,
  formatIops,
  formatLatency,
  formatPercent,
  formatSignedMilliseconds,
} from '@/composables/format'
import {
  clockDomainState,
  clockOffsetLevel,
  clockProbeAgeSec,
  clockServerCells,
  resolveClockThresholds,
} from '@/composables/sysmonClock'
import { resourceLevel, type ResourceLevel } from '@/composables/useResourceLevel'
import type {
  SysmonAvailabilityEntry,
  SysmonClockConfigPayload,
  SysmonClockInfo,
  SysmonDiskIoInfo,
  SysmonMemoryInfo,
  SysmonNetworkInfo,
  SysmonShmDirInfo,
  SysmonSwapInfo,
  SysmonThresholdsConfigPayload,
  SysmonVolumeInfo,
} from '@/types/generated'

// ---- 契约默认阈值（§3.1 的副本）----
// 发送方是唯一真相源：`dzsysmon` 未收到配置帧时按这些默认值工作，前端在「配置镜像尚未到达」
// 的窗口内必须给出同一结论，否则台账会先按一套阈值显示余量、配置到达后跳变。与
// `sysmonClock.ts` 的 `CLOCK_DEFAULT_*` 同一先例（那里也只复制阈值、不复制数组型配置）。
export const LEDGER_DEFAULT_MEMORY_USED_PCT = 90
export const LEDGER_DEFAULT_SWAP_USED_PCT = 50
export const LEDGER_DEFAULT_VOLUME_FREE_PCT = 10

/** 台账分组标识（三组，顺序固定：内存 → 存储 → 网络与时钟） */
export type LedgerGroupId = 'memory' | 'storage' | 'network_clock'

/**
 * 行状态。`ok` 之外的四类是**四种不同的事实**，文案与着色都不同：
 * - `unavailable`：能力不可用（`availability.metrics` 说 false），带 reason
 * - `empty`：能力可用但本帧没有值（首次采样 / 采集粘性 / 全部被过滤）
 * - `disabled`：被配置关闭（属于 `unavailable` 的一种 reason，单独一档是因为它可自助恢复）
 * - `none`：该维度在本机不存在（如无交换分区），不是故障
 */
export type LedgerRowState = 'ok' | 'unavailable' | 'empty' | 'disabled' | 'none'

/** 问号提示型明细项（展开后按「标签 / 值」两列渲染） */
export interface LedgerField {
  label: string
  value: string
}

/** 台账的一行：行内容即七列的取值，视图只负责摆放 */
export interface LedgerRow {
  /** 稳定标识（`v-for` 的 `:key` 与测试断言用），形如 `memory` / `volume:/data` / `nic:eno1:rx` */
  id: string
  group: LedgerGroupId
  /** 行的中文名（指标名 / 挂载点 / 设备名 / 网卡名） */
  label: string
  /** 契约里的指标键（§2 的可用性映射键），作为一行小字显示，便于对照日志与契约 */
  contractKey: string
  state: LedgerRowState
  /** 状态文案：`ok` 时为空串，其余四类各给一句（含 reason 的原文兜底） */
  stateText: string
  /** 等级的文字形态：正常 / 警戒 / 危险；无等级可判时为空串（状态不得只靠颜色，契约随 DESIGN.md） */
  levelText: string
  /** 量程轨道的填充比例 [0,1]；null = 该行没有可比的分母（网卡 / 共享内存 / 时钟） */
  ratio: number | null
  /** 由 ratio 经 `useResourceLevel` 定档；时钟行由 `clockOffsetLevel` 按配置阈值定档 */
  level: ResourceLevel | null
  /** 当前值（已格式化；空态时为空串，**不得**是 0 的格式化结果） */
  value: string
  /** 一行的明细（已格式化） */
  detail: string
  /** 该指标真实的契约阈值；没有契约阈值时为 `—` */
  thresholdText: string
  /** 到契约阈值的余量；没有契约阈值时为 `—`，已越线时为「已超 X」 */
  headroomText: string
  /** 非 null 时该行数值可点，跳历史视图并预选此指标（契约 §6.1） */
  historyKey: string | null
  /** 展开后的完整明细 */
  fields: LedgerField[]
}

/** 台账的一个分组 */
export interface LedgerGroup {
  id: LedgerGroupId
  title: string
  /** 组标题右侧的一句注（该组的计数与总量口径） */
  note: string
  rows: LedgerRow[]
}

/**
 * 构造台账的输入：全部是 store 已净化的 computed 与生效配置，本模块**不读 store**。
 *
 * `availability` 传 `useSysmonStore().availabilityOf`；`nowUnixMs` 由调用方给当前时刻
 * （不在这里调 `Date.now()`——隐藏的时间输入会让「时钟上次探测多久以前」无法在测试里锁定）。
 */
export interface MonitorLedgerInput {
  memory: SysmonMemoryInfo | null
  swap: SysmonSwapInfo | null
  /** store 的 `readableVolumes`：已按 ADR 0048 排好序，本模块保持其顺序 */
  readableVolumes: readonly SysmonVolumeInfo[]
  /** 只用于组注里的计数（不进主列，与既往「无容量挂载」折叠口径一致） */
  zeroCapacityVolumes: readonly SysmonVolumeInfo[]
  diskIo: readonly SysmonDiskIoInfo[]
  network: readonly SysmonNetworkInfo[]
  shmDir: SysmonShmDirInfo | null
  clock: SysmonClockInfo | null
  /** 生效配置的 `thresholds` 段；null = 镜像未到达，按契约默认值回退 */
  thresholds: SysmonThresholdsConfigPayload | null
  /** 生效配置的 `clock` 段；null = 镜像未到达，按契约默认值回退 */
  clockConfig: SysmonClockConfigPayload | null
  availability: (metric: string) => SysmonAvailabilityEntry | undefined
  nowUnixMs: number
}

/**
 * 不可用原因的中文文案（契约 §2 的建议取值 + 未知原因原文兜底）。
 *
 * 这是监控页「缺数据怎么说」的**唯一**实现：台账与组注共用一份。不认识的原因原样显示而不是
 * 归并成「不可用」——把 `some_new_reason` 抹成「不可用」会让新增的采集失败原因永远不可见。
 */
export function unavailableReasonText(reason: string | undefined): string {
  if (!reason) return '当前快照未提供该指标'
  if (reason === 'not_implemented') return '该指标将在后续版本接入'
  if (reason === 'collect_failed') return '采集失败，暂不可用'
  if (reason === 'disabled_by_config') return '已在配置中关闭'
  if (reason === 'no_permission') return '权限不足，无法读取该指标'
  return reason
}

/** 等级 → 中文（状态不得只靠颜色；空等级返回空串，不冒充「正常」） */
export function ledgerLevelText(level: ResourceLevel | null): string {
  if (level === 'danger') return '危险'
  if (level === 'warn') return '警戒'
  if (level === 'normal') return '正常'
  return ''
}

/** 阈值列在没有契约阈值时的占位（不是「不可用」——该指标本来就没有阈值） */
export const NO_THRESHOLD_TEXT = '—'

/**
 * 余量 → 文案：非负走原单位，负数表示已越线，改写为「已超 X」。
 *
 * 不能直接交给 `formatBytes`：那个函数对负值返回「不可用」，而「已用超过阈值 1.5 GiB」是一个
 * 明确且重要的读数，抹成「不可用」等于把越线藏起来。
 *
 * **符号判定在调用方按展示精度取整后进行**（`pointsHeadroomText` 取 1 位小数、字节取整）：
 * `(1 − 0.9) × 100 − 10` 在浮点下等于 `−1.78e−15`，若按原始值判符号，恰好卡在阈值上的卷会
 * 显示成「已超 0.0 点」——一个精度噪声伪装成的越线结论。
 */
function headroomText(value: number, format: (n: number) => string): string {
  return value >= 0 ? format(value) : `已超 ${format(-value)}`
}

/** 百分点余量（卷的空闲、时钟的毫秒余量共用同一「点数」口径），按 1 位小数取整后判符号 */
function pointsHeadroomText(value: number): string {
  const rounded = Math.round(value * 10) / 10
  return headroomText(rounded, n => `${n.toFixed(1)} 点`)
}

/** 取阈值：配置已到达用它，未到达回退契约默认（配置镜像到达后不跳变） */
function thresholdOr(value: number | null | undefined, fallback: number): number {
  return typeof value === 'number' && Number.isFinite(value) ? value : fallback
}

/** 可用性三元判定：可用 / 不可用（带 reason）/ 未知（映射缺失按不可用处理，与 store 同口径） */
function availabilityState(availability: (metric: string) => SysmonAvailabilityEntry | undefined, metric: string): {
  available: boolean
  reason: string | undefined
} {
  const entry = availability(metric)
  return { available: entry?.available === true, reason: entry?.reason }
}

/** 由 ratio 构造 level（null 不冒充 normal） */
function levelOf(ratio: number | null): ResourceLevel | null {
  return ratio === null ? null : resourceLevel(ratio)
}

/** 空态行（`ok` 之外的统一构造：不产出 value / ratio / level，但保留标签与契约键） */
function blankRow(
  base: Pick<LedgerRow, 'id' | 'group' | 'label' | 'contractKey'>,
  state: Exclude<LedgerRowState, 'ok'>,
  stateText: string,
): LedgerRow {
  return {
    ...base,
    state,
    stateText,
    levelText: '',
    ratio: null,
    level: null,
    value: '',
    detail: '',
    thresholdText: NO_THRESHOLD_TEXT,
    headroomText: NO_THRESHOLD_TEXT,
    historyKey: null,
    fields: [],
  }
}

/**
 * 可用性 → 空态行。`disabled_by_config` 单独成 `disabled` 档：它是唯一可以自助恢复的一类，
 * 与「平台不支持」混在一起会让用户去查文档而不是去改配置。
 */
function unavailableRow(
  base: Pick<LedgerRow, 'id' | 'group' | 'label' | 'contractKey'>,
  metric: string,
  input: MonitorLedgerInput,
): LedgerRow {
  const { reason } = availabilityState(input.availability, metric)
  const state = reason === 'disabled_by_config' ? 'disabled' : 'unavailable'
  return blankRow(base, state, `${UNAVAILABLE_TEXT}：${unavailableReasonText(reason)}`)
}

/** 内存组：内存已用 + 交换已用（两行都是可点跳历史的标量） */
function memoryGroup(input: MonitorLedgerInput): LedgerGroup {
  const rows: LedgerRow[] = []
  const memoryPct = thresholdOr(input.thresholds?.memory_used_pct, LEDGER_DEFAULT_MEMORY_USED_PCT)
  const swapPct = thresholdOr(input.thresholds?.swap_used_pct, LEDGER_DEFAULT_SWAP_USED_PCT)
  const { available: memoryAvailable } = availabilityState(input.availability, 'memory')

  const memoryBase = {
    id: 'memory',
    group: 'memory' as const,
    label: '内存已用',
    contractKey: 'memory',
  }
  if (!memoryAvailable) {
    rows.push(unavailableRow(memoryBase, 'memory', input))
  } else if (input.memory === null) {
    rows.push(blankRow(memoryBase, 'empty', '暂无数据：本帧未采集到内存用量'))
  } else {
    const memory = input.memory
    const ratio = memory.used_ratio
    // 余量 = 阈值对应的字节上限 − 已用。用 threshold 而不是 ratio 差，是因为「还剩多少字节」
    // 比「还剩几个百分点」更贴近「这台机器还能不能再扛一个策略」这个原始问题。
    const limitBytes = memory.total_bytes * (memoryPct / 100)
    const headroom = limitBytes - memory.used_bytes
    rows.push({
      ...memoryBase,
      state: 'ok',
      stateText: '',
      level: levelOf(ratio),
      levelText: ledgerLevelText(levelOf(ratio)),
      ratio,
      value: formatPercent(ratio),
      detail: `${formatBytes(memory.used_bytes)} / ${formatBytes(memory.total_bytes)}`,
      thresholdText: `${memoryPct}%`,
      headroomText: headroomText(headroom, formatBytes),
      historyKey: 'memory.used_ratio',
      fields: [
        { label: '总量', value: formatBytes(memory.total_bytes) },
        { label: '已用（总量 − 可用）', value: formatBytes(memory.used_bytes) },
        { label: '可用', value: formatBytes(memory.available_bytes) },
        { label: '缓存（可回收）', value: formatBytes(memory.cached_bytes) },
        { label: '缓冲（内核元数据）', value: formatBytes(memory.buffers_bytes) },
      ],
    })
  }

  const swapBase = {
    id: 'swap',
    group: 'memory' as const,
    label: '交换已用',
    contractKey: 'swap',
  }
  const swapAvailability = availabilityState(input.availability, 'swap')
  if (!swapAvailability.available) {
    // 无交换分区是平台事实（契约 §2 的 `unsupported`），不是采集故障：给专用文案而不是
    // 通用的「不可用」，否则用户会去查为什么采集失败。
    if (swapAvailability.reason === 'unsupported') {
      rows.push(blankRow(swapBase, 'none', '本机无交换分区/页面文件'))
    } else {
      rows.push(unavailableRow(swapBase, 'swap', input))
    }
  } else if (input.swap === null) {
    rows.push(blankRow(swapBase, 'empty', '暂无数据：本帧未采集到交换分区'))
  } else if (input.swap.total_bytes === 0) {
    // 总量 0 是**合法事实**，不是 0% 的使用率：显示 0% 会被读成「有 swap 但一点没用」。
    rows.push(blankRow(swapBase, 'none', '无交换分区'))
  } else {
    const swap = input.swap
    const ratio = swap.used_ratio
    const limitBytes = swap.total_bytes * (swapPct / 100)
    const headroom = limitBytes - swap.used_bytes
    rows.push({
      ...swapBase,
      state: 'ok',
      stateText: '',
      level: levelOf(ratio),
      levelText: ledgerLevelText(levelOf(ratio)),
      ratio,
      value: formatPercent(ratio),
      detail: `${formatBytes(swap.used_bytes)} / ${formatBytes(swap.total_bytes)}`,
      thresholdText: `${swapPct}%`,
      headroomText: headroomText(headroom, formatBytes),
      historyKey: 'swap.used_ratio',
      fields: [
        { label: '总量', value: formatBytes(swap.total_bytes) },
        { label: '已用', value: formatBytes(swap.used_bytes) },
        { label: '剩余', value: formatBytes(Math.max(0, swap.total_bytes - swap.used_bytes)) },
      ],
    })
  }

  const memoryTotal = input.memory === null ? UNAVAILABLE_TEXT : formatBytes(input.memory.total_bytes)
  const swapTotalText = input.swap === null || input.swap.total_bytes === 0
    ? '无交换'
    : `交换 ${formatBytes(input.swap.total_bytes)}`
  return {
    id: 'memory',
    title: '内存',
    note: `${memoryTotal} 总量 · ${swapTotalText}`,
    rows,
  }
}

/** 单个卷的一行（`readableVolumes` 已保证 `used_ratio` 非 null） */
function volumeRow(volume: SysmonVolumeInfo, freePct: number): LedgerRow {
  const ratio = volume.used_ratio
  // 卷的判据是**空闲**低于阈值（契约 §3.1 `volume_free_pct`），因此余量算的是空闲百分点：
  // 用 `已用` 相减会得到符号相反的结论，这是本表最容易做错的一格。
  const freePoints = ratio === null ? 0 : (1 - ratio) * 100
  const headroom = freePoints - freePct
  const total = volume.total_bytes
  const used = volume.used_bytes
  return {
    id: `volume:${volume.mount}`,
    group: 'storage',
    label: volume.mount,
    contractKey: 'volumes',
    state: 'ok',
    stateText: '',
    level: levelOf(ratio),
    levelText: ledgerLevelText(levelOf(ratio)),
    ratio,
    value: formatPercent(ratio),
    detail: total === null || used === null
      ? UNAVAILABLE_TEXT
      : `${formatBytes(used)} / ${formatBytes(total)}`,
    thresholdText: `${freePct}% 空闲`,
    headroomText: pointsHeadroomText(headroom),
    historyKey: null,
    fields: [
      { label: '挂载点', value: volume.mount },
      { label: '总量', value: formatBytes(total) },
      { label: '已用', value: formatBytes(used) },
      { label: '可用', value: total === null || used === null ? UNAVAILABLE_TEXT : formatBytes(total - used) },
    ],
  }
}

/** 单块物理盘的一行：忙碌占比有填充与色阶，但**没有契约阈值**（故阈值/余量列为 —） */
function diskRow(disk: SysmonDiskIoInfo): LedgerRow {
  const ratio = disk.utilization
  const level = levelOf(ratio)
  return {
    id: `disk:${disk.device}`,
    group: 'storage',
    // 「忙碌占比」不是「利用率」：`utilization` 度量的是设备忙时间占比（契约 §2.5），
    // 叫「利用率」会被读成「吞吐占带宽的比例」。
    label: `${disk.device} 忙碌占比`,
    contractKey: 'disk_io',
    state: 'ok',
    stateText: '',
    level,
    levelText: ledgerLevelText(level),
    ratio,
    value: formatPercent(ratio),
    detail: `写 ${formatByteRate(disk.write_bytes_per_sec)} · 延迟 ${formatLatency(disk.latency_ms)}`,
    thresholdText: NO_THRESHOLD_TEXT,
    headroomText: NO_THRESHOLD_TEXT,
    historyKey: null,
    fields: [
      { label: '读吞吐', value: formatByteRate(disk.read_bytes_per_sec) },
      { label: '写吞吐', value: formatByteRate(disk.write_bytes_per_sec) },
      { label: '读 IOPS', value: formatIops(disk.read_iops) },
      { label: '写 IOPS', value: formatIops(disk.write_iops) },
      { label: '平均延迟', value: formatLatency(disk.latency_ms) },
      { label: '队列深度', value: formatDecimal(disk.queue_depth) },
      { label: '忙碌占比', value: formatPercent(ratio) },
    ],
  }
}

/** 共享内存目录：没有总容量分母，因此无轨道、无阈值；页数与最旧文件年龄才是风险信号 */
function shmRow(shm: SysmonShmDirInfo): LedgerRow {
  return {
    id: 'shm_dir',
    group: 'storage',
    label: '共享内存目录',
    contractKey: 'shm_dir',
    state: 'ok',
    stateText: '',
    level: null,
    levelText: '',
    ratio: null,
    value: formatBytes(shm.total_bytes),
    detail: `${formatCount(shm.page_files)} 页文件${
      shm.oldest_file_age_sec === null ? '' : ` · 最旧 ${formatAgedays(shm.oldest_file_age_sec)}`
    }`,
    thresholdText: NO_THRESHOLD_TEXT,
    headroomText: NO_THRESHOLD_TEXT,
    historyKey: null,
    fields: [
      { label: '路径', value: shm.path },
      { label: '占用', value: formatBytes(shm.total_bytes) },
      { label: '页文件数', value: formatCount(shm.page_files) },
      { label: '最旧页文件年龄', value: formatAgedays(shm.oldest_file_age_sec) },
    ],
  }
}

/** 秒 → 「N 天」，与 `formatAge` 同档口径但固定到最大档（页文件年龄跨月时读天更有意义） */
function formatAgedays(seconds: number | null): string {
  if (seconds === null || !Number.isFinite(seconds) || seconds < 0) return UNAVAILABLE_TEXT
  const days = Math.floor(seconds / 86_400)
  if (days >= 1) return `${days} 天`
  const hours = Math.floor(seconds / 3_600)
  if (hours >= 1) return `${hours} 小时`
  return `${Math.floor(seconds / 60)} 分钟`
}

/** 存储组：卷（保持 store 顺序）→ 物理盘（按 device 升序）→ 共享内存目录 */
function storageGroup(input: MonitorLedgerInput): LedgerGroup {
  const rows: LedgerRow[] = []
  const freePct = thresholdOr(input.thresholds?.volume_free_pct, LEDGER_DEFAULT_VOLUME_FREE_PCT)
  const { available: volumesAvailable } = availabilityState(input.availability, 'volumes')

  if (!volumesAvailable) {
    rows.push(unavailableRow(
      { id: 'volumes', group: 'storage', label: '卷容量', contractKey: 'volumes' },
      'volumes',
      input,
    ))
  } else if (input.readableVolumes.length === 0) {
    rows.push(blankRow(
      { id: 'volumes', group: 'storage', label: '卷容量', contractKey: 'volumes' },
      'empty',
      '暂无数据：本帧没有容量可读的挂载点',
    ))
  } else {
    for (const volume of input.readableVolumes) rows.push(volumeRow(volume, freePct))
  }

  const { available: diskIoAvailable } = availabilityState(input.availability, 'disk_io')
  if (!diskIoAvailable) {
    rows.push(unavailableRow(
      { id: 'disk_io', group: 'storage', label: '磁盘 I/O', contractKey: 'disk_io' },
      'disk_io',
      input,
    ))
  } else if (input.diskIo.length === 0) {
    rows.push(blankRow(
      { id: 'disk_io', group: 'storage', label: '磁盘 I/O', contractKey: 'disk_io' },
      'empty',
      '暂无数据：本帧没有物理盘 I/O 读数',
    ))
  } else {
    // 按 device 升序：store 已排好，这里再排一次是为了让「传入未排序数组」也不会让行序抖动
    // ——行序稳定是本表的核心承诺（约定 3）。
    for (const disk of [...input.diskIo].sort((a, b) => a.device.localeCompare(b.device))) {
      rows.push(diskRow(disk))
    }
  }

  const { available: shmAvailable } = availabilityState(input.availability, 'shm_dir')
  if (!shmAvailable) {
    rows.push(unavailableRow(
      { id: 'shm_dir', group: 'storage', label: '共享内存目录', contractKey: 'shm_dir' },
      'shm_dir',
      input,
    ))
  } else if (input.shmDir === null) {
    rows.push(blankRow(
      { id: 'shm_dir', group: 'storage', label: '共享内存目录', contractKey: 'shm_dir' },
      'empty',
      '暂无数据：本帧未采集到共享内存目录',
    ))
  } else {
    rows.push(shmRow(input.shmDir))
  }

  const volumeCount = volumesAvailable ? input.readableVolumes.length : 0
  const zeroCount = input.zeroCapacityVolumes.length
  const diskCount = diskIoAvailable ? input.diskIo.length : 0
  const noteParts = [`${volumeCount} 个挂载点`, `${diskCount} 块磁盘`]
  if (zeroCount > 0) noteParts.push(`${zeroCount} 个无容量挂载`)
  return { id: 'storage', title: '存储', note: noteParts.join(' · '), rows }
}

/** 单张网卡的一半（接收或发送）：两个方向各自一行，各自带自己的五项明细 */
function nicRow(nic: SysmonNetworkInfo, direction: 'rx' | 'tx'): LedgerRow {
  const isRx = direction === 'rx'
  const rate = isRx ? nic.rx_bytes_per_sec : nic.tx_bytes_per_sec
  const errorsPerSec = isRx ? nic.rx_errors_per_sec : nic.tx_errors_per_sec
  const dropsPerSec = isRx ? nic.rx_drops_per_sec : nic.tx_drops_per_sec
  const errorsTotal = isRx ? nic.rx_errors_total : nic.tx_errors_total
  const dropsTotal = isRx ? nic.rx_drops_total : nic.tx_drops_total
  const bytesTotal = isRx ? nic.rx_bytes_total : nic.tx_bytes_total
  const packetsTotal = isRx ? nic.rx_packets_total : nic.tx_packets_total
  const dirLabel = isRx ? '接收' : '发送'
  return {
    id: `nic:${nic.interface}:${direction}`,
    group: 'network_clock',
    label: `${nic.interface} ${dirLabel}`,
    contractKey: 'network',
    state: 'ok',
    stateText: '',
    // 网卡没有链路速率字段（`SysmonNetworkInfo` 不提供），无法归一化到量程：
    // ratio 必须为 null 而不是拿一个编造的分母算百分比。
    level: null,
    levelText: '',
    ratio: null,
    value: formatByteRate(rate),
    // 累计错误与丢弃是这张卡的风险信号，放在明细里让它不点开也可见（不是藏在展开里）。
    detail: `错 ${formatCount(errorsTotal)} 累计 · 丢 ${formatCount(dropsTotal)} 累计`,
    thresholdText: NO_THRESHOLD_TEXT,
    headroomText: NO_THRESHOLD_TEXT,
    historyKey: null,
    fields: [
      { label: `${dirLabel}速率`, value: formatByteRate(rate) },
      { label: `${dirLabel}错误速率`, value: formatCountPerSecOf(errorsPerSec) },
      { label: `${dirLabel}丢弃速率`, value: formatCountPerSecOf(dropsPerSec) },
      { label: `${dirLabel}字节（累计）`, value: formatCount(bytesTotal) },
      { label: `${dirLabel}包数（累计）`, value: formatCount(packetsTotal) },
      { label: `${dirLabel}错误（累计）`, value: formatCount(errorsTotal) },
      { label: `${dirLabel}丢弃（累计）`, value: formatCount(dropsTotal) },
      { label: '虚拟网卡', value: nic.is_virtual ? '是（不由硬件设备支撑）' : '否' },
    ],
  }
}

/** 每秒事件数（1 位小数，与「错误/丢弃速率」口径一致） */
function formatCountPerSecOf(value: number | null): string {
  if (value === null) return UNAVAILABLE_TEXT
  return `${value.toFixed(1)}/s`
}

/**
 * 时钟偏差行：阈值是**绝对毫秒**（不是比率），因此 ratio 恒为 null、level 由
 * `clockOffsetLevel` 按生效配置的 warn / error 两档定档——与卡片共用同一实现，
 * 不构造伪 ratio（那会让「档位阈值来自配置」这个事实消失）。
 */
function clockRow(clock: SysmonClockInfo | null, input: MonitorLedgerInput): LedgerRow {
  const base = {
    id: 'clock',
    group: 'network_clock' as const,
    label: '时钟偏差',
    contractKey: 'clock.offset',
  }
  const domain = clockDomainState(clock)
  if (domain === 'disabled') return blankRow(base, 'disabled', `${UNAVAILABLE_TEXT}：${unavailableReasonText('disabled_by_config')}`)
  if (domain === 'unprobed') return blankRow(base, 'none', '尚未探测')
  if (domain === 'unavailable' || clock === null) {
    return blankRow(base, 'unavailable', `${UNAVAILABLE_TEXT}：本轮探测无可用参考源`)
  }

  const thresholds = resolveClockThresholds(input.clockConfig)
  const level = clockOffsetLevel(clock.offset_ms, thresholds)
  const age = clockProbeAgeSec(clock.last_probe_unix_ms, input.nowUnixMs)
  // 余量按 **error** 档算：warn 只是提前示警，能不能再等一轮要看离弹窗档还有多远。
  const errorHeadroom = clock.offset_ms === null
    ? null
    : thresholds.error_ms - Math.abs(clock.offset_ms)
  const fields: LedgerField[] = clock.servers.map(server => ({
    label: server.host,
    value: clockServerCells(server).map(cell => `${cell.label} ${cell.value}`).join(' · '),
  }))
  fields.push({
    label: '时间同步服务',
    value: clock.daemons.length === 0
      ? '未检测到运行中的时间同步服务'
      : clock.daemons.map(daemon => `${daemon.name}${daemon.running ? '（运行中）' : '（未运行）'}`).join('、'),
  })
  return {
    ...base,
    state: 'ok',
    stateText: '',
    level,
    levelText: ledgerLevelText(level),
    ratio: null,
    value: formatSignedMilliseconds(clock.offset_ms),
    detail: `误差界 ±${formatMillisecondsPlain(clock.bound_ms)} · ${clock.source || '无来源'}${
      age === null ? '' : ` · ${formatAgedays(age)}前`
    }`,
    // 阈值列写两档（warn / error）：时钟的判据是「进入哪一档」，只写一档会丢掉另一档。
    thresholdText: `±${thresholds.warn_ms} / ±${thresholds.error_ms} ms`,
    headroomText: errorHeadroom === null ? NO_THRESHOLD_TEXT : `${errorHeadroom.toFixed(1)} ms`,
    historyKey: null,
    fields,
  }
}

/** 毫秒量去掉单位后缀（`±39.3 ms` 的 detail 里已带 ±，避免出现 `±39.3 ms ms`） */
function formatMillisecondsPlain(milliseconds: number | null): string {
  if (milliseconds === null || !Number.isFinite(milliseconds)) return UNAVAILABLE_TEXT
  return `${milliseconds.toFixed(1)} ms`
}

/** 网络与时钟组：每张网卡两个方向 + 时钟偏差 */
function networkClockGroup(input: MonitorLedgerInput): LedgerGroup {
  const rows: LedgerRow[] = []
  const { available: networkAvailable } = availabilityState(input.availability, 'network')
  if (!networkAvailable) {
    rows.push(unavailableRow(
      { id: 'network', group: 'network_clock', label: '网卡', contractKey: 'network' },
      'network',
      input,
    ))
  } else if (input.network.length === 0) {
    rows.push(blankRow(
      { id: 'network', group: 'network_clock', label: '网卡', contractKey: 'network' },
      'empty',
      '暂无数据：本帧没有网卡读数（可能已被「显示全部网卡」过滤）',
    ))
  } else {
    for (const nic of [...input.network].sort((a, b) => a.interface.localeCompare(b.interface))) {
      rows.push(nicRow(nic, 'rx'))
      rows.push(nicRow(nic, 'tx'))
    }
  }
  rows.push(clockRow(input.clock, input))

  const nicCount = networkAvailable ? input.network.length : 0
  const shmText = input.shmDir === null ? '共享内存不可用' : `共享内存 ${formatBytes(input.shmDir.total_bytes)}`
  return {
    id: 'network_clock',
    title: '网络与时钟',
    note: `${nicCount} 块网卡 · ${shmText}`,
    rows,
  }
}

/**
 * 构造资源监控台账（三组、固定顺序）。
 *
 * 行数由数据决定（卷数 / 盘数 / 网卡数），不是常量：8 个卷就是 8 行，0 张网卡就是 0 行。
 * 视图只负责按行渲染，不写任何「该不该显示」的判断。
 */
export function buildMonitorLedger(input: MonitorLedgerInput): LedgerGroup[] {
  return [memoryGroup(input), storageGroup(input), networkClockGroup(input)]
}

// ---- CPU 趋势的自适应纵轴 ----

/** 自适应纵轴的取值：上下界 + 三档刻度（刻度是渲染用的标签，不是数据点） */
export interface AdaptiveAxis {
  /** 纵轴下界（含外扩） */
  lo: number
  /** 纵轴上界（含外扩） */
  hi: number
  /** 三档刻度：下界 / 中点 / 上界 */
  labels: { value: number; text: string }[]
}

/** 外扩比例：序列满幅上下各留 10%，读数不会贴着上下边框 */
const AXIS_PADDING_RATIO = 0.1
/** 等值序列的最小外扩（比率口径 0.1 个百分点）：防止「一直 19%」把轴压成一条线 */
const AXIS_MIN_PADDING = 0.001

/**
 * 计算 CPU 使用率趋势的自适应纵轴（#361）。
 *
 * 为什么不用固定 0–100%：盯盘时 CPU 常年在 10–25% 之间波动，固定轴会把真实波动压成一条
 * 直线，等于把「趋势」这个信息删掉。自适应轴保留形状，代价是需要显式刻度——因此本函数同时
 * 给出三档标签，左侧刻度列直接渲染它。
 *
 * 外扩量按**序列跨度**算（10% of span），不按绝对读数算：按读数算会让「19% 附近 ±0.2% 的
 * 波动」被外扩成 ±1.9% 的轴，等于把波动又压回去。等值序列（跨度为 0）退回最小外扩，
 * 此时轴范围只由 `AXIS_MIN_PADDING` 决定，曲线落在中线附近。
 *
 * 下界不低于 0（使用率不可能是负数），上界不超过 1（比率口径的物理上限）。
 *
 * @param points 序列（Unix 秒 + 比率）；非有限值逐点丢弃
 * @returns 有效点少于 1 个时为 null（画不出轴，调用方不渲染趋势）
 */
export function adaptiveAxisTicks(points: readonly { ts: number; value: number }[]): AdaptiveAxis | null {
  let min = Number.POSITIVE_INFINITY
  let max = Number.NEGATIVE_INFINITY
  for (const point of points) {
    if (!Number.isFinite(point.value)) continue
    if (point.value < min) min = point.value
    if (point.value > max) max = point.value
  }
  if (!Number.isFinite(min) || !Number.isFinite(max)) return null

  const span = max - min
  const padding = Math.max(span * AXIS_PADDING_RATIO, AXIS_MIN_PADDING)
  const lo = Math.max(0, min - padding)
  const hi = Math.min(1, max + padding)
  // 上界被 1 夹住、或数据本身贴着 1 时，夹取会让 hi 落到 lo 以下：保底抬到 max（曲线不能画到
  // 轴外），仍不足最小跨度时再抬 AXIS_MIN_PADDING。
  const safeHi = hi - lo >= AXIS_MIN_PADDING
    ? hi
    : Math.min(1, Math.max(max, lo + AXIS_MIN_PADDING))
  const mid = lo + (safeHi - lo) / 2
  return {
    lo,
    hi: safeHi,
    labels: [lo, mid, safeHi].map(value => ({ value, text: `${Math.round(value * 100)}%` })),
  }
}
