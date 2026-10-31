// #233 验收第 2/3 条：表单控件与 <label> 的关联真实成立，且同页多实例的 id 不重复。
//
// 为什么必须是测试而不是只靠 eslint：lint 只能看单个文件里的静态结构（「label 有 for」+
// 「控件有 id」各自存在即可），看不到运行期三件事——
//   1. for 指向的 id 在**渲染后的 DOM 里真的有对应元素**（写错 slug、拼错前缀都过 lint）；
//   2. 同页多实例（多个经纪商 / 多个弹窗 / v-for 字段）的 id **不撞**（撞了 for 只关联第一个，
//      读屏报出错误名称，而这正是本票要根治的失败形态）；
//   3. 每个控件都能被**报出名称**（nesting 能满足 lint，但 label 内无可读文字时读屏仍报不出）。
//
// 说明：本文件是 .ts 测试，不是 SFC；下面用若干最小探针组件（defineComponent）验证
// useFieldId 的 id 生成语义，`vue/one-component-per-file` 对测试探针无意义。
/* eslint-disable vue/one-component-per-file */
// 覆盖范围取本票改动的四类代表场景，不逐个组件复刻（组件内具体 slug 由各自 spec 断言）：
import { describe, it, expect, beforeEach, vi } from 'vitest'
import { mount, flushPromises } from '@vue/test-utils'
import { createPinia, setActivePinia } from 'pinia'
import ChangePasswordModal from '@/components/shared/ChangePasswordModal.vue'
import SyncSwitch from '@/components/shared/SyncSwitch.vue'
import { useFieldId } from '@/composables/useFieldId'
import { defineComponent, h } from 'vue'

vi.mock('@/api/auth', () => ({
  authApi: { changePassword: vi.fn() },
}))

const modalStubs = {
  Modal: {
    props: ['open', 'title'],
    template: '<div v-if="open" class="modal-stub"><slot /><slot name="footer" /></div>',
  },
}

/** 取容器内所有 id 属性值（空串不算），用于唯一性断言 */
function collectIds(root: Element): string[] {
  const ids: string[] = []
  root.querySelectorAll('[id]').forEach((el) => {
    const id = el.getAttribute('id')
    if (id) ids.push(id)
  })
  return ids
}

/** 断言每个 for 都能在文档里找到唯一目标，且目标本身有 id（for 悬空是本票要防的核心回归） */
function assertEveryLabelResolves(root: Element): void {
  const labels = Array.from(root.querySelectorAll('label[for]'))
  expect(labels.length).toBeGreaterThan(0)
  for (const label of labels) {
    const target = label.getAttribute('for')!
    const matches = root.querySelectorAll(`[id="${target}"]`)
    expect(matches.length, `label for="${target}" 应恰好命中 1 个元素`).toBe(1)
  }
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
})

describe('ChangePasswordModal 表单关联（#233）', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
  })

  it('三个密码输入框都能被 label 报出名称，且 for 指向真实元素', async () => {
    const wrapper = mount(ChangePasswordModal, {
      props: { open: true },
      global: { stubs: modalStubs },
    })
    await flushPromises()

    assertEveryLabelResolves(wrapper.element as Element)

    // 名字层面：每个 input 都能通过 label 得到可读名称
    const inputs = wrapper.findAll('input[type="password"]')
    expect(inputs).toHaveLength(3)
    for (const input of inputs) {
      const id = input.attributes('id')
      expect(id, '每个受控 input 必须有 id 才能被 label 关联').toBeTruthy()
      const label = wrapper.find(`label[for="${id}"]`)
      expect(label.exists(), `id=${id} 应有对应 label`).toBe(true)
      expect(label.text().length).toBeGreaterThan(0)
    }

    // 具体文案对齐，防止 id 配对到相邻字段（配错了也能过上面的存在性断言）
    expect(wrapper.find('label[for="' + inputs[0].attributes('id') + '"]').text()).toBe('旧密码')
    expect(wrapper.find('label[for="' + inputs[1].attributes('id') + '"]').text()).toBe('新密码')
    expect(wrapper.find('label[for="' + inputs[2].attributes('id') + '"]').text()).toBe('确认新密码')
  })

  it('同页两个实例（侧边栏改密 + 强制改密同时挂载）的 id 不重复', async () => {
    // 必须模型化「同一个 Vue app 内两个实例」——真实页面里 App.vue 的强制改密弹窗与
    // 侧边栏改密弹窗同属一个 app。分开 mount 会各自新建 app 上下文、useId 计数各自从 0 起，
    // 那是测试构造出来的假冲突，不是产品行为。
    const Host = defineComponent({
      components: { ChangePasswordModal },
      template: `
        <div>
          <ChangePasswordModal :open="true" />
          <ChangePasswordModal :open="true" forced />
        </div>`,
    })
    const wrapper = mount(Host, { global: { stubs: modalStubs }, attachTo: document.body })
    await flushPromises()

    const ids = collectIds(wrapper.element as Element)
    expect(ids).toHaveLength(6) // 两个实例 × 三个字段
    // 全文档范围内 id 唯一（重复 id 会让 for 只关联第一个，读屏报错名称）
    expect(new Set(ids).size).toBe(ids.length)

    // 两个实例的每个 for 仍各自解析到唯一目标
    assertEveryLabelResolves(wrapper.element as Element)

    wrapper.unmount()
  })
})

describe('SyncSwitch 可访问名称（#233）', () => {
  it('传入 label 时读屏能报出名称', () => {
    const wrapper = mount(SyncSwitch, { props: { modelValue: false, label: '自动登录' } })
    expect(wrapper.find('input[type="checkbox"]').attributes('aria-label')).toBe('自动登录')
  })

  it('未传 label 时不臆造 aria-label（纯装饰场景不应出现空名）', () => {
    const wrapper = mount(SyncSwitch, { props: { modelValue: false } })
    expect(wrapper.find('input[type="checkbox"]').attributes('aria-label')).toBeUndefined()
  })
})
