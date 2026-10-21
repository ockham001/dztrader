import { describe, it, expect, vi, beforeEach } from 'vitest'
import { mount, flushPromises } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import MonitorView from '../MonitorView.vue'
import { sysmonApi } from '@/api/sysmon'
import { makeSysmonSnapshot } from '@/test-utils/sysmonSnapshot'

vi.mock('@/api/sysmon', () => ({
  sysmonApi: { getSnapshot: vi.fn() },
}))

const stubs = {
  Icon: { template: '<span class="icon" />' },
}

async function mountView() {
  const wrapper = mount(MonitorView, { global: { stubs } })
  await flushPromises()
  return wrapper
}

describe('MonitorView 资源监控页（三种快照状态）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    vi.clearAllMocks()
  })

  it('全部可用：系统信息与 CPU 渲染真实数据，资源区块不显示不可用', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(
      makeSysmonSnapshot({ cpuUsageRatio: 0.187, resourceData: true }),
    )
    const wrapper = await mountView()

    // 系统信息接真实数据
    expect(wrapper.find('[data-test="monitor-system"]').text()).toContain('trade-01')
    expect(wrapper.find('[data-test="monitor-system"]').text()).toContain('AMD Ryzen 9 7950X 16-Core Processor')
    expect(wrapper.find('[data-test="monitor-system"]').text()).toContain('01:01:01')  // 运行时长
    // CPU 总使用率接真实数据（含进度条）
    expect(wrapper.find('[data-test="monitor-cpu-value"]').text()).toBe('18.7%')
    expect(wrapper.find('[role="progressbar"]').attributes('aria-valuenow')).toBe('19')
    // 内存 / 磁盘 / 网络按可用性渲染真实数据
    expect(wrapper.find('[data-test="monitor-memory-value"]').text()).toBe('50.0%')
    expect(wrapper.find('[data-test="monitor-disk"]').text()).toContain('/data')
    expect(wrapper.find('[data-test="monitor-disk"]').text()).toContain('25.0%')
    expect(wrapper.find('[data-test="monitor-network"]').text()).toContain('eth0')
    expect(wrapper.find('[data-test="monitor-network"]').text()).toContain('收 1.0 MiB/s')
    expect(wrapper.text()).not.toContain('不可用')
  })

  it('部分不可用：不可用指标显示明确空态（不得显示 0），其余真实数据照常渲染', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({
      cpuUsageRatio: null,
      metrics: { 'cpu.total': { available: false, reason: 'unsupported' } },
    }))
    const wrapper = await mountView()

    expect(wrapper.find('[data-test="monitor-cpu-unavailable"]').exists()).toBe(true)
    expect(wrapper.find('[data-test="monitor-cpu-unavailable"]').text()).toContain('不可用')
    expect(wrapper.find('[data-test="monitor-cpu-unavailable"]').text()).toContain('unsupported')
    expect(wrapper.find('[data-test="monitor-memory-unavailable"]').exists()).toBe(true)
    expect(wrapper.find('[data-test="monitor-disk-unavailable"]').exists()).toBe(true)
    expect(wrapper.find('[data-test="monitor-network-unavailable"]').exists()).toBe(true)
    expect(wrapper.find('[data-test="monitor-memory-unavailable"]').text()).toContain('该指标将在后续版本接入')
    // 系统信息仍可用
    expect(wrapper.find('[data-test="monitor-system"]').text()).toContain('trade-01')
    // 不得用 0 冒充 CPU 数值
    expect(wrapper.find('[data-test="monitor-cpu"]').text()).not.toContain('0.0%')
  })

  it('快照尚未到达：整页等待空态，不渲染任何指标数字', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockRejectedValue(new Error('HTTP 503'))
    const wrapper = await mountView()

    expect(wrapper.find('[data-test="monitor-waiting"]').exists()).toBe(true)
    expect(wrapper.find('[data-test="monitor-waiting"]').text()).toContain('等待系统监控数据')
    expect(wrapper.find('[data-test="monitor-system"]').exists()).toBe(false)
    expect(wrapper.find('[data-test="monitor-cpu"]').exists()).toBe(false)
    expect(wrapper.text()).not.toContain('%')
  })
})
