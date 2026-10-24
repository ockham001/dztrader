<script setup lang="ts">
import { computed, reactive, watch } from 'vue'
import Icon from '@/components/shared/Icon.vue'
import type { SysmonConfigPatch } from '@/api/sysmon'
import { PENDING_TIMEOUT } from '@/composables/usePending'
import { useAuthStore } from '@/stores/auth'
import { useSysmonStore } from '@/stores/sysmon'
import { useToastStore } from '@/stores/toast'
import type {
  SysmonAlertsConfigPayload,
  SysmonConfigPayload,
  SysmonDiskConfigPayload,
  SysmonHistoryConfigPayload,
  SysmonSamplingConfigPayload,
} from '@/types/generated'

// 资源监控「配置」tab（《帧契约：系统监控》§3）：展示当前**生效**配置，并给 admin 提供编辑入口。
//
// 数据源是 store 的配置镜像（WS `sysmon_config`；首次挂载经 GET /api/sysmon/config 补拉）。
// - 展示：所有已认证用户可见；镜像未到达时只显示「配置尚未到达」空态，**不显示契约默认值冒充生效值**。
// - 编辑：仅 admin（与系统设置页一致）；表单保存时只下发与原镜像的**深度差异**（merge patch），
//   groups 只带改动的键、数组字段整体替换；保存成功后保持 pending 直到 WS 回推生效值。
// - 镜像每次变化都重置表单草稿：后端校验失败回推旧值时表单跟随回退，不显示未生效的值。
const sysmon = useSysmonStore()
const auth = useAuthStore()
const toast = useToastStore()

/** 指标大类（契约 sysmon §3.1 sampling.groups）：展示与编辑都按这 8 项全量覆盖 */
const METRIC_GROUPS = [
  { key: 'cpu', label: 'CPU' },
  { key: 'memory', label: '内存' },
  { key: 'swap', label: '交换' },
  { key: 'volumes', label: '卷容量' },
  { key: 'disk_io', label: '磁盘 I/O' },
  { key: 'network', label: '网络' },
  { key: 'temperature', label: '温度' },
  { key: 'cpu_frequency', label: 'CPU 频率' },
] as const

/** 数字字段描述：label 用于表单与校验提示，unit 只用于展示/输入标签 */
interface NumberFieldSpec {
  key: string
  label: string
  unit: string
  integer: boolean
}

const FIELDS = {
  measureInterval: { key: 'measure_interval_ms', label: '计量间隔', unit: 'ms', integer: true },
  recordInterval: { key: 'record_interval_sec', label: '记录间隔', unit: '秒', integer: true },
  phaseOffset: { key: 'phase_offset_sec', label: '记录相位偏移', unit: '秒', integer: true },
  rawRetention: { key: 'raw_retention_hours', label: '原始记录保留时长', unit: '小时', integer: true },
  maxRetention: { key: 'max_retention_days', label: '聚合记录保留时长', unit: '天', integer: true },
  maxBytes: { key: 'max_bytes', label: '历史库总量上限', unit: '字节', integer: true },
  pushInterval: { key: 'push_interval_sec', label: '推送间隔', unit: '秒', integer: true },
  cpuSustainPct: { key: 'cpu_sustain_used_pct', label: 'CPU 持续使用率阈值', unit: '%', integer: false },
  cpuSustainSamples: { key: 'cpu_sustain_samples', label: 'CPU 持续采样数', unit: '次', integer: true },
  memoryUsedPct: { key: 'memory_used_pct', label: '内存使用率阈值', unit: '%', integer: false },
  swapUsedPct: { key: 'swap_used_pct', label: '交换使用率阈值', unit: '%', integer: false },
  volumeFreePct: { key: 'volume_free_pct', label: '关注卷剩余空间阈值（百分比）', unit: '%', integer: false },
  volumeFreeBytes: { key: 'volume_free_bytes', label: '关注卷剩余空间阈值（绝对容量）', unit: '字节', integer: true },
  cooldown: { key: 'cooldown_sec', label: '告警冷却时间', unit: '秒', integer: true },
} as const satisfies Record<string, NumberFieldSpec>

const SAMPLING_FIELDS: readonly NumberFieldSpec[] = [FIELDS.measureInterval, FIELDS.recordInterval, FIELDS.phaseOffset]
const HISTORY_FIELDS: readonly NumberFieldSpec[] = [FIELDS.rawRetention, FIELDS.maxRetention, FIELDS.maxBytes]
const REALTIME_FIELDS: readonly NumberFieldSpec[] = [FIELDS.pushInterval]
const ALERTS_FIELDS: readonly NumberFieldSpec[] = [
  FIELDS.cpuSustainPct,
  FIELDS.cpuSustainSamples,
  FIELDS.memoryUsedPct,
  FIELDS.swapUsedPct,
  FIELDS.volumeFreePct,
  FIELDS.volumeFreeBytes,
  FIELDS.cooldown,
]

const canEdit = computed(() => auth.isAdmin)
const config = computed(() => sysmon.config)
const savePending = computed(() => sysmon.configPending)

/** 展示值格式化：字节 → 人类可读（历史库总量上限用；0 = 不限制由调用方单独处理） */
function formatBytes(bytes: number): string {
  if (!Number.isFinite(bytes) || bytes < 0) return '—'
  const gib = 1024 ** 3
  const mib = 1024 ** 2
  if (bytes >= gib) return `${(bytes / gib).toFixed(1)} GiB`
  if (bytes >= mib) return `${(bytes / mib).toFixed(1)} MiB`
  return `${bytes} B`
}

function enabledText(enabled: boolean): string {
  return enabled ? '已启用' : '已关闭'
}

/** 展示行：一个字段一行；list 用于逐条展示的数组字段（watch_paths / devices） */
interface DisplayRow {
  field: string
  label: string
  value?: string
  list?: string[]
}

interface DisplaySection {
  key: string
  title: string
  rows: DisplayRow[]
}

/**
 * 六个配置段的只读展示（数据全部来自生效镜像；镜像为 null 时不产出任何行）。
 * 数字一律带单位、布尔一律「已启用 / 已关闭」，缺失键按契约 §3.1「缺省 true」解释。
 */
const displaySections = computed<DisplaySection[]>(() => {
  const cfg = config.value
  if (!cfg) return []
  return [
    {
      key: 'sampling',
      title: '采样',
      rows: [
        { field: 'sampling.measure_interval_ms', label: '计量间隔', value: `${cfg.sampling.measure_interval_ms} ms` },
        { field: 'sampling.record_interval_sec', label: '记录间隔', value: `${cfg.sampling.record_interval_sec} 秒` },
        { field: 'sampling.phase_offset_sec', label: '记录相位偏移', value: `${cfg.sampling.phase_offset_sec} 秒` },
        ...METRIC_GROUPS.map(group => ({
          field: `sampling.groups.${group.key}`,
          label: `指标大类 · ${group.label}`,
          value: enabledText(cfg.sampling.groups[group.key] !== false),
        })),
      ],
    },
    {
      key: 'disk',
      title: '磁盘',
      rows: [
        cfg.disk.watch_paths.length > 0
          ? { field: 'disk.watch_paths', label: '关注路径', list: cfg.disk.watch_paths }
          : { field: 'disk.watch_paths', label: '关注路径', value: '默认关注路径（共享内存目录与数据库目录）' },
        cfg.disk.devices.length > 0
          ? { field: 'disk.devices', label: '设备范围', list: cfg.disk.devices }
          : { field: 'disk.devices', label: '设备范围', value: '全部物理盘' },
      ],
    },
    {
      key: 'network',
      title: '网络',
      rows: [
        { field: 'network.hide_virtual', label: '隐藏虚拟网卡', value: enabledText(cfg.network.hide_virtual) },
      ],
    },
    {
      key: 'history',
      title: '历史记录',
      rows: [
        { field: 'history.enabled', label: '历史记录', value: enabledText(cfg.history.enabled) },
        { field: 'history.raw_retention_hours', label: '原始记录保留', value: `${cfg.history.raw_retention_hours} 小时` },
        { field: 'history.max_retention_days', label: '聚合记录保留', value: `${cfg.history.max_retention_days} 天` },
        {
          field: 'history.max_bytes',
          label: '历史库总量上限',
          value: cfg.history.max_bytes === 0 ? '不限制' : formatBytes(cfg.history.max_bytes),
        },
      ],
    },
    {
      key: 'realtime',
      title: '实时推送',
      rows: [
        { field: 'realtime.push_interval_sec', label: '推送间隔', value: `${cfg.realtime.push_interval_sec} 秒` },
      ],
    },
    {
      key: 'alerts',
      title: '阈值告警',
      rows: [
        { field: 'alerts.cpu_sustain_used_pct', label: 'CPU 持续使用率阈值', value: `${cfg.alerts.cpu_sustain_used_pct}%` },
        { field: 'alerts.cpu_sustain_samples', label: 'CPU 持续采样数', value: `${cfg.alerts.cpu_sustain_samples} 次` },
        { field: 'alerts.memory_used_pct', label: '内存使用率阈值', value: `${cfg.alerts.memory_used_pct}%` },
        { field: 'alerts.swap_used_pct', label: '交换使用率阈值', value: `${cfg.alerts.swap_used_pct}%` },
        { field: 'alerts.volume_free_pct', label: '关注卷剩余空间阈值', value: `${cfg.alerts.volume_free_pct}%` },
        { field: 'alerts.volume_free_bytes', label: '关注卷剩余空间阈值', value: formatBytes(cfg.alerts.volume_free_bytes) },
        { field: 'alerts.cooldown_sec', label: '告警冷却时间', value: `${cfg.alerts.cooldown_sec} 秒` },
      ],
    },
  ]
})

// ===== 编辑表单草稿（仅 admin） =====
// 数字字段用 string 保存（允许清空/中间态），保存时统一解析；与镜像不同源，镜像变化即重置，
// 保证「表单里看到的永远是生效值，或用户刚输入、尚未保存的草稿」。
interface ConfigForm {
  // 数字字段：初始化时是镜像值的字符串形式；Vue 对 type=number 输入会按数值回写，
  // 用户编辑后可能是 number —— 解析函数两种都接受，绝不假设只有一种类型。
  samplingNumbers: Record<string, string | number>
  groups: Record<string, boolean>
  disk: { watch_paths: string; devices: string }
  hideVirtual: boolean
  historyNumbers: Record<string, string | number>
  historyEnabled: boolean
  realtimeNumbers: Record<string, string | number>
  alertNumbers: Record<string, string | number>
}

function formFromConfig(cfg: SysmonConfigPayload): ConfigForm {
  return {
    samplingNumbers: {
      [FIELDS.measureInterval.key]: String(cfg.sampling.measure_interval_ms),
      [FIELDS.recordInterval.key]: String(cfg.sampling.record_interval_sec),
      [FIELDS.phaseOffset.key]: String(cfg.sampling.phase_offset_sec),
    },
    groups: Object.fromEntries(METRIC_GROUPS.map(group => [group.key, cfg.sampling.groups[group.key] !== false])),
    disk: {
      watch_paths: cfg.disk.watch_paths.join('\n'),
      devices: cfg.disk.devices.join('\n'),
    },
    hideVirtual: cfg.network.hide_virtual,
    historyNumbers: {
      [FIELDS.rawRetention.key]: String(cfg.history.raw_retention_hours),
      [FIELDS.maxRetention.key]: String(cfg.history.max_retention_days),
      [FIELDS.maxBytes.key]: String(cfg.history.max_bytes),
    },
    historyEnabled: cfg.history.enabled,
    realtimeNumbers: {
      [FIELDS.pushInterval.key]: String(cfg.realtime.push_interval_sec),
    },
    alertNumbers: Object.fromEntries(ALERTS_FIELDS.map(spec => [spec.key, String(cfg.alerts[spec.key as keyof SysmonAlertsConfigPayload])])),
  }
}

const form = reactive<ConfigForm>({
  samplingNumbers: {},
  groups: {},
  disk: { watch_paths: '', devices: '' },
  hideVirtual: true,
  historyNumbers: {},
  historyEnabled: true,
  realtimeNumbers: {},
  alertNumbers: {},
})

/** 表单草稿跟随配置镜像（WS 回推 / REST 补拉 / 校验失败回推旧值都走这里） */
function resetForm(cfg: SysmonConfigPayload | null): void {
  if (!cfg) return
  const next = formFromConfig(cfg)
  form.samplingNumbers = next.samplingNumbers
  form.groups = next.groups
  form.disk = next.disk
  form.hideVirtual = next.hideVirtual
  form.historyNumbers = next.historyNumbers
  form.historyEnabled = next.historyEnabled
  form.realtimeNumbers = next.realtimeNumbers
  form.alertNumbers = next.alertNumbers
}

watch(config, (cfg) => { resetForm(cfg) }, { immediate: true })

/**
 * 解析数字输入：空 / 非数字 / 要求整数却给了小数 → null（明显非法值不下发，后端也会严格校验）。
 * `type="number"` 的 v-model 会把用户输入按数值回写（字符串初值 → number），两种都要接受。
 */
function parseNumberInput(raw: string | number | undefined, integer: boolean): number | null {
  if (typeof raw === 'number') {
    if (!Number.isFinite(raw)) return null
    if (integer && !Number.isInteger(raw)) return null
    return raw
  }
  const text = (raw ?? '').trim()
  if (text === '') return null
  const value = Number(text)
  if (!Number.isFinite(value)) return null
  if (integer && !Number.isInteger(value)) return null
  return value
}

/** 多行文本 → 数组：一行一个，去首尾空白、丢弃空行（数组字段整体替换，契约 §3） */
function splitLines(text: string): string[] {
  return text.split('\n').map(line => line.trim()).filter(line => line !== '')
}

interface ParsedForm {
  /** null = 存在非法数字（invalidLabel 给出首个非法字段的中文标签） */
  config: SysmonConfigPayload | null
  invalidLabel: string | null
}

/** 把表单草稿解析成一份完整配置（保存前统一解析，diff 与校验共用） */
function parseForm(): ParsedForm {
  let invalidLabel: string | null = null
  const number = (raw: string | number | undefined, spec: NumberFieldSpec): number => {
    const parsed = parseNumberInput(raw, spec.integer)
    if (parsed === null) {
      invalidLabel ??= spec.label
      return 0 // 占位值；invalidLabel 非空时整份 config 不会被使用
    }
    return parsed
  }
  const parsed: SysmonConfigPayload = {
    sampling: {
      measure_interval_ms: number(form.samplingNumbers[FIELDS.measureInterval.key], FIELDS.measureInterval),
      record_interval_sec: number(form.samplingNumbers[FIELDS.recordInterval.key], FIELDS.recordInterval),
      phase_offset_sec: number(form.samplingNumbers[FIELDS.phaseOffset.key], FIELDS.phaseOffset),
      groups: { ...form.groups },
    },
    disk: {
      watch_paths: splitLines(form.disk.watch_paths),
      devices: splitLines(form.disk.devices),
    },
    network: { hide_virtual: form.hideVirtual },
    history: {
      enabled: form.historyEnabled,
      raw_retention_hours: number(form.historyNumbers[FIELDS.rawRetention.key], FIELDS.rawRetention),
      max_retention_days: number(form.historyNumbers[FIELDS.maxRetention.key], FIELDS.maxRetention),
      max_bytes: number(form.historyNumbers[FIELDS.maxBytes.key], FIELDS.maxBytes),
    },
    realtime: { push_interval_sec: number(form.realtimeNumbers[FIELDS.pushInterval.key], FIELDS.pushInterval) },
    alerts: {
      cpu_sustain_used_pct: number(form.alertNumbers[FIELDS.cpuSustainPct.key], FIELDS.cpuSustainPct),
      cpu_sustain_samples: number(form.alertNumbers[FIELDS.cpuSustainSamples.key], FIELDS.cpuSustainSamples),
      memory_used_pct: number(form.alertNumbers[FIELDS.memoryUsedPct.key], FIELDS.memoryUsedPct),
      swap_used_pct: number(form.alertNumbers[FIELDS.swapUsedPct.key], FIELDS.swapUsedPct),
      volume_free_pct: number(form.alertNumbers[FIELDS.volumeFreePct.key], FIELDS.volumeFreePct),
      volume_free_bytes: number(form.alertNumbers[FIELDS.volumeFreeBytes.key], FIELDS.volumeFreeBytes),
      cooldown_sec: number(form.alertNumbers[FIELDS.cooldown.key], FIELDS.cooldown),
    },
  }
  return { config: invalidLabel === null ? parsed : null, invalidLabel }
}

function sameStringArray(a: string[], b: string[]): boolean {
  return a.length === b.length && a.every((item, index) => item === b[index])
}

/**
 * 深度差异：只产出与原生效镜像不同的项（RFC 7386 merge patch，契约 sysmon §3）。
 * - sampling.groups：只带改动的键（缺省键按 true 处理，不能因为镜像里没有该键就误判成改动）；
 * - disk.watch_paths / devices：数组整体替换，逐项比较决定是否下发；
 * - 各数字字段按数值比较（表单文本与镜像文本不同但数值相同不算改动）。
 */
function diffConfig(next: SysmonConfigPayload, current: SysmonConfigPayload): SysmonConfigPatch {
  const patch: SysmonConfigPatch = {}

  const sampling: Partial<SysmonSamplingConfigPayload> = {}
  if (next.sampling.measure_interval_ms !== current.sampling.measure_interval_ms) {
    sampling.measure_interval_ms = next.sampling.measure_interval_ms
  }
  if (next.sampling.record_interval_sec !== current.sampling.record_interval_sec) {
    sampling.record_interval_sec = next.sampling.record_interval_sec
  }
  if (next.sampling.phase_offset_sec !== current.sampling.phase_offset_sec) {
    sampling.phase_offset_sec = next.sampling.phase_offset_sec
  }
  const groups: Record<string, boolean> = {}
  for (const group of METRIC_GROUPS) {
    const currentEnabled = current.sampling.groups[group.key] !== false
    const nextEnabled = next.sampling.groups[group.key] !== false
    if (nextEnabled !== currentEnabled) groups[group.key] = nextEnabled
  }
  if (Object.keys(groups).length > 0) sampling.groups = groups
  if (Object.keys(sampling).length > 0) patch.sampling = sampling

  const disk: Partial<SysmonDiskConfigPayload> = {}
  if (!sameStringArray(next.disk.watch_paths, current.disk.watch_paths)) {
    disk.watch_paths = next.disk.watch_paths
  }
  if (!sameStringArray(next.disk.devices, current.disk.devices)) {
    disk.devices = next.disk.devices
  }
  if (Object.keys(disk).length > 0) patch.disk = disk

  if (next.network.hide_virtual !== current.network.hide_virtual) {
    patch.network = { hide_virtual: next.network.hide_virtual }
  }

  const history: Partial<SysmonHistoryConfigPayload> = {}
  if (next.history.enabled !== current.history.enabled) history.enabled = next.history.enabled
  if (next.history.raw_retention_hours !== current.history.raw_retention_hours) {
    history.raw_retention_hours = next.history.raw_retention_hours
  }
  if (next.history.max_retention_days !== current.history.max_retention_days) {
    history.max_retention_days = next.history.max_retention_days
  }
  if (next.history.max_bytes !== current.history.max_bytes) history.max_bytes = next.history.max_bytes
  if (Object.keys(history).length > 0) patch.history = history

  if (next.realtime.push_interval_sec !== current.realtime.push_interval_sec) {
    patch.realtime = { push_interval_sec: next.realtime.push_interval_sec }
  }

  const alerts: Partial<SysmonAlertsConfigPayload> = {}
  if (next.alerts.cpu_sustain_used_pct !== current.alerts.cpu_sustain_used_pct) {
    alerts.cpu_sustain_used_pct = next.alerts.cpu_sustain_used_pct
  }
  if (next.alerts.cpu_sustain_samples !== current.alerts.cpu_sustain_samples) {
    alerts.cpu_sustain_samples = next.alerts.cpu_sustain_samples
  }
  if (next.alerts.memory_used_pct !== current.alerts.memory_used_pct) {
    alerts.memory_used_pct = next.alerts.memory_used_pct
  }
  if (next.alerts.swap_used_pct !== current.alerts.swap_used_pct) {
    alerts.swap_used_pct = next.alerts.swap_used_pct
  }
  if (next.alerts.volume_free_pct !== current.alerts.volume_free_pct) {
    alerts.volume_free_pct = next.alerts.volume_free_pct
  }
  if (next.alerts.volume_free_bytes !== current.alerts.volume_free_bytes) {
    alerts.volume_free_bytes = next.alerts.volume_free_bytes
  }
  if (next.alerts.cooldown_sec !== current.alerts.cooldown_sec) {
    alerts.cooldown_sec = next.alerts.cooldown_sec
  }
  if (Object.keys(alerts).length > 0) patch.alerts = alerts

  return patch
}

/** 当前草稿与生效镜像的差异；invalidLabel 非空表示存在非法数字（也视为「有改动」，点保存时给出提示） */
const draft = computed<{ patch: SysmonConfigPatch; invalidLabel: string | null }>(() => {
  const parsed = parseForm()
  const cfg = config.value
  if (!parsed.config || !cfg) return { patch: {}, invalidLabel: parsed.invalidLabel }
  return { patch: diffConfig(parsed.config, cfg), invalidLabel: null }
})

const dirty = computed(() => draft.value.invalidLabel !== null || Object.keys(draft.value.patch).length > 0)
const saveDisabled = computed(() => !canEdit.value || savePending.value || !dirty.value)

/**
 * 保存：只下发差异 patch。成功（REST 已写事件通道）后 toast「配置已保存」并保持 pending，
 * 生效信号仍是 WS `sysmon_config` 回推（校验失败时发送方回推旧值，表单随镜像回退）。
 */
async function onSave(): Promise<void> {
  if (saveDisabled.value) return
  const { patch, invalidLabel } = draft.value
  if (invalidLabel !== null) {
    toast.error(`${invalidLabel}需为有效数字`)
    return
  }
  if (Object.keys(patch).length === 0) return
  const result = await sysmon.setConfig(patch)
  if (result === PENDING_TIMEOUT) return
  if (result === false) {
    toast.error('系统监控配置保存下发失败')
    return
  }
  toast.success('配置已保存')
}
</script>

<template>
  <div class="sysmon-config" data-test="sysmon-config-tab">
    <!-- 非 admin：只读展示 + 明确提示（与系统设置页一致：编辑入口仅 admin） -->
    <div
      v-if="!canEdit"
      class="ds-alert ds-alert--info"
      data-test="sysmon-config-readonly"
      role="status"
    >
      <span class="ds-alert__icon"><Icon name="Lock" :size="16" /></span>
      <div>
        <div class="ds-alert__title">仅管理员可修改</div>
        <div class="ds-alert__desc">以下为当前生效配置（只读）；如需修改，请使用管理员账号。</div>
      </div>
    </div>

    <!-- 配置镜像尚未到达：明确空态，不显示契约默认值冒充生效值 -->
    <div v-if="!config" class="ds-empty sysmon-config__state" data-test="sysmon-config-unavailable">
      <div class="ds-empty__icon"><Icon name="info" :size="20" /></div>
      <div class="ds-empty__title">配置尚未到达</div>
      <div class="ds-empty__desc">dzsysmon 尚未上报生效配置，配置到达后本页自动更新</div>
    </div>

    <template v-else>
      <!-- 生效配置展示：按契约六个段分组 -->
      <div class="sysmon-config__grid">
        <section
          v-for="section in displaySections"
          :key="section.key"
          class="ds-card sysmon-config__card"
          :data-test="`sysmon-config-section-${section.key}`"
        >
          <h2 class="ds-card__title sysmon-config__card-title">{{ section.title }}</h2>
          <dl class="sysmon-config__rows">
            <div
              v-for="row in section.rows"
              :key="row.field"
              class="sysmon-config__row"
            >
              <dt class="sysmon-config__label">{{ row.label }}</dt>
              <dd class="sysmon-config__value" :data-test="`sysmon-config-value-${row.field}`">
                <template v-if="row.list">
                  <span v-for="(item, index) in row.list" :key="`${item}-${index}`" class="sysmon-config__list-item">{{ item }}</span>
                </template>
                <template v-else>{{ row.value }}</template>
              </dd>
            </div>
          </dl>
        </section>
      </div>

      <!-- 编辑入口（仅 admin）：草稿与生效镜像分离，保存只下发差异 -->
      <section v-if="canEdit" class="ds-card sysmon-config__editor" data-test="sysmon-config-editor">
        <h2 class="ds-card__title sysmon-config__card-title">修改配置</h2>
        <p class="sysmon-config__hint">
          保存只下发与原生效配置不同的项；实际生效值以 dzsysmon 回推为准，校验失败时表单回退到生效值。
        </p>

        <div class="sysmon-config__form">
          <section class="sysmon-config__form-section" data-test="sysmon-config-editor-sampling">
            <h3 class="sysmon-config__form-title">采样</h3>
            <div class="sysmon-config__fields">
              <label
                v-for="field in SAMPLING_FIELDS"
                :key="field.key"
                class="sysmon-config__field"
              >
                <span class="sysmon-config__field-label">{{ field.label }}（{{ field.unit }}）</span>
                <span class="ds-input">
                  <input
                    v-model="form.samplingNumbers[field.key]"
                    type="number"
                    :disabled="savePending"
                    :data-test="`sysmon-config-input-${field.key}`"
                  >
                </span>
              </label>
            </div>
            <div class="sysmon-config__field">
              <span class="sysmon-config__field-label">指标大类</span>
              <div class="sysmon-config__groups" data-test="sysmon-config-groups">
                <label v-for="group in METRIC_GROUPS" :key="group.key" class="ds-check">
                  <input
                    v-model="form.groups[group.key]"
                    type="checkbox"
                    :disabled="savePending"
                    :data-test="`sysmon-config-group-${group.key}`"
                  >
                  <span class="ds-check__box"></span>
                  {{ group.label }}
                </label>
              </div>
            </div>
          </section>

          <section class="sysmon-config__form-section" data-test="sysmon-config-editor-disk">
            <h3 class="sysmon-config__form-title">磁盘</h3>
            <div class="sysmon-config__fields">
              <label class="sysmon-config__field">
                <span class="sysmon-config__field-label">关注路径（一行一个；留空 = 使用默认关注路径）</span>
                <textarea
                  v-model="form.disk.watch_paths"
                  class="ds-textarea"
                  rows="3"
                  :disabled="savePending"
                  data-test="sysmon-config-input-disk.watch_paths"
                ></textarea>
              </label>
              <label class="sysmon-config__field">
                <span class="sysmon-config__field-label">设备范围（一行一个设备标识；留空 = 全部物理盘）</span>
                <textarea
                  v-model="form.disk.devices"
                  class="ds-textarea"
                  rows="3"
                  :disabled="savePending"
                  data-test="sysmon-config-input-disk.devices"
                ></textarea>
              </label>
            </div>
          </section>

          <section class="sysmon-config__form-section" data-test="sysmon-config-editor-network">
            <h3 class="sysmon-config__form-title">网络</h3>
            <div class="sysmon-config__fields">
              <label class="ds-check">
                <input
                  v-model="form.hideVirtual"
                  type="checkbox"
                  :disabled="savePending"
                  data-test="sysmon-config-input-network.hide_virtual"
                >
                <span class="ds-check__box"></span>
                隐藏虚拟网卡
              </label>
            </div>
          </section>

          <section class="sysmon-config__form-section" data-test="sysmon-config-editor-history">
            <h3 class="sysmon-config__form-title">历史记录</h3>
            <div class="sysmon-config__fields">
              <label class="ds-check">
                <input
                  v-model="form.historyEnabled"
                  type="checkbox"
                  :disabled="savePending"
                  data-test="sysmon-config-input-history.enabled"
                >
                <span class="ds-check__box"></span>
                启用历史记录
              </label>
              <label
                v-for="field in HISTORY_FIELDS"
                :key="field.key"
                class="sysmon-config__field"
              >
                <span class="sysmon-config__field-label">{{ field.label }}（{{ field.unit }}）</span>
                <span class="ds-input">
                  <input
                    v-model="form.historyNumbers[field.key]"
                    type="number"
                    :disabled="savePending"
                    :data-test="`sysmon-config-input-${field.key}`"
                  >
                </span>
              </label>
            </div>
          </section>

          <section class="sysmon-config__form-section" data-test="sysmon-config-editor-realtime">
            <h3 class="sysmon-config__form-title">实时推送</h3>
            <div class="sysmon-config__fields">
              <label
                v-for="field in REALTIME_FIELDS"
                :key="field.key"
                class="sysmon-config__field"
              >
                <span class="sysmon-config__field-label">{{ field.label }}（{{ field.unit }}）</span>
                <span class="ds-input">
                  <input
                    v-model="form.realtimeNumbers[field.key]"
                    type="number"
                    :disabled="savePending"
                    :data-test="`sysmon-config-input-${field.key}`"
                  >
                </span>
              </label>
            </div>
          </section>

          <section class="sysmon-config__form-section" data-test="sysmon-config-editor-alerts">
            <h3 class="sysmon-config__form-title">阈值告警</h3>
            <div class="sysmon-config__fields">
              <label
                v-for="field in ALERTS_FIELDS"
                :key="field.key"
                class="sysmon-config__field"
              >
                <span class="sysmon-config__field-label">{{ field.label }}<template v-if="field.unit">（{{ field.unit }}）</template></span>
                <span class="ds-input">
                  <input
                    v-model="form.alertNumbers[field.key]"
                    type="number"
                    :disabled="savePending"
                    :data-test="`sysmon-config-input-${field.key}`"
                  >
                </span>
              </label>
            </div>
          </section>
        </div>

        <div class="sysmon-config__footer">
          <button
            class="ds-btn ds-btn--primary"
            type="button"
            :disabled="saveDisabled"
            data-test="sysmon-config-save"
            @click="onSave"
          >
            <span v-if="savePending" class="ds-btn__spinner"></span>
            {{ savePending ? '保存中...' : '保存' }}
          </button>
          <span v-if="!dirty && !savePending" class="sysmon-config__hint" data-test="sysmon-config-clean">无改动</span>
          <span v-else-if="savePending" class="sysmon-config__hint">等待 dzsysmon 回推生效值</span>
        </div>
      </section>
    </template>
  </div>
</template>

<style scoped>
.sysmon-config {
  display: flex;
  flex-direction: column;
  gap: var(--spacer-16);
}

.sysmon-config__state {
  padding: var(--spacer-32) var(--spacer-12);
}

.sysmon-config__grid {
  display: grid;
  grid-template-columns: repeat(auto-fill, minmax(340px, 1fr));
  gap: var(--spacer-16);
}

.sysmon-config__card {
  min-width: 0;
}

.sysmon-config__card-title {
  margin: 0 0 var(--spacer-12);
}

.sysmon-config__rows {
  display: flex;
  flex-direction: column;
  gap: var(--spacer-8);
  margin: 0;
}

.sysmon-config__row {
  display: flex;
  align-items: baseline;
  justify-content: space-between;
  gap: var(--spacer-12);
  min-width: 0;
}

.sysmon-config__label {
  flex: 0 0 auto;
  color: var(--text-tertiary);
  font-size: var(--body-sm-font-size);
  line-height: var(--body-sm-line-height);
}

.sysmon-config__value {
  display: flex;
  flex-direction: column;
  align-items: flex-end;
  gap: var(--spacer-2);
  min-width: 0;
  margin: 0;
  color: var(--text-default);
  font-family: var(--font-family-metric);
  font-size: var(--body-sm-font-size);
  line-height: var(--body-sm-line-height);
  font-variant-numeric: tabular-nums;
  text-align: right;
  overflow-wrap: anywhere;
}

.sysmon-config__list-item {
  white-space: nowrap;
}

.sysmon-config__editor {
  display: flex;
  flex-direction: column;
  gap: var(--spacer-16);
}

.sysmon-config__hint {
  margin: 0;
  color: var(--text-tertiary);
  font-size: var(--body-xs-font-size);
  line-height: var(--body-xs-line-height);
}

.sysmon-config__form {
  display: grid;
  grid-template-columns: repeat(auto-fill, minmax(320px, 1fr));
  gap: var(--spacer-16);
}

.sysmon-config__form-section {
  display: flex;
  flex-direction: column;
  gap: var(--spacer-8);
  min-width: 0;
}

.sysmon-config__form-title {
  margin: 0;
  color: var(--text-secondary);
  font-size: var(--body-sm-strong-font-size);
  line-height: var(--body-sm-strong-line-height);
  font-weight: var(--body-sm-strong-font-weight);
}

.sysmon-config__fields {
  display: flex;
  flex-direction: column;
  gap: var(--spacer-8);
}

.sysmon-config__field {
  display: flex;
  flex-direction: column;
  gap: var(--spacer-4);
  min-width: 0;
}

.sysmon-config__field-label {
  color: var(--text-tertiary);
  font-size: var(--body-xs-font-size);
  line-height: var(--body-xs-line-height);
}

.sysmon-config__groups {
  display: flex;
  flex-wrap: wrap;
  gap: var(--spacer-4) var(--spacer-16);
}

.sysmon-config__footer {
  display: flex;
  align-items: center;
  gap: var(--spacer-12);
  flex-wrap: wrap;
}

@media (max-width: 767.98px) {
  .sysmon-config__grid,
  .sysmon-config__form {
    grid-template-columns: 1fr;
  }
}
</style>
