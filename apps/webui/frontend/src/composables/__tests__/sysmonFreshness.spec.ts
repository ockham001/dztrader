import { describe, it, expect } from 'vitest'
import {
  DEFAULT_PUSH_INTERVAL_SEC,
  SNAPSHOT_STALE_PUSH_MULTIPLE,
  isSnapshotStale,
  receivedAgeMs,
  resolvePushIntervalSec,
  staleSnapshotText,
  staleThresholdMs,
} from '../sysmonFreshness'

// #371 停推判定的纯函数单测：契约 sysmon §2（快照 `timestamp` = 生成时刻，Unix 毫秒）、
// §3.1（`realtime.push_interval_sec` 默认 5、[1, 3600]）、§4（周期推送间隔 = 该配置）。
// 直接调纯函数（不 mount、不渲染、不挂 pinia），与 composables/__tests__/sysmonClock.spec.ts 同形态。
//
// 断言一律用**字面量**而非用被测函数或常量反推期望值：从 `3 × 5 × 1000` 算出 15000 再断言
// `isSnapshotStale(15001)` 为 true，改坏了倍数常量后两边一起变，用例仍绿。

describe('receivedAgeMs：距上次收到新快照 = 本机此刻 − 本机接收时刻', () => {
  it('正常：差值为毫秒数（1.5 秒前收到）', () => {
    const receivedAt = Date.UTC(2026, 9, 21, 15, 0, 0)
    expect(receivedAgeMs(receivedAt, receivedAt + 1_500)).toBe(1_500)
  })

  it('还没有任何接收记录（null）返回 null：不拿「不知道」冒充「刚刚收到」', () => {
    expect(receivedAgeMs(null, Date.UTC(2026, 9, 21, 15, 0, 1))).toBeNull()
  })

  it('接收时刻非法（NaN / Infinity / 0 / 负数）返回 null', () => {
    const now = Date.UTC(2026, 9, 21, 15, 0, 1)
    expect(receivedAgeMs(Number.NaN, now)).toBeNull()
    expect(receivedAgeMs(Number.POSITIVE_INFINITY, now)).toBeNull()
    expect(receivedAgeMs(0, now)).toBeNull()
    expect(receivedAgeMs(-1, now)).toBeNull()
  })

  it('本机此刻非法（NaN）返回 null：不把「读不到时间」算成 0', () => {
    expect(receivedAgeMs(Date.UTC(2026, 9, 21, 15, 0, 0), Number.NaN)).toBeNull()
  })

  it('时钟倒流（now < 接收时刻）返回 null：同一口钟自相矛盾时不作断言', () => {
    // 两个数出自同一口本机钟。它们互相矛盾 ⇒ 这口钟此刻不可信 ⇒ 既不说「停推」也不说「新鲜」。
    // 钳 0 会把「不可信」写成「刚刚收到」，与本模块「不用 0 冒充刚生成」是同一笔账。
    const receivedAt = Date.UTC(2026, 9, 21, 15, 0, 0)
    expect(receivedAgeMs(receivedAt, receivedAt - 2_500)).toBeNull()
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
    // 配置上限 3600 秒：阈值 10800 秒（3 小时），10 分钟的接收间隔仍属正常
    expect(isSnapshotStale(600_000, 3_600)).toBe(false)
    expect(isSnapshotStale(10_800_001, 3_600)).toBe(true)
  })

  it('无接收记录（null）不判停推：不知道 ≠ 停了', () => {
    expect(isSnapshotStale(null, 5)).toBe(false)
  })

  it('阈值为 3 倍推送间隔（#331 triage 结论，不是 2 倍）', () => {
    expect(SNAPSHOT_STALE_PUSH_MULTIPLE).toBe(3)
    expect(staleThresholdMs(5)).toBe(15_000)
    expect(staleThresholdMs(1)).toBe(3_000)
  })
})

describe('判据是本机接收时刻，不是发送方 timestamp（#371 / #331 triage）', () => {
  // 这组用例存在的理由：旧口径（本机 now − 发送方 snapshot.timestamp）在两机时钟偏差下
  // **两个方向都会坏**。要证明它确实有牙，不能只断言「新口径没判停推」——那对任何实现都成立。
  // 所以每条都同时算出「旧口径会得到的值」，并断言那个值确实越过了阈值（或确实为负），
  // 从而证明这条输入正是会让旧口径误判的那类输入。

  it('发送方钟落后 60 秒：旧口径会永久判停推，新口径不判', () => {
    const now = Date.UTC(2026, 9, 21, 15, 0, 0)
    // 快照确实是刚收到的（本机刚记了接收时刻）……
    const receivedAt = now
    // ……但发送方这口钟慢了 60 秒，它给 timestamp 写的是 now − 60s
    const senderTimestamp = now - 60_000

    // 先证明这条输入会让旧口径误判：60 秒的数据龄 > 15 秒阈值 → 旧口径会说「快照已停 1 分钟」
    const ageUnderOldRule = now - senderTimestamp
    expect(ageUnderOldRule).toBe(60_000)
    expect(isSnapshotStale(ageUnderOldRule, 5)).toBe(true)   // ← 旧口径在这里就是错的

    // 新口径：本机接收时刻就是此刻 → 0 毫秒数据龄 → 不判停推
    expect(receivedAgeMs(receivedAt, now)).toBe(0)
    expect(isSnapshotStale(receivedAgeMs(receivedAt, now), 5)).toBe(false)
    expect(staleSnapshotText(receivedAgeMs(receivedAt, now), 5)).toBeNull()
  })

  it('发送方钟超前 60 秒：旧口径永远不判停推（静默失效），新口径照常判', () => {
    const now = Date.UTC(2026, 9, 21, 15, 0, 0)
    // 真停推了：本机最后一次收到快照是 60 秒前……
    const receivedAt = now - 60_000
    // ……而发送方这口钟快 60 秒，最后一帧的 timestamp 反倒「晚于」本机此刻
    const senderTimestamp = now + 60_000

    // 旧口径在这条输入上算出负值，于是永远不判停推 —— 停推检测静默失效
    const ageUnderOldRule = now - senderTimestamp
    expect(ageUnderOldRule).toBe(-60_000)
    expect(isSnapshotStale(ageUnderOldRule, 5)).toBe(false)  // ← 旧口径在这里漏报

    // 新口径：本机 60 秒没收到 → 超 15 秒阈值 → 判停推，且文案是「1 分钟」
    expect(receivedAgeMs(receivedAt, now)).toBe(60_000)
    expect(isSnapshotStale(receivedAgeMs(receivedAt, now), 5)).toBe(true)
    expect(staleSnapshotText(receivedAgeMs(receivedAt, now), 5)).toBe('快照已停 1 分钟')
  })

  it('两机时钟偏差任意大都不影响判定：只看本机记的接收时刻', () => {
    const now = Date.UTC(2026, 9, 21, 15, 0, 0)
    const receivedAt = now - 2_000   // 本机 2 秒前收到
    // 发送方 timestamp 取三个极端值（慢 1 天 / 一致 / 快 1 天）：新口径的结论必须完全一致
    const verdicts = [-86_400_000, 0, 86_400_000].map((senderOffset) => {
      // 旧口径下这三个偏移分别给出「永久假红」「恰好正确」「永久假绿」三种互相矛盾的结论
      const ageUnderOldRule = now - (now + senderOffset)
      return {
        ageUnderOldRule,
        stale: isSnapshotStale(receivedAgeMs(receivedAt, now), 5),
      }
    })
    // 旧口径的三种结论确实不同（-1 天 / 0 / +1 天）→ 证明这条输入有区分力
    expect(verdicts.map((v) => v.ageUnderOldRule))
      .toEqual([86_400_000, 0, -86_400_000])
    // 新口径对三者一致：本机 2 秒前收到 → 不判停推
    expect(verdicts.map((v) => v.stale)).toEqual([false, false, false])
    expect(receivedAgeMs(receivedAt, now)).toBe(2_000)
  })
})

describe('staleSnapshotText：降级文案', () => {
  it('未超阈值返回 null（调用方据此保留「实时快照」）', () => {
    expect(staleSnapshotText(9_000, 5)).toBeNull()
    expect(staleSnapshotText(15_000, 5)).toBeNull()
    expect(staleSnapshotText(null, 5)).toBeNull()
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
