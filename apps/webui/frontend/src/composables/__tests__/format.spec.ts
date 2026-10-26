import { describe, it, expect } from 'vitest'
import {
  UNAVAILABLE_TEXT,
  formatByteRate,
  formatBytes,
  formatCount,
  formatCountPerSec,
  formatDecimal,
  formatDuration,
  formatFrequency,
  formatIops,
  formatLatency,
  formatNumberAuto,
  formatPercent,
  formatPerSec,
  formatTemperature,
} from '../format'

// 输入输出表驱动（#126 AC）：格式化是纯函数，边界值是最容易写错的地方——
// 单位档切换点（1023 B / 1024 B / 1 MiB / 1 GiB / 1 TiB）以及负值、非有限值。

describe('formatBytes：IEC 单位档与精度（1024 进制）', () => {
  const cases: Array<[number, string]> = [
    // ---- B 档取整：字节是最小单位，绝不出现小数与浮点原值 ----
    [0, '0 B'],
    [1, '1 B'],
    [512, '512 B'],
    [1023, '1023 B'], // 1023 B 仍是 B 档（未达 1024）
    [1024, '1.0 KiB'], // 恰好 1024 B 进位到 KiB
    // ---- ≥KiB 固定 1 位小数 ----
    [1025, '1.0 KiB'],
    [1536, '1.5 KiB'],
    // 进位前的最后一档：1048575 B 按 1 位小数会显示成 1024.0 KiB，等于跨过档位，
    // 因此升档显示为 1.0 MiB（与下面的取整后升档用例同一规则）
    [1024 * 1024 - 1, '1.0 MiB'],
    [1024 * 1024, '1.0 MiB'], // 恰好 1 MiB
    [1024 * 1024 * 1024, '1.0 GiB'], // 恰好 1 GiB
    [1024 ** 4, '1.0 TiB'], // 恰好 1 TiB
    // ---- 票面实例：22 TiB 的卷显示 TiB 而不是 22528.0 GiB ----
    [22 * 1024 ** 4, '22.0 TiB'],
    [22528 * 1024 ** 3, '22.0 TiB'], // 等价于 22528 GiB：定标到 TiB，不再是 22528.0 GiB
    // ---- 按显示精度四舍五入后再判定进位：1023.6 B 显示为 KiB 而不是 `1024 B` ----
    [1023.6, '1.0 KiB'],
    // ---- TiB 是展示上限：更大的值仍以 TiB 呈现，不进 PiB ----
    [1024 ** 5, '1024.0 TiB'],
    // 32 GiB 内存 / 256 MiB 缓存（夹具常用值，保证显示稳定）
    [32 * 1024 ** 3, '32.0 GiB'],
    [256 * 1024 ** 2, '256.0 MiB'],
  ]

  it.each(cases)('%d 字节 → %s', (bytes, expected) => {
    expect(formatBytes(bytes)).toBe(expected)
  })

  it('负值不是合法字节数：显示不可用而不是负容量', () => {
    expect(formatBytes(-1)).toBe(UNAVAILABLE_TEXT)
    expect(formatBytes(-1024)).toBe(UNAVAILABLE_TEXT)
  })

  it('非有限值与缺失值一律显示不可用，不得用 0 冒充', () => {
    for (const value of [null, undefined, Number.NaN, Number.POSITIVE_INFINITY, Number.NEGATIVE_INFINITY]) {
      expect(formatBytes(value)).toBe(UNAVAILABLE_TEXT)
    }
  })
})

describe('formatByteRate：速率 = 字节值 + /s', () => {
  const cases: Array<[number, string]> = [
    // 父票实测口径：1823 B / 7 s ≈ 260.43 B/s。按 AC 的单位规则（B 档取整）渲染为 `260 B/s`，
    // 不再输出 260.42857142857144 这类浮点原值。
    [1823 / 7, '260 B/s'],
    // #126 验收标准字面例子 `260.4 KiB/s` 对应的输入：1823 KiB / 7 s ≈ 260.4 KiB/s。
    // （两条并存是刻意的：一条锁 B 档取整，一条锁 KiB 档的 1 位小数。）
    [(1823 * 1024) / 7, '260.4 KiB/s'],
    [0, '0 B/s'], // 0 是合法真实值（本窗口确实没有增量），与「不可用」区分
    [1, '1 B/s'],
    [1023, '1023 B/s'], // 未达 1024，仍是 B 档
    [1024, '1.0 KiB/s'], // 速率与容量共用同一套定标
    [1024 * 1024, '1.0 MiB/s'],
    [64 * 1024 ** 2, '64.0 MiB/s'],
    [2 * 1024 ** 4, '2.0 TiB/s'],
  ]

  it.each(cases)('%d B/s → %s', (rate, expected) => {
    expect(formatByteRate(rate)).toBe(expected)
  })

  it('不可用（首次采样/计数器重置）显示不可用而不是 0', () => {
    for (const value of [null, undefined, Number.NaN, Number.POSITIVE_INFINITY]) {
      expect(formatByteRate(value)).toBe(UNAVAILABLE_TEXT)
    }
  })

  it('负速率不可用（计数器回退应已被上游判定为重置，不得显示负值）', () => {
    expect(formatByteRate(-1)).toBe(UNAVAILABLE_TEXT)
  })
})

describe('formatPercent：比率 → 固定 1 位百分比', () => {
  const cases: Array<[number | null, string]> = [
    [0, '0.0%'], // 0 是合法真实值，不是「不可用」
    [0.5, '50.0%'],
    [0.425, '42.5%'],
    [1, '100.0%'],
    [0.187, '18.7%'],
    [null, UNAVAILABLE_TEXT],
    [Number.NaN, UNAVAILABLE_TEXT],
  ]

  it.each(cases)('%s → %s', (ratio, expected) => {
    expect(formatPercent(ratio)).toBe(expected)
  })
})

describe('formatDecimal / formatLatency：固定小数位', () => {
  it('负载与队列深度固定 2 位，整数也带小数位（列宽稳定优先于单值精度）', () => {
    expect(formatDecimal(0.42)).toBe('0.42')
    expect(formatDecimal(3)).toBe('3.00') // 不因是整数而丢掉小数位
    expect(formatDecimal(0)).toBe('0.00')
    expect(formatDecimal(null)).toBe(UNAVAILABLE_TEXT)
  })

  it('延迟固定 2 位 + ms；无定义（窗口内无完成请求）显示不可用', () => {
    expect(formatLatency(1.25)).toBe('1.25 ms')
    expect(formatLatency(0)).toBe('0.00 ms')
    expect(formatLatency(null)).toBe(UNAVAILABLE_TEXT)
    expect(formatLatency(Number.POSITIVE_INFINITY)).toBe(UNAVAILABLE_TEXT)
  })
})

describe('formatTemperature / formatFrequency：温度与频率', () => {
  it('温度固定 1 位 + °C', () => {
    expect(formatTemperature(45.5)).toBe('45.5 °C')
    expect(formatTemperature(44)).toBe('44.0 °C')
    expect(formatTemperature(27.75)).toBe('27.8 °C') // 正常四舍五入
    expect(formatTemperature(null)).toBe(UNAVAILABLE_TEXT)
  })

  it('频率取整 + MHz', () => {
    expect(formatFrequency(3600)).toBe('3600 MHz')
    expect(formatFrequency(3600.4)).toBe('3600 MHz')
    expect(formatFrequency(null)).toBe(UNAVAILABLE_TEXT)
  })
})

describe('formatIops / formatCountPerSec / formatCount：取整与千分位', () => {
  it('IOPS 取整 + IOPS', () => {
    expect(formatIops(2048)).toBe('2048 IOPS')
    expect(formatIops(0)).toBe('0 IOPS') // 0 是合法真实值
    expect(formatIops(15.4)).toBe('15 IOPS')
    expect(formatIops(null)).toBe(UNAVAILABLE_TEXT)
  })

  it('每秒事件数取整 + /s', () => {
    expect(formatCountPerSec(1234.5)).toBe('1235/s') // 取整四舍五入
    expect(formatCountPerSec(567)).toBe('567/s')
    expect(formatCountPerSec(0)).toBe('0/s')
    expect(formatCountPerSec(null)).toBe(UNAVAILABLE_TEXT)
  })

  it('formatPerSec 指定小数位（错误/丢弃速率 1 位）；不可用时不带 /s 后缀', () => {
    expect(formatPerSec(0, 1)).toBe('0.0/s') // 0 是合法真实值（本窗口无增量）
    expect(formatPerSec(1.25, 1)).toBe('1.3/s')
    expect(formatPerSec(1234.5)).toBe('1235/s')
    expect(formatPerSec(null, 1)).toBe(UNAVAILABLE_TEXT) // 不是「不可用/s」
    expect(formatPerSec(Number.NaN, 1)).toBe(UNAVAILABLE_TEXT)
  })

  it('计数取整并带千分位', () => {
    expect(formatCount(12_000_000)).toBe('12,000,000')
    expect(formatCount(0)).toBe('0')
    expect(formatCount(999)).toBe('999') // 不足千位不加分隔符
    expect(formatCount(1234.6)).toBe('1,235') // 先取整再分组
    expect(formatCount(null)).toBe(UNAVAILABLE_TEXT)
  })
})

describe('formatNumberAuto：图表通用档（整数原样、非整数 2 位）', () => {
  it('按值本身取形态，如实体现实测值', () => {
    expect(formatNumberAuto(12)).toBe('12')
    expect(formatNumberAuto(0.42)).toBe('0.42')
    expect(formatNumberAuto(0)).toBe('0')
    expect(formatNumberAuto(1234.5678)).toBe('1234.57')
    expect(formatNumberAuto(null)).toBe(UNAVAILABLE_TEXT)
  })
})

describe('formatDuration：时长（秒 → 可读时长；#142 从历史视图迁入）', () => {
  it('不足 1 分钟按秒显示（sparkline 实际覆盖几十秒时不冒充整分钟）', () => {
    expect(formatDuration(0)).toBe('0 秒')
    expect(formatDuration(45)).toBe('45 秒')
    expect(formatDuration(59)).toBe('59 秒')
  })

  it('分钟向下取整（不足整窗不得四舍五入成 15 分钟）；小时 / 天保留 1 位小数', () => {
    expect(formatDuration(60)).toBe('1 分钟')
    expect(formatDuration(90)).toBe('1 分钟')  // 1 分 30 秒按分钟向下取整
    expect(formatDuration(870)).toBe('14 分钟')  // 14 分 30 秒 → 14 分钟（不冒充 15）
    expect(formatDuration(899)).toBe('14 分钟')  // 14 分 59 秒 → 14 分钟（不冒充 15）
    expect(formatDuration(15 * 60)).toBe('15 分钟')
    expect(formatDuration(3600)).toBe('1.0 小时')
    expect(formatDuration(6 * 3600)).toBe('6.0 小时')
    expect(formatDuration(86400)).toBe('1.0 天')
  })

  it('非法值（null / 非有限 / 负数）返回不可用占位，不显示负时长', () => {
    expect(formatDuration(null)).toBe(UNAVAILABLE_TEXT)
    expect(formatDuration(Number.NaN)).toBe(UNAVAILABLE_TEXT)
    expect(formatDuration(-1)).toBe(UNAVAILABLE_TEXT)
  })
})
