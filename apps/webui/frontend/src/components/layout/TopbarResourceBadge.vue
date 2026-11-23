<script setup lang="ts">
/**
 * 顶栏单枚常驻资源徽章（CPU / 内存；#452 / ADR 0054）。
 *
 * 为什么单独成组件：两枚徽章的形态（标签 + 数值 + 状态色 + 悬停说明）完全同构，抽出来后
 * 顶栏模板只差数据；同时把「等级 → class」的映射收在一处（`resourceLevelClass` 是唯一实现）。
 *
 * #452 起不再承载卷 / NTP 形态：卷徽章与独立 NTP 徽章已删除（时钟偏差改在右上墙钟的括号里），
 * 因此 props 里没有 `mount` / `testId` / 点数形态——不留「只为一个已删除形态存在」的参数。
 *
 * 视觉样式在 `assets/css/layout.css` 的 `.topbar__resource*` 规则里（顶栏视觉的唯一处），
 * 本组件不含样式；宽度紧时的紧凑形态也由那里的断点规则负责。
 */
import { resourceLevelClass, type ResourceLevel } from '@/composables/useResourceLevel'

defineProps<{
  /** 资源类别：决定 `topbar__resource--<kind>` 类（语义锚点；样式在 layout.css 的
   * `topbar__resource*` 通用规则里，不按类别分叉） */
  kind: 'cpu' | 'mem'
  /** 短标签（CPU / MEM） */
  label: string
  /** 数值文本（`23%` / `—` / `不可用`） */
  value: string
  /** 使用率展示等级；`null` = 不可判读 / 等待，走中性色，不套状态色 */
  level: ResourceLevel | null
  /** 悬停完整说明（使用率、不可用原因等） */
  title: string
  /** 元素 id（CPU / MEM 两枚徽章使用，供既有测试锚定） */
  domId?: string
}>()
</script>

<template>
  <span
    class="topbar__resource"
    :class="[`topbar__resource--${kind}`, resourceLevelClass(level)]"
    :id="domId"
    :data-level="level ?? undefined"
    :title="title"
  >
    <span class="res-label">{{ label }}</span>
    <span class="res-value">{{ value }}</span>
  </span>
</template>
