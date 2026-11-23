import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest'
import { enableAutoUnmount, mount, flushPromises } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import MonitorView from '../MonitorView.vue'
import { sysmonApi } from '@/api/sysmon'
import { useAuthStore } from '@/stores/auth'
import { sysmonPending, useSysmonStore } from '@/stores/sysmon'
import {
  SYSMON_CLOCK_LIVE,
  makeSysmonConfig,
  makeSysmonSnapshot,
} from '@/test-utils/sysmonSnapshot'

// #450：监控页时钟块（契约 sysmon §2.9 / §2.8 / §3.1）。
// 旧「时间卡片」（#302）随台账删除，改为块内四数字 + 服务器表 + 同步服务一行；口径全部复用
// `composables/sysmonClock.ts`（域状态 / 定档 / 质量判据 / 失败原因 / 每台单元格）。
// 本文件只覆盖**按钮与交互边界**（非 admin / HTTP 失败 / pending / 冷却 / 配置关闭），
// 四数字与空态的渲染断言在 MonitorView.spec.ts（三块行为用例）里，避免同一事实两处维护。

vi.mock('@/api/sysmon', () => ({
  sysmonApi: {
    getSnapshot: vi.fn(),
    setConfig: vi.fn(),
    getConfig: vi.fn(),
    getHistory: vi.fn(),
    probeClock: vi.fn(),
  },
}))

const stubs = { Icon: { template: '<span class="icon" />' } }

/** admin 判定走 auth store 的 isAdmin（role === 'admin'），与网络开关 / 配置编辑同一权限来源 */
function setRole(role: 'admin' | 'user'): void {
  useAuthStore().user = {
    id: 1,
    username: role,
    display_name: role,
    email: `${role}@x.com`,
    role,
    status: 'online',
    created_at: '2026-08-01',
  }
}

async function mountView() {
  const wrapper = mount(MonitorView, { global: { stubs } })
  await flushPromises()
  return wrapper
}

// #371：MonitorView 挂了 1 秒 ticker，用例必须卸载 —— 不卸载时未清理的定时器会在
// 后续用例的 flush 里插入无主更新，实测会让无关用例偶发假红。
enableAutoUnmount(afterEach)

describe('时钟块 · 「立即查询」的交互边界（契约 sysmon §1.1 / §3.1）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    sysmonPending.__resetForTests()
    vi.clearAllMocks()
  })

  afterEach(() => {
    vi.useRealTimers()
  })

  it('非 admin：按钮可见但不可点（避免隐藏造成「功能不存在」的误解），点击不下发', async () => {
    setRole('user')
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ clock: SYSMON_CLOCK_LIVE }))
    const wrapper = await mountView()

    const button = wrapper.find('[data-test="monitor-clock-probe"]')
    expect(button.exists()).toBe(true)
    expect(button.attributes('disabled')).toBeDefined()
    expect(button.attributes('title')).toContain('仅管理员')
    await button.trigger('click')
    await flushPromises()
    expect(sysmonApi.probeClock).not.toHaveBeenCalled()
  })

  it('配置关闭：按钮渲染但置灰（点击无处可点，dzsysmon 会忽略）', async () => {
    setRole('admin')
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ clock: null }))
    const wrapper = await mountView()

    expect(wrapper.find('[data-test="monitor-clock-probe"]').attributes('disabled')).toBeDefined()
    expect(wrapper.find('[data-test="monitor-clock-disabled"]').exists()).toBe(true)
  })

  it('HTTP 失败立即恢复可点，且不伪造冷却（冷却秒数仍为 0）', async () => {
    setRole('admin')
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ clock: SYSMON_CLOCK_LIVE }))
    vi.mocked(sysmonApi.getConfig).mockResolvedValue(
      makeSysmonConfig({ clock: { manual_min_interval_sec: 10 } }),
    )
    vi.mocked(sysmonApi.probeClock).mockRejectedValue(new Error('HTTP 503'))
    const wrapper = await mountView()

    await wrapper.find('[data-test="monitor-clock-probe"]').trigger('click')
    await flushPromises()

    expect(useSysmonStore().clockProbeCooldownSec).toBe(0)
    expect(wrapper.find('[data-test="monitor-clock-probe"]').attributes('disabled')).toBeUndefined()
    expect(wrapper.find('[data-test="monitor-clock-probe"]').text()).toContain('立即查询')
  })

  it('REST 已下发但探测未完成：文案为「查询中…」（冷却文案之外的另一个置灰分支）', async () => {
    setRole('admin')
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ clock: SYSMON_CLOCK_LIVE }))
    // 配置 0 = 不限流：只剩「等快照」一条约束，pending 不被冷却文案盖住
    vi.mocked(sysmonApi.getConfig).mockResolvedValue(
      makeSysmonConfig({ clock: { manual_min_interval_sec: 0 } }),
    )
    // REST 永不 resolve：pending 保持在途，模拟「事件通道已写、探测还在进行」
    vi.mocked(sysmonApi.probeClock).mockImplementation(() => new Promise(() => {}))
    const wrapper = await mountView()

    await wrapper.find('[data-test="monitor-clock-probe"]').trigger('click')
    await flushPromises()

    expect(useSysmonStore().clockProbePending).toBe(true)
    expect(wrapper.find('[data-test="monitor-clock-probe"]').attributes('disabled')).toBeDefined()
    expect(wrapper.find('[data-test="monitor-clock-probe"]').text()).toContain('查询中…')
  })

  it('手动限流 0：快照前进即恢复可点，不进入倒计时', async () => {
    setRole('admin')
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ clock: SYSMON_CLOCK_LIVE }))
    vi.mocked(sysmonApi.getConfig).mockResolvedValue(
      makeSysmonConfig({ clock: { manual_min_interval_sec: 0 } }),
    )
    vi.mocked(sysmonApi.probeClock).mockResolvedValue({ ok: true })
    const wrapper = await mountView()

    await wrapper.find('[data-test="monitor-clock-probe"]').trigger('click')
    await flushPromises()
    expect(useSysmonStore().clockProbePending).toBe(true)

    useSysmonStore().applySnapshot(makeSysmonSnapshot({
      clock: { ...SYSMON_CLOCK_LIVE, last_probe_unix_ms: SYSMON_CLOCK_LIVE.last_probe_unix_ms + 5_000 },
    }))
    await flushPromises()
    expect(useSysmonStore().clockProbeCooldownSec).toBe(0)
    expect(wrapper.find('[data-test="monitor-clock-probe"]').attributes('disabled')).toBeUndefined()
  })
})

describe('时钟块 · 只属于本页（不越界到顶栏 / 无历史曲线）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    sysmonPending.__resetForTests()
    vi.clearAllMocks()
  })

  it('配置关闭态下本视图不出现 NTP 徽章文案（顶栏时钟偏差图标徽章由 Topbar 自己渲染）', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ clock: null }))
    const wrapper = await mountView()
    expect(wrapper.find('[data-test="monitor-clock"]').text()).not.toContain('NTP')
  })

  it('时钟维度不在 history v1 里：块内没有曲线 / sparkline（不编造历史）', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ clock: SYSMON_CLOCK_LIVE }))
    const wrapper = await mountView()
    expect(wrapper.find('[data-test="monitor-clock"] .sysmon-sparkline').exists()).toBe(false)
    expect(wrapper.find('[data-test="monitor-clock"] .monitor-view__chart').exists()).toBe(false)
  })

})
