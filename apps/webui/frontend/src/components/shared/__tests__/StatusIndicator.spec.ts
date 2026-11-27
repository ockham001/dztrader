import { describe, it, expect } from 'vitest'
import { mount } from '@vue/test-utils'
import StatusIndicator from '@/components/shared/StatusIndicator.vue'

// StatusIndicator 组件测试：验证不同 current 值对应的状态切换
describe('StatusIndicator', () => {
  // idle 状态：current <= min 时显示"未登录"
  it('当 current <= min 时应处于 idle 状态并显示 idle 文本', () => {
    const wrapper = mount(StatusIndicator, {
      props: { current: 0, min: 0, max: 1 },
    })
    expect(wrapper.text()).toContain('未登录')
    expect(wrapper.classes()).toContain('ds-status--idle')
  })

  // done 状态：current >= max 时显示"已登录"
  it('当 current >= max 时应处于 done 状态并显示 done 文本', () => {
    const wrapper = mount(StatusIndicator, {
      props: { current: 1, min: 0, max: 1 },
    })
    expect(wrapper.text()).toContain('已登录')
    expect(wrapper.classes()).toContain('ds-status--done')
  })

  // loading 状态：min < current < max 时显示"登录中"
  it('当 min < current < max 时应处于 loading 状态并显示 loading 文本', () => {
    const wrapper = mount(StatusIndicator, {
      props: { current: 0.5, min: 0, max: 1 },
    })
    expect(wrapper.text()).toContain('登录中')
    expect(wrapper.classes()).toContain('ds-status--loading')
  })

  // 自定义文本应正确渲染（三态各自生效）
  it('应支持自定义 idle/loading/done 文本', () => {
    const baseProps = {
      min: 0,
      max: 1,
      idleText: '离线',
      loadingText: '连接中',
      doneText: '在线',
    }
    const idle = mount(StatusIndicator, { props: { ...baseProps, current: 0 } })
    expect(idle.text()).toContain('离线')

    const loading = mount(StatusIndicator, { props: { ...baseProps, current: 0.5 } })
    expect(loading.text()).toContain('连接中')

    const done = mount(StatusIndicator, { props: { ...baseProps, current: 1 } })
    expect(done.text()).toContain('在线')
  })

  // 契约 progress: max <= min（含 max==0）为不确定进度，current 值忽略
  it('max <= min 时进入不确定进度分支（max==0）', () => {
    const wrapper = mount(StatusIndicator, {
      props: { current: 0, min: 0, max: 0 },
    })
    expect(wrapper.classes()).toContain('ds-status--indeterminate')
    expect(wrapper.classes()).not.toContain('ds-status--idle')
    expect(wrapper.classes()).not.toContain('ds-status--done')
    // 不确定进度无填充比例
    expect(wrapper.attributes('style') ?? '').not.toContain('--ds-status-fill')
  })

  it('max < min 时同样为不确定进度，且忽略 current', () => {
    const wrapper = mount(StatusIndicator, {
      props: { current: 5, min: 10, max: 3 },
    })
    expect(wrapper.classes()).toContain('ds-status--indeterminate')
    expect(wrapper.classes()).not.toContain('ds-status--loading')
    expect(wrapper.attributes('style') ?? '').not.toContain('--ds-status-fill')
  })

  it('不确定进度优先展示 desc 文案', () => {
    const wrapper = mount(StatusIndicator, {
      props: { current: 0, min: 0, max: 0, desc: '订阅合约中' },
    })
    expect(wrapper.find('.ds-status__text').text()).toBe('订阅合约中')
  })

  // mini 模式应添加 mini 类名
  it('mini 属性为 true 时应添加 ds-status--mini 类名', () => {
    const wrapper = mount(StatusIndicator, {
      props: { current: 0, min: 0, max: 1, mini: true },
    })
    expect(wrapper.classes()).toContain('ds-status--mini')
  })
})

// 失败态（#509）：语义由调用方给——组件自己不能从 current/max 推失败
// （md 没有失败态；td 的失败态停在 3/5/7/8，数值映射只会说「进行中」）
describe('failed 失败态', () => {
  it('failed 压过 loading：数值停在中间也渲染 ds-status--failed 徽章', () => {
    const wrapper = mount(StatusIndicator, {
      props: { current: 3, min: 0, max: 10, desc: '认证失败', failed: true },
    })
    expect(wrapper.classes()).toContain('ds-status--failed')
    expect(wrapper.classes()).not.toContain('ds-status--loading')
    // desc 是阶段文案，不能替代「失败」结论
    expect(wrapper.find('.ds-status__text').text()).toBe('失败')
    // 失败态不注入填充比例（不渲染进度条，没有「还在推进」的暗示）：没有内联 style 才是正断言
    expect(wrapper.attributes('style')).toBeUndefined()
  })

  it('failed 压过 done：current 到达 max 也不显示「已登录」', () => {
    const wrapper = mount(StatusIndicator, {
      props: { current: 10, min: 0, max: 10, failed: true },
    })
    expect(wrapper.classes()).toContain('ds-status--failed')
    expect(wrapper.classes()).not.toContain('ds-status--done')
    expect(wrapper.find('.ds-status__text').text()).toBe('失败')
  })

  it('failed 压过 indeterminate 与 idle', () => {
    const indeterminate = mount(StatusIndicator, {
      props: { current: 0, min: 0, max: 0, failed: true },
    })
    expect(indeterminate.classes()).toContain('ds-status--failed')
    expect(indeterminate.classes()).not.toContain('ds-status--indeterminate')

    const idle = mount(StatusIndicator, {
      props: { current: 0, min: 0, max: 10, failed: true },
    })
    expect(idle.classes()).toContain('ds-status--failed')
    expect(idle.classes()).not.toContain('ds-status--idle')
  })

  it('failedText 默认「失败」，可自定义', () => {
    const fallback = mount(StatusIndicator, {
      props: { current: 3, min: 0, max: 10, failed: true },
    })
    expect(fallback.find('.ds-status__text').text()).toBe('失败')

    const custom = mount(StatusIndicator, {
      props: { current: 3, min: 0, max: 10, failed: true, failedText: '认证失败' },
    })
    expect(custom.find('.ds-status__text').text()).toBe('认证失败')
  })
})

// desc 文本（后端进度描述）
describe('desc 文本（后端进度描述）', () => {
  it('desc 优先于三态固定文案（loading 态显示 desc）', () => {
    const wrapper = mount(StatusIndicator, {
      props: { current: 2, min: 0, max: 4, desc: '已连接' },
    })
    expect(wrapper.find('.ds-status__progress-text').text()).toBe('已连接')
  })

  it('desc 优先于三态固定文案（done 态显示 desc）', () => {
    const wrapper = mount(StatusIndicator, {
      props: { current: 4, min: 0, max: 4, desc: '已登录' },
    })
    expect(wrapper.find('.ds-status__text').text()).toBe('已登录')
  })

  it('未传 desc 时保持三态固定文案', () => {
    const wrapper = mount(StatusIndicator, {
      props: { current: 4, min: 0, max: 4, doneText: '就绪' },
    })
    expect(wrapper.find('.ds-status__text').text()).toBe('就绪')
  })
})
