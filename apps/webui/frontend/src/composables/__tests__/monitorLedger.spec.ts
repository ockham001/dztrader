import { describe, it, expect } from 'vitest'
import {
  LEDGER_DEFAULT_MEMORY_USED_PCT,
  LEDGER_DEFAULT_SWAP_USED_PCT,
  LEDGER_DEFAULT_VOLUME_FREE_PCT,
  NO_THRESHOLD_TEXT,
  adaptiveAxisTicks,
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
  it('三组恒按 内存 → 存储 → 网络与时钟 排列', () => {
    const { groups } = ledgerFrom({ metrics: allAvailable() })
    expect(groups.map(group => group.id)).toEqual(['memory', 'storage', 'network_clock'])
    expect(groups.map(group => group.title)).toEqual(['内存', '存储', '网络与时钟'])
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

  it('ratio 为 null → level 为 null 且 levelText 为空串（不得冒充 normal）', () => {
    const { groups } = ledgerFrom({ metrics: allAvailable() })
    const shm = rowById(groups, 'shm_dir')
    expect(shm.ratio).toBeNull()
    expect(shm.level).toBeNull()
    expect(shm.levelText).toBe('')
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
  it('内存 / 交换有 historyKey，卷 / 盘 / 网卡 / shm / 时钟为 null', () => {
    const { groups } = ledgerFrom({ metrics: allAvailable(), clock: SYSMON_CLOCK_LIVE })
    expect(rowById(groups, 'memory').historyKey).toBe('memory.used_ratio')
    expect(rowById(groups, 'swap').historyKey).toBe('swap.used_ratio')
    expect(rowById(groups, 'volume:/').historyKey).toBeNull()
    expect(rowById(groups, 'disk:sda').historyKey).toBeNull()
    expect(rowById(groups, 'shm_dir').historyKey).toBeNull()
    expect(rowById(groups, 'clock').historyKey).toBeNull()
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
  it('等级映射到中文，null 返回空串', () => {
    expect(ledgerLevelText('normal')).toBe('正常')
    expect(ledgerLevelText('warn')).toBe('警戒')
    expect(ledgerLevelText('danger')).toBe('危险')
    expect(ledgerLevelText(null)).toBe('')
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
})
