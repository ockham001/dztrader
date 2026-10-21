import { describe, it, expect, vi, beforeEach } from 'vitest'
import { mount, flushPromises } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import FrontendTable from '../FrontendTable.vue'
import { useMarketSourcesStore } from '@/stores/marketSources'
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

  it('确认删除后下发 removeFrontend 并关闭 Modal', async () => {
    const store = useMarketSourcesStore()
    const spy = vi.spyOn(store, 'removeFrontend').mockResolvedValue()
    const src = sourceWith()
    const w = mount(FrontendTable, { props: { source: src, broker }, global: { stubs } })

    await w.findAll('button.ds-btn--danger-icon')[0].trigger('click')
    await w.findAll('button').find(b => b.text() === '删除')!.trigger('click')
    await flushPromises()

    expect(spy).toHaveBeenCalledWith(1, 'A券商', 'tcp://1.2.3.4:10211')
    expect(w.text()).not.toContain('确认删除前置地址')
  })

  it('勾选/取消启用前置下发 setFrontendEnabled（含地址与原值）', async () => {
    const store = useMarketSourcesStore()
    const spy = vi.spyOn(store, 'setFrontendEnabled').mockResolvedValue()
    const src = sourceWith()
    const w = mount(FrontendTable, { props: { source: src, broker }, global: { stubs } })

    const boxes = w.findAll('input[type="checkbox"]')
    await boxes[0].setValue(false)
    expect(spy).toHaveBeenCalledWith(1, 'A券商', 'tcp://1.2.3.4:10211', false)

    await boxes[1].setValue(true)
    expect(spy).toHaveBeenLastCalledWith(1, 'A券商', 'tcp://5.6.7.8:10211', true)
  })

  it('地址失焦且值改变时下发 editFrontend', async () => {
    const store = useMarketSourcesStore()
    const spy = vi.spyOn(store, 'editFrontend').mockResolvedValue()
    const src = sourceWith()
    const w = mount(FrontendTable, { props: { source: src, broker }, global: { stubs } })

    const address = w.findAll('.fe-address-input')[0]
    await address.setValue('tcp://1.2.3.4:10212')
    await address.trigger('blur')

    expect(spy).toHaveBeenCalledWith(
      1, 'A券商', 'tcp://1.2.3.4:10211', 'tcp://1.2.3.4:10212',
    )
  })

  it('地址失焦但值未改变时不下发 editFrontend', async () => {
    const store = useMarketSourcesStore()
    const spy = vi.spyOn(store, 'editFrontend').mockResolvedValue()
    const src = sourceWith()
    const w = mount(FrontendTable, { props: { source: src, broker }, global: { stubs } })

    const address = w.findAll('.fe-address-input')[0]
    await address.setValue('tcp://1.2.3.4:10211')
    await address.trigger('blur')

    expect(spy).not.toHaveBeenCalled()
  })

  it('添加前置：地址必填，成功后下发并关闭 Modal', async () => {
    const store = useMarketSourcesStore()
    const spy = vi.spyOn(store, 'addFrontend').mockResolvedValue()
    const src = sourceWith()
    const w = mount(FrontendTable, { props: { source: src, broker }, global: { stubs } })

    await w.findAll('button').find(b => b.text() === '添加前置')!.trigger('click')
    const address = w.find('input[placeholder*="tcp://"]')
    expect(address.exists()).toBe(true)
    expect(w.findAll('button').find(b => b.text() === '添加')!.attributes('disabled')).toBeDefined()

    await address.setValue('tcp://9.9.9.9:10211')
    await w.find('input[placeholder*="电信线路"]').setValue('备用')
    await w.findAll('button').find(b => b.text() === '添加')!.trigger('click')
    await flushPromises()

    expect(spy).toHaveBeenCalledWith(1, 'A券商', 'tcp://9.9.9.9:10211', '备用')
    expect(w.find('input[placeholder*="tcp://"]').exists()).toBe(false)
  })
})
