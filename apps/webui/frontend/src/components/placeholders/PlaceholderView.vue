<script setup lang="ts">
import { useRoute } from 'vue-router'
import { computed } from 'vue'

const route = useRoute()
const title = computed(() => (route.meta.title as string | undefined) || '页面')
</script>

<template>
  <div :style="{ padding: 'var(--spacer-32) var(--spacer-24)' }">
    <div class="ds-empty">
      <div class="ds-empty__title">{{ title }}</div>
      <div class="ds-empty__desc">此页面尚未实现</div>
    </div>
  </div>
</template>
