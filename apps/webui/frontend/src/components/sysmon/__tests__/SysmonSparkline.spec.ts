import { describe, expect, it } from 'vitest'
import { mount } from '@vue/test-utils'
import SysmonSparkline from '../SysmonSparkline.vue'
import {
  SPARKLINE_WINDOW_SEC,
  type SparklinePoint,
  type SparklineWindow,
} from '@/composables/sysmonSparkline'

function points(...values: number[]): SparklinePoint[] {
  return values.map((value, index) => ({ ts: 1_790_000_000 + index * 60, value }))
}

function windowOf(overrides: Partial<SparklineWindow> = {}): SparklineWindow {
  return {
    coveredSec: SPARKLINE_WINDOW_SEC,
    truncated: false,
    stale: false,
    ...overrides,
  }
}

function mountSparkline(
  values: number[],
  window: SparklineWindow = windowOf(),
  label = 'CPU 使用率',
) {
  return mount(SysmonSparkline, { props: { points: points(...values), label, window } })
}

describe('SysmonSparkline 内联趋势（#127）', () => {
  it('渲染折线：polyline 的 points 来自纯几何，末点值暴露给测试与标题', () => {
    const wrapper = mountSparkline([10, 20])

    expect(wrapper.find('polyline').attributes('points')).toBe('0,22 100,2')
    expect(wrapper.find('.sysmon-sparkline').attributes('data-last-value')).toBe('20')
    expect(wrapper.find('.sysmon-sparkline').attributes('title')).toContain('CPU 使用率')
    expect(wrapper.find('.sysmon-sparkline').attributes('title')).toContain('最近 15 分钟')
  })

  it('点击整条 sparkline 触发 open（跳转历史视图并预选指标）', async () => {
    const wrapper = mountSparkline([1, 2, 3], windowOf(), '内存使用率')

    await wrapper.find('.sysmon-sparkline').trigger('click')

    expect(wrapper.emitted('open')).toHaveLength(1)
  })

  it('少于两个点不渲染（一个点画不出趋势；历史不可用时同样静默不渲染）', () => {
    expect(mountSparkline([]).find('.sysmon-sparkline').exists()).toBe(false)
    expect(mountSparkline([0.5]).find('.sysmon-sparkline').exists()).toBe(false)
  })

  it('装饰性图形对辅助技术隐藏，但整条曲线是可聚焦的按钮', () => {
    const wrapper = mountSparkline([1, 2])

    expect(wrapper.find('button.sysmon-sparkline').exists()).toBe(true)
    expect(wrapper.find('svg').attributes('aria-hidden')).toBe('true')
    expect(wrapper.find('button.sysmon-sparkline').attributes('aria-label')).toContain('CPU 使用率')
  })
})

describe('SysmonSparkline 时间窗口径（#142：标题如实标注实际覆盖）', () => {
  it('完整覆盖 15 分钟：沿用「最近 15 分钟」文案', () => {
    const root = mountSparkline([1, 2]).find('.sysmon-sparkline')

    expect(root.attributes('title')).toBe('CPU 使用率 · 最近 15 分钟（点击查看历史）')
    expect(root.attributes('data-window-complete')).toBe('true')
    expect(root.attributes('data-window-truncated')).toBe('false')
  })

  it('历史范围内无桶（实际只有几十秒）：标题标注实际覆盖，不冒充 15 分钟', () => {
    const root = mountSparkline([1, 2], windowOf({ coveredSec: 45 }))
      .find('.sysmon-sparkline')

    expect(root.attributes('title')).toContain('最近 45 秒')
    expect(root.attributes('title')).not.toContain('最近 15 分钟')
    expect(root.attributes('aria-label')).toContain('最近 45 秒')
    expect(root.attributes('data-window-complete')).toBe('false')
  })

  it('max_points 截断：标题与历史视图同口径（实际覆盖 + 仅显示最近一段）', () => {
    const root = mountSparkline([1, 2], windowOf({ coveredSec: 300, truncated: true }))
      .find('.sysmon-sparkline')

    expect(root.attributes('title')).toContain('最近 5 分钟')
    expect(root.attributes('title')).toContain('点数超限，仅显示最近一段')
    expect(root.attributes('data-window-truncated')).toBe('true')
  })

  it('历史未刷新（单指标补拉降级）：保留曲线并标记 stale（title + 虚线类）', () => {
    const root = mountSparkline([1, 2], windowOf({ stale: true })).find('.sysmon-sparkline')

    expect(root.attributes('title')).toContain('历史未刷新')
    expect(root.attributes('aria-label')).toContain('历史未刷新')
    expect(root.attributes('data-window-stale')).toBe('true')
    expect(root.classes()).toContain('sysmon-sparkline--stale')
  })

  it('未 stale 时不带 stale 标记（避免所有曲线长得一样）', () => {
    const root = mountSparkline([1, 2]).find('.sysmon-sparkline')

    expect(root.classes()).not.toContain('sysmon-sparkline--stale')
    expect(root.attributes('title')).not.toContain('历史未刷新')
  })
})
