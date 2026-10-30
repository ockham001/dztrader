import { describe, it, expect, vi, beforeEach } from 'vitest'
import { readFileSync } from 'node:fs'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { mount, flushPromises } from '@vue/test-utils'
import type { VueWrapper } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import SysmonConfigTab from '../SysmonConfigTab.vue'
import { useAuthStore } from '@/stores/auth'
import { sysmonPending, useSysmonStore } from '@/stores/sysmon'
import { makeSysmonConfig } from '@/test-utils/sysmonSnapshot'
import type { SysmonConfigPayload } from '@/types/generated'

// 监控配置页布局收敛（#207 D1）：保存 footer sticky + admin 只读摘要默认折叠。
// 单独成文件（而非并入 SysmonConfigTab.spec.ts）以保持本票每项「独立可回滚」。

vi.mock('@/api/sysmon', () => ({
  sysmonApi: { getSnapshot: vi.fn(), setConfig: vi.fn(), getConfig: vi.fn() },
}))

const stubs = {
  Icon: { template: '<span class="icon" />' },
}

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

/** 保持 D1 的默认折叠态挂载（不自动展开，用于验证默认布局） */
async function mountTab(options: {
  config?: SysmonConfigPayload
  role?: 'admin' | 'user'
} = {}) {
  const store = useSysmonStore()
  if (options.config) store.applyConfig(options.config)
  setRole(options.role ?? 'admin')
  const wrapper = mount(SysmonConfigTab, { global: { stubs } })
  await flushPromises()
  return { wrapper, store }
}

async function expandSummary(wrapper: VueWrapper): Promise<void> {
  const toggle = wrapper.find('[data-test="sysmon-config-summary-toggle"]')
  if (toggle.exists() && toggle.attributes('aria-expanded') === 'false') {
    await toggle.trigger('click')
    await flushPromises()
  }
}

function valueText(wrapper: VueWrapper, field: string): string {
  return wrapper.find(`[data-test="sysmon-config-value-${field}"]`).text()
}

describe('SysmonConfigTab 布局收敛（#207 D1）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
    sysmonPending.__resetForTests()
    vi.clearAllMocks()
  })

  it('admin：只读摘要默认折叠，编辑表单可见（不再同屏两套字段）', async () => {
    const { wrapper } = await mountTab({ config: makeSysmonConfig() })

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
    const { wrapper } = await mountTab({ config: makeSysmonConfig() })

    const toggle = () => wrapper.find('[data-test="sysmon-config-summary-toggle"]')
    await expandSummary(wrapper)
    expect(toggle().attributes('aria-expanded')).toBe('true')
    expect(wrapper.find('[data-test="sysmon-config-summary"]').exists()).toBe(true)
    expect(valueText(wrapper, 'realtime.push_interval_sec')).toBe('5 秒')
    expect(toggle().text()).toBe('收起当前生效配置')

    await toggle().trigger('click')
    await flushPromises()
    expect(wrapper.find('[data-test="sysmon-config-summary"]').exists()).toBe(false)
  })

  it('非 admin：没有摘要折叠入口，只读摘要始终展示（只读视图不回归）', async () => {
    const { wrapper } = await mountTab({ config: makeSysmonConfig(), role: 'user' })

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
