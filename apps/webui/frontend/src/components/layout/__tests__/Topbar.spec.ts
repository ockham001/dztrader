import { describe, it, expect, vi, beforeEach } from 'vitest'
import { mount, flushPromises } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import Topbar from '../Topbar.vue'
import { sysmonApi } from '@/api/sysmon'
import { useSysmonStore } from '@/stores/sysmon'
import { makeSysmonSnapshot } from '@/test-utils/sysmonSnapshot'

vi.mock('@/api/sysmon', () => ({
  sysmonApi: { getSnapshot: vi.fn() },
}))

vi.mock('@/composables/wsClient', () => ({
  useWebSocket: () => ({ connectionState: { value: 'connected' } }),
}))

const stubs = {
  Icon: { template: '<span class="icon" />' },
  ThemeDropdown: { template: '<div class="theme-dropdown-stub" />' },
}

async function mountTopbar() {
  const wrapper = mount(Topbar, { global: { stubs } })
  await flushPromises()
  return wrapper
}

function cpuBadgeText(wrapper: Awaited<ReturnType<typeof mountTopbar>>): string {
  return wrapper.find('#resCpu .res-value').text()
}

describe('Topbar 资源徽章（契约 sysmon 实时快照）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    vi.clearAllMocks()
  })

  it('删除写死的 NTP / 内存 / 磁盘假徽章，不残留任何假数字', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockRejectedValue(new Error('HTTP 503'))
    const wrapper = await mountTopbar()

    expect(wrapper.find('#resNtp').exists()).toBe(false)
    expect(wrapper.find('#resMem').exists()).toBe(false)
    expect(wrapper.find('#resDisk0').exists()).toBe(false)
    expect(wrapper.find('#resDisk1').exists()).toBe(false)
    for (const fake of ['NTP', '+12ms', '76%', '93%', '45%']) {
      expect(wrapper.text()).not.toContain(fake)
    }
  })

  it('快照尚未到达时 CPU 徽章显示等待态（不得显示 0）', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockRejectedValue(new Error('HTTP 503'))
    const wrapper = await mountTopbar()

    expect(cpuBadgeText(wrapper)).toBe('—')
    expect(wrapper.find('#resCpu').classes()).toContain('topbar__resource--unavailable')
  })

  it('CPU 徽章绑定实时快照百分比', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ cpuUsageRatio: 0.234 }))
    const wrapper = await mountTopbar()

    expect(cpuBadgeText(wrapper)).toBe('23%')
    expect(wrapper.find('#resCpu').classes()).toContain('topbar__resource--normal')
  })

  it('CPU 徽章按阈值映射警告/危险状态色', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ cpuUsageRatio: 0.75 }))
    const wrapper = await mountTopbar()
    expect(cpuBadgeText(wrapper)).toBe('75%')
    expect(wrapper.find('#resCpu').classes()).toContain('topbar__resource--warn')

    const store = useSysmonStore()
    store.applySnapshot(makeSysmonSnapshot({ cpuUsageRatio: 0.95 }))
    await wrapper.vm.$nextTick()
    expect(cpuBadgeText(wrapper)).toBe('95%')
    expect(wrapper.find('#resCpu').classes()).toContain('topbar__resource--danger')
  })

  it('CPU 不可用（可用性映射 false / 数值 null）时显示「不可用」', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({
      cpuUsageRatio: null,
      metrics: { 'cpu.total': { available: false, reason: 'unsupported' } },
    }))
    const wrapper = await mountTopbar()

    expect(cpuBadgeText(wrapper)).toBe('不可用')
    expect(wrapper.find('#resCpu').classes()).toContain('topbar__resource--unavailable')
  })

  it('WS 推送更新时顶栏随之刷新（不重新 REST 拉取）', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockRejectedValue(new Error('HTTP 503'))
    const wrapper = await mountTopbar()
    expect(cpuBadgeText(wrapper)).toBe('—')

    useSysmonStore().applySnapshot(makeSysmonSnapshot({ cpuUsageRatio: 0.5 }))
    await wrapper.vm.$nextTick()
    expect(cpuBadgeText(wrapper)).toBe('50%')
    expect(sysmonApi.getSnapshot).toHaveBeenCalledTimes(1)
  })
})
