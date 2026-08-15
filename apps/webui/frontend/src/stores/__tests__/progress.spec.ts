import { describe, it, expect, beforeEach } from 'vitest'
import { setActivePinia, createPinia } from 'pinia'
import { useProgressStore } from '../progress'

describe('useProgressStore', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
  })

  it('applyProgress 写入完整条目', () => {
    const store = useProgressStore()
    store.applyProgress('dzmd_ctp', { min: 0, max: 4, current: 2, desc: '订阅合约中' })
    expect(store.progress['dzmd_ctp']).toEqual({ min: 0, max: 4, current: 2, desc: '订阅合约中' })
  })

  // 契约 05：单条完整状态，后到覆盖先到（对应后端 ProgressDomainServiceTest.ProgressOverwrites）
  it('applyProgress 后到完整覆盖先到（含 max 归 0 / desc 清空）', () => {
    const store = useProgressStore()
    store.applyProgress('dzmd_ctp', { min: 0, max: 4, current: 2, desc: '订阅合约中' })
    store.applyProgress('dzmd_ctp', { min: 0, max: 0, current: 0, desc: '' })
    expect(store.progress['dzmd_ctp']).toEqual({ min: 0, max: 0, current: 0, desc: '' })
  })

  it('不同 instanceId 互不影响', () => {
    const store = useProgressStore()
    store.applyProgress('dzmd_ctp', { min: 0, max: 4, current: 2 })
    store.applyProgress('dzmd_other', { min: 0, max: 2, current: 1 })
    expect(store.progress['dzmd_ctp']).toEqual({ min: 0, max: 4, current: 2, desc: undefined })
    expect(store.progress['dzmd_other']).toEqual({ min: 0, max: 2, current: 1, desc: undefined })
    expect(Object.keys(store.progress)).toHaveLength(2)
  })

  it('空 instanceId 忽略', () => {
    const store = useProgressStore()
    store.applyProgress('', { min: 0, max: 1, current: 0 })
    expect(Object.keys(store.progress)).toHaveLength(0)
  })

  it('非对象 payload 忽略（不写不抛）', () => {
    const store = useProgressStore()
    store.applyProgress('dzmd_ctp', 'not-an-object')
    store.applyProgress('dzmd_ctp', null)
    store.applyProgress('dzmd_ctp', undefined)
    expect(Object.keys(store.progress)).toHaveLength(0)
  })

  it('字段类型防御：非 number 归 0、非 string desc 置 undefined', () => {
    const store = useProgressStore()
    store.applyProgress('dzmd_ctp', { min: 'a', max: null, current: 3, desc: 42 })
    expect(store.progress['dzmd_ctp']).toEqual({ min: 0, max: 0, current: 3, desc: undefined })
  })
})
