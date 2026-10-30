<script setup lang="ts">
import { computed, onMounted, ref, watch } from 'vue'
import Icon from '@/components/shared/Icon.vue'
import ThemeDropdown from './ThemeDropdown.vue'
import { UNAVAILABLE_TEXT } from '@/composables/format'
import { processCategory, type ProcessCategory } from '@/composables/processNaming'
import { useBreakpoint } from '@/composables/useBreakpoint'
import { processStateLevel, processStateText, type ProcessLevel } from '@/composables/useProcessState'
import { resourceLevel, worstResourceLevel, type ResourceLevel } from '@/composables/useResourceLevel'
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

// 顶栏资源徽章（CPU / 内存 / 各卷）：契约 sysmon 实时快照（固定实例 dzsysmon）。
// 首次挂载经 REST 补拉镜像，之后只由 WS 推送刷新 —— 刷新基线由发送方
// realtime.push_interval_sec（默认 5s）决定，前端不轮询、不改 WS 契约。
const sysmon = useSysmonStore()
onMounted(() => { void sysmon.init() })

// 顶栏密度（#127）：断点只做行为分支 —— ≥lg 保持完整徽章组并把卷徽章收敛为「前 2 + 其余 N 个」，
// <lg 复用 layout.css 既有的折叠模式类把整组徽章收成单个状态图标。档位唯一来源是 useBreakpoint
// （与 CSS 断点白名单同源）；layout.css 的 gauge 折叠规则本身不含宽度 @media，因此窄屏判定
// 只能走 JS 行为分支——本组件不新增 @media，也不用容器查询。
const { tier } = useBreakpoint()
const isWide = computed(() => tier.value === 'lg' || tier.value === 'xl')

interface ResourceBadge {
  text: string
  /** 使用率展示等级（与监控页共用 resourceLevel 的同一结论）；等待 / 不可用为 null，不套色标 */
  level: ResourceLevel | null
  title: string
}

/** 徽章状态色 class 由 level 派生（等待 / 不可用走中性色）：单一表示，避免 class 与 level 两份状态漂移（#125） */
function resourceStateClass(level: ResourceLevel | null): string {
  return level === null ? 'topbar__resource--unavailable' : `topbar__resource--${level}`
}

const cpuBadge = computed<ResourceBadge>(() => {
  const entry = sysmon.availabilityOf('cpu.total')
  if (!sysmon.snapshot) {
    // 快照尚未到达：显示等待态，不得用假数字占位
    return {
      text: '—',
      level: null,
      title: 'CPU 使用率（等待 dzsysmon 快照）',
    }
  }
  const ratio = sysmon.cpuUsageRatio
  if (!entry || !entry.available || ratio === null) {
    // 不可用/首次采样：按契约显示不可用，不得用 0 冒充（契约 sysmon §2.7）
    return {
      text: UNAVAILABLE_TEXT,
      level: null,
      title: entry?.reason ? `CPU 使用率不可用: ${entry.reason}` : 'CPU 使用率不可用',
    }
  }
  // 顶栏徽章是横向窄条，刻意用整数百分比（监控页用共享模块的 1 位小数）：同一数值在两处
  // 精度不同是**有意的取舍**，不是格式化漂移——顶栏要在常驻条里塞下 CPU / 内存 / 多个卷徽章，
  // 小数位会挤掉后面的徽章。状态色仍与监控页共用 resourceLevel，结论不会不一致（#126）。
  const percent = Math.round(ratio * 100)
  // 状态色与监控页卷列表共用 resourceLevel（含边界），不在此处各自比较阈值
  const level = resourceLevel(ratio)
  return { text: `${percent}%`, level, title: `CPU 使用率 ${percent}%` }
})

// 内存徽章与 CPU 徽章同构：等待 / 不可用 / 百分比 + 阈值状态色；比率取自 store 净化后的
// memoryInfo（可用性由 availability.metrics.memory 承载），0 不是合法占位（契约 sysmon §2.4）。
const memBadge = computed<ResourceBadge>(() => {
  const entry = sysmon.availabilityOf('memory')
  if (!sysmon.snapshot) {
    return {
      text: '—',
      level: null,
      title: '内存使用率（等待 dzsysmon 快照）',
    }
  }
  const ratio = sysmon.memoryInfo?.used_ratio ?? null
  if (!entry || !entry.available || ratio === null) {
    return {
      text: UNAVAILABLE_TEXT,
      level: null,
      title: entry?.reason ? `内存使用率不可用: ${entry.reason}` : '内存使用率不可用',
    }
  }
  // 整数百分比：与 CPU 徽章同一取舍（窄条优先，见上）
  const percent = Math.round(ratio * 100)
  const level = resourceLevel(ratio)
  return { text: `${percent}%`, level, title: `内存使用率 ${percent}%` }
})

/**
 * 各盘徽章（#75）：卷集合与顺序全部由快照驱动，不硬编码盘符或数量。
 * 顺序与监控页同源（store 按使用率降序，`#122`）。
 * 单个卷 available=false 显示「不可用」，标题带原因 —— 不得用 0 冒充（契约 sysmon §2.4）。
 * 无容量挂载的折叠是监控页正文列表的显示行为（`#122`）；顶栏这里不省略「存在但读不到」的
 * 事实，但宽屏只直接展示前 2 个（`#127`），其余卷的信息仍完整保留在「其余 N 个」徽章的
 * title 里（不可用卷排在末尾，通常落进「其余」并如实带原因）。
 */
interface DiskBadge extends ResourceBadge {
  mount: string
  label: string
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
        text: UNAVAILABLE_TEXT,
        level: null,
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
        text: UNAVAILABLE_TEXT,
        level: null,
        title: `${volume.mount} 卷使用率不可用`,
      }
    }
    // 整数百分比：与 CPU / 内存徽章同一取舍（窄条优先，见 CPU 徽章注释）
    const percent = Math.round(ratio * 100)
    const level = resourceLevel(ratio)
    return {
      mount: volume.mount,
      label: shortMountLabel(volume.mount),
      text: `${percent}%`,
      level,
      title: `${volume.mount} 使用率 ${percent}%`,
    }
  })
})

/**
 * 宽屏卷徽章上限（#127 顶栏密度 AC：≥lg 只显示一部分卷 + 「其余 N 个」）。
 * 本票取前 2 个：常驻窄条里 CPU / 内存已占去宽度，2 个卷徽章 + 「其余」是可读上限。
 *
 * 口径说明：AC 原文「显示承载关键数据的卷」是 #124 删除「关注路径」概念之前的遗留措辞
 * （ADR 0034 Decision 3：卷容量监控范围改为本机全部卷，「哪个卷承载关键数据」这一信息已不存在）。
 * #124 之后可用的等价口径是「最需要关注」= store 已按使用率降序排好的前 2 个卷
 * （排序见 #122；不可用 / 总量为 0 的卷排在末尾，通常落入「其余」）。
 */
const WIDE_DISK_BADGE_LIMIT = 2

// 展开状态只是组件内临时状态：刷新回默认、不持久化（#127 AC）
const diskExpanded = ref(false)

const visibleDiskBadges = computed<DiskBadge[]>(() => {
  // 窄屏整组卷徽章由 layout.css 的 topbar__resources--gauge 规则隐藏，此处不必再切分
  if (!isWide.value || diskExpanded.value) return diskBadges.value
  return diskBadges.value.slice(0, WIDE_DISK_BADGE_LIMIT)
})

/** 被折叠进「其余 N 个」的卷（沿用 store 顺序，不重新排序） */
const foldedDiskBadges = computed<DiskBadge[]>(() => diskBadges.value.slice(WIDE_DISK_BADGE_LIMIT))

/** 「其余 N 个」徽章 title：列出被折叠卷的挂载点 + 使用率 / 不可用原因 */
const diskRestTitle = computed(() => {
  if (diskExpanded.value) return '收起其余卷'
  return `其余 ${foldedDiskBadges.value.length} 个卷: ${foldedDiskBadges.value.map(badge => badge.title).join('；')}`
})

function toggleDiskExpanded(): void {
  diskExpanded.value = !diskExpanded.value
}

// 展开态只属于当次宽屏布局：跨断点（含回到宽屏）回到「前 2 + 其余 N 个」的默认密度（#127 AC「不持久化」）
watch(isWide, () => { diskExpanded.value = false })

interface ResourceGauge {
  stateClass: string
  title: string
}

/**
 * 窄屏（<lg）单状态图标（#127）：复用 layout.css 已存在但此前从未被引用的折叠模式类
 * `topbar__gauge` / `topbar__resources--gauge`，不新增 CSS 样式、不新增 @media 或容器查询。
 * 状态色取 CPU / 内存 / 全部卷中的最差等级（阈值仍只由 useResourceLevel 定义）；
 * 快照未到达时为中性等待态，不得用 normal 冒充「一切正常」。
 */
const resourceGauge = computed<ResourceGauge>(() => {
  if (!sysmon.snapshot) {
    return {
      stateClass: resourceStateClass(null),
      title: '系统资源（等待 dzsysmon 快照）',
    }
  }
  const readings: Pick<ResourceBadge, 'level' | 'title'>[] = [cpuBadge.value, memBadge.value, ...diskBadges.value]
  // 取最差等级的唯一实现（useResourceLevel.worstResourceLevel）；全部不可判读 → null 走中性等待态
  const worst = worstResourceLevel(readings.map(reading => reading.level))
  // title 汇总各资源读数：沿用各徽章自身的 title 文案，避免在窄屏另造一套格式化口径
  return {
    stateClass: resourceStateClass(worst),
    title: readings.map(reading => reading.title).join(' · '),
  }
})

// 顶栏服务状态徽章（#199）：聚合 process_status 镜像，不再写死 MD / TD / STG 状态。
// 数据源：WS process_status 增量 + 建连 snapshot 全量 → stores/process.ts 的 statuses 镜像；
// 进程类别由 composables/processNaming.ts 唯一判定（日志页进程类型推导共用同一规则）。
// 聚合逻辑与资源徽章（#127）同层留在组件内：两类徽章都只服务顶栏、无第二个消费者，
// 下沉 composable 只会多一层转手（需要时再抽，不在本票提前抽象）。
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
 * 状态色 class 复用资源徽章的同一映射（`resourceStateClass`，#125）：未知态走中性
 * unavailable 类，不套 status 色，也不命中 layout.css 的 warn / danger 闪烁规则
 * （#199 AC「未知不闪烁」）。
 */
function serviceStateClass(level: ProcessLevel): string {
  return resourceStateClass(level === 'unknown' ? null : level)
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

      <!-- Resources：<lg 时追加既有折叠模式类，由 layout.css 隐藏整组徽章（#127） -->
      <div
        class="topbar__resources"
        id="topbarResources"
        :class="{ 'topbar__resources--gauge': !isWide }"
      >
        <!-- 服务状态徽章（#199）：状态由 process_status 镜像按进程类别聚合，不再写死。
             图标随状态切换（勾 / 警告三角 / 叉 / 中性横线），颜色由状态类给出；
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
        <!-- 窄屏折叠模式（<lg）：整组 CPU / 内存 / 卷徽章由 layout.css 隐藏，只留一个状态图标。
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
        <!-- 资源徽章：CPU（#71）/ 内存（#74）/ 各卷（#75）均已接入实时快照；
             NTP 与写死的磁盘假数字已删除 -->
        <span
          class="topbar__resource topbar__resource--cpu"
          :class="resourceStateClass(cpuBadge.level)"
          id="resCpu"
          :title="cpuBadge.title"
        >
          <span class="res-label">CPU</span>
          <span class="res-value">{{ cpuBadge.text }}</span>
        </span>
        <span
          class="topbar__resource topbar__resource--mem"
          :class="resourceStateClass(memBadge.level)"
          id="resMem"
          :title="memBadge.title"
        >
          <span class="res-label">MEM</span>
          <span class="res-value">{{ memBadge.text }}</span>
        </span>
        <!-- 各盘徽章：卷集合 / 顺序完全由快照驱动，不硬编码盘符或数量；
             data-level 与监控页卷列表同源（共享 resourceLevel），供跨组件同一结论断言。
             宽屏最多直接显示前 2 个，其余折叠进下方「其余 N 个」（#127） -->
        <span
          v-for="badge in visibleDiskBadges"
          :key="badge.mount"
          class="topbar__resource topbar__resource--disk"
          :class="resourceStateClass(badge.level)"
          :data-mount="badge.mount"
          :data-level="badge.level"
          data-test="topbar-disk-badge"
          :title="badge.title"
        >
          <span class="res-label">{{ badge.label }}</span>
          <span class="res-value">{{ badge.text }}</span>
        </span>
        <!-- 「其余 N 个」：宽屏且卷超过上限时出现，点击展开全部卷、再点收起；
             展开态仅为组件内临时状态（不持久化，#127）。它代表整组卷而非某一卷，因此不写
             data-mount / data-level，也不带 --disk 类（该组卷徽章只在窄屏被 CSS 隐藏，
             这里 v-if 已保证窄屏不渲染）。用原生 button + 既有 .ds-link-btn 做文字按钮
             reset / cursor / hover；只有一个 scoped 样式补回按钮的字体继承（浏览器 UA 不为
             button 继承页面字体），不新增布局/密度样式。键盘 Enter/Space 由 button 原生处理 -->
        <button
          v-if="isWide && foldedDiskBadges.length > 0"
          type="button"
          class="topbar__resource ds-link-btn"
          data-test="topbar-disk-rest"
          :aria-expanded="diskExpanded"
          :title="diskRestTitle"
          @click="toggleDiskExpanded"
        >
          <span class="res-value">{{ diskExpanded ? '收起' : `其余 ${foldedDiskBadges.length} 个` }}</span>
        </button>
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

/* 指标不可用 / 快照未到达 / 服务状态未知：中性色，不得用状态色或 0 冒充数据；
   服务徽章只有 res-label / res-icon，中性图标颜色也由本规则给出（#199） */
.topbar__resource--unavailable .res-value,
.topbar__resource--unavailable .res-icon {
  color: var(--text-tertiary);
}

/* 「其余 N 个」是原生 button：浏览器 UA 不把页面字体继承给 button（.ds-link-btn 只重置
   background/border/color/font-size），不补这一行会与相邻 span 徽章字体不一致 */
.topbar__resource.ds-link-btn {
  font-family: inherit;
}
</style>
