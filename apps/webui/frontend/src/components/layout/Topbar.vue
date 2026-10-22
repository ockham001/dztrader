<script setup lang="ts">
import { computed, onMounted } from 'vue'
import Icon from '@/components/shared/Icon.vue'
import ThemeDropdown from './ThemeDropdown.vue'
import { useWebSocket } from '@/composables/wsClient'
import { useSysmonStore } from '@/stores/sysmon'

const emit = defineEmits<{ toggleMobile: [] }>()

// WebSocket connection status
const { connectionState } = useWebSocket()

const connStatus = computed<{ text: string; dotClass: string }>(() => {
  switch (connectionState.value) {
    case 'connected':
      return { text: '已连接', dotClass: 'topbar__conn-dot--success' }
    case 'connecting':
      return { text: '连接中...', dotClass: 'topbar__conn-dot--warning' }
    case 'reconnecting':
      return { text: '重连中...', dotClass: 'topbar__conn-dot--warning' }
    case 'failed':
      return { text: '连接失败', dotClass: 'topbar__conn-dot--error' }
    case 'disconnected':
    default:
      return { text: '已断开', dotClass: 'topbar__conn-dot--error' }
  }
})

// 顶栏资源徽章（CPU / 内存 / 各卷）：契约 sysmon 实时快照（固定实例 dzsysmon）。
// 首次挂载经 REST 补拉镜像，之后只由 WS 推送刷新 —— 刷新基线由发送方
// realtime.push_interval_sec（默认 5s）决定，前端不轮询、不改 WS 契约。
const sysmon = useSysmonStore()
onMounted(() => { void sysmon.init() })

const RESOURCE_WARN_RATIO = 0.7
const RESOURCE_DANGER_RATIO = 0.9

interface ResourceBadge {
  text: string
  stateClass: string
  title: string
}

const cpuBadge = computed<ResourceBadge>(() => {
  const entry = sysmon.availabilityOf('cpu.total')
  if (!sysmon.snapshot) {
    // 快照尚未到达：显示等待态，不得用假数字占位
    return {
      text: '—',
      stateClass: 'topbar__resource--unavailable',
      title: 'CPU 使用率（等待 dzsysmon 快照）',
    }
  }
  if (!entry || !entry.available || sysmon.cpuUsageRatio === null) {
    // 不可用/首次采样：按契约显示不可用，不得用 0 冒充（契约 sysmon §2.7）
    return {
      text: '不可用',
      stateClass: 'topbar__resource--unavailable',
      title: entry?.reason ? `CPU 使用率不可用: ${entry.reason}` : 'CPU 使用率不可用',
    }
  }
  const percent = Math.round(sysmon.cpuUsageRatio * 100)
  const stateClass = sysmon.cpuUsageRatio >= RESOURCE_DANGER_RATIO
    ? 'topbar__resource--danger'
    : sysmon.cpuUsageRatio >= RESOURCE_WARN_RATIO
      ? 'topbar__resource--warn'
      : 'topbar__resource--normal'
  return { text: `${percent}%`, stateClass, title: `CPU 使用率 ${percent}%` }
})

// 内存徽章与 CPU 徽章同构：等待 / 不可用 / 百分比 + 阈值状态色；比率取自 store 净化后的
// memoryInfo（可用性由 availability.metrics.memory 承载），0 不是合法占位（契约 sysmon §2.4）。
const memBadge = computed<ResourceBadge>(() => {
  const entry = sysmon.availabilityOf('memory')
  if (!sysmon.snapshot) {
    return {
      text: '—',
      stateClass: 'topbar__resource--unavailable',
      title: '内存使用率（等待 dzsysmon 快照）',
    }
  }
  const ratio = sysmon.memoryInfo?.used_ratio ?? null
  if (!entry || !entry.available || ratio === null) {
    return {
      text: '不可用',
      stateClass: 'topbar__resource--unavailable',
      title: entry?.reason ? `内存使用率不可用: ${entry.reason}` : '内存使用率不可用',
    }
  }
  const percent = Math.round(ratio * 100)
  const stateClass = ratio >= RESOURCE_DANGER_RATIO
    ? 'topbar__resource--danger'
    : ratio >= RESOURCE_WARN_RATIO
      ? 'topbar__resource--warn'
      : 'topbar__resource--normal'
  return { text: `${percent}%`, stateClass, title: `内存使用率 ${percent}%` }
})

/**
 * 各盘徽章（#75）：卷集合与顺序全部由快照驱动，不硬编码盘符或数量。
 * 关注卷（watched=true）在 store 里已排前，此处额外高亮（topbar__resource--watched）。
 * 单个卷 available=false 显示「不可用」，标题带原因 —— 不得用 0 冒充（契约 sysmon §2.4）。
 */
interface DiskBadge {
  mount: string
  label: string
  text: string
  stateClass: string
  watched: boolean
  title: string
}

/**
 * 徽章标签取挂载点的末段（`/data` → `data`、`C:\\` → `C:`、根挂载点保留 `/`）：
 * 顶栏是横向窄条且 `overflow: hidden`，完整路径会把后面的徽章挤出可视区；
 * 完整路径保留在 title 里。旧写死徽章同样是 `C:` / `D:` 这类短标签。
 */
function shortMountLabel(mount: string): string {
  const trimmed = mount.replace(/[\\/]+$/, '')
  if (!trimmed) return mount  // 根挂载点（"/"、"C:\\" 归一后为空）保留原样
  const lastSeparator = Math.max(trimmed.lastIndexOf('/'), trimmed.lastIndexOf('\\'))
  if (lastSeparator < 0) return trimmed  // "C:" 之类无分隔符
  const segment = trimmed.slice(lastSeparator + 1)
  return segment || trimmed
}

const diskBadges = computed<DiskBadge[]>(() => {
  if (!sysmon.snapshot) return []
  return sysmon.volumes.map((volume) => {
    if (!volume.available) {
      return {
        mount: volume.mount,
        label: shortMountLabel(volume.mount),
        text: '不可用',
        stateClass: 'topbar__resource--unavailable',
        watched: volume.watched,
        title: volume.reason
          ? `${volume.mount} 卷不可用: ${volume.reason}`
          : `${volume.mount} 卷不可用`,
      }
    }
    const ratio = volume.used_ratio
    if (ratio === null) {
      // 可用但比率为 null（total=0 的特殊卷）：显示不可用而不是 0%
      return {
        mount: volume.mount,
        label: shortMountLabel(volume.mount),
        text: '不可用',
        stateClass: 'topbar__resource--unavailable',
        watched: volume.watched,
        title: `${volume.mount} 卷使用率不可用`,
      }
    }
    const percent = Math.round(ratio * 100)
    const stateClass = ratio >= RESOURCE_DANGER_RATIO
      ? 'topbar__resource--danger'
      : ratio >= RESOURCE_WARN_RATIO
        ? 'topbar__resource--warn'
        : 'topbar__resource--normal'
    return {
      mount: volume.mount,
      label: shortMountLabel(volume.mount),
      text: `${percent}%`,
      stateClass,
      watched: volume.watched,
      title: `${volume.mount} 使用率 ${percent}%${volume.watched ? '（关注卷）' : ''}`,
    }
  })
})
</script>

<template>
  <header class="topbar">
    <div class="topbar__left">
      <button type="button"
        class="topbar__hamburger"
        aria-label="打开导航菜单"
        @click="emit('toggleMobile')"
      >
        <Icon name="menu" :size="20" />
      </button>

      <!-- Resources -->
      <div class="topbar__resources" id="topbarResources">
        <!-- Service status: MD / TD / STG -->
        <span class="topbar__resource topbar__resource--svc topbar__resource--normal" id="resMd" title="行情服务">
          <span class="res-label">MD</span>
          <span class="res-icon">
            <svg width="10" height="10" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="3" stroke-linecap="round" stroke-linejoin="round"><polyline points="20 6 9 17 4 12"/></svg>
          </span>
        </span>
        <span class="topbar__resource topbar__resource--svc topbar__resource--warn" id="resTd" title="交易服务">
          <span class="res-label">TD</span>
          <span class="res-icon">
            <svg width="10" height="10" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"><path d="M10.29 3.86 1.82 18a2 2 0 0 0 1.71 3h16.94a2 2 0 0 0 1.71-3L13.71 3.86a2 2 0 0 0-3.42 0z"/><line x1="12" y1="9" x2="12" y2="13"/><line x1="12" y1="17" x2="12.01" y2="17"/></svg>
          </span>
        </span>
        <span class="topbar__resource topbar__resource--svc topbar__resource--danger" id="resStg" title="策略服务">
          <span class="res-label">STG</span>
          <span class="res-icon">
            <svg width="10" height="10" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="10"/><line x1="15" y1="9" x2="9" y2="15"/><line x1="9" y1="9" x2="15" y2="15"/></svg>
          </span>
        </span>
        <span class="topbar__resource-divider"></span>
        <!-- 资源徽章：CPU（#71）/ 内存（#74）/ 各卷（#75）均已接入实时快照；
             NTP 与写死的磁盘假数字已删除 -->
        <span
          class="topbar__resource topbar__resource--cpu"
          :class="cpuBadge.stateClass"
          id="resCpu"
          :title="cpuBadge.title"
        >
          <span class="res-label">CPU</span>
          <span class="res-value">{{ cpuBadge.text }}</span>
        </span>
        <span
          class="topbar__resource topbar__resource--mem"
          :class="memBadge.stateClass"
          id="resMem"
          :title="memBadge.title"
        >
          <span class="res-label">MEM</span>
          <span class="res-value">{{ memBadge.text }}</span>
        </span>
        <!-- 各盘徽章：卷集合 / 顺序完全由快照驱动，不硬编码盘符或数量；
             关注卷额外加 --watched 高亮 -->
        <span
          v-for="badge in diskBadges"
          :key="badge.mount"
          class="topbar__resource topbar__resource--disk"
          :class="[badge.stateClass, { 'topbar__resource--watched': badge.watched }]"
          :data-mount="badge.mount"
          :title="badge.title"
        >
          <span class="res-label">{{ badge.label }}</span>
          <span class="res-value">{{ badge.text }}</span>
        </span>
      </div>
    </div>

    <div class="topbar__right">
      <span class="topbar__conn">
        <span class="topbar__conn-dot" :class="connStatus.dotClass"></span>
        <span>{{ connStatus.text }}</span>
      </span>
      <ThemeDropdown />
    </div>
  </header>
</template>

<style scoped>
.topbar__conn-dot {
  width: 6px;
  height: 6px;
  border-radius: 50%;
  display: inline-block;
}

.topbar__conn-dot--success {
  background: var(--status-success-default);
}

.topbar__conn-dot--warning {
  background: var(--status-warning-default);
}

.topbar__conn-dot--error {
  background: var(--status-error-default);
}

/* 指标不可用 / 快照未到达：中性色，不得用状态色或 0 冒充数据 */
.topbar__resource--unavailable .res-value {
  color: var(--text-tertiary);
}

/* 关注卷徽章（#75）：标签用警示色 token 高亮，与数值状态色区分（不硬编码颜色） */
.topbar__resource--watched .res-label {
  color: var(--status-warning-default);
}
</style>
