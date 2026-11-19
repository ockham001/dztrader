import { describe, it, expect } from 'vitest'
import {
  DEFAULT_PUSH_INTERVAL_SEC,
  SNAPSHOT_STALE_PUSH_MULTIPLE,
  isSnapshotStale,
  resolvePushIntervalSec,
  snapshotAgeMs,
  staleSnapshotText,
  staleThresholdMs,
} from '../sysmonFreshness'

// #371 快照数据龄的纯函数单测：契约 sysmon §2（快照 `timestamp` = 生成时刻，Unix 毫秒）、
// §3.1（`realtime.push_interval_sec` 默认 5、[1, 3600]）、§4（周期推送间隔 = 该配置）。
// 直接调纯函数（不 mount、不渲染、不挂 pinia），与 composables/__tests__/sysmonClock.spec.ts 同形态。
//
// 断言一律用**字面量**而非用被测函数或常量反推期望值：从 `3 × 5 × 1000` 算出 15000 再断言
// `isSnapshotStale(15001)` 为 true，改坏了倍数常量后两边一起变，用例仍绿。

describe('snapshotAgeMs：数据龄 = 本机此刻 − 快照 timestamp', () => {
  it('正常：差值为毫秒数（快照 1.5 秒前生成）', () => {
    const timestamp = Date.UTC(2026, 9, 21, 15, 0, 0)
    expect(snapshotAgeMs(timestamp, timestamp + 1_500)).toBe(1_500)
  })

  it('快照无 timestamp（0 = 不是一次有效生成时刻）返回 null，不用 0 冒充「刚刚生成」', () => {
    expect(snapshotAgeMs(0, Date.UTC(2026, 9, 21, 15, 0, 1))).toBeNull()
  })

  it('timestamp 非法（NaN / Infinity / 负数）返回 null', () => {
    const now = Date.UTC(2026, 9, 21, 15, 0, 1)
    expect(snapshotAgeMs(Number.NaN, now)).toBeNull()
    expect(snapshotAgeMs(Number.POSITIVE_INFINITY, now)).toBeNull()
    expect(snapshotAgeMs(-1, now)).toBeNull()
  })

  it('本机此刻非法（NaN）返回 null：不把「读不到时间」算成 0 秒龄', () => {
    expect(snapshotAgeMs(Date.UTC(2026, 9, 21, 15, 0, 0), Number.NaN)).toBeNull()
  })

  it('时钟回拨：发送方时间戳比本机快时返回负值（不截断成 0）', () => {
    const timestamp = Date.UTC(2026, 9, 21, 15, 0, 0)
    // 快照时间戳比本机「此刻」晚 2.5 秒 = 两机时钟偏差，差值必须如实为负
    expect(snapshotAgeMs(timestamp, timestamp - 2_500)).toBe(-2_500)
  })
})

describe('isSnapshotStale：越过 3 × push_interval_sec 才判停推', () => {
  it('默认推送间隔 5 秒：阈值 15 秒，边界取严格大于', () => {
    expect(isSnapshotStale(14_999, 5)).toBe(false)
    expect(isSnapshotStale(15_000, 5)).toBe(false)   // 正好 3 倍不算停推
    expect(isSnapshotStale(15_001, 5)).toBe(true)
  })

  it('推送间隔变化即改阈值：1 秒 → 3 秒、3600 秒 → 3 小时', () => {
    expect(isSnapshotStale(3_000, 1)).toBe(false)
    expect(isSnapshotStale(3_001, 1)).toBe(true)
    // 配置上限 3600 秒：阈值 10800 秒（3 小时），10 分钟的数据龄仍属正常
    expect(isSnapshotStale(600_000, 3_600)).toBe(false)
    expect(isSnapshotStale(10_800_001, 3_600)).toBe(true)
  })

  it('无 timestamp（null）不判停推：不知道数据有多旧 ≠ 数据停了', () => {
    expect(isSnapshotStale(null, 5)).toBe(false)
  })

  it('时钟回拨（负值）不判停推：时钟不准不是数据停了', () => {
    expect(isSnapshotStale(-60_000, 5)).toBe(false)
  })

  it('阈值为 3 倍推送间隔（#331 triage 结论，不是 2 倍）', () => {
    expect(SNAPSHOT_STALE_PUSH_MULTIPLE).toBe(3)
    expect(staleThresholdMs(5)).toBe(15_000)
    expect(staleThresholdMs(1)).toBe(3_000)
  })
})

describe('staleSnapshotText：降级文案', () => {
  it('未超阈值返回 null（调用方据此保留「实时快照」）', () => {
    expect(staleSnapshotText(9_000, 5)).toBeNull()
    expect(staleSnapshotText(15_000, 5)).toBeNull()
    expect(staleSnapshotText(null, 5)).toBeNull()
    expect(staleSnapshotText(-2_500, 5)).toBeNull()
  })

  it('超阈值给「快照已停 N 秒」：秒数向下取整', () => {
    expect(staleSnapshotText(16_000, 5)).toBe('快照已停 16 秒')
    // 18.9 秒 → 18 秒（向下取整：向上会把 17.1 秒报成 18 秒，读者以为更久）
    expect(staleSnapshotText(18_900, 5)).toBe('快照已停 18 秒')
    // 17.1 秒 → 17 秒（若误用四舍五入会得到 17、若误用向上取整会得到 18）
    expect(staleSnapshotText(17_100, 5)).toBe('快照已停 17 秒')
  })

  it('超过一分钟复用同一份「多久以前」口径（formatAge），不另写一套取整', () => {
    expect(staleSnapshotText(75_000, 5)).toBe('快照已停 1 分钟')
    expect(staleSnapshotText(3_600_000, 5)).toBe('快照已停 1 小时')
  })
})

describe('resolvePushIntervalSec：阈值来源是生效配置，不是组件里的常量', () => {
  it('读生效配置的 realtime.push_interval_sec', () => {
    expect(resolvePushIntervalSec({ push_interval_sec: 30 })).toBe(30)
  })

  it('配置未到达 / 字段缺失 / 非正数：回退契约默认 5 秒（§3.1）', () => {
    expect(DEFAULT_PUSH_INTERVAL_SEC).toBe(5)
    expect(resolvePushIntervalSec(null)).toBe(5)
    expect(resolvePushIntervalSec(undefined)).toBe(5)
    expect(resolvePushIntervalSec({})).toBe(5)
    expect(resolvePushIntervalSec({ push_interval_sec: 0 })).toBe(5)
    expect(resolvePushIntervalSec({ push_interval_sec: -5 })).toBe(5)
    expect(resolvePushIntervalSec({ push_interval_sec: Number.NaN })).toBe(5)
  })

  it('超出契约区间 [1, 3600] 的值按边界收窄', () => {
    expect(resolvePushIntervalSec({ push_interval_sec: 0.5 })).toBe(1)
    expect(resolvePushIntervalSec({ push_interval_sec: 7_200 })).toBe(3_600)
  })
})
