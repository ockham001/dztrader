import { describe, it, expect, vi, beforeEach } from 'vitest'
import { nextTick } from 'vue'
import { mount, flushPromises } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import CtpCard from '../CtpCard.vue'
import { useMarketSourcesStore } from '@/stores/marketSources'
import { makeSource } from '@/composables/marketSourceView'
import type { BrokerEntry } from '@/types/api'

const brokerA: BrokerEntry = { name: 'A', broker_id: '1', user_id: 'u', password: '****', product_info: '', frontends: [] }
const brokerB: BrokerEntry = { name: 'B', broker_id: '2', user_id: 'u', password: '****', product_info: '', frontends: [] }

// 全量 stub 子组件, 只验证经纪商折叠/展开编排 + 添加经纪商 Modal
const stubs = {
  Icon: { template: '<span class="icon" />' },
  Modal: { props: ['open', 'title'], template: '<div v-if="open" class="modal-stub"><slot /><slot name="footer" /></div>' },
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

describe('CtpCard 卡片 header 上次失败展示（契约 md-status 前端义务②）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
  })

  it('有 lastLoginError 时 header 展示「上次失败：<原因>（时间）」', async () => {
    const timestamp = 1_700_000_000 // 绝对 Unix 秒
    const src = makeSource({
      id: 1,
      source_type: 'CTP',
      source_name: 'dzmd_ctp',
      display_name: 'CTP',
      lastLoginError: { code: 'login_rejected', message: 'CTP:用户密码错误', errorId: 3, timestamp },
    })
    const w = mount(CtpCard, { props: { source: src }, global: { stubs } })
    await nextTick()
    const timeText = new Date(timestamp * 1000).toLocaleTimeString('zh-CN', { hour12: false })
    expect(w.text()).toContain(`上次失败：CTP:用户密码错误（${timeText}）`)
    // 展示层不得出现 snake_case 机器码
    expect(w.text()).not.toContain('login_rejected')
  })

  it('message 为空时按 code 映射中文（不得展示 machine code）', async () => {
    const src = makeSource({
      id: 1,
      source_type: 'CTP',
      source_name: 'dzmd_ctp',
      display_name: 'CTP',
      lastLoginError: { code: 'connect_timeout', message: '', errorId: 0, timestamp: 1_700_000_000 },
    })
    const w = mount(CtpCard, { props: { source: src }, global: { stubs } })
    await nextTick()
    expect(w.text()).toContain('连接超时')
    expect(w.text()).not.toContain('connect_timeout')
  })

  it('无 lastLoginError 时不展示上次失败', async () => {
    const src = makeSource({ id: 1, source_type: 'CTP', source_name: 'dzmd_ctp', display_name: 'CTP' })
    const w = mount(CtpCard, { props: { source: src }, global: { stubs } })
    await nextTick()
    expect(w.text()).not.toContain('上次失败')
  })
})

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

  it('添加经纪商：名称为空时确认禁用，填写后可提交', async () => {
    const src = sourceWith([], null)
    const w = mount(CtpCard, { props: { source: src }, global: { stubs } })
    await nextTick()
    await w.findAll('button').find(b => b.text() === '添加经纪商')!.trigger('click')

    const nameInput = w.find('input[placeholder*="创建后不可修改"]')
    expect(nameInput.exists()).toBe(true)
    const confirm = () => w.findAll('button').find(b => b.text() === '确认')!
    expect(confirm().attributes('disabled')).toBeDefined()

    await nameInput.setValue('上海中期')
    expect(confirm().attributes('disabled')).toBeUndefined()
  })

  it('添加经纪商成功：按表单值下发并关闭 Modal', async () => {
    const store = useMarketSourcesStore()
    const spy = vi.spyOn(store, 'addBroker').mockResolvedValue()
    const src = sourceWith([], null)
    const w = mount(CtpCard, { props: { source: src }, global: { stubs } })
    await nextTick()
    await w.findAll('button').find(b => b.text() === '添加经纪商')!.trigger('click')

    await w.find('input[placeholder*="创建后不可修改"]').setValue(' 上海中期 ')
    await w.find('input[placeholder="经纪商编号"]').setValue('0001')
    await w.find('input[placeholder="用户代码"]').setValue('u1')
    await w.find('input[type="password"]').setValue('pw')
    await w.find('input[placeholder*="客户端产品信息"]').setValue('pi')
    await w.findAll('button').find(b => b.text() === '确认')!.trigger('click')
    await flushPromises()

    expect(spy).toHaveBeenCalledWith(1, {
      name: '上海中期',
      broker_id: '0001',
      user_id: 'u1',
      password: 'pw',
      product_info: 'pi',
    })
    expect(w.find('input[placeholder*="创建后不可修改"]').exists()).toBe(false)
  })

  it('添加经纪商失败：保持 Modal 打开并可重试', async () => {
    const store = useMarketSourcesStore()
    const spy = vi.spyOn(store, 'addBroker').mockRejectedValue(new Error('add broker failed'))
    const src = sourceWith([], null)
    const w = mount(CtpCard, { props: { source: src }, global: { stubs } })
    await nextTick()
    await w.findAll('button').find(b => b.text() === '添加经纪商')!.trigger('click')
    await w.find('input[placeholder*="创建后不可修改"]').setValue('上海中期')

    await w.findAll('button').find(b => b.text() === '确认')!.trigger('click')
    await flushPromises()

    expect(spy).toHaveBeenCalledTimes(1)
    expect(w.find('input[placeholder*="创建后不可修改"]').exists()).toBe(true)
    expect(w.findAll('button').find(b => b.text() === '确认')!.attributes('disabled')).toBeUndefined()
  })
})
