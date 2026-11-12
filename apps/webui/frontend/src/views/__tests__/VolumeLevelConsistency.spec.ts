import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest'
import { mount, flushPromises, type VueWrapper } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import Topbar from '@/components/layout/Topbar.vue'
import MonitorView from '../MonitorView.vue'
import { sysmonApi } from '@/api/sysmon'
import { makeSysmonSnapshot } from '@/test-utils/sysmonSnapshot'

vi.mock('@/api/sysmon', () => ({
  sysmonApi: { getSnapshot: vi.fn(), getHistory: vi.fn(), setConfig: vi.fn(), getConfig: vi.fn() },
}))

vi.mock('@/composables/wsClient', () => ({
  useWebSocket: () => ({ connectionState: { value: 'connected' } }),
}))

// #127 / ADR 0048：Topbar 经 useBreakpoint 读视口档位做行为分支（≥lg 显示卷徽章组）。
// jsdom 无 matchMedia / ResizeObserver，用固定宽屏（1600 = xl）+ 空观察者 stub，让顶栏按宽屏形态渲染。
// 宽度未测量（clientWidth = 0）时折叠算法按「全部可见」处理，因此这里断言的是全量卷集合。
beforeEach(() => {
  vi.stubGlobal('ResizeObserver', class {
    observe(): void {}
    disconnect(): void {}
  })
  vi.stubGlobal('matchMedia', (q: string) => {
    const m = q.match(/\(min-width:\s*([\d.]+)px\)/)
    return {
      media: q,
      matches: m ? 1600 >= Number.parseFloat(m[1]) : false,
      addEventListener: () => {},
      removeEventListener: () => {},
    }
  })
})

afterEach(() => {
  vi.unstubAllGlobals()
})

const stubs = {
  Icon: { template: '<span class="icon" />' },
  ThemeDropdown: { template: '<div class="theme-dropdown-stub" />' },
}

const GIB = 1024 ** 3

/**
 * 同时挂载顶栏与监控页：两者共用同一个 pinia / sysmon store 实例，
 * 因此比较的是「同一快照、同一卷」在两个组件里给出的展示结论。
 */
async function mountBoth() {
  const topbar = mount(Topbar, { global: { stubs } })
  await flushPromises()
  const monitor = mount(MonitorView, { global: { stubs } })
  await flushPromises()
  return { topbar, monitor }
}

/**
 * 挂载点 → 色标等级（data-level）映射，两侧用同一把尺子比较。
 *
 * 排除顶栏的**隐形测量行**（ADR 0048）：它渲染同一份卷徽章只为量宽度，挂在
 * `[data-test="topbar-resource-measure"]` 下，不属于可见结论。
 */
function levelMap(wrapper: VueWrapper, selector: string): Map<string | undefined, string | undefined> {
  const visibleMatches = wrapper.findAll(selector)
    .filter(el => !el.element.closest('[data-test="topbar-resource-measure"]'))
  return new Map(visibleMatches.map(el => [el.attributes('data-mount'), el.attributes('data-level')]))
}

describe('顶栏卷徽章 / 监控页卷列表色标一致性（共用 useResourceLevel）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    vi.clearAllMocks()
  })

  it('同一快照的同一卷在两侧得到同一 data-level（<70% normal、恰好 70% warn、恰好 90% danger）', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({
      volumes: [
        { mount: '/low', available: true, total_bytes: 100 * GIB, used_bytes: 40 * GIB, used_ratio: 0.4 },
        { mount: '/warn', available: true, total_bytes: 100 * GIB, used_bytes: 70 * GIB, used_ratio: 0.7 },
        { mount: '/danger', available: true, total_bytes: 100 * GIB, used_bytes: 90 * GIB, used_ratio: 0.9 },
      ],
    }))
    const { topbar, monitor } = await mountBoth()

    const topbarLevels = levelMap(topbar, '.topbar__resource--disk')
    const monitorLevels = levelMap(monitor, '[data-test="monitor-disk-volume"]')

    // 两侧都对同一组卷给出结论（卷集合由快照驱动）
    expect([...topbarLevels.keys()].sort()).toEqual(['/danger', '/low', '/warn'])
    expect([...monitorLevels.keys()].sort()).toEqual(['/danger', '/low', '/warn'])

    // 核心断言：同一卷两侧 data-level 相等 —— 行为层验证两处结论一致（若将来一侧
    // 单独改阈值或映射，这里会失败；两侧同源由共享模块 useResourceLevel 保证）
    for (const mountPoint of ['/low', '/warn', '/danger']) {
      expect(topbarLevels.get(mountPoint)).toBe(monitorLevels.get(mountPoint))
    }
    expect(topbarLevels.get('/low')).toBe('normal')
    expect(topbarLevels.get('/warn')).toBe('warn')
    expect(topbarLevels.get('/danger')).toBe('danger')
  })

  it('边界外侧同样一致：0.6999 → normal、0.8999 → warn', async () => {
    // 整数字节 + 整数总量保证 store 重算出的比率精确落在边界外侧
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({
      volumes: [
        { mount: '/below-warn', available: true, total_bytes: 10_000 * GIB, used_bytes: 6_999 * GIB, used_ratio: 0.6999 },
        { mount: '/below-danger', available: true, total_bytes: 10_000 * GIB, used_bytes: 8_999 * GIB, used_ratio: 0.8999 },
      ],
    }))
    const { topbar, monitor } = await mountBoth()

    const topbarLevels = levelMap(topbar, '.topbar__resource--disk')
    const monitorLevels = levelMap(monitor, '[data-test="monitor-disk-volume"]')

    for (const mountPoint of ['/below-warn', '/below-danger']) {
      expect(topbarLevels.get(mountPoint)).toBe(monitorLevels.get(mountPoint))
    }
    expect(topbarLevels.get('/below-warn')).toBe('normal')
    expect(topbarLevels.get('/below-danger')).toBe('warn')
  })
})
