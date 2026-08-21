<script setup lang="ts">
import { ref, computed, onMounted, onUnmounted } from 'vue'
import { useLogsStore } from '@/stores/logs'
import { useSystemStore } from '@/stores/system'
import { useWebSocket } from '@/composables/wsClient'
import Dropdown from '@/components/shared/Dropdown.vue'
import type { DropdownItem } from '@/components/shared/Dropdown.vue'
import DatePicker from '@/components/shared/DatePicker.vue'
import LogViewerTab from '@/components/logs/LogViewerTab.vue'
import LogAnalysisTab from '@/components/logs/LogAnalysisTab.vue'
import LogLevelControlTab from '@/components/logs/LogLevelControlTab.vue'

const store = useLogsStore()
const system = useSystemStore()
const ws = useWebSocket()

const logBrowserEl = ref<HTMLElement | null>(null)
const sidebarFlex = ref(240)
interface DragInfo { startPos: number; startWidth: number }
const dragInfo = ref<DragInfo | null>(null)

const loggerFilterItems = computed<DropdownItem[]>(() => [
  { value: '', label: '全部 Logger' },
  ...store.loggerOptions.map(log => ({ value: log, label: log })),
])

function formatSize(bytes: number): string {
  if (bytes < 1024) return `${bytes}B`
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)}KB`
  return `${(bytes / (1024 * 1024)).toFixed(1)}MB`
}

async function handleSelectFile(path: string, logger: string): Promise<void> {
  if (store.tailEnabled) {
    ws.unsubscribeLog()
    store.setTailEnabled(false)
  }
  await store.selectFile(path, logger)
}

async function handleLoggerFilterChange(value: string | number): Promise<void> {
  store.fileLoggerFilter = String(value)
  await store.loadFiles(true)
}

async function handleDateFilterChange(value: string): Promise<void> {
  store.fileDateFilter = value
  await store.loadFiles(true)
}

async function handleRefreshFiles(): Promise<void> {
  await store.refreshFiles()
}

function loadMoreFiles(): void {
  store.loadFiles(false)
}

function onSplitterDown(e: MouseEvent): void {
  if (!logBrowserEl.value) return
  e.preventDefault()
  dragInfo.value = { startPos: e.clientX, startWidth: sidebarFlex.value }
  window.addEventListener('mousemove', onSplitterMove)
  window.addEventListener('mouseup', onSplitterUp)
}

function onSplitterMove(e: MouseEvent): void {
  const di = dragInfo.value
  if (!di) return
  const delta = e.clientX - di.startPos
  const maxX = logBrowserEl.value ? logBrowserEl.value.clientWidth * 0.6 : 500
  sidebarFlex.value = Math.max(150, Math.min(maxX, di.startWidth + delta))
}

function onSplitterUp(): void {
  dragInfo.value = null
  window.removeEventListener('mousemove', onSplitterMove)
  window.removeEventListener('mouseup', onSplitterUp)
}

/// 从文件名提取最新日期（格式：logger_YYYY-MM-DD.log）
function extractLatestDate(filenames: { name: string }[]): string {
  let latest = ''
  for (const f of filenames) {
    const base = f.name.replace(/\.log$/, '')
    const match = base.match(/_(\d{4}-\d{2}-\d{2})$/)
    if (match && match[1] > latest) {
      latest = match[1]
    }
  }
  return latest
}

onMounted(async () => {
  if (store.files.length === 0) {
    await store.loadFiles(true)
    // 默认设置为列表中最新文件的日期
    if (!store.fileDateFilter && store.files.length > 0) {
      const latestDate = extractLatestDate(store.files)
      if (latestDate) {
        store.fileDateFilter = latestDate
        await store.loadFiles(true)
      }
    }
  }
})

onUnmounted(() => {
  onSplitterUp()
})
</script>

<template>
  <div :style="{ flex: 1, overflow: 'auto', maxWidth: '1400px', width: '100%', margin: '0 auto', padding: 'var(--spacer-32) var(--spacer-24)' }">
    <div class="page-header">
      <h1 :style="{
        margin: 0,
        fontSize: 'var(--heading-xl-font-size)',
        lineHeight: 'var(--heading-xl-line-height)',
        fontWeight: 'var(--heading-xl-font-weight)',
        color: 'var(--text-default)',
      }">日志</h1>
    </div>

    <div class="section-gap">
      <div class="ds-card">
        <div class="ds-card__title">日志级别控制</div>
        <LogLevelControlTab />
      </div>
    </div>

    <div class="section-gap">
      <div class="ds-card">
        <div class="log-browser__header" style="display: flex; align-items: center; justify-content: space-between;">
          <div class="ds-card__title" style="margin-bottom: 0;">日志浏览器</div>
          <div class="ds-tabs" style="margin-bottom: 0;">
            <button class="ds-tab" :class="{ 'is-active': store.browserTab === 'viewer' }" type="button" @click="store.browserTab = 'viewer'">查看</button>
            <button class="ds-tab" :class="{ 'is-active': store.browserTab === 'analysis' }" type="button" @click="store.browserTab = 'analysis'">分析</button>
          </div>
        </div>
        <div ref="logBrowserEl" class="log-browser" :style="{
          display: 'grid',
          gridTemplateColumns: `minmax(200px, ${sidebarFlex}px) 6px minmax(300px, 1fr)`,
          overflow: 'hidden',
          minHeight: '500px',
        }">
          <div class="log-browser__sidebar" :style="{ display: 'flex', flexDirection: 'column', overflow: 'hidden', minWidth: 0 }">
            <div class="log-browser__toolbar">
              <div class="log-browser__toolbar-spacer"></div>
              <button
                class="ds-btn ds-btn--secondary ds-btn--icon log-browser__refresh-btn"
                type="button"
                :disabled="store.filesLoading"
                @click="handleRefreshFiles"
                title="刷新文件列表"
              >
                <span class="icon" data-icon :style="{ width: '14px', height: '14px', '--icon-url': `url('/icons/builtin/Refresh.svg')` }"></span>
              </button>
            </div>
            <div class="log-browser__filter">
              <Dropdown
                :items="loggerFilterItems"
                :model-value="store.fileLoggerFilter"
                placeholder="全部 Logger"
                @update:model-value="handleLoggerFilterChange"
              />
            </div>
            <div class="log-browser__date-filter">
              <DatePicker
                :model-value="store.fileDateFilter"
                @update:model-value="handleDateFilterChange"
              />
            </div>
            <div class="log-browser__file-list">
              <div
                v-for="f in store.files"
                :key="f.path"
                class="log-browser__file-item"
                :class="{ 'is-selected': f.path === store.selectedFile }"
                @click="handleSelectFile(f.path, f.logger)"
              >
                <div class="log-browser__file-name">
                  {{ f.name }}
                  <span v-if="system.isSelf(f.logger)" class="log-browser__dzweb-badge">WebUI</span>
                </div>
                <div class="log-browser__file-meta">
                  <span>{{ formatSize(f.size) }}</span>
                  <span>{{ f.mtime.substring(0, 10) }}</span>
                </div>
              </div>
              <div v-if="store.filesLoading" class="log-browser__file-empty">加载中...</div>
              <div v-if="!store.filesLoading && store.files.length === 0" class="log-browser__file-empty">暂无日志文件</div>
            </div>
            <button
              v-if="store.filesHasMore && !store.filesLoading"
              class="ds-btn ds-btn--secondary ds-btn--sm log-browser__load-more"
              type="button"
              @click="loadMoreFiles"
            >加载更多</button>
          </div>

          <div class="splitter-v log-splitter" @mousedown="onSplitterDown"></div>

          <div class="log-browser__content" :style="{ display: 'flex', flexDirection: 'column', overflow: 'hidden', minWidth: 0 }">
            <LogViewerTab v-show="store.browserTab === 'viewer'" />
            <LogAnalysisTab v-show="store.browserTab === 'analysis'" />
          </div>
        </div>
      </div>
    </div>
  </div>
</template>

<style scoped>
.page-header {
  margin-bottom: var(--spacer-24);
}

.section-gap {
  margin-bottom: var(--spacer-24);
}

.log-browser__header {
  margin-bottom: var(--spacer-16);
}

.log-browser {
  border: 1px solid var(--border-neutral-l1);
  border-radius: var(--radius-12);
  background: var(--bg-base-secondary);
}

.log-splitter {
  width: 6px;
  cursor: col-resize;
  flex-shrink: 0;
  margin: 0;
  position: relative;
  background: transparent;
  transition: background-color 120ms;
  z-index: 5;
}

.log-splitter::before {
  content: '';
  position: absolute;
  z-index: -1;
  left: -7px;
  right: -7px;
  top: 0;
  bottom: 0;
}

.log-splitter::after {
  content: '';
  position: absolute;
  top: 50%;
  left: 50%;
  transform: translate(-50%, -50%);
  width: 2px;
  height: 32px;
  border-radius: var(--radius-1);
  background: var(--border-neutral-l1);
  transition: background-color 120ms;
}

.log-splitter:hover {
  background: var(--bg-brand);
}

.log-splitter:hover::after {
  background: var(--bg-brand);
}

.log-browser__sidebar {
  border-right: 1px solid var(--border-neutral-l1);
}

.log-browser__toolbar {
  display: flex;
  align-items: center;
  padding: var(--spacer-8) var(--spacer-8) 0;
}

.log-browser__toolbar-spacer {
  flex: 1;
}

.log-browser__refresh-btn {
  flex-shrink: 0;
  height: 32px;
  width: 32px;
}

.log-browser__filter {
  display: flex;
  align-items: center;
  padding: var(--spacer-8);
  position: relative;
  z-index: 10;
}

.log-browser__filter .ds-dropdown {
  width: 100%;
  flex-shrink: 0;
}

.log-browser__date-filter {
  padding: 0 var(--spacer-8) var(--spacer-8);
}

.log-browser__file-list {
  flex: 1;
  overflow-y: auto;
  padding: var(--spacer-4);
  border-top: 1px solid var(--border-neutral-l1);
}

.log-browser__file-item {
  padding: var(--spacer-8);
  border-radius: var(--radius-8);
  cursor: pointer;
  transition: background 0.1s ease;
}

.log-browser__file-item:hover {
  background: var(--bg-overlay-l1);
}

.log-browser__file-item.is-selected {
  background: var(--bg-overlay-l2);
}

.log-browser__file-name {
  font-size: var(--body-sm-font-size);
  color: var(--text-default);
  font-family: var(--code-terminal-font-family);
  white-space: nowrap;
  overflow: hidden;
  text-overflow: ellipsis;
}

.log-browser__dzweb-badge {
  display: inline-block;
  font-size: var(--font-size-9);
  padding: var(--spacer-1) var(--spacer-4);
  margin-left: var(--spacer-4);
  border-radius: var(--radius-3);
  background: var(--bg-overlay-l2);
  color: var(--text-tertiary);
  vertical-align: middle;
}

.log-browser__file-meta {
  display: flex;
  gap: var(--spacer-8);
  font-size: var(--font-size-10);
  color: var(--text-tertiary);
  margin-top: var(--spacer-2);
}

.log-browser__file-empty {
  padding: var(--spacer-16);
  text-align: center;
  color: var(--text-tertiary);
  font-size: var(--body-sm-font-size);
}

.log-browser__load-more {
  margin: var(--spacer-8);
  justify-content: center;
}

.log-browser__content {
  border: none;
  border-radius: 0;
  background: transparent;
}

/* ===== 响应式：窄屏单列布局 ===== */
@media (max-width: 1024px) {
  .log-browser {
    display: flex !important;
    flex-direction: column;
    overflow: visible !important;
    gap: var(--spacer-8);
    padding: var(--spacer-8);
    min-height: auto !important;
  }

  .log-splitter {
    display: none !important;
  }

  .log-browser__sidebar {
    border-right: none;
    flex: none !important;
    min-height: auto;
  }

  .log-browser__content {
    flex: none !important;
    min-height: 400px;
    border: 1px solid var(--border-neutral-l1);
    border-radius: var(--radius-12);
    background: var(--bg-base-secondary);
    overflow: hidden;
  }
}
</style>
