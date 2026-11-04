import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest'
import { mount, flushPromises } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import StatsBar from '../StatsBar.vue'
import { __resetTdStatsSchedulerForTests, useTdStatsStore } from '@/stores/tdStats'
import { tdApi } from '@/api/td'
import type { TdStatsValues } from '@/types/api'

vi.mock('@/api/td', () => ({
  tdApi: { getStats: vi.fn() },
}))

function values(overrides: Partial<TdStatsValues> = {}): TdStatsValues {
  return {
    total_orders: 1240,
    total_volume: 2480,
    unfilled_orders: 12,
    unfilled_volume: 21,
    ...overrides,
  }
}

/** 统计条上的两个数字（未成交 / 总）；VTU text() 会折叠元素间空白，故按节点取值断言 */
function numberTexts(wrapper: ReturnType<typeof mount>): string[] {
  return wrapper.findAll('.td-stats-bar__num').map(node => node.text())
}

function statsResponse(overrides: Record<string, unknown> = {}) {
  return {
    available: true as const,
    trading_day: '20260808',
    online_accounts: ['acc_a'],
    stats: values(),
    ...overrides,
  }
}

describe('StatsBar（#247 契约 rest §2.9.5 / webui-ws §5 第 10 条）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    __resetTdStatsSchedulerForTests()
    vi.useFakeTimers()
    vi.resetAllMocks()
  })
  afterEach(() => {
    vi.useRealTimers()
    __resetTdStatsSchedulerForTests()
  })

  it('挂载时拉统计并渲染「未成交 / 总」与单位（笔数）', async () => {
    vi.mocked(tdApi.getStats).mockResolvedValue(statsResponse())
    const wrapper = mount(StatsBar)
    await flushPromises()

    expect(tdApi.getStats).toHaveBeenCalledTimes(1)
    const valuesEl = wrapper.find('[data-test="td-stats-values"]')
    expect(valuesEl.exists()).toBe(true)
    expect(numberTexts(wrapper)).toEqual(['12', '1240'])
    expect(valuesEl.text()).toContain('未成交')
    expect(valuesEl.text()).toContain('总')
    expect(wrapper.find('.td-stats-bar__unit').text()).toBe('笔')
    expect(wrapper.find('[data-test="td-stats-degraded"]').exists()).toBe(false)
    wrapper.unmount()
  })

  it('单位切换：点按钮从笔数切到手数，再点回笔数（纯展示，不重拉）', async () => {
    vi.mocked(tdApi.getStats).mockResolvedValue(statsResponse())
    const wrapper = mount(StatsBar)
    await flushPromises()

    const toggle = wrapper.find('[data-test="td-stats-unit-toggle"]')
    expect(toggle.text()).toBe('按手数')

    await toggle.trigger('click')
    expect(numberTexts(wrapper)).toEqual(['21', '2480'])
    expect(wrapper.find('.td-stats-bar__unit').text()).toBe('手')
    expect(toggle.text()).toBe('按笔数')

    await toggle.trigger('click')
    expect(numberTexts(wrapper)).toEqual(['12', '1240'])
    expect(wrapper.find('.td-stats-bar__unit').text()).toBe('笔')
    expect(tdApi.getStats).toHaveBeenCalledTimes(1)
    wrapper.unmount()
  })

  it('降级：显示统一降级文案且不出现 0（不拿 0 冒充数据）', async () => {
    vi.mocked(tdApi.getStats).mockResolvedValue({ available: false, reason: 'not_ready' })
    const wrapper = mount(StatsBar)
    await flushPromises()

    const degraded = wrapper.find('[data-test="td-stats-degraded"]')
    expect(degraded.exists()).toBe(true)
    expect(degraded.text()).toBe('统计暂不可用，正在自动重试')
    // 降级不能渲染数值节点（尤其不能渲染 0 冒充数据）：整条只剩降级文案
    expect(wrapper.find('[data-test="td-stats-values"]').exists()).toBe(false)
    expect(wrapper.find('[data-test="td-stats-pending"]').exists()).toBe(false)
    expect(wrapper.text()).toBe('统计暂不可用，正在自动重试')
    wrapper.unmount()
  })

  it('首拉未返回时显示占位 —（不显示 0）', async () => {
    vi.mocked(tdApi.getStats).mockReturnValue(new Promise(() => {}) as never)
    const wrapper = mount(StatsBar)
    await flushPromises()

    expect(wrapper.find('[data-test="td-stats-pending"]').text()).toBe('—')
    expect(wrapper.find('[data-test="td-stats-values"]').exists()).toBe(false)
    // 占位态不得出现任何数字（0 也不能用来冒充数据）
    expect(wrapper.find('[data-test="td-stats-pending"]').text()).not.toMatch(/\d/)
    wrapper.unmount()
  })

  it('无在线账户（trading_day:null）显示「无在线交易账户」，不显示数值', async () => {
    vi.mocked(tdApi.getStats).mockResolvedValue(statsResponse({
      trading_day: null,
      online_accounts: [],
      stats: values({ total_orders: 0, total_volume: 0, unfilled_orders: 0, unfilled_volume: 0 }),
    }))
    const wrapper = mount(StatsBar)
    await flushPromises()

    expect(wrapper.text()).toContain('无在线交易账户')
    expect(wrapper.find('[data-test="td-stats-values"]').exists()).toBe(false)
    wrapper.unmount()
  })

  it('卸载时停轮询（dispose 生效），不再发起请求', async () => {
    vi.mocked(tdApi.getStats).mockResolvedValue(statsResponse())
    const wrapper = mount(StatsBar)
    await flushPromises()
    expect(tdApi.getStats).toHaveBeenCalledTimes(1)

    wrapper.unmount()
    await vi.advanceTimersByTimeAsync(5_000)

    expect(tdApi.getStats).toHaveBeenCalledTimes(1)
  })

  it('统计条随轮询更新为新值', async () => {
    vi.mocked(tdApi.getStats).mockResolvedValue(statsResponse())
    const wrapper = mount(StatsBar)
    await flushPromises()

    vi.mocked(tdApi.getStats).mockResolvedValue(statsResponse({
      stats: values({ total_orders: 1241, total_volume: 2483, unfilled_orders: 11, unfilled_volume: 19 }),
    }))
    await vi.advanceTimersByTimeAsync(1_000)
    await flushPromises()

    expect(numberTexts(wrapper)).toEqual(['11', '1241'])
    wrapper.unmount()
  })

  it('store 单例共享：视图内挂载的统计条与组件读到同一份状态', async () => {
    vi.mocked(tdApi.getStats).mockResolvedValue(statsResponse())
    const wrapper = mount(StatsBar)
    await flushPromises()

    expect(useTdStatsStore().totalText).toBe('1240')
    expect(wrapper.text()).toContain('1240')
    wrapper.unmount()
  })
})
