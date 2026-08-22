import { describe, it, expect, beforeEach } from 'vitest'
import { mount } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import FrontendTable from '../FrontendTable.vue'
import { makeSource } from '@/composables/marketSourceView'
import type { MarketSourceView } from '@/stores/marketSources'
import type { BrokerEntry } from '@/types/api'

// 最小 stub: Icon（占位 span）; Modal（模拟 v-if="open", 渲染 body + footer slot）
const stubs = {
  Icon: { template: '<span class="icon" />' },
  Modal: { props: ['open'], template: '<div v-if="open"><slot /><slot name="footer" /></div>' },
}

const broker: BrokerEntry = {
  name: 'A券商',
  broker_id: '0001',
  user_id: 'u1',
  password: '****',
  product_info: '',
  frontends: [
    { address: 'tcp://1.2.3.4:10211', label: '电信', enabled: true },
    { address: 'tcp://5.6.7.8:10211', label: '联通', enabled: false },
  ],
}

// patch 用 Partial<MarketSourceView> 保证 makeSource(Partial) 参数类型兼容（type-check 门禁）
function sourceWith(patch: Partial<MarketSourceView> = {}) {
  return makeSource({
    id: 1,
    source_type: 'CTP',
    source_name: 'dzmd_ctp',
    display_name: 'CTP',
    brokers: [broker],
    selectedBrokerId: null,
    process_state: 'Running',  // 运行+未登录 → isStateIdle=true, 删除可用
    ...patch,
  })
}

describe('FrontendTable', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
  })

  it('每行渲染一个垃圾桶图标删除按钮, 默认无 spinner', () => {
    const src = sourceWith()
    const w = mount(FrontendTable, { props: { source: src, broker }, global: { stubs } })
    expect(w.findAll('button.ds-btn--danger-icon')).toHaveLength(2)
    expect(w.findAll('.ds-btn__spinner')).toHaveLength(0)
  })

  it('点击删除按钮打开二次确认 Modal（含地址文案）', async () => {
    const src = sourceWith()
    const w = mount(FrontendTable, { props: { source: src, broker }, global: { stubs } })
    await w.findAll('button.ds-btn--danger-icon')[0].trigger('click')
    expect(w.text()).toContain('确认删除前置地址')
    expect(w.text()).toContain('tcp://1.2.3.4:10211')
  })

  it('frontendRemovePending 时按钮 disabled 且图标位变 spinner', () => {
    const src = sourceWith({ frontendRemovePending: true })
    const w = mount(FrontendTable, { props: { source: src, broker }, global: { stubs } })
    const btn = w.findAll('button.ds-btn--danger-icon')[0]
    expect((btn.element as HTMLButtonElement).disabled).toBe(true)
    expect(btn.find('.ds-btn__spinner').exists()).toBe(true)
  })
})
