<script setup lang="ts">
/**
 * 顶栏单枚资源指标徽章（#452 / ADR 0054）：12px 图标 + 数值。
 *
 * 三枚常驻徽章（CPU / 内存 / 时钟偏差）形态完全同构，抽成一个组件后顶栏模板只差数据；
 * 「等级 → class」映射也收在一处（`resourceLevelClass` 是唯一实现）。
 *
 * 可访问名挂在**整枚徽章**上（`role="img"` + `aria-label`，例「CPU 使用率 19%」）：
 * 图标（`Icon` 组件自带 `aria-hidden`）不单独承担语义，数值文本只是它的视觉呈现。
 * 图标颜色永远是中性色，状态只由数值文本的颜色表达（见 layout.css）。
 *
 * 视觉样式在 `assets/css/layout.css` 的 `.topbar__metric*` 规则里（顶栏视觉的唯一处）。
 */
import Icon from '@/components/shared/Icon.vue'
import { resourceLevelClass, type ResourceLevel } from '@/composables/useResourceLevel'

defineProps<{
  /** 指标类别：决定 `topbar__metric--<kind>` 类（时钟偏差的 normal 中性色按它覆盖） */
  kind: 'cpu' | 'mem' | 'ntp'
  /** 内联图标的资源名（`public/icons/builtin/<name>.svg`，与 `Icon` 组件的 name 同源） */
  icon: string
  /** 数值文本（`19%` / `+13ms` / `—`） */
  value: string
  /** 数值展示等级；`null` = 不可判读，走中性色，不套状态色 */
  level: ResourceLevel | null
  /** 悬停完整说明（完整名称 + 读数 + 含义；时钟偏差含误差界 / 来源 / stratum / 上次探测） */
  title: string
  /** 读屏名（整枚徽章的可访问名；图标与数值文本本身不承担语义）。
   * 不叫 `ariaLabel`：`aria-*` 前缀在模板里会被 Vue 当成原生属性（不参与 props 匹配）。 */
  accessibleName: string
  /** 元素 id（`resCpu` / `resMem` / `resNtp`，供测试锚定） */
  domId?: string
}>()
</script>

<template>
  <span
    class="topbar__metric"
    :class="[`topbar__metric--${kind}`, resourceLevelClass(level)]"
    :id="domId"
    :data-level="level ?? undefined"
    :title="title"
    role="img"
    :aria-label="accessibleName"
  >
    <Icon class="res-icon" :name="icon" :size="12" />
    <span class="res-value">{{ value }}</span>
  </span>
</template>
