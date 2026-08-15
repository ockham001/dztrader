<script setup lang="ts">
import { computed } from 'vue'
import { getIconUrl } from '@/composables/useIcon'

const props = withDefaults(defineProps<{
  name: string
  size?: number
}>(), { size: 16 })

const style = computed(() => ({
  '--icon-url': `url('${getIconUrl(props.name)}')`,
  width: `${props.size}px`,
  height: `${props.size}px`,
}))
</script>

<template>
  <span class="icon" data-icon :style="style" aria-hidden="true"></span>
</template>
