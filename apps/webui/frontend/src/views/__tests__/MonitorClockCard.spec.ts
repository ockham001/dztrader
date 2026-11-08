import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest'
import { mount, flushPromises } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import MonitorView from '../MonitorView.vue'
import { sysmonApi } from '@/api/sysmon'
import { useAuthStore } from '@/stores/auth'
import { sysmonPending, useSysmonStore } from '@/stores/sysmon'
import {
  SYSMON_CLOCK_ALL_FAILED,
  SYSMON_CLOCK_LIVE,
  makeSysmonConfig,
  makeSysmonSnapshot,
} from '@/test-utils/sysmonSnapshot'

// #302：监控页「时间」卡片（契约 sysmon §2.9 / §2.8 / §3.1）。
// 本文件只覆盖时间卡片：三种空态、偏差着色、测量质量提示的独立性、展开明细（每台服务器 +
// 同步服务）、「立即查询」按钮的置灰与恢复。其余实时卡片的数据渲染见 MonitorView.spec.ts，
// 折叠 / 响应式见 MonitorViewTrend.spec.ts，展示口径的纯函数判据见 composables/__tests__/sysmonClock.spec.ts。

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

/** 上次探测时刻（夹具常量：快照 timestamp 前 30 秒）→ 相对时间「30 秒前」 */
const PROBE_AGE_TEXT = '30 秒前'

describe('时间卡片 · 空态三分支（契约 §2.9）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    sysmonPending.__resetForTests()
    vi.clearAllMocks()
  })

  it('尚未探测：显示「尚未探测」明确文案，不显示 0、不显示「不可用」', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot())
    const wrapper = await mountView()

    const card = wrapper.find('[data-test="monitor-clock"]')
    expect(card.exists()).toBe(true)
    expect(wrapper.find('[data-test="monitor-clock-unprobed"]').exists()).toBe(true)
    expect(wrapper.find('[data-test="monitor-clock-unprobed"]').text()).toContain('尚未探测')
    // 尚未探测没有主数值（不得用 0 冒充）
    expect(wrapper.find('[data-test="monitor-clock-offset"]').exists()).toBe(false)
    expect(card.text()).not.toContain('0.0 ms')
    // 「尚未探测」不是「不可用」：两者是不同事实（契约 §2.9 注）
    expect(wrapper.find('[data-test="monitor-clock-unavailable"]').exists()).toBe(false)
    expect(wrapper.find('[data-test="monitor-clock-disabled"]').exists()).toBe(false)
  })

  it('尚未探测：不同步服务区（首轮探测前没有做过检测，不得凭空报「未检测到服务」）', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot())
    const wrapper = await mountView()

    expect(wrapper.find('[data-test="monitor-clock-daemons"]').exists()).toBe(false)
    expect(wrapper.find('[data-test="monitor-clock-daemon-none"]').exists()).toBe(false)
  })

  it('全部失败：显示「不可用」+ 每台原因，不显示 0 或上一轮数字', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(
      makeSysmonSnapshot({ clock: SYSMON_CLOCK_ALL_FAILED }),
    )
    const wrapper = await mountView()

    expect(wrapper.find('[data-test="monitor-clock-unavailable"]').exists()).toBe(true)
    const hint = wrapper.find('[data-test="monitor-clock-unavailable"]').text()
    expect(hint).toContain('不可用')
    // 逐台原因去重并列（超时 / 无法解析），可诊断信息不被抹掉
    expect(hint).toContain('超时未响应')
    expect(hint).toContain('无法解析或连接失败')
    // 主数值显示「不可用」而不是 0
    expect(wrapper.find('[data-test="monitor-clock-offset"]').text()).toBe('不可用')
    // 不可用不套色标（不得用 normal 冒充「时钟很准」）
    expect(wrapper.find('[data-test="monitor-clock-offset"]').element.parentElement?.getAttribute('data-level'))
      .toBeNull()
  })

  it('配置关闭：显示明确空态（而不是整块消失），且无偏差数字', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ clock: null }))
    const wrapper = await mountView()

    // 卡片仍在：`clock` 域被关掉是「这项监控被关掉了」这一运维事实，静默消失会被误读为功能不存在
    expect(wrapper.find('[data-test="monitor-clock"]').exists()).toBe(true)
    expect(wrapper.find('[data-test="monitor-clock-disabled"]').exists()).toBe(true)
    expect(wrapper.find('[data-test="monitor-clock-disabled"]').text()).toContain('已在配置中关闭')
    expect(wrapper.find('[data-test="monitor-clock-offset"]').exists()).toBe(false)
    // 配置关闭时同步服务区不渲染（该域整体关闭，服务检测同样不执行）
    expect(wrapper.find('[data-test="monitor-clock-daemons"]').exists()).toBe(false)
    // 立即查询不可点（dzsysmon 会忽略，按钮无处可点）
    expect(wrapper.find('[data-test="monitor-clock-probe"]').attributes('disabled')).toBeDefined()
  })
})

describe('时间卡片 · 正常态数值与着色', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    sysmonPending.__resetForTests()
    vi.clearAllMocks()
  })

  it('显示带符号偏差、误差界、来源服务器、stratum、上次探测相对时间', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ clock: SYSMON_CLOCK_LIVE }))
    const wrapper = await mountView()

    expect(wrapper.find('[data-test="monitor-clock-offset"]').text()).toBe('+12.5 ms')
    const summary = wrapper.find('[data-test="monitor-clock-summary"]').text()
    expect(summary).toContain('误差界 18.3 ms')
    expect(summary).toContain('来源 ntp.aliyun.com')
    expect(summary).toContain('stratum 2')
    expect(summary).toContain(PROBE_AGE_TEXT)
  })

  it('负偏差保留符号（本地慢），方向是语义的一部分', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({
      clock: { ...SYSMON_CLOCK_LIVE, offset_ms: -12.5 },
    }))
    const wrapper = await mountView()

    expect(wrapper.find('[data-test="monitor-clock-offset"]').text()).toBe('-12.5 ms')
  })

  it.each([
    [12.5, 'normal'],
    [100, 'warn'],     // 恰好等于 warn 阈值算进入（契约 §2.8：≥）
    [999, 'warn'],
    [1000, 'danger'],  // 恰好等于 error 阈值算进入
    [-1000, 'danger'],
  ] as Array<[number, string]>)('偏差 %s ms → 着色 %s', async (offsetMs, expected) => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({
      clock: { ...SYSMON_CLOCK_LIVE, offset_ms: offsetMs },
    }))
    const wrapper = await mountView()

    expect(wrapper.find('[data-test="monitor-clock-offset"]').element.parentElement?.getAttribute('data-level'))
      .toBe(expected)
  })

  it('阈值取生效配置画像：配置把 warn 调到 50 ms 后，偏差 60 ms 即为 warn（前端不另定一套阈值）', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({
      clock: { ...SYSMON_CLOCK_LIVE, offset_ms: 60 },
    }))
    vi.mocked(sysmonApi.getConfig).mockResolvedValue(makeSysmonConfig({
      clock: { offset_warn_ms: 50, offset_error_ms: 500 },
    }))
    const wrapper = await mountView()

    expect(wrapper.find('[data-test="monitor-clock-offset"]').element.parentElement?.getAttribute('data-level'))
      .toBe('warn')
  })
})

describe('时间卡片 · 测量质量提示独立于偏差着色（契约 §2.8）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    sysmonPending.__resetForTests()
    vi.clearAllMocks()
  })

  it('误差界越界但偏差正常：出现质量提示，偏差仍是 normal', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({
      clock: { ...SYSMON_CLOCK_LIVE, offset_ms: 3, bound_ms: 500 },
    }))
    const wrapper = await mountView()

    expect(wrapper.find('[data-test="monitor-clock-quality"]').exists()).toBe(true)
    expect(wrapper.find('[data-test="monitor-clock-quality"]').text()).toContain('测量质量')
    expect(wrapper.find('[data-test="monitor-clock-quality"]').text()).toContain('200 ms')
    expect(wrapper.find('[data-test="monitor-clock-offset"]').element.parentElement?.getAttribute('data-level'))
      .toBe('normal')
  })

  it('偏差越界但误差界正常：偏差着色照常，不出现质量提示', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({
      clock: { ...SYSMON_CLOCK_LIVE, offset_ms: 1500, bound_ms: 10 },
    }))
    const wrapper = await mountView()

    expect(wrapper.find('[data-test="monitor-clock-quality"]').exists()).toBe(false)
    expect(wrapper.find('[data-test="monitor-clock-offset"]').element.parentElement?.getAttribute('data-level'))
      .toBe('danger')
  })

  it('误差界恰好等于阈值不算越界（契约 §2.8：> 才进入）', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({
      clock: { ...SYSMON_CLOCK_LIVE, bound_ms: 200 },
    }))
    const wrapper = await mountView()

    expect(wrapper.find('[data-test="monitor-clock-quality"]').exists()).toBe(false)
  })
})

describe('时间卡片 · 展开明细（每台服务器 + 同步服务检测）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    sysmonPending.__resetForTests()
    vi.clearAllMocks()
  })

  it('默认收起；展开后每台服务器六列齐全，失败台的四项测量显示「不可用」而不是 0', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ clock: SYSMON_CLOCK_LIVE }))
    const wrapper = await mountView()

    expect(wrapper.find('[data-test="monitor-clock-server"]').exists()).toBe(false)
    const toggle = wrapper.find('[data-test="monitor-clock-detail-toggle"]')
    expect(toggle.attributes('aria-expanded')).toBe('false')
    await toggle.trigger('click')
    await flushPromises()

    expect(toggle.attributes('aria-expanded')).toBe('true')
    const rows = wrapper.findAll('[data-test="monitor-clock-server"]')
    expect(rows).toHaveLength(3)  // 两台成功 + 一台失败（部分失败是合法形态）
    expect(rows.map(row => row.attributes('data-host'))).toEqual([
      'cn.pool.ntp.org',
      'ntp.aliyun.com',
      'ntp.tencent.com',
    ])

    // 成功台：偏差带符号、误差界 / RTT 1 位毫秒、stratum、无失败原因
    const aliyun = rows.find(row => row.attributes('data-host') === 'ntp.aliyun.com')!
    expect(aliyun.find('[data-test="monitor-clock-server-offset"]').text()).toBe('+12.5 ms')
    expect(aliyun.find('[data-test="monitor-clock-server-bound"]').text()).toBe('18.3 ms')
    expect(aliyun.find('[data-test="monitor-clock-server-rtt"]').text()).toBe('24.1 ms')
    expect(aliyun.find('[data-test="monitor-clock-server-stratum"]').text()).toBe('2')
    expect(aliyun.find('[data-test="monitor-clock-server-error"]').text()).toBe('—')

    // 失败台：四项测量全部「不可用」（失败样本不产出任何测量值，契约 §2.9），原因给中文文案
    const failed = rows.find(row => row.attributes('data-host') === 'cn.pool.ntp.org')!
    expect(failed.attributes('data-ok')).toBe('false')
    for (const key of ['offset', 'bound', 'rtt', 'stratum']) {
      expect(failed.find(`[data-test="monitor-clock-server-${key}"]`).text()).toBe('不可用')
    }
    expect(failed.find('[data-test="monitor-clock-server-error"]').text()).toBe('超时未响应')
  })

  it('同步服务列表展示名称与运行状态；未检测到时显示明确文案（事实，不是不可用）', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ clock: SYSMON_CLOCK_LIVE }))
    const wrapper = await mountView()

    const daemons = wrapper.findAll('[data-test="monitor-clock-daemon"]')
    expect(daemons).toHaveLength(1)
    expect(daemons[0].attributes('data-name')).toBe('chronyd')
    expect(daemons[0].text()).toContain('运行中')

    // 未检测到运行中的服务：空数组是事实（契约 §2.9 / §2.5），显示文案而不是「不可用」。
    // 同一 store 实例内推进快照（组件挂载后 loaded 已为 true，不会再走 REST 补拉）。
    useSysmonStore().applySnapshot(makeSysmonSnapshot({ clock: { ...SYSMON_CLOCK_LIVE, daemons: [] } }))
    await flushPromises()
    const none = wrapper.find('[data-test="monitor-clock-daemon-none"]')
    expect(none.exists()).toBe(true)
    expect(none.text()).toContain('未检测到运行中的时间同步服务')
    expect(none.text()).not.toContain('不可用')
  })

  it('多服务同时运行时显示冲突提示（不弹窗、不判故障，契约 §2.9）', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({
      clock: {
        ...SYSMON_CLOCK_LIVE,
        daemons: [{ name: 'chronyd', running: true }, { name: 'ntpd', running: true }],
        daemon_conflict: true,
      },
    }))
    const wrapper = await mountView()

    expect(wrapper.findAll('[data-test="monitor-clock-daemon"]')).toHaveLength(2)
    expect(wrapper.find('[data-test="monitor-clock-daemon-conflict"]').exists()).toBe(true)
    expect(wrapper.find('[data-test="monitor-clock-daemon-conflict"]').text()).toContain('多个时间同步服务')
  })
})

describe('时间卡片 · 「立即查询」按钮（契约 sysmon §1.1）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    sysmonPending.__resetForTests()
    vi.clearAllMocks()
  })

  afterEach(() => {
    vi.useRealTimers()
  })

  it('admin 点击后按钮置灰，随后的快照（探测时刻前进）恢复可点', async () => {
    setRole('admin')
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ clock: SYSMON_CLOCK_LIVE }))
    // 配置不限流（manual_min_interval_sec = 0）：只剩「等快照」一条约束，快照到达即恢复可点。
    // 默认 10 秒冷却的置灰由下一个用例覆盖（两条约束取更长者，见卡片注释）。
    vi.mocked(sysmonApi.getConfig).mockResolvedValue(makeSysmonConfig({ clock: { manual_min_interval_sec: 0 } }))
    vi.mocked(sysmonApi.probeClock).mockResolvedValue({ ok: true })
    const wrapper = await mountView()

    const button = wrapper.find('[data-test="monitor-clock-probe"]')
    expect(button.attributes('disabled')).toBeUndefined()
    expect(button.text()).toContain('立即查询')

    await button.trigger('click')
    await flushPromises()

    expect(sysmonApi.probeClock).toHaveBeenCalledTimes(1)
    // REST 成功只表示「已写事件通道」：按钮仍置灰，等探测完成的快照（契约 §1.1）
    expect(wrapper.find('[data-test="monitor-clock-probe"]').attributes('disabled')).toBeDefined()

    // 探测完成：last_probe_unix_ms 前进 → 恢复可点
    useSysmonStore().applySnapshot(makeSysmonSnapshot({
      clock: { ...SYSMON_CLOCK_LIVE, last_probe_unix_ms: SYSMON_CLOCK_LIVE.last_probe_unix_ms + 5_000 },
    }))
    await flushPromises()
    expect(wrapper.find('[data-test="monitor-clock-probe"]').attributes('disabled')).toBeUndefined()
  })

  it('置灰时长取配置 clock.manual_min_interval_sec，并在按钮上显示剩余秒数', async () => {
    vi.useFakeTimers()
    setRole('admin')
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ clock: SYSMON_CLOCK_LIVE }))
    vi.mocked(sysmonApi.getConfig).mockResolvedValue(makeSysmonConfig({
      clock: { manual_min_interval_sec: 3 },
    }))
    vi.mocked(sysmonApi.probeClock).mockResolvedValue({ ok: true })
    const wrapper = await mountView()

    await wrapper.find('[data-test="monitor-clock-probe"]').trigger('click')
    await vi.advanceTimersByTimeAsync(0)
    await flushPromises()

    expect(wrapper.find('[data-test="monitor-clock-probe"]').attributes('disabled')).toBeDefined()
    expect(wrapper.find('[data-test="monitor-clock-probe"]').text()).toContain('3 秒后可再次查询')

    await vi.advanceTimersByTimeAsync(2_000)
    await flushPromises()
    expect(wrapper.find('[data-test="monitor-clock-probe"]').text()).toContain('1 秒后可再次查询')

    await vi.advanceTimersByTimeAsync(1_000)
    await flushPromises()
    expect(wrapper.find('[data-test="monitor-clock-probe"]').attributes('disabled')).toBeUndefined()
  })

  it('非 admin：按钮可见但不可点（避免隐藏造成「功能不存在」的误解）', async () => {
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

  it('HTTP 失败立即恢复可点（不伪造冷却）', async () => {
    setRole('admin')
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ clock: SYSMON_CLOCK_LIVE }))
    vi.mocked(sysmonApi.probeClock).mockRejectedValue(new Error('HTTP 503'))
    const wrapper = await mountView()

    await wrapper.find('[data-test="monitor-clock-probe"]').trigger('click')
    await flushPromises()

    expect(wrapper.find('[data-test="monitor-clock-probe"]').attributes('disabled')).toBeUndefined()
  })
})

describe('时间卡片 · 不越界到顶栏', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    sysmonPending.__resetForTests()
    vi.clearAllMocks()
  })

  it('时钟卡片只在监控页实时网格内渲染（顶栏徽章不在本视图）', async () => {
    vi.mocked(sysmonApi.getSnapshot).mockResolvedValue(makeSysmonSnapshot({ clock: SYSMON_CLOCK_LIVE }))
    const wrapper = await mountView()

    expect(wrapper.find('[data-test="monitor-clock"]').exists()).toBe(true)
    // 顶栏的 NTP 徽章已删除且不得回归（Topbar.spec.ts 的假数字断言保持有效、本票不改它）：
    // 本视图可见文本里不含任何 NTP 文案（模板注释里的「NTP 徽章」不算可见内容）。
    expect(wrapper.text()).not.toContain('NTP')
  })
})
