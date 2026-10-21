<script setup lang="ts">
import { computed, onMounted } from 'vue'
import Icon from '@/components/shared/Icon.vue'
import { useSysmonStore } from '@/stores/sysmon'

// 资源监控页：系统信息与 CPU 区块（总量 / 每核 / 系统负载 / 上下文切换与中断）
// 接契约 sysmon 实时快照；内存 / 磁盘 / 网络区块按可用性映射渲染，
// 未接入时显示明确空态（不得显示 0 冒充数据）。
// 刷新基线由发送方 realtime.push_interval_sec（默认 5s）决定，本页不轮询、不改 WS 契约。
const sysmon = useSysmonStore()

onMounted(() => { void sysmon.init() })

const snapshot = computed(() => sysmon.snapshot)
const system = computed(() => snapshot.value?.system ?? null)
const platformDescription = computed(() => snapshot.value?.availability.platform_description ?? '')
const updatedAt = computed(() => {
  const timestamp = snapshot.value?.timestamp
  return typeof timestamp === 'number' ? formatDateTime(timestamp) : ''
})

// ---- 格式化（不可用一律显示「不可用」，不得用 0 冒充数据）----
const UNAVAILABLE = '不可用'

function formatPercent(ratio: number | null | undefined, digits = 1): string {
  if (typeof ratio !== 'number' || !Number.isFinite(ratio)) return UNAVAILABLE
  return `${(ratio * 100).toFixed(digits)}%`
}

function formatBytes(bytes: number | null | undefined): string {
  if (typeof bytes !== 'number' || !Number.isFinite(bytes) || bytes < 0) return UNAVAILABLE
  const gib = 1024 ** 3
  const mib = 1024 ** 2
  if (bytes >= gib) return `${(bytes / gib).toFixed(1)} GiB`
  if (bytes >= mib) return `${(bytes / mib).toFixed(1)} MiB`
  return `${bytes} B`
}

function formatRate(bytesPerSec: number | null | undefined): string {
  if (typeof bytesPerSec !== 'number' || !Number.isFinite(bytesPerSec)) return UNAVAILABLE
  return `${formatBytes(bytesPerSec)}/s`
}

/** 系统负载 / 处理器队列长度：保留 2 位小数；null 一律显示不可用（不得用 0 冒充） */
function formatLoadValue(value: number | null | undefined): string {
  if (typeof value !== 'number' || !Number.isFinite(value)) return UNAVAILABLE
  return value.toFixed(2)
}

/** 速率型计数（每秒上下文切换 / 中断）：整数值 + /s；null 显示不可用 */
function formatCountPerSec(value: number | null | undefined): string {
  if (typeof value !== 'number' || !Number.isFinite(value)) return UNAVAILABLE
  return `${Math.round(value)}/s`
}

function formatDateTime(unixMs: number): string {
  const date = new Date(unixMs)
  if (Number.isNaN(date.getTime())) return UNAVAILABLE
  const pad = (n: number): string => String(n).padStart(2, '0')
  return `${date.getFullYear()}-${pad(date.getMonth() + 1)}-${pad(date.getDate())} `
    + `${pad(date.getHours())}:${pad(date.getMinutes())}:${pad(date.getSeconds())}`
}

function formatUptime(seconds: number): string {
  if (!Number.isFinite(seconds) || seconds < 0) return UNAVAILABLE
  const total = Math.floor(seconds)
  const days = Math.floor(total / 86_400)
  const hours = Math.floor((total % 86_400) / 3_600)
  const minutes = Math.floor((total % 3_600) / 60)
  const secs = total % 60
  const hms = [hours, minutes, secs].map(n => String(n).padStart(2, '0')).join(':')
  return days > 0 ? `${days} 天 ${hms}` : hms
}

// ---- 可用性映射（契约 sysmon §2.5：按数据渲染，不按 OS 名硬编码分支）----
function isAvailable(metric: string): boolean {
  return sysmon.availabilityOf(metric)?.available === true
}

function unavailableDesc(metric: string): string {
  const reason = sysmon.availabilityOf(metric)?.reason
  if (!reason) return '当前快照未提供该指标'
  // reason 为契约约定的自由文案（建议取值 not_implemented/unsupported/no_permission/no_sensor/collect_failed）
  if (reason === 'not_implemented') return '该指标将在后续版本接入'
  if (reason === 'collect_failed') return '采集失败，暂不可用'
  return reason
}

// ---- CPU 总使用率 ----
const cpuAvailable = computed(() => isAvailable('cpu.total'))
const cpuRatio = computed(() => sysmon.cpuUsageRatio)
const cpuPercent = computed(() => formatPercent(cpuRatio.value))
const cpuBarWidth = computed(() => {
  const ratio = cpuRatio.value
  if (ratio === null) return '0%'
  return `${Math.min(100, Math.max(0, ratio * 100)).toFixed(1)}%`
})
const cpuEmptyDesc = computed(() => cpuAvailable.value
  ? '暂无可用采样（首次采样未完成或计数器重置）'
  : unavailableDesc('cpu.total'))

// ---- CPU 区块补全（#72）：每核 / 系统负载 / 上下文切换与中断 ----
// 只按 availability.metrics 与数据语义渲染，不按 platform.family 分支。
const cpuCoresAvailable = computed(() => isAvailable('cpu.cores'))
const cpuCores = computed(() => sysmon.cpuCores)
const coresEmptyDesc = computed(() => cpuCoresAvailable.value
  ? '当前快照未提供每核数据'
  : unavailableDesc('cpu.cores'))

const loadAvailable = computed(() => isAvailable('cpu.load_average'))
const loadAverage = computed(() => sysmon.cpuLoadAverage)
const loadHint = computed(() => sysmon.semanticHint('cpu.load_average'))
const loadEmptyDesc = computed(() => loadAvailable.value ? '当前快照未提供负载数据' : unavailableDesc('cpu.load_average'))
// 口径由发送方 semantic 字段承载：processor_queue 只展示单值，不与 load average 同轴比较；
// 契约外的未知 semantic 按 1/5/15 三项数值展示（不与其他口径做同轴比较）。
const PROCESSOR_QUEUE_SEMANTIC = 'processor_queue'
const loadIsProcessorQueue = computed(() => loadAverage.value?.semantic === PROCESSOR_QUEUE_SEMANTIC)

const ratesAvailable = computed(() => isAvailable('cpu.context_switches'))
const contextSwitches = computed(() => sysmon.cpuContextSwitchesPerSec)
const interrupts = computed(() => sysmon.cpuInterruptsPerSec)

// ---- 内存 / 磁盘（卷容量）/ 网络 ----
const memoryAvailable = computed(() => isAvailable('memory'))
const memory = computed(() => snapshot.value?.memory ?? null)
const volumesAvailable = computed(() => isAvailable('volumes'))
const volumes = computed(() => snapshot.value?.volumes ?? null)
const networkAvailable = computed(() => isAvailable('network'))
const network = computed(() => snapshot.value?.network ?? null)
</script>

<template>
  <div class="monitor-view">
    <header class="monitor-view__header">
      <div class="monitor-view__heading">
        <h1 class="monitor-view__title">资源监控</h1>
        <p class="monitor-view__subtitle" data-test="monitor-subtitle">
          <template v-if="snapshot">{{ platformDescription }}<template v-if="updatedAt"> · 更新于 {{ updatedAt }}</template></template>
          <template v-else>系统监控数据尚未到达</template>
        </p>
      </div>
      <span v-if="snapshot" class="ds-tag">实时快照</span>
      <span v-else class="ds-tag ds-tag--warning">等待快照</span>
    </header>

    <!-- 快照尚未到达：整页空态（不得显示 0 或假数字） -->
    <div v-if="!snapshot" class="ds-empty monitor-view__waiting" data-test="monitor-waiting">
      <div class="ds-empty__icon"><Icon name="monitor" :size="20" /></div>
      <div class="ds-empty__title">等待系统监控数据</div>
      <div class="ds-empty__desc">dzsysmon 尚未上报快照，数据到达后本页自动更新</div>
    </div>

    <div v-else class="monitor-view__grid">
      <!-- 系统信息 -->
      <section class="ds-card monitor-view__card monitor-view__card--wide" data-test="monitor-system">
        <h2 class="ds-card__title monitor-view__card-title">
          <span class="monitor-view__card-icon"><Icon name="system" :size="14" /></span>系统信息
        </h2>
        <dl class="monitor-view__info">
          <div class="monitor-view__info-row">
            <dt>主机名</dt>
            <dd>{{ system?.hostname ?? '—' }}</dd>
          </div>
          <div class="monitor-view__info-row">
            <dt>平台</dt>
            <dd>{{ platformDescription || '—' }}</dd>
          </div>
          <div class="monitor-view__info-row">
            <dt>CPU 型号</dt>
            <dd>{{ system?.cpu_model || '—' }}</dd>
          </div>
          <div class="monitor-view__info-row">
            <dt>逻辑核数</dt>
            <dd>{{ system?.logical_cores ?? '—' }}</dd>
          </div>
          <div class="monitor-view__info-row">
            <dt>启动时间</dt>
            <dd>{{ system ? formatDateTime(system.boot_time_unix_sec * 1000) : '—' }}</dd>
          </div>
          <div class="monitor-view__info-row">
            <dt>运行时长</dt>
            <dd>{{ system ? formatUptime(system.uptime_sec) : '—' }}</dd>
          </div>
        </dl>
      </section>

      <!-- CPU（#72 完整化）：总使用率 / 每核（含饱和标记）/ 系统负载 / 上下文切换与中断 -->
      <section class="ds-card monitor-view__card monitor-view__card--wide" data-test="monitor-cpu">
        <h2 class="ds-card__title monitor-view__card-title">
          <span class="monitor-view__card-icon"><Icon name="CPU" :size="14" /></span>CPU
        </h2>
        <div class="monitor-view__cpu">
          <!-- 总使用率 -->
          <div class="monitor-view__cpu-panel">
            <h3 class="monitor-view__panel-title">总使用率</h3>
            <template v-if="cpuRatio !== null">
              <div class="monitor-view__metric" data-test="monitor-cpu-value">{{ cpuPercent }}</div>
              <div
                class="monitor-view__bar"
                role="progressbar"
                :aria-valuenow="Math.round(cpuRatio * 100)"
                aria-valuemin="0"
                aria-valuemax="100"
              >
                <div class="monitor-view__bar-fill" :style="{ width: cpuBarWidth }"></div>
              </div>
              <div class="monitor-view__meta">{{ system?.logical_cores ?? '—' }} 逻辑核</div>
            </template>
            <div v-else class="ds-empty monitor-view__empty" data-test="monitor-cpu-unavailable">
              <div class="ds-empty__title">{{ UNAVAILABLE }}</div>
              <div class="ds-empty__desc">{{ cpuEmptyDesc }}</div>
            </div>
          </div>

          <!-- 系统负载：语义由 semantic 字段 + semantic_hints 承载，不与另一口径同轴比较 -->
          <div class="monitor-view__cpu-panel" data-test="monitor-load">
            <h3 class="monitor-view__panel-title">系统负载</h3>
            <template v-if="loadAverage">
              <dl class="monitor-view__load">
                <div v-if="loadIsProcessorQueue" class="monitor-view__load-item">
                  <dt class="monitor-view__load-label">处理器队列长度</dt>
                  <dd class="monitor-view__load-value" data-test="monitor-load-queue">{{ formatLoadValue(loadAverage.one) }}</dd>
                </div>
                <template v-else>
                  <div class="monitor-view__load-item">
                    <dt class="monitor-view__load-label">1 分钟</dt>
                    <dd class="monitor-view__load-value" data-test="monitor-load-one">{{ formatLoadValue(loadAverage.one) }}</dd>
                  </div>
                  <div class="monitor-view__load-item">
                    <dt class="monitor-view__load-label">5 分钟</dt>
                    <dd class="monitor-view__load-value" data-test="monitor-load-five">{{ formatLoadValue(loadAverage.five) }}</dd>
                  </div>
                  <div class="monitor-view__load-item">
                    <dt class="monitor-view__load-label">15 分钟</dt>
                    <dd class="monitor-view__load-value" data-test="monitor-load-fifteen">{{ formatLoadValue(loadAverage.fifteen) }}</dd>
                  </div>
                </template>
              </dl>
              <p v-if="loadHint" class="monitor-view__hint" data-test="monitor-load-hint">语义提示：{{ loadHint }}</p>
            </template>
            <div v-else class="ds-empty monitor-view__empty" data-test="monitor-load-unavailable">
              <div class="ds-empty__title">{{ UNAVAILABLE }}</div>
              <div class="ds-empty__desc">{{ loadEmptyDesc }}</div>
            </div>
          </div>

          <!-- 上下文切换 / 中断（两者共用可用性键 cpu.context_switches） -->
          <div class="monitor-view__cpu-panel" data-test="monitor-cpu-rates">
            <h3 class="monitor-view__panel-title">上下文切换 / 中断</h3>
            <dl class="monitor-view__rates">
              <div class="monitor-view__rate-item">
                <dt class="monitor-view__rate-label">上下文切换</dt>
                <dd class="monitor-view__rate-value" data-test="monitor-context-switches">{{ formatCountPerSec(contextSwitches) }}</dd>
              </div>
              <div class="monitor-view__rate-item">
                <dt class="monitor-view__rate-label">中断</dt>
                <dd class="monitor-view__rate-value" data-test="monitor-interrupts">{{ formatCountPerSec(interrupts) }}</dd>
              </div>
            </dl>
            <p v-if="!ratesAvailable" class="monitor-view__hint">{{ unavailableDesc('cpu.context_switches') }}</p>
          </div>

          <!-- 每核使用率（核数随快照变化；不可用 / 空数组分别显示明确空态） -->
          <div class="monitor-view__cpu-panel monitor-view__cpu-panel--wide" data-test="monitor-cpu-cores">
            <h3 class="monitor-view__panel-title">每核使用率</h3>
            <ul v-if="cpuCores.length > 0" class="monitor-view__cores">
              <li
                v-for="core in cpuCores"
                :key="core.index"
                class="monitor-view__core"
                :class="{ 'monitor-view__core--saturated': core.saturated }"
                data-test="monitor-core"
                :data-core-index="core.index"
              >
                <span class="monitor-view__core-index">核 {{ core.index }}</span>
                <span class="monitor-view__core-value">{{ formatPercent(core.usage_ratio) }}</span>
                <span
                  v-if="core.saturated"
                  class="ds-tag ds-tag--warning monitor-view__core-tag"
                  data-test="monitor-core-saturated"
                >饱和</span>
              </li>
            </ul>
            <div v-else class="ds-empty monitor-view__empty" data-test="monitor-cores-empty">
              <div class="ds-empty__title">{{ cpuCoresAvailable ? '暂无每核数据' : UNAVAILABLE }}</div>
              <div class="ds-empty__desc">{{ coresEmptyDesc }}</div>
            </div>
          </div>
        </div>
      </section>

      <!-- 内存 -->
      <section class="ds-card monitor-view__card" data-test="monitor-memory">
        <h2 class="ds-card__title monitor-view__card-title">
          <span class="monitor-view__card-icon"><Icon name="MEM" :size="14" /></span>内存
        </h2>
        <template v-if="memoryAvailable && memory">
          <div class="monitor-view__metric" data-test="monitor-memory-value">{{ formatPercent(memory.used_ratio) }}</div>
          <div class="monitor-view__meta">{{ formatBytes(memory.used_bytes) }} / {{ formatBytes(memory.total_bytes) }}</div>
        </template>
        <div v-else class="ds-empty monitor-view__empty" data-test="monitor-memory-unavailable">
          <div class="ds-empty__title">{{ UNAVAILABLE }}</div>
          <div class="ds-empty__desc">{{ unavailableDesc('memory') }}</div>
        </div>
      </section>

      <!-- 磁盘（卷容量） -->
      <section class="ds-card monitor-view__card" data-test="monitor-disk">
        <h2 class="ds-card__title monitor-view__card-title">
          <span class="monitor-view__card-icon"><Icon name="Disk" :size="14" /></span>磁盘
        </h2>
        <ul v-if="volumesAvailable && volumes && volumes.length > 0" class="monitor-view__list">
          <li v-for="volume in volumes" :key="volume.mount" class="monitor-view__list-row">
            <span class="monitor-view__list-label">{{ volume.mount }}</span>
            <span class="monitor-view__list-value">{{ formatPercent(volume.used_ratio) }}</span>
            <span class="monitor-view__list-meta">{{ formatBytes(volume.used_bytes) }} / {{ formatBytes(volume.total_bytes) }}</span>
          </li>
        </ul>
        <div v-else class="ds-empty monitor-view__empty" data-test="monitor-disk-unavailable">
          <div class="ds-empty__title">{{ UNAVAILABLE }}</div>
          <div class="ds-empty__desc">{{ volumesAvailable ? '当前快照未提供卷数据' : unavailableDesc('volumes') }}</div>
        </div>
      </section>

      <!-- 网络 -->
      <section class="ds-card monitor-view__card" data-test="monitor-network">
        <h2 class="ds-card__title monitor-view__card-title">
          <span class="monitor-view__card-icon"><Icon name="network" :size="14" /></span>网络
        </h2>
        <ul v-if="networkAvailable && network && network.length > 0" class="monitor-view__list">
          <li v-for="nic in network" :key="nic.interface" class="monitor-view__list-row">
            <span class="monitor-view__list-label">{{ nic.interface }}</span>
            <span class="monitor-view__list-value">收 {{ formatRate(nic.rx_bytes_per_sec) }}</span>
            <span class="monitor-view__list-meta">发 {{ formatRate(nic.tx_bytes_per_sec) }}</span>
          </li>
        </ul>
        <div v-else class="ds-empty monitor-view__empty" data-test="monitor-network-unavailable">
          <div class="ds-empty__title">{{ UNAVAILABLE }}</div>
          <div class="ds-empty__desc">{{ networkAvailable ? '当前快照未提供网卡数据' : unavailableDesc('network') }}</div>
        </div>
      </section>
    </div>
  </div>
</template>

<style scoped>
.monitor-view {
  flex: 1;
  min-height: 0;
  overflow: auto;
  padding: var(--spacer-32) var(--spacer-24);
}

.monitor-view__header {
  display: flex;
  align-items: flex-start;
  justify-content: space-between;
  gap: var(--spacer-16);
  margin-bottom: var(--spacer-24);
  flex-wrap: wrap;
}

.monitor-view__title {
  margin: 0 0 var(--spacer-4);
  font-size: var(--heading-xl-font-size);
  line-height: var(--heading-xl-line-height);
  font-weight: var(--heading-xl-font-weight);
  color: var(--text-default);
}

.monitor-view__subtitle {
  margin: 0;
  font-size: var(--body-sm-font-size);
  line-height: var(--body-sm-line-height);
  color: var(--text-tertiary);
}

.monitor-view__waiting {
  max-width: 720px;
  margin: 0 auto;
}

.monitor-view__grid {
  display: grid;
  grid-template-columns: repeat(2, minmax(0, 1fr));
  gap: var(--spacer-16);
}

.monitor-view__card--wide {
  grid-column: 1 / -1;
}

.monitor-view__card-title {
  display: flex;
  align-items: center;
  gap: var(--spacer-6);
}

.monitor-view__card-icon {
  display: inline-flex;
  align-items: center;
  color: var(--icon-secondary);
}

.monitor-view__info {
  display: grid;
  grid-template-columns: repeat(2, minmax(0, 1fr));
  gap: var(--spacer-12) var(--spacer-24);
  margin: 0;
}

.monitor-view__info-row {
  display: flex;
  gap: var(--spacer-8);
  min-width: 0;
}

.monitor-view__info-row dt {
  flex: 0 0 auto;
  min-width: var(--spacer-64);
  color: var(--text-tertiary);
  font-size: var(--body-sm-font-size);
  line-height: var(--body-sm-line-height);
}

.monitor-view__info-row dd {
  flex: 1;
  min-width: 0;
  margin: 0;
  color: var(--text-default);
  font-size: var(--body-sm-font-size);
  line-height: var(--body-sm-line-height);
  overflow-wrap: anywhere;
}

.monitor-view__metric {
  font-family: var(--font-family-metric);
  font-size: var(--heading-xl-font-size);
  line-height: var(--heading-xl-line-height);
  font-weight: var(--heading-xl-font-weight);
  color: var(--text-default);
  font-variant-numeric: tabular-nums;
}

.monitor-view__meta {
  font-size: var(--body-xs-font-size);
  line-height: var(--body-xs-line-height);
  color: var(--text-tertiary);
}

.monitor-view__bar {
  width: 100%;
  height: var(--spacer-6);
  margin: var(--spacer-8) 0 var(--spacer-4);
  border-radius: var(--radius-full);
  background: var(--bg-overlay-l2);
  overflow: hidden;
}

.monitor-view__bar-fill {
  height: 100%;
  border-radius: var(--radius-full);
  background: var(--status-primary-default);
  transition: width 0.2s ease;
}

.monitor-view__empty {
  padding: var(--spacer-20) var(--spacer-12);
}

/* ---- CPU 区块（#72）：总量 / 负载 / 速率面板 + 每核网格 ---- */
.monitor-view__cpu {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(200px, 1fr));
  gap: var(--spacer-16) var(--spacer-24);
}

.monitor-view__cpu-panel {
  min-width: 0;
}

.monitor-view__cpu-panel--wide {
  grid-column: 1 / -1;
}

.monitor-view__panel-title {
  margin: 0 0 var(--spacer-8);
  color: var(--text-secondary);
  font-size: var(--body-sm-font-size);
  line-height: var(--body-sm-line-height);
  font-weight: var(--body-sm-strong-font-weight);
}

.monitor-view__cores {
  display: grid;
  grid-template-columns: repeat(auto-fill, minmax(160px, 1fr));
  gap: var(--spacer-8);
  margin: 0;
  padding: 0;
  list-style: none;
}

.monitor-view__core {
  display: flex;
  align-items: center;
  gap: var(--spacer-6);
  min-width: 0;
  padding: var(--spacer-4) var(--spacer-8);
  border: 1px solid var(--border-neutral-l1);
  border-radius: var(--radius-6);
}

/* 持续饱和核：警示样式 + 「饱和」标签（颜色/背景均取状态 token） */
.monitor-view__core--saturated {
  border-color: var(--status-warning-default);
  background: var(--status-warning-surface-l1);
}

.monitor-view__core-index {
  color: var(--text-tertiary);
  font-size: var(--body-xs-font-size);
  line-height: var(--body-xs-line-height);
  white-space: nowrap;
}

.monitor-view__core-value {
  margin-left: auto;
  color: var(--text-default);
  font-family: var(--font-family-metric);
  font-size: var(--body-sm-font-size);
  line-height: var(--body-sm-line-height);
  font-variant-numeric: tabular-nums;
  white-space: nowrap;
}

.monitor-view__core-tag {
  flex: 0 0 auto;
}

.monitor-view__load,
.monitor-view__rates {
  display: flex;
  flex-wrap: wrap;
  gap: var(--spacer-8) var(--spacer-16);
  margin: 0;
}

.monitor-view__load-item,
.monitor-view__rate-item {
  display: flex;
  flex-direction: column;
  gap: var(--spacer-2);
  min-width: 0;
}

.monitor-view__load-label,
.monitor-view__rate-label {
  color: var(--text-tertiary);
  font-size: var(--body-xs-font-size);
  line-height: var(--body-xs-line-height);
}

.monitor-view__load-value,
.monitor-view__rate-value {
  margin: 0;
  color: var(--text-default);
  font-family: var(--font-family-metric);
  font-size: var(--body-base-font-size);
  line-height: var(--body-base-line-height);
  font-variant-numeric: tabular-nums;
}

.monitor-view__hint {
  margin: var(--spacer-8) 0 0;
  color: var(--text-tertiary);
  font-size: var(--body-xs-font-size);
  line-height: var(--body-xs-line-height);
}

.monitor-view__list {
  list-style: none;
  margin: 0;
  padding: 0;
  display: flex;
  flex-direction: column;
  gap: var(--spacer-8);
}

.monitor-view__list-row {
  display: flex;
  align-items: baseline;
  gap: var(--spacer-8);
  min-width: 0;
}

.monitor-view__list-label {
  color: var(--text-default);
  font-size: var(--body-sm-font-size);
  line-height: var(--body-sm-line-height);
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.monitor-view__list-value {
  color: var(--text-default);
  font-size: var(--body-sm-font-size);
  line-height: var(--body-sm-line-height);
  font-variant-numeric: tabular-nums;
}

.monitor-view__list-meta {
  margin-left: auto;
  color: var(--text-tertiary);
  font-size: var(--body-xs-font-size);
  line-height: var(--body-xs-line-height);
  font-variant-numeric: tabular-nums;
}

@media (max-width: 767.98px) {
  .monitor-view {
    padding: var(--spacer-24) var(--spacer-16);
  }

  .monitor-view__grid {
    grid-template-columns: 1fr;
  }

  .monitor-view__card--wide {
    grid-column: auto;
  }

  .monitor-view__info {
    grid-template-columns: 1fr;
  }

  .monitor-view__cpu {
    grid-template-columns: 1fr;
  }

  .monitor-view__cpu-panel--wide {
    grid-column: auto;
  }
}
</style>
