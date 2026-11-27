<script setup lang="ts">
/**
 * 消息中心「查看全部」抽屉（ADR 0060 决策 6，契约 notify-ui / rest §2.6）。
 *
 * 与仪表盘消息面板的分工：面板只显示最近 50 条（空间有限），本抽屉是全量（最多 100 条）
 * 并支持按级别筛选、全部标为已读。数据来自 `useNotifyStore().items`——它由 REST 历史基线
 * 和 WS `notify_ui` 增量共同填充，**不是另一个数据源**。
 */
import { computed, ref, watch } from 'vue'
import Modal from '@/components/shared/Modal.vue'
import { useNotifyStore, type NotifyLevel } from '@/stores/notify'

const props = defineProps<{ open: boolean }>()
const emit = defineEmits<{ close: [] }>()

const store = useNotifyStore()

type Filter = 'all' | NotifyLevel
const filter = ref<Filter>('all')

const FILTERS: Array<{ value: Filter; label: string }> = [
  { value: 'all', label: '全部' },
  { value: 'error', label: '错误' },
  { value: 'warning', label: '警告' },
  { value: 'info', label: '信息' },
]

/** 新的在前（与面板一致） */
const rows = computed(() => {
  const all = [...store.items].reverse()
  return filter.value === 'all' ? all : all.filter(i => i.level === filter.value)
})

const counts = computed(() => ({
  all: store.items.length,
  error: store.items.filter(i => i.level === 'error').length,
  warning: store.items.filter(i => i.level === 'warning').length,
  info: store.items.filter(i => i.level === 'info').length,
}))

const LEVEL_TEXT: Record<NotifyLevel, string> = {
  info: '信息',
  warning: '警告',
  error: '错误',
}

function formatTime(ts: number): string {
  const d = new Date(ts)
  const pad = (n: number) => String(n).padStart(2, '0')
  return `${pad(d.getMonth() + 1)}-${pad(d.getDate())} ${pad(d.getHours())}:${pad(d.getMinutes())}:${pad(d.getSeconds())}`
}

// 打开即标为已读：用户已经看到全部内容，保留未读徽标反而误导。
// immediate：抽屉以 open=true 挂载时也要生效（不只响应后续的 false→true 切换）。
watch(() => props.open, (open) => {
  if (open) store.markAllRead()
}, { immediate: true })
</script>

<template>
  <Modal :open="open" title="消息中心" @close="emit('close')">
    <div class="notify-drawer">
      <div class="notify-drawer__toolbar">
        <div class="notify-drawer__filters" role="group" aria-label="按级别筛选">
          <button
            v-for="f in FILTERS"
            :key="f.value"
            type="button"
            class="ds-btn ds-btn--sm"
            :class="filter === f.value ? 'ds-btn--primary' : 'ds-btn--ghost'"
            :aria-pressed="filter === f.value"
            @click="filter = f.value"
          >{{ f.label }} ({{ counts[f.value] }})</button>
        </div>
        <button
          type="button"
          class="ds-btn ds-btn--sm ds-btn--tertiary"
          data-test="notify-drawer-mark-read"
          @click="store.markAllRead()"
        >全部已读</button>
      </div>

      <div class="notify-drawer__list">
        <div v-for="item in rows" :key="item.id" class="notify-drawer__row">
          <span class="notify-drawer__level" :class="`notify-drawer__level--${item.level}`">
            {{ LEVEL_TEXT[item.level] }}
          </span>
          <div class="notify-drawer__body">
            <div class="notify-drawer__msg">
              {{ item.message }}
              <span v-if="item.count > 1" class="notify-drawer__count">×{{ item.count }}</span>
            </div>
            <div class="notify-drawer__meta">
              {{ formatTime(item.timestamp) }}<template v-if="item.source"> · {{ item.source }}</template>
            </div>
          </div>
        </div>
        <div v-if="rows.length === 0" class="notify-drawer__empty" data-test="notify-drawer-empty">
          暂无消息
        </div>
      </div>
    </div>
  </Modal>
</template>

<style scoped>
.notify-drawer {
  display: flex;
  flex-direction: column;
  gap: var(--spacer-12);
  min-height: 0;
}

.notify-drawer__toolbar {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: var(--spacer-8);
  flex-wrap: wrap;
}

.notify-drawer__filters {
  display: inline-flex;
  gap: var(--spacer-4);
  flex-wrap: wrap;
}

.notify-drawer__list {
  display: flex;
  flex-direction: column;
  max-height: 440px;
  overflow-y: auto;
}

.notify-drawer__row {
  display: flex;
  align-items: flex-start;
  gap: var(--spacer-10);
  padding: var(--spacer-8) 0;
  border-bottom: 1px solid var(--border-neutral-l1);
}

.notify-drawer__row:last-child {
  border-bottom: none;
}

/* 级别用文字标签而不是彩色左条（与 toast 同一视觉原则） */
.notify-drawer__level {
  flex-shrink: 0;
  width: 40px;
  font-size: var(--font-size-10);
  line-height: var(--body-sm-line-height);
  text-align: center;
}

.notify-drawer__level--info { color: var(--status-primary-default); }
.notify-drawer__level--warning { color: var(--status-warning-default); }
.notify-drawer__level--error { color: var(--status-error-default); }

.notify-drawer__body {
  flex: 1;
  min-width: 0;
}

.notify-drawer__msg {
  color: var(--text-default);
  font-size: var(--body-sm-font-size);
  line-height: var(--body-sm-line-height);
  word-break: break-word;
}

.notify-drawer__count {
  margin-left: var(--spacer-4);
  color: var(--text-tertiary);
  font-variant-numeric: tabular-nums;
}

.notify-drawer__meta {
  margin-top: var(--spacer-2);
  color: var(--text-tertiary);
  font-size: var(--font-size-10);
  font-variant-numeric: tabular-nums;
}

.notify-drawer__empty {
  padding: var(--spacer-24);
  text-align: center;
  color: var(--text-tertiary);
  font-size: var(--body-sm-font-size);
}
</style>
