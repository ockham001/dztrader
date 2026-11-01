// #233 验收第 3 条：同页多实例（v-for 字段、每经纪商的登录字段、多用户弹窗）的 id 不重复。
//
// 为什么必须是测试而不是只靠 eslint：lint 只看单个文件的静态结构（「label 有 for」+
// 「控件有 id」各自存在即可），看不到运行期的 id 唯一性——而撞 id 会让 <label for> 只关联
// 文档里第一个元素，屏幕阅读器报出错误名称，这正是本票要根治的失败形态。
//
// 本文件只验证 useFieldId 这一**生成机制**的语义（单组件内 / 多实例 / v-for / 多经纪商）。
// 组件级的「for 在渲染后 DOM 里真的解析到唯一元素」「控件可被报出名称」由各组件自己的
// spec 断言（ChangePasswordModal.spec.ts / LogViewerTab.spec.ts / SyncSwitch.spec.ts），
// 避免同一能力两处维护。
//
// 说明：本文件是 .ts 测试，不是 SFC；下面用最小探针组件（defineComponent）验证 id 语义，
// `vue/one-component-per-file` 对测试探针无意义。
/* eslint-disable vue/one-component-per-file */
import { describe, it, expect } from 'vitest'
import { mount } from '@vue/test-utils'
import { defineComponent, h } from 'vue'
import { useFieldId } from '@/composables/useFieldId'

/** 取容器内所有 id 属性值（空串不算），用于唯一性断言 */
function collectIds(root: Element): string[] {
  const ids: string[] = []
  root.querySelectorAll('[id]').forEach((el) => {
    const id = el.getAttribute('id')
    if (id) ids.push(id)
  })
  return ids
}

describe('useFieldId', () => {
  it('同一组件内不同 slug 生成不同 id，同一 slug 稳定', () => {
    const Probe = defineComponent({
      setup() {
        const fieldId = useFieldId()
        return () => h('div', [
          h('span', { id: fieldId('a') }),
          h('span', { id: fieldId('b') }),
          h('span', { id: fieldId('a') }),
        ])
      },
    })
    const wrapper = mount(Probe)
    const ids = collectIds(wrapper.element as Element)
    // a / b 不同，a 的两次调用相同 → 去重后 2 个
    expect(new Set(ids).size).toBe(2)
    expect(ids[0]).toBe(ids[2])
    expect(ids[0]).not.toBe(ids[1])
  })

  it('同一页面上多个实例各自生成不冲突的 id 前缀', () => {
    const Probe = defineComponent({
      setup() {
        const fieldId = useFieldId()
        return () => h('span', { id: fieldId('same-slug') })
      },
    })
    const Host = defineComponent({
      render: () => h('div', [h(Probe), h(Probe), h(Probe)]),
    })
    const wrapper = mount(Host)
    const ids = collectIds(wrapper.element as Element)
    expect(ids).toHaveLength(3)
    // 三个实例用同一个 slug，id 仍必须互不相同——这就是改为 useId 前缀的原因
    expect(new Set(ids).size).toBe(3)
  })

  it('v-for 场景：slug 拼循环键后各行 id 互不相同（不得用 index 之外的共享 slug）', () => {
    const ROWS = [{ key: '08:45' }, { key: '09:00' }, { key: '10:30' }]
    const FieldList = defineComponent({
      setup() {
        const fieldId = useFieldId()
        return () => h('div', ROWS.map(r => h('input', { key: r.key, id: fieldId(`preload-${r.key}-pages`) })))
      },
    })
    const wrapper = mount(FieldList)
    const ids = collectIds(wrapper.element as Element)
    expect(ids).toHaveLength(ROWS.length)
    expect(new Set(ids).size).toBe(ROWS.length)
  })

  it('多经纪商场景：同一 slug 在各实例内独立，合起来全文档唯一', () => {
    // 对应 CtpCard 的 v-for="broker" → BrokerCard（每个经纪商一份登录字段，slug 相同）
    const BrokerCardLike = defineComponent({
      setup() {
        const fieldId = useFieldId()
        return () => h('div', ['broker-id', 'user-id', 'password', 'product-info']
          .map(slug => h('input', { key: slug, id: fieldId(slug) })))
      },
    })
    const Host = defineComponent({
      render: () => h('div', [h(BrokerCardLike), h(BrokerCardLike), h(BrokerCardLike)]),
    })
    const wrapper = mount(Host)
    const ids = collectIds(wrapper.element as Element)
    expect(ids).toHaveLength(12) // 3 经纪商 × 4 字段
    expect(new Set(ids).size).toBe(12)
  })
})
