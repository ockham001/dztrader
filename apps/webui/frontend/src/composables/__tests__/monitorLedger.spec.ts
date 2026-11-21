import { describe, it, expect } from 'vitest'
import {
  LEDGER_DEFAULT_MEMORY_USED_PCT,
  LEDGER_DEFAULT_SWAP_USED_PCT,
  LEDGER_DEFAULT_VOLUME_FREE_PCT,
  NO_THRESHOLD_TEXT,
  UNRATED_LEVEL_TEXT,
  adaptiveAxisTicks,
  adaptiveTrendGeometry,
  buildMemoryComposition,
  buildMonitorLedger,
  ledgerLevelText,
  unavailableReasonText,
  type LedgerGroup,
  type LedgerRow,
  type MonitorLedgerInput,
} from '../monitorLedger'
import type {
  SysmonAvailabilityEntry,
  SysmonClockInfo,
  SysmonConfigPayload,
  SysmonSnapshotPayload,
  SysmonVolumeInfo,
} from '@/types/generated'
import {
  SYSMON_CLOCK_ALL_FAILED,
  SYSMON_CLOCK_LIVE,
  SYSMON_CLOCK_UNPROBED,
  SYSMON_METRIC_KEYS,
  makeSysmonConfig,
  makeSysmonSnapshot,
  type SysmonSnapshotOptions,
} from '@/test-utils/sysmonSnapshot'

// #361 口径单测：台账的行构成、七列取值、阈值来源与余量口径、四类空态、行序稳定性。
// 直接调纯函数（不 mount、不渲染），与 composables/__tests__/sysmonClock.spec.ts 同形态。
//
// AC1：被测算子模块不 import vue / pinia / store，**本用例也不挂 pinia**——输入直接从夹具
// payload 构造。视图取数走 store 的净化 computed，那层净化有 `stores/__tests__/sysmon.spec.ts`
// 自己的用例；这里若 import store，纯模块的边界就只是纸面约定。

type LedgerFixtureOptions = SysmonSnapshotOptions
type LedgerConfig = {
  thresholds?: Partial<SysmonConfigPayload['thresholds']>
  clock?: Partial<SysmonConfigPayload['clock']>
}

/**
 * 复刻 store 的卷展示序（ADR 0048：挂载深度升序 → 挂载点升序），**仅供夹具构造**。
 *
 * 纯模块单测不得 import store（AC1），因此这里按 `stores/sysmon.ts` 的 `volumeDepth` 同口径
 * 做一个最小等价实现，只为把夹具 payload 排成视图会看到的顺序。
 */
function fixtureVolumeDepth(mount: string): number {
  const normalized = mount.replace(/\\/g, '/').replace(/\/+$/, '')
  if (!normalized) return 0
  const driveMatch = /^([A-Za-z]):(.*)$/.exec(normalized)
  if (driveMatch) {
    const rest = driveMatch[2]
    return rest ? rest.split('/').filter(Boolean).length : 0
  }
  return normalized.split('/').filter(Boolean).length
}

function fixtureSortedVolumes(volumes: readonly SysmonVolumeInfo[]): SysmonVolumeInfo[] {
  return [...volumes].sort((a, b) => {
    const depthDelta = fixtureVolumeDepth(a.mount) - fixtureVolumeDepth(b.mount)
    if (depthDelta !== 0) return depthDelta
    return a.mount.localeCompare(b.mount)
  })
}

/** 从夹具 payload 直接组装台账输入（不 import store / pinia） */
function ledgerInput(
  options: LedgerFixtureOptions = {},
  config: LedgerConfig = {},
  patch: Partial<MonitorLedgerInput> = {},
): MonitorLedgerInput {
  const snapshot: SysmonSnapshotPayload = makeSysmonSnapshot({ resourceData: true, ...options })
  const cfg = makeSysmonConfig(config)
  const volumes = fixtureSortedVolumes(snapshot.volumes ?? [])
  return {
    // 夹具 payload 与 store 净化口径自洽（内存 used = total − available、卷 used_ratio 由
    // used/total 派出），因此直接透传；派生逻辑本身由 store 的单测锁定。
    memory: snapshot.memory,
    swap: snapshot.swap,
    readableVolumes: volumes.filter(volume => volume.used_ratio !== null),
    zeroCapacityVolumes: volumes.filter(volume => volume.used_ratio === null),
    diskIo: snapshot.disk_io ?? [],
    network: snapshot.network ?? [],
    shmDir: snapshot.shm_dir,
    clock: snapshot.clock,
    cpuCores: snapshot.cpu?.cores ?? [],
    cpuContextSwitchesPerSec: snapshot.cpu?.context_switches_per_sec ?? null,
    cpuInterruptsPerSec: snapshot.cpu?.interrupts_per_sec ?? null,
    thresholds: cfg.thresholds,
    clockConfig: cfg.clock,
    availability: metric => snapshot.availability.metrics[metric],
    nowUnixMs: Date.UTC(2026, 9, 21, 15, 0, 0),
    ...patch,
  }
}

/** 构造输入并产出三组台账（与视图的构造入口一致） */
function ledgerFrom(options: LedgerFixtureOptions = {}, config: LedgerConfig = {}) {
  const input = ledgerInput(options, config)
  return { groups: buildMonitorLedger(input), input }
}

/** 取某组的全部行（组缺失时返回空数组，让断言失败而不是抛错） */
function rowsOf(groups: LedgerGroup[], id: LedgerGroup['id']): LedgerRow[] {
  return groups.find(group => group.id === id)?.rows ?? []
}

/** 按 id 找一行 */
function rowById(groups: LedgerGroup[], id: string): LedgerRow {
  const row = groups.flatMap(group => group.rows).find(candidate => candidate.id === id)
  if (!row) throw new Error(`row not found: ${id}`)
  return row
}

/** 全量可用性映射（构造「全部指标可用」的快照） */
function allAvailable(): Record<string, SysmonAvailabilityEntry> {
  return Object.fromEntries(SYSMON_METRIC_KEYS.map(key => [key, { available: true }]))
}

describe('buildMonitorLedger · 分组与行序（固定顺序，不按使用率重排）', () => {
  it('四组恒按 CPU → 内存 → 存储 → 网络与时钟 排列', () => {
    const { groups } = ledgerFrom({ metrics: allAvailable() })
    expect(groups.map(group => group.id)).toEqual(['cpu', 'memory', 'storage', 'network_clock'])
    expect(groups.map(group => group.title)).toEqual(['CPU', '内存', '存储', '网络与时钟'])
  })

  it('内存组恒两行：内存已用 → 交换已用', () => {
    const { groups } = ledgerFrom({ metrics: allAvailable() })
    expect(rowsOf(groups, 'memory').map(row => row.id)).toEqual(['memory', 'swap'])
  })

  it('存储组：卷（保持 store 的 ADR 0048 顺序）→ 盘（device 升序）→ 共享内存目录', () => {
    const { groups } = ledgerFrom({ metrics: allAvailable() })
    const ids = rowsOf(groups, 'storage').map(row => row.id)
    expect(ids).toEqual([
      'volume:/', 'volume:/data', 'volume:/home',
      'disk:nvme0n1', 'disk:sda',
      'shm_dir',
    ])
  })

  it('传入乱序的盘数组时仍按 device 升序排（行序稳定不依赖调用方）', () => {
    const base = ledgerInput({ metrics: allAvailable() })
    const groups = buildMonitorLedger({ ...base, diskIo: [...base.diskIo].reverse() })
    expect(rowsOf(groups, 'storage').map(row => row.id)).toContain('disk:nvme0n1')
    const diskIds = rowsOf(groups, 'storage').map(row => row.id).filter(id => id.startsWith('disk:'))
    expect(diskIds).toEqual(['disk:nvme0n1', 'disk:sda'])
  })

  it('网络与时钟组：每张网卡两行（接收 → 发送），时钟在组末', () => {
    const { groups } = ledgerFrom({
      metrics: allAvailable(),
      network: [
        {
          interface: 'eno1', rx_bytes_total: 1, tx_bytes_total: 2, rx_packets_total: 3, tx_packets_total: 4,
          rx_errors_total: 0, tx_errors_total: 0, rx_drops_total: 0, tx_drops_total: 0,
          rx_bytes_per_sec: 1024, tx_bytes_per_sec: 2048,
          rx_errors_per_sec: 0, tx_errors_per_sec: 0, rx_drops_per_sec: 0, tx_drops_per_sec: 0,
          is_virtual: false,
        },
      ],
    })
    expect(rowsOf(groups, 'network_clock').map(row => row.id))
      .toEqual(['nic:eno1:rx', 'nic:eno1:tx', 'clock'])
  })

  it('行数随数据变化：8 个卷 → 8 行（不是固定 13 行）', () => {
    const volumes = Array.from({ length: 8 }, (_, index) => ({
      mount: `/vol${index}`,
      available: true,
      total_bytes: 100 * 1024 ** 3,
      used_bytes: 50 * 1024 ** 3,
      used_ratio: 0.5,
    }))
    const { groups } = ledgerFrom({ metrics: allAvailable(), volumes })
    const volumeRows = rowsOf(groups, 'storage').filter(row => row.id.startsWith('volume:'))
    expect(volumeRows).toHaveLength(8)
  })

  it('0 张网卡 → 网络组落到一行空态而不是 0 行', () => {
    const { groups } = ledgerFrom({ metrics: allAvailable(), network: [] })
    const ids = rowsOf(groups, 'network_clock').map(row => row.id)
    expect(ids).toEqual(['network', 'clock'])
    expect(rowById(groups, 'network').state).toBe('empty')
  })
})

describe('buildMonitorLedger · CPU 组（#370：上下文切换 / 中断移出 CPU 宽卡）', () => {
  it('CPU 组恒在最前，含 cpu.cores → cpu.rates 两行（行序稳定）', () => {
    const { groups } = ledgerFrom({ metrics: allAvailable() })
    expect(groups[0].id).toBe('cpu')
    expect(rowsOf(groups, 'cpu').map(row => row.id)).toEqual(['cpu.cores', 'cpu.rates'])
  })

  it('cpu.cores 行值给最高核占用率与核数摘要，明细给饱和核数，阈值/余量为 —，无量程', () => {
    const { groups } = ledgerFrom({ metrics: allAvailable() })
    const row = rowById(groups, 'cpu.cores')
    expect(row.state).toBe('ok')
    expect(row.value).toContain('16 核')
    expect(row.value).toMatch(/最高 \d+(\.\d+)?% · 16 核/)
    expect(row.detail).toBe('0 核饱和 / 16 核')
    expect(row.thresholdText).toBe(NO_THRESHOLD_TEXT)
    expect(row.headroomText).toBe(NO_THRESHOLD_TEXT)
    expect(row.ratio).toBeNull()
    expect(row.level).toBeNull()
    expect(row.levelText).toBe(UNRATED_LEVEL_TEXT)
    expect(row.historyKey).toBeNull()
  })

  it('cpu.cores 的饱和核计入明细，行值仍是最高占用率（饱和不改变主列口径）', () => {
    const { groups } = ledgerFrom({ metrics: allAvailable(), saturatedCores: [2, 5] })
    const row = rowById(groups, 'cpu.cores')
    expect(row.detail).toBe('2 核饱和 / 16 核')
    expect(row.state).toBe('ok')
  })

  it('cpu.cores 能力不可用时是 unavailable 空态；可用但无读数时是 empty 空态', () => {
    const unavailable = ledgerFrom({
      metrics: { 'cpu.cores': { available: false, reason: 'no_permission' } },
    })
    expect(rowById(unavailable.groups, 'cpu.cores').state).toBe('unavailable')

    const empty = ledgerFrom({ metrics: allAvailable(), cpuCores: [] })
    const row = rowById(empty.groups, 'cpu.cores')
    expect(row.state).toBe('empty')
    expect(row.value).toBe('')
  })

  it('cpu.rates 行值是「上下文切换 · 中断」两段速率，明细列写明这个顺序', () => {
    const { groups } = ledgerFrom({ metrics: allAvailable() })
    const row = rowById(groups, 'cpu.rates')
    expect(row.state).toBe('ok')
    expect(row.value).toBe('1234/s · 567/s')
    expect(row.detail).toBe('上下文切换 · 中断')
  })

  it('cpu.rates 没有契约阈值：阈值与余量写 —，等级无阈值，ratio 为 null，无历史入口', () => {
    const { groups } = ledgerFrom({ metrics: allAvailable() })
    const row = rowById(groups, 'cpu.rates')
    expect(row.thresholdText).toBe(NO_THRESHOLD_TEXT)
    expect(row.headroomText).toBe(NO_THRESHOLD_TEXT)
    expect(row.level).toBeNull()
    expect(row.ratio).toBeNull()
    expect(row.levelText).toBe(UNRATED_LEVEL_TEXT)
    expect(row.historyKey).toBeNull()
  })

  it('能力不可用时是 unavailable 空态，不产出读数', () => {
    const { groups } = ledgerFrom({
      metrics: { 'cpu.context_switches': { available: false, reason: 'no_permission' } },
    })
    const row = rowById(groups, 'cpu.rates')
    expect(row.state).toBe('unavailable')
    expect(row.value).toBe('')
    expect(row.ratio).toBeNull()
    expect(row.stateText).toContain('权限不足，无法读取该指标')
  })

  it('能力可用但本帧两值皆缺时是 empty 空态（不产出数字，也不冒充 0）', () => {
    const { groups } = ledgerFrom({
      metrics: allAvailable(),
      contextSwitchesPerSec: null,
      interruptsPerSec: null,
    })
    const row = rowById(groups, 'cpu.rates')
    expect(row.state).toBe('empty')
    expect(row.value).toBe('')
  })

  it('只有一项缺值时该段显示「不可用」而不是 0（另一段照常给速率）', () => {
    const { groups } = ledgerFrom({ metrics: allAvailable(), interruptsPerSec: null })
    const row = rowById(groups, 'cpu.rates')
    expect(row.state).toBe('ok')
    expect(row.value).toBe('1234/s · 不可用')
  })
})

describe('buildMonitorLedger · 阈值列只写契约里真实存在的阈值', () => {
  it('内存 90% / 交换 50% / 卷 10% 空闲，取自生效配置', () => {
    const { groups } = ledgerFrom({ metrics: allAvailable() })
    expect(rowById(groups, 'memory').thresholdText).toBe('90%')
    expect(rowById(groups, 'swap').thresholdText).toBe('50%')
    expect(rowById(groups, 'volume:/').thresholdText).toBe('10% 空闲')
  })

  it('配置镜像改了阈值后阈值列跟着变（前端不另定一套）', () => {
    const { groups } = ledgerFrom(
      { metrics: allAvailable() },
      { thresholds: { memory_used_pct: 80, swap_used_pct: 40, volume_free_pct: 20 } },
    )
    expect(rowById(groups, 'memory').thresholdText).toBe('80%')
    expect(rowById(groups, 'swap').thresholdText).toBe('40%')
    expect(rowById(groups, 'volume:/').thresholdText).toBe('20% 空闲')
  })

  it('磁盘忙碌占比 / 网卡 / 共享内存目录没有契约阈值，写 —（不编造）', () => {
    const { groups } = ledgerFrom({ metrics: allAvailable() })
    expect(rowById(groups, 'disk:sda').thresholdText).toBe(NO_THRESHOLD_TEXT)
    expect(rowById(groups, 'disk:sda').headroomText).toBe(NO_THRESHOLD_TEXT)
    expect(rowById(groups, 'shm_dir').thresholdText).toBe(NO_THRESHOLD_TEXT)
    expect(rowById(groups, 'shm_dir').headroomText).toBe(NO_THRESHOLD_TEXT)
    const netRow = rowsOf(groups, 'network_clock').find(row => row.contractKey === 'network' && row.state === 'ok')
    // 先断言存在再取值：守卫式 if 会让整组断言在夹具漂移时被静默跳过（finding 6）
    expect(netRow).toBeDefined()
    expect(netRow!.thresholdText).toBe(NO_THRESHOLD_TEXT)
    expect(netRow!.headroomText).toBe(NO_THRESHOLD_TEXT)
  })

  it('时钟阈值取生效配置的两档（不是前端常量），并同时给 warn 与 error', () => {
    const { groups } = ledgerFrom(
      { metrics: allAvailable(), clock: SYSMON_CLOCK_LIVE },
      { clock: { offset_warn_ms: 50, offset_error_ms: 500 } },
    )
    expect(rowById(groups, 'clock').thresholdText).toBe('±50 / ±500 ms')
  })

  it('配置镜像未到达时回退契约默认值 90 / 50 / 10 / 100 / 1000（不跳变）', () => {
    const groups = buildMonitorLedger({
      ...ledgerInput({ metrics: allAvailable(), clock: SYSMON_CLOCK_LIVE }),
      thresholds: null,
      clockConfig: null,
    })
    expect(rowById(groups, 'memory').thresholdText).toBe(`${LEDGER_DEFAULT_MEMORY_USED_PCT}%`)
    expect(rowById(groups, 'swap').thresholdText).toBe(`${LEDGER_DEFAULT_SWAP_USED_PCT}%`)
    expect(rowById(groups, 'volume:/').thresholdText).toBe(`${LEDGER_DEFAULT_VOLUME_FREE_PCT}% 空闲`)
    expect(rowById(groups, 'clock').thresholdText).toBe('±100 / ±1000 ms')
  })
})

describe('buildMonitorLedger · 余量按契约阈值算（不是 70/90 展示分档）', () => {
  it('内存余量 = 总量 × 阈值% − 已用（字节）', () => {
    const { groups } = ledgerFrom({ metrics: allAvailable() })
    // 默认夹具：32 GiB 总量 / 16 GiB 已用 / 阈值 90% → 28.8 − 16 = 12.8 GiB
    expect(rowById(groups, 'memory').headroomText).toBe('12.8 GiB')
  })

  it('交换余量 = 总量 × 阈值% − 已用（字节）', () => {
    const { groups } = ledgerFrom({ metrics: allAvailable() })
    // 默认夹具：8 GiB 总量 / 2 GiB 已用 / 阈值 50% → 4 − 2 = 2.0 GiB
    expect(rowById(groups, 'swap').headroomText).toBe('2.0 GiB')
  })

  it('卷余量 = 空闲百分点 − 10（百分点），不是已用百分比相减', () => {
    const { groups } = ledgerFrom({ metrics: allAvailable() })
    // `/` 40% 已用 → 60% 空闲 → 60 − 10 = 50.0 点
    expect(rowById(groups, 'volume:/').headroomText).toBe('50.0 点')
    // `/home` 62% 已用 → 38% 空闲 → 38 − 10 = 28.0 点
    expect(rowById(groups, 'volume:/home').headroomText).toBe('28.0 点')
  })

  it('余量恰好为 0 时显示 0 而不是越线文案（边界含）', () => {
    // 卷已用 90% → 空闲 10% → 余量恰好 0
    const { groups } = ledgerFrom({
      metrics: allAvailable(),
      volumes: [{
        mount: '/edge', available: true,
        total_bytes: 100 * 1024 ** 3, used_bytes: 90 * 1024 ** 3, used_ratio: 0.9,
      }],
    })
    expect(rowById(groups, 'volume:/edge').headroomText).toBe('0.0 点')
  })

  it('已越线时显示「已超 X」而不是「不可用」', () => {
    const { groups } = ledgerFrom({
      metrics: allAvailable(),
      volumes: [{
        mount: '/over', available: true,
        total_bytes: 100 * 1024 ** 3, used_bytes: 98 * 1024 ** 3, used_ratio: 0.98,
      }],
    })
    // 98% 已用 → 2% 空闲 → 2 − 10 = −8 点 → 已超 8.0 点
    expect(rowById(groups, 'volume:/over').headroomText).toBe('已超 8.0 点')
  })

  it('时钟余量按 error 档算（warn 只是提前示警）', () => {
    const { groups } = ledgerFrom({ metrics: allAvailable(), clock: SYSMON_CLOCK_LIVE })
    // SYSMON_CLOCK_LIVE: offset 12.5 ms，error 1000 → 1000 − 12.5 = 987.5
    expect(rowById(groups, 'clock').headroomText).toBe('987.5 ms')
  })

  it('字节余量按展示精度判符号：阈值上恰好卡线不得显示「已超 0 B」（finding 2 复现）', () => {
    // 8 GiB × 90% = 7730941132.8，用 7730941133 让差值略负（−0.19999980926513672）。
    // 按原始浮点判符号会把「刚好到阈值」报成越线，一个精度噪声伪装成结论。
    const totalBytes = 8 * 1024 ** 3
    const { groups } = ledgerFrom({
      metrics: allAvailable(),
      memory: {
        total_bytes: totalBytes,
        available_bytes: totalBytes - 7730941133,
        used_bytes: 7730941133,
        used_ratio: 7730941133 / totalBytes,
        cached_bytes: null,
        buffers_bytes: null,
      },
    })
    expect(rowById(groups, 'memory').headroomText).toBe('0 B')
    expect(rowById(groups, 'memory').headroomText).not.toBe('已超 0 B')
  })
})

// #382：阈值/余量两列是定宽（两列都 88px、全档同宽），而契约
// 允许 `offset_warn_ms ∈ [1, 60000]`、`offset_error_ms ∈ (warn, 600000]`。台账必须在**契约全空间**
// 内都不撑破列。渲染宽度由人眼 + 实测负责（本仓单测不挂浏览器），本组锁的是「文本形态收敛」：
// ① 默认配置逐字符不变；② 每个合法档位产出的是短形态而不是 6 位毫秒；③ 两档永不塌成同一个数。
describe('buildMonitorLedger · 阈值/余量列在契约全空间内收敛（#382）', () => {
  const clockAt = (offsetWarnMs: number, offsetErrorMs: number, offsetMs = 12.5) =>
    rowById(
      ledgerFrom(
        { metrics: allAvailable(), clock: { ...SYSMON_CLOCK_LIVE, offset_ms: offsetMs } },
        { clock: { offset_warn_ms: offsetWarnMs, offset_error_ms: offsetErrorMs } },
      ).groups,
      'clock',
    )

  it('默认配置 100 / 1000 的文案逐字符不变（改它等于改所有人的肌肉记忆）', () => {
    expect(clockAt(100, 1000).thresholdText).toBe('±100 / ±1000 ms')
  })

  it('契约上限 60000 / 600000 折成分钟，不再是 110px 的 6 位毫秒', () => {
    expect(clockAt(60_000, 600_000).thresholdText).toBe('±1 / ±10 min')
    // 负向对照：旧实现（恒写 ms）的形态必须已被消灭
    expect(clockAt(60_000, 600_000).thresholdText).not.toContain('ms')
  })

  it('中间档 1000 / 10000 折成秒（旧文本 ±1000 / ±10000 ms 实测 97.4px，超 88px 列 9.4px）', () => {
    expect(clockAt(1000, 10_000).thresholdText).toBe('±1 / ±10 s')
  })

  it('非整秒配置保留 3 位有效数字，且不塌成一档', () => {
    expect(clockAt(1500, 6000).thresholdText).toBe('±1.5 / ±6 s')
    expect(clockAt(59_999, 60_000).thresholdText).toBe('±59.9 / ±60 s')
    expect(clockAt(59_999, 60_000).thresholdText).not.toBe('±60 / ±60 s')
  })

  it('契约内任一合法组合的阈值文本都不含 3 位以上整数毫秒（列宽 88px 的硬前提）', () => {
    // 抽样覆盖三个分支的边界与跨档点（全空间 3.6e10 组合不可能穷举，形态由 format 的单测锁死）
    const samples: Array<[number, number]> = [
      [1, 2], [1, 1000], [999, 1000], [1000, 1001], [1000, 2000],
      [1001, 60_000], [9999, 10_000], [10_000, 59_999], [30_000, 60_000],
      [59_999, 60_000], [60_000, 60_001], [60_000, 120_000], [60_000, 600_000],
    ]
    for (const [warn, error] of samples) {
      const text = clockAt(warn, error).thresholdText
      expect(text, `${warn}/${error} → ${text}`).toMatch(/^±\d+(\.\d+)? \/ ±\d+(\.\d+)? (ms|s|min)$/)
      // 单侧数值最多 4 个字符（`600` / `59.9` / `0.001` / `1000`），6 位毫秒形态已不可能
      for (const side of text.replace(/^±/, '').replace(/ (ms|s|min)$/, '').split(' / ±')) {
        expect(side.length, `${warn}/${error} → ${text}`).toBeLessThanOrEqual(5)
      }
    }
  })

  it('时钟余量：默认档文案不变，契约上限换秒（600000 ms 加前缀实测 96.6px）', () => {
    // SYSMON_CLOCK_LIVE offset 12.5 → error 1000 − 12.5 = 987.5
    expect(clockAt(100, 1000).headroomText).toBe('987.5 ms')
    // error 600000、offset 12.5 → 599987.5 ms → 600 s（旧文本「599987.5 ms」+「已超 」放不下）
    expect(clockAt(60_000, 600_000).headroomText).toBe('600 s')
    // 越线（offset 超过 error）时负号留给调用方口径：时钟行直接给带符号值
    expect(clockAt(100, 1000, 1200).headroomText).toBe('-200.0 ms')
  })

  it('字节余量：真实大内存（≥123 GiB 余量）的文本是「已超 X」形态且不含 6 位以上数字', () => {
    const totalBytes = 512 * 1024 ** 3
    const { groups } = ledgerFrom({
      metrics: allAvailable(),
      memory: {
        total_bytes: totalBytes,
        // 已用远超 90% 阈值：余量 = 512 × 90% − 已用 = 460.8 − 512 × 0.99 → 负值走「已超」
        available_bytes: totalBytes * 0.01,
        used_bytes: totalBytes * 0.99,
        used_ratio: 0.99,
        cached_bytes: null,
        buffers_bytes: null,
      },
    })
    const text = rowById(groups, 'memory').headroomText
    // 512 × 0.9 = 460.8 GiB 阈值；已用 506.88 GiB → 已超 46.08 GiB → 1 位小数 46.1 GiB
    expect(text).toBe('已超 46.1 GiB')
    expect(text.length).toBeLessThanOrEqual(13)
  })
})

describe('buildMonitorLedger · 定档边界与「无法定档」', () => {
  it('ratio 恰好 0.7 → warn、恰好 0.9 → danger（边界含）', () => {
    const { groups } = ledgerFrom({
      metrics: allAvailable(),
      volumes: [
        { mount: '/w', available: true, total_bytes: 1000, used_bytes: 700, used_ratio: 0.7 },
        { mount: '/d', available: true, total_bytes: 1000, used_bytes: 900, used_ratio: 0.9 },
      ],
    })
    expect(rowById(groups, 'volume:/w').level).toBe('warn')
    expect(rowById(groups, 'volume:/d').level).toBe('danger')
    expect(rowById(groups, 'volume:/w').levelText).toBe('警戒')
    expect(rowById(groups, 'volume:/d').levelText).toBe('危险')
  })

  it('ratio 为 null → level 为 null，levelText 写「无阈值」（不得冒充 normal，也不留空）', () => {
    const { groups } = ledgerFrom({ metrics: allAvailable() })
    const shm = rowById(groups, 'shm_dir')
    expect(shm.ratio).toBeNull()
    expect(shm.level).toBeNull()
    // 不留空串：没有文字的状态点在灰度截屏下等于没有状态（#363 AC3）
    expect(shm.levelText).toBe(UNRATED_LEVEL_TEXT)
    expect(shm.levelText).not.toBe('正常')
  })

  it('网卡行没有链路速率分母，ratio 恒为 null（不编造分母）', () => {
    const { groups } = ledgerFrom({
      metrics: allAvailable(),
      network: [{
        interface: 'eno1', rx_bytes_total: 1, tx_bytes_total: 2, rx_packets_total: 3, tx_packets_total: 4,
        rx_errors_total: 0, tx_errors_total: 0, rx_drops_total: 0, tx_drops_total: 0,
        rx_bytes_per_sec: 1024, tx_bytes_per_sec: 2048,
        rx_errors_per_sec: 0, tx_errors_per_sec: 0, rx_drops_per_sec: 0, tx_drops_per_sec: 0,
        is_virtual: false,
      }],
    })
    expect(rowById(groups, 'nic:eno1:rx').ratio).toBeNull()
    expect(rowById(groups, 'nic:eno1:rx').level).toBeNull()
  })

  it('时钟按配置阈值定档：60 ms 在 warn=50 下是 warn', () => {
    const clock: SysmonClockInfo = { ...SYSMON_CLOCK_LIVE, offset_ms: 60 }
    const { groups } = ledgerFrom(
      { metrics: allAvailable(), clock },
      { clock: { offset_warn_ms: 50, offset_error_ms: 500 } },
    )
    expect(rowById(groups, 'clock').level).toBe('warn')
    expect(rowById(groups, 'clock').levelText).toBe('警戒')
  })

  it('时钟 ratio 恒为 null（阈值是绝对毫秒，没有可归一化量程）', () => {
    const { groups } = ledgerFrom({ metrics: allAvailable(), clock: SYSMON_CLOCK_LIVE })
    expect(rowById(groups, 'clock').ratio).toBeNull()
  })
})

describe('buildMonitorLedger · 四类空态互不混淆且都不产出数字', () => {
  it('不可用（unsupported）：state=unavailable，带原因文案，value 为空串', () => {
    const { groups } = ledgerFrom({
      metrics: { ...allAvailable(), memory: { available: false, reason: 'unsupported' } },
    })
    const row = rowById(groups, 'memory')
    expect(row.state).toBe('unavailable')
    expect(row.stateText).toContain('不可用')
    expect(row.stateText).toContain('unsupported')
    expect(row.value).toBe('')
    expect(row.ratio).toBeNull()
  })

  it('配置关闭：state=disabled（可自助恢复的一类单独成档）', () => {
    const { groups } = ledgerFrom({
      metrics: { ...allAvailable(), memory: { available: false, reason: 'disabled_by_config' } },
    })
    const row = rowById(groups, 'memory')
    expect(row.state).toBe('disabled')
    expect(row.stateText).toContain('已在配置中关闭')
  })

  it('能力可用但本帧无数据：state=empty（不是 unavailable）', () => {
    const { groups } = ledgerFrom({ metrics: allAvailable(), memory: null })
    const row = rowById(groups, 'memory')
    expect(row.state).toBe('empty')
    expect(row.stateText).toContain('暂无数据')
    expect(row.value).toBe('')
  })

  it('无交换分区（total_bytes = 0）：state=none，显示明确事实而不是 0%', () => {
    const { groups } = ledgerFrom({
      metrics: allAvailable(),
      swap: { total_bytes: 0, used_bytes: 0, used_ratio: null },
    })
    const row = rowById(groups, 'swap')
    expect(row.state).toBe('none')
    expect(row.stateText).toBe('无交换分区')
    expect(row.value).toBe('')
    expect(row.ratio).toBeNull()
  })

  it('swap 能力 unsupported（平台无交换）：state=none 且文案是平台事实', () => {
    const { groups } = ledgerFrom({
      metrics: { ...allAvailable(), swap: { available: false, reason: 'unsupported' } },
    })
    const row = rowById(groups, 'swap')
    expect(row.state).toBe('none')
    expect(row.stateText).toContain('无交换分区')
  })

  it('任何空态的 value 都不是「0」或「0.0%」', () => {
    const { groups } = ledgerFrom({
      metrics: {
        ...allAvailable(),
        memory: { available: false, reason: 'unsupported' },
        swap: { available: false, reason: 'disabled_by_config' },
        disk_io: { available: false, reason: 'no_permission' },
      },
    })
    for (const row of groups.flatMap(group => group.rows)) {
      if (row.state === 'ok') continue
      expect(row.value).toBe('')
      expect(row.ratio).toBeNull()
      expect(row.value).not.toBe('0')
      expect(row.value).not.toBe('0.0%')
    }
  })

  it('未知 reason 原样显示（不抹成「不可用」）', () => {
    expect(unavailableReasonText('some_new_reason')).toBe('some_new_reason')
    expect(unavailableReasonText(undefined)).toBe('当前快照未提供该指标')
  })

  it('不可读容量的卷不进主列（与 ADR 0048 的可读性分池一致）', () => {
    const { groups } = ledgerFrom({
      metrics: allAvailable(),
      volumes: [
        { mount: '/', available: true, total_bytes: 100, used_bytes: 40, used_ratio: 0.4 },
        { mount: '/bad', available: false, total_bytes: null, used_bytes: null, used_ratio: null, reason: 'no_permission' },
      ],
    })
    const volumeIds = rowsOf(groups, 'storage').map(row => row.id).filter(id => id.startsWith('volume:'))
    expect(volumeIds).toEqual(['volume:/'])
  })
})

describe('buildMonitorLedger · historyKey 只在标量行上有值', () => {
  it('内存 / 交换 / 共享内存目录有 historyKey，卷 / 盘 / 网卡 / 时钟为 null', () => {
    const { groups } = ledgerFrom({ metrics: allAvailable(), clock: SYSMON_CLOCK_LIVE })
    expect(rowById(groups, 'memory').historyKey).toBe('memory.used_ratio')
    expect(rowById(groups, 'swap').historyKey).toBe('swap.used_ratio')
    expect(rowById(groups, 'volume:/').historyKey).toBeNull()
    expect(rowById(groups, 'disk:sda').historyKey).toBeNull()
    expect(rowById(groups, 'clock').historyKey).toBeNull()
    // #363：共享内存目录的数值保留跳历史入口（#127 / #325 的既有标量），它没有阈值但与
    // 「能不能看历史」无关——旧卡片里这个数字就是可点的，台账不得把它降级成纯文本。
    expect(rowById(groups, 'shm_dir').historyKey).toBe('shm_dir.total_bytes')
  })
})

describe('buildMonitorLedger · 时钟三空态', () => {
  it('尚未探测：state=none，文案「尚未探测」（不是不可用）', () => {
    const { groups } = ledgerFrom({ metrics: allAvailable(), clock: SYSMON_CLOCK_UNPROBED })
    const row = rowById(groups, 'clock')
    expect(row.state).toBe('none')
    expect(row.stateText).toBe('尚未探测')
    expect(row.value).toBe('')
  })

  it('全部失败：state=unavailable，不显示 0 ms', () => {
    const { groups } = ledgerFrom({ metrics: allAvailable(), clock: SYSMON_CLOCK_ALL_FAILED })
    const row = rowById(groups, 'clock')
    expect(row.state).toBe('unavailable')
    expect(row.value).toBe('')
    expect(row.value).not.toContain('0.0 ms')
  })

  it('配置关闭（clock = null）：state=disabled', () => {
    const { groups } = ledgerFrom({ metrics: allAvailable(), clock: null })
    const row = rowById(groups, 'clock')
    expect(row.state).toBe('disabled')
    expect(row.stateText).toContain('已在配置中关闭')
  })
})

describe('buildMonitorLedger · 明细与展开字段', () => {
  it('内存行的明细是「已用 / 总量」，展开字段含缓存与缓冲', () => {
    const { groups } = ledgerFrom({ metrics: allAvailable() })
    const row = rowById(groups, 'memory')
    expect(row.detail).toBe('16.0 GiB / 32.0 GiB')
    expect(row.fields.map(field => field.label)).toEqual([
      '总量', '已用（总量 − 可用）', '可用', '缓存（可回收）', '缓冲（内核元数据）',
    ])
  })

  it('盘行展开给七项指标', () => {
    const { groups } = ledgerFrom({ metrics: allAvailable() })
    expect(rowById(groups, 'disk:sda').fields).toHaveLength(7)
  })

  it('网卡行展开给八项（含累计错误与丢弃）', () => {
    const { groups } = ledgerFrom({
      metrics: allAvailable(),
      network: [{
        interface: 'eno1', rx_bytes_total: 1, tx_bytes_total: 2, rx_packets_total: 3, tx_packets_total: 4,
        rx_errors_total: 5, tx_errors_total: 6, rx_drops_total: 7, tx_drops_total: 8,
        rx_bytes_per_sec: 1024, tx_bytes_per_sec: 2048,
        rx_errors_per_sec: 0, tx_errors_per_sec: 0, rx_drops_per_sec: 0, tx_drops_per_sec: 0,
        is_virtual: false,
      }],
    })
    expect(rowById(groups, 'nic:eno1:rx').fields).toHaveLength(8)
  })

  it('组注给出计数（卷数 / 盘数 / 网卡数）', () => {
    const { groups } = ledgerFrom({ metrics: allAvailable() })
    expect(groups.find(group => group.id === 'storage')?.note).toContain('3 个挂载点')
    expect(groups.find(group => group.id === 'storage')?.note).toContain('2 块磁盘')
  })
})

describe('ledgerLevelText', () => {
  it('等级映射到中文；null（没有判定基准）写「无阈值」而不是空串', () => {
    expect(ledgerLevelText('normal')).toBe('正常')
    expect(ledgerLevelText('warn')).toBe('警戒')
    expect(ledgerLevelText('danger')).toBe('危险')
    // 空串会让状态点旁只剩一个灰点：灰度截屏 / 色觉障碍下这一行的状态不可读（AC3）
    expect(ledgerLevelText(null)).toBe(UNRATED_LEVEL_TEXT)
    expect(UNRATED_LEVEL_TEXT).not.toBe('')
  })

  it('没有量程基准的行（网卡 / 共享内存目录）等级文字是「无阈值」，不是四类空态词', () => {
    const groups = buildMonitorLedger(ledgerInput({ metrics: allAvailable() }))
    for (const rowId of ['nic:eth0:rx', 'shm_dir']) {
      const row = rowById(groups, rowId)
      expect(row.state, rowId).toBe('ok')
      expect(row.level, rowId).toBeNull()
      expect(row.levelText, rowId).toBe(UNRATED_LEVEL_TEXT)
      // 它 != 空态标签：读数在，只是没有判定基准
      expect(row.stateLabel, rowId).toBe('')
    }
  })
})

describe('adaptiveAxisTicks · CPU 趋势的自适应纵轴', () => {
  it('普通序列按 min/max 外扩 10% 并给三档标签', () => {
    const axis = adaptiveAxisTicks([
      { ts: 0, value: 0.19 }, { ts: 1, value: 0.21 }, { ts: 2, value: 0.20 },
    ])
    expect(axis).not.toBeNull()
    expect(axis!.lo).toBeCloseTo(0.188, 4)
    expect(axis!.hi).toBeCloseTo(0.212, 4)
    expect(axis!.labels).toHaveLength(3)
    expect(axis!.labels[0].text).toBe('19%')
    expect(axis!.labels[2].text).toBe('21%')
  })

  it('等值序列不塌成一条线（退到最小外扩）', () => {
    const axis = adaptiveAxisTicks([
      { ts: 0, value: 0.19 }, { ts: 1, value: 0.19 }, { ts: 2, value: 0.19 },
    ])
    expect(axis).not.toBeNull()
    expect(axis!.hi - axis!.lo).toBeGreaterThan(0)
  })

  /**
   * 三档刻度必须互相可区分：轴越窄，整数标签越容易撞成同一个数。只断言 `hi - lo > 0`
   * 放过了「轴有宽度但三档都写 19%」——那时读者面对的是三条无信息量的刻度。
   */
  it('等值 19% 序列的三档标签互不相同（窄轴改用 1 位小数）', () => {
    const axis = adaptiveAxisTicks([
      { ts: 0, value: 0.19 }, { ts: 1, value: 0.19 }, { ts: 2, value: 0.19 },
    ])
    const texts = axis!.labels.map(label => label.text)
    expect(texts).toHaveLength(3)
    expect(new Set(texts).size).toBe(3)
    // 轴宽 0.2 个百分点（最小外扩上下各 0.1），整数标签必然撞号：这里是 18.9 / 19.0 / 19.1
    expect(texts).toEqual(['18.9%', '19.0%', '19.1%'])
  })

  it('19.0%→19.2% 的窄轴三档标签也互不相同', () => {
    const axis = adaptiveAxisTicks([
      { ts: 0, value: 0.19 }, { ts: 1, value: 0.191 }, { ts: 2, value: 0.192 },
    ])
    const texts = axis!.labels.map(label => label.text)
    // 轴 18.9–19.3（外扩 10% of 0.2 个百分点）：整数化会把三档全写成 19%
    expect(new Set(texts).size).toBe(3)
    expect(texts).toEqual(['18.9%', '19.1%', '19.3%'])
  })

  it('12.5% 恒定序列不被整成重复值（窄轴判据不依赖具体数值）', () => {
    const axis = adaptiveAxisTicks([{ ts: 0, value: 0.125 }, { ts: 1, value: 0.125 }])
    const texts = axis!.labels.map(label => label.text)
    expect(new Set(texts).size).toBe(3)
  })

  it('宽轴仍用整数百分比（精度只随跨度放宽，不全局改）', () => {
    const axis = adaptiveAxisTicks([{ ts: 0, value: 0.19 }, { ts: 1, value: 0.21 }])
    expect(axis!.labels.map(label => label.text)).toEqual(['19%', '20%', '21%'])
  })

  it('下界不被压到负数、上界不超过 1', () => {
    const axis = adaptiveAxisTicks([{ ts: 0, value: 0 }, { ts: 1, value: 1 }])
    expect(axis!.lo).toBeGreaterThanOrEqual(0)
    expect(axis!.hi).toBeLessThanOrEqual(1)
  })

  it('空序列 / 全非法值返回 null（不抛错）', () => {
    expect(adaptiveAxisTicks([])).toBeNull()
    expect(adaptiveAxisTicks([{ ts: 0, value: Number.NaN }])).toBeNull()
  })

  it('单点序列仍给可渲染的轴', () => {
    const axis = adaptiveAxisTicks([{ ts: 0, value: 0.5 }])
    expect(axis).not.toBeNull()
    expect(axis!.hi).toBeGreaterThan(axis!.lo)
  })
  it('y 按传入的轴范围投影（不是按序列自身 min/max）：轴内的平段落在中线附近', () => {
    // 轴 18%–22%（外扩自 19/21），序列两端贴轴边界：20% 落在 0–34 的绘图区中线
    const axis = adaptiveAxisTicks([{ ts: 0, value: 0.19 }, { ts: 2, value: 0.21 }])
    const geometry = adaptiveTrendGeometry(
      [{ ts: 0, value: axis!.lo }, { ts: 1, value: (axis!.lo + axis!.hi) / 2 }, { ts: 2, value: axis!.hi }],
      axis,
    )
    expect(geometry).not.toBeNull()
    const ys = geometry!.points.split(' ').map(pair => Number(pair.split(',')[1]))
    // SVG 的 y 向下增大：下界 → 底部留白（32），中点 → 中线（17），上界 → 顶部留白（2）
    expect(ys[0]).toBeCloseTo(32, 1)
    expect(ys[1]).toBeCloseTo(17, 1)
    expect(ys[2]).toBeCloseTo(2, 1)
    expect(geometry!.lastValue).toBe(axis!.hi)
  })

  it('轴外的值被钳制在绘图区内（不画出容器）', () => {
    const axis = { lo: 0.2, hi: 0.3, labels: [] }
    const geometry = adaptiveTrendGeometry(
      [{ ts: 0, value: 0.05 }, { ts: 1, value: 0.9 }],
      axis,
    )
    const ys = geometry!.points.split(' ').map(pair => Number(pair.split(',')[1]))
    expect(Math.min(...ys)).toBeGreaterThanOrEqual(2)
    expect(Math.max(...ys)).toBeLessThanOrEqual(32)
  })

  it('x 按时间比例（桶距不均如实反映），时间跨度为 0 时退回序号等距', () => {
    const axis = { lo: 0, hi: 1, labels: [] }
    const spaced = adaptiveTrendGeometry([{ ts: 0, value: 0.5 }, { ts: 90, value: 0.5 }], axis)
    expect(spaced!.points.split(' ')[1].split(',')[0]).toBe('100')

    const sameTs = adaptiveTrendGeometry([{ ts: 5, value: 0.1 }, { ts: 5, value: 0.2 }, { ts: 5, value: 0.3 }], axis)
    expect(sameTs!.points.split(' ').map(pair => pair.split(',')[0])).toEqual(['0', '50', '100'])
  })

  it('有效点少于 2 个或轴为 null 时返回 null（调用方据此不渲染）', () => {
    const axis = { lo: 0, hi: 1, labels: [] }
    expect(adaptiveTrendGeometry([], axis)).toBeNull()
    expect(adaptiveTrendGeometry([{ ts: 0, value: 0.5 }], axis)).toBeNull()
    expect(adaptiveTrendGeometry([{ ts: 0, value: 0.1 }, { ts: 1, value: 0.2 }], null)).toBeNull()
  })
})

// ---------------------------------------------------------------------------
// #363：内存组成带（两个口径不合并）、空态短标签、网卡明细分段
// ---------------------------------------------------------------------------

describe('buildMemoryComposition · 组成带（已用 / 缓存 / 缓冲 / 空闲）', () => {
  const GIB = 1024 ** 3

  /** 四段齐全的组成带：段序固定 used → cached → buffers → free，宽度按各段占总量比例 */
  it('四段齐全：free 由前三段派生，四段之和等于总量', () => {
    const composition = buildMemoryComposition({
      total_bytes: 32 * GIB,
      available_bytes: 16 * GIB,
      used_bytes: 16 * GIB,
      used_ratio: 0.5,
      cached_bytes: 8 * GIB,
      buffers_bytes: 1 * GIB,
    })!

    expect(composition.degraded).toBe(false)
    expect(composition.segments.map(segment => segment.id)).toEqual(['used', 'cached', 'buffers', 'free'])
    expect(composition.segments.map(segment => segment.bytes)).toEqual([
      // 「已用」= total − free − cached − buffers = 32 − 7 − 8 − 1 = 16 GiB
      16 * GIB,
      8 * GIB,
      1 * GIB,
      // free 不是快照字段：32 − 16 − 8 − 1 = 7 GiB
      7 * GIB,
    ])
    const sum = composition.segments.reduce((total, segment) => total + (segment.bytes ?? 0), 0)
    expect(sum).toBe(32 * GIB)
    // 比例合计 1（截断误差在浮点范围内）
    expect(composition.segments.reduce((total, segment) => total + (segment.ratio ?? 0), 0)).toBeCloseTo(1, 6)
  })

  it('组成带的「已用」按 total − free − cached − buffers 派生，与主数值的 available 口径各自独立', () => {
    // 夹具：总计 32 GiB、已用 12 GiB、缓存 5 GiB、缓冲 1 GiB → 派生空闲 = 32 − 12 − 5 − 1 = 14 GiB。
    // `available` 取 18 GiB 而非 20 GiB：真实 Linux 上 available 含可回收的缓存/缓冲，
    // 因此 available 必大于「总量 − 已用」。这样两个口径**本帧不相等**，夹具才具备鉴别力。
    const composition = buildMemoryComposition({
      total_bytes: 32 * GIB,
      available_bytes: 18 * GIB,
      used_bytes: 12 * GIB,
      used_ratio: 12 / 32,
      cached_bytes: 5 * GIB,
      buffers_bytes: 1 * GIB,
    })!
    const used = composition.segments.find(segment => segment.id === 'used')!
    // 组成带口径：已用 = total − free − cached − buffers = 32 − 14 − 5 − 1 = 12 GiB
    expect(used.bytes).toBe(12 * GIB)
    expect(composition.segments.find(segment => segment.id === 'free')!.bytes).toBe(14 * GIB)
    // 主数值口径是 total − available = 14 GiB，与上面的 12 GiB 是两个量。
    // 两条断言一起看：谁也不许被「合并」成另一个——把 used 段改写成 total − available 会让
    // 第一条变红，把主数值改写成派生口径会让第二条变红。
    expect(32 * GIB - 18 * GIB).toBe(14 * GIB)
    expect(used.bytes).not.toBe(32 * GIB - 18 * GIB)
  })

  it('缓存 / 缓冲为 null：退化为两段并标注，不补 0', () => {
    const composition = buildMemoryComposition({
      total_bytes: 32 * GIB,
      available_bytes: 16 * GIB,
      used_bytes: 16 * GIB,
      used_ratio: 0.5,
      cached_bytes: null,
      buffers_bytes: null,
    })!

    expect(composition.degraded).toBe(true)
    expect(composition.segments.map(segment => segment.id)).toEqual(['used', 'free'])
    // 退化态的两段之和仍是总量（已用 = total − available）
    expect(composition.segments.reduce((total, segment) => total + (segment.bytes ?? 0), 0)).toBe(32 * GIB)
    // 标注说明为什么只有两段——不静默丢段
    expect(composition.degradedText).toContain('缓存与缓冲不可得')
  })

  it('缓存可得以缓冲为 null：同样退化两段，不把缓冲当 0 画一段', () => {
    const composition = buildMemoryComposition({
      total_bytes: 32 * GIB,
      available_bytes: 16 * GIB,
      used_bytes: 16 * GIB,
      used_ratio: 0.5,
      cached_bytes: 8 * GIB,
      buffers_bytes: null,
    })!
    expect(composition.degraded).toBe(true)
    expect(composition.segments.map(segment => segment.id)).toEqual(['used', 'free'])
    // 缓存 8 GiB 不可丢：退化态把它并入「可用（含缓存/缓冲）」段而不是静默消失
    expect(composition.segments.some(segment => segment.id === 'cached')).toBe(false)
    expect(composition.segments.find(segment => segment.id === 'free')!.bytes).toBe(16 * GIB)
  })

  it('内存对象缺失或总量非正：返回 null（调用方据此不渲染组成带）', () => {
    expect(buildMemoryComposition(null)).toBeNull()
    expect(buildMemoryComposition({
      total_bytes: 0,
      available_bytes: 0,
      used_bytes: 0,
      used_ratio: null,
      cached_bytes: 0,
      buffers_bytes: 0,
    })).toBeNull()
  })

  it('脏数据（可用 > 总量）：段宽不为负，已用段钳到 0', () => {
    const composition = buildMemoryComposition({
      total_bytes: 16 * GIB,
      available_bytes: 32 * GIB,
      used_bytes: 0,
      used_ratio: 0,
      cached_bytes: 32 * GIB,
      buffers_bytes: 32 * GIB,
    })!
    // 派生空闲 = max(0, 16 − 0 − 32 − 32) = 0，已用段 = max(0, 16 − 0 − 32 − 32) = 0：
    // 钳位把两个负值都折到 0（不是「随便一个非负数」），比 >= 0 更能锁住 `Math.max(0, …)`
    const byId = new Map(composition.segments.map(segment => [segment.id, segment]))
    expect(byId.get('used')!.bytes).toBe(0)
    expect(byId.get('free')!.bytes).toBe(0)
    expect(byId.get('used')!.ratio).toBe(0)
    expect(byId.get('free')!.ratio).toBe(0)
    // 缓存 / 缓冲是快照原值，段宽不被截断：超量部分的 ratio 由 `Math.min(1, …)` 钳到 1
    expect(byId.get('cached')!.bytes).toBe(32 * GIB)
    expect(byId.get('buffers')!.bytes).toBe(32 * GIB)
    expect(byId.get('cached')!.ratio).toBe(1)
    expect(byId.get('buffers')!.ratio).toBe(1)
  })
})

describe('LedgerRow.stateLabel · 四类空态各给短标签（状态不得只靠颜色）', () => {
  it('ok 行的标签为空（等级文字在 levelText 里）；空态行的标签非空且各不相同', () => {
    const groups = buildMonitorLedger(ledgerInput({ metrics: allAvailable() }))
    const memoryRow = rowById(groups, 'memory')
    expect(memoryRow.state).toBe('ok')
    expect(memoryRow.stateLabel).toBe('')
    expect(memoryRow.levelText).toBe('正常')

    // 四类空态：unavailable / empty / disabled / none 的标签互不相同
    const unavailable = buildMonitorLedger(ledgerInput({
      metrics: { ...allAvailable(), memory: { available: false, reason: 'collect_failed' } },
    }))
    expect(rowById(unavailable, 'memory').stateLabel).toBe('不可用')

    const empty = buildMonitorLedger(ledgerInput({ memory: null }))
    expect(rowById(empty, 'memory').stateLabel).toBe('暂无数据')

    const disabled = buildMonitorLedger(ledgerInput({
      metrics: { ...allAvailable(), memory: { available: false, reason: 'disabled_by_config' } },
    }))
    expect(rowById(disabled, 'memory').stateLabel).toBe('已关闭')

    const none = buildMonitorLedger(ledgerInput({ swap: { total_bytes: 0, used_bytes: 0, used_ratio: null } }))
    // 「无交换」不是「无此维度」：交换这一类事实各有各的词
    expect(rowById(none, 'swap').stateLabel).toBe('无交换')
  })

  it('时钟「尚未探测」的短标签就是「尚未探测」，不混进任何空态类别词', () => {
    const groups = buildMonitorLedger(ledgerInput({ clock: SYSMON_CLOCK_UNPROBED }))
    const row = rowById(groups, 'clock')
    expect(row.state).toBe('none')
    expect(row.stateLabel).toBe('尚未探测')
    expect(row.stateText).toBe('尚未探测')
  })

  it('ok 行没有 stateText（长文案只属于空态）', () => {
    const groups = buildMonitorLedger(ledgerInput({ metrics: allAvailable() }))
    for (const group of groups) {
      for (const row of group.rows) {
        if (row.state === 'ok') expect(row.stateText).toBe('')
        else expect(row.stateText.length).toBeGreaterThan(0)
      }
    }
  })
})

describe('网卡元的明细分段（错误 / 丢弃各自可寻址）', () => {
  it('收发两行各自给 errors / drops 两段，速率与累计量并列', () => {
    const groups = buildMonitorLedger(ledgerInput({ metrics: allAvailable() }))
    const rx = rowById(groups, 'nic:eth0:rx')
    expect(rx.detailParts.map(part => part.id)).toEqual(['errors', 'drops'])
    for (const part of rx.detailParts) {
      // 速率（可为 0）+ 累计量（始终有效）都在同一段里
      expect(part.text).toContain('/s')
      expect(part.text).toContain('累计')
    }
  })

  it('速率 null 时该段写「不可用」但仍给累计量（累计计数器没有不可用语义）', () => {
    const groups = buildMonitorLedger(ledgerInput({
      metrics: allAvailable(),
      network: [{
        ...makeSysmonSnapshot({ resourceData: true }).network![0]!,
        rx_errors_per_sec: null,
        rx_drops_per_sec: null,
      }],
    }))
    const rx = rowById(groups, 'nic:eth0:rx')
    expect(rx.detailParts[0].text).toContain('不可用')
    expect(rx.detailParts[0].text).toContain('累计 3')
    expect(rx.detailParts[0].text).not.toContain('0.0/s')
  })

  it('非网卡行没有分段（明细列就是一句话）', () => {
    const groups = buildMonitorLedger(ledgerInput({ metrics: allAvailable() }))
    for (const group of groups) {
      for (const row of group.rows) {
        if (!row.id.startsWith('nic:')) expect(row.detailParts).toEqual([])
      }
    }
  })
})

describe('网卡全被过滤 vs 本机确实无网卡：文案不同（都不是「不可用」）', () => {
  it('networkFilteredAll=true：空态行说明「已隐藏全部虚拟网卡」', () => {
    const groups = buildMonitorLedger(ledgerInput({ network: [] }, {}, { networkFilteredAll: true }))
    const row = rowById(groups, 'network')
    expect(row.state).toBe('empty')
    expect(row.stateText).toContain('已隐藏全部虚拟网卡')
    expect(row.stateText).not.toContain('暂无数据')
  })

  it('networkFilteredAll 缺省 / false：写「暂无数据」，不臆测是过滤造成的', () => {
    for (const input of [
      ledgerInput({ network: [] }),
      ledgerInput({ network: [] }, {}, { networkFilteredAll: false }),
    ]) {
      const row = rowById(buildMonitorLedger(input), 'network')
      expect(row.stateText).toContain('暂无数据')
      expect(row.stateText).not.toContain('已隐藏全部虚拟网卡')
    }
  })

  it('network=null（本帧未采集）与空数组走同一档空态，都不说成「不可用」', () => {
    const row = rowById(buildMonitorLedger(ledgerInput({ network: null })), 'network')
    expect(row.state).toBe('empty')
    expect(row.stateText).not.toContain('不可用')
  })
})
