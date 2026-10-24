<script setup lang="ts">
import { computed, onMounted, ref } from 'vue'
import Icon from '@/components/shared/Icon.vue'
import SyncSwitch from '@/components/shared/SyncSwitch.vue'
import SysmonHistoryTab from '@/components/sysmon/SysmonHistoryTab.vue'
import { PENDING_TIMEOUT } from '@/composables/usePending'
import { useAuthStore } from '@/stores/auth'
import { useSysmonStore, type SysmonActiveAlert } from '@/stores/sysmon'
import { useToastStore } from '@/stores/toast'

// 资源监控页：系统信息与 CPU 区块（总量 / 每核 / 系统负载 / 上下文切换与中断 / 每核频率）、
// 温度、内存与交换、磁盘（卷容量 / 关注路径 / 共享内存目录）接契约 sysmon 实时快照；
// 网络区块（#77）接每网卡收发速率与累计量，并提供「显示全部网卡」开关。
// 内存 / 交换（#74）按可用性映射渲染：不可用显示「不可用」+ 原因，可用但本帧无数据
// 显示「暂无数据」；交换在可用且总容量为 0 时显示「无交换分区」。
// 温度 / 每核频率（B 组）按可用性映射渲染：不可用时整块不渲染（不是显示 0 / 不可用占位）。
// 磁盘（#75）按可用性映射渲染：卷按 watched 优先排序并高亮关注卷；单个卷 available=false
// 显示「不可用」+ 原因而不是 0；shm_dir 展示共享内存目录占用（页清理是否生效的可观测项）。
// 磁盘 I/O（#76）按可用性映射渲染：每物理盘的吞吐 / IOPS / 延迟 / 队列深度 / 利用率，
// 数值 null（首次采样/计数器重置）逐项显示「不可用」，能力不可用时整块显示原因。
// 网络（#77）：可用但数组为空 = 发送方按 hide_virtual 滤掉了全部虚拟网卡（契约 §2.5 明文，
// **不是**不可用），显示「已隐藏全部虚拟网卡」+ 切换提示；开关走 REST 下发、以 WS sysmon_config
// 回推为生效信号。
// 阈值告警（#80）：快照 alerts[] 是持续状态，页内以常驻横幅展示（冷却期内通知被抑制时仍在）；
// 触发/恢复的事件通知走既有 notify_ui 链路（触发 error+popup、恢复 info），本页不重复弹 toast。
// 历史视图（#79）为独立视图：切换只影响渲染来源，不卸载/不重建实时 store，
// 因此 WS 连接与实时快照推送完全不受影响（契约 sysmon §8：历史视图不订阅 WS）。
// 刷新基线由发送方 realtime.push_interval_sec（默认 5s）决定，本页不轮询、不改 WS 契约。
const sysmon = useSysmonStore()

/** 视图模式：实时（默认） / 历史 */
const viewMode = ref<'realtime' | 'history'>('realtime')

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
/** 配置关闭的统一文案（卷 / 指标可用性映射共用，避免两处字面量漂移） */
const DISABLED_BY_CONFIG_TEXT = '已在配置中关闭'

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

/** 2 位小数格式化（系统负载 / 处理器队列长度 / 磁盘队列深度）；null 显示不可用（不得用 0 冒充） */
function formatLoadValue(value: number | null | undefined): string {
  if (typeof value !== 'number' || !Number.isFinite(value)) return UNAVAILABLE
  return value.toFixed(2)
}

/** 每秒 I/O 操作数（IOPS）：整数 + 单位；null 一律显示不可用（不得用 0 冒充） */
function formatIops(value: number | null | undefined): string {
  if (typeof value !== 'number' || !Number.isFinite(value)) return UNAVAILABLE
  return `${Math.round(value)} IOPS`
}

/** 平均每请求延迟（毫秒）：保留 2 位小数 + 单位；null 一律显示不可用 */
function formatLatency(ms: number | null | undefined): string {
  if (typeof ms !== 'number' || !Number.isFinite(ms)) return UNAVAILABLE
  return `${ms.toFixed(2)} ms`
}

/** 速率型计数（每秒上下文切换 / 中断）：整数值 + /s；null 显示不可用 */
function formatCountPerSec(value: number | null | undefined): string {
  if (typeof value !== 'number' || !Number.isFinite(value)) return UNAVAILABLE
  return `${Math.round(value)}/s`
}

/** 温度（摄氏度）：保留 1 位小数 + 单位；调用方只在指标可用时传入有效值 */
function formatTemperature(celsius: number): string {
  return `${celsius.toFixed(1)} °C`
}

/** 每核频率（MHz）：整数 + 单位；调用方只在指标可用时传入有效值 */
function formatFrequency(mhz: number): string {
  return `${mhz} MHz`
}

/** 文件年龄（秒 → 可读时长）：页文件年龄是页清理是否生效的直接信号（契约 sysmon §2.4） */
function formatAge(seconds: number): string {
  if (!Number.isFinite(seconds) || seconds < 0) return UNAVAILABLE
  const total = Math.floor(seconds)
  if (total < 60) return `${total} 秒`
  if (total < 3_600) return `${Math.floor(total / 60)} 分钟`
  if (total < 86_400) return `${Math.floor(total / 3_600)} 小时`
  return `${Math.floor(total / 86_400)} 天`
}

/** 卷 / 关注路径的不可用原因（契约 sysmon §2.4 建议取值）：文案化，裸 reason 兜底 */
function volumeReasonDesc(reason: string | undefined): string {
  if (!reason) return '容量读取失败'
  if (reason === 'no_permission') return '无访问权限'
  if (reason === 'path_not_found') return '路径不存在或无法解析'
  if (reason === 'collect_failed') return '采集失败'
  if (reason === 'disabled_by_config') return DISABLED_BY_CONFIG_TEXT
  return reason
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
  // reason 为契约约定的自由文案（建议取值 not_implemented/unsupported/no_permission/no_sensor/
  // collect_failed/disabled_by_config）
  if (reason === 'not_implemented') return '该指标将在后续版本接入'
  if (reason === 'collect_failed') return '采集失败，暂不可用'
  if (reason === 'disabled_by_config') return DISABLED_BY_CONFIG_TEXT
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

// ---- 温度 / 每核频率（#73，B 组可用性样板）----
// 与内存 / 磁盘等「不可用显示空态」的区块不同：对应指标不可用时整块不渲染，
// 不是显示 0、也不是显示「不可用」占位。只按 availability.metrics 与数据渲染，不按 platform.family 分支。
const temperatureAvailable = computed(() => isAvailable('temperature'))
const temperatures = computed(() => sysmon.temperatures)
const frequencyAvailable = computed(() => isAvailable('cpu.frequency'))
const coreFrequencies = computed(() => sysmon.coreFrequencies)

// ---- 内存 / 交换（#74）/ 磁盘（#75：卷容量 / 关注路径 / 共享内存目录）/ 网络 ----
// 内存 / 交换 / 磁盘一律走 store 的净化 computed（可用性 + 数值防御），组件不直接读 payload 字段。
const memoryAvailable = computed(() => isAvailable('memory'))
const memory = computed(() => sysmon.memoryInfo)
const swapAvailable = computed(() => isAvailable('swap'))
const swapInfo = computed(() => sysmon.swapInfo)
// 无交换分区是平台事实（契约 sysmon §2.5 的 unsupported），不是采集故障：给出明确文案，
// 其余不可用原因仍按 unavailableDesc 展示。
const swapUnavailableDesc = computed(() => {
  if (sysmon.availabilityOf('swap')?.reason === 'unsupported') {
    return '本机无交换分区/页面文件'
  }
  return unavailableDesc('swap')
})
const volumesAvailable = computed(() => isAvailable('volumes'))
const volumes = computed(() => sysmon.volumes)
const watchPaths = computed(() => sysmon.watchPaths)
const shmAvailable = computed(() => isAvailable('shm_dir'))
const shmDir = computed(() => sysmon.shmDirInfo)
// ---- 磁盘 I/O（#76）：每物理盘吞吐 / IOPS / 延迟 / 队列深度 / 利用率，按可用性映射渲染 ----
// 只在 availability.metrics 上分支，不按 platform.family 分支；数值 null（首次采样/计数器重置）
// 逐项显示「不可用」，绝不显示 0。
const diskIoAvailable = computed(() => isAvailable('disk_io'))
const diskIo = computed(() => sysmon.diskIo)
const diskIoHint = computed(() => sysmon.semanticHint('disk_io'))
// 磁盘性能计数器最常见的不可用原因（Windows 非管理员 / 计数器未开启）给出可操作文案；
// 其余 reason 走既有 unavailableDesc。
const diskIoUnavailableDesc = computed(() => {
  // no_permission 在磁盘 I/O 语境下是「性能计数器需特权/未开启」，给出可操作文案；
  // 其余原因（disabled_by_config / collect_failed / 未知）统一走既有 unavailableDesc。
  if (sysmon.availabilityOf('disk_io')?.reason === 'no_permission') {
    return '磁盘性能计数器不可用（需管理员权限或性能计数器未开启）'
  }
  return unavailableDesc('disk_io')
})
// ---- 网络（#77）：每网卡收发速率 / 累计量 / 错误 / 丢弃，并按可用性映射渲染 ----
// 与磁盘 I/O 同构：只在 availability.metrics 上分支，不按 platform.family 分支；
// 六个速率字段 null（首次采样/本帧新出现/计数器重置）逐项显示「不可用」，绝不显示 0。
// hide_virtual=true 时发送方已滤掉虚拟网卡；全部被滤掉时 payload 是空数组且 available=true，
// 此时显示「已隐藏全部虚拟网卡」而不是「不可用」（契约 sysmon §2.5 明文要求区分）。
const networkAvailable = computed(() => isAvailable('network'))
const network = computed(() => sysmon.network)
// 「显示全部网卡」开关：admin 才可操作（复用 auth store 的 isAdmin）；实际值取生效配置镜像
// （契约 sysmon §3.1 network.hide_virtual），切换后等 WS sysmon_config 回推才变更。
const auth = useAuthStore()
const toast = useToastStore()
const canToggleNetwork = computed(() => auth.isAdmin)
const hideVirtual = computed(() => sysmon.hideVirtual)
const showAllNics = computed(() => !hideVirtual.value)
const hideVirtualPending = computed(() => sysmon.hideVirtualPending)
// 可用但空数组 = 发送方按 hide_virtual 滤掉了全部虚拟网卡（契约 §2.5）。
// 仅当 hideVirtual 为真时才能这么解释；hide_virtual=false 却为空数组 = 本机确实无网卡
//（罕见，容器内可能），走「暂无数据」而不是「已隐藏」。
const networkHiddenAll = computed(() => sysmon.networkFilteredAll && hideVirtual.value)

// ---- 阈值告警（#80）：快照 alerts[] 的持续状态 ----
// 通知（触发 error+popup / 恢复 info）走既有 notify_ui 链路；本区块只做**常驻**提示：
// 冷却期内通知被抑制时它仍在，恢复后随下一个快照自动消失（契约 sysmon §2.8）。
const activeAlerts = computed<SysmonActiveAlert[]>(() => sysmon.activeAlerts)

/** 告警条目文案：kind + 当前快照数值（阈值细节在触发通知正文里，不重复展示） */
function alertText(alert: SysmonActiveAlert): string {
  if (alert.kind === 'memory_used') {
    const ratio = memory.value?.used_ratio
    return ratio == null ? '内存使用率达到告警阈值' : `内存使用率 ${formatPercent(ratio)} 超过告警阈值`
  }
  if (alert.kind === 'swap_used') {
    const ratio = swapInfo.value?.used_ratio
    return ratio == null ? '交换使用率达到告警阈值' : `交换使用率 ${formatPercent(ratio)} 超过告警阈值`
  }
  // volume_free：subject 为挂载点（契约 sysmon §2.8），剩余空间取同帧卷数据；读不到就不猜数值
  const volume = volumes.value.find(entry => entry.mount === alert.subject)
  if (volume && volume.available && volume.total_bytes != null && volume.used_bytes != null) {
    const free = volume.total_bytes - volume.used_bytes
    return `关注卷 ${alert.subject} 剩余空间不足（剩余 ${formatBytes(free)}）`
  }
  return `关注卷 ${alert.subject} 剩余空间低于告警阈值`
}
// 开关语义是「显示全部网卡」(= hide_virtual 取反)，直接下发取反后的 hide_virtual 值。
// 结果契约（对齐 settings/mdConfig 的配置类操作）：true = 已下发（等 WS sysmon_config 生效）、
// false = 防重入或 HTTP 失败、PENDING_TIMEOUT = 超时（usePending 已 toast，此处不重复弹）。
// 失败必须有可见反馈：否则用户点了开关、界面无变化也无提示（静默失败）。
async function onToggleShowAll(next: boolean): Promise<void> {
  const result = await sysmon.setHideVirtual(!next)
  if (result === PENDING_TIMEOUT) return
  if (result === false) toast.error('显示全部网卡切换下发失败')
}

// ---- 网卡累计量 / 速率格式化 ----
/** 累计计数器（包 / 错误 / 丢弃）：千分位整数；累计量没有"不可用"语义 */
function formatTotalCount(value: number): string {
  return value.toLocaleString('en-US')
}
/** 错误 / 丢弃速率（个每秒）：保留 1 位小数（可为 0 = 本窗口确实没有增量）；null 显示不可用 */
function formatEventRate(value: number | null | undefined): string {
  if (typeof value !== 'number' || !Number.isFinite(value)) return UNAVAILABLE
  return `${value.toFixed(1)}/s`
}
/**
 * 错误 / 丢弃单元格：速率 + 累计量并列（速率反映当前窗口，累计量反映是否曾经发生过）。
 * 速率 null（首次采样/计数器重置）显示「不可用」而不是 0；累计量始终有效（累计计数器没有
 * 不可用语义），因此即便速率不可用仍能看到历史累计。
 */
function formatEventRateWithTotal(rate: number | null | undefined, total: number): string {
  return `${formatEventRate(rate)}（累计 ${formatTotalCount(total)}）`
}
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
      <div class="monitor-view__view-switch" role="tablist" data-test="monitor-view-switch">
        <button
          class="ds-btn ds-btn--sm"
          :class="viewMode === 'realtime' ? 'ds-btn--primary' : 'ds-btn--secondary'"
          type="button"
          role="tab"
          :aria-selected="viewMode === 'realtime'"
          data-test="monitor-tab-realtime"
          @click="viewMode = 'realtime'"
        >实时</button>
        <button
          class="ds-btn ds-btn--sm"
          :class="viewMode === 'history' ? 'ds-btn--primary' : 'ds-btn--secondary'"
          type="button"
          role="tab"
          :aria-selected="viewMode === 'history'"
          data-test="monitor-tab-history"
          @click="viewMode = 'history'"
        >历史</button>
      </div>
      <span v-if="viewMode === 'realtime' && snapshot" class="ds-tag">实时快照</span>
      <span v-else-if="viewMode === 'realtime'" class="ds-tag ds-tag--warning">等待快照</span>
    </header>

    <!-- 历史视图（#79）：独立渲染，不卸载实时 store（WS 连接与实时数据不受影响） -->
    <SysmonHistoryTab v-if="viewMode === 'history'" />

    <!-- 实时视图（默认）：快照尚未到达时整页空态（不得显示 0 或假数字） -->
    <div
      v-if="viewMode === 'realtime' && !snapshot"
      class="ds-empty monitor-view__waiting"
      data-test="monitor-waiting"
    >
      <div class="ds-empty__icon"><Icon name="monitor" :size="20" /></div>
      <div class="ds-empty__title">等待系统监控数据</div>
      <div class="ds-empty__desc">dzsysmon 尚未上报快照，数据到达后本页自动更新</div>
    </div>

    <div v-else-if="viewMode === 'realtime'" class="monitor-view__grid">
      <!-- 阈值告警（#80）：持续状态——冷却期内通知被抑制时仍在，恢复后自动消失；
           触发/恢复的事件通知走既有 notify_ui 链路（error+popup / info），这里不重复弹 toast -->
      <section
        v-if="activeAlerts.length > 0"
        class="ds-alert ds-alert--danger monitor-view__alerts"
        data-test="monitor-alerts"
        role="alert"
      >
        <span class="ds-alert__icon"><Icon name="warning_line" :size="16" /></span>
        <div class="monitor-view__alerts-body">
          <div class="ds-alert__title" data-test="monitor-alerts-title">
            资源告警（{{ activeAlerts.length }}）
          </div>
          <ul class="monitor-view__alerts-list">
            <li
              v-for="alert in activeAlerts"
              :key="`${alert.kind}|${alert.subject}`"
              class="monitor-view__alert-item"
              data-test="monitor-alert"
              :data-kind="alert.kind"
              :data-subject="alert.subject"
            >
              <span>{{ alertText(alert) }}</span>
              <span v-if="alert.since > 0" class="monitor-view__alert-since" data-test="monitor-alert-since">
                始于 {{ formatDateTime(alert.since * 1000) }}
              </span>
            </li>
          </ul>
        </div>
      </section>

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

          <!-- 每核频率（#73）：仅在 cpu.frequency 可用时渲染整块面板；不可用时整块不渲染 -->
          <div
            v-if="frequencyAvailable"
            class="monitor-view__cpu-panel monitor-view__cpu-panel--wide"
            data-test="monitor-frequency"
          >
            <h3 class="monitor-view__panel-title">每核频率</h3>
            <ul v-if="coreFrequencies.length > 0" class="monitor-view__cores">
              <li
                v-for="frequency in coreFrequencies"
                :key="frequency.index"
                class="monitor-view__core"
                data-test="monitor-frequency-core"
                :data-core-index="frequency.index"
              >
                <span class="monitor-view__core-index">核 {{ frequency.index }}</span>
                <span class="monitor-view__core-value">{{ formatFrequency(frequency.mhz) }}</span>
              </li>
            </ul>
            <div v-else class="ds-empty monitor-view__empty" data-test="monitor-frequency-empty">
              <div class="ds-empty__title">暂无数据</div>
              <div class="ds-empty__desc">当前快照未提供每核频率</div>
            </div>
          </div>
        </div>
      </section>

      <!-- 温度（#73）：仅在 temperature 可用时渲染整块卡片；不可用整块不渲染（不是显示 0） -->
      <section v-if="temperatureAvailable" class="ds-card monitor-view__card" data-test="monitor-temperature">
        <h2 class="ds-card__title monitor-view__card-title">
          <span class="monitor-view__card-icon"><Icon name="system" :size="14" /></span>温度
        </h2>
        <ul v-if="temperatures.length > 0" class="monitor-view__list">
          <li
            v-for="reading in temperatures"
            :key="reading.sensor"
            class="monitor-view__list-row"
            data-test="monitor-temperature-sensor"
          >
            <span class="monitor-view__list-label">{{ reading.sensor }}</span>
            <span class="monitor-view__list-value">{{ formatTemperature(reading.celsius) }}</span>
          </li>
        </ul>
        <div v-else class="ds-empty monitor-view__empty" data-test="monitor-temperature-empty">
          <div class="ds-empty__title">暂无数据</div>
          <div class="ds-empty__desc">当前快照未提供温度读数</div>
        </div>
      </section>

      <!-- 内存 / 交换（#74）：内存数值 + 卡片内交换子区块；不可用一律显式空态，不得显示 0 -->
      <section class="ds-card monitor-view__card" data-test="monitor-memory">
        <h2 class="ds-card__title monitor-view__card-title">
          <span class="monitor-view__card-icon"><Icon name="MEM" :size="14" /></span>内存
        </h2>
        <template v-if="memory">
          <div class="monitor-view__metric" data-test="monitor-memory-value">{{ formatPercent(memory.used_ratio) }}</div>
          <div class="monitor-view__meta" data-test="monitor-memory-meta">{{ formatBytes(memory.used_bytes) }} / {{ formatBytes(memory.total_bytes) }}</div>
          <ul class="monitor-view__list monitor-view__subsection">
            <li class="monitor-view__list-row">
              <span class="monitor-view__list-label">可用</span>
              <span class="monitor-view__list-value" data-test="monitor-memory-available">{{ formatBytes(memory.available_bytes) }}</span>
            </li>
            <!-- 缓存 / 缓冲：null = 本平台不提供/本帧未读到，不渲染该行（Windows 恒无缓冲） -->
            <li v-if="memory.cached_bytes != null" class="monitor-view__list-row">
              <span class="monitor-view__list-label">缓存</span>
              <span class="monitor-view__list-value" data-test="monitor-memory-cached">{{ formatBytes(memory.cached_bytes) }}</span>
            </li>
            <li v-if="memory.buffers_bytes != null" class="monitor-view__list-row">
              <span class="monitor-view__list-label">缓冲</span>
              <span class="monitor-view__list-value" data-test="monitor-memory-buffers">{{ formatBytes(memory.buffers_bytes) }}</span>
            </li>
          </ul>
        </template>
        <div v-else-if="memoryAvailable" class="ds-empty monitor-view__empty" data-test="monitor-memory-empty">
          <div class="ds-empty__title">暂无数据</div>
          <div class="ds-empty__desc">当前快照未提供内存数据</div>
        </div>
        <div v-else class="ds-empty monitor-view__empty" data-test="monitor-memory-unavailable">
          <div class="ds-empty__title">{{ UNAVAILABLE }}</div>
          <div class="ds-empty__desc">{{ unavailableDesc('memory') }}</div>
        </div>

        <!-- 交换子区块：可用且总量 > 0 → 使用率 + 已用/总量；可用但总量 0 → 无交换分区；
             可用但本帧无对象 → 暂无数据；不可用 → 不可用 + 原因（不得显示 0） -->
        <div class="monitor-view__subsection" data-test="monitor-swap">
          <h3 class="monitor-view__panel-title">交换</h3>
          <ul v-if="swapInfo && swapInfo.total_bytes > 0" class="monitor-view__list" data-test="monitor-memory-swap">
            <li class="monitor-view__list-row">
              <span class="monitor-view__list-label">使用率</span>
              <span class="monitor-view__list-value">{{ formatPercent(swapInfo.used_ratio) }}</span>
            </li>
            <li class="monitor-view__list-row">
              <span class="monitor-view__list-label">已用 / 总量</span>
              <span class="monitor-view__list-meta">{{ formatBytes(swapInfo.used_bytes) }} / {{ formatBytes(swapInfo.total_bytes) }}</span>
            </li>
          </ul>
          <div
            v-else-if="swapAvailable && swapInfo"
            class="monitor-view__meta"
            data-test="monitor-memory-swap-none"
          >无交换分区</div>
          <div v-else-if="swapAvailable" class="ds-empty monitor-view__empty" data-test="monitor-memory-swap-empty">
            <div class="ds-empty__title">暂无数据</div>
            <div class="ds-empty__desc">当前快照未提供交换数据</div>
          </div>
          <div v-else class="ds-empty monitor-view__empty" data-test="monitor-memory-swap-unavailable">
            <div class="ds-empty__title">{{ UNAVAILABLE }}</div>
            <div class="ds-empty__desc">{{ swapUnavailableDesc }}</div>
          </div>
        </div>
      </section>

      <!-- 磁盘（#75：卷容量 / 关注路径 / 共享内存目录），全部按 availability.metrics 渲染 -->
      <section class="ds-card monitor-view__card" data-test="monitor-disk">
        <h2 class="ds-card__title monitor-view__card-title">
          <span class="monitor-view__card-icon"><Icon name="Disk" :size="14" /></span>磁盘
        </h2>

        <!-- 卷：watched=true 高亮并排在前（store 已按 watched 优先排序）；
             available=false 的卷显示「不可用」+ 原因，绝不显示 0 -->
        <div class="monitor-view__subsection" data-test="monitor-disk-volumes">
          <h3 class="monitor-view__panel-title">卷容量</h3>
          <ul v-if="volumesAvailable && volumes.length > 0" class="monitor-view__list">
            <li
              v-for="volume in volumes"
              :key="volume.mount"
              class="monitor-view__list-row monitor-view__disk-row"
              :class="{ 'monitor-view__disk-row--watched': volume.watched }"
              data-test="monitor-disk-volume"
              :data-mount="volume.mount"
            >
              <span class="monitor-view__list-label">{{ volume.mount }}</span>
              <span v-if="volume.watched" class="ds-tag ds-tag--warning" data-test="monitor-disk-volume-watched">关注</span>
              <template v-if="volume.available">
                <!-- 可用但比率为 null（total_bytes=0 的异常/特殊卷）：显示「—」而不是 0% -->
                <span class="monitor-view__list-value">{{ volume.used_ratio === null ? '—' : formatPercent(volume.used_ratio) }}</span>
                <span class="monitor-view__list-meta">{{ formatBytes(volume.used_bytes) }} / {{ formatBytes(volume.total_bytes) }}</span>
              </template>
              <template v-else>
                <span class="monitor-view__list-value">{{ UNAVAILABLE }}</span>
                <span class="monitor-view__list-meta" data-test="monitor-disk-volume-reason">{{ volumeReasonDesc(volume.reason) }}</span>
              </template>
            </li>
          </ul>
          <div v-else class="ds-empty monitor-view__empty" data-test="monitor-disk-unavailable">
            <div class="ds-empty__title">{{ UNAVAILABLE }}</div>
            <div class="ds-empty__desc">{{ volumesAvailable ? '当前快照未提供卷数据' : unavailableDesc('volumes') }}</div>
          </div>

          <!-- 关注路径：落卷解析失败时显示「未解析」+ 原因（不猜卷、不回退到 /） -->
          <ul
            v-if="watchPaths.length > 0"
            class="monitor-view__list monitor-view__subsection"
            data-test="monitor-watch-paths"
          >
            <li
              v-for="watchPath in watchPaths"
              :key="watchPath.path"
              class="monitor-view__list-row"
              data-test="monitor-watch-path"
            >
              <span class="monitor-view__list-label">{{ watchPath.path }}</span>
              <span v-if="watchPath.available" class="monitor-view__list-meta">落卷 {{ watchPath.mount }}</span>
              <span v-else class="monitor-view__list-meta" data-test="monitor-watch-path-unavailable">
                未解析 · {{ volumeReasonDesc(watchPath.reason) }}
              </span>
            </li>
          </ul>
        </div>

        <!-- 共享内存目录占用（页清理是否生效的可观测项）：不可用 / 可用但本帧未采集分别空态 -->
        <div class="monitor-view__subsection" data-test="monitor-shm-dir">
          <h3 class="monitor-view__panel-title">共享内存目录</h3>
          <template v-if="shmDir">
            <dl class="monitor-view__list">
              <div class="monitor-view__list-row">
                <dt class="monitor-view__list-label">路径</dt>
                <dd class="monitor-view__list-value" data-test="monitor-shm-path">{{ shmDir.path }}</dd>
              </div>
              <div class="monitor-view__list-row">
                <dt class="monitor-view__list-label">占用</dt>
                <dd class="monitor-view__list-value" data-test="monitor-shm-total">{{ formatBytes(shmDir.total_bytes) }}</dd>
              </div>
              <div class="monitor-view__list-row">
                <dt class="monitor-view__list-label">页文件</dt>
                <dd class="monitor-view__list-value" data-test="monitor-shm-page-files">{{ shmDir.page_files }}</dd>
              </div>
              <!-- 无页文件时 oldest_file_age_sec 为 null：不渲染该行（不是 0 秒） -->
              <div v-if="shmDir.oldest_file_age_sec != null" class="monitor-view__list-row">
                <dt class="monitor-view__list-label">最旧页文件</dt>
                <dd class="monitor-view__list-value" data-test="monitor-shm-oldest-age">{{ formatAge(shmDir.oldest_file_age_sec) }}</dd>
              </div>
            </dl>
          </template>
          <div v-else-if="shmAvailable" class="ds-empty monitor-view__empty" data-test="monitor-shm-empty">
            <div class="ds-empty__title">暂无数据</div>
            <div class="ds-empty__desc">当前快照未提供共享内存目录占用</div>
          </div>
          <div v-else class="ds-empty monitor-view__empty" data-test="monitor-shm-unavailable">
            <div class="ds-empty__title">{{ UNAVAILABLE }}</div>
            <div class="ds-empty__desc">{{ unavailableDesc('shm_dir') }}</div>
          </div>
        </div>
      </section>

      <!-- 磁盘 I/O（#76）：按 availability.metrics 渲染；每盘吞吐 / IOPS / 延迟 / 队列深度 /
           利用率，任一数值 null 逐项显示「不可用」（首次采样/计数器重置，绝不显示 0） -->
      <section class="ds-card monitor-view__card" data-test="monitor-disk-io">
        <h2 class="ds-card__title monitor-view__card-title">
          <span class="monitor-view__card-icon"><Icon name="Disk" :size="14" /></span>磁盘 I/O
        </h2>

        <template v-if="diskIoAvailable">
          <ul v-if="diskIo.length > 0" class="monitor-view__io-list">
            <li
              v-for="device in diskIo"
              :key="device.device"
              class="monitor-view__io-row"
              data-test="monitor-disk-io-row"
              :data-device="device.device"
            >
              <div class="monitor-view__io-device">{{ device.device }}</div>
              <dl class="monitor-view__io-metrics">
                <div class="monitor-view__io-metric">
                  <dt class="monitor-view__io-label">读吞吐</dt>
                  <dd class="monitor-view__io-value">{{ formatRate(device.read_bytes_per_sec) }}</dd>
                </div>
                <div class="monitor-view__io-metric">
                  <dt class="monitor-view__io-label">写吞吐</dt>
                  <dd class="monitor-view__io-value">{{ formatRate(device.write_bytes_per_sec) }}</dd>
                </div>
                <div class="monitor-view__io-metric">
                  <dt class="monitor-view__io-label">读 IOPS</dt>
                  <dd class="monitor-view__io-value">{{ formatIops(device.read_iops) }}</dd>
                </div>
                <div class="monitor-view__io-metric">
                  <dt class="monitor-view__io-label">写 IOPS</dt>
                  <dd class="monitor-view__io-value">{{ formatIops(device.write_iops) }}</dd>
                </div>
                <div class="monitor-view__io-metric">
                  <dt class="monitor-view__io-label">延迟</dt>
                  <dd class="monitor-view__io-value">{{ formatLatency(device.latency_ms) }}</dd>
                </div>
                <div class="monitor-view__io-metric">
                  <dt class="monitor-view__io-label">队列深度</dt>
                  <dd class="monitor-view__io-value">{{ formatLoadValue(device.queue_depth) }}</dd>
                </div>
                <div class="monitor-view__io-metric">
                  <dt class="monitor-view__io-label">利用率</dt>
                  <dd class="monitor-view__io-value">{{ formatPercent(device.utilization) }}</dd>
                </div>
              </dl>
            </li>
          </ul>
          <div v-else class="ds-empty monitor-view__empty" data-test="monitor-disk-io-empty">
            <div class="ds-empty__title">暂无数据</div>
            <div class="ds-empty__desc">当前快照未提供磁盘 I/O 数据</div>
          </div>
          <!-- 语义提示（如 Windows 计数器口径说明）：非空时展示，参考系统负载 hint 的做法 -->
          <p v-if="diskIoHint" class="monitor-view__hint" data-test="monitor-disk-io-hint">语义提示：{{ diskIoHint }}</p>
        </template>
        <div v-else class="ds-empty monitor-view__empty" data-test="monitor-disk-io-unavailable">
          <div class="ds-empty__title">{{ UNAVAILABLE }}</div>
          <div class="ds-empty__desc">{{ diskIoUnavailableDesc }}</div>
        </div>
      </section>

      <!-- 网络（#77）：每网卡收发速率 / 累计字节与包 / 错误 / 丢弃。
           空数组 + 可用 = 发送方按 hide_virtual 滤掉全部虚拟网卡（契约 §2.5，不是不可用）；
           开关走 REST 下发，以 WS sysmon_config 回推为生效信号（REST 只表示已下发）。 -->
      <section class="ds-card monitor-view__card" data-test="monitor-network">
        <div class="monitor-view__card-head">
          <h2 class="ds-card__title monitor-view__card-title">
            <span class="monitor-view__card-icon"><Icon name="network" :size="14" /></span>网络
          </h2>
          <!-- admin 才可操作；非 admin 渲染为 disabled（可见但不可点，避免隐藏造成"功能不存在"误解） -->
          <div v-if="networkAvailable" class="monitor-view__net-toggle" data-test="monitor-network-show-all">
            <SyncSwitch
              :model-value="showAllNics"
              :pending="hideVirtualPending"
              :disabled="!canToggleNetwork"
              @change="onToggleShowAll"
            />
            <span class="monitor-view__net-toggle-label">显示全部网卡</span>
          </div>
        </div>

        <template v-if="networkAvailable">
          <ul v-if="network.length > 0" class="monitor-view__io-list">
            <li
              v-for="nic in network"
              :key="nic.interface"
              class="monitor-view__io-row"
              data-test="monitor-network-row"
              :data-interface="nic.interface"
            >
              <div class="monitor-view__io-device">
                {{ nic.interface }}
                <span v-if="nic.is_virtual" class="ds-tag" data-test="monitor-network-virtual">虚拟</span>
              </div>
              <dl class="monitor-view__io-metrics">
                <div class="monitor-view__io-metric">
                  <dt class="monitor-view__io-label">收速率</dt>
                  <dd class="monitor-view__io-value" data-test="monitor-network-rx-rate">{{ formatRate(nic.rx_bytes_per_sec) }}</dd>
                </div>
                <div class="monitor-view__io-metric">
                  <dt class="monitor-view__io-label">发速率</dt>
                  <dd class="monitor-view__io-value" data-test="monitor-network-tx-rate">{{ formatRate(nic.tx_bytes_per_sec) }}</dd>
                </div>
                <div class="monitor-view__io-metric">
                  <dt class="monitor-view__io-label">收字节</dt>
                  <dd class="monitor-view__io-value" data-test="monitor-network-rx-bytes">{{ formatBytes(nic.rx_bytes_total) }}</dd>
                </div>
                <div class="monitor-view__io-metric">
                  <dt class="monitor-view__io-label">发字节</dt>
                  <dd class="monitor-view__io-value" data-test="monitor-network-tx-bytes">{{ formatBytes(nic.tx_bytes_total) }}</dd>
                </div>
                <div class="monitor-view__io-metric">
                  <dt class="monitor-view__io-label">收包</dt>
                  <dd class="monitor-view__io-value" data-test="monitor-network-rx-packets">{{ formatTotalCount(nic.rx_packets_total) }}</dd>
                </div>
                <div class="monitor-view__io-metric">
                  <dt class="monitor-view__io-label">发包</dt>
                  <dd class="monitor-view__io-value" data-test="monitor-network-tx-packets">{{ formatTotalCount(nic.tx_packets_total) }}</dd>
                </div>
                <div class="monitor-view__io-metric">
                  <dt class="monitor-view__io-label">收错误</dt>
                  <dd class="monitor-view__io-value" data-test="monitor-network-rx-errors">{{ formatEventRateWithTotal(nic.rx_errors_per_sec, nic.rx_errors_total) }}</dd>
                </div>
                <div class="monitor-view__io-metric">
                  <dt class="monitor-view__io-label">发错误</dt>
                  <dd class="monitor-view__io-value" data-test="monitor-network-tx-errors">{{ formatEventRateWithTotal(nic.tx_errors_per_sec, nic.tx_errors_total) }}</dd>
                </div>
                <div class="monitor-view__io-metric">
                  <dt class="monitor-view__io-label">收丢弃</dt>
                  <dd class="monitor-view__io-value" data-test="monitor-network-rx-drops">{{ formatEventRateWithTotal(nic.rx_drops_per_sec, nic.rx_drops_total) }}</dd>
                </div>
                <div class="monitor-view__io-metric">
                  <dt class="monitor-view__io-label">发丢弃</dt>
                  <dd class="monitor-view__io-value" data-test="monitor-network-tx-drops">{{ formatEventRateWithTotal(nic.tx_drops_per_sec, nic.tx_drops_total) }}</dd>
                </div>
              </dl>
            </li>
          </ul>
          <!-- 可用但空数组：发送方按 hide_virtual 滤掉了全部虚拟网卡（契约 §2.5），
               与"采集失败/不可用"是不同事实，不得显示「不可用」 -->
          <div v-else-if="networkHiddenAll" class="ds-empty monitor-view__empty" data-test="monitor-network-hidden-all">
            <div class="ds-empty__title">已隐藏全部虚拟网卡</div>
            <div class="ds-empty__desc">本机只有虚拟/回环网卡；打开「显示全部网卡」查看</div>
          </div>
          <div v-else class="ds-empty monitor-view__empty" data-test="monitor-network-empty">
            <div class="ds-empty__title">暂无数据</div>
            <div class="ds-empty__desc">当前快照未提供网卡数据</div>
          </div>
        </template>
        <div v-else class="ds-empty monitor-view__empty" data-test="monitor-network-unavailable">
          <div class="ds-empty__title">{{ UNAVAILABLE }}</div>
          <div class="ds-empty__desc">{{ unavailableDesc('network') }}</div>
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

/* 实时 / 历史视图切换（#79）：分段按钮，与顶栏徽章同排 */
.monitor-view__view-switch {
  display: inline-flex;
  gap: var(--spacer-4);
  margin-left: auto;
}

.monitor-view__grid {
  display: grid;
  grid-template-columns: repeat(2, minmax(0, 1fr));
  gap: var(--spacer-16);
}

/* 阈值告警横幅（#80）：跨整行常驻提示；颜色沿用 ds-alert--danger，排版全部走设计 token */
.monitor-view__alerts {
  grid-column: 1 / -1;
}

.monitor-view__alerts-body {
  flex: 1;
  min-width: 0;
}

.monitor-view__alerts-list {
  list-style: none;
  margin: 0;
  padding: 0;
  display: flex;
  flex-direction: column;
  gap: var(--spacer-4);
}

.monitor-view__alert-item {
  display: flex;
  flex-wrap: wrap;
  align-items: baseline;
  gap: var(--spacer-8);
  color: var(--text-secondary);
  font-size: var(--body-md-font-size);
  line-height: var(--body-md-line-height);
  overflow-wrap: anywhere;
}

.monitor-view__alert-since {
  color: var(--text-tertiary);
  font-size: var(--body-xs-font-size);
  line-height: var(--body-xs-line-height);
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

/* 卡片头（标题 + 右侧操作，如网络区块的「显示全部网卡」开关） */
.monitor-view__card-head {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: var(--spacer-12);
  flex-wrap: wrap;
  margin-bottom: var(--spacer-8);
}

.monitor-view__card-head .ds-card__title {
  margin-bottom: 0;
}

.monitor-view__net-toggle {
  display: inline-flex;
  align-items: center;
  gap: var(--spacer-8);
}

.monitor-view__net-toggle-label {
  color: var(--text-secondary);
  font-size: var(--body-xs-font-size);
  line-height: var(--body-xs-line-height);
  white-space: nowrap;
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

/* 内存卡片内的交换子区块（#74）：与内存数值之间留白，复用面板标题 / 列表样式 */
.monitor-view__subsection {
  margin-top: var(--spacer-12);
}

/* 卷行（#75）：关注卷用左侧警示色描边 + 淡背景高亮（token 取状态色，不硬编码颜色） */
.monitor-view__disk-row {
  padding: var(--spacer-2) var(--spacer-6);
  border-left: 2px solid transparent;
  border-radius: var(--radius-4);
}

.monitor-view__disk-row--watched {
  border-left-color: var(--status-warning-default);
  background: var(--status-warning-surface-l1);
}

/* ---- 磁盘 I/O（#76）：每个物理盘一块，指标网格窄屏自动换行（auto-fit），全部走设计 token ---- */
.monitor-view__io-list {
  list-style: none;
  margin: 0;
  padding: 0;
  display: flex;
  flex-direction: column;
  gap: var(--spacer-8);
}

.monitor-view__io-row {
  display: grid;
  gap: var(--spacer-8);
  min-width: 0;
  padding: var(--spacer-8) var(--spacer-12);
  border: 1px solid var(--border-neutral-l1);
  border-radius: var(--radius-6);
}

.monitor-view__io-device {
  color: var(--text-default);
  font-size: var(--body-sm-font-size);
  line-height: var(--body-sm-line-height);
  font-weight: var(--body-sm-strong-font-weight);
  overflow-wrap: anywhere;
}

/* 网络行（#77）：虚拟网卡标记与网卡名同行，间距走 token（不硬编码） */
.monitor-view__io-device .ds-tag {
  margin-left: var(--spacer-6);
  vertical-align: middle;
}

.monitor-view__io-metrics {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(96px, 1fr));
  gap: var(--spacer-4) var(--spacer-16);
  margin: 0;
}

.monitor-view__io-metric {
  display: flex;
  flex-direction: column;
  gap: var(--spacer-2);
  min-width: 0;
}

.monitor-view__io-label {
  color: var(--text-tertiary);
  font-size: var(--body-xs-font-size);
  line-height: var(--body-xs-line-height);
}

.monitor-view__io-value {
  margin: 0;
  color: var(--text-default);
  font-family: var(--font-family-metric);
  font-size: var(--body-sm-font-size);
  line-height: var(--body-sm-line-height);
  font-variant-numeric: tabular-nums;
  overflow-wrap: anywhere;
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
