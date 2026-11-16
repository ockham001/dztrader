<script setup lang="ts">
import { computed } from 'vue'
import { useRoute } from 'vue-router'
import Icon from '@/components/shared/Icon.vue'

const route = useRoute()

const title = computed<string>(() => {
  const t = route.meta.title
  return typeof t === 'string' && t.length > 0 ? t : '占位页面'
})

const iconName = computed<string>(() => {
  const i = route.meta.icon
  return typeof i === 'string' && i.length > 0 ? i : 'tool'
})

const description = computed<string>(() => {
  const d = route.meta.description
  return typeof d === 'string' && d.length > 0 ? d : '该功能正在开发中，敬请期待。'
})
</script>

<template>
  <div class="placeholder-view">
    <div class="ds-card placeholder-view__card">
      <div class="placeholder-view__body">
        <div class="ds-empty__icon placeholder-view__icon" aria-hidden="true">
          <Icon :name="iconName" :size="24" />
        </div>
        <span class="ds-tag ds-tag--warning placeholder-view__tag">功能开发中</span>
        <h1 class="ds-card__title placeholder-view__title">{{ title }}</h1>
        <p class="ds-card__desc placeholder-view__desc">{{ description }}</p>
      </div>
    </div>
  </div>
</template>

<style scoped>
/* #346：模板里的静态内联样式归位。只搬运不改值。 */
.placeholder-view {
  flex: 1;
  overflow-y: auto;
  padding: var(--spacer-24);
}
.placeholder-view__card {
  display: flex;
  align-items: center;
  justify-content: center;
  min-height: 320px;
  padding: var(--spacer-40) var(--spacer-24);
}
.placeholder-view__body {
  display: flex;
  flex-direction: column;
  align-items: center;
  text-align: center;
  max-width: 420px;
}
/* 覆盖 .ds-empty__icon 的 40px 方框（原内联覆盖同名字号/尺寸） */
.placeholder-view__icon {
  width: 56px;
  height: 56px;
  margin-bottom: var(--spacer-16);
  color: var(--icon-secondary);
  display: inline-flex;
  align-items: center;
  justify-content: center;
}
.placeholder-view__tag { margin-bottom: var(--spacer-12); }
.placeholder-view__title { margin-bottom: var(--spacer-8); }
.placeholder-view__desc { margin: 0; }
</style>
