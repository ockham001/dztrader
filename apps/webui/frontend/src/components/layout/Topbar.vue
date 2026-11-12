<script setup lang="ts">
import { computed, nextTick, onMounted, onUnmounted, ref, watch } from 'vue'
import Icon from '@/components/shared/Icon.vue'
import ThemeDropdown from './ThemeDropdown.vue'
import TopbarResourceBadge from './TopbarResourceBadge.vue'
import { UNAVAILABLE_TEXT, formatAge } from '@/composables/format'
import { processCategory, type ProcessCategory } from '@/composables/processNaming'
import {
  CLOCK_BADGE_LABEL,
  clockBadge,
  clockProbeAgeSec,
  resolveClockThresholds,
} from '@/composables/sysmonClock'
import { visibleItemCount } from '@/composables/topbarOverflow'
import { useBreakpoint } from '@/composables/useBreakpoint'
import { processStateLevel, processStateText, type ProcessLevel } from '@/composables/useProcessState'
import {
  resourceLevel,
  resourceLevelClass,
  worstResourceLevel,
  type ResourceLevel,
} from '@/composables/useResourceLevel'
import { useWebSocket } from '@/composables/wsClient'
import { useProcessStore } from '@/stores/process'
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

// 顶栏资源徽章（CPU / 内存 / 时钟 / 各卷）：契约 sysmon 实时快照（固定实例 dzsysmon）。
// 首次挂载经 REST 补拉镜像，之后只由 WS 推送刷新 —— 刷新基线由发送方
// realtime.push_interval_sec（默认 5s）决定，前端不轮询、不改 WS 契约。
const sysmon = useSysmonStore()
onMounted(() => { void sysmon.init() })

// 顶栏密度（#127 / ADR 0048）：断点只做**窄屏收敛**的行为分支 —— <lg 把整组资源徽章收成单个
// 状态图标（layout.css 既有 topbar__resources--gauge / topbar__gauge 折叠模式类）；≥lg 走
// **按可用宽度自适应折叠**（`composables/topbarOverflow` 纯函数 + ResizeObserver 实测宽度）。
// 档位唯一来源是 useBreakpoint（与 CSS 断点白名单同源）；layout.css 的 gauge 折叠规则本身
// 不含宽度 @media，因此窄屏判定只能走 JS 行为分支——本组件不新增 @media，也不用容器查询。
const { tier } = useBreakpoint()
const isWide = computed(() => tier.value === 'lg' || tier.value === 'xl')

/** 资源项（`kind` 决定形态：`disk` = 状态点，其余 = 标签 + 数值） */
interface ResourceItem {
  /** 稳定键（`v-for :key` / 浮层条目锚点；不用中文标签做标识） */
  key: string
  kind: 'cpu' | 'mem' | 'ntp' | 'disk'
  label: string
  value: string
  level: ResourceLevel | null
  title: string
  /** 元素 id（CPU / MEM / NTP 三枚固定徽章用，供既有测试锚定） */
  domId?: string
  /** 卷挂载点（`disk` 形态；同时写进 `data-mount`） */
  mount?: string
  testId?: string
  /**
   * 浮层里显示的标签（可选）：顶栏徽章受窄条限制只给挂载点末段（`/mnt/nas/team` → `team`），
   * 浮层有整行宽度，显示完整挂载点才不会让人猜是哪一卷。
   */
  overflowLabel?: string
  /**
   * 浮层里显示的读数（可选）：卷徽章本身只有状态点，浮层补上百分比 / 不可用文案。
   * `value` 是徽章上的数值文本，两者语义不同（卷徽章不显示 value），因此单独一个字段。
   */
  overflowValue?: string
}

const cpuItem = computed<ResourceItem>(() => {
  const entry = sysmon.availabilityOf('cpu.total')
  if (!sysmon.snapshot) {
    // 快照尚未到达：显示等待态，不得用假数字占位
    return {
      key: 'cpu',
      kind: 'cpu',
      label: 'CPU',
      value: '—',
      level: null,
      title: 'CPU 使用率（等待 dzsysmon 快照）',
      domId: 'resCpu',
    }
  }
  const ratio = sysmon.cpuUsageRatio
  if (!entry || !entry.available || ratio === null) {
    // 不可用/首次采样：按契约显示不可用，不得用 0 冒充（契约 sysmon §2.7）
    return {
      key: 'cpu',
      kind: 'cpu',
      label: 'CPU',
      value: UNAVAILABLE_TEXT,
      level: null,
      title: entry?.reason ? `CPU 使用率不可用: ${entry.reason}` : 'CPU 使用率不可用',
      domId: 'resCpu',
    }
  }
  // 顶栏徽章是横向窄条，刻意用整数百分比（监控页用共享模块的 1 位小数）：同一数值在两处
  // 精度不同是**有意的取舍**，不是格式化漂移——顶栏要在常驻条里塞下 CPU / 内存 / 时钟 / 多个
  // 卷徽章，小数位会挤掉后面的项。状态色仍与监控页共用 resourceLevel，结论不会不一致（#126）。
  const percent = Math.round(ratio * 100)
  // 状态色与监控页卷列表共用 resourceLevel（含边界），不在此处各自比较阈值
  const level = resourceLevel(ratio)
  return {
    key: 'cpu',
    kind: 'cpu',
    label: 'CPU',
    value: `${percent}%`,
    level,
    title: `CPU 使用率 ${percent}%`,
    domId: 'resCpu',
  }
})

// 内存徽章与 CPU 徽章同构：等待 / 不可用 / 百分比 + 阈值状态色；比率取自 store 净化后的
// memoryInfo（可用性由 availability.metrics.memory 承载），0 不是合法占位（契约 sysmon §2.4）。
const memItem = computed<ResourceItem>(() => {
  const entry = sysmon.availabilityOf('memory')
  if (!sysmon.snapshot) {
    return {
      key: 'mem',
      kind: 'mem',
      label: 'MEM',
      value: '—',
      level: null,
      title: '内存使用率（等待 dzsysmon 快照）',
      domId: 'resMem',
    }
  }
  const ratio = sysmon.memoryInfo?.used_ratio ?? null
  if (!entry || !entry.available || ratio === null) {
    return {
      key: 'mem',
      kind: 'mem',
      label: 'MEM',
      value: UNAVAILABLE_TEXT,
      level: null,
      title: entry?.reason ? `内存使用率不可用: ${entry.reason}` : '内存使用率不可用',
      domId: 'resMem',
    }
  }
  // 整数百分比：与 CPU 徽章同一取舍（窄条优先，见上）
  const percent = Math.round(ratio * 100)
  const level = resourceLevel(ratio)
  return {
    key: 'mem',
    kind: 'mem',
    label: 'MEM',
    value: `${percent}%`,
    level,
    title: `内存使用率 ${percent}%`,
    domId: 'resMem',
  }
})

/**
 * 时钟偏差徽章（ADR 0048；契约 sysmon §2.9「顶栏徽章」条）：位置在 CPU / 内存之后、卷之前。
 *
 * 四态与阈值口径全部在 `composables/sysmonClock.clockBadge`（纯函数、可单测）：`null` 表示该域
 * 被配置关闭 → **整枚徽章不渲染**（配置关闭是运维事实，顶栏不放无信息空壳）。相对时间基准取
 * 快照 `timestamp`（与服务端 `last_probe_unix_ms` 同一时间轴），**不用本机 Date.now()**。
 */
const clockItem = computed<ResourceItem | null>(() => {
  const thresholds = resolveClockThresholds(sysmon.config?.clock)
  const snapshotTs = sysmon.snapshot?.timestamp
  const probeAgeSec = sysmon.clock && typeof snapshotTs === 'number'
    ? clockProbeAgeSec(sysmon.clock.last_probe_unix_ms, snapshotTs)
    : null
  const probeAgeText = probeAgeSec === null ? null : formatAge(probeAgeSec)
  const badge = clockBadge(sysmon.clock, thresholds, sysmon.snapshot !== null, probeAgeText)
  if (badge === null) return null
  return {
    key: 'ntp',
    kind: 'ntp',
    label: CLOCK_BADGE_LABEL,
    value: badge.text,
    level: badge.level,
    title: badge.title,
    domId: 'resNtp',
  }
})

/**
 * 徽章标签取挂载点的末段（`/data` → `data`、`C:\\` → `C:`、根挂载点保留 `/`）：
 * 顶栏是横向窄条，完整路径会把后面的徽章挤出可视区；完整路径保留在 title 与浮层里。
 */
function shortMountLabel(mount: string): string {
  const trimmed = mount.replace(/[\\/]+$/, '')
  if (!trimmed) return mount  // 根挂载点（"/"、"C:\\" 归一后为空）保留原样
  const lastSeparator = Math.max(trimmed.lastIndexOf('/'), trimmed.lastIndexOf('\\'))
  if (lastSeparator < 0) return trimmed  // "C:" 之类无分隔符
  const segment = trimmed.slice(lastSeparator + 1)
  return segment || trimmed
}

/**
 * 各盘徽章（#75 / ADR 0048）：卷集合与顺序全部由快照驱动，不硬编码盘符或数量。
 * 顺序与监控页同源（store 的固定顺序：挂载深度升序，根挂载点恒在最前）。
 * 顶栏只呈现**状态色点 + 挂载点短标签**，完整挂载点 / 使用率 / 不可用原因都在 title 里
 * ——卷数量不定，数字会让每枚徽章宽一倍（ADR 0048）。
 * 单个卷 available=false 走中性色，title 带不可用原因 —— 不得用 0 冒充（契约 sysmon §2.4）。
 */
const diskItems = computed<ResourceItem[]>(() => {
  if (!sysmon.snapshot) return []
  return sysmon.volumes.map((volume) => {
    const base = {
      key: `disk:${volume.mount}`,
      kind: 'disk' as const,
      label: shortMountLabel(volume.mount),
      value: '',
      mount: volume.mount,
      testId: 'topbar-disk-badge',
      // 浮层有整行宽度，显示完整挂载点（徽章的短标签只是窄条妥协）
      overflowLabel: volume.mount,
    }
    if (!volume.available) {
      return {
        ...base,
        level: null,
        title: volume.reason
          ? `${volume.mount} 卷不可用: ${volume.reason}`
          : `${volume.mount} 卷不可用`,
        overflowValue: UNAVAILABLE_TEXT,
      }
    }
    const ratio = volume.used_ratio
    if (ratio === null) {
      // 可用但比率为 null（total=0 的特殊卷）：中性色 + 明确说明，不得显示 0%
      return {
        ...base,
        level: null,
        title: `${volume.mount} 卷使用率不可用`,
        overflowValue: UNAVAILABLE_TEXT,
      }
    }
    // 整数百分比只进 title 与浮层（顶栏徽章本身只给状态色点）
    const percent = Math.round(ratio * 100)
    return {
      ...base,
      level: resourceLevel(ratio),
      title: `${volume.mount} 使用率 ${percent}%`,
      // 浮层里显示百分比：徽章空间只够状态点，浮层是「点开看细节」的入口，必须补上读数
      overflowValue: `${percent}%`,
    }
  })
})

/**
 * ≥lg 的资源项全集（**展示顺序即优先级**，折叠总是从尾部开始）：
 * CPU → 内存 → 时钟 → 各卷（固定顺序）。服务徽章不在此列（ADR 0048：MD/TD/STG 永不折叠）。
 */
const resourceItems = computed<ResourceItem[]>(() => {
  const clock = clockItem.value
  return [
    cpuItem.value,
    memItem.value,
    ...(clock === null ? [] : [clock]),
    ...diskItems.value,
  ]
})

// ===== 按可用宽度自适应折叠（ADR 0048 决策 4；算法见 composables/topbarOverflow）=====

/** 资源组容器（测量对象；`clientWidth` 即可用宽度） */
const resourcesRef = ref<HTMLElement | null>(null)
/** 隐形测量行（渲染全部项、不占可见布局，只用来量每项宽度） */
const measureRowRef = ref<HTMLElement | null>(null)
/** 溢出浮层开关（组件内临时状态，不持久化） */
const overflowOpen = ref(false)
/** 溢出入口按钮（浮层定位基准） */
const overflowTriggerRef = ref<HTMLElement | null>(null)
/** 浮层本体（定位时要读它的实测宽度；它 Teleport 到 body，不能用资源组作参照） */
const overflowPanelRef = ref<HTMLElement | null>(null)
/** 浮层内联定位（`position: fixed`）：走 Teleport 到 body，避开 `.topbar__resource-run`
 *  的 `overflow: hidden`（在容器内绝对定位会被裁成看不到） */
const overflowPanelStyle = ref<Record<string, string>>({})
/** 资源组容器实测宽度（px）；0 = 尚未测量（jsdom / 首帧）→ 按「全部可见」处理 */
const availableWidth = ref(0)
/** 每项实测宽度（与 `resourceItems` 同序）；测量行未渲染时为空数组 */
const itemWidths = ref<number[]>([])
/** 项间距：初值取 layout.css 的 `.topbar__resources` gap（`--spacer-12` = 14px），实测后以计算值为准 */
const gapPx = ref(14)

/**
 * 溢出入口宽度预算：首帧入口还没渲染（量不到宽度），先用偏大估计值 34（`4 ▾` 实测约 26px），
 * 宁可早折一项也不让入口被 `overflow: hidden` 截掉（被截掉就等于丢事实）；
 * 入口出现后由 {@link remeasureOverflowButton} 换成实测宽度，估计值随即失效。
 */
const OVERFLOW_ESTIMATED_WIDTH = 34
const overflowButtonWidth = ref(OVERFLOW_ESTIMATED_WIDTH)

/** 能直接显示多少项（折叠数量的唯一判据，算法在 composables/topbarOverflow） */
const visibleCount = computed(() =>
  visibleItemCount(itemWidths.value, availableWidth.value, gapPx.value, overflowButtonWidth.value))

const visibleItems = computed<ResourceItem[]>(() => resourceItems.value.slice(0, visibleCount.value))
const foldedItems = computed<ResourceItem[]>(() => resourceItems.value.slice(visibleCount.value))

/**
 * 重新测量：容器宽度 + 每项宽度 → 重算可见数量。
 *
 * 用 `getBoundingClientRect().width` 而不是 `offsetWidth`：两者在某缩放 / transform 场景下
 * 不同，以「实际占位」为准。测量行是 `visibility: hidden` 的绝对定位行，不影响可见布局，
 * 也不会被 `overflow: hidden` 截断（被截断会让宽度变成 0，折叠数量必然算错）。
 */
function remeasure(): void {
  const container = resourcesRef.value
  const row = measureRowRef.value
  if (!container) return
  availableWidth.value = container.clientWidth
  if (!row) {
    itemWidths.value = []
    return
  }
  itemWidths.value = Array.from(row.children).map((child) =>
    (child as HTMLElement).getBoundingClientRect().width)
  // 间距从计算样式取，避免把 14 硬编码成 CSS 之外的第二份真相
  const style = window.getComputedStyle(row)
  const gap = Number.parseFloat(style.columnGap)
  if (Number.isFinite(gap) && gap >= 0) gapPx.value = gap
  remeasureOverflowButton()
}

/**
 * 实测溢出入口宽度（它只在折叠发生时存在）：读不到时保留上一次的值 / 估计值。
 * 只在「已经进入折叠态」时调用，避免与可见数量形成互相触发的循环——入口宽度变化不会改变
 * 「是否需要入口」的判断结果，只会让边界更准（多折/少折一项），实测一次即稳定。
 */
function remeasureOverflowButton(): void {
  const trigger = overflowTriggerRef.value
  if (!trigger) return
  const width = trigger.getBoundingClientRect().width
  if (Number.isFinite(width) && width > 0) overflowButtonWidth.value = width
}

let observer: ResizeObserver | null = null

onMounted(() => {
  if (typeof ResizeObserver === 'function' && resourcesRef.value) {
    observer = new ResizeObserver(() => remeasure())
    observer.observe(resourcesRef.value)
    if (measureRowRef.value) observer.observe(measureRowRef.value)
  }
  void nextTick(remeasure)
})

onUnmounted(() => {
  observer?.disconnect()
  observer = null
})

// 项集合 / 数值变化（快照推送、卷增减、时钟态切换）后重测：宽度是渲染结果，必须先重渲染再量
watch(
  () => resourceItems.value.map(item => `${item.key}:${item.value}`).join('|'),
  () => { void nextTick(remeasure) },
)

// 断点切换后可用宽度会变（窄屏隐藏整组 / 宽屏恢复）：重测一次，并把浮层与折叠一起复位
watch(isWide, () => {
  overflowOpen.value = false
  void nextTick(remeasure)
})

/**
 * 入口出现时实测它的宽度，替换首帧用的估计值。
 *
 * **不要在入口消失时把宽度重置回估计值**：那会让边界来回跳——实测 26 让预算变大 →
 * 少折一项 → 入口消失 → 重置成 34 → 预算变小 → 又折回去，1↔0 无限循环，且每轮都会
 * 卸载/挂载一次按钮。保留上一次实测值既能收敛，也比估计值更准；估计值只在从未折叠过
 * （因此也从未需要入口）时有意义。
 */
watch(
  () => foldedItems.value.length > 0,
  (hasOverflow) => {
    if (!hasOverflow) {
      // 只关浮层：浮层不该留在一个点不开的按钮下面
      overflowOpen.value = false
      return
    }
    void nextTick(remeasureOverflowButton)
  },
  { immediate: true },
)

/**
 * 浮层定位（与 `shared/DatePicker.vue` 同款：Teleport + fixed）。
 *
 * 为什么不用容器内绝对定位：`.topbar__resource-run` 必须 `overflow: hidden`（否则放不下的
 * 徽章会把顶栏撑破），容器内的绝对定位浮层会一并被裁掉。Teleport 到 body + `fixed` 既逃出
 * 裁剪，又不受顶栏 `z-index` 叠层影响。
 */
function updateOverflowPanelPosition(): void {
  const trigger = overflowTriggerRef.value
  if (!trigger) return
  const rect = trigger.getBoundingClientRect()
  // 浮层宽度以**实测**为准（浮层此刻已渲染）：`min-width` 归 CSS 所有，JS 不再持有一份
  // 宽度常量，否则两处一旦不同步就会出现「收进来了但仍溢出」或「白收一段」。
  const panelWidth = overflowPanelRef.value?.getBoundingClientRect().width ?? 0
  const viewportWidth = window.innerWidth
  // 右溢出：贴右边界收进来（浮层比按钮宽，默认左对齐会跑出屏幕）
  const maxLeft = viewportWidth - panelWidth - 8
  const left = panelWidth > 0 ? Math.min(rect.left, Math.max(8, maxLeft)) : rect.left
  overflowPanelStyle.value = {
    position: 'fixed',
    left: `${left}px`,
    top: `${rect.bottom + 4}px`,
  }
}

function toggleOverflow(): void {
  overflowOpen.value = !overflowOpen.value
  if (overflowOpen.value) {
    // 首帧浮层还没进 DOM（宽度读不到）→ 先按按钮左缘定位，渲染完再按实测宽度校正一次
    updateOverflowPanelPosition()
    void nextTick(updateOverflowPanelPosition)
  }
}

// 点击浮层之外 / 入口之外 / Escape 关闭（与 ThemeDropdown 同款交互）。
// 浮层 Teleport 到了 body，不在 `.topbar__resource-run` 的 DOM 子树里，因此要分别判两个锚点。
function handleOutsideClick(event: MouseEvent): void {
  if (!overflowOpen.value) return
  const target = event.target as HTMLElement
  if (target.closest('.topbar__overflow')) return
  if (target.closest('[data-test="topbar-overflow-trigger"]')) return
  overflowOpen.value = false
}

function handleKeydown(event: KeyboardEvent): void {
  if (event.key === 'Escape' && overflowOpen.value) overflowOpen.value = false
}

onMounted(() => {
  document.addEventListener('click', handleOutsideClick)
  document.addEventListener('keydown', handleKeydown)
})
onUnmounted(() => {
  document.removeEventListener('click', handleOutsideClick)
  document.removeEventListener('keydown', handleKeydown)
})

interface ResourceGauge {
  stateClass: string
  title: string
}

/**
 * 窄屏（<lg）单状态图标（#127 / ADR 0048）：复用 layout.css 已存在的折叠模式类
 * `topbar__gauge` / `topbar__resources--gauge`，不新增 CSS 样式、不新增 @media 或容器查询。
 * 状态色取 CPU / 内存 / 时钟 / 全部卷中的最差等级（阈值仍只由 useResourceLevel 与时钟配置阈值
 * 定义）；快照未到达时为中性等待态，不得用 normal 冒充「一切正常」。
 * 时钟**参与**取最差（ADR 0048）：偏差 danger 会让图标变红，时钟问题不会在窄屏被折叠掉。
 */
const resourceGauge = computed<ResourceGauge>(() => {
  if (!sysmon.snapshot) {
    return {
      stateClass: resourceLevelClass(null),
      title: '系统资源（等待 dzsysmon 快照）',
    }
  }
  const worst = worstResourceLevel(resourceItems.value.map(item => item.level))
  // title 汇总各资源读数：沿用各徽章自身的 title 文案，避免在窄屏另造一套格式化口径
  return {
    stateClass: resourceLevelClass(worst),
    title: resourceItems.value.map(item => item.title).join(' · '),
  }
})

const overflowTitle = computed(() =>
  `其余 ${foldedItems.value.length} 项: ${foldedItems.value.map(item => item.title).join('；')}`)

// 顶栏服务状态徽章（#199）：聚合 process_status 镜像，不再写死 MD / TD / STG 状态。
// 数据源：WS process_status 增量 + 建连 snapshot 全量 → stores/process.ts 的 statuses 镜像；
// 进程类别由 composables/processNaming.ts 唯一判定（日志页进程类型推导共用同一规则）。
// **服务徽章永不折叠**（ADR 0048）：它们是盯盘最关键的三个信号，且数量固定。
// 镜像未到达 / 该类别无进程 = 中性「未知」，不得默认绿。
const processStore = useProcessStore()

/** 服务徽章对应的进程类别（master / sysmon / WebUI 不属于 MD / TD / STG 服务） */
type ServiceCategory = Extract<ProcessCategory, 'md' | 'td' | 'strategy'>

interface ServiceDefinition {
  /** DOM 锚点 id（#199 要求保留，供测试 / E2E 断言） */
  id: 'resMd' | 'resTd' | 'resStg'
  label: string
  /** 进程类别（判定规则唯一实现见 composables/processNaming.ts） */
  category: ServiceCategory
  /** 中文服务名（GLOSSARY 术语：行情源 / 交易接口 / 策略） */
  name: string
}

const SERVICE_DEFINITIONS: readonly ServiceDefinition[] = [
  { id: 'resMd', label: 'MD', category: 'md', name: '行情源' },
  { id: 'resTd', label: 'TD', category: 'td', name: '交易接口' },
  { id: 'resStg', label: 'STG', category: 'strategy', name: '策略' },
]

interface ServiceBadge {
  id: ServiceDefinition['id']
  label: string
  level: ProcessLevel
  title: string
}

/**
 * 服务徽章的状态色 class 复用资源徽章的同一映射：`unknown` 不是 `ResourceLevel`，
 * 先转成 `null`（中性不可判读），走同一条 `resourceLevelClass` —— 未知态因此不套
 * status 色，也不命中 layout.css 的 warn / danger 闪烁规则（#199 AC「未知不闪烁」）。
 */
function serviceStateClass(level: ProcessLevel): string {
  return resourceLevelClass(level === 'unknown' ? null : level)
}

/** 单个服务类别 → 聚合等级 + title（多进程取最差；title 列出各进程状态，未知时说明原因） */
function aggregateService(def: ServiceDefinition): ServiceBadge {
  const statuses = Object.values(processStore.statuses)
  const matched = statuses.filter(status => processCategory(status.name) === def.category)
  // 多进程聚合取「最差等级」：与窄屏资源图标共用 worstResourceLevel 唯一实现；
  // unknown 不是进程状态等级，转 null 不参与取最差（全部不可判读时保持中性未知）
  const worst = worstResourceLevel(matched.map(status => {
    const level = processStateLevel(status.state)
    return level === 'unknown' ? null : level
  }))
  let title: string
  if (matched.length === 0) {
    title = statuses.length === 0
      ? `${def.name}状态未知（等待进程状态镜像）`
      : `${def.name}状态未知（无对应进程）`
  } else {
    title = `${def.name}：${matched.map(status => `${status.name} ${processStateText(status.state)}`).join('；')}`
  }
  return { id: def.id, label: def.label, level: worst ?? 'unknown', title }
}

const serviceBadges = computed<ServiceBadge[]>(() => SERVICE_DEFINITIONS.map(aggregateService))
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

      <!-- Resources：<lg 时追加既有折叠模式类，由 layout.css 隐藏整组资源徽章（#127） -->
      <div
        class="topbar__resources"
        id="topbarResources"
        :class="{ 'topbar__resources--gauge': !isWide }"
      >
        <!-- 服务状态徽章（#199 / ADR 0048）：**永不折叠**。状态由 process_status 镜像按进程类别聚合，
             不再写死。图标随状态切换（勾 / 警告三角 / 叉 / 中性横线），颜色由状态类给出；
             未知态走中性 unavailable 类 —— layout.css 的闪烁规则只匹配 warn / danger
             （仅真实告警闪烁），因此未知态不闪烁。 -->
        <span
          v-for="badge in serviceBadges"
          :key="badge.id"
          class="topbar__resource topbar__resource--svc"
          :class="serviceStateClass(badge.level)"
          :id="badge.id"
          :data-level="badge.level === 'unknown' ? undefined : badge.level"
          :title="badge.title"
        >
          <span class="res-label">{{ badge.label }}</span>
          <span class="res-icon">
            <svg v-if="badge.level === 'normal'" width="10" height="10" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="3" stroke-linecap="round" stroke-linejoin="round"><polyline points="20 6 9 17 4 12"/></svg>
            <svg v-else-if="badge.level === 'warn'" width="10" height="10" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"><path d="M10.29 3.86 1.82 18a2 2 0 0 0 1.71 3h16.94a2 2 0 0 0 1.71-3L13.71 3.86a2 2 0 0 0-3.42 0z"/><line x1="12" y1="9" x2="12" y2="13"/><line x1="12" y1="17" x2="12.01" y2="17"/></svg>
            <svg v-else-if="badge.level === 'danger'" width="10" height="10" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="10"/><line x1="15" y1="9" x2="9" y2="15"/><line x1="9" y1="9" x2="15" y2="15"/></svg>
            <svg v-else width="10" height="10" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="10"/><line x1="8" y1="12" x2="16" y2="12"/></svg>
          </span>
        </span>
        <span class="topbar__resource-divider"></span>

        <!-- 资源组（ADR 0048）：自有 `flex:1 + min-width:0` 的**行**，它的 clientWidth 就是
             资源项真正可用的宽度。为什么必须单独一行：服务徽章（MD / TD / STG）与分隔线也在
             `.topbar__resources` 里，直接拿容器 `clientWidth` 会把它们的宽度算成可用空间，
             折叠会晚一步触发、尾部徽章被 `overflow:hidden` 静默截断（实测 1440px 下
             8 项全显示、容器已溢出）。本行宽度与折叠结果无关（永远是「容器减服务簇」），
             因此不会出现「折叠 → 宽度变 → 再折叠」的抖动。 -->
        <span
          ref="resourcesRef"
          class="topbar__resource-run"
          data-test="topbar-resource-run"
        >
          <!-- 窄屏折叠模式（<lg）：整组 CPU / 内存 / 时钟 / 卷徽章由 layout.css 隐藏，只留一个状态图标。
               既有 CSS 没有 `.topbar__gauge` 的基础 display:none 规则，所以必须 v-if 只在窄屏渲染，
               否则宽屏会多出一个图标（#127；不新增 CSS）。内联 svg 而非 Icon 组件：状态色与闪烁
               规则按 `.topbar__gauge ... svg` 选择器书写，Icon 的 mask 图标不命中这些规则 -->
          <span
            v-if="!isWide"
            class="topbar__gauge"
            :class="resourceGauge.stateClass"
            data-test="topbar-resource-gauge"
            :title="resourceGauge.title"
          >
            <svg width="12" height="12" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round"><rect x="2" y="2.75" width="12" height="8.5" rx="1"/><path d="M5.5 14h5M8 11.25V14"/></svg>
          </span>

          <!-- 隐形测量行（ADR 0048）：渲染**全部**资源项，`visibility: hidden` + 绝对定位，
               只用来量每项真实宽度（折叠数量的唯一输入）。不进可访问性树、不参与可见布局 -->
          <span
            ref="measureRowRef"
            class="topbar__resource-measure"
            aria-hidden="true"
            data-test="topbar-resource-measure"
          >
            <TopbarResourceBadge
              v-for="item in resourceItems"
              :key="item.key"
              :kind="item.kind"
              :label="item.label"
              :value="item.value"
              :level="item.level"
              :title="item.title"
            />
          </span>

          <!-- 可见资源组（≥lg）：CPU / 内存 / 时钟 / 各卷，按可用宽度自适应折叠（ADR 0048） -->
          <TopbarResourceBadge
            v-for="item in visibleItems"
            :key="item.key"
            :kind="item.kind"
            :label="item.label"
            :value="item.value"
            :level="item.level"
            :title="item.title"
            :dom-id="item.domId"
            :mount="item.mount"
            :test-id="item.testId"
          />

          <!-- 溢出入口（▾ N）：放不下时出现，点击弹浮层列出被收起的项（再点收起）。
               用原生 button 保证键盘 Enter/Space 与 aria-expanded 的语义；
               `data-mount` / `data-level` 刻意不写（它代表整组而非某一项，会污染按卷比较的映射） -->
          <button
            v-if="isWide && foldedItems.length > 0"
            ref="overflowTriggerRef"
            type="button"
            class="topbar__resource topbar__overflow-trigger ds-link-btn"
            data-test="topbar-overflow-trigger"
            :aria-expanded="overflowOpen"
            :aria-label="overflowTitle"
            :title="overflowTitle"
            @click="toggleOverflow"
          >
            <span class="topbar__overflow-count">{{ foldedItems.length }}</span>
            <Icon name="Down" :size="10" />
          </button>

          <!-- 溢出浮层：复用 .theme-dropdown__menu 的视觉 token（--bg-menu / 阴影 / radius / z-index），
               逐条列出被收起项；带状态色点与精确读数（徽章本身只给状态色点）。
               Teleport 到 body + fixed 定位：资源组行必须 overflow:hidden，容器内绝对定位会被裁掉 -->
          <Teleport to="body">
            <div
              v-if="isWide && overflowOpen && foldedItems.length > 0"
              ref="overflowPanelRef"
              class="topbar__overflow"
              :style="overflowPanelStyle"
              data-test="topbar-overflow-panel"
            >
              <span
                v-for="item in foldedItems"
                :key="item.key"
                class="topbar__overflow-item"
                :data-level="item.level ?? undefined"
                :title="item.title"
              >
                <span
                  class="topbar__overflow-dot"
                  :class="resourceLevelClass(item.level)"
                  aria-hidden="true"
                ></span>
                <span class="topbar__overflow-label">{{ item.overflowLabel ?? item.label }}</span>
                <span class="topbar__overflow-value">{{ item.overflowValue ?? item.value }}</span>
              </span>
            </div>
          </Teleport>
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

/* 「▾ N」是原生 button：浏览器 UA 不把页面字体继承给 button（.ds-link-btn 只重置
   background/border/color/font-size），不补这一行会与相邻 span 徽章字体不一致 */
.topbar__resource.ds-link-btn {
  font-family: inherit;
}
</style>
