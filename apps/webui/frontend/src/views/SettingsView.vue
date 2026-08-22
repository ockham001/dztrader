<script setup lang="ts">
import { ref, reactive, watch, computed, onMounted } from 'vue'
import Icon from '@/components/shared/Icon.vue'
import Modal from '@/components/shared/Modal.vue'
import TimePicker from '@/components/shared/TimePicker.vue'
import type { EventShmConfigPatch } from '@/api/settings'
import { useSettingsStore } from '@/stores/settings'
import { useToastStore } from '@/stores/toast'

const store = useSettingsStore()
const toast = useToastStore()

const tab = ref<'event' | 'webui' | 'master'>('event')

onMounted(() => {
  store.loadMaster()
  store.loadWebui()
})

// ===== 事件通道 tab =====
// 每项修改即时下发主进程 (dzweb → SHM SET_EVENT_SHM_CONFIG → master 合并持久化),
// 对齐 CTP 行情通道: check_* 失焦单字段提交, preload_points 增删改即时提交, 无"保存配置"按钮。
// 前端不做范围校验: 修改多少就下发多少, 范围/类型由后端 validate() 严格校验,
// 非法值被拒后经 RTN_EVENT_SHM_CONFIG 回推旧值, WS 同步镜像回退 (唯一真相源在 master)。
type ShmCheckField = 'check_interval_min' | 'check_pages' | 'check_bytes'

// check_* 三字段失焦单字段提交: 仅值改变/可序列化时才下发
function onCheckBlur(field: ShmCheckField, event: Event): void {
  const raw = (event.target as HTMLInputElement).value.trim()
  if (raw === '') return
  const parsed = Number(raw)
  if (!Number.isFinite(parsed)) return  // 非数字无法序列化, 忽略
  const old = store.eventShmConfig?.[field]
  if (old !== undefined && parsed === old) return
  store.setEventShmConfig({ [field]: parsed } as EventShmConfigPatch)
}

// ===== 预加载点（对齐 CTP 行情通道: 行内编辑 + 添加/删除, 即时单操作提交）=====
const preloadEntries = computed(() =>
  Object.entries(store.eventShmConfig?.preload_points ?? {})
    .sort(([a], [b]) => a.localeCompare(b)))

// 行内 pages/bytes 失焦提交: 从镜像读当前值拼完整 value 下发（递归合并按 key 覆盖）
// 范围/类型由后端 validate() 校验, 非法值被拒后经 RTN 回推旧值同步
function onPreloadBlur(time: string, field: 'pages' | 'bytes', event: Event): void {
  const raw = (event.target as HTMLInputElement).value.trim()
  if (raw === '') return
  const parsed = Number(raw)
  if (!Number.isFinite(parsed)) return  // 非数字无法序列化, 忽略
  const cur = store.eventShmConfig?.preload_points[time]
  if (cur && parsed === cur[field]) return
  const next = { pages: cur?.pages ?? 0, bytes: cur?.bytes ?? 0, [field]: parsed }
  store.setEventShmConfig({ preload_points: { [time]: next } })
}

// 删除预加载点: 契约 shm 唯一合法 null 位置（preload_points 内 key 的 value=null）
function removePreloadPoint(time: string): void {
  store.setEventShmConfig({ preload_points: { [time]: null } })
}

// 添加预加载点 Modal（TimePicker 选 HH:MM + pages/bytes 输入）
const preloadModalOpen = ref(false)
const preloadTime = ref('08:45')
const preloadPages = ref('1')
const preloadBytes = ref('0')
const preloadError = ref<string | null>(null)

function openPreloadModal(): void {
  preloadTime.value = '08:45'
  preloadPages.value = '1'
  preloadBytes.value = '0'
  preloadError.value = null
  preloadModalOpen.value = true
}

async function confirmAddPreload(): Promise<void> {
  const time = preloadTime.value
  const pages = Number(preloadPages.value)
  const bytes = Number(preloadBytes.value)
  if (preloadEntries.value.some(([t]) => t === time)) {
    preloadError.value = '该时间点已存在'
    return
  }
  if (!Number.isFinite(pages) || !Number.isFinite(bytes) || preloadPages.value.trim() === '' || preloadBytes.value.trim() === '') {
    preloadError.value = '页数、字节需为有效数字'
    return
  }
  // 范围/类型由后端 validate() 校验, 非法值被拒后经 RTN 回推旧值同步
  const ok = await store.setEventShmConfig({ preload_points: { [time]: { pages, bytes } } })
  // false = 防重入或下发失败, 保持 Modal 打开; 其余（成功/超时）关闭, 最终以 WS 回推为准
  if (ok !== false) preloadModalOpen.value = false
}

// ===== WebUI tab =====
const webuiForm = reactive({ token_ttl_sec: 3600, notify_cache_size: 100 })
const webuiSaving = ref(false)

watch(() => store.webui, (w) => {
  if (!w) return
  webuiForm.token_ttl_sec = w.token_ttl_sec
  webuiForm.notify_cache_size = w.notify_cache_size
}, { immediate: true })

async function saveWebui(): Promise<void> {
  if (webuiSaving.value) return
  webuiSaving.value = true
  try {
    const ok = await store.setWebui({
      token_ttl_sec: Number(webuiForm.token_ttl_sec),
      notify_cache_size: Number(webuiForm.notify_cache_size),
    })
    if (ok) {
      toast.success('WebUI 配置已保存')
    } else {
      toast.error('WebUI 配置保存失败')
    }
  } finally {
    webuiSaving.value = false
  }
}
</script>

<template>
  <div class="settings-view" style="flex:1;overflow-y:auto;padding:var(--spacer-24)">
    <div class="page-header">
      <h1 :style="{
        margin: 0,
        fontSize: 'var(--heading-xl-font-size)',
        lineHeight: 'var(--heading-xl-line-height)',
        fontWeight: 'var(--heading-xl-font-weight)',
        color: 'var(--text-default)',
      }">系统设置</h1>
    </div>

    <div class="ds-tabs" style="margin-bottom:var(--spacer-16)">
      <button class="ds-tab" :class="{ 'is-active': tab === 'event' }" type="button" @click="tab = 'event'">事件通道</button>
      <button class="ds-tab" :class="{ 'is-active': tab === 'webui' }" type="button" @click="tab = 'webui'">WebUI</button>
      <button class="ds-tab" :class="{ 'is-active': tab === 'master' }" type="button" @click="tab = 'master'">主进程</button>
    </div>

    <!-- 事件通道 (契约 shm: page_size_mb 不可变; check_* 与 preload_points 可编辑, preload 内 null=删除) -->
    <div v-if="tab === 'event'" class="ds-card" style="padding:var(--spacer-16);max-width:640px">
      <div class="ds-card__title">事件通道配置</div>
      <div v-if="store.eventShmConfig" style="display:flex;flex-direction:column;gap:var(--spacer-12)">
        <div class="cfg-row">
          <span class="cfg-label">页大小</span>
          <span>{{ store.eventShmConfig.page_size_mb }} MB</span>
          <span class="ds-tag ds-tag--warning">不可变，改配置文件重启生效</span>
        </div>
        <div class="cfg-row">
          <span class="cfg-label">周期检查间隔(分)</span>
          <div class="ds-input" style="width:120px"><input type="number" :value="store.eventShmConfig.check_interval_min" :disabled="store.eventShmPending" @blur="onCheckBlur('check_interval_min', $event)"></div>
        </div>
        <div class="cfg-row">
          <span class="cfg-label">检查预载页数</span>
          <div class="ds-input" style="width:120px"><input type="number" :value="store.eventShmConfig.check_pages" :disabled="store.eventShmPending" @blur="onCheckBlur('check_pages', $event)"></div>
        </div>
        <div class="cfg-row">
          <span class="cfg-label">检查预载字节</span>
          <div class="ds-input" style="width:120px"><input type="number" :value="store.eventShmConfig.check_bytes" :disabled="store.eventShmPending" @blur="onCheckBlur('check_bytes', $event)"></div>
        </div>
        <div class="cfg-row">
          <span class="cfg-label">预加载点</span>
          <button class="ds-btn ds-btn--tertiary ds-btn--sm" type="button" :disabled="store.eventShmPending" @click="openPreloadModal">添加</button>
        </div>
        <div v-for="[time, pt] in preloadEntries" :key="time" class="cfg-row preload-row">
          <span class="cfg-label preload-time">{{ time }}</span>
          <label class="preload-field">页
            <input class="shm-num-input" type="number" :value="pt.pages"
              :disabled="store.eventShmPending" @blur="onPreloadBlur(time, 'pages', $event)">
          </label>
          <label class="preload-field">字节
            <input class="shm-num-input" type="number" :value="pt.bytes"
              :disabled="store.eventShmPending" @blur="onPreloadBlur(time, 'bytes', $event)">
          </label>
          <button
            class="ds-btn ds-btn--tertiary ds-btn--sm ds-btn--icon ds-btn--danger-icon"
            style="margin-left: auto"
            type="button"
            :disabled="store.eventShmPending"
            :title="`删除预加载点 ${time}`"
            :aria-label="`删除预加载点 ${time}`"
            @click="removePreloadPoint(time)"
          >
            <Icon name="Delete" :size="14" />
          </button>
        </div>
        <div v-if="preloadEntries.length === 0" class="cfg-row">
          <span class="cfg-label"></span>
          <span style="color:var(--text-tertiary);font-size:var(--body-sm-font-size)">无预加载点</span>
        </div>
      </div>
      <div v-else style="color:var(--text-tertiary)">等待 WebSocket 推送事件通道配置（dztraderd 未启动时不可见）</div>
    </div>

    <!-- WebUI (webui.json) -->
    <div v-if="tab === 'webui'" class="ds-card" style="padding:var(--spacer-16);max-width:640px">
      <div class="ds-card__title">WebUI 配置</div>
      <div v-if="store.webui" style="display:flex;flex-direction:column;gap:var(--spacer-12)">
        <div class="cfg-row">
          <span class="cfg-label">监听地址</span>
          <span>{{ store.webui.server_listen }}:{{ store.webui.server_port }}</span>
          <span class="ds-tag ds-tag--warning">不可变，改配置文件重启生效</span>
        </div>
        <div class="cfg-row">
          <span class="cfg-label">JWT 密钥</span>
          <span>{{ store.webui.jwt_secret_set ? '已配置' : '未配置' }}</span>
          <span class="ds-tag ds-tag--warning">不可变，改配置文件重启生效</span>
        </div>
        <div class="cfg-row">
          <span class="cfg-label">Token 有效期(秒)</span>
          <div class="ds-input" style="width:120px"><input v-model.number="webuiForm.token_ttl_sec" type="number" min="60" max="604800" :disabled="webuiSaving"></div>
        </div>
        <div class="cfg-row">
          <span class="cfg-label">Notify 缓存条数</span>
          <div class="ds-input" style="width:120px"><input v-model.number="webuiForm.notify_cache_size" type="number" min="0" max="1000000" :disabled="webuiSaving"></div>
          <span class="ds-tag ds-tag--warning">重启后生效</span>
        </div>
        <div class="cfg-row">
          <button class="ds-btn ds-btn--primary ds-btn--sm" type="button" :disabled="webuiSaving" @click="saveWebui">
            <span v-if="webuiSaving" class="ds-btn__spinner"></span>
            {{ webuiSaving ? '保存中...' : '保存配置' }}
          </button>
        </div>
      </div>
      <div v-else style="color:var(--text-tertiary)">加载中...</div>
    </div>

    <!-- 主进程 (只读) -->
    <div v-if="tab === 'master'" class="ds-card" style="padding:var(--spacer-16);max-width:640px">
      <div class="ds-card__title">主进程配置（dztraderd）</div>
      <div v-if="store.master" style="display:flex;flex-direction:column;gap:var(--spacer-12)">
        <div class="cfg-row">
          <span class="cfg-label">停止超时(秒)</span>
          <span>{{ store.master.single_stop_timeout_sec }}</span>
        </div>
        <div class="cfg-row">
          <span class="cfg-label">旧页保留页数</span>
          <span>{{ store.master.cleanup_max_page_count }}</span>
        </div>
        <div class="cfg-row">
          <span class="cfg-label">旧页保留时长(时)</span>
          <span>{{ store.master.cleanup_max_page_age_hours }}</span>
        </div>
        <div class="cfg-row">
          <span class="cfg-label">元数据文件大小</span>
          <span>{{ (store.master.meta_file_size / 1024 / 1024).toFixed(1) }} MB</span>
        </div>
        <p style="margin:0;color:var(--text-tertiary);font-size:var(--body-sm-font-size)">
          以上项当前仅支持人工修改 dztraderd.json 后重启 dztraderd 生效；UI 可编辑能力规划中。
        </p>
      </div>
      <div v-else style="color:var(--text-tertiary)">加载中...</div>
    </div>

    <!-- ===== Add Preload Point Modal ===== -->
    <Modal :open="preloadModalOpen" title="添加预加载点" @close="preloadModalOpen = false">
      <div class="dialog-form">
        <div class="dialog-row">
          <label class="dialog-row__label">时间</label>
          <div class="dialog-row__control"><TimePicker v-model="preloadTime" /></div>
        </div>
        <div class="dialog-row">
          <label class="dialog-row__label">预加载页数</label>
          <div class="ds-input dialog-row__control">
            <input v-model="preloadPages" type="number" placeholder="页数">
          </div>
        </div>
        <div class="dialog-row">
          <label class="dialog-row__label">预加载字节</label>
          <div class="ds-input dialog-row__control">
            <input v-model="preloadBytes" type="number" placeholder="字节数">
          </div>
        </div>
        <div v-if="preloadError" class="dialog-row__error">{{ preloadError }}</div>
      </div>
      <template #footer>
        <button class="ds-btn ds-btn--secondary" type="button" @click="preloadModalOpen = false">取消</button>
        <button class="ds-btn ds-btn--primary" type="button" @click="confirmAddPreload">添加</button>
      </template>
    </Modal>
  </div>
</template>

<style scoped>
.page-header {
  margin-bottom: var(--spacer-24);
}

/* 配置行: 标签(固定宽) + 值/输入, 水平居中 */
.cfg-row {
  display: flex;
  gap: var(--spacer-8);
  align-items: center;
}

.cfg-label {
  width: 180px;
  color: var(--text-secondary);
}

/* 预加载点行（对齐 CTP 行情通道: 行内数字输入 + 删除） */
.preload-row { min-height: 30px; }

.preload-time {
  width: 64px;
  color: var(--text-default);
  font-family: var(--code-editor-font-family);
  font-variant-numeric: tabular-nums;
}

.preload-field {
  display: inline-flex;
  align-items: center;
  gap: var(--spacer-4);
  font-size: var(--body-xs-font-size);
  color: var(--text-tertiary);
}

.shm-num-input {
  width: 72px; padding: var(--spacer-1) var(--spacer-4); font-size: var(--body-sm-font-size);
  font-family: var(--code-editor-font-family); font-variant-numeric: tabular-nums;
  color: var(--text-secondary); background: transparent;
  border: 1px solid transparent; border-radius: var(--radius-4);
}
.shm-num-input:focus { outline: none; border-color: var(--border-neutral-l1); background: var(--bg-base-secondary); }
.shm-num-input:disabled { opacity: 0.6; }

/* 添加预加载点对话框 */
.dialog-form { display: flex; flex-direction: column; gap: var(--spacer-12); }
.dialog-row { display: flex; align-items: center; gap: var(--spacer-12); }
.dialog-row__label { font-size: var(--body-sm-font-size); color: var(--text-tertiary); font-weight: var(--font-weight-medium); flex-shrink: 0; width: 110px; white-space: nowrap; }
.dialog-row__control { flex: 1; min-width: 0; }
.dialog-row__error { font-size: var(--body-sm-font-size); color: var(--status-error-default); padding-left: var(--form-label-col-width); }
</style>
