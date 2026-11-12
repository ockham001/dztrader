<script setup lang="ts">
/**
 * 顶栏单个资源项（ADR 0048）：`value` 形态（CPU / 内存 / 时钟：标签 + 数值）与
 * `dot` 形态（卷：状态色点 + 挂载点短标签）。
 *
 * 为什么单独成组件：顶栏折叠要**先知道每项渲染多宽**，所以同一份项要渲染两遍
 * （隐形测量行 + 可见行）。抽成一个组件，两行共用同一份渲染与类名——否则测量行的宽度
 * 不可能与可见行一致，折叠数量就会算错。视觉样式仍在 `assets/css/layout.css` 的
 * `.topbar__resource*` 规则里（顶栏视觉的唯一处），本组件不含样式。
 *
 * 形态由 `kind` 决定（`disk` = 状态点，其余 = 标签 + 数值）：不额外设 `variant` 开关，
 * 免得「kind=disk 却给了 value」这种自相矛盾的组合出现。
 */
import { resourceLevelClass, type ResourceLevel } from '@/composables/useResourceLevel'

withDefaults(defineProps<{
  /** 资源类别：决定 `topbar__resource--<kind>` 类（窄屏 `--gauge` 隐藏规则按类名匹配，layout.css 是唯一处） */
  kind: 'cpu' | 'mem' | 'ntp' | 'disk'
  /** 短标签（CPU / MEM / NTP / 卷的挂载点末段） */
  label: string
  /** 数值文本（`23%` / `+12ms` / `—` / `不可用`）；`disk` 形态不渲染它，百分比只进 title / 浮层 */
  value?: string
  /** 使用率展示等级；`null` = 不可判读 / 等待，走中性色，不套状态色 */
  level: ResourceLevel | null
  /** 悬停完整说明（挂载点全路径、使用率、不可用原因等） */
  title: string
  /** 元素 id（CPU / MEM / NTP 三枚固定徽章使用，供既有测试锚定） */
  domId?: string
  /** 卷挂载点（`disk` 形态专用；同时写进 `data-mount` 供跨组件断言） */
  mount?: string
  /** `data-test` 标记（卷徽章 / 资源项） */
  testId?: string
}>(), { value: '', mount: undefined, domId: undefined, testId: undefined })
</script>

<template>
  <span
    class="topbar__resource"
    :class="[`topbar__resource--${kind}`, resourceLevelClass(level)]"
    :id="domId"
    :data-mount="mount"
    :data-level="level ?? undefined"
    :data-test="testId"
    :title="title"
  >
    <span v-if="kind === 'disk'" class="res-dot" aria-hidden="true"></span>
    <span class="res-label">{{ label }}</span>
    <span v-if="kind !== 'disk'" class="res-value">{{ value }}</span>
  </span>
</template>
