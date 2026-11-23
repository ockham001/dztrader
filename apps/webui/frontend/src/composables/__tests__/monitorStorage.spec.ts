import { describe, it, expect } from 'vitest'
import {
  DEFAULT_VOLUME_FREE_PCT,
  buildDiskRow,
  buildStorageSummary,
  buildVolumeRow,
  resolveVolumeFreePct,
  shmUsageText,
  volumeFreePercent,
  volumeReasonText,
  zeroCapacityText,
} from '@/composables/monitorStorage'
import type {
  SysmonDiskIoInfo,
  SysmonThresholdsConfigPayload,
  SysmonVolumeInfo,
} from '@/types/generated'

// #450：存储块的纯函数展示口径（卷统计 / 空间不足阈值 / 卷行与盘行格式化 / 无容量挂载行）。
// 这些断言刻意按**用户可见文本**写（不从被测实现取期望），覆盖三处最容易做错的地方：
// 空间不足是「空闲低于契约阈值」而不是「已用高于 70/90」；容量条填充的是已用；
// 失败 / 首帧的 null 一律「不可用」而不是 0。

const GIB = 1024 ** 3
const MIB = 1024 ** 2

function volume(overrides: Partial<SysmonVolumeInfo> & Pick<SysmonVolumeInfo, 'mount'>): SysmonVolumeInfo {
  return {
    available: true,
    total_bytes: 100 * GIB,
    used_bytes: 40 * GIB,
    used_ratio: 0.4,
    ...overrides,
  }
}

function thresholds(volumeFreePct: number): SysmonThresholdsConfigPayload {
  return {
    cpu_sustain_used_pct: 90,
    cpu_sustain_samples: 10,
    memory_used_pct: 90,
    swap_used_pct: 50,
    volume_free_pct: volumeFreePct,
    log_repeat_sec: 600,
  }
}

describe('monitorStorage 阈值解析（契约 sysmon §3.1 volume_free_pct）', () => {
  it('配置镜像未到达 / 字段非法时回退契约默认 10（避免标签先按一套阈值、配置到达后跳变）', () => {
    expect(DEFAULT_VOLUME_FREE_PCT).toBe(10)
    expect(resolveVolumeFreePct(null)).toBe(10)
    expect(resolveVolumeFreePct(undefined)).toBe(10)
    expect(resolveVolumeFreePct(thresholds(20))).toBe(20)
    expect(resolveVolumeFreePct({ ...thresholds(10), volume_free_pct: Number.NaN })).toBe(10)
  })
})

describe('monitorStorage 卷行与空间不足判据', () => {
  it('卷行：容量条按已用填充，可用值与总量分开给（百分比与判定同源）', () => {
    const row = buildVolumeRow(volume({ mount: '/', total_bytes: 100 * GIB, used_bytes: 40 * GIB }), 10)
    expect(row).toMatchObject({
      mount: '/',
      fill: '40.0%',
      level: 'normal',
      availableText: '60.0 GiB / 100.0 GiB',
      freeBytes: 60 * GIB,
      totalBytes: 100 * GIB,
      freeText: '60.0% 可用',
      insufficient: false,
    })
  })

  it('空间不足判据是**空闲**低于契约阈值（不是已用 ≥70/90）：边界恰好 10.0% 不算不足', () => {
    // used_ratio = 0.9：浮点 (1 − 0.9) × 100 = 9.999…，按展示精度取整后是 10.0 → 不低于 10
    const boundary = buildVolumeRow(volume({ mount: '/boundary', used_ratio: 0.9, used_bytes: 90 * GIB }), 10)
    expect(volumeFreePercent({ used_ratio: 0.9 })).toBe(10)
    expect(boundary.freeText).toBe('10.0% 可用')
    expect(boundary.insufficient).toBe(false)
    // 两套数字不互相推导：条色按 70/90 展示分档（used=0.90 → danger），chip 按契约阈值（恰好 10% 不算不足）
    expect(boundary.level).toBe('danger')

    const below = buildVolumeRow(volume({ mount: '/below', used_ratio: 0.91, used_bytes: 91 * GIB }), 10)
    expect(below.freeText).toBe('9.0% 可用')
    expect(below.insufficient).toBe(true)

    // 改阈值即改判定（前端不另定一套常量），且颜色与 chip 一起变
    const stricter = buildVolumeRow(volume({ mount: '/boundary', used_ratio: 0.9 }), 11)
    expect(stricter.insufficient).toBe(true)
    expect(stricter.level).toBe('danger')
  })

  it('条色三档走 70/90 展示分档：<70 normal（中性）、70–90 warn、≥90 danger（与顶栏 CPU/内存徽章一致）', () => {
    expect(buildVolumeRow(volume({ mount: '/a', used_ratio: 0.69 }), 10).level).toBe('normal')
    expect(buildVolumeRow(volume({ mount: '/b', used_ratio: 0.71 }), 10).level).toBe('warn')
    const low = buildVolumeRow(volume({ mount: '/c', used_ratio: 0.95 }), 10)
    expect(low.level).toBe('danger')
    expect(low.insufficient).toBe(true)
  })

  it('已用比例 null（不可读 / 总量 0）不冒充 0%：可用文本与 freePercent 走不可用语义', () => {
    const row = buildVolumeRow(volume({
      mount: '/x',
      total_bytes: null,
      used_bytes: null,
      used_ratio: null,
    }), 10)
    expect(row.fill).toBe('不可用')
    expect(row.level).toBeNull()
    expect(row.freeText).toContain('不可用')
    expect(row.insufficient).toBe(false)
  })
})

describe('monitorStorage 顶部四数字统计', () => {
  const readable = [
    volume({ mount: '/', total_bytes: 100 * GIB, used_bytes: 40 * GIB, used_ratio: 0.4 }),
    volume({ mount: '/data', total_bytes: 1024 * GIB, used_bytes: 256 * GIB, used_ratio: 0.25 }),
    volume({ mount: '/home', total_bytes: 50 * GIB, used_bytes: 31 * GIB, used_ratio: 0.62 }),
  ]
  const zeroCapacity = [
    volume({ mount: '/backup', available: false, total_bytes: null, used_bytes: null, used_ratio: null, reason: 'no_permission' }),
  ]

  it('卷数 / 总容量 / 可用 / 最紧的卷与无容量计数', () => {
    const summary = buildStorageSummary({
      readableVolumes: readable,
      zeroCapacityVolumes: zeroCapacity,
      diskCount: 2,
      freeThresholdPct: 10,
    })
    expect(summary.volumeCount).toBe(3)
    expect(summary.zeroCount).toBe(1)
    expect(summary.diskCount).toBe(2)
    expect(summary.totalBytes).toBe(1174 * GIB)
    expect(summary.freeBytes).toBe(847 * GIB)
    expect(summary.freeTotalPercentText).toBe('72.1%')
    expect(summary.insufficientCount).toBe(0)
    // 最紧 = 空闲百分比最低（/home 38%）；同值时保持 store 固定顺序（不重新排序）
    expect(summary.tightest).toEqual({ mount: '/home', freePercent: 38, freeBytes: 19 * GIB })
  })

  it('空间不足计数与最紧的卷：多卷不足时取最低者，计数按契约阈值', () => {
    const summary = buildStorageSummary({
      readableVolumes: [
        volume({ mount: '/a', used_ratio: 0.8, used_bytes: 80 * GIB }),   // 空闲 20%
        volume({ mount: '/b', used_ratio: 0.97, used_bytes: 97 * GIB }),  // 空闲 3%
        volume({ mount: '/c', used_ratio: 0.95, used_bytes: 95 * GIB }),  // 空闲 5%
      ],
      zeroCapacityVolumes: [],
      diskCount: 0,
      freeThresholdPct: 10,
    })
    expect(summary.insufficientCount).toBe(2)
    expect(summary.tightest).toEqual({ mount: '/b', freePercent: 3, freeBytes: 3 * GIB })
  })

  it('无可用卷：求和为 0、没有「最紧的卷」、可用占比给 —（不是 0.0%）', () => {
    const summary = buildStorageSummary({
      readableVolumes: [],
      zeroCapacityVolumes: zeroCapacity,
      diskCount: 0,
      freeThresholdPct: 10,
    })
    expect(summary.volumeCount).toBe(0)
    expect(summary.totalBytes).toBe(0)
    expect(summary.freeTotalPercentText).toBe('—')
    expect(summary.insufficientCount).toBe(0)
    expect(summary.tightest).toBeNull()
  })
})

describe('monitorStorage 无容量挂载与原因文案', () => {
  it('无容量挂载折叠成一行：逐个给挂载点与原因（读不到 ≠ 总量为 0）', () => {
    const text = zeroCapacityText([
      volume({ mount: '/run/user/1000', total_bytes: 0, used_bytes: 0, used_ratio: null }),
      volume({ mount: '/backup', available: false, total_bytes: null, used_bytes: null, used_ratio: null, reason: 'no_permission' }),
    ])
    expect(text).toBe('无容量挂载（2） · /run/user/1000（总容量为 0） · /backup（权限不足，无法读取该指标）')
    expect(zeroCapacityText([])).toBeNull()
  })

  it('原因映射：已知 reason 中文、未知 reason 原样透出、缺失给兜底', () => {
    expect(volumeReasonText('no_permission')).toBe('权限不足，无法读取该指标')
    expect(volumeReasonText('collect_failed')).toBe('采集失败，暂不可用')
    expect(volumeReasonText('disabled_by_config')).toBe('已在配置中关闭')
    expect(volumeReasonText('some_new_reason')).toBe('some_new_reason')
    expect(volumeReasonText(undefined)).toBe('容量读取失败')
  })
})

describe('monitorStorage 磁盘行格式化（契约 sysmon §2.4 disk_io）', () => {
  const disk: SysmonDiskIoInfo = {
    device: 'nvme0n1',
    read_bytes_per_sec: 64 * MIB,
    write_bytes_per_sec: 32 * MIB,
    read_iops: 2048,
    write_iops: 1024,
    latency_ms: 0.45,
    queue_depth: 1.25,
    utilization: 0.72,
  }

  it('吞吐 / 忙碌占比 / 延迟 / 队列：忙碌色标按 70/90 分档，文案叫「忙碌」不叫「利用率」', () => {
    const row = buildDiskRow(disk)
    expect(row.device).toBe('nvme0n1')
    expect(row.ioText).toBe('读 64.0 MiB/s · 写 32.0 MiB/s')
    expect(row.busyFill).toBe('72.0%')
    expect(row.busyLevel).toBe('warn')
    expect(row.busyText).toBe('忙碌 72.0%')
    expect(row.busyText).not.toContain('利用率')
    expect(row.extraText).toBe('延迟 0.45 ms · 队列 1.25')
  })

  it('首帧 / 计数器重置（派生字段全 null）：逐项「不可用」，不注入填充（不冒充 0%）', () => {
    const row = buildDiskRow({
      ...disk,
      read_bytes_per_sec: null,
      write_bytes_per_sec: null,
      read_iops: null,
      write_iops: null,
      latency_ms: null,
      queue_depth: null,
      utilization: null,
    })
    expect(row.ioText).toBe('读 不可用 · 写 不可用')
    expect(row.busyFill).toBe('')
    expect(row.busyLevel).toBeNull()
    expect(row.busyText).toBe('忙碌 不可用')
    expect(row.extraText).toBe('延迟 不可用 · 队列 不可用')
  })
})

describe('monitorStorage 共享内存一行', () => {
  it('占用 · 页文件数 · 最旧年龄（无页文件写「无页文件」，有页文件但年龄缺失才写不可用）', () => {
    expect(shmUsageText({
      path: '/srv/dztrader/shm',
      total_bytes: 720 * MIB,
      page_files: 12,
      oldest_file_age_sec: 480,
    })).toBe('720.0 MiB · 12 页文件 · 最旧 8 分钟')

    expect(shmUsageText({
      path: '/srv/dztrader/shm',
      total_bytes: 0,
      page_files: 0,
      oldest_file_age_sec: null,
    })).toBe('0 B · 0 页文件 · 最旧 无页文件')

    // 有页文件但年龄读不到：值缺失，写不可用（不是「没有页文件」）
    expect(shmUsageText({
      path: '/srv/dztrader/shm',
      total_bytes: 1024,
      page_files: 3,
      oldest_file_age_sec: null,
    })).toBe('1.0 KiB · 3 页文件 · 最旧 不可用')
  })
})
