/**
 * 表单字段 id 生成器（#233）：新增/改动的 `label for` ↔ 控件 `id` 配对统一走这里。
 *
 * 为什么需要它：`label + input` 手写配对散落在 14 个 .vue 里，`for`/`id` 靠人肉对齐，
 * 改一处漏一处；而同一页面上多实例（v-for 生成的字段、每个经纪商各一份登录字段、
 * 多个弹窗同时挂载）会撞 id——重复 id 会让 `<label for>` 只关联到文档里第一个匹配项，
 * 屏幕阅读器报出错误名称，`getElementById` 也只返回首个。
 *
 * 用法（`<script setup>` 内调一次，模板里每个字段传一个稳定的语义 slug）：
 *
 * ```ts
 * const fieldId = useFieldId()
 * ```
 * ```html
 * <label :for="fieldId('old-password')">旧密码</label>
 * <input :id="fieldId('old-password')" type="password">
 * ```
 *
 * **v-for 里必须把循环维度的稳定键拼进 slug**（`:key` 用的那个值，不要用 index），
 * 否则同一组件内 N 次迭代拿到同一个 id：
 *
 * ```html
 * <label v-for="field in FIELDS" :key="field.key" :for="fieldId(`sampling-${field.key}`)">
 * ```
 *
 * 唯一性来源：`useId()` 按**组件实例**前缀（Vue 3.5 起提供），因此多实例各自不撞；
 * 组件内再靠调用方传入的 slug 区分。两段合起来覆盖「同页多实例」这一唯一冲突面。
 */
import { useId } from 'vue'

/** 返回 `(name) => id` 的生成器；`name` 是该字段在组件内唯一且稳定的语义 slug */
export function useFieldId(): (name: string) => string {
  // useId() 只在 setup 上下文有效，因此必须在组件 setup 期间调用（本函数即此用途）
  const instancePrefix = useId()
  return (name: string) => `${instancePrefix}-${name}`
}
