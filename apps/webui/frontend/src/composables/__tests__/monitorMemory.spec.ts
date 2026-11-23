import { describe, it, expect } from 'vitest'
import {
  MEMORY_DEFAULT_USED_PCT,
  SWAP_DEFAULT_USED_PCT,
  buildMemoryComposition,
  bytesHeadroomText,
  memoryFacts,
  memorySegmentPercent,
  resolveThresholdPct,
  resourceHeadroomText,
  swapFacts,
  swapLabel,
  swapPlatformNote,
} from '../monitorMemory'

// #445：内存卡自己的展示口径（组成带 / 数字格事实串 / 交换标签）。纯函数直接测，不挂 pinia。
const GIB = 1024 ** 3

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



describe('内存 / 交换数字格的事实串（#445 第二轮拍板 B1-B4 + B6）', () => {
  const memory = {
    total_bytes: 32 * GIB,
    available_bytes: 16 * GIB,
    used_bytes: 16 * GIB,
    used_ratio: 0.5,
    cached_bytes: 8 * GIB,
    buffers_bytes: 256 * 1024 ** 2,
  }
  const swap = { total_bytes: 8 * GIB, used_bytes: 2 * GIB, used_ratio: 0.25 }

  it('阈值默认值取契约 §3.1 的 90 / 50；配置到达后按配置', () => {
    expect(MEMORY_DEFAULT_USED_PCT).toBe(90)
    expect(SWAP_DEFAULT_USED_PCT).toBe(50)
    expect(resolveThresholdPct(undefined, MEMORY_DEFAULT_USED_PCT)).toBe(90)
    expect(resolveThresholdPct(null, SWAP_DEFAULT_USED_PCT)).toBe(50)
    expect(resolveThresholdPct(80, MEMORY_DEFAULT_USED_PCT)).toBe(80)
  })

  it('字节余量：非负写剩余、越线写「已超」，卡线不被浮点残差误判（#382 同坑）', () => {
    // 32 GiB × 90% − 16 GiB = 12.8 GiB
    expect(resourceHeadroomText(32 * GIB, 16 * GIB, 90)).toBe('12.8 GiB')
    // 阈值上恰好卡线：30 GiB × 90% = 27 GiB 已用 → 0 B，不是「已超 0 B」
    expect(resourceHeadroomText(30 * GIB, 27 * GIB, 90)).toBe('0 B')
    expect(resourceHeadroomText(32 * GIB, 30 * GIB, 90)).toBe('已超 1.2 GiB')
    expect(bytesHeadroomText(-0.2)).toBe('0 B')
  })

  it('内存事实串：已用/总量 + 可用（带口径）+ 空闲 + 阈值 + 余量', () => {
    const facts = memoryFacts(memory, 90, '7.8 GiB')
    expect(facts).toContain('已用 16.0 GiB / 共 32.0 GiB')
    expect(facts).toContain('可用 16.0 GiB（含可回收缓存/缓冲）')
    expect(facts).toContain('空闲 7.8 GiB')
    expect(facts).toContain('阈值 90%')
    expect(facts).toContain('余量 12.8 GiB')
  })

  it('组成带退化时不写「空闲」（free 段其实是 available，不能冒充空闲）', () => {
    const facts = memoryFacts(memory, 90, null)
    expect(facts).not.toContain('空闲')
    expect(facts).toContain('可用 16.0 GiB')
  })

  it('交换事实串：已用/总量 + 剩余 + 阈值 + 余量', () => {
    const facts = swapFacts(swap, 50)
    expect(facts).toContain('已用 2.0 GiB / 共 8.0 GiB')
    expect(facts).toContain('剩余 6.0 GiB')
    expect(facts).toContain('阈值 50%')
    expect(facts).toContain('余量 2.0 GiB')
  })

  it('交换剩余不为负（脏数据已用 > 总量时钳到 0）', () => {
    expect(swapFacts({ total_bytes: GIB, used_bytes: 2 * GIB, used_ratio: 1 }, 50)).toContain('剩余 0 B')
  })

  it('交换标签随平台改口（Windows 页面文件 / 其它交换分区），口径说明只给 Windows', () => {
    expect(swapLabel('windows')).toBe('页面文件')
    expect(swapLabel('linux')).toBe('交换分区')
    expect(swapLabel(null)).toBe('交换分区')
    expect(swapPlatformNote('windows')).toBe('（页面文件支撑的提交上限）')
    expect(swapPlatformNote('linux')).toBe('')
  })

  it('memorySegmentPercent：比例 → 百分比文本，不可得为 null（不补 0）', () => {
    expect(memorySegmentPercent({ id: 'used', label: '已用', bytes: 16 * GIB, ratio: 0.5, text: '16.0 GiB' })).toBe('50.0%')
    expect(memorySegmentPercent({ id: 'buffers', label: '缓冲', bytes: null, ratio: null, text: null })).toBeNull()
  })
})
