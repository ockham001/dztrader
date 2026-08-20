<script setup lang="ts">
import { ref, reactive, watch, onMounted } from 'vue'
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
const eventForm = reactive({ check_interval_min: 5, check_pages: 1, check_bytes: 0 })

// 镜像为权威值: 到达/回推时同步表单
watch(() => store.eventShmConfig, (cfg) => {
  if (!cfg) return
  eventForm.check_interval_min = cfg.check_interval_min
  eventForm.check_pages = cfg.check_pages
  eventForm.check_bytes = cfg.check_bytes
}, { immediate: true })

async function saveEventConfig(): Promise<void> {
  const patch: EventShmConfigPatch = {
    check_interval_min: Number(eventForm.check_interval_min),
    check_pages: Number(eventForm.check_pages),
    check_bytes: Number(eventForm.check_bytes),
  }
  const result = await store.setEventShmConfig(patch)
  if (result === true) {
    toast.success('事件通道配置已下发，等待生效')
  } else if (result === false) {
    toast.error('事件通道配置下发失败')
  }
  // PENDING_TIMEOUT 分支不弹成功不弹失败: usePending 已弹超时 toast (§7.2 防双 toast)
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

    <!-- 事件通道 (契约 shm: page_size_mb 不可变, preload 只读, 仅 check 可编辑) -->
    <div v-if="tab === 'event'" class="ds-card" style="padding:var(--spacer-16);max-width:640px">
      <div class="ds-card__title">事件通道配置</div>
      <div v-if="store.eventShmConfig" style="display:flex;flex-direction:column;gap:var(--spacer-12)">
        <div class="cfg-row">
          <span class="cfg-label">page_size_mb</span>
          <span>{{ store.eventShmConfig.page_size_mb }}</span>
          <span class="ds-tag ds-tag--warning">不可变，改配置文件重启生效</span>
        </div>
        <div class="cfg-row">
          <span class="cfg-label">清理检查间隔(分)</span>
          <div class="ds-input" style="width:120px"><input v-model.number="eventForm.check_interval_min" type="number" min="0" :disabled="store.eventShmPending"></div>
        </div>
        <div class="cfg-row">
          <span class="cfg-label">检查页数</span>
          <div class="ds-input" style="width:120px"><input v-model.number="eventForm.check_pages" type="number" min="0" :disabled="store.eventShmPending"></div>
        </div>
        <div class="cfg-row">
          <span class="cfg-label">检查字节数</span>
          <div class="ds-input" style="width:120px"><input v-model.number="eventForm.check_bytes" type="number" min="0" :disabled="store.eventShmPending"></div>
        </div>
        <div class="cfg-row">
          <span class="cfg-label">预加载点</span>
          <span>{{ Object.keys(store.eventShmConfig.preload_points).join(', ') || '无' }}</span>
        </div>
        <div class="cfg-row">
          <button class="ds-btn ds-btn--primary ds-btn--sm" type="button" :disabled="store.eventShmPending" @click="saveEventConfig">
            <span v-if="store.eventShmPending" class="ds-btn__spinner"></span>
            {{ store.eventShmPending ? '下发生效中...' : '保存配置' }}
          </button>
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
          <div class="ds-input" style="width:120px"><input v-model.number="webuiForm.token_ttl_sec" type="number" min="60"></div>
        </div>
        <div class="cfg-row">
          <span class="cfg-label">Notify 缓存条数</span>
          <div class="ds-input" style="width:120px"><input v-model.number="webuiForm.notify_cache_size" type="number" min="0"></div>
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
</style>
