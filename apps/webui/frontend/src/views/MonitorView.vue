<script setup lang="ts">
import { computed, onMounted, ref } from 'vue'
import Icon from '@/components/shared/Icon.vue'
import SyncSwitch from '@/components/shared/SyncSwitch.vue'
import SysmonConfigTab from '@/components/sysmon/SysmonConfigTab.vue'
import SysmonHistoryTab from '@/components/sysmon/SysmonHistoryTab.vue'
import SysmonSparkline from '@/components/sysmon/SysmonSparkline.vue'
import { PENDING_TIMEOUT } from '@/composables/usePending'
import {
  UNAVAILABLE_TEXT,
  formatByteRate,
  formatBytes,
  formatCount,
  formatCountPerSec,
  formatDecimal,
  formatFrequency,
  formatIops,
  formatLatency,
  formatPerSec,
  formatPercent,
  formatTemperature,
} from '@/composables/format'
import {
  groupTemperatures,
  normalizeTemperatureReadings,
} from '@/composables/sysmonTemperature'
import { semanticHintText } from '@/composables/sysmonSemanticHint'
import { resourceLevel, type ResourceLevel } from '@/composables/useResourceLevel'
import { useAuthStore } from '@/stores/auth'
import { useSysmonHistoryStore } from '@/stores/sysmonHistory'
import { useSysmonStore } from '@/stores/sysmon'
import { trendMetricLabel, useSysmonTrendStore } from '@/stores/sysmonTrend'
import { useToastStore } from '@/stores/toast'
import type { SysmonVolumeInfo } from '@/types/generated'

// 资源监控页：系统信息与 CPU 区块（总量 / 每核 / 系统负载 / 上下文切换与中断 / 每核频率）、
// 温度、内存与交换、磁盘（卷容量 / 共享内存目录）接契约 sysmon 实时快照；
// 网络区块（#77）接每网卡收发速率与累计量，并提供「显示全部网卡」开关。
// 内存 / 交换（#74）按可用性映射渲染：不可用显示「不可用」+ 原因，可用但本帧无数据
// 显示「暂无数据」；交换在可用且总容量为 0 时显示「无交换分区」。
// 温度 / 每核频率（B 组）按可用性映射渲染：不可用时整块不渲染（不是显示 0 / 不可用占位）。
// 磁盘（#75）按可用性映射渲染：卷按使用率降序排列（#122）；正文只列容量可读且
// 总量 > 0 的卷，其余折叠进「无容量挂载（N）」，绝不显示 0；shm_dir 展示共享内存目录占用
//（页清理是否生效的可观测项）。
// 磁盘 I/O（#76）按可用性映射渲染：每物理盘的吞吐 / IOPS / 延迟 / 队列深度 / 利用率，
// 数值 null（首次采样/计数器重置）逐项显示「不可用」，能力不可用时整块显示原因。
// 网络（#77）：可用但数组为空 = 发送方按 hide_virtual 滤掉了全部虚拟网卡（契约 §2.5 明文，
// **不是**不可用），显示「已隐藏全部虚拟网卡」+ 切换提示；开关走 REST 下发、以 WS sysmon_config
// 回推为生效信号。
// 历史视图（#79）为独立视图：切换只影响渲染来源，不卸载/不重建实时 store，
// 因此 WS 连接与实时快照推送完全不受影响（契约 sysmon §8：历史视图不订阅 WS）。
// 配置视图（#81）：展示生效配置镜像（WS sysmon_config，首次挂载经 GET /api/sysmon/config 补拉）；
// admin 可编辑并只下发与原镜像的差异，非 admin 只读（编辑仅 admin，与系统设置页一致）。
// 内联趋势（#127）：标量指标（CPU 使用率 / 负载 / 内存 / 交换 / 共享内存目录）旁有最近 15 分钟
// sparkline（历史 REST 补齐 + WS 增量推进，末点即旁边的当前值）；历史不可用时静默不渲染。
// 时间窗按实际覆盖标注（#142）：只有覆盖满 15 分钟才写「最近 15 分钟」，历史范围内无桶 /
// 被 max_points 截断时标注真实覆盖时长，截断与历史视图同口径；补拉降级保留上一轮曲线并标未刷新。
// 点击 sparkline 或数值切到历史视图并预选该指标（契约 sysmon §6.1，不新增接口）。
// 卡片折叠（#127）：每核明细 / 每传感器明细 / 每盘七项指标 / 每网卡十项指标默认收起、展开显示全部；
// 折叠状态不持久化；可用性空态不进折叠。列数 <md 单列、md–xl 双列、≥xl 三列（沿用既有断点，
// 不使用容器查询），图表用容器自适应重排。
// 刷新基线由发送方 realtime.push_interval_sec（默认 5s）决定，本页不轮询、不改 WS 契约。
const sysmon = useSysmonStore()
/** 历史视图 store（#79）：与实时 store 完全独立，切换视图不影响 WS 与实时快照 */
const history = useSysmonHistoryStore()
/** 内联趋势 store（#127）：历史 REST 补齐 + WS 增量推进，历史不可用时静默不渲染；
 *  补拉降级保留上一轮序列并标记未刷新（#142） */
const trend = useSysmonTrendStore()

/** 视图模式：实时（默认） / 历史 / 配置（#81 展示生效配置 + admin 编辑） */
const viewMode = ref<'realtime' | 'history' | 'config'>('realtime')

onMounted(() => {
  void sysmon.init()
  // 内联趋势（#127）：进入页面时补拉最近 15 分钟历史；历史库不可用/被关闭时静默降级，
  // 已保留的上一轮曲线在补拉降级时继续显示并标记未刷新（#142）
  void trend.init()
})

/**
 * 点击 sparkline 或数值：切到历史视图并预选该指标（验收标准「跳转到历史视图并预选对应指标」）。
 * 只预选、不查询 —— 查询由历史视图挂载时统一发起，避免同一指标查两次。
 */
function openHistory(metricKey: string): void {
  history.preselect(metricKey)
  viewMode.value = 'history'
}

const snapshot = computed(() => sysmon.snapshot)
const system = computed(() => snapshot.value?.system ?? null)
const platformDescription = computed(() => snapshot.value?.availability.platform_description ?? '')
const updatedAt = computed(() => {
  const timestamp = snapshot.value?.timestamp
  return typeof timestamp === 'number' ? formatDateTime(timestamp) : ''
})

// ---- 格式化（#126：唯一实现收敛到 @/composables/format，本文件不得再写 toFixed）----
// 「不可用」占位直接用共享模块导出的 UNAVAILABLE_TEXT（不在本文件另起别名，
// 否则同一事实出现两个名字——Topbar 等其它文件都直接用导出名）。
/** 配置关闭的统一文案（卷 / 指标可用性映射共用，避免两处字面量漂移） */
const DISABLED_BY_CONFIG_TEXT = '已在配置中关闭'

/** 文件年龄（秒 → 可读时长）：页文件年龄是页清理是否生效的直接信号（契约 sysmon §2.4） */
function formatAge(seconds: number): string {
  if (!Number.isFinite(seconds) || seconds < 0) return UNAVAILABLE_TEXT
  const total = Math.floor(seconds)
  if (total < 60) return `${total} 秒`
  if (total < 3_600) return `${Math.floor(total / 60)} 分钟`
  if (total < 86_400) return `${Math.floor(total / 3_600)} 小时`
  return `${Math.floor(total / 86_400)} 天`
}

/** 卷的不可用原因（契约 sysmon §2.4 建议取值）：文案化，裸 reason 兜底 */
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
  if (Number.isNaN(date.getTime())) return UNAVAILABLE_TEXT
  const pad = (n: number): string => String(n).padStart(2, '0')
  return `${date.getFullYear()}-${pad(date.getMonth() + 1)}-${pad(date.getDate())} `
    + `${pad(date.getHours())}:${pad(date.getMinutes())}:${pad(date.getSeconds())}`
}

function formatUptime(seconds: number): string {
  if (!Number.isFinite(seconds) || seconds < 0) return UNAVAILABLE_TEXT
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
  // 进度条宽度是 CSS 值，仍需钳制到 [0,1]（异常比率不得把条画出容器）；
  // 输出形态与文字百分比同源（formatPercent），避免条宽与旁边数字精度不一致。
  return formatPercent(Math.min(1, Math.max(0, ratio)))
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
const loadHintRaw = computed(() => sysmon.semanticHint('cpu.load_average'))
// 语义提示按稳定 key 映射中文（#207 B2）；发送方原文保留在 tooltip 里可对照
const loadHint = computed(() => {
  const raw = loadHintRaw.value
  return raw === '' ? null : { text: semanticHintText('cpu.load_average', raw), raw }
})
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
// 温度按芯片分组展示（#126）：分组/友好名/排序全部在共享模块内（纯函数，可单测）；
// 本视图只负责渲染。**不过滤任何传感器**——ACPI 类只是排在组列表末尾。
const temperatureGroups = computed(() =>
  groupTemperatures(normalizeTemperatureReadings(sysmon.temperatures)),
)
const frequencyAvailable = computed(() => isAvailable('cpu.frequency'))
const coreFrequencies = computed(() => sysmon.coreFrequencies)

// ---- 内存 / 交换（#74）/ 磁盘（#75：卷容量 / 共享内存目录）/ 网络 ----
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
// 卷列表分两池（#122）：正文只留容量可读且总量 > 0 的卷，无容量挂载折叠成组。
// 折叠状态不持久化（刷新回到默认收起），与既有手风琴一致。
const readableVolumes = computed(() => sysmon.readableVolumes)
const zeroCapacityVolumes = computed(() => sysmon.zeroCapacityVolumes)
const zeroVolumesOpen = ref(false)

/**
 * 使用率色标等级（卷 / 内存 / 交换；#120 收口）：与顶栏徽章共用 `resourceLevel` 的同一组
 * 常量（≥70% 黄、≥90% 红）。`null`（不可用 / 本帧无值）不套任何状态色——不可用不是 0%，
 * 不得用 normal 冒充「一切正常」。
 */
function usageLevel(ratio: number | null): ResourceLevel | null {
  return ratio === null ? null : resourceLevel(ratio)
}

/**
 * 卷行的使用率展示等级（与顶栏卷徽章共用 `usageLevel` / `resourceLevel`）。
 * 正文卷列表由 store 的 `readableVolumes` 口径保证 `used_ratio` 非 null；这里仍显式处理 null
 * （不伪造 0 再套「正常」色）—— 真出现时不套任何状态色，该行本不该出现在正文。
 */
function volumeLevel(volume: SysmonVolumeInfo): ResourceLevel | null {
  return usageLevel(volume.used_ratio)
}

const shmAvailable = computed(() => isAvailable('shm_dir'))
const shmDir = computed(() => sysmon.shmDirInfo)
// ---- 磁盘 I/O（#76）：每物理盘吞吐 / IOPS / 延迟 / 队列深度 / 利用率，按可用性映射渲染 ----
// 只在 availability.metrics 上分支，不按 platform.family 分支；数值 null（首次采样/计数器重置）
// 逐项显示「不可用」，绝不显示 0。
const diskIoAvailable = computed(() => isAvailable('disk_io'))
const diskIo = computed(() => sysmon.diskIo)
const diskIoHintRaw = computed(() => sysmon.semanticHint('disk_io'))
// 语义提示按稳定 key 映射中文（#207 B2）；发送方原文保留在 tooltip 里可对照
const diskIoHint = computed(() => {
  const raw = diskIoHintRaw.value
  return raw === '' ? null : { text: semanticHintText('disk_io', raw), raw }
})
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

// 开关语义是「显示全部网卡」(= hide_virtual 取反)，直接下发取反后的 hide_virtual 值。
// 结果契约（对齐 settings/mdConfig 的配置类操作）：true = 已下发（等 WS sysmon_config 生效）、
// false = 防重入或 HTTP 失败、PENDING_TIMEOUT = 超时（usePending 已 toast，此处不重复弹）。
// 失败必须有可见反馈：否则用户点了开关、界面无变化也无提示（静默失败）。
async function onToggleShowAll(next: boolean): Promise<void> {
  const result = await sysmon.setHideVirtual(!next)
  if (result === PENDING_TIMEOUT) return
  if (result === false) toast.error('显示全部网卡切换下发失败')
}

// ---- 卡片折叠（#127）：维度明细默认收起，展开后显示全部 ----
// 折叠状态只存组件本地 ref：**不持久化**，刷新（组件重新挂载）回到产品认为重要的默认视图
//（验收标准）。不可用 / 空数据的空态不进折叠——可用性事实不藏在展开按钮后面。
const cpuDetailOpen = ref(false)
const temperatureDetailOpen = ref(false)
const diskIoDetailOpen = ref(false)
const networkDetailOpen = ref(false)

/** 一组可能缺失的数值中的最大值；全缺失时 null（缺失不是 0，不参与比较） */
function peakOf(values: (number | null)[]): number | null {
  let peak: number | null = null
  for (const value of values) {
    if (value === null) continue
    if (peak === null || value > peak) peak = value
  }
  return peak
}

/** 每核明细是否有内容可展开（使用率或频率任一有数据；频率可用但使用率读不到时仍能查看频率） */
const cpuHasDetail = computed(() => cpuCores.value.length > 0
  || (frequencyAvailable.value && coreFrequencies.value.length > 0))

/** 每核明细摘要（收起时显示）：核数 + 最高使用率；没有数据就是没有，不伪造 0 */
const cpuDetailSummary = computed(() => {
  const parts: string[] = []
  if (cpuCores.value.length > 0) parts.push(`${cpuCores.value.length} 核`)
  const peak = peakOf(cpuCores.value.map(core => core.usage_ratio))
  if (peak !== null) parts.push(`最高 ${formatPercent(peak)}`)
  else if (coreFrequencies.value.length > 0) parts.push(`${coreFrequencies.value.length} 核频率`)
  return parts.join(' · ')
})

/** 温度读数摊平：分组只用于展开后的展示，摘要取全局最高温与数量 */
const temperatureReadings = computed(() => temperatureGroups.value.flatMap(group => group.readings))

/** 最高温度的读数与其归属（芯片分组名 + 通道标签，不丢可读命名口径，见 #120 故事 37/38） */
interface TemperaturePeak {
  celsius: number
  groupName: string
  label: string
}

/** 最高温度（主数值）及其归属；无读数时为 null（不伪造 0 °C） */
const temperaturePeak = computed<TemperaturePeak | null>(() => {
  let peak: TemperaturePeak | null = null
  for (const group of temperatureGroups.value) {
    for (const reading of group.readings) {
      if (peak === null || reading.celsius > peak.celsius) {
        peak = { celsius: reading.celsius, groupName: group.displayName, label: reading.label }
      }
    }
  }
  return peak
})
const temperatureDetailSummary = computed(
  () => `${temperatureReadings.value.length} 个传感器 · ${temperatureGroups.value.length} 个芯片`,
)

/** 磁盘 I/O 摘要（收起时显示）：盘数 + 最高利用率；全为 null 时只说盘数，不伪造 0 */
const diskIoDetailSummary = computed(() => {
  const parts = [`${diskIo.value.length} 块盘`]
  const peak = peakOf(diskIo.value.map(device => device.utilization))
  if (peak !== null) parts.push(`最高利用率 ${formatPercent(peak)}`)
  return parts.join(' · ')
})

/** 网络摘要（收起时显示）：网卡数（虚拟网卡是否隐藏由发送方决定，这里只统计收到的条目） */
const networkDetailSummary = computed(() => `${network.value.length} 块网卡`)

// ---- 网卡累计量 / 速率格式化（#126：单位档与精度全部走共享模块）----
/**
 * 错误 / 丢弃单元格：速率 + 累计量并列（速率反映当前窗口，累计量反映是否曾经发生过）。
 * 速率 null（首次采样/计数器重置）显示「不可用」而不是 0；累计量始终有效（累计计数器没有
 * 不可用语义），因此即便速率不可用仍能看到历史累计。
 * 错误/丢弃速率保留 1 位小数（可为 0 = 本窗口确实没有增量）。
 */
function formatEventRateWithTotal(rate: number | null | undefined, total: number): string {
  return `${formatPerSec(rate, 1)}（累计 ${formatCount(total)}）`
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
        <button
          class="ds-btn ds-btn--sm"
          :class="viewMode === 'config' ? 'ds-btn--primary' : 'ds-btn--secondary'"
          type="button"
          role="tab"
          :aria-selected="viewMode === 'config'"
          data-test="monitor-tab-config"
          @click="viewMode = 'config'"
        >配置</button>
      </div>
      <span v-if="viewMode === 'realtime' && snapshot" class="ds-tag">实时快照</span>
      <span v-else-if="viewMode === 'realtime'" class="ds-tag ds-tag--warning">等待快照</span>
    </header>

    <!-- 历史视图（#79）：独立渲染，不卸载实时 store（WS 连接与实时数据不受影响） -->
    <SysmonHistoryTab v-if="viewMode === 'history'" />

    <!-- 配置视图（#81）：展示生效配置（所有认证用户）+ 编辑入口（仅 admin） -->
    <SysmonConfigTab v-if="viewMode === 'config'" />

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
              <!-- 数值与 sparkline 都可点：跳转历史视图并预选该指标（#127）；sparkline 末点即本数值 -->
              <div class="monitor-view__metric-row">
                <button
                  type="button"
                  class="monitor-view__metric monitor-view__value-btn"
                  data-test="monitor-cpu-value"
                  @click="openHistory('cpu.usage_ratio')"
                >{{ cpuPercent }}</button>
                <SysmonSparkline
                  :points="trend.seriesOf('cpu.usage_ratio')"
                  :label="trendMetricLabel('cpu.usage_ratio')"
                  :window="trend.windowOf('cpu.usage_ratio')"
                  class="monitor-view__sparkline"
                  data-test="monitor-sparkline-cpu"
                  @open="openHistory('cpu.usage_ratio')"
                />
              </div>
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
              <div class="ds-empty__title">{{ UNAVAILABLE_TEXT }}</div>
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
                  <dd class="monitor-view__load-value">
                    <button
                      type="button"
                      class="monitor-view__value-btn"
                      data-test="monitor-load-queue"
                      @click="openHistory('cpu.load_average.one')"
                    >{{ formatDecimal(loadAverage.one) }}</button>
                  </dd>
                </div>
                <template v-else>
                  <div class="monitor-view__load-item">
                    <dt class="monitor-view__load-label">1 分钟</dt>
                    <dd class="monitor-view__load-value">
                      <button
                        type="button"
                        class="monitor-view__value-btn"
                        data-test="monitor-load-one"
                        @click="openHistory('cpu.load_average.one')"
                      >{{ formatDecimal(loadAverage.one) }}</button>
                    </dd>
                  </div>
                  <div class="monitor-view__load-item">
                    <dt class="monitor-view__load-label">5 分钟</dt>
                    <dd class="monitor-view__load-value" data-test="monitor-load-five">{{ formatDecimal(loadAverage.five) }}</dd>
                  </div>
                  <div class="monitor-view__load-item">
                    <dt class="monitor-view__load-label">15 分钟</dt>
                    <dd class="monitor-view__load-value" data-test="monitor-load-fifteen">{{ formatDecimal(loadAverage.fifteen) }}</dd>
                  </div>
                </template>
              </dl>
              <!-- 系统负载趋势（#127）：取历史库的 1 分钟列（Windows 上同一列是处理器队列代理量，
                   语义提示仍由 available.semantic_hints 承载，sparkline 不另做口径判断） -->
              <SysmonSparkline
                v-if="loadAverage.one !== null"
                :points="trend.seriesOf('cpu.load_average.one')"
                :label="trendMetricLabel('cpu.load_average.one')"
                :window="trend.windowOf('cpu.load_average.one')"
                class="monitor-view__sparkline monitor-view__sparkline--block"
                data-test="monitor-sparkline-load"
                @open="openHistory('cpu.load_average.one')"
              />
              <p v-if="loadHint" class="monitor-view__hint" data-test="monitor-load-hint" :title="loadHint.raw">语义提示：{{ loadHint.text }}</p>
            </template>
            <div v-else class="ds-empty monitor-view__empty" data-test="monitor-load-unavailable">
              <div class="ds-empty__title">{{ UNAVAILABLE_TEXT }}</div>
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

          <!-- 每核明细（#127）：每核使用率 + 每核频率同属维度明细，默认收起、展开后显示全部；
               不可用 / 空数据的空态不折叠（可用性事实不藏起来）。核数随快照变化。 -->
          <div class="monitor-view__cpu-panel monitor-view__cpu-panel--wide" data-test="monitor-cpu-cores">
            <div class="monitor-view__panel-head">
              <h3 class="monitor-view__panel-title">每核使用率</h3>
              <button
                v-if="cpuHasDetail"
                class="ds-btn ds-btn--tertiary ds-btn--sm"
                type="button"
                :aria-expanded="cpuDetailOpen"
                data-test="monitor-cpu-detail-toggle"
                @click="cpuDetailOpen = !cpuDetailOpen"
              >{{ cpuDetailOpen ? '收起' : '展开' }}</button>
            </div>
            <p
              v-if="cpuHasDetail && !cpuDetailOpen"
              class="monitor-view__meta"
              data-test="monitor-cpu-detail-summary"
            >{{ cpuDetailSummary }}</p>
            <ul v-if="cpuCores.length > 0 && cpuDetailOpen" class="monitor-view__cores">
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
            <div v-else-if="cpuCores.length === 0" class="ds-empty monitor-view__empty" data-test="monitor-cores-empty">
              <div class="ds-empty__title">{{ cpuCoresAvailable ? '暂无每核数据' : UNAVAILABLE_TEXT }}</div>
              <div class="ds-empty__desc">{{ coresEmptyDesc }}</div>
            </div>

            <!-- 每核频率（#73）：仅在 cpu.frequency 可用时渲染；与每核使用率同属每核明细，一起收起 -->
            <div v-if="frequencyAvailable" class="monitor-view__subsection" data-test="monitor-frequency">
              <h3 class="monitor-view__panel-title">每核频率</h3>
              <ul v-if="coreFrequencies.length > 0 && cpuDetailOpen" class="monitor-view__cores">
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
              <p
                v-else-if="coreFrequencies.length > 0"
                class="monitor-view__meta"
                data-test="monitor-frequency-collapsed"
              >{{ coreFrequencies.length }} 核频率 · 展开每核明细查看</p>
              <div v-else class="ds-empty monitor-view__empty" data-test="monitor-frequency-empty">
                <div class="ds-empty__title">暂无数据</div>
                <div class="ds-empty__desc">当前快照未提供每核频率</div>
              </div>
            </div>
          </div>
        </div>
      </section>

      <!-- 温度（#73）：仅在 temperature 可用时渲染整块卡片；不可用整块不渲染（不是显示 0）。
           #126：按芯片分组（CPU / GPU / 无线网卡 / 主板 ACPI 等友好名），组内逐通道显示；
           原始 sensor 标识降级到 title（tooltip）——需要与系统工具输出对照时仍可查到。
           不过滤任何传感器：主板 ACPI 类只是排在组列表末尾。 -->
      <section v-if="temperatureAvailable" class="ds-card monitor-view__card" data-test="monitor-temperature">
        <h2 class="ds-card__title monitor-view__card-title">
          <span class="monitor-view__card-icon"><Icon name="system" :size="14" /></span>温度
        </h2>
        <template v-if="temperatureGroups.length > 0">
          <!-- #127：默认只显示主数值（最高温）+ 关键数字（传感器 / 芯片数），逐传感器明细收起 -->
          <div class="monitor-view__metric-row">
            <div class="monitor-view__metric" data-test="monitor-temperature-max">
              {{ formatTemperature(temperaturePeak?.celsius ?? null) }}
            </div>
            <button
              class="ds-btn ds-btn--tertiary ds-btn--sm"
              type="button"
              :aria-expanded="temperatureDetailOpen"
              data-test="monitor-temperature-detail-toggle"
              @click="temperatureDetailOpen = !temperatureDetailOpen"
            >{{ temperatureDetailOpen ? '收起' : '展开明细' }}</button>
          </div>
          <!-- 归属（芯片友好名 + 通道标签）跟着主数值走：只说「最高 45.5 °C」不知道是哪颗芯片 -->
          <p class="monitor-view__meta" data-test="monitor-temperature-summary">
            最高温度 · {{ temperaturePeak?.groupName }} · {{ temperaturePeak?.label }} · {{ temperatureDetailSummary }}
          </p>
          <div v-if="temperatureDetailOpen" class="monitor-view__temp-groups">
            <div
              v-for="group in temperatureGroups"
              :key="group.chip"
              class="monitor-view__temp-group"
              data-test="monitor-temperature-group"
              :data-chip="group.chip"
            >
              <h3 class="monitor-view__panel-title" data-test="monitor-temperature-group-name">
                {{ group.displayName }}
              </h3>
              <ul class="monitor-view__list">
                <li
                  v-for="reading in group.readings"
                  :key="reading.sensor"
                  class="monitor-view__list-row"
                  data-test="monitor-temperature-sensor"
                  :data-sensor="reading.sensor"
                  :title="reading.sensor"
                >
                  <span class="monitor-view__list-label">{{ reading.label }}</span>
                  <span class="monitor-view__list-value">{{ formatTemperature(reading.celsius) }}</span>
                </li>
              </ul>
            </div>
          </div>
        </template>
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
          <div class="monitor-view__metric-row" :data-level="usageLevel(memory.used_ratio)">
            <button
              type="button"
              class="monitor-view__metric monitor-view__value-btn"
              data-test="monitor-memory-value"
              @click="openHistory('memory.used_ratio')"
            >{{ formatPercent(memory.used_ratio) }}</button>
            <SysmonSparkline
              :points="trend.seriesOf('memory.used_ratio')"
              :label="trendMetricLabel('memory.used_ratio')"
              :window="trend.windowOf('memory.used_ratio')"
              class="monitor-view__sparkline"
              data-test="monitor-sparkline-memory"
              @open="openHistory('memory.used_ratio')"
            />
          </div>
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
          <div class="ds-empty__title">{{ UNAVAILABLE_TEXT }}</div>
          <div class="ds-empty__desc">{{ unavailableDesc('memory') }}</div>
        </div>

        <!-- 交换子区块：可用且总量 > 0 → 使用率 + 已用/总量；可用但总量 0 → 无交换分区；
             可用但本帧无对象 → 暂无数据；不可用 → 不可用 + 原因（不得显示 0） -->
        <div class="monitor-view__subsection" data-test="monitor-swap">
          <h3 class="monitor-view__panel-title">交换</h3>
          <ul v-if="swapInfo && swapInfo.total_bytes > 0" class="monitor-view__list" data-test="monitor-memory-swap">
            <li class="monitor-view__list-row" :data-level="usageLevel(swapInfo.used_ratio)">
              <span class="monitor-view__list-label">使用率</span>
              <span class="monitor-view__list-value">
                <button
                  type="button"
                  class="monitor-view__value-btn"
                  data-test="monitor-memory-swap-usage"
                  @click="openHistory('swap.used_ratio')"
                >{{ formatPercent(swapInfo.used_ratio) }}</button>
              </span>
              <SysmonSparkline
                :points="trend.seriesOf('swap.used_ratio')"
                :label="trendMetricLabel('swap.used_ratio')"
                :window="trend.windowOf('swap.used_ratio')"
                class="monitor-view__sparkline monitor-view__sparkline--inline"
                data-test="monitor-sparkline-swap"
                @open="openHistory('swap.used_ratio')"
              />
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
            <div class="ds-empty__title">{{ UNAVAILABLE_TEXT }}</div>
            <div class="ds-empty__desc">{{ swapUnavailableDesc }}</div>
          </div>
        </div>
      </section>

      <!-- 磁盘（#75：卷容量 / 共享内存目录），全部按 availability.metrics 渲染 -->
      <section class="ds-card monitor-view__card" data-test="monitor-disk">
        <h2 class="ds-card__title monitor-view__card-title">
          <span class="monitor-view__card-icon"><Icon name="Disk" :size="14" /></span>磁盘
        </h2>

        <!-- 卷：正文只留容量可读且总量 > 0 的卷（#122），按使用率降序（store 已排好）；
             available=false 与 total_bytes=0 的挂载折叠进「无容量挂载（N）」，绝不显示 0。
             data-level 为使用率色标（与顶栏卷徽章共用 resourceLevel；色标是展示规则，不是越限判定） -->
        <div class="monitor-view__subsection" data-test="monitor-disk-volumes">
          <h3 class="monitor-view__panel-title">卷容量</h3>
          <ul v-if="readableVolumes.length > 0" class="monitor-view__list">
            <li
              v-for="volume in readableVolumes"
              :key="volume.mount"
              class="monitor-view__list-row monitor-view__disk-row"
              data-test="monitor-disk-volume"
              :data-mount="volume.mount"
              :data-level="volumeLevel(volume)"
            >
              <span class="monitor-view__list-label">{{ volume.mount }}</span>
              <span class="monitor-view__list-value">{{ formatPercent(volume.used_ratio) }}</span>
              <span class="monitor-view__list-meta">{{ formatBytes(volume.used_bytes) }} / {{ formatBytes(volume.total_bytes) }}</span>
            </li>
          </ul>
          <div v-else-if="volumes.length === 0" class="ds-empty monitor-view__empty" data-test="monitor-disk-unavailable">
            <div class="ds-empty__title">{{ UNAVAILABLE_TEXT }}</div>
            <div class="ds-empty__desc">{{ volumesAvailable ? '当前快照未提供卷数据' : unavailableDesc('volumes') }}</div>
          </div>

          <!-- 无容量挂载（#122）：容量读不到 / 总量为 0 的挂载折叠在此，默认收起。
               折叠是纯显示行为，不改变采集语义；展开后逐条给出挂载点与原因。 -->
          <div v-if="zeroCapacityVolumes.length > 0" class="monitor-view__zero-volumes" data-test="monitor-disk-zero-volumes">
            <button
              class="ds-btn ds-btn--tertiary ds-btn--sm"
              type="button"
              :aria-expanded="zeroVolumesOpen"
              data-test="monitor-disk-zero-toggle"
              @click="zeroVolumesOpen = !zeroVolumesOpen"
            >无容量挂载（{{ zeroCapacityVolumes.length }}）· {{ zeroVolumesOpen ? '收起' : '展开' }}</button>
            <ul v-if="zeroVolumesOpen" class="monitor-view__list">
              <li
                v-for="volume in zeroCapacityVolumes"
                :key="volume.mount"
                class="monitor-view__list-row"
                data-test="monitor-disk-zero-volume"
                :data-mount="volume.mount"
              >
                <span class="monitor-view__list-label">{{ volume.mount }}</span>
                <!-- 读不到容量与「总量确实为 0」是两件事，原因列分别说明，都不用 0 冒充 -->
                <span class="monitor-view__list-value">{{ volume.available ? '—' : UNAVAILABLE_TEXT }}</span>
                <span class="monitor-view__list-meta" data-test="monitor-disk-zero-reason">
                  {{ volume.available ? '总容量为 0' : volumeReasonDesc(volume.reason) }}
                </span>
              </li>
            </ul>
          </div>
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
                <dd class="monitor-view__list-value" data-test="monitor-shm-total">
                  <button
                    type="button"
                    class="monitor-view__value-btn"
                    @click="openHistory('shm_dir.total_bytes')"
                  >{{ formatBytes(shmDir.total_bytes) }}</button>
                </dd>
                <SysmonSparkline
                  :points="trend.seriesOf('shm_dir.total_bytes')"
                  :label="trendMetricLabel('shm_dir.total_bytes')"
                  :window="trend.windowOf('shm_dir.total_bytes')"
                  class="monitor-view__sparkline monitor-view__sparkline--inline"
                  data-test="monitor-sparkline-shm"
                  @open="openHistory('shm_dir.total_bytes')"
                />
              </div>
              <div class="monitor-view__list-row">
                <dt class="monitor-view__list-label">页文件</dt>
                <dd class="monitor-view__list-value" data-test="monitor-shm-page-files">{{ formatCount(shmDir.page_files) }}</dd>
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
            <div class="ds-empty__title">{{ UNAVAILABLE_TEXT }}</div>
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
          <!-- #127：每盘七项指标属维度明细，默认收起；空数据 / 不可用空态不折叠 -->
          <div v-if="diskIo.length > 0" class="monitor-view__detail-bar">
            <span class="monitor-view__meta" data-test="monitor-disk-io-summary">{{ diskIoDetailSummary }}</span>
            <button
              class="ds-btn ds-btn--tertiary ds-btn--sm"
              type="button"
              :aria-expanded="diskIoDetailOpen"
              data-test="monitor-disk-io-detail-toggle"
              @click="diskIoDetailOpen = !diskIoDetailOpen"
            >{{ diskIoDetailOpen ? '收起' : '展开明细' }}</button>
          </div>
          <ul v-if="diskIo.length > 0 && diskIoDetailOpen" class="monitor-view__io-list">
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
                  <dd class="monitor-view__io-value">{{ formatByteRate(device.read_bytes_per_sec) }}</dd>
                </div>
                <div class="monitor-view__io-metric">
                  <dt class="monitor-view__io-label">写吞吐</dt>
                  <dd class="monitor-view__io-value">{{ formatByteRate(device.write_bytes_per_sec) }}</dd>
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
                  <dd class="monitor-view__io-value">{{ formatDecimal(device.queue_depth) }}</dd>
                </div>
                <div class="monitor-view__io-metric">
                  <dt class="monitor-view__io-label">利用率</dt>
                  <dd class="monitor-view__io-value">{{ formatPercent(device.utilization) }}</dd>
                </div>
              </dl>
            </li>
          </ul>
          <div v-else-if="diskIo.length === 0" class="ds-empty monitor-view__empty" data-test="monitor-disk-io-empty">
            <div class="ds-empty__title">暂无数据</div>
            <div class="ds-empty__desc">当前快照未提供磁盘 I/O 数据</div>
          </div>
          <!-- 语义提示（如 Windows 计数器口径说明）：非空时展示，参考系统负载 hint 的做法 -->
          <p v-if="diskIoHint" class="monitor-view__hint" data-test="monitor-disk-io-hint" :title="diskIoHint.raw">语义提示：{{ diskIoHint.text }}</p>
        </template>
        <div v-else class="ds-empty monitor-view__empty" data-test="monitor-disk-io-unavailable">
          <div class="ds-empty__title">{{ UNAVAILABLE_TEXT }}</div>
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
              label="显示全部网卡"
              @change="onToggleShowAll"
            />
            <span class="monitor-view__net-toggle-label">显示全部网卡</span>
          </div>
        </div>

        <template v-if="networkAvailable">
          <!-- #127：每网卡十项指标属维度明细，默认收起；隐藏全部 / 空数据空态不折叠 -->
          <div v-if="network.length > 0" class="monitor-view__detail-bar">
            <span class="monitor-view__meta" data-test="monitor-network-summary">{{ networkDetailSummary }}</span>
            <button
              class="ds-btn ds-btn--tertiary ds-btn--sm"
              type="button"
              :aria-expanded="networkDetailOpen"
              data-test="monitor-network-detail-toggle"
              @click="networkDetailOpen = !networkDetailOpen"
            >{{ networkDetailOpen ? '收起' : '展开明细' }}</button>
          </div>
          <ul v-if="network.length > 0 && networkDetailOpen" class="monitor-view__io-list">
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
                  <dd class="monitor-view__io-value" data-test="monitor-network-rx-rate">{{ formatByteRate(nic.rx_bytes_per_sec) }}</dd>
                </div>
                <div class="monitor-view__io-metric">
                  <dt class="monitor-view__io-label">发速率</dt>
                  <dd class="monitor-view__io-value" data-test="monitor-network-tx-rate">{{ formatByteRate(nic.tx_bytes_per_sec) }}</dd>
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
                  <dd class="monitor-view__io-value" data-test="monitor-network-rx-packets">{{ formatCount(nic.rx_packets_total) }}</dd>
                </div>
                <div class="monitor-view__io-metric">
                  <dt class="monitor-view__io-label">发包</dt>
                  <dd class="monitor-view__io-value" data-test="monitor-network-tx-packets">{{ formatCount(nic.tx_packets_total) }}</dd>
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
          <div
            v-else-if="network.length === 0 && networkHiddenAll"
            class="ds-empty monitor-view__empty"
            data-test="monitor-network-hidden-all"
          >
            <div class="ds-empty__title">已隐藏全部虚拟网卡</div>
            <div class="ds-empty__desc">本机只有虚拟/回环网卡；打开「显示全部网卡」查看</div>
          </div>
          <div
            v-else-if="network.length === 0"
            class="ds-empty monitor-view__empty"
            data-test="monitor-network-empty"
          >
            <div class="ds-empty__title">暂无数据</div>
            <div class="ds-empty__desc">当前快照未提供网卡数据</div>
          </div>
        </template>
        <div v-else class="ds-empty monitor-view__empty" data-test="monitor-network-unavailable">
          <div class="ds-empty__title">{{ UNAVAILABLE_TEXT }}</div>
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

/* #127 列数断点：<md 单列（下方 max-width 块）、md–xl 双列（上方基础规则）、≥xl 三列。
   沿用既有断点体系（xl=1280，#127 明确不使用容器查询），图表用容器自适应重排。 */
@media (min-width: 1280px) {
  .monitor-view__grid {
    grid-template-columns: repeat(3, minmax(0, 1fr));
  }
}

/* 资源使用率色标（卷 / 内存 / 交换；#120 收口）：与顶栏徽章共用 useResourceLevel 的同一组
   常量与设计 token（≥70% 黄、≥90% 红）；色标是独立展示规则，不是越限判定。 */
.monitor-view__disk-row[data-level='normal'] .monitor-view__list-value,
.monitor-view__metric-row[data-level='normal'] .monitor-view__metric,
.monitor-view__list-row[data-level='normal'] .monitor-view__value-btn {
  color: var(--status-success-default);
}

.monitor-view__disk-row[data-level='warn'] .monitor-view__list-value,
.monitor-view__metric-row[data-level='warn'] .monitor-view__metric,
.monitor-view__list-row[data-level='warn'] .monitor-view__value-btn {
  color: var(--status-alert-default);
}

.monitor-view__disk-row[data-level='danger'] .monitor-view__list-value,
.monitor-view__metric-row[data-level='danger'] .monitor-view__metric,
.monitor-view__list-row[data-level='danger'] .monitor-view__value-btn {
  color: var(--status-error-default);
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

/* 可点击数值（#127：点击跳转历史视图并预选指标）：只重置按钮外观，字号/颜色沿用所在行的样式，
   因此必须定义在 .monitor-view__metric 之前（后者的字号规则要能覆盖 font: inherit） */
.monitor-view__value-btn {
  margin: 0;
  padding: 0;
  border: none;
  background: transparent;
  color: inherit;
  font: inherit;
  text-align: inherit;
  cursor: pointer;
}

.monitor-view__value-btn:hover {
  text-decoration: underline;
  text-underline-offset: 2px;
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

/* 数值 + sparkline 同排（#127）：sparkline 是辅助信息，固定高度、宽度跟随卡片列宽（容器自适应，
   不靠 JS 计算断点），不抢数字的主体地位 */
.monitor-view__metric-row {
  display: flex;
  align-items: center;
  gap: var(--spacer-12);
  min-width: 0;
}

.monitor-view__sparkline {
  flex: 1;
  min-width: 48px;
  max-width: 160px;
}

/* 负载 / 交换 / 共享内存目录的 sparkline 跟在数值块之后，独占一行时也不撑满整卡 */
.monitor-view__sparkline--block {
  margin-top: var(--spacer-4);
}

.monitor-view__sparkline--inline {
  flex: 0 1 96px;
  align-self: center;
  margin-left: auto;
}

/* 内存卡片内的交换子区块（#74）：与内存数值之间留白，复用面板标题 / 列表样式 */
.monitor-view__subsection {
  margin-top: var(--spacer-12);
}

/* 卷行（#75）：行内边距与圆角 */
.monitor-view__disk-row {
  padding: var(--spacer-2) var(--spacer-6);
  border-radius: var(--radius-4);
}

/* 无容量挂载折叠组（#122）：正文之后、与列表之间留白（折叠按钮自带尺寸，不另设边框） */
.monitor-view__zero-volumes {
  margin-top: var(--spacer-8);
}

/* 温度分组（#126）：每个芯片一组，组间留白；复用面板标题与列表样式，不新增视觉范式 */
.monitor-view__temp-groups {
  display: flex;
  flex-direction: column;
  gap: var(--spacer-12);
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

/* 面板头（#127）：标题与折叠按钮同排；标题不再需要下边距 */
.monitor-view__panel-head {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: var(--spacer-8);
}

.monitor-view__panel-head .monitor-view__panel-title {
  margin-bottom: 0;
}

/* 维度明细的折叠条（#127）：左侧摘要、右侧展开/收起按钮，复用既有 tertiary 按钮，不新增折叠组件 */
.monitor-view__detail-bar {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: var(--spacer-8);
  margin-bottom: var(--spacer-8);
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
