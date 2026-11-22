import { describe, it, expect } from 'vitest'
import {
  UNAVAILABLE_TEXT,
  formatByteRate,
  formatBytes,
  formatClockHeadroom,
  formatClockThreshold,
  formatClockTime,
  formatDateTime,
  formatCount,
  formatCountPerSec,
  formatDecimal,
  formatDuration,
  formatFrequency,
  formatFrequencyParts,
  formatIops,
  formatLatency,
  formatAge,
  formatCompactSignedMilliseconds,
  formatMilliseconds,
  formatNumberAuto,
  formatPercent,
  formatPerSec,
  formatPlatformName,
  formatSignedMilliseconds,
  formatTemperature,
  formatUptime,
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

describe('formatMilliseconds / formatSignedMilliseconds：时钟偏差与误差界（#302）', () => {
  it('毫秒固定 1 位 + ms；0 是合法真实值（真实测到 0 偏差）', () => {
    expect(formatMilliseconds(12.34)).toBe('12.3 ms')
    expect(formatMilliseconds(12.36)).toBe('12.4 ms')
    expect(formatMilliseconds(0)).toBe('0.0 ms')
    expect(formatMilliseconds(-12.3)).toBe('-12.3 ms')  // 符号由带符号格式化器负责，本函数不吞负号
    expect(formatMilliseconds(null)).toBe(UNAVAILABLE_TEXT)
    expect(formatMilliseconds(Number.NaN)).toBe(UNAVAILABLE_TEXT)
    expect(formatMilliseconds(Number.POSITIVE_INFINITY)).toBe(UNAVAILABLE_TEXT)
  })

  it('偏差带符号：正 = 本地快加 +，负保留 -，恰好 0 不加符号', () => {
    expect(formatSignedMilliseconds(12.34)).toBe('+12.3 ms')
    expect(formatSignedMilliseconds(-12.34)).toBe('-12.3 ms')
    expect(formatSignedMilliseconds(0)).toBe('0.0 ms')
    expect(formatSignedMilliseconds(null)).toBe(UNAVAILABLE_TEXT)
    expect(formatSignedMilliseconds(Number.NEGATIVE_INFINITY)).toBe(UNAVAILABLE_TEXT)
  })

  it('紧凑带符号毫秒（顶栏 NTP 徽章，ADR 0048）：取整 + 无空格单位，符号跟取整后的值走', () => {
    expect(formatCompactSignedMilliseconds(12.34)).toBe('+12ms')
    expect(formatCompactSignedMilliseconds(12.5)).toBe('+13ms')  // Math.round（.5 向上）
    expect(formatCompactSignedMilliseconds(-12.6)).toBe('-13ms')
    expect(formatCompactSignedMilliseconds(0)).toBe('0ms')
    // 恰好 ±0.4 取整后是 0：不得显示成「-0ms」这种负零
    expect(formatCompactSignedMilliseconds(-0.4)).toBe('0ms')
    expect(formatCompactSignedMilliseconds(0.4)).toBe('0ms')
    expect(formatCompactSignedMilliseconds(null)).toBe(UNAVAILABLE_TEXT)
    expect(formatCompactSignedMilliseconds(Number.NaN)).toBe(UNAVAILABLE_TEXT)
  })
})

describe('formatClockThreshold：两档阈值按单位折算（#382）', () => {
  it('默认值逐字符不变（最常见的配置，也是 88px 列宽的唯一依据）', () => {
    expect(formatClockThreshold(100, 1000)).toBe('±100 / ±1000 ms')
    // 契约默认值来自 sysmon §3.1，前端不得自行改写
    expect(formatClockThreshold(50, 500)).toBe('±50 / ±500 ms')
  })

  it('毫秒形态装得下就留原样——判据是文本长度，不是 error 的位数', () => {
    // 小值：
    expect(formatClockThreshold(1, 2)).toBe('±1 / ±2 ms')
    expect(formatClockThreshold(999, 1000)).toBe('±999 / ±1000 ms')
    expect(formatClockThreshold(1000 - 1, 1000)).toBe('±999 / ±1000 ms')
    // **这批是审查抓到的回归**：它们全是 15 字符、84.86px，88px 列装得下，
    // 第一版按 `error <= 1000` 把它们推去秒档（`±0.2 / ±2 s`），等于在用户没要求的情况下
    // 改了他读到的东西。仓内后端测试自用的 `500/2000`、`250/3000`、`300/1200` 全在其中。
    expect(formatClockThreshold(200, 2000)).toBe('±200 / ±2000 ms')
    expect(formatClockThreshold(500, 2000)).toBe('±500 / ±2000 ms')
    expect(formatClockThreshold(250, 3000)).toBe('±250 / ±3000 ms')
    expect(formatClockThreshold(300, 1200)).toBe('±300 / ±1200 ms')
    // 极端不对称也装得下：`±1 / ±600000 ms` 恰好 15 字符
    expect(formatClockThreshold(1, 600_000)).toBe('±1 / ±600000 ms')
    // 而 16 字符（91.16px）真的装不下 → 必须折算
    expect(formatClockThreshold(1600, 2000)).toBe('±1.6 / ±2 s')
    expect(formatClockThreshold(9999, 9999)).toBe('±9.99 / ±10 s')
  })

  it('长度边界钉在 15：装得下的最长形态与装不下的最短形态各一例', () => {
    // 15 字符 = 84.86px（实测，11px tabular-nums）→ 留毫秒
    expect('±999 / ±9999 ms').toHaveLength(15)
    expect(formatClockThreshold(999, 9999)).toBe('±999 / ±9999 ms')
    // 16 字符 = 91.16px → 折算。这一对是「边界在哪」的唯一直接证据，
    // 改列宽或改字号时它会先红，逼着重新量。
    expect('±9999 / ±9999 ms').toHaveLength(16)
    expect(formatClockThreshold(9999, 9999)).not.toContain(' ms')
  })

  it('两档都是整分钟时用 min（契约上限即此形）', () => {
    expect(formatClockThreshold(60_000, 600_000)).toBe('±1 / ±10 min')
    expect(formatClockThreshold(60_000, 60_000 * 3)).toBe('±1 / ±3 min')
  })

  it('装不下的走秒、3 位有效数字（warn 向下、error 向上）', () => {
    expect(formatClockThreshold(1000, 10_000)).toBe('±1 / ±10 s')
    expect(formatClockThreshold(1500, 6000)).toBe('±1.5 / ±6 s')
    expect(formatClockThreshold(1000, 1500)).toBe('±1 / ±1.5 s')
    expect(formatClockThreshold(1600, 2000)).toBe('±1.6 / ±2 s')
    expect(formatClockThreshold(59_999, 599_999)).toBe('±59.9 / ±600 s')
    // 亚秒形态：`±10 / ±100000 ms` 是 16 字符（装不下）→ 走秒档，warn 侧落到毫秒量级
    expect('±10 / ±100000 ms').toHaveLength(16)
    expect(formatClockThreshold(10, 100_000)).toBe('±0.01 / ±100 s')
  })

  it('两档在显示上永不相等（否则「两档」这个事实消失）', () => {
    // 59999 / 60000 换秒后是 59.999 / 60：warn 向下、error 向上才不会塌成「±59.9 / ±59.9 s」
    expect(formatClockThreshold(59_999, 60_000)).toBe('±59.9 / ±60 s')
    expect(formatClockThreshold(59_999, 60_000)).not.toBe('±60 / ±60 s')
  })

  it('舍入方向写反/写漏也会塌陷：取「同一取整台阶内」的一对值', () => {
    // 审查 finding：原来那四组（999/1001、1999/2001、9999/10001、14999/15001）只能抓
    // 「两侧都 nearest」——它们跨了取整台阶，两侧都 floor 时也不会撞。真正区分方向的是
    // **落在同一台阶内**的一对值：两侧都用 floor 时它们映到同一个显示值。
    // 下面每组的左值都在秒档（长度 16+ 才走到这里），且 `floor(left) == floor(right)`。
    const sameStepPairs: Array<[number, number, string]> = [
      [14_500, 14_550, '±14.5 / ±14.6 s'],   // 都要 2 位小数；两侧 floor 会得 ±14.5 / ±14.5
      [10_100, 10_140, '±10.1 / ±10.2 s'],   // 同台阶的两位小数对
      [25_000, 25_040, '±25 / ±25.1 s'],     // 2 位有效数字档：两侧 floor 都会得 ±25
      [14_700, 14_760, '±14.7 / ±14.8 s'],   // 1 位小数档内的近邻对
    ]
    for (const [warn, error, expected] of sameStepPairs) {
      // 先确认这两值确实落在同一取整台阶（否则这组样本没有鉴别力）
      const text = formatClockThreshold(warn, error)
      const [left, right] = text.replace(/^±/, '').replace(/ s$/, '').split(' / ±')
      expect(left, `${warn}/${error} → ${text}：两侧塌成同一个数`).not.toBe(right)
      expect(text, `${warn}/${error} 的折算文案变了`).toBe(expected)
    }
  })

  it('两档在显示上永不相等（全契约空间抽样，含跨台阶的一对）', () => {
    // 跨台阶的一对：两侧都 nearest 时不会撞，所以它们抓的是「nearest 那种实现」
    for (const [warn, error] of [[999, 1001], [1999, 2001], [9999, 10_001], [14_999, 15_001]]) {
      const text = formatClockThreshold(warn, error)
      const [left, right] = text.replace(/^±/, '').replace(/ s$/, '').split(' / ±')
      expect(left, `${warn}/${error} → ${text}`).not.toBe(right)
    }
  })

  it('非有限值退化为不可用占位（不编造阈值）', () => {
    expect(formatClockThreshold(Number.NaN, 1000)).toBe(UNAVAILABLE_TEXT)
    expect(formatClockThreshold(100, Number.POSITIVE_INFINITY)).toBe(UNAVAILABLE_TEXT)
  })
})

describe('formatClockHeadroom：余量按单位折算（#382）', () => {
  it('1 s 以下保留 1 位小数毫秒（既有文案不变）', () => {
    expect(formatClockHeadroom(987.5)).toBe('987.5 ms')
    expect(formatClockHeadroom(0)).toBe('0.0 ms')
    expect(formatClockHeadroom(999.9)).toBe('999.9 ms')
    expect(formatClockHeadroom(12.34)).toBe('12.3 ms')
  })

  it('取整先于换档：999.95 ms 不被写成「1000.0 ms」', () => {
    expect(formatClockHeadroom(999.95)).toBe('1 s')
    expect(formatClockHeadroom(999.94)).toBe('999.9 ms')
  })

  it('1 s 及以上换秒、3 位有效数字（契约上限 600 s 不再需要 6 位毫秒）', () => {
    expect(formatClockHeadroom(1000)).toBe('1 s')
    expect(formatClockHeadroom(3600)).toBe('3.6 s')
    expect(formatClockHeadroom(59_999)).toBe('60 s')
    expect(formatClockHeadroom(600_000)).toBe('600 s')
    // 6 位毫秒在 92px 列里放不下（实测 65.4px + 「已超 」前缀 96.6px）
    expect(formatClockHeadroom(600_000)).not.toContain('ms')
  })

  it('负值保留负号（越线由调用方加前缀，纯格式化不吞符号）', () => {
    expect(formatClockHeadroom(-12.5)).toBe('-12.5 ms')
    expect(formatClockHeadroom(-3600)).toBe('-3.6 s')
    // 负零天然不会漏出来（`(-0).toFixed(1)` 就是 `"0.0"`），所以这里**没有**专门的守卫分支。
    // 这几条锁的是「行为正确」，不是「守卫存在」——审查指出原版那条断言无法区分两者，
    // 而守卫本身不可观测，已删除（留注释说明，不留死代码）。
    expect(formatClockHeadroom(-0.04)).toBe('0.0 ms')
    expect(formatClockHeadroom(-0)).toBe('0.0 ms')
    expect(-0.04 * 10).toBeLessThan(0)          // 前提：取整前确实是负数
    expect((-0).toFixed(1)).toBe('0.0')          // 前提：toFixed 自己就处理了负零
  })

  it('非有限值退化为不可用占位', () => {
    expect(formatClockHeadroom(Number.NaN)).toBe(UNAVAILABLE_TEXT)
    expect(formatClockHeadroom(Number.NEGATIVE_INFINITY)).toBe(UNAVAILABLE_TEXT)
  })
})

describe('formatAge：「多久以前」口径（页文件年龄 / 时钟上次探测共用）', () => {
  it('整档向下取整：<60 秒按秒、<1 小时按分钟、<1 天按小时、其余按天', () => {
    expect(formatAge(0)).toBe('0 秒')
    expect(formatAge(30)).toBe('30 秒')
    expect(formatAge(59.9)).toBe('59 秒')
    expect(formatAge(60)).toBe('1 分钟')
    expect(formatAge(899)).toBe('14 分钟')  // 14 分 59 秒不得说成 15 分钟
    expect(formatAge(3_600)).toBe('1 小时')
    expect(formatAge(86_399)).toBe('23 小时')
    expect(formatAge(86_400)).toBe('1 天')
    expect(formatAge(90_000)).toBe('1 天')
  })

  it('非法值返回不可用占位（负时长没有意义）', () => {
    expect(formatAge(-1)).toBe(UNAVAILABLE_TEXT)
    expect(formatAge(null)).toBe(UNAVAILABLE_TEXT)
    expect(formatAge(Number.NaN)).toBe(UNAVAILABLE_TEXT)
  })
})

describe('formatTemperature / formatFrequency：温度与频率', () => {
  it('温度固定 1 位 + °C', () => {
    expect(formatTemperature(45.5)).toBe('45.5 °C')
    expect(formatTemperature(44)).toBe('44.0 °C')
    expect(formatTemperature(27.75)).toBe('27.8 °C') // 正常四舍五入
    expect(formatTemperature(null)).toBe(UNAVAILABLE_TEXT)
  })

  it('频率取整 + MHz（<1000 MHz）', () => {
    expect(formatFrequency(962)).toBe('962 MHz')
    expect(formatFrequency(962.4)).toBe('962 MHz')
    expect(formatFrequency(0)).toBe('0 MHz')
  })

  it('频率 ≥1000 MHz 换 GHz 两位小数（与主机面板同口径）', () => {
    expect(formatFrequency(3600)).toBe('3.60 GHz')
    expect(formatFrequency(3798)).toBe('3.80 GHz')
    expect(formatFrequency(4350)).toBe('4.35 GHz')
    expect(formatFrequency(5100)).toBe('5.10 GHz')
    expect(formatFrequency(null)).toBe(UNAVAILABLE_TEXT)
  })

  it('formatFrequencyParts 拆出数值与单位（主值大小两段排版用）', () => {
    expect(formatFrequencyParts(3798)).toEqual({ value: '3.80', unit: 'GHz' })
    expect(formatFrequencyParts(962)).toEqual({ value: '962', unit: 'MHz' })
    expect(formatFrequencyParts(null)).toEqual({ value: UNAVAILABLE_TEXT, unit: '' })
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

// #430：主机条的三个展示格式化。三者都是纯函数，边界（无后缀 / 跨日 / 不足 1 分钟）最先崩。
describe('formatPlatformName：platform_description 剥掉末尾 (kernel …) / (build …)', () => {
  it('Linux：剥后缀得到发行版名', () => {
    expect(formatPlatformName('Ubuntu 24.04.1 LTS (kernel 6.8.0-45-generic)')).toBe('Ubuntu 24.04.1 LTS')
  })

  it('Windows：剥 build 后缀（build 号由内核字段单独承担，不重复）', () => {
    expect(formatPlatformName('Windows 10 Pro (build 19045)')).toBe('Windows 10 Pro')
  })

  it('没有括号后缀时原样返回；空白与 null 返回空串（由调用方决定占位）', () => {
    expect(formatPlatformName('Debian GNU/Linux 12')).toBe('Debian GNU/Linux 12')
    expect(formatPlatformName('')).toBe('')
    expect(formatPlatformName('   ')).toBe('')
    expect(formatPlatformName(null)).toBe('')
    expect(formatPlatformName(undefined)).toBe('')
  })

  it('只剥末尾那一段：发行版名自带的其它括号段不动', () => {
    expect(formatPlatformName('Fedora Linux 40 (Workstation Edition) (kernel 6.9.4)'))
      .toBe('Fedora Linux 40 (Workstation Edition)')
  })
})

describe('formatDateTime：历史 / 配置副标题的完整日期时间', () => {
  const at = (year: number, month: number, day: number, hour: number, minute: number, second: number): number =>
    new Date(year, month - 1, day, hour, minute, second).getTime()

  it('本地日期 + 时分秒（日期始终可见，与主机条的紧凑时刻不同）', () => {
    expect(formatDateTime(at(2026, 10, 21, 15, 0, 0))).toBe('2026-10-21 15:00:00')
  })

  it('非有限值 / 非法时间戳 → 不可用', () => {
    expect(formatDateTime(null)).toBe(UNAVAILABLE_TEXT)
    expect(formatDateTime(Number.NaN)).toBe(UNAVAILABLE_TEXT)
  })
})

describe('formatUptime：主机条「已运行」口径（参考图：X 天 Y 小时 / X 小时 Y 分）', () => {
  const cases: Array<[number, string]> = [
    [0, '0 秒'],
    [59, '59 秒'],
    [60, '1 分钟'],
    [3_599, '59 分钟'],
    [3_600, '1 小时 0 分'],
    [3_661, '1 小时 1 分'],
    [86_399, '23 小时 59 分'],
    [86_400, '1 天 0 小时'],
    [90_000, '1 天 1 小时'],
  ]
  for (const [seconds, expected] of cases) {
    it(`${seconds} 秒 → ${expected}`, () => {
      expect(formatUptime(seconds)).toBe(expected)
    })
  }

  it('非有限值 / 负值 → 不可用（不显示负数冒充运行时长）', () => {
    expect(formatUptime(null)).toBe(UNAVAILABLE_TEXT)
    expect(formatUptime(Number.NaN)).toBe(UNAVAILABLE_TEXT)
    expect(formatUptime(-1)).toBe(UNAVAILABLE_TEXT)
  })
})

describe('formatClockTime：状态槽的更新时刻（当日 HH:MM:SS / 跨日 MM-DD HH:MM:SS）', () => {
  // 用本地时间构造，避免用例随 CI 时区漂移（格式化按浏览器本地时区渲染）
  const at = (year: number, month: number, day: number, hour: number, minute: number, second: number): number =>
    new Date(year, month - 1, day, hour, minute, second).getTime()

  it('同一日：只给时分秒', () => {
    expect(formatClockTime(at(2026, 10, 21, 15, 0, 0), at(2026, 10, 21, 15, 0, 2))).toBe('15:00:00')
  })

  it('跨日：补 MM-DD（页面常开跨夜时不能让人误读成今天）', () => {
    expect(formatClockTime(at(2026, 10, 20, 23, 59, 0), at(2026, 10, 21, 0, 0, 5))).toBe('10-20 23:59:00')
  })

  it('跨年同样按日期判：只给 MM-DD，不给年份（状态槽是「多久以前的更新」不是日期控件）', () => {
    expect(formatClockTime(at(2025, 12, 31, 23, 59, 0), at(2026, 1, 1, 0, 0, 5))).toBe('12-31 23:59:00')
  })

  it('非有限值 / 非法时间戳 → 不可用', () => {
    expect(formatClockTime(null, at(2026, 10, 21, 15, 0, 2))).toBe(UNAVAILABLE_TEXT)
    expect(formatClockTime(at(2026, 10, 21, 15, 0, 0), Number.NaN)).toBe(UNAVAILABLE_TEXT)
    expect(formatClockTime(Number.NaN, at(2026, 10, 21, 15, 0, 2))).toBe(UNAVAILABLE_TEXT)
  })
})
