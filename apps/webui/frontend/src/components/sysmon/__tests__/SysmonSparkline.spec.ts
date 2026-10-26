import { describe, expect, it } from 'vitest'
import { mount } from '@vue/test-utils'
import SysmonSparkline from '../SysmonSparkline.vue'
import type { SparklinePoint } from '@/composables/sysmonSparkline'

function points(...values: number[]): SparklinePoint[] {
  return values.map((value, index) => ({ ts: 1_790_000_000 + index * 60, value }))
}

describe('SysmonSparkline 内联趋势（#127）', () => {
  it('渲染折线：polyline 的 points 来自纯几何，末点值暴露给测试与标题', () => {
    const wrapper = mount(SysmonSparkline, {
      props: { points: points(10, 20), label: 'CPU 使用率' },
    })

    expect(wrapper.find('polyline').attributes('points')).toBe('0,22 100,2')
    expect(wrapper.find('.sysmon-sparkline').attributes('data-last-value')).toBe('20')
    expect(wrapper.find('.sysmon-sparkline').attributes('title')).toContain('CPU 使用率')
    expect(wrapper.find('.sysmon-sparkline').attributes('title')).toContain('最近 15 分钟')
  })

  it('点击整条 sparkline 触发 open（跳转历史视图并预选指标）', async () => {
    const wrapper = mount(SysmonSparkline, { props: { points: points(1, 2, 3), label: '内存使用率' } })

    await wrapper.find('.sysmon-sparkline').trigger('click')

    expect(wrapper.emitted('open')).toHaveLength(1)
  })

  it('少于两个点不渲染（一个点画不出趋势；历史不可用时同样静默不渲染）', () => {
    expect(mount(SysmonSparkline, { props: { points: [], label: 'x' } }).find('.sysmon-sparkline').exists())
      .toBe(false)
    expect(mount(SysmonSparkline, { props: { points: points(0.5), label: 'x' } }).find('.sysmon-sparkline').exists())
      .toBe(false)
  })

  it('装饰性图形对辅助技术隐藏，但整条曲线是可聚焦的按钮', () => {
    const wrapper = mount(SysmonSparkline, { props: { points: points(1, 2), label: 'CPU 使用率' } })

    expect(wrapper.find('button.sysmon-sparkline').exists()).toBe(true)
    expect(wrapper.find('svg').attributes('aria-hidden')).toBe('true')
    expect(wrapper.find('button.sysmon-sparkline').attributes('aria-label')).toContain('CPU 使用率')
  })
})
