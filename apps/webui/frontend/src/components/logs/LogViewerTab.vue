<script setup lang="ts">
import { ref, watch, onMounted, onUnmounted, nextTick } from 'vue'
import { RecycleScroller } from 'vue-virtual-scroller'
import type { RecycleScrollerExposed } from 'vue-virtual-scroller'
import 'vue-virtual-scroller/dist/vue-virtual-scroller.css'
import { useLogsStore } from '@/stores/logs'
import { useSystemStore } from '@/stores/system'
import { useWebSocket } from '@/composables/wsClient'
import { LOG_LEVELS } from '@/stores/logs'
import { LEVEL_COLORS, levelColor } from '@/composables/useLogLevelColors'
import Dropdown from '@/components/shared/Dropdown.vue'
import type { DropdownItem } from '@/components/shared/Dropdown.vue'
import type { LogLine } from '@/types/api'

const store = useLogsStore()
const system = useSystemStore()
const ws = useWebSocket()
const scroller = ref<RecycleScrollerExposed | null>(null)

function levelTagClass(level: string): string {
  return `log-level-tag log-level-${level}`
}

function highlightMessage(msg: string): string {
  const escaped = msg
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
  const pattern = /(\d{4}-\d{2}-\d{2}[T ]\d{2}:\d{2}:\d{2}(?:\.\d+)?)|(\b\w+:\/\/\S+)|(\b\w*Exception\b)|(\b0x[0-9a-fA-F]+\b)|(\w+)=|("(?:[^"\\]|\\.)*")|('(?:[^'\\]|\\.)*')|(\d+\.?\d*)/g
  return escaped.replace(pattern, (match, date, url, exception, hex, kvKey, dstr, sstr, num) => {
    if (date !== undefined) return `<span class="log-date">${date}</span>`
    if (url !== undefined) return `<span class="log-url">${url}</span>`
    if (exception !== undefined) return `<span class="log-exception">${exception}</span>`
    if (hex !== undefined) return `<span class="log-hex">${hex}</span>`
    if (kvKey !== undefined) return `<span class="log-kv-key">${kvKey}</span>=`
    if (dstr !== undefined) return `<span class="log-string">${dstr}</span>`
    if (sstr !== undefined) return `<span class="log-string">${sstr}</span>`
    if (num !== undefined) return `<span class="log-number">${num}</span>`
    return match
  })
}

function highlightKeyword(text: string): string {
  if (!store.keyword) return text
  const kw = store.keyword.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')
  return text.replace(
    new RegExp(`(${kw})`, 'gi'),
    '<span class="log-highlight">$1</span>'
  )
}

const STRATEGY_LEVEL_RE = /\b(trace|debug|info|warn|warning|error|fatal|critical)\b/i
function detectStrategyLevel(raw: string): string {
  const match = raw.match(STRATEGY_LEVEL_RE)
  if (!match) return ''
  return match[1].toLowerCase()
}

function lineLevel(line: LogLine): string {
  if (line.parsed) return line.level
  return detectStrategyLevel(line.raw)
}

function isErrorLine(line: LogLine): boolean {
  const lvl = lineLevel(line)
  return lvl === 'error' || lvl === 'critical'
}

function toggleTail(): void {
  if (store.tailEnabled) {
    store.setTailEnabled(false)
    ws.unsubscribeLog()
  } else {
    if (!store.selectedFile) return
    store.setTailEnabled(true)
    ws.subscribeLog(store.selectedFile)
    nextTick(() => scrollToBottom())
  }
}

function scrollToBottom(): void {
  if (scroller.value && store.lines.length > 0) {
    scroller.value.scrollToItem(store.lines.length - 1)
  }
}

async function refreshContent(): Promise<void> {
  await store.loadContent()
  nextTick(() => scrollToBottom())
}

const levelItems: DropdownItem[] = [
  { value: '', label: '全部级别' },
  ...LOG_LEVELS.map(lvl => ({
    value: lvl,
    label: lvl,
    dot: LEVEL_COLORS[lvl] ?? 'var(--text-tertiary)',
  })),
]

async function handleLevelChange(value: string | number): Promise<void> {
  store.levelFilter = String(value)
  if (store.tailEnabled) {
    ws.unsubscribeLog()
    store.setTailEnabled(false)
  }
  await store.loadContent()
}

let keywordTimer: ReturnType<typeof setTimeout> | null = null
function handleKeywordInput(event: Event): void {
  const value = (event.target as HTMLInputElement).value
  if (keywordTimer) clearTimeout(keywordTimer)
  keywordTimer = setTimeout(() => {
    store.keyword = value
    if (store.tailEnabled) {
      ws.unsubscribeLog()
      store.setTailEnabled(false)
    }
    store.loadContent()
  }, 300)
}

watch(() => store.lines.length, () => {
  if (store.tailEnabled) {
    nextTick(() => scrollToBottom())
  }
})

watch(() => store.contentLoading, (loading, prev) => {
  if (!loading && prev) {
    nextTick(() => scrollToBottom())
  }
})

// I1: 订阅绑定连接（契约 webui-ws §3 subscribe_log），断线即随旧连接失效；
// 重连成功时按当前 tail 状态重建订阅，消除"开关仍开但 log_line 静默断流"。
// prev 守卫：首连（挂载时已 connected）不重复订阅——onMounted 已处理
watch(() => ws.connectionState.value, (state, prev) => {
  if (state === 'connected' && prev && prev !== 'connected' &&
      store.tailEnabled && store.selectedFile) {
    ws.subscribeLog(store.selectedFile)
    nextTick(() => scrollToBottom())
  }
})

onMounted(() => {
  if (store.tailEnabled && store.selectedFile) {
    ws.subscribeLog(store.selectedFile)
  }
})

onUnmounted(() => {
  // M14: 清理未触发的关键字防抖定时器（避免卸载后改共享 store 状态）
  if (keywordTimer) {
    clearTimeout(keywordTimer)
    keywordTimer = null
  }
  if (store.tailEnabled) {
    ws.unsubscribeLog()
  }
})

function formatTs(ts: string): string {
  if (ts.length > 19) return ts.substring(11, 19)
  return ts
}
</script>

<template>
  <div class="log-viewer-panel">
    <div class="log-viewer__toolbar">
      <div class="log-viewer__toolbar-row log-viewer__toolbar-row--filters">
        <Dropdown
          :items="levelItems"
          :model-value="store.levelFilter"
          placeholder="全部级别"
          @update:model-value="handleLevelChange"
        />
        <span class="log-viewer__info">约 {{ store.totalLines }} 行 · 显示 {{ store.lines.length }} 行</span>
        <div class="ds-input log-viewer__search">
          <input
            type="text"
            placeholder="关键字搜索..."
            :value="store.keyword"
            @input="handleKeywordInput"
          >
        </div>
      </div>
      <div class="log-viewer__toolbar-row log-viewer__toolbar-row--actions">
        <div class="log-viewer__actions-spacer"></div>
        <button class="ds-btn ds-btn--secondary ds-btn--sm" type="button" @click="scrollToBottom">跳到底部</button>
        <button
          class="ds-btn ds-btn--secondary ds-btn--sm"
          type="button"
          :disabled="store.contentLoading || !store.selectedFile"
          @click="refreshContent"
        >
          {{ store.contentLoading ? '...' : '刷新' }}
        </button>
        <label class="ds-switch">
          <input
            type="checkbox"
            :checked="store.tailEnabled"
            :disabled="!store.selectedFile || system.isSelf(store.selectedLogger)"
            @change="toggleTail"
          >
          <span class="ds-switch__track"><span class="ds-switch__thumb"></span></span>
        </label>
        <span class="log-viewer__tail-label">实时</span>
      </div>
    </div>

    <div class="log-viewer__lines">
      <div v-if="store.contentLoading" class="log-viewer__file-empty">加载中...</div>
      <div v-else-if="store.lines.length === 0" class="log-viewer__file-empty">
        {{ store.selectedFile ? '暂无日志内容' : '请选择左侧日志文件' }}
      </div>
      <RecycleScroller
        v-else
        ref="scroller"
        class="log-viewer__scroller"
        :items="store.lines"
        :item-size="20"
        key-field="n"
        v-slot="{ item }"
      >
        <div
          class="log-row"
          :class="{ 'log-row-error': isErrorLine(item) }"
        >
          <span class="log-row__n">{{ item.n }}</span>
          <span class="log-row__ts">{{ formatTs(item.ts) }}</span>
          <span :class="levelTagClass(lineLevel(item))" :style="{ color: levelColor(lineLevel(item)) }">
            {{ lineLevel(item) || '----' }}
          </span>
          <span class="log-row__logger">{{ item.logger }}</span>
          <span
            class="log-row__msg"
            v-html="highlightKeyword(highlightMessage(item.parsed ? item.msg : item.raw))"
          ></span>
        </div>
      </RecycleScroller>
    </div>
  </div>
</template>

<style scoped>
.log-viewer-panel {
  flex: 1;
  min-height: 400px;
  min-width: 0;
  display: flex;
  flex-direction: column;
  overflow: hidden;
}

.log-viewer__toolbar {
  display: grid;
  grid-template-rows: auto auto;
  gap: var(--spacer-4);
  padding: var(--spacer-8) var(--spacer-12);
  border-bottom: 1px solid var(--border-neutral-l1);
}
.log-viewer__toolbar-row {
  display: flex;
  align-items: center;
  gap: var(--spacer-8);
}
.log-viewer__toolbar-row--filters {
  flex-wrap: wrap;
}
.log-viewer__toolbar-row--actions {
  flex-wrap: wrap;
}
.log-viewer__actions-spacer {
  flex: 1;
}
.log-viewer__tail-label {
  font-size: var(--body-sm-font-size);
  color: var(--text-secondary);
  white-space: nowrap;
}
.log-viewer__toolbar .ds-dropdown {
  width: auto;
  flex-shrink: 0;
  max-width: 160px;
}
.log-viewer__search {
  flex: 1;
  min-width: 120px;
}
.log-viewer__search input {
  height: 32px;
}
.log-viewer__info {
  font-size: 10px;
  color: var(--text-tertiary);
  white-space: nowrap;
  flex-shrink: 0;
}

.log-viewer__lines {
  flex: 1;
  overflow: auto;
}
.log-viewer__scroller {
  height: 100%;
  font-family: var(--code-terminal-font-family);
  font-size: var(--code-terminal-font-size);
  line-height: var(--code-terminal-line-height);
}
.log-viewer__lines :deep(.vue-recycle-scroller__item-wrapper) {
  overflow: visible;
}
.log-viewer__lines :deep(.vue-recycle-scroller__item-view) {
  width: auto;
}
.log-viewer__lines :deep(.vue-recycle-scroller.direction-vertical) {
  overflow-x: auto;
}

.log-row {
  display: flex;
  align-items: baseline;
  gap: var(--spacer-8);
  padding: 1px var(--spacer-8);
  white-space: nowrap;
}
.log-row:hover {
  background: var(--bg-overlay-l1);
}
.log-row-error {
  background: color-mix(in srgb, var(--status-error-default) 8%, transparent);
}
.log-row__n {
  color: var(--text-tertiary);
  flex-shrink: 0;
  width: 50px;
  text-align: right;
  font-variant-numeric: tabular-nums;
}
.log-row__ts {
  color: var(--text-tertiary);
  flex-shrink: 0;
  font-variant-numeric: tabular-nums;
}
.log-level-tag {
  flex-shrink: 0;
  font-weight: 600;
  width: 50px;
  text-align: center;
}
.log-row__logger {
  color: var(--text-secondary);
  flex-shrink: 0;
  max-width: 120px;
  overflow: hidden;
  text-overflow: ellipsis;
}
.log-row__msg {
  color: var(--text-default);
  flex: 1;
  min-width: 0;
}

.log-viewer-panel :deep(.log-kv-key) { color: var(--log-kv-key); }
.log-viewer-panel :deep(.log-number) { color: var(--log-number); }
.log-viewer-panel :deep(.log-string) { color: var(--log-string); }
.log-viewer-panel :deep(.log-hex) { color: var(--log-hex); }
.log-viewer-panel :deep(.log-date) { color: var(--log-date); }
.log-viewer-panel :deep(.log-url) { color: var(--log-url); }
.log-viewer-panel :deep(.log-exception) { color: var(--log-exception); font-style: italic; }
.log-viewer-panel :deep(.log-highlight) {
  background: color-mix(in srgb, var(--status-warning-default) 30%, transparent);
  border-radius: 2px;
}
.log-viewer__file-empty {
  padding: var(--spacer-48);
  text-align: center;
  color: var(--text-tertiary);
  font-size: var(--body-sm-font-size);
}
</style>
