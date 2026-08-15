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
  <div :style="{ flex: 1, overflowY: 'auto', padding: 'var(--spacer-24)' }">
    <div class="ds-card" :style="{ display: 'flex', alignItems: 'center', justifyContent: 'center', minHeight: '320px', padding: 'var(--spacer-40) var(--spacer-24)' }">
      <div :style="{ display: 'flex', flexDirection: 'column', alignItems: 'center', textAlign: 'center', maxWidth: '420px' }">
        <div
          class="ds-empty__icon"
          :style="{ width: '56px', height: '56px', marginBottom: 'var(--spacer-16)', color: 'var(--icon-secondary)', display: 'inline-flex', alignItems: 'center', justifyContent: 'center' }"
          aria-hidden="true"
        >
          <Icon :name="iconName" :size="24" />
        </div>
        <span class="ds-tag ds-tag--warning" :style="{ marginBottom: 'var(--spacer-12)' }">功能开发中</span>
        <h1 class="ds-card__title" :style="{ marginBottom: 'var(--spacer-8)' }">{{ title }}</h1>
        <p class="ds-card__desc" :style="{ margin: 0 }">{{ description }}</p>
      </div>
    </div>
  </div>
</template>
