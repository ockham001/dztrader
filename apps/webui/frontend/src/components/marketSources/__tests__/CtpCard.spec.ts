import { describe, it, expect, beforeEach } from 'vitest'
import { nextTick } from 'vue'
import { mount } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import CtpCard from '../CtpCard.vue'
import { makeSource } from '@/composables/marketSourceView'
import type { BrokerEntry } from '@/types/api'

const brokerA: BrokerEntry = { name: 'A', broker_id: '1', user_id: 'u', password: '****', product_info: '', frontends: [] }
const brokerB: BrokerEntry = { name: 'B', broker_id: '2', user_id: 'u', password: '****', product_info: '', frontends: [] }

// 全量 stub 子组件, 只验证经纪商折叠/展开编排
const stubs = {
  Icon: { template: '<span class="icon" />' },
  Modal: { template: '<div class="modal-stub" />' },
  TimePicker: { template: '<span />' },
  StatusIndicator: { template: '<span class="status" />' },
  LoginPanel: { template: '<div class="lp" />' },
  ScheduleManager: { template: '<div class="sm" />' },
  BrokerCard: {
    props: ['source', 'broker', 'expanded'],
    emits: ['toggle'],
    template: '<div class="broker-stub" @click="$emit(\'toggle\')">{{ broker.name }}:{{ expanded ? \'open\' : \'closed\' }}</div>',
  },
}

function sourceWith(brokers: BrokerEntry[], current: string | null) {
  return makeSource({
    id: 1,
    source_type: 'CTP',
    source_name: 'dzmd_ctp',
    display_name: 'CTP',
    brokers,
    selectedBrokerId: current,
    expanded: true, // 展开卡片体, 使经纪商区块渲染
    process_state: 'Running', // 模拟进程运行+未登录, radio 等操作可用(isStateIdle=true)
  })
}

describe('CtpCard 经纪商折叠', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
  })

  it('默认全部折叠(无论是否有当前选中)', async () => {
    const src = sourceWith([brokerA, brokerB], 'B')
    const w = mount(CtpCard, { props: { source: src }, global: { stubs } })
    await nextTick()
    const cards = w.findAll('.broker-stub')
    expect(cards[0].text()).toBe('A:closed')
    expect(cards[1].text()).toBe('B:closed')
  })

  it('点击折叠行切换展开(手风琴: 同一时刻只有一个展开)', async () => {
    const src = sourceWith([brokerA, brokerB], 'B')
    const w = mount(CtpCard, { props: { source: src }, global: { stubs } })
    await nextTick()
    await w.findAll('.broker-stub')[0].trigger('click')
    const cards = w.findAll('.broker-stub')
    expect(cards[0].text()).toBe('A:open')
    expect(cards[1].text()).toBe('B:closed')
    // 展开另一张 → 手风琴收起前一张
    await w.findAll('.broker-stub')[1].trigger('click')
    const cards2 = w.findAll('.broker-stub')
    expect(cards2[0].text()).toBe('A:closed')
    expect(cards2[1].text()).toBe('B:open')
  })

  it('点击已展开的卡片头部可收起(回到全部折叠)', async () => {
    const src = sourceWith([brokerA, brokerB], 'B')
    const w = mount(CtpCard, { props: { source: src }, global: { stubs } })
    await nextTick()
    await w.findAll('.broker-stub')[0].trigger('click')
    expect(w.findAll('.broker-stub')[0].text()).toBe('A:open')
    await w.findAll('.broker-stub')[0].trigger('click')
    const cards = w.findAll('.broker-stub')
    expect(cards[0].text()).toBe('A:closed')
    expect(cards[1].text()).toBe('B:closed')
  })
})
