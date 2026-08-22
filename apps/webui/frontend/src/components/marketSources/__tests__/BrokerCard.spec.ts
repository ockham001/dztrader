import { describe, it, expect, vi, beforeEach } from 'vitest'
import { mount } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import BrokerCard from '../BrokerCard.vue'
import { useMarketSourcesStore } from '@/stores/marketSources'
import { makeSource } from '@/composables/marketSourceView'
import type { BrokerEntry } from '@/types/api'

// 最小 stub: Icon/Modal/FrontendTable（FrontendTable 用轻量占位避免依赖表格内部 Modal）
const stubs = {
  Icon: { template: '<span class="icon" />' },
  Modal: { props: ['open'], template: '<div v-if="open"><slot /><slot name="footer" /></div>' },
  FrontendTable: { props: ['source', 'broker'], template: '<div class="fe-stub">前置</div>' },
}

const brokerA: BrokerEntry = {
  name: 'A券商',
  broker_id: '0001',
  user_id: 'u1',
  password: '****',
  product_info: '',
  frontends: [{ address: 'tcp://1.2.3.4:10211', label: '电信', enabled: true }],
}

function sourceWith(brokers: BrokerEntry[], current: string | null) {
  return makeSource({
    id: 1,
    source_type: 'CTP',
    source_name: 'dzmd_ctp',
    display_name: 'CTP',
    brokers,
    selectedBrokerId: current,
    process_state: 'Running', // 模拟进程运行+未登录(loginState=offline), radio 等操作可用(isStateIdle=true)
  })
}

describe('BrokerCard', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
  })

  it('expanded=true 渲染前置表格与登录字段, expanded=false 只显示折叠摘要', () => {
    const src = sourceWith([brokerA], 'A券商')
    const expanded = mount(BrokerCard, { props: { source: src, broker: brokerA, expanded: true }, global: { stubs } })
    expect(expanded.find('.fe-stub').exists()).toBe(true)
    expect(expanded.text()).toContain('登录字段')

    const collapsed = mount(BrokerCard, { props: { source: src, broker: brokerA, expanded: false }, global: { stubs } })
    expect(collapsed.find('.fe-stub').exists()).toBe(false)
    expect(collapsed.text()).toContain('1 个前置')
  })

  it('点击折叠行头部(非 radio 区域)触发 toggle 事件', async () => {
    const src = sourceWith([brokerA], 'A券商')
    const w = mount(BrokerCard, { props: { source: src, broker: brokerA, expanded: false }, global: { stubs } })
    await w.find('.broker-card__header').trigger('click')
    expect(w.emitted('toggle')).toHaveLength(1)
  })

  it('radio 选中状态反映 selectedBrokerId, 且当前经纪商无 is-current 类', () => {
    const src = sourceWith([brokerA], 'A券商')
    const current = mount(BrokerCard, { props: { source: src, broker: brokerA, expanded: true }, global: { stubs } })
    expect((current.find('input[type="radio"]').element as HTMLInputElement).checked).toBe(true)
    expect(current.find('.broker-card').classes()).not.toContain('is-current')

    const src2 = sourceWith([brokerA], null)
    const nonCurrent = mount(BrokerCard, { props: { source: src2, broker: brokerA, expanded: false }, global: { stubs } })
    expect((nonCurrent.find('input[type="radio"]').element as HTMLInputElement).checked).toBe(false)
  })

  it('点击 radio 触发 store.selectBroker 且不触发 toggle', async () => {
    const store = useMarketSourcesStore()
    const spy = vi.spyOn(store, 'selectBroker').mockResolvedValue()
    const src = sourceWith([brokerA], null)
    const w = mount(BrokerCard, { props: { source: src, broker: brokerA, expanded: false }, global: { stubs } })
    await w.find('input[type="radio"]').setValue(true)
    expect(spy).toHaveBeenCalledWith(1, 'A券商')
    expect(w.emitted('toggle')).toBeUndefined()
  })

  it('切换中(brokerSelectPending) radio 禁用防重入', () => {
    const src = sourceWith([brokerA], null)
    const w = mount(BrokerCard, {
      props: { source: { ...src, brokerSelectPending: true }, broker: brokerA, expanded: false },
      global: { stubs },
    })
    expect((w.find('input[type="radio"]').element as HTMLInputElement).disabled).toBe(true)
  })

  it('非乐观强制回滚: 点击 radio 后 DOM 立即回到镜像值, 不发请求前不显示选中', async () => {
    const store = useMarketSourcesStore()
    const spy = vi.spyOn(store, 'selectBroker').mockResolvedValue()
    const src = sourceWith([brokerA], null)  // 镜像未选中 A
    const w = mount(BrokerCard, { props: { source: src, broker: brokerA, expanded: false }, global: { stubs } })
    // 点击 radio: 浏览器置 checked=true 派发 change; handler 手动回滚到镜像(null) → false
    await w.find('input[type="radio"]').setValue(true)
    expect((w.find('input[type="radio"]').element as HTMLInputElement).checked).toBe(false)
    expect(spy).toHaveBeenCalledWith(1, 'A券商')
  })

  it('切换中(brokerSelectPending): label 带 is-pending class 且 radio 禁用', () => {
    const src = sourceWith([brokerA], null)
    const w = mount(BrokerCard, {
      props: { source: { ...src, brokerSelectPending: true }, broker: brokerA, expanded: false },
      global: { stubs },
    })
    expect(w.find('.broker-card__radio').classes()).toContain('is-pending')
    expect((w.find('input[type="radio"]').element as HTMLInputElement).disabled).toBe(true)
  })
})
