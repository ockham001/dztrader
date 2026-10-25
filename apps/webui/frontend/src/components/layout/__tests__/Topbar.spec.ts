import { describe, it, expect, vi, beforeEach } from 'vitest'
import { mount, flushPromises } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import Topbar from '../Topbar.vue'
import { sysmonApi } from '@/api/sysmon'
import { useSysmonStore } from '@/stores/sysmon'
import { makeSysmonSnapshot } from '@/test-utils/sysmonSnapshot'

vi.mock('@/api/sysmon', () => ({
  sysmonApi: { getSnapshot: vi.fn(), getConfig: vi.fn() },
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

function memBadgeText(wrapper: Awaited<ReturnType<typeof mountTopbar>>): string {
  return wrapper.find('#resMem .res-value').text()
}

describe('Topbar 资源徽章（契约 sysmon 实时快照）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    vi.clearAllMocks()
  })

  it('删除写死的 NTP / 磁盘假徽章；内存徽章改由实时快照驱动，不残留假数字', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockRejectedValue(new Error('HTTP 503'))
    const wrapper = await mountTopbar()

    expect(wrapper.find('#resNtp').exists()).toBe(false)
    expect(wrapper.find('#resDisk0').exists()).toBe(false)
    expect(wrapper.find('#resDisk1').exists()).toBe(false)
    // 内存徽章存在，但快照未到达 → 等待态，不得显示假百分比（#74）
    expect(wrapper.find('#resMem').exists()).toBe(true)
    expect(memBadgeText(wrapper)).toBe('—')
    for (const fake of ['NTP', '+12ms', '76%', '93%', '45%']) {
      expect(wrapper.text()).not.toContain(fake)
    }
  })

  it('快照尚未到达时 CPU / 内存徽章显示等待态（不得显示 0）', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockRejectedValue(new Error('HTTP 503'))
    const wrapper = await mountTopbar()

    expect(cpuBadgeText(wrapper)).toBe('—')
    expect(wrapper.find('#resCpu').classes()).toContain('topbar__resource--unavailable')
    expect(memBadgeText(wrapper)).toBe('—')
    expect(wrapper.find('#resMem').classes()).toContain('topbar__resource--unavailable')
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

  it('内存徽章绑定实时快照使用率（#74，与 CPU 同构）', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot())
    const wrapper = await mountTopbar()

    expect(memBadgeText(wrapper)).toBe('50%')
    expect(wrapper.find('#resMem').classes()).toContain('topbar__resource--normal')
    expect(wrapper.find('#resMem').attributes('title')).toBe('内存使用率 50%')
  })

  it('内存徽章按 70%/90% 阈值映射警告/危险状态色', async () => {
    const GIB = 1024 ** 3
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({
      memory: {
        total_bytes: 100 * GIB,
        available_bytes: 25 * GIB,
        used_bytes: 75 * GIB,
        used_ratio: 0.75,
        cached_bytes: null,
        buffers_bytes: null,
      },
    }))
    const wrapper = await mountTopbar()
    expect(memBadgeText(wrapper)).toBe('75%')
    expect(wrapper.find('#resMem').classes()).toContain('topbar__resource--warn')

    useSysmonStore().applySnapshot(makeSysmonSnapshot({
      memory: {
        total_bytes: 100 * GIB,
        available_bytes: 5 * GIB,
        used_bytes: 95 * GIB,
        used_ratio: 0.95,
        cached_bytes: null,
        buffers_bytes: null,
      },
    }))
    await wrapper.vm.$nextTick()
    expect(memBadgeText(wrapper)).toBe('95%')
    expect(wrapper.find('#resMem').classes()).toContain('topbar__resource--danger')
    expect(wrapper.find('#resMem').attributes('title')).toBe('内存使用率 95%')
  })

  it('内存不可用（可用性映射 false）时显示「不可用」+ 原因', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({
      metrics: { memory: { available: false, reason: 'collect_failed' } },
    }))
    const wrapper = await mountTopbar()

    expect(memBadgeText(wrapper)).toBe('不可用')
    expect(wrapper.find('#resMem').classes()).toContain('topbar__resource--unavailable')
    expect(wrapper.find('#resMem').attributes('title')).toContain('collect_failed')
  })

  it('内存可用但本帧无有效比率（payload null）时显示「不可用」，不得退回 0', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ memory: null }))
    const wrapper = await mountTopbar()

    expect(memBadgeText(wrapper)).toBe('不可用')
    expect(wrapper.find('#resMem').classes()).toContain('topbar__resource--unavailable')
  })

  it('WS 推送更新时顶栏随之刷新（不重新 REST 拉取）', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockRejectedValue(new Error('HTTP 503'))
    const wrapper = await mountTopbar()
    expect(cpuBadgeText(wrapper)).toBe('—')
    expect(memBadgeText(wrapper)).toBe('—')

    useSysmonStore().applySnapshot(makeSysmonSnapshot({ cpuUsageRatio: 0.5 }))
    await wrapper.vm.$nextTick()
    expect(cpuBadgeText(wrapper)).toBe('50%')
    expect(memBadgeText(wrapper)).toBe('50%')
    expect(sysmonApi.getSnapshot).toHaveBeenCalledTimes(1)
  })
})

describe('Topbar 各盘徽章（#75：由快照卷集合驱动）', () => {
  const GIB = 1024 ** 3

  beforeEach(() => {
    setActivePinia(createPinia())
    vi.clearAllMocks()
  })

  function diskBadges(wrapper: Awaited<ReturnType<typeof mountTopbar>>) {
    return wrapper.findAll('.topbar__resource--disk')
  }

  it('默认快照：按卷集合渲染徽章，顺序为使用率降序（#122，不依赖发送端顺序）', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot())

    const wrapper = await mountTopbar()

    const badges = diskBadges(wrapper)
    expect(badges).toHaveLength(3)
    // #122：按使用率降序（62% > 40% > 25%），关注标记不再影响顺序
    expect(badges[0].attributes('data-mount')).toBe('/home')
    expect(badges[0].find('.res-value').text()).toBe('62%')
    expect(badges[0].classes()).not.toContain('topbar__resource--watched')

    expect(badges[1].attributes('data-mount')).toBe('/')
    expect(badges[1].find('.res-value').text()).toBe('40%')

    // watched 高亮与标题的「关注卷」标注保留（字段本身由 #124 删除）
    expect(badges[2].attributes('data-mount')).toBe('/data')
    expect(badges[2].classes()).toContain('topbar__resource--watched')
    // 标签取挂载点末段（顶栏窄条 overflow:hidden），完整路径在 data-mount / title 里
    expect(badges[2].find('.res-label').text()).toBe('data')
    expect(badges[2].find('.res-value').text()).toBe('25%')
    expect(badges[2].attributes('title')).toBe('/data 使用率 25%（关注卷）')
  })

  it('不硬编码盘符或数量：卷集合随快照变化（Windows 盘符同样按数据渲染）', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({
      volumes: [
        { mount: 'C:', watched: false, available: true, total_bytes: 500 * GIB, used_bytes: 250 * GIB, used_ratio: 0.5 },
        { mount: 'D:', watched: true, available: true, total_bytes: 1000 * GIB, used_bytes: 800 * GIB, used_ratio: 0.8 },
      ],
    }))
    const wrapper = await mountTopbar()

    const badges = diskBadges(wrapper)
    expect(badges).toHaveLength(2)
    expect(badges.map(badge => badge.attributes('data-mount'))).toEqual(['D:', 'C:'])
    expect(badges[0].classes()).toContain('topbar__resource--watched')
    expect(badges[0].find('.res-value').text()).toBe('80%')
    expect(badges[0].classes()).toContain('topbar__resource--warn')
    expect(badges[1].find('.res-value').text()).toBe('50%')
    expect(badges[1].classes()).toContain('topbar__resource--normal')
  })

  it('单卷不可用：徽章显示「不可用」+ 原因，不得用 0 冒充', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({
      volumes: [
        { mount: '/', watched: false, available: true, total_bytes: 100 * GIB, used_bytes: 40 * GIB, used_ratio: 0.4 },
        { mount: '/backup', watched: false, available: false, total_bytes: null, used_bytes: null, used_ratio: null, reason: 'no_permission' },
      ],
    }))
    const wrapper = await mountTopbar()

    const badges = diskBadges(wrapper)
    expect(badges).toHaveLength(2)
    expect(badges[1].find('.res-value').text()).toBe('不可用')
    expect(badges[1].classes()).toContain('topbar__resource--unavailable')
    expect(badges[1].attributes('title')).toBe('/backup 卷不可用: no_permission')
    expect(badges[1].text()).not.toContain('0%')
  })

  it('volumes 不可用 / 快照未到达：不渲染盘徽章（不残留假数字）', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockRejectedValue(new Error('HTTP 503'))
    const wrapper = await mountTopbar()
    expect(diskBadges(wrapper)).toHaveLength(0)

    useSysmonStore().applySnapshot(makeSysmonSnapshot({
      metrics: { volumes: { available: false, reason: 'disabled_by_config' } },
    }))
    await wrapper.vm.$nextTick()
    expect(diskBadges(wrapper)).toHaveLength(0)
  })

  it('WS 推送刷新盘徽章（不重新 REST 拉取）', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot())
    const wrapper = await mountTopbar()
    expect(diskBadges(wrapper)).toHaveLength(3)

    useSysmonStore().applySnapshot(makeSysmonSnapshot({
      volumes: [
        { mount: '/only', watched: true, available: true, total_bytes: 10 * GIB, used_bytes: 9 * GIB, used_ratio: 0.9 },
      ],
    }))
    await wrapper.vm.$nextTick()

    const badges = diskBadges(wrapper)
    expect(badges).toHaveLength(1)
    expect(badges[0].attributes('data-mount')).toBe('/only')
    expect(badges[0].find('.res-value').text()).toBe('90%')
    expect(badges[0].classes()).toContain('topbar__resource--danger')
    expect(sysmonApi.getSnapshot).toHaveBeenCalledTimes(1)
  })
})

describe('Topbar 阈值告警徽章（#80）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    vi.clearAllMocks()
  })

  it('无活动告警：不渲染告警徽章', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot())
    const wrapper = await mountTopbar()

    expect(wrapper.find('#resAlerts').exists()).toBe(false)
  })

  it('有活动告警：渲染全局常驻计数徽章，title 汇总类型', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({
      alerts: [
        { kind: 'memory_used', subject: '', since: 1 },
        { kind: 'volume_free', subject: '/data', since: 2 },
      ],
    }))
    const wrapper = await mountTopbar()

    const badge = wrapper.find('#resAlerts')
    expect(badge.exists()).toBe(true)
    expect(badge.text()).toContain('ALERT')
    expect(badge.text()).toContain('2')
    expect(badge.classes()).toContain('topbar__resource--danger')
    expect(badge.attributes('title')).toContain('资源告警 2 项')
    expect(badge.attributes('title')).toContain('内存')
    expect(badge.attributes('title')).toContain('/data')
  })

  it('恢复后（后续快照 alerts 为空）徽章消失', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({
      alerts: [{ kind: 'swap_used', subject: '', since: 1 }],
    }))
    const wrapper = await mountTopbar()
    expect(wrapper.find('#resAlerts').exists()).toBe(true)

    useSysmonStore().applySnapshot(makeSysmonSnapshot())
    await wrapper.vm.$nextTick()
    expect(wrapper.find('#resAlerts').exists()).toBe(false)
  })
})
