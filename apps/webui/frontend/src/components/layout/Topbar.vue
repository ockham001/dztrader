<script setup lang="ts">
import { computed, onMounted } from 'vue'
import Icon from '@/components/shared/Icon.vue'
import ThemeDropdown from './ThemeDropdown.vue'
import TopbarMetricBadge from './TopbarMetricBadge.vue'
import { UNAVAILABLE_TEXT, formatAge } from '@/composables/format'
import { processCategory, type ProcessCategory } from '@/composables/processNaming'
import { clockProbeAgeSec, resolveClockThresholds } from '@/composables/sysmonClock'
import { ntpBadgeModel, type NtpBadgeModel } from '@/composables/topbarNtp'
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

// 顶栏资源区（#452 / ADR 0054）：三枚「图标 + 数值」常驻徽章 —— CPU → 内存 → 时钟偏差。
// 数据源：契约 sysmon 实时快照（固定实例 dzsysmon），首次挂载经 REST 补拉镜像，之后只由 WS
// 推送刷新 —— 刷新基线由发送方 realtime.push_interval_sec（默认 5s）决定，前端不轮询、不改 WS 契约。
//
// 为什么全部用图标 + 数值（没有文字标签、没有卷徽章、没有墙钟/日期时间、没有折叠）：
// - 32px 状态条只够扫读：图标 + 数值比「NTP」「MEM」文字标签省宽，且语言中立；
// - 不给 `NTP` 一个文字标签：它是协议名而不是用户概念，这里回答的是「本机时钟偏了多少」；
// - 不显示当前时间：顶栏不需要日期时间，也就不需要每秒定时器（秒级重渲染的成本不值）；
// - 任何宽度都常驻：宽度紧时只由 layout.css 的断点规则收紧间距，组件不读视口宽度
//   （无 JS 行为分支、不用容器查询、没有 overflow 浮层）。
const sysmon = useSysmonStore()
onMounted(() => { void sysmon.init() })

/** 三枚资源徽章的图标名：与 `public/icons/builtin/<name>.svg` **精确同名**（大小写敏感）。
 * 集中成一张表而不是写死在模板里：`iconNames.spec` 的动态来源提取器按这张表机器校验图标
 * 文件存在（`TopbarMetricBadge` 内部的图标名是动态用法，必须可静态求值）。 */
const RESOURCE_ICONS: Record<'cpu' | 'mem' | 'ntp', string> = {
  cpu: 'CPU',
  mem: 'memory-module',
  ntp: 'clock_time',
}

/** 单枚指标徽章的展示模型（CPU / 内存同构：读数 + 状态等级 + 悬停说明 + 读屏名） */
interface ResourceBadgeModel {
  value: string
  level: ResourceLevel | null
  title: string
  ariaLabel: string
}

const cpuItem = computed<ResourceBadgeModel>(() => {
  const entry = sysmon.availabilityOf('cpu.total')
  if (!sysmon.snapshot) {
    // 快照尚未到达：显示等待态，不得用假数字占位
    return {
      value: '—',
      level: null,
      title: 'CPU 使用率（等待 dzsysmon 快照）',
      ariaLabel: 'CPU 使用率等待快照',
    }
  }
  const ratio = sysmon.cpuUsageRatio
  if (!entry || !entry.available || ratio === null) {
    // 不可用/首次采样：按契约显示不可用，不得用 0 冒充（契约 sysmon §2.7）
    return {
      value: UNAVAILABLE_TEXT,
      level: null,
      title: entry?.reason ? `CPU 使用率不可用: ${entry.reason}` : 'CPU 使用率不可用',
      ariaLabel: 'CPU 使用率不可用',
    }
  }
  // 顶栏徽章是横向窄条，刻意用整数百分比（监控页用共享模块的 1 位小数）：同一数值在两处
  // 精度不同是**有意的取舍**，不是格式化漂移。定档仍与监控页共用 resourceLevel，但顶栏只在
  // warn / danger 着色（normal 与服务徽章文字同色，#452 用户拍板「颜色统一」）。
  const percent = Math.round(ratio * 100)
  // 状态色与监控页共用 resourceLevel（含边界），不在此处各自比较阈值
  return {
    value: `${percent}%`,
    level: resourceLevel(ratio),
    title: `CPU 使用率 ${percent}%`,
    ariaLabel: `CPU 使用率 ${percent}%`,
  }
})

// 内存徽章与 CPU 徽章同构：等待 / 不可用 / 百分比 + 阈值状态色；比率取自 store 净化后的
// memoryInfo（可用性由 availability.metrics.memory 承载），0 不是合法占位（契约 sysmon §2.4）。
const memItem = computed<ResourceBadgeModel>(() => {
  const entry = sysmon.availabilityOf('memory')
  if (!sysmon.snapshot) {
    return {
      value: '—',
      level: null,
      title: '内存使用率（等待 dzsysmon 快照）',
      ariaLabel: '内存使用率等待快照',
    }
  }
  const ratio = sysmon.memoryInfo?.used_ratio ?? null
  if (!entry || !entry.available || ratio === null) {
    return {
      value: UNAVAILABLE_TEXT,
      level: null,
      title: entry?.reason ? `内存使用率不可用: ${entry.reason}` : '内存使用率不可用',
      ariaLabel: '内存使用率不可用',
    }
  }
  // 整数百分比：与 CPU 徽章同一取舍（窄条优先，见上）
  const percent = Math.round(ratio * 100)
  return {
    value: `${percent}%`,
    level: resourceLevel(ratio),
    title: `内存使用率 ${percent}%`,
    ariaLabel: `内存使用率 ${percent}%`,
  }
})

/**
 * 时钟偏差徽章（契约 sysmon §2.9「顶栏徽章」条）：展示口径全部在
 * `composables/topbarNtp.ntpBadgeModel`（纯函数、可单测）——四态、阈值定档、数值文本、
 * title / aria 文案都在那里；`null` 表示该域被配置关闭 → **整枚徽章不渲染**。
 *
 * 上次探测的相对时间与监控页时钟块同口径：基准取快照 `timestamp`（与服务端
 * `last_probe_unix_ms` 同一时间轴），**不用本机墙钟**——两台机器时钟不一致时相对时间会失真。
 */
const ntpItem = computed<NtpBadgeModel | null>(() => {
  const snapshotTs = sysmon.snapshot?.timestamp
  const probeAgeSec = sysmon.clock && typeof snapshotTs === 'number'
    ? clockProbeAgeSec(sysmon.clock.last_probe_unix_ms, snapshotTs)
    : null
  return ntpBadgeModel(
    sysmon.clock,
    resolveClockThresholds(sysmon.config?.clock),
    sysmon.snapshot !== null,
    probeAgeSec === null ? null : formatAge(probeAgeSec),
  )
})

// 顶栏服务状态徽章（#199）：聚合 process_status 镜像，不再写死 MD / TD / STG 状态。
// 数据源：WS process_status 增量 + 建连 snapshot 全量 → stores/process.ts 的 statuses 镜像；
// 进程类别由 composables/processNaming.ts 唯一判定（日志页进程类型推导共用同一规则）。
// **服务徽章永不折叠**（ADR 0054 继承 ADR 0048 的这条约束）：它们是盯盘最关键的三个信号，
// 且数量固定。镜像未到达 / 该类别无进程 = 中性「未知」，不得默认绿。
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
  // 多进程聚合取「最差等级」：与 worstResourceLevel 唯一实现同源；
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
  <header class="topbar" aria-label="全局状态栏">
    <!-- #380：壳层顶栏是全站唯一的 banner 地标，必须是有名字的那一个：
         读屏的「跳到 banner」若不命名，只会报一个无名的 banner，用户不知道落在哪里。
         `aria-label` 而非 `aria-labelledby`：顶栏里没有能当标题的元素（全是状态徽章与资源指标徽章）。
         注：注释必须写在根元素**内部** —— 写在 <template> 顶层会让组件变成多根（注释也占一个
         根节点），$el 退化成包裹用的 div，app-shell 里那层 header 语义就丢了。 -->
    <div class="topbar__left">
      <button type="button"
        class="topbar__hamburger"
        aria-label="打开导航菜单"
        @click="emit('toggleMobile')"
      >
        <Icon name="menu" :size="20" />
      </button>

      <!-- 资源区（#452 / ADR 0054）：服务徽章 + 分隔线 + 三枚「图标 + 数值」常驻徽章
           （CPU → 内存 → 时钟偏差）。任何宽度都不折叠、不收起：宽度紧时只由 layout.css 的
           断点规则收紧间距，组件不读视口宽度（无 JS 行为分支、不用容器查询）。 -->
      <!-- 图标名与 `public/icons/builtin/<name>.svg` 精确同名（iconNames.spec 校验）：
           CPU / memory-module / clock_time 都是既有资源图标；时钟偏差用最简表盘（用户 2026-11-23 拍板「就一个时钟」），
           表达「时钟同步/偏差」而不是当前时间）。 -->
      <div class="topbar__resources" id="topbarResources">
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

        <TopbarMetricBadge
          kind="cpu"
          :icon="RESOURCE_ICONS.cpu"
          :value="cpuItem.value"
          :level="cpuItem.level"
          :title="cpuItem.title"
          :accessible-name="cpuItem.ariaLabel"
          dom-id="resCpu"
        />
        <TopbarMetricBadge
          kind="mem"
          :icon="RESOURCE_ICONS.mem"
          :value="memItem.value"
          :level="memItem.level"
          :title="memItem.title"
          :accessible-name="memItem.ariaLabel"
          dom-id="resMem"
        />
        <!-- 配置关闭（clock = null）时整枚不渲染（不放 0 或空壳） -->
        <TopbarMetricBadge
          v-if="ntpItem"
          kind="ntp"
          :icon="RESOURCE_ICONS.ntp"
          :value="ntpItem.value"
          :level="ntpItem.level"
          :title="ntpItem.title"
          :accessible-name="ntpItem.ariaLabel"
          dom-id="resNtp"
        />
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
</style>
