import { describe, it, expect, vi, beforeEach } from 'vitest'
import { readFileSync } from 'node:fs'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { mount, flushPromises } from '@vue/test-utils'
import type { VueWrapper } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import SysmonConfigTab from '../SysmonConfigTab.vue'
import { sysmonApi } from '@/api/sysmon'
import { useAuthStore } from '@/stores/auth'
import { sysmonPending, useSysmonStore } from '@/stores/sysmon'
import { useToastStore } from '@/stores/toast'
import { makeSysmonConfig } from '@/test-utils/sysmonSnapshot'
import type { SysmonConfigPayload } from '@/types/generated'

vi.mock('@/api/sysmon', () => ({
  sysmonApi: { getSnapshot: vi.fn(), setConfig: vi.fn(), getConfig: vi.fn() },
}))

const stubs = {
  Icon: { template: '<span class="icon" />' },
}

/** admin 判定走 auth store 的 isAdmin（role === 'admin'），与系统设置页同一权限来源 */
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

async function mountTab(options: {
  config?: SysmonConfigPayload
  role?: 'admin' | 'user'
  /** false = 保持 #207 D1 的默认折叠态（用于验证默认布局）；默认展开以便断言生效值 */
  expandSummary?: boolean
} = {}) {
  const store = useSysmonStore()
  if (options.config) store.applyConfig(options.config)
  setRole(options.role ?? 'admin')
  const wrapper = mount(SysmonConfigTab, { global: { stubs } })
  await flushPromises()
  if (options.expandSummary !== false) await expandSummary(wrapper)
  return { wrapper, store }
}

function valueText(wrapper: VueWrapper, field: string): string {
  return wrapper.find(`[data-test="sysmon-config-value-${field}"]`).text()
}

/**
 * #207 D1 起 admin 的只读摘要默认折叠（避免与编辑表单字段重复两套）；
 * 验证「生效值展示」的用例需先展开。
 */
async function expandSummary(wrapper: VueWrapper): Promise<void> {
  const toggle = wrapper.find('[data-test="sysmon-config-summary-toggle"]')
  if (toggle.exists() && toggle.attributes('aria-expanded') === 'false') {
    await toggle.trigger('click')
    await flushPromises()
  }
}

describe('SysmonConfigTab 生效配置展示（#81）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    sysmonPending.__resetForTests()
    vi.clearAllMocks()
  })

  it('配置镜像尚未到达：显示「配置尚未到达」空态，不渲染分区与编辑表单（不显示假默认值）', async () => {
    const { wrapper } = await mountTab()

    expect(wrapper.find('[data-test="sysmon-config-unavailable"]').text()).toContain('配置尚未到达')
    expect(wrapper.find('[data-test="sysmon-config-section-sampling"]').exists()).toBe(false)
    expect(wrapper.find('[data-test="sysmon-config-editor"]').exists()).toBe(false)
  })

  it('镜像到达：按六个段展示生效值（数值带单位、布尔语义化、数组逐条）', async () => {
    const { wrapper } = await mountTab({
      config: makeSysmonConfig({
        disk: { devices: ['sda'] },
      }),
    })

    // 六个段都在
    for (const section of ['sampling', 'disk', 'network', 'history', 'realtime', 'thresholds']) {
      expect(wrapper.find(`[data-test="sysmon-config-section-${section}"]`).exists()).toBe(true)
    }
    // 数字 + 单位
    expect(valueText(wrapper, 'sampling.measure_interval_ms')).toBe('1000 ms')
    expect(valueText(wrapper, 'sampling.record_interval_sec')).toBe('60 秒')
    expect(valueText(wrapper, 'history.raw_retention_hours')).toBe('24 小时')
    expect(valueText(wrapper, 'history.max_retention_days')).toBe('30 天')
    expect(valueText(wrapper, 'history.max_bytes')).toBe('2.0 GiB')
    expect(valueText(wrapper, 'realtime.push_interval_sec')).toBe('5 秒')
    expect(valueText(wrapper, 'thresholds.cpu_sustain_used_pct')).toBe('90%')
    expect(valueText(wrapper, 'thresholds.cpu_sustain_samples')).toBe('10 次')
    expect(valueText(wrapper, 'thresholds.memory_used_pct')).toBe('90%')
    expect(valueText(wrapper, 'thresholds.swap_used_pct')).toBe('50%')
    expect(valueText(wrapper, 'thresholds.volume_free_pct')).toBe('10%')
    expect(valueText(wrapper, 'thresholds.log_repeat_sec')).toBe('600 秒')
    // 布尔语义化
    expect(valueText(wrapper, 'network.hide_virtual')).toBe('已启用')
    expect(valueText(wrapper, 'history.enabled')).toBe('已启用')
    // groups 8 项逐项展示
    const groupKeys = ['cpu', 'memory', 'swap', 'volumes', 'disk_io', 'network', 'temperature', 'cpu_frequency']
    for (const key of groupKeys) {
      expect(wrapper.find(`[data-test="sysmon-config-value-sampling.groups.${key}"]`).exists()).toBe(true)
    }
    expect(valueText(wrapper, 'sampling.groups.cpu')).toBe('已启用')
    // devices 逐条
    expect(wrapper.find('[data-test="sysmon-config-value-disk.devices"]').text()).toBe('sda')
    // 卷空间阈值只有一个形态（空闲百分比），标签必须点明「空闲百分比」，避免与使用率口径混淆。
    expect(valueText(wrapper, 'thresholds.volume_free_pct')).toBe('10%')
    expect(wrapper.text()).toContain('卷剩余空间阈值（空闲百分比）')
  })

  it('空数组字段按契约语义展示（devices 全部物理盘），不留空值', async () => {
    const { wrapper, store } = await mountTab({ config: makeSysmonConfig() })

    expect(valueText(wrapper, 'disk.devices')).toBe('全部物理盘')
    // max_bytes=0 表示不限制（不是 0 B）
    store.applyConfig(makeSysmonConfig({ history: { max_bytes: 0 } }))
    await flushPromises()
    expect(valueText(wrapper, 'history.max_bytes')).toBe('不限制')
  })

  it('非 admin：展示生效值 + 只读提示，不渲染编辑表单与保存按钮', async () => {
    const { wrapper } = await mountTab({ config: makeSysmonConfig(), role: 'user' })

    expect(wrapper.find('[data-test="sysmon-config-readonly"]').text()).toContain('仅管理员可修改')
    expect(valueText(wrapper, 'sampling.measure_interval_ms')).toBe('1000 ms')
    expect(wrapper.find('[data-test="sysmon-config-editor"]').exists()).toBe(false)
    expect(wrapper.find('[data-test="sysmon-config-save"]').exists()).toBe(false)
  })
})

describe('SysmonConfigTab 编辑与保存（#81，仅 admin）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    sysmonPending.__resetForTests()
    vi.clearAllMocks()
  })

  it('表单初值与镜像一致：保存禁用并提示「无改动」', async () => {
    const { wrapper } = await mountTab({ config: makeSysmonConfig() })

    expect(wrapper.find('[data-test="sysmon-config-save"]').attributes('disabled')).toBeDefined()
    expect(wrapper.find('[data-test="sysmon-config-clean"]').exists()).toBe(true)
    expect((wrapper.find('[data-test="sysmon-config-input-measure_interval_ms"]').element as HTMLInputElement).value)
      .toBe('1000')
  })

  it('只下发与原镜像的差异：改一个数字 → patch 只含该字段；pending 到 WS 回推才清', async () => {
    vi.mocked(sysmonApi.setConfig).mockResolvedValue({ ok: true, message: 'dispatched' })
    const successSpy = vi.spyOn(useToastStore(), 'success')
    const { wrapper, store } = await mountTab({ config: makeSysmonConfig() })

    await wrapper.find('[data-test="sysmon-config-input-measure_interval_ms"]').setValue('500')
    expect(wrapper.find('[data-test="sysmon-config-save"]').attributes('disabled')).toBeUndefined()

    await wrapper.find('[data-test="sysmon-config-save"]').trigger('click')
    await flushPromises()

    // 深度 diff：只带改动字段，其余段/键不出现（RFC 7386 merge patch）
    expect(sysmonApi.setConfig).toHaveBeenCalledTimes(1)
    expect(sysmonApi.setConfig).toHaveBeenCalledWith({ sampling: { measure_interval_ms: 500 } })
    expect(successSpy).toHaveBeenCalledWith('配置已保存')
    // REST 只是「已下发」：pending 期间输入与保存均禁用，展示仍是生效镜像旧值（无乐观更新）
    expect(store.configPending).toBe(true)
    expect(wrapper.find('[data-test="sysmon-config-save"]').attributes('disabled')).toBeDefined()
    expect((wrapper.find('[data-test="sysmon-config-input-measure_interval_ms"]').element as HTMLInputElement).disabled)
      .toBe(true)
    expect(valueText(wrapper, 'sampling.measure_interval_ms')).toBe('1000 ms')

    // WS sysmon_config 到达：pending 清、展示更新、表单跟随镜像（差异消失 → 保存重新禁用）
    store.applyConfig(makeSysmonConfig({ sampling: { measure_interval_ms: 500 } }))
    await flushPromises()
    expect(store.configPending).toBe(false)
    expect(valueText(wrapper, 'sampling.measure_interval_ms')).toBe('500 ms')
    expect((wrapper.find('[data-test="sysmon-config-input-measure_interval_ms"]').element as HTMLInputElement).value)
      .toBe('500')
    expect(wrapper.find('[data-test="sysmon-config-save"]').attributes('disabled')).toBeDefined()
  })

  it('groups 只带改动的键（其余 7 项沿用生效值，不无谓下发）', async () => {
    vi.mocked(sysmonApi.setConfig).mockResolvedValue({ ok: true })
    const { wrapper } = await mountTab({ config: makeSysmonConfig() })

    await wrapper.find('[data-test="sysmon-config-group-cpu"]').setValue(false)
    await wrapper.find('[data-test="sysmon-config-save"]').trigger('click')
    await flushPromises()

    expect(sysmonApi.setConfig).toHaveBeenCalledWith({ sampling: { groups: { cpu: false } } })
  })

  it('多行文本 → 数组整体替换（去空白/空行）；布尔字段单独下发', async () => {
    vi.mocked(sysmonApi.setConfig).mockResolvedValue({ ok: true })
    const { wrapper } = await mountTab({ config: makeSysmonConfig() })

    await wrapper.find('[data-test="sysmon-config-input-disk.devices"]')
      .setValue('sda\n sdb \n\nnvme0n1')
    await wrapper.find('[data-test="sysmon-config-input-network.hide_virtual"]').setValue(false)
    await wrapper.find('[data-test="sysmon-config-save"]').trigger('click')
    await flushPromises()

    expect(sysmonApi.setConfig).toHaveBeenCalledWith({
      disk: { devices: ['sda', 'sdb', 'nvme0n1'] },
      network: { hide_virtual: false },
    })
  })

  it('非法数字：不下发并提示具体字段（后端范围校验由 dzsysmon 兜底，前端只挡明显非法）', async () => {
    vi.mocked(sysmonApi.setConfig).mockResolvedValue({ ok: true })
    const errorSpy = vi.spyOn(useToastStore(), 'error')
    const { wrapper } = await mountTab({ config: makeSysmonConfig() })

    await wrapper.find('[data-test="sysmon-config-input-measure_interval_ms"]').setValue('')
    await wrapper.find('[data-test="sysmon-config-save"]').trigger('click')
    await flushPromises()

    expect(sysmonApi.setConfig).not.toHaveBeenCalled()
    expect(errorSpy.mock.calls[0][0]).toContain('计量间隔')
  })

  it('HTTP 失败：给出可见失败提示，pending 立即清除', async () => {
    vi.mocked(sysmonApi.setConfig).mockRejectedValue(new Error('HTTP 403'))
    const errorSpy = vi.spyOn(useToastStore(), 'error')
    const { wrapper, store } = await mountTab({ config: makeSysmonConfig() })

    await wrapper.find('[data-test="sysmon-config-input-measure_interval_ms"]').setValue('500')
    await wrapper.find('[data-test="sysmon-config-save"]').trigger('click')
    await flushPromises()

    expect(store.configPending).toBe(false)
    expect(errorSpy).toHaveBeenCalledWith('系统监控配置保存下发失败')
  })

  it('校验失败回推旧值：表单跟随镜像重置，不保留未生效的草稿', async () => {
    // SET 已被 dzweb 受理（HTTP 200 = 已写事件通道），但 dzsysmon 校验失败，按四件套
    // 回推**旧值**（契约 sysmon §3.3）——这条路径与上面的 HTTP 失败不同：pending 要等
    // WS `sysmon_config`（旧值）到达才清，表单必须跟随镜像回退而不是保留草稿。
    vi.mocked(sysmonApi.setConfig).mockResolvedValue({ ok: true, message: 'dispatched' })
    const { wrapper, store } = await mountTab({ config: makeSysmonConfig() })

    await wrapper.find('[data-test="sysmon-config-input-measure_interval_ms"]').setValue('500')
    await wrapper.find('[data-test="sysmon-config-save"]').trigger('click')
    await flushPromises()

    // REST 只表示"已下发"：pending 保持，镜像（生效值）未变。
    expect(store.configPending).toBe(true)
    expect(valueText(wrapper, 'sampling.measure_interval_ms')).toBe('1000 ms')

    // 校验失败 → RTN 回推旧值：镜像仍是 1000，pending 清除、表单重置为生效值。
    store.applyConfig(makeSysmonConfig())
    await flushPromises()

    expect(store.configPending).toBe(false)
    expect((wrapper.find('[data-test="sysmon-config-input-measure_interval_ms"]').element as HTMLInputElement).value)
      .toBe('1000')
    expect(valueText(wrapper, 'sampling.measure_interval_ms')).toBe('1000 ms')
  })
})

// #207 B1：字节输入框与摘要的 IEC 单位同屏，必须给出实时换算提示，避免错 1024 倍
describe('SysmonConfigTab 历史库上限单位换算（#207 B1）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    sysmonPending.__resetForTests()
    vi.clearAllMocks()
  })

  function hintText(wrapper: VueWrapper): string {
    const hint = wrapper.find('[data-test="sysmon-config-hint-max_bytes"]')
    expect(hint.exists(), '缺少 max_bytes 换算提示').toBe(true)
    return hint.text()
  }

  it('初值（镜像 2147483648 字节）就显示与摘要一致的 IEC 换算', async () => {
    const { wrapper } = await mountTab({ config: makeSysmonConfig() })

    expect((wrapper.find('[data-test="sysmon-config-input-max_bytes"]').element as HTMLInputElement).value)
      .toBe('2147483648')
    // 输入框是字节口径，摘要行是 2.0 GiB —— 提示必须把两者对上
    expect(hintText(wrapper)).toBe('≈ 2.0 GiB')
  })

  it('输入 2147483648 时同屏显示 2.0 GiB（实时跟随输入）', async () => {
    const { wrapper } = await mountTab({ config: makeSysmonConfig({ history: { max_bytes: 0 } }) })

    await wrapper.find('[data-test="sysmon-config-input-max_bytes"]').setValue('2147483648')
    expect(hintText(wrapper)).toBe('≈ 2.0 GiB')

    await wrapper.find('[data-test="sysmon-config-input-max_bytes"]').setValue('1048576')
    expect(hintText(wrapper)).toBe('≈ 1.0 MiB')
  })

  it('0 表示不限制（与只读摘要同一语义），不显示 0 B', async () => {
    const { wrapper } = await mountTab({ config: makeSysmonConfig({ history: { max_bytes: 0 } }) })

    expect(hintText(wrapper)).toBe('0 = 不限制')
    expect(hintText(wrapper)).not.toContain('0 B')
  })

  it('非法输入（非数字 / 负数）给出可读提示而不是换算结果', async () => {
    const { wrapper } = await mountTab({ config: makeSysmonConfig() })

    await wrapper.find('[data-test="sysmon-config-input-max_bytes"]').setValue('-1')
    expect(hintText(wrapper)).toBe('字节数不能为负')

    await wrapper.find('[data-test="sysmon-config-input-max_bytes"]').setValue('')
    expect(hintText(wrapper)).toBe('请输入有效字节数')
  })
})


// #207 D1：admin 同时有只读摘要与编辑表单 → 同批字段两套；摘要默认折叠、保存按钮 sticky
describe('SysmonConfigTab 布局收敛（#207 D1）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    sysmonPending.__resetForTests()
    vi.clearAllMocks()
  })

  it('admin：只读摘要默认折叠，编辑表单可见（不再同屏两套字段）', async () => {
    const { wrapper } = await mountTab({ config: makeSysmonConfig(), expandSummary: false })

    expect(wrapper.find('[data-test="sysmon-config-editor"]').exists()).toBe(true)
    // 默认折叠：摘要卡片不渲染，字段值自然也不渲染（避免同批字段两套）
    expect(wrapper.find('[data-test="sysmon-config-summary"]').exists()).toBe(false)
    expect(wrapper.find('[data-test="sysmon-config-section-sampling"]').exists()).toBe(false)
    const toggle = wrapper.find('[data-test="sysmon-config-summary-toggle"]')
    expect(toggle.exists()).toBe(true)
    expect(toggle.attributes('aria-expanded')).toBe('false')
    expect(toggle.text()).toBe('查看当前生效配置（只读）')
  })

  it('admin：展开摘要后可见生效值，再收起回到折叠态（入口始终可用）', async () => {
    const { wrapper } = await mountTab({ config: makeSysmonConfig(), expandSummary: false })

    const toggle = () => wrapper.find('[data-test="sysmon-config-summary-toggle"]')
    await toggle().trigger('click')
    await flushPromises()
    expect(toggle().attributes('aria-expanded')).toBe('true')
    expect(wrapper.find('[data-test="sysmon-config-summary"]').exists()).toBe(true)
    expect(valueText(wrapper, 'realtime.push_interval_sec')).toBe('5 秒')
    expect(toggle().text()).toBe('收起当前生效配置')

    await toggle().trigger('click')
    await flushPromises()
    expect(wrapper.find('[data-test="sysmon-config-summary"]').exists()).toBe(false)
  })

  it('非 admin：没有摘要折叠入口，只读摘要始终展示（只读视图不回归）', async () => {
    const { wrapper } = await mountTab({ config: makeSysmonConfig(), role: 'user', expandSummary: false })

    expect(wrapper.find('[data-test="sysmon-config-summary-toggle"]').exists()).toBe(false)
    expect(wrapper.find('[data-test="sysmon-config-summary"]').exists()).toBe(true)
    expect(valueText(wrapper, 'realtime.push_interval_sec')).toBe('5 秒')
    expect(wrapper.find('[data-test="sysmon-config-editor"]').exists()).toBe(false)
  })

  it('保存 footer 是 sticky：底部吸附 + 背景/上边框把浮起按钮条与字段分离', async () => {
    // jsdom 不做布局，sticky 的真实吸附行为无法断言；这里锁样式契约本身
    // （回归形态是「保存按钮回到文档流末尾 → 改顶部字段要滚到底才能保存」）。
    const source = readFileSync(
      join(dirname(fileURLToPath(import.meta.url)), '..', 'SysmonConfigTab.vue'),
      'utf8',
    )
    const footer = source.slice(source.indexOf('.sysmon-config__footer'))
    const rule = footer.slice(0, footer.indexOf('}'))
    expect(rule).toContain('position: sticky')
    expect(rule).toContain('bottom: 0')
    expect(rule).toContain('background:')
    expect(rule).toContain('border-top:')
    // sticky 元素必须仍在编辑卡片内、位于表单之后（结构不能被挪出滚动祖先）
    expect(source.indexOf('sysmon-config__footer')).toBeGreaterThan(source.indexOf('sysmon-config__form'))
  })
})
