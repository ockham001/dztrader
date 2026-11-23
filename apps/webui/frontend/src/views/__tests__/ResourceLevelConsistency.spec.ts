import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest'
import { enableAutoUnmount, mount, flushPromises } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import Topbar from '@/components/layout/Topbar.vue'
import MonitorView from '../MonitorView.vue'
import { sysmonApi } from '@/api/sysmon'
import { makeSysmonSnapshot } from '@/test-utils/sysmonSnapshot'

vi.mock('@/api/sysmon', () => ({
  sysmonApi: { getSnapshot: vi.fn(), getHistory: vi.fn(), setConfig: vi.fn(), getConfig: vi.fn(), probeClock: vi.fn() },
}))

vi.mock('@/composables/wsClient', () => ({
  useWebSocket: () => ({ connectionState: { value: 'connected' } }),
}))

// #452 / ADR 0054：顶栏资源区只剩 CPU / 内存两枚徽章（卷徽章已随本票删除），它们与监控页
// CPU / 内存数值共用 `useResourceLevel` 的同一套阈值与等级词表。本 spec 挂同一 pinia 的两侧，
// 逐档比较 data-level：若将来一侧单独改阈值或映射，这里会失败（同源由共享模块保证）。
//
// 历史：#127 / ADR 0048 的顶栏卷徽章一致性用例随卷徽章删除而废止（#452），改为比较仍存在的
// CPU / 内存两处消费方。
const stubs = {
  Icon: { template: '<span class="icon" />' },
  ThemeDropdown: { template: '<div class="theme-dropdown-stub" />' },
}

const GIB = 1024 ** 3

/** 内存对象：按「已用 / 总量」精确给出比率（store 按唯一口径 used = total − available 重算） */
function memoryWithRatio(usedGiB: number, totalGiB: number) {
  return {
    total_bytes: totalGiB * GIB,
    available_bytes: (totalGiB - usedGiB) * GIB,
    used_bytes: usedGiB * GIB,
    used_ratio: usedGiB / totalGiB,
    cached_bytes: null,
    buffers_bytes: null,
  }
}

/**
 * 同时挂载顶栏与监控页：两者共用同一个 pinia / sysmon store 实例，
 * 因此比较的是「同一快照、同一指标」在两个组件里给出的展示结论。
 */
async function mountBoth() {
  const topbar = mount(Topbar, { global: { stubs } })
  await flushPromises()
  const monitor = mount(MonitorView, { global: { stubs } })
  await flushPromises()
  return { topbar, monitor }
}

// #371：MonitorView 挂了 1 秒 ticker，用例必须卸载 —— 不卸载时未清理的定时器会在
// 后续用例的 flush 里插入无主更新，实测会让无关用例偶发假红（顶栏墙钟同样有 interval）。
enableAutoUnmount(afterEach)

beforeEach(() => {
  setActivePinia(createPinia())
  vi.clearAllMocks()
})

afterEach(() => {
  vi.unstubAllGlobals()
})

describe('顶栏 CPU / 内存徽章 / 监控页 CPU / 内存数值色标一致性（共用 useResourceLevel）', () => {
  it('同一快照的同一指标在两侧得到同一 data-level（<70% normal、恰好 70% warn、恰好 90% danger）', async () => {
    const cases = [
      { ratio: 0.4, expected: 'normal' },
      { ratio: 0.7, expected: 'warn' },
      { ratio: 0.9, expected: 'danger' },
    ] as const
    for (const { ratio, expected } of cases) {
      // 每轮重建 pinia：store 有 `loaded` 短路，复用实例会让后续轮次看不到新快照
      setActivePinia(createPinia())
      vi.clearAllMocks()
      vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({
        cpuUsageRatio: ratio,
        memory: memoryWithRatio(ratio * 100, 100),
      }))
      const { topbar, monitor } = await mountBoth()

      const topbarCpu = topbar.find('#resCpu').attributes('data-level')
      const monitorCpu = monitor.find('[data-test="monitor-cpu-value"]').attributes('data-level')
      expect(topbarCpu).toBe(monitorCpu)
      expect(topbarCpu).toBe(expected)

      const topbarMem = topbar.find('#resMem').attributes('data-level')
      const monitorMem = monitor.find('[data-test="monitor-memory-percent"]').attributes('data-level')
      expect(topbarMem).toBe(monitorMem)
      expect(topbarMem).toBe(expected)
    }
  })

  it('边界外侧同样一致：0.6999 → normal、0.8999 → warn（整数字节保证比率精确落在边界外）', async () => {
    const cases = [
      { usedGiB: 6_999, totalGiB: 10_000, expected: 'normal' },
      { usedGiB: 8_999, totalGiB: 10_000, expected: 'warn' },
    ] as const
    for (const { usedGiB, totalGiB, expected } of cases) {
      setActivePinia(createPinia())
      vi.clearAllMocks()
      const ratio = usedGiB / totalGiB
      vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({
        cpuUsageRatio: ratio,
        memory: memoryWithRatio(usedGiB, totalGiB),
      }))
      const { topbar, monitor } = await mountBoth()

      expect(topbar.find('#resCpu').attributes('data-level'))
        .toBe(monitor.find('[data-test="monitor-cpu-value"]').attributes('data-level'))
      expect(topbar.find('#resCpu').attributes('data-level')).toBe(expected)
      expect(topbar.find('#resMem').attributes('data-level'))
        .toBe(monitor.find('[data-test="monitor-memory-percent"]').attributes('data-level'))
      expect(topbar.find('#resMem').attributes('data-level')).toBe(expected)
    }
  })

  it('不可用不得用 0 冒充：CPU / 内存两侧都不套状态色（data-level 缺失）', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({
      metrics: { 'cpu.total': { available: false, reason: 'collect_failed' }, memory: { available: false, reason: 'collect_failed' } },
    }))
    const { topbar, monitor } = await mountBoth()

    // 顶栏：中性 unavailable 类，data-level 不写
    expect(topbar.find('#resCpu').classes()).toContain('topbar__resource--unavailable')
    expect(topbar.find('#resCpu').attributes('data-level')).toBeUndefined()
    expect(topbar.find('#resMem').attributes('data-level')).toBeUndefined()
    // 监控页：同指标进入不可用空态（不渲染带 data-level 的数值按钮）
    expect(monitor.find('[data-test="monitor-cpu-value"]').exists()).toBe(false)
    expect(monitor.find('[data-test="monitor-memory-percent"]').exists()).toBe(false)
  })
})
