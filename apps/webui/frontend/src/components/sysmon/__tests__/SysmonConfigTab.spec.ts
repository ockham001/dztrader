import { describe, it, expect, vi, beforeEach } from 'vitest'
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

async function mountTab(options: { config?: SysmonConfigPayload; role?: 'admin' | 'user' } = {}) {
  const store = useSysmonStore()
  if (options.config) store.applyConfig(options.config)
  setRole(options.role ?? 'admin')
  const wrapper = mount(SysmonConfigTab, { global: { stubs } })
  await flushPromises()
  return { wrapper, store }
}

function valueText(wrapper: VueWrapper, field: string): string {
  return wrapper.find(`[data-test="sysmon-config-value-${field}"]`).text()
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
        disk: { watch_paths: ['/data', '/var/lib'], devices: ['sda'] },
      }),
    })

    // 六个段都在
    for (const section of ['sampling', 'disk', 'network', 'history', 'realtime', 'alerts']) {
      expect(wrapper.find(`[data-test="sysmon-config-section-${section}"]`).exists()).toBe(true)
    }
    // 数字 + 单位
    expect(valueText(wrapper, 'sampling.measure_interval_ms')).toBe('1000 ms')
    expect(valueText(wrapper, 'sampling.record_interval_sec')).toBe('60 秒')
    expect(valueText(wrapper, 'history.raw_retention_hours')).toBe('24 小时')
    expect(valueText(wrapper, 'history.max_retention_days')).toBe('30 天')
    expect(valueText(wrapper, 'history.max_bytes')).toBe('2.0 GiB')
    expect(valueText(wrapper, 'realtime.push_interval_sec')).toBe('5 秒')
    expect(valueText(wrapper, 'alerts.cpu_sustain_used_pct')).toBe('90%')
    expect(valueText(wrapper, 'alerts.cpu_sustain_samples')).toBe('10 次')
    expect(valueText(wrapper, 'alerts.cooldown_sec')).toBe('600 秒')
    // 布尔语义化
    expect(valueText(wrapper, 'network.hide_virtual')).toBe('已启用')
    expect(valueText(wrapper, 'history.enabled')).toBe('已启用')
    // groups 8 项逐项展示
    const groupKeys = ['cpu', 'memory', 'swap', 'volumes', 'disk_io', 'network', 'temperature', 'cpu_frequency']
    for (const key of groupKeys) {
      expect(wrapper.find(`[data-test="sysmon-config-value-sampling.groups.${key}"]`).exists()).toBe(true)
    }
    expect(valueText(wrapper, 'sampling.groups.cpu')).toBe('已启用')
    // watch_paths / devices 逐条
    const paths = wrapper.findAll('[data-test="sysmon-config-value-disk.watch_paths"] .sysmon-config__list-item')
    expect(paths.map(item => item.text())).toEqual(['/data', '/var/lib'])
    expect(wrapper.find('[data-test="sysmon-config-value-disk.devices"]').text()).toBe('sda')
  })

  it('空数组字段按契约语义展示（watch_paths 默认路径 / devices 全部物理盘），不留空值', async () => {
    const { wrapper, store } = await mountTab({ config: makeSysmonConfig() })

    expect(valueText(wrapper, 'disk.watch_paths')).toContain('默认关注路径')
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
    const { wrapper } = await mountTab({ config: makeSysmonConfig({ disk: { watch_paths: ['/data'] } }) })

    await wrapper.find('[data-test="sysmon-config-input-disk.watch_paths"]')
      .setValue('/data\n /var/lib \n\n/logs')
    await wrapper.find('[data-test="sysmon-config-input-network.hide_virtual"]').setValue(false)
    await wrapper.find('[data-test="sysmon-config-save"]').trigger('click')
    await flushPromises()

    expect(sysmonApi.setConfig).toHaveBeenCalledWith({
      disk: { watch_paths: ['/data', '/var/lib', '/logs'] },
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
    // dzsysmon 校验失败会经 WS 回推旧值（契约 sysmon §3.3 四件套）——镜像到达即重置表单，
    // 否则用户会以为自己改的值已经生效。
    vi.mocked(sysmonApi.setConfig).mockRejectedValue(new Error('HTTP 500'))
    const { wrapper, store } = await mountTab({ config: makeSysmonConfig() })

    await wrapper.find('[data-test="sysmon-config-input-measure_interval_ms"]').setValue('500')
    await wrapper.find('[data-test="sysmon-config-save"]').trigger('click')
    await flushPromises()

    store.applyConfig(makeSysmonConfig())
    await flushPromises()

    expect((wrapper.find('[data-test="sysmon-config-input-measure_interval_ms"]').element as HTMLInputElement).value)
      .toBe('1000')
    expect(valueText(wrapper, 'sampling.measure_interval_ms')).toBe('1000 ms')
  })
})
