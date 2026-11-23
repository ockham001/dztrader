<script setup lang="ts">
import { computed, getCurrentInstance, onMounted, onUnmounted, ref, watch } from 'vue'
import { onBeforeRouteLeave, type NavigationGuardNext, type Router } from 'vue-router'
import Icon from '@/components/shared/Icon.vue'
import Modal from '@/components/shared/Modal.vue'
import SyncSwitch from '@/components/shared/SyncSwitch.vue'
import SysmonConfigTab from '@/components/sysmon/SysmonConfigTab.vue'
import SysmonHistoryTab from '@/components/sysmon/SysmonHistoryTab.vue'
import SysmonSparkline from '@/components/sysmon/SysmonSparkline.vue'
import { PENDING_TIMEOUT } from '@/composables/usePending'
import {
  NO_THRESHOLD_TEXT,
  buildMemoryComposition,
  buildMonitorLedger,
  unavailableReasonText,
  type LedgerGroup,
  type LedgerRow,
  type MemoryCompositionSegment,
} from '@/composables/monitorLedger'
import {
  PERCENT_TREND_GRIDLINE_YS,
  coreBarColumns,
  percentTrendGeometry,
} from '@/composables/monitorCpu'
import {
  RESOURCE_DANGER_RATIO,
  RESOURCE_WARN_RATIO,
  resourceLevel,
} from '@/composables/useResourceLevel'
import type { ResourceLevel } from '@/composables/useResourceLevel'
import {
  UNAVAILABLE_TEXT,
  formatAge,
  formatBytes,
  formatClockTime,
  formatDateTime,
  formatDecimal,
  formatFrequency,
  formatFrequencyParts,
  formatMilliseconds,
  formatPercent,
  formatPlatformName,
  formatTemperature,
  formatUptime,
} from '@/composables/format'
import {
  groupTemperatures,
  normalizeTemperatureReadings,
} from '@/composables/sysmonTemperature'
import {
  type SparklinePoint,
  sparklineAriaLabel,
  sparklineTitle,
  sparklineWindowComplete,
  sparklineWindowText,
} from '@/composables/sysmonSparkline'
import { semanticHintText } from '@/composables/sysmonSemanticHint'
import {
  CLOCK_DAEMON_CONFLICT_TEXT,
  CLOCK_DISABLED_DESC,
  CLOCK_DISABLED_TITLE,
  CLOCK_NO_DAEMON_DESC,
  CLOCK_NO_DAEMON_TEXT,
  CLOCK_QUALITY_HINT,
  CLOCK_UNPROBED_DESC,
  CLOCK_UNPROBED_TITLE,
  clockBoundAlarm,
  clockDomainState,
  clockFailureSummary,
  clockProbeAgeSec,
  clockServerCells,
  resolveClockThresholds,
  sortClockDaemons,
} from '@/composables/sysmonClock'
import {
  isSnapshotStale,
  receivedAgeMs,
  resolvePushIntervalSec,
  staleSnapshotText,
} from '@/composables/sysmonFreshness'
import { useAuthStore } from '@/stores/auth'
import { useSysmonHistoryStore } from '@/stores/sysmonHistory'
import { useSysmonStore } from '@/stores/sysmon'
import { trendMetricLabel, useSysmonTrendStore } from '@/stores/sysmonTrend'
import { useToastStore } from '@/stores/toast'

// 资源监控页：主机条（#430：主机 / 系统 / 内核 / 已运行 + 右端数据龄状态槽；CPU 型号 · 逻辑核数
// 已移出，归 CPU 块）、CPU 块（#435：标题 + 右上型号 / 四数字 / 固定 0–100% 单曲线 / 自适应每核
// 条形）、指标台账（#363；三组：内存 / 存储 / 网络与时钟，后续块按参考布局逐块替换）。
//
// 台账是**纯渲染层**：三组（内存 / 存储 / 网络与时钟）× 七列（状态点 · 指标名 + 契约键 ·
// 量程轨道 · 当前值 · 明细 · 阈值 · 余量）的行数据全部来自 `@/composables/monitorLedger` 的
// `buildMonitorLedger`——本组件不判断「该不该显示这一行」「空态是哪一类」「阈值是多少」。
// 行的四类空态（不可用 / 暂无数据 / 已在配置中关闭 / 无此维度）与阈值、余量都在纯函数层决定，
// 组件只把 `state` / `thresholdText` / `headroomText` 原样铺进对应的格子。
//
// 两个口径不得合并（本页最容易做错的地方）：
// - 内存主数值的比率是 `used / total`，其中 `used = total − available`（ADR 0024）；
// - 内存组成带的「已用」段是 `total − free − cached − buffers` 的总量分解，`free` 不是快照字段
//   必须派生（`buildMemoryComposition`）；缓存或缓冲为 null 时组成带只画可得的两段并标注，不补 0。
//
// 轨道缺口只在**有契约阈值**的行上出现，位置取 `useResourceLevel` 的两个展示分档常量（70 / 90）：
// 那是显示规则（ADR 0034），与「阈值」列的契约值（`memory_used_pct` 等）是两个不同的东西。
// 磁盘 `disk_io.utilization` 有填充但无缺口——它没有契约阈值，且它是设备忙时间占比，
// 中文一律叫「忙碌占比」不叫「利用率」。
// 网络：可用但数组为空 = 发送方按 hide_virtual 滤掉了全部虚拟网卡（契约 §2.5 明文，
// **不是**不可用），台账里的空态行给出「已隐藏全部虚拟网卡」；开关走 REST 下发、
// 以 WS sysmon_config 回推为生效信号，位置随「网络与时钟」组走。
// 历史视图（#79）为独立视图：切换只影响渲染来源，不卸载/不重建实时 store，
// 因此 WS 连接与实时快照推送完全不受影响（契约 sysmon §8：历史视图不订阅 WS）。
// 配置视图（#81）：展示生效配置镜像（WS sysmon_config，首次挂载经 GET /api/sysmon/config 补拉）；
// admin 可编辑并只下发与原镜像的差异，非 admin 只读（编辑仅 admin，与系统设置页一致）。
// 内联趋势（#127）：内存 / 交换 / 共享内存目录等标量指标旁有最近 15 分钟 sparkline（历史 REST
// 补齐 + WS 增量推进，末点即旁边的当前值）；历史不可用时静默不渲染。CPU 使用率是 CPU 块内的固定
// 0–100% 宽图（#435），负载从 #435 起不再有迷你趋势（用户拍板：只保留数字与 5/15 分小字）。
// 时间窗按实际覆盖标注（#142）：只有覆盖满 15 分钟才写「最近 15 分钟」，历史范围内无桶 /
// 被 max_points 截断时标注真实覆盖时长，截断与历史视图同口径；补拉降级保留上一轮曲线并标未刷新。
// 点击 sparkline 或数值切到历史视图并预选该指标（契约 sysmon §6.1，不新增接口）。
// 明细展开（#363）：每个台账行可展开完整明细（卷 / 盘 / 网卡 / 时钟各一套），折叠状态不持久化。
// CPU 块（#435）：主图只画 CPU 总使用率**一条**曲线，纵轴固定 0–100%（不再随序列自适应，#361 的
// 自适应纵轴对 CPU 图作废）；每核使用率直接排在曲线下方（按容器宽度自适应换行、多行尽量等分），
// 逐核频率进条形 tooltip；上下文切换 / 中断从实时页移除（历史视图两项都可查）。
// 温度传感器明细仍是卡片级折叠（#127 的既有语义）。
// 卡片折叠（#127）：每传感器明细默认收起、展开显示全部；
// 折叠状态不持久化；可用性空态不进折叠。实时视图为「页头 + 通栏主机条（#430）+ 单列网格」：
// 网格内 CPU 块 / 指标台账跨满整行，列数不再是版面变量；图表用容器自适应重排（沿用既有断点，
// 不使用容器查询）。
// 时间卡片（#302，契约 sysmon §2.9 / §2.8）：主数值为带符号偏差（1 位毫秒、正 = 本地快），
// 副信息为误差界 / 来源服务器 / stratum / 上次探测相对时间 / 偏差方向说明（#326：正 = 本地快于
// 参考源，触屏与键盘用户看不到注释）；偏差按配置阈值着色（与资源类共用
// `useResourceLevel` 的定档实现与同一套等级词表），误差界越界另给「测量质量」提示且**不改变**
// 偏差着色；展开显示每台服务器明细与同步服务检测（多服务运行给冲突提示）。三种空态（配置关闭 /
// 尚未探测 / 全部失败）都渲染这张卡且都不显示 0；「立即查询」置灰时长取 `manual_min_interval_sec`，
// 以随后的 WS 快照（`last_probe_unix_ms` 前进）为清除信号。顶栏另有一枚常驻 NTP 徽章（ADR 0048）。
// 刷新基线由发送方 realtime.push_interval_sec（默认 5s）决定，本页不轮询、不改 WS 契约。
const sysmon = useSysmonStore()
/** 历史视图 store（#79）：与实时 store 完全独立，切换视图不影响 WS 与实时快照 */
const history = useSysmonHistoryStore()
/** 内联趋势 store（#127）：历史 REST 补齐 + WS 增量推进，历史不可用时静默不渲染；
 *  补拉降级保留上一轮序列并标记未刷新（#142） */
const trend = useSysmonTrendStore()

/** 视图模式：实时（默认） / 历史 / 配置（#81 展示生效配置 + admin 编辑） */
const viewMode = ref<'realtime' | 'history' | 'config'>('realtime')

// ---- 停推判定的「此刻」（#371）：停推时没有任何事件到达，时长必须自己走秒 ----
// 本页靠 WS 推送驱动（契约 sysmon §4），一帧都不来的时候没有回调可用；「快照已停 N 秒」
// 里的 N 若只在推送时算一次，就会永远停在推送前那一刻（正是本票要修的那句「永不失效的断言」）。
// 1 秒一跳，只写一个 ref；不轮询、不发请求、不改 WS 契约。
// 「上次何时收到」由 store 提供（`sysmon.lastReceivedAtMs`），不在这里记：那是快照流的属性，
// 且这个组件在路由内被重建时（切走再切回）局部 ref 会归零，会在停推的机器上显示成「刚收到」。
const nowMs = ref(Date.now())
let freshnessTicker: ReturnType<typeof setInterval> | null = null

onMounted(() => {
  void sysmon.init()
  // 内联趋势（#127）：进入页面时补拉最近 15 分钟历史；历史库不可用/被关闭时静默降级，
  // 已保留的上一轮曲线在补拉降级时继续显示并标记未刷新（#142）
  void trend.init()
  freshnessTicker = setInterval(() => { nowMs.value = Date.now() }, 1_000)
})

onUnmounted(() => {
  // 定时器必须随组件清理：本页在三种视图间切换时不重建，但离开路由后没人再读数据龄
  if (freshnessTicker !== null) clearInterval(freshnessTicker)
  freshnessTicker = null
  // #435：每核条形的 ResizeObserver 同理——ref→null 的 watch 在组件卸载时不一定触发，
  // 必须在卸载钩子里断开（与 Topbar 的既有做法一致）
  disconnectCoreBarsObserver()
})

/**
 * 点击 sparkline 或数值：切到历史视图并预选该指标（验收标准「跳转到历史视图并预选对应指标」）。
 * 只预选、不查询 —— 查询由历史视图挂载时统一发起，避免同一指标查两次。
 */
function openHistory(metricKey: string): void {
  history.preselect(metricKey)
  viewMode.value = 'history'
}

/**
 * 可点标量的历史入口说明（#325）：让「数值可点、点了看什么」可被看见。
 * 此前唯一线索是 `:hover` 下划线——触屏与键盘用户完全看不到。
 * - `title`：短句动作（悬停提示）；
 * - `aria-label`：**先带可见数值**（WCAG 2.5.3 Label in Name——读屏与语音控制要能用可见文本
 *   定位控件），再接同一个动作。
 * 不写死历史区间：历史视图有独立的区间选择器，区间文案的唯一真相源是 `SYSMON_HISTORY_RANGES`。
 */
function historyTitle(metricKey: string): string {
  return `查看${trendMetricLabel(metricKey)}历史变化`
}

function historyAriaLabel(metricKey: string, visibleValue: string): string {
  return `${trendMetricLabel(metricKey)} ${visibleValue}，${historyTitle(metricKey)}`
}

/** 台账行可点数值的 title（有 `historyKey` 的行才有；口径与 #325 的标量完全一致） */
function rowHistoryTitle(row: LedgerRow): string {
  return row.historyKey === null ? '' : historyTitle(row.historyKey)
}

/** 台账行可点数值的 aria-label：**先带可见数值**（WCAG 2.5.3），再接同一个动作 */
function rowHistoryAriaLabel(row: LedgerRow): string {
  return row.historyKey === null ? '' : historyAriaLabel(row.historyKey, row.value)
}

/** 点击台账行数值：切到历史视图并预选该指标（查询由历史视图挂载时发起） */
function openRowHistory(row: LedgerRow): void {
  if (row.historyKey !== null) openHistory(row.historyKey)
}

// ---- 配置草稿守卫（#322）----
// 配置 tab 用 v-show 常驻（切视图不再卸载，草稿因此不丢）；但它仍会随页面卸载而丢失，
// 而离开路由是日常最易误触的丢失路径（侧栏就在旁边），所以离开时按 `dirty` 拦一次。
// 判定口径完全复用组件内的 `dirty`（不新写第二套 diff）；不拦刷新/关页（beforeunload）。
//
// 已知残留（本改动**没有**引入、也没变差）：`SysmonConfigTab` 内的
// `watch(config, …, { immediate: true })` 在每次镜像更新时重置表单草稿。v-if 时代它只在
// 组件挂着时生效；v-show 后组件常驻，于是「隐藏期间收到 WS sysmon_config」也会重置草稿。
// 两种情况里草稿都会丢（挂着时收到推送同样会丢），差别只是发生时机；要做「脏时冻结草稿 +
// 冲突提示」得另开票（#322 的「不做」段）。
const configTabRef = ref<InstanceType<typeof SysmonConfigTab> | null>(null)
// 生效镜像未到达（`GET /api/sysmon/config` 503 或 WS 尚未推送）时表单是空的，组件内的 `dirty`
// 会把「空表单解析不出数字」算成有改动 —— 那会让每次离开都误报「有未保存的改动」。
// 因此只在镜像就绪后才认这个判定（镜像一旦到达就不会再变回 null）。
const configDirty = computed(() => sysmon.config !== null && configTabRef.value?.dirty === true)
const leaveConfirmOpen = ref(false)
/** 挂起的路由离开回调：确认后调用 `next()`，取消时 `next(false)` */
let pendingLeaveNext: NavigationGuardNext | null = null

function cancelLeave(): void {
  leaveConfirmOpen.value = false
  pendingLeaveNext?.(false)
  pendingLeaveNext = null
}

function confirmLeave(): void {
  leaveConfirmOpen.value = false
  const next = pendingLeaveNext
  pendingLeaveNext = null
  next?.()
}

// 守卫必须注册在 setup 同步阶段（路由离开时才注册会漏掉本次导航）。
// 只在真的挂了 router 时注册：`onBeforeRouteLeave` 依赖 `<router-view>` provide 的路由记录，
// 直接 mount 本组件的单元测试没有路由上下文（不判断会在每个用例上打一条 vue-router 警告）。
// 取 router 用 appContext 而不是 useRouter()：后者在没有 router 时本身就会 warn。
const router = getCurrentInstance()?.appContext.config.globalProperties.$router as Router | undefined
if (router) {
  onBeforeRouteLeave((to, _from, next) => {
    // 会话结束类导航直接放行：登出路径是「先清 token 再 push('/login')」
    // （`Sidebar.vue` logout），此时本页已在 AppShell 里且会话已失效，
    // 再拦下来会让用户停在已登出的私有页上（选「留下」也没有任何意义）。
    if (to.name === 'login' || !configDirty.value) {
      next()
      return
    }
    pendingLeaveNext = next
    leaveConfirmOpen.value = true
  })
}

const snapshot = computed(() => sysmon.snapshot)
const system = computed(() => snapshot.value?.system ?? null)
const platformDescription = computed(() => snapshot.value?.availability.platform_description ?? '')
const updatedAt = computed(() => {
  const timestamp = snapshot.value?.timestamp
  return typeof timestamp === 'number' ? formatDateTime(timestamp) : ''
})

// ---- 快照停推判定（#371；术语见 GLOSSARY.md「数据龄」）----
// 判据是**本机收到新快照的时刻**（store 的 `lastReceivedAtMs`），不是发送方 `snapshot.timestamp`：
// 两机时钟偏差会让「本机 now − 发送方 timestamp」两个方向都坏（钟慢 → 永久假红，钟快 → 永久假绿），
// 详见 composables/sysmonFreshness.ts 模块头。发送方 timestamp 仍用于「更新于 …」的展示。
// 阈值来自生效配置 `realtime.push_interval_sec`（3 倍，默认 5s → 15s），不硬编码在组件里。
const pushIntervalSec = computed(() => resolvePushIntervalSec(sysmon.config?.realtime))
const snapshotAge = computed(() => receivedAgeMs(sysmon.lastReceivedAtMs, nowMs.value))
/** 停推：距上次收到新快照已超阈值（无接收记录时为 false —— 不知道就不说） */
const snapshotStale = computed(() => isSnapshotStale(snapshotAge.value, pushIntervalSec.value))
/** 降级文案「快照已停 N 秒」；未停推为 null（此时状态槽显示「更新于 …」） */
const snapshotStaleText = computed(() => staleSnapshotText(snapshotAge.value, pushIntervalSec.value))

// ---- 格式化（#126：唯一实现收敛到 @/composables/format，本文件不得再写 toFixed）----
// 「不可用」占位直接用共享模块导出的 UNAVAILABLE_TEXT（不在本文件另起别名，
// 否则同一事实出现两个名字——Topbar 等其它文件都直接用导出名）。
/** 配置关闭的统一文案（卷 / 指标可用性映射共用，避免两处字面量漂移） */
const DISABLED_BY_CONFIG_TEXT = '已在配置中关闭'

/** 卷的不可用原因（契约 sysmon §2.4 建议取值）：文案化，裸 reason 兜底 */
function volumeReasonDesc(reason: string | undefined): string {
  if (!reason) return '容量读取失败'
  if (reason === 'no_permission') return '无访问权限'
  if (reason === 'path_not_found') return '路径不存在或无法解析'
  if (reason === 'collect_failed') return '采集失败'
  if (reason === 'disabled_by_config') return DISABLED_BY_CONFIG_TEXT
  return reason
}

// ---- 可用性映射（契约 sysmon §2.5：按数据渲染，不按 OS 名硬编码分支）----
function isAvailable(metric: string): boolean {
  return sysmon.availabilityOf(metric)?.available === true
}

function unavailableDesc(metric: string): string {
  // 文案映射只有一份（`composables/monitorLedger` 的 `unavailableReasonText`，台账共用）：
  // 这里只负责先查可用性映射拿到 reason，不在视图里再写一条 if 链。
  return unavailableReasonText(sysmon.availabilityOf(metric)?.reason)
}

// ---- CPU 总使用率 ----
const cpuAvailable = computed(() => isAvailable('cpu.total'))
const cpuRatio = computed(() => sysmon.cpuUsageRatio)
const cpuPercent = computed(() => formatPercent(cpuRatio.value))
const cpuEmptyDesc = computed(() => cpuAvailable.value
  ? '暂无可用采样（首次采样未完成或计数器重置）'
  : unavailableDesc('cpu.total'))

// ---- CPU 区块补全（#72）：每核 / 系统负载 / 每核频率 / 温度（上下文切换与中断已随 #435 移出实时页） ----
// 只按 availability.metrics 与数据语义渲染，不按 platform.family 分支。
const cpuCoresAvailable = computed(() => isAvailable('cpu.cores'))
const cpuCores = computed(() => sysmon.cpuCores)

/**
 * 每核条形空态（#435 从台账 CPU 行迁入）：三档不混淆——可用但本帧无数据 / 配置关闭 /
 * 能力不可用。`disabled_by_config` 是唯一可自助恢复的一类，不能并进「不可用」；
 * 恢复提示指向配置页（CPU 指标组开关），与契约 sysmon §3.1.1 的 `sampling.groups.cpu` 对应。
 */
function coresUnavailableReason(): string | undefined {
  return sysmon.availabilityOf('cpu.cores')?.reason
}
const coresEmptyTitle = computed(() => {
  if (cpuCoresAvailable.value) return '暂无每核数据'
  return coresUnavailableReason() === 'disabled_by_config' ? DISABLED_BY_CONFIG_TEXT : UNAVAILABLE_TEXT
})
const coresEmptyDesc = computed(() => {
  if (cpuCoresAvailable.value) return '当前快照未提供每核数据'
  if (coresUnavailableReason() === 'disabled_by_config') return '可在「配置」页重新启用 CPU 指标组'
  return unavailableDesc('cpu.cores')
})

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

// ---- 指标台账（#363）：三组、每行七列，行数据全部来自 `buildMonitorLedger` ----
// 视图**不写任何「该不该显示」的判断**：行是否存在、空态是哪一类、阈值与余量写什么、
// 数值能不能点跳历史，全部由 `@/composables/monitorLedger` 决定（纯函数，可单测）。
// 这里只把 store 已净化的 computed 与生效配置交给它，并把结果摆成七列。
//
// 台账之外的存量口径继续走 store 的净化 computed：内存 / 交换 / 卷 / 磁盘 I/O / 网络 / 时钟
// 一律不直接读 payload 字段。
const zeroCapacityVolumes = computed(() => sysmon.zeroCapacityVolumes)
const zeroVolumesOpen = ref(false)

/** 生效快照的时刻（服务端时间轴）：台账的「上次探测多久以前」用它，不用本机 Date.now() */
const ledgerNowUnixMs = computed(() => {
  const ts = snapshot.value?.timestamp
  return typeof ts === 'number' ? ts : Date.now()
})

/** 台账两组（存储 / 网络与时钟），行数由数据决定；CPU 组随 #435、内存组随 #445 移入各自的块卡 */
const ledger = computed<LedgerGroup[]>(() => buildMonitorLedger({
  readableVolumes: sysmon.readableVolumes,
  zeroCapacityVolumes: sysmon.zeroCapacityVolumes,
  diskIo: sysmon.diskIo,
  network: sysmon.network,
  shmDir: sysmon.shmDirInfo,
  clock: sysmon.clock,
  thresholds: sysmon.config?.thresholds ?? null,
  clockConfig: sysmon.config?.clock ?? null,
  // 可用性映射直接传 store 的查询函数：台账按它区分「不可用」「配置关闭」「暂无数据」
  availability: metric => sysmon.availabilityOf(metric),
  nowUnixMs: ledgerNowUnixMs.value,
  networkFilteredAll: sysmon.networkFilteredAll && sysmon.hideVirtual,
}))

// ---- 内存块（#445）：照主机面板插件内存卡（半宽、Meter + 组成带 + 交换子条） ----

/** 内存快照与能力（契约 sysmon §2.4；可用性按映射判，不按平台名分支） */
const memoryInfo = computed(() => sysmon.memoryInfo)
const memoryAvailable = computed(() => isAvailable('memory'))
const memoryUsedRatio = computed(() => memoryInfo.value?.used_ratio ?? null)
/** 内存已用率的展示等级：与顶栏 MEM 徽章共用 70 / 90 分档（ADR 0034，色标不是越限判定） */
const memoryLevel = computed(() => (memoryUsedRatio.value === null ? null : resourceLevel(memoryUsedRatio.value)))
const memoryEmptyTitle = computed(() => {
  if (memoryAvailable.value) return '暂无数据'
  return sysmon.availabilityOf('memory')?.reason === 'disabled_by_config' ? DISABLED_BY_CONFIG_TEXT : UNAVAILABLE_TEXT
})
const memoryEmptyDesc = computed(() => {
  if (memoryAvailable.value) return '当前快照未提供内存数据'
  if (sysmon.availabilityOf('memory')?.reason === 'disabled_by_config') return '可在「配置」页重新启用内存指标组'
  return unavailableDesc('memory')
})

/** 内存组成带（#363）：四段 + 图例；缓存/缓冲不可得时退化为两段并标注（不补 0） */
const memoryComposition = computed(() => buildMemoryComposition(sysmon.memoryInfo))

/** 交换分区（契约 sysmon §2.4：Windows = 页面文件支撑的提交上限口径；无交换为 unsupported） */
const swapInfo = computed(() => sysmon.swapInfo)
const swapAvailable = computed(() => isAvailable('swap'))
const swapUsedRatio = computed(() => swapInfo.value?.used_ratio ?? null)
/** 交换已用率的展示等级：与内存同一 70 / 90 分档；交换无单独顶栏徽章 */
const swapLevel = computed(() => (swapUsedRatio.value === null ? null : resourceLevel(swapUsedRatio.value)))
/** 有可画的交换读数：可用 + 本帧有数据 + 总量 > 0（总量 0 是「无交换」事实，不画 0%） */
const swapHasData = computed(
  () => swapAvailable.value && swapInfo.value !== null && swapInfo.value.total_bytes > 0,
)
/** 交换子区的空态文案：无交换 / 配置关闭 / 不可用 / 暂无数据，四档不混淆 */
const swapEmptyText = computed(() => {
  if (!swapAvailable.value) {
    const reason = sysmon.availabilityOf('swap')?.reason
    if (reason === 'unsupported') return '本机无交换分区/页面文件'
    if (reason === 'disabled_by_config') return DISABLED_BY_CONFIG_TEXT
    return `${UNAVAILABLE_TEXT} · ${unavailableDesc('swap')}`
  }
  // 可用但总量 0：与 unsupported 同一平台事实（契约 §2.4：无交换时 total_bytes=0），不是 0%
  if (swapInfo.value !== null && swapInfo.value.total_bytes === 0) return '本机无交换分区/页面文件'
  return '暂无数据：本帧未采集到交换分区'
})

/**
 * Meter 填充比例 → CSS 变量（与 CPU 每核柱的 `--core-fill` 同一形态：值逐帧不同，只能运行时注入）。
 * `ratio === null` 不注入变量 → 轨道空着；与「真的 0%」的区别由数值文字承担（空读数写「不可用」）。
 */
function meterFillStyle(ratio: number | null): Record<string, string> {
  if (ratio === null) return {}
  const clamped = Math.min(1, Math.max(0, ratio))
  return { '--meter-fill': formatPercent(clamped) }
}

/**
 * 轨道填充比例 → CSS 变量（与 CPU 每核柱的 `--core-fill` 同一形态：值逐行不同，只能运行时注入）。
 * `ratio === null`（网卡 / 共享内存 / 时钟：没有可比的分母）返回空对象 → 不注入变量，
 * 样式里 `width: var(--ledger-fill, 0%)` 取兜底值，视觉上是一条空轨道。空轨道与「真的 0%」
 * 的区别由当前值文字承担（台账的空态行根本没有 ratio，也不画填充）。
 */
function trackFillStyle(row: LedgerRow): Record<string, string> {
  if (row.ratio === null) return {}
  const clamped = Math.min(1, Math.max(0, row.ratio))
  return { '--ledger-fill': formatPercent(clamped) }
}

/** 组成带某段占总量比例 → CSS 变量（`flex-basis`），不可得的段不画（不注入变量） */
function memorySegmentStyle(segment: MemoryCompositionSegment): Record<string, string> {
  if (segment.ratio === null) return {}
  return { '--memory-seg': formatPercent(Math.min(1, Math.max(0, segment.ratio))) }
}

/**
 * 轨道缺口位置 → CSS 变量。两个位置来自**展示分档常量**（`RESOURCE_WARN_RATIO` /
 * `RESOURCE_DANGER_RATIO`，ADR 0034 明文是展示规则），不是契约阈值——因此缺口与「阈值」列
 * 是两个不同的东西：缺口位置不随配置变化，阈值列随 `thresholds` 变化。
 */
function gapStyle(ratio: number): Record<string, string> {
  return { '--ledger-gap': formatPercent(ratio) }
}

/** 该行是否有契约阈值（只有有阈值的行才在轨道上开 70/90 缺口） */
function hasThreshold(row: LedgerRow): boolean {
  return row.thresholdText !== NO_THRESHOLD_TEXT
}

/** 行 id → data-test（稳定的按类选择器；行 id 由 #361 定义，这里不做语义判断） */
function rowTestId(row: LedgerRow): string {
  if (row.id.startsWith('volume:')) return 'monitor-disk-volume'
  if (row.id.startsWith('disk:')) return 'monitor-disk-io-row'
  if (row.id.startsWith('nic:')) return 'monitor-network-row'
  if (row.id === 'memory') return 'monitor-memory-row'
  if (row.id === 'swap') return 'monitor-swap-row'
  if (row.id === 'shm_dir') return 'monitor-shm-row'
  if (row.id === 'clock') return 'monitor-clock'
  return `ledger-row-${row.id}`
}

/** 标量行的数值 data-test：四个既有可点标量沿用原选择器（#325 的入口不掉），其余行通用 */
function rowValueTestId(row: LedgerRow): string {
  if (row.id === 'memory') return 'monitor-memory-value'
  if (row.id === 'swap') return 'monitor-memory-swap-usage'
  if (row.id === 'shm_dir') return 'monitor-shm-total'
  if (row.id === 'clock') return 'monitor-clock-offset'
  return 'ledger-value'
}

/**
 * 值列是否存在「读数槽」。
 *
 * - `ok`：有读数 → 四列都渲染（含可选的历史入口）；
 * - `unavailable`：**尝试过读但没读到** → 槽仍在，写「不可用」（不是 0、也不是空白）；
 * - `empty` / `disabled` / `none`：**没有这次读数**（本帧未采集 / 被配置关掉 / 本机无此维度）→
 *   值列写 `—`，且不提供读数槽（浏览器与测试都不该在这里找一个「当前值」）。
 */
function rowHasReading(row: LedgerRow): boolean {
  return row.state === 'ok' || row.state === 'unavailable'
}

/** 值列的显示文本：有读数用读数，不可用写「不可用」，其余空态写 `—`（都不是 0） */
function rowValueText(row: LedgerRow): string {
  if (row.state === 'ok') return row.value
  if (row.state === 'unavailable') return UNAVAILABLE_TEXT
  return NO_THRESHOLD_TEXT
}

/** 行 id 的维度部分（`volume:<mount>` → 挂载点 / `disk:<device>` / `nic:<if>:<dir>`） */
function rowDimension(row: LedgerRow): string {
  const [, ...rest] = row.id.split(':')
  return rest.join(':')
}

/** 标量行的趋势序列（数据在 `stores/sysmonTrend`，末点即当前值） */
function rowSparklinePoints(row: LedgerRow): SparklinePoint[] {
  return row.historyKey === null ? [] : trend.seriesOf(row.historyKey)
}

/** 趋势 sparkline 的 data-test 后缀：沿用 #127 的既有选择器名（memory / swap / shm） */
function rowSparklineTestSuffix(row: LedgerRow): string {
  if (row.id === 'memory') return 'memory'
  if (row.id === 'swap') return 'swap'
  if (row.id === 'shm_dir') return 'shm'
  return row.id
}

/** 网卡行的方向（`rx` / `tx`）：明细段的 data-test 用它拼出 `monitor-network-rx-errors` 等 */
function directionTestId(row: LedgerRow): string {
  return row.id.split(':')[2] ?? ''
}

/** 状态点旁的文字：正常/警戒/危险（`ok` 行）或四类空态各自的短标签——状态不得只靠颜色 */
function rowStateTag(row: LedgerRow): string {
  return row.state === 'ok' ? row.levelText : row.stateLabel
}

/** 空态行的说明文字（`ok` 行没有说明；值列为 `—`） */
function rowStateNote(row: LedgerRow): string {
  return row.state === 'ok' ? '' : row.stateText
}

// ---- 行明细展开（#363）：折叠状态只存组件本地 ref，**不持久化**（刷新回默认收起）----
const openLedgerRows = ref<Record<string, boolean>>({})

function toggleLedgerRow(rowId: string): void {
  openLedgerRows.value = { ...openLedgerRows.value, [rowId]: !openLedgerRows.value[rowId] }
}

function isLedgerRowOpen(rowId: string): boolean {
  return openLedgerRows.value[rowId] === true
}

// ---- 磁盘 I/O 语义提示（#207 B2）：稳定 key 映射中文，原文留在 tooltip ----
const diskIoHintRaw = computed(() => sysmon.semanticHint('disk_io'))
const diskIoHint = computed(() => {
  const raw = diskIoHintRaw.value
  return raw === '' ? null : { text: semanticHintText('disk_io', raw), raw }
})
// ---- 网络「显示全部网卡」开关（#77）：admin 才可操作，实际值取生效配置镜像 ----
const networkAvailable = computed(() => isAvailable('network'))
// 「显示全部网卡」开关：admin 才可操作（复用 auth store 的 isAdmin）；实际值取生效配置镜像
// （契约 sysmon §3.1 network.hide_virtual），切换后等 WS sysmon_config 回推才变更。
const auth = useAuthStore()
const toast = useToastStore()
const canToggleNetwork = computed(() => auth.isAdmin)
const hideVirtual = computed(() => sysmon.hideVirtual)
const showAllNics = computed(() => !hideVirtual.value)
const hideVirtualPending = computed(() => sysmon.hideVirtualPending)

// 开关语义是「显示全部网卡」(= hide_virtual 取反)，直接下发取反后的 hide_virtual 值。
// 结果契约（对齐 settings/mdConfig 的配置类操作）：true = 已下发（等 WS sysmon_config 生效）、
// false = 防重入或 HTTP 失败、PENDING_TIMEOUT = 超时（usePending 已 toast，此处不重复弹）。
// 失败必须有可见反馈：否则用户点了开关、界面无变化也无提示（静默失败）。
async function onToggleShowAll(next: boolean): Promise<void> {
  const result = await sysmon.setHideVirtual(!next)
  if (result === PENDING_TIMEOUT) return
  if (result === false) toast.error('显示全部网卡切换下发失败')
}

// ---- 时钟偏差（#302，契约 sysmon §2.9 / §2.8）：卡片主数值 + 每台明细 + 同步服务检测 ----
// 状态分类、定档、质量判据、失败原因文案全部在 `@/composables/sysmonClock`（纯函数，可单测）；
// 本视图只负责取 store 已净化的域与渲染。三条口径要点：
// - 三种空态**都渲染卡片**（配置关闭 = 明确的空态而不是整块消失）：`clock` 域的 null 是
//   「这项监控被关掉了」这一运维事实，静默消失会让读者以为功能不存在；
// - 偏差着色按 `|offset_ms|` 与配置阈值（与资源类共用同一份定档实现与同一套等级词表）；
// - 「测量质量」提示由 `bound_ms > bound_alarm_ms` 独立决定，**不改变**偏差着色（契约 §2.8）。
const clock = computed(() => sysmon.clock)
const clockState = computed(() => clockDomainState(clock.value))
// 阈值取生效配置镜像（未到达时按契约 §3.1 默认值，避免卡片先按一套阈值着色、配置到达后跳变）
const clockThresholds = computed(() => resolveClockThresholds(sysmon.config?.clock))
const clockQualityAlarm = computed(
  () => clockBoundAlarm(clock.value?.bound_ms ?? null, clockThresholds.value),
)
// 「上次探测多久之前」：时间基准取快照 timestamp（服务端时间轴），**不用本机 Date.now()**——
// 两者若有系统时钟偏差，相对时间会跟着偏；快照 timestamp 与 last_probe_unix_ms 同一时间轴。
const clockProbeAge = computed(() => {
  const c = clock.value
  const snapshotTs = snapshot.value?.timestamp
  if (!c || typeof snapshotTs !== 'number') return null
  const ageSec = clockProbeAgeSec(c.last_probe_unix_ms, snapshotTs)
  // 文案复用共享模块的 `formatAge`（同一份「多久以前」口径，顶栏时钟徽章共用），不新写第二套
  return ageSec === null ? null : formatAge(ageSec)
})
const clockServers = computed(() => clock.value?.servers ?? [])
const clockDaemons = computed(() => sortClockDaemons(clock.value?.daemons ?? []))
const clockFailure = computed(() => clockFailureSummary(clockServers.value))
const clockDaemonAvailable = computed(() => isAvailable('clock.sync_service'))
const clockDaemonVisible = computed(
  () => clockDaemonAvailable.value && (clockState.value === 'live' || clockState.value === 'unavailable'),
)
// 「立即查询」：admin 可见（非 admin 渲染为 disabled 并给出原因，与网络开关同一口径）
const canProbeClock = computed(() => auth.isAdmin)
const clockProbePending = computed(() => sysmon.clockProbePending)
const clockProbeCooldownSec = computed(() => sysmon.clockProbeCooldownSec)
// 只有「已有探测能力且域未关闭」时才提供按钮：配置关闭时按钮无处可点（点击也只写事件通道，
// dzsysmon 会忽略）。admin 之外仍渲染 disabled —— 可见但不可点，避免「功能不存在」的误解。
const clockProbeDisabled = computed(() => !canProbeClock.value
  || clockProbePending.value
  || clockProbeCooldownSec.value > 0
  || clockState.value === 'disabled')
const clockProbeLabel = computed(() => {
  // 冷却文案优先：成功触发后两条约束同时成立（等快照 + N 秒限流），pending 可能先解除，
  // 而按钮仍不可点——此时必须说「N 秒后可再次查询」而不是「查询中…」，否则用户看到的是
  // 一个点不动、又自称正在查询的按钮。因此有冷却剩余就显示冷却，没有才显示 pending。
  if (clockProbeCooldownSec.value > 0) return `${clockProbeCooldownSec.value} 秒后可再次查询`
  if (clockProbePending.value) return '查询中…'
  return '立即查询'
})

/**
 * 「立即查询」：结果契约对齐配置类操作（true = 已触发、等快照；false = 防空转 / HTTP 失败；
 * PENDING_TIMEOUT = 超时，usePending 已 toast）。REST 成功只表示「已写事件通道」——协议不承诺
 * 本次触发一定生效（dzsysmon 权威限流，契约 sysmon §1.1），因此这里**不**提示「已探测完成」。
 */
async function onProbeClock(): Promise<void> {
  const result = await sysmon.probeClock()
  if (result === PENDING_TIMEOUT) return
  if (result === false && !clockProbePending.value && clockProbeCooldownSec.value === 0) {
    toast.error('立即查询时钟偏差下发失败')
  }
}

// ---- 温度读数（#120 / #435）：主值取全局最高温，逐传感器明细改为悬停 / 读屏可达 ----
// #435 验收反馈：四数字行里只有温度格带一个「展开明细」按钮，视觉上多出一行、和其余三格
// 不齐；改为把逐传感器读数放进值的 title / aria（四格同一高度，信息仍可达，不丢能力）。

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
/**
 * 温度格的 hover / 读屏补充：逐传感器读数按「芯片 · 通道 温度」列出。
 * 主值是全部传感器里的最高温（#120 的归属口径），这里给全量明细，避免为看另一颗
 * 传感器的读数去展开一个只有这一格才有的折叠按钮。
 */
const temperatureSensorTitle = computed(() => {
  const rows = temperatureGroups.value.flatMap(group =>
    group.readings.map(reading => `${group.displayName} · ${reading.label} ${formatTemperature(reading.celsius)}`),
  )
  return rows.length > 0 ? `全部传感器：${rows.join('；')}` : ''
})

/**
 * 温度格的读屏名：`aria-label` 挂在 `role="group"` 的统计格上。
 * 裸 div 的 aria-label 会被读屏忽略（ARIA 规范禁止在 generic 角色上命名），因此不能只写在值上；
 * 值本身保留 `title` 供鼠标悬停，group 的 aria-label 负责把「主值 + 全部传感器」一次念完。
 */
const temperatureAriaLabel = computed(() => {
  const value = formatTemperature(temperaturePeak.value?.celsius ?? null)
  return temperatureSensorTitle.value === '' ? `温度 ${value}` : `温度 ${value}；${temperatureSensorTitle.value}`
})

// ---- 主机条（#430）：一条通栏主机信息条（主机 / 系统 / 内核 / 已运行 + 右端状态槽） ----
// 字段全部来自既有快照的 `system` / `platform_description` / `platform.kernel`，不新增接口。
// 无可显示值的字段统一 `—`（不是空白、不是 0）：空串与 null 都算「没有这个值」。
function displayOrDash(value: string | number | null | undefined): string {
  if (value === null || value === undefined) return '—'
  if (typeof value === 'string' && value.trim() === '') return '—'
  return String(value)
}

const hostName = computed(() => displayOrDash(system.value?.hostname))
/**
 * 系统（发行版名）：`platform_description` 已含 `(kernel …)` / `(build …)`，主机条把内核单列，
 * 所以这里剥掉末尾括号段——纯展示层派生，规则与边界见 `formatPlatformName`（#429 决策 3）。
 */
const hostSystem = computed(() => displayOrDash(formatPlatformName(platformDescription.value)))
const hostKernel = computed(() => displayOrDash(snapshot.value?.platform?.kernel))
const hostUptime = computed(() => (system.value ? formatUptime(system.value.uptime_sec) : '—'))
/**
 * 右端状态槽：快照新鲜时说「更新于 …」，停推时同一位置改说停推事实（数据龄并入，不再单起一行）；
 * 两者都不可得时给 `—`（不显示「实时」这种无法被推翻的话）。
 */
const hostStatusText = computed(() => {
  if (snapshotStaleText.value !== null) return snapshotStaleText.value
  const timestamp = snapshot.value?.timestamp
  if (typeof timestamp !== 'number') return '—'
  return `更新于 ${formatClockTime(timestamp, nowMs.value)}`
})
/**
 * 「更新于」的悬停补充（#432）：采样间隔与累计漏拍。两者是**数据质量**事实（采样是否在跑、
 * 有没有漏拍），不是主机身份，所以只进 title、不占版面。字段缺失或非法时不拼半句、不拿 0 冒充：
 * `measure_interval_ms` 是计量间隔（基线 1000，非正值视为没有），`missed_ticks` 是累计漏拍（0 是合法值）。
 */
const hostSamplingTitle = computed(() => {
  const sampling = snapshot.value?.sampling
  if (!sampling) return ''
  const parts: string[] = []
  const interval = sampling.measure_interval_ms
  if (typeof interval === 'number' && Number.isFinite(interval) && interval > 0) {
    parts.push(`采样间隔 ${interval} ms`)
  }
  const missed = sampling.missed_ticks
  if (typeof missed === 'number' && Number.isFinite(missed)) parts.push(`累计漏拍 ${missed}`)
  return parts.join(' · ')
})

/** 「已运行」的悬停补充（#432）：启动时刻，由 `boot_time_unix_sec` 换算（与已运行同一事实）。 */
const hostStartTimeTitle = computed(() => {
  const boot = system.value?.boot_time_unix_sec
  if (typeof boot !== 'number' || !Number.isFinite(boot) || boot <= 0) return ''
  return `启动于 ${formatDateTime(boot * 1_000)}`
})

/** 逻辑核数文案（CPU 四数字的小字上下文用；主机条不再显示 CPU 型号 · 核数，#429 决策 6） */
const logicalCoreCount = computed(() => system.value?.logical_cores ?? cpuCores.value.length)

// ---- CPU 块（#435）：型号 · 四数字 · 固定 0–100% 单曲线 · 自适应每核条形 ----

/** 标题行右侧的 CPU 型号（原样展示，过长由 CSS 省略号 + title 兜底；#429 决策 6 从主机条移入） */
const cpuModel = computed(() => system.value?.cpu_model?.trim() ?? '')

/**
 * 占用率的小字上下文：只写逻辑核数。
 * 8080 验收反馈：「无核饱和」零信息、真出现时也没人据此做动作——「饱和」这类信息不要了，
 * 每核条形也不再标记饱和核（见 `coreBars` 的按等级着色）。
 */
const coresContextText = computed(() => (logicalCoreCount.value > 0 ? `${logicalCoreCount.value} 逻辑核` : '—'))

/**
 * 占用率主数值的展示等级（#435 决策 7）：与顶栏 CPU 徽章共用 `useResourceLevel` 的 70 / 90
 * 两个展示分档（ADR 0034：色标是展示规则，不是越限判定）。颜色只是冗余信号——数字本身
 * 始终可读；未知值不套色。
 */
const cpuUsageLevel = computed(() => (cpuRatio.value === null ? null : resourceLevel(cpuRatio.value)))

/**
 * 当前频率（主数值）：全部有读数核的**平均**（#435 决策 3）。
 *
 * 不用最高核：单核瞬时 boost 会主导读数，让数字在 800 MHz ↔ 3.4 GHz 之间跳；平均值代表
 * 「整颗 CPU 现在跑在哪一档」，单核的最高值仍可在每核条形的 tooltip 里看到。
 * 无读数时 null（不伪造 0 MHz）。
 */
const frequencyAverage = computed(() => {
  let sum = 0
  let count = 0
  for (const frequency of coreFrequencies.value) {
    if (!Number.isFinite(frequency.mhz) || frequency.mhz <= 0) continue
    sum += frequency.mhz
    count += 1
  }
  return count === 0 ? null : Math.round(sum / count)
})

/**
 * 频率主值的「数值 + 单位」两段排版（#435 8080 验收：对齐主机面板的 19px 数字 + 12px 单位，
 * ≥1000 MHz 换成 GHz）。格式化口径只在 `formatFrequencyParts` 里定义。
 */
const frequencyDisplay = computed(() => formatFrequencyParts(frequencyAverage.value))

/** 每核频率按 OS CPU 编号查表（tooltip 用；`cpu.core_frequencies[].index` 与 `cpu.cores[].index` 同源） */
const frequencyByCoreIndex = computed(() => {
  const map = new Map<number, number>()
  for (const frequency of coreFrequencies.value) map.set(frequency.index, frequency.mhz)
  return map
})

/** 一条每核条形：数值、填充比例与 title / 读屏共用的文案（视图只负责摆，不再判断） */
interface CoreBar {
  index: number
  /** 本核占用率的展示等级（normal / warn / danger），与顶栏 CPU 徽章同一 70 / 90 分档 */
  level: ResourceLevel | null
  /** 本帧是否有该核读数：false 时轨道加斜纹，避免「缺失」与「真的 0%」在图形层同形 */
  available: boolean
  style: Record<string, string>
  /** `核 N · 45.0%`；频率可用时追加 ` · 3.20 GHz` */
  text: string
}

/**
 * 每核条形数据（#435）：填充的是「占用率」而不是固定满格；轨道中性底色给出 100% 的参照。
 *
 * `null`（首次采样未就绪）不注入 `--core-fill` → 样式里的 `height: var(--core-fill, 0%)` 取
 * 兜底 0%，视觉上是一条空轨道，与「真的 0%」同形；区别由 tooltip 文字承担
 * （`formatPercent(null)` 显示「不可用」），本格不画任何数字，也不编造 0。
 */
const coreBars = computed<CoreBar[]>(() => cpuCores.value.map(core => {
  const ratio = typeof core.usage_ratio === 'number' && Number.isFinite(core.usage_ratio)
    ? Math.min(1, Math.max(0, core.usage_ratio))
    : null
  const frequency = frequencyByCoreIndex.value.get(core.index)
  const parts = [`核 ${core.index}`, formatPercent(ratio)]
  if (typeof frequency === 'number' && Number.isFinite(frequency) && frequency > 0) {
    parts.push(formatFrequency(frequency))
  }
  const style: Record<string, string> = ratio === null ? {} : { '--core-fill': formatPercent(ratio) }
  return {
    index: core.index,
    level: ratio === null ? null : resourceLevel(ratio),
    available: ratio !== null,
    style,
    text: parts.join(' · '),
  }
}))

/**
 * 每核条形容器（#435 决策 6）：用 ResizeObserver 实测容器宽度，交给纯函数
 * `coreBarColumns` 算列数——多行时每行数量尽量相等（23 + 1 这种失衡不允许出现）。
 * 与顶栏资源组同一测量模式（`Topbar.vue`）；jsdom 无布局，组件测试用可手动触发的桩。
 */
const coreBarsRef = ref<HTMLElement | null>(null)
const coreBarsWidth = ref(0)
let coreBarsObserver: ResizeObserver | null = null

function disconnectCoreBarsObserver(): void {
  coreBarsObserver?.disconnect()
  coreBarsObserver = null
}

watch(coreBarsRef, element => {
  disconnectCoreBarsObserver()
  if (element === null) {
    coreBarsWidth.value = 0
    return
  }
  coreBarsWidth.value = element.clientWidth
  if (typeof ResizeObserver !== 'function') return
  coreBarsObserver = new ResizeObserver(() => {
    coreBarsWidth.value = element.clientWidth
  })
  coreBarsObserver.observe(element)
})

/** 列数（宽度未测量时为单行；纯函数见 `composables/monitorCpu.ts`，均衡规则单测覆盖） */
const coreBarColumnCount = computed(() => coreBarColumns(cpuCores.value.length, coreBarsWidth.value))

/** 趋势序列：既有 `cpu.usage_ratio` 窗口（15 分钟 / 60 点 / 15 秒桶），不新增窗口、不改 store */
const usageSeries = computed(() => trend.seriesOf('cpu.usage_ratio'))
/** 时间窗的如实口径（#142）：满 15 分钟才写「最近 15 分钟」，否则写实际跨度 */
const usageWindow = computed(() => trend.windowOf('cpu.usage_ratio'))
/** 折线几何（#435 决策 2）：纵轴固定 0–100%，不随序列 min/max 自适应 */
const usageGeometry = computed(() => percentTrendGeometry(usageSeries.value))
/** 是否覆盖满整个时间窗（data 属性供测试断言，与 SysmonSparkline 同口径） */
const usageWindowComplete = computed(() => sparklineWindowComplete(usageWindow.value))

/**
 * 横轴两端时刻（#435）：与趋势序列同源（首点 / 末点），以快照时间轴换算成墙钟；跨日由
 * `formatClockTime` 带上日期。没有点时不写半截（返回空串，模板据此不渲染该端）。
 */
function trendAxisClock(timestampSec: number | undefined): string {
  if (typeof timestampSec !== 'number' || !Number.isFinite(timestampSec)) return ''
  return formatClockTime(timestampSec * 1_000, nowMs.value)
}
// 用几何自己的有效首/末点，而不是原始序列首末项：折线会丢弃 ts/value 非有限的点，
// 两端标签必须与曲线实际起止同源（否则边缘点非法时标签空着、曲线从别处开始）。
const trendStartText = computed(() => trendAxisClock(usageGeometry.value?.firstTs))
const trendEndText = computed(() => trendAxisClock(usageGeometry.value?.lastTs))

/**
 * 趋势下方说明：只写时间窗（#435 决策 2 去掉了「纵轴 …%」——固定 0–100% 不再需要解释）。
 * 满 15 分钟才写「最近 15 分钟」，否则写实际覆盖（#142 语义不变）。
 */
const cpuTrendCaption = computed(() => sparklineWindowText(usageWindow.value))

/**
 * 趋势 title / aria：指标名 + 时间窗 + 轴口径 + 查看历史（与 SysmonSparkline 同口径）。
 *
 * 可见界面按拍板 2 不写轴范围与刻度；但读屏用户看不到网格线，轴口径与横轴起止时刻
 * 必须在 aria 里补上（视觉隐藏的坐标轴信息不能连无障碍一起删掉）。
 */
const cpuTrendTitle = computed(
  () => `${sparklineTitle(trendMetricLabel('cpu.usage_ratio'), usageWindow.value)} · 纵轴固定 0–100%`,
)
const cpuTrendAriaLabel = computed(() => {
  const range = trendStartText.value !== '' && trendEndText.value !== ''
    ? `，从 ${trendStartText.value} 到 ${trendEndText.value}`
    : ''
  return `${sparklineAriaLabel(trendMetricLabel('cpu.usage_ratio'), usageWindow.value)}，纵轴固定 0–100%${range}`
})


</script>

<template>
  <div class="monitor-view">
    <!-- 顶栏容器（#328）：≥768 单行（标题组 + Tab）；<md 用 display:contents 拆开，
         让视图切换行成为滚动容器 .monitor-view 的直接子级，从而能单独吸顶（不钉整块 header）。 -->
    <div
      class="monitor-view__topbar"
      :class="{ 'monitor-view__topbar--host-follows': viewMode === 'realtime' && snapshot }"
    >
      <header class="monitor-view__header">
        <div class="monitor-view__heading">
          <h1 class="monitor-view__title">资源监控</h1>
          <!-- 副标题只留给历史 / 配置视图：实时视图的主机信息由下方主机条承载（#430），
               副标题与主机条同时出现就是同一事实说两遍（原 #373 的 kernel 重复即由此而来）。 -->
          <p
            v-if="viewMode !== 'realtime'"
            class="monitor-view__subtitle"
            data-test="monitor-subtitle"
          >
            <template v-if="snapshot">{{ platformDescription }}<template v-if="updatedAt"> · 更新于 {{ updatedAt }}</template></template>
            <template v-else>系统监控数据尚未到达</template>
          </p>
        </div>
      </header>
      <!-- 视图切换是分段按钮组，不是 ARIA tabs（#323）：先前声明了 role=tablist/tab 却没有
           tabpanel、aria-controls、方向键与 roving tabindex——读屏按 tabs 报语义、键盘却按按钮走，
           比不做更糟。这里退回普通按钮组，与 LogsView / SettingsView / TradingDataView 一致：
           不声明 ARIA tabs 角色（沿用 ds-btn 分段样式，不是 .ds-tab 类）。选中态用 aria-pressed
           ——切换按钮的推荐形态，且不引入「当前项」这种导航语义：本控件切的是同一页内的视图来源，
           不是跳转目的地（aria-pressed 只宣称「按下」这一个事实，不宣称与面板的关联）。 -->
      <div class="monitor-view__view-switch" data-test="monitor-view-switch">
        <button
          class="ds-btn ds-btn--sm"
          :class="viewMode === 'realtime' ? 'ds-btn--primary' : 'ds-btn--secondary'"
          type="button"
          :aria-pressed="viewMode === 'realtime'"
          data-test="monitor-tab-realtime"
          @click="viewMode = 'realtime'"
        >实时</button>
        <button
          class="ds-btn ds-btn--sm"
          :class="viewMode === 'history' ? 'ds-btn--primary' : 'ds-btn--secondary'"
          type="button"
          :aria-pressed="viewMode === 'history'"
          data-test="monitor-tab-history"
          @click="viewMode = 'history'"
        >历史</button>
        <button
          class="ds-btn ds-btn--sm"
          :class="viewMode === 'config' ? 'ds-btn--primary' : 'ds-btn--secondary'"
          type="button"
          :aria-pressed="viewMode === 'config'"
          data-test="monitor-tab-config"
          @click="viewMode = 'config'"
        >配置</button>
      </div>
    </div>

    <!-- 主机条（#430）：页头下方的一条通栏信息条，取代原先浮在标题行中间的主机卡。
         行为照抄主机面板（`@local/dsh-host-panel` 的 HostStrip）：整条可换行、单条内容不换行
         （超长省略号 + title）、标签次级色 / 值主色、spacer 把剩余宽度吃掉使状态槽恒在所在行最右。
         只在实时视图且快照到达时渲染；无可显示值一律 —（不是空白）。 -->
    <section
      v-if="viewMode === 'realtime' && snapshot"
      class="monitor-view__host"
      :class="{ 'monitor-view__host--stale': snapshotStale }"
      data-test="monitor-host"
      aria-label="主机"
    >
      <span class="monitor-view__host-dot" aria-hidden="true"></span>
      <span class="monitor-view__host-item">
        <span class="monitor-view__host-label">主机</span>
        <span class="monitor-view__host-value" data-test="monitor-host-name" :title="hostName">{{ hostName }}</span>
      </span>
      <span class="monitor-view__host-item">
        <span class="monitor-view__host-label">系统</span>
        <span class="monitor-view__host-value" data-test="monitor-host-system" :title="hostSystem">{{ hostSystem }}</span>
      </span>
      <span class="monitor-view__host-item">
        <span class="monitor-view__host-label">内核</span>
        <span class="monitor-view__host-value" data-test="monitor-host-kernel" :title="hostKernel">{{ hostKernel }}</span>
      </span>
      <span class="monitor-view__host-item">
        <span class="monitor-view__host-label">已运行</span>
        <span
          class="monitor-view__host-value"
          data-test="monitor-host-uptime"
          :title="hostStartTimeTitle || undefined"
        >{{ hostUptime }}</span>
      </span>
      <span class="monitor-view__host-spacer" aria-hidden="true"></span>
      <span
        class="monitor-view__host-status"
        data-test="monitor-host-status"
        :title="hostSamplingTitle || undefined"
      >{{ hostStatusText }}</span>
    </section>

    <!-- 历史视图（#79）：独立渲染，不卸载实时 store（WS 连接与实时数据不受影响）。
         历史 tab **保持 v-if**：它的 onMounted 会发一次 history REST 查询，store 无缓存无去重，
         常驻会改变加载时机（#322 有意保留这一处不对称）。 -->
    <SysmonHistoryTab v-if="viewMode === 'history'" />

    <!-- 配置视图（#81）：展示生效配置（所有认证用户）+ 编辑入口（仅 admin）。
         用 v-show 而不是 v-if：草稿是组件本地 reactive，卸载即丢（#322）；本组件无 onMounted、
         无网络请求、无定时器，常驻代价仅是多一份表单 DOM 与一个 watch。 -->
    <SysmonConfigTab ref="configTabRef" v-show="viewMode === 'config'" />

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
      <!-- 主机条是顶栏下方的独立一条（#430），网格里不再重复渲染。

       CPU 块（#435）：标题 + 右上型号 → 四数字（占用率 / 系统负载 / 当前频率 / 最高温度）→
           固定 0–100% 单曲线（横轴两端真实时刻）→ 每核条形（按容器宽度自适应换行、多行尽量等分）。
           曲线不叠内存 / 负载第二条线；曲线下方不设标题行；上下文切换 / 中断已移出实时页
           （历史视图两项都可查），避免同一事实两处实现。 -->
      <section class="ds-card monitor-view__card monitor-view__card--wide" data-test="monitor-cpu">
        <div class="monitor-view__block-head">
          <h2 class="ds-card__title monitor-view__card-title">
            <span class="monitor-view__card-icon"><Icon name="CPU" :size="14" /></span>CPU
          </h2>
          <p
            v-if="cpuModel"
            class="monitor-view__cpu-model"
            data-test="monitor-cpu-model"
            :title="cpuModel"
          >{{ cpuModel }}</p>
        </div>
        <div class="monitor-view__cpu">
          <div class="monitor-view__stats">
            <!-- 占用率：主数值可点跳历史（#127）；颜色是按等级的冗余信号（#435 决策 7），
                 数字本身始终可读，未知值不套色。 -->
            <div class="monitor-view__stat">
              <p class="monitor-view__stat-label">总占用</p>
              <template v-if="cpuRatio !== null">
                <button
                  type="button"
                  class="monitor-view__metric monitor-view__value-btn monitor-view__cpu-usage"
                  :data-level="cpuUsageLevel"
                  data-test="monitor-cpu-value"
                  :title="historyTitle('cpu.usage_ratio')"
                  :aria-label="historyAriaLabel('cpu.usage_ratio', cpuPercent)"
                  @click="openHistory('cpu.usage_ratio')"
                >{{ cpuPercent }}</button>
                <p class="monitor-view__stat-context" data-test="monitor-cpu-context">{{ coresContextText }}</p>
              </template>
              <div v-else class="ds-empty monitor-view__empty" data-test="monitor-cpu-unavailable">
                <div class="ds-empty__title">{{ UNAVAILABLE_TEXT }}</div>
                <div class="ds-empty__desc">{{ cpuEmptyDesc }}</div>
              </div>
            </div>

            <!-- 系统负载：语义由 semantic 字段 + semantic_hints 承载，不与另一口径同轴比较。
                 #435 起不再带迷你趋势（用户拍板：负载只保留文字）。 -->
            <div class="monitor-view__stat" data-test="monitor-load">
              <p class="monitor-view__stat-label">负载</p>
              <template v-if="loadAverage">
                <div v-if="loadIsProcessorQueue" class="monitor-view__metric">
                  <button
                    type="button"
                    class="monitor-view__value-btn"
                    data-test="monitor-load-queue"
                    :title="historyTitle('cpu.load_average.one')"
                    :aria-label="historyAriaLabel('cpu.load_average.one', formatDecimal(loadAverage.one))"
                    @click="openHistory('cpu.load_average.one')"
                  >{{ formatDecimal(loadAverage.one) }}</button>
                </div>
                <div v-else class="monitor-view__metric">
                  <button
                    type="button"
                    class="monitor-view__value-btn"
                    data-test="monitor-load-one"
                    :title="historyTitle('cpu.load_average.one')"
                    :aria-label="historyAriaLabel('cpu.load_average.one', formatDecimal(loadAverage.one))"
                    @click="openHistory('cpu.load_average.one')"
                  >{{ formatDecimal(loadAverage.one) }}</button>
                </div>
                <!-- 小字上下文：处理器队列口径不冒充 1/5/15 分钟（文本里也不出现「1 分钟」） -->
                <p class="monitor-view__stat-context" data-test="monitor-load-context">
                  <template v-if="loadIsProcessorQueue">处理器队列长度</template>
                  <template v-else>5 分钟 <span data-test="monitor-load-five">{{ formatDecimal(loadAverage.five) }}</span> · 15 分钟 <span data-test="monitor-load-fifteen">{{ formatDecimal(loadAverage.fifteen) }}</span></template>
                </p>
                <p v-if="loadHint" class="monitor-view__hint" data-test="monitor-load-hint" :title="loadHint.raw">语义提示：{{ loadHint.text }}</p>
              </template>
              <div v-else class="ds-empty monitor-view__empty" data-test="monitor-load-unavailable">
                <div class="ds-empty__title">{{ UNAVAILABLE_TEXT }}</div>
                <div class="ds-empty__desc">{{ loadEmptyDesc }}</div>
              </div>
            </div>

            <!-- 当前频率（#73 / #435 决策 3）：主值 = 全部有读数核的平均；
                 cpu.frequency 不可用时整格不渲染（不是显示 0）。 -->
            <div v-if="frequencyAvailable" class="monitor-view__stat" data-test="monitor-frequency">
              <p class="monitor-view__stat-label">频率</p>
              <template v-if="coreFrequencies.length > 0">
                <div
                  class="monitor-view__metric monitor-view__metric--with-unit"
                  data-test="monitor-frequency-value"
                >{{ frequencyDisplay.value }} <span class="monitor-view__stat-unit">{{ frequencyDisplay.unit }}</span></div>
              </template>
              <div v-else class="ds-empty monitor-view__empty" data-test="monitor-frequency-empty">
                <div class="ds-empty__title">暂无数据</div>
                <div class="ds-empty__desc">当前快照未提供每核频率</div>
              </div>
            </div>

            <!-- 最高温度（#73）：temperature 不可用时整格不渲染（不是显示 0）；
                 归属摘要跟着主数值走，逐传感器读数收在本格的小入口后面（#127 折叠语义不变）。 -->
            <div
              v-if="temperatureAvailable"
              class="monitor-view__stat"
              data-test="monitor-temperature"
              role="group"
              :aria-label="temperatureAriaLabel"
            >
              <p class="monitor-view__stat-label">温度</p>
              <template v-if="temperatureGroups.length > 0">
                <!-- 主值 = 全部传感器最高温；逐传感器读数在 title / aria（悬停与读屏可达） -->
                <div
                  class="monitor-view__metric"
                  data-test="monitor-temperature-max"
                  :title="temperatureSensorTitle || undefined"
                >
                  {{ formatTemperature(temperaturePeak?.celsius ?? null) }}
                </div>
                <!-- 归属（芯片友好名 + 通道标签）跟着主数值走：只说「45.5 °C」不知道是哪颗芯片 -->
                <p class="monitor-view__stat-context" data-test="monitor-temperature-summary">
                  {{ temperaturePeak?.groupName }} · {{ temperaturePeak?.label }}
                </p>
              </template>
              <div v-else class="ds-empty monitor-view__empty" data-test="monitor-temperature-empty">
                <div class="ds-empty__title">暂无数据</div>
                <div class="ds-empty__desc">当前快照未提供温度读数</div>
              </div>
            </div>
          </div>

          <!-- 固定 0–100% 单曲线（#435 决策 2）：无左侧刻度标签、无「纵轴…%」说明——网格线承担
               尺度感，横轴两端给真实时刻，下方只写时间窗。整块是按钮：点击跳历史并预选指标。 -->
          <div v-if="cpuRatio !== null && usageGeometry" class="monitor-view__trend" data-test="monitor-cpu-trend">
            <button
              type="button"
              class="monitor-view__chart monitor-view__value-btn"
              :title="cpuTrendTitle"
              :aria-label="cpuTrendAriaLabel"
              :data-last-value="usageGeometry.lastValue"
              :data-window-complete="usageWindowComplete"
              :data-window-truncated="usageWindow.truncated"
              :data-window-stale="usageWindow.stale"
              data-test="monitor-sparkline-cpu"
              @click="openHistory('cpu.usage_ratio')"
            >
              <svg
                class="monitor-view__chart-svg"
                viewBox="0 0 100 34"
                preserveAspectRatio="none"
                aria-hidden="true"
                focusable="false"
              >
                <line
                  v-for="y in PERCENT_TREND_GRIDLINE_YS"
                  :key="y"
                  class="monitor-view__chart-gridline"
                  x1="0"
                  x2="100"
                  :y1="y"
                  :y2="y"
                  vector-effect="non-scaling-stroke"
                />
                <polyline class="monitor-view__chart-line" :points="usageGeometry.points" vector-effect="non-scaling-stroke" />
              </svg>
            </button>
            <div class="monitor-view__chart-axis-row" aria-hidden="true">
              <span class="monitor-view__chart-axis-end" data-test="monitor-cpu-trend-start">{{ trendStartText }}</span>
              <span class="monitor-view__chart-axis-end" data-test="monitor-cpu-trend-end">{{ trendEndText }}</span>
            </div>
            <p class="monitor-view__meta monitor-view__chart-caption" data-test="monitor-cpu-trend-caption">
              {{ cpuTrendCaption }}
            </p>
          </div>

          <!-- 每核条形（#435 决策 6）：曲线下方直接渲染、无标题行；列数由容器宽度实测后交给纯函数
               计算（多行时每行尽量等分）。title / 读屏文案：`核 N · X% · Y GHz`（频率不可用省略）；
               填充颜色按占用率等级（70 / 90）与顶栏 CPU 徽章同判据，不再标记「饱和」核。 -->
          <div ref="coreBarsRef" class="monitor-view__core-bars" data-test="monitor-cpu-cores">
            <ul
              v-if="coreBars.length > 0"
              class="monitor-view__cores"
              :style="{ '--core-columns': String(coreBarColumnCount) }"
            >
              <li
                v-for="bar in coreBars"
                :key="bar.index"
                class="monitor-view__core"
                :title="bar.text"
                :aria-label="bar.text"
                data-test="monitor-core"
                :data-core-index="bar.index"
              >
                <span
                  class="monitor-view__core-bar"
                  :class="{ 'monitor-view__core-bar--unavailable': !bar.available }"
                  aria-hidden="true"
                >
                  <span
                    class="monitor-view__core-bar-fill"
                    :data-level="bar.level ?? undefined"
                    :style="bar.style"
                  ></span>
                </span>
              </li>
            </ul>
            <div v-else class="ds-empty monitor-view__empty" data-test="monitor-cores-empty">
              <div class="ds-empty__title">{{ coresEmptyTitle }}</div>
              <div class="ds-empty__desc">{{ coresEmptyDesc }}</div>
            </div>
          </div>

        </div>
      </section>

      <!-- 内存块（#445）：照主机面板插件的内存卡——标题 + 右端总量、已用率 Meter、
           四段组成带 + 图例、交换子 Meter。与插件一致只占半宽（插件那半是 GPU；本机无 GPU，
           右列暂空，后续块再占用）；不做 15 分钟曲线（插件内存卡无图）。 -->
      <section class="ds-card monitor-view__card monitor-view__memory" data-test="monitor-memory">
        <div class="monitor-view__block-head">
          <h2 class="ds-card__title monitor-view__card-title">
            <span class="monitor-view__card-icon"><Icon name="memory" :size="14" /></span>内存
          </h2>
          <p
            v-if="memoryAvailable && memoryInfo"
            class="monitor-view__memory-total"
            data-test="monitor-memory-total"
          >{{ formatBytes(memoryInfo.total_bytes) }}</p>
        </div>

        <template v-if="memoryAvailable && memoryInfo">
          <div class="monitor-view__meter" data-test="monitor-memory-meter">
            <div class="monitor-view__meter-head">
              <span class="monitor-view__meter-label">已用</span>
              <button
                v-if="memoryUsedRatio !== null"
                type="button"
                class="monitor-view__value-btn monitor-view__meter-value"
                :data-level="memoryLevel"
                data-test="monitor-memory-percent"
                :title="historyTitle('memory.used_ratio')"
                :aria-label="historyAriaLabel('memory.used_ratio', formatPercent(memoryUsedRatio))"
                @click="openHistory('memory.used_ratio')"
              >{{ formatPercent(memoryUsedRatio) }}</button>
              <span v-else class="monitor-view__meter-value" data-test="monitor-memory-percent">{{ UNAVAILABLE_TEXT }}</span>
            </div>
            <div class="monitor-view__meter-track" aria-hidden="true">
              <span class="monitor-view__meter-fill" :data-level="memoryLevel" :style="meterFillStyle(memoryUsedRatio)"></span>
            </div>
            <p class="monitor-view__meter-caption" data-test="monitor-memory-caption">
              {{ formatBytes(memoryInfo.used_bytes) }} · 共 {{ formatBytes(memoryInfo.total_bytes) }}
            </p>
          </div>

          <!-- 组成带（#363 口径，迁入内存块）：主数值是「用量比率」，组成带是「总量分解」，
               两者各自标注分母；缓存/缓冲不可得时退化为两段并注记（不补 0）。 -->
          <div v-if="memoryComposition" class="monitor-view__memory-composition" data-test="monitor-memory-composition">
            <div class="monitor-view__memory-band">
              <span
                v-for="segment in memoryComposition.segments"
                :key="segment.id"
                class="monitor-view__memory-band-seg"
                :class="`monitor-view__memory-band-seg--${segment.id}`"
                :style="memorySegmentStyle(segment)"
                :data-segment="segment.id"
              ></span>
            </div>
            <ul class="monitor-view__memory-legend" data-test="monitor-memory-legend">
              <li
                v-for="segment in memoryComposition.segments"
                :key="segment.id"
                class="monitor-view__memory-legend-item"
                :data-segment="segment.id"
                data-test="monitor-memory-legend-item"
              >
                <span class="monitor-view__memory-legend-swatch" :class="`monitor-view__memory-band-seg--${segment.id}`" aria-hidden="true"></span>
                <span class="monitor-view__memory-legend-label">{{ segment.label }}</span>
                <span class="monitor-view__memory-legend-value" data-test="monitor-memory-legend-value">{{ segment.text ?? '不可得' }}</span>
              </li>
            </ul>
            <p
              v-if="memoryComposition.degraded"
              class="monitor-view__hint"
              data-test="monitor-memory-composition-degraded"
            >{{ memoryComposition.degradedText }}</p>
          </div>

        </template>
        <div v-else class="ds-empty monitor-view__empty" data-test="monitor-memory-empty">
          <div class="ds-empty__title">{{ memoryEmptyTitle }}</div>
          <div class="ds-empty__desc">{{ memoryEmptyDesc }}</div>
        </div>

        <!-- 交换子 Meter（插件式）：有交换分区才画条；无交换/关闭/不可用/暂无数据只给一句文案。
             交换可用性独立于内存：内存空态不影响交换读数。 -->
        <div v-if="swapHasData" class="monitor-view__meter monitor-view__meter--compact" data-test="monitor-swap">
            <div class="monitor-view__meter-head">
              <span class="monitor-view__meter-label">交换分区</span>
              <button
                v-if="swapUsedRatio !== null"
                type="button"
                class="monitor-view__value-btn monitor-view__meter-value"
                :data-level="swapLevel"
                data-test="monitor-swap-percent"
                :title="historyTitle('swap.used_ratio')"
                :aria-label="historyAriaLabel('swap.used_ratio', formatPercent(swapUsedRatio))"
                @click="openHistory('swap.used_ratio')"
              >{{ formatPercent(swapUsedRatio) }}</button>
              <span v-else class="monitor-view__meter-value" data-test="monitor-swap-percent">{{ UNAVAILABLE_TEXT }}</span>
            </div>
            <div class="monitor-view__meter-track monitor-view__meter-track--compact" aria-hidden="true">
              <span class="monitor-view__meter-fill" :data-level="swapLevel" :style="meterFillStyle(swapUsedRatio)"></span>
            </div>
            <p class="monitor-view__meter-caption" data-test="monitor-swap-caption">
              {{ formatBytes(swapInfo!.used_bytes) }} · 共 {{ formatBytes(swapInfo!.total_bytes) }}
            </p>
        </div>
        <p v-else class="monitor-view__memory-muted" data-test="monitor-swap-empty">{{ swapEmptyText }}</p>
      </section>

      <!-- 指标台账（#363；#435 起 CPU 组移入 CPU 块，只剩三组 内存 → 存储 → 网络与时钟）、每行七列，行数据全部来自 `buildMonitorLedger`。
           组件不写任何「该不该显示」的判断——行的存在、空态类别、阈值与余量、能否点跳历史
           全部由 `@/composables/monitorLedger` 决定。七列共用一条列基线（每行同一套
           `grid-template-columns`），因此任意一列可纵向读下来。 -->
      <section class="ds-card monitor-view__card monitor-view__card--wide" data-test="monitor-ledger">
        <div
          v-for="group in ledger"
          :key="group.id"
          class="monitor-view__ledger-group"
          :data-group="group.id"
          :data-test="`monitor-ledger-group-${group.id}`"
        >
          <div class="monitor-view__ledger-group-head">
            <h2 class="monitor-view__ledger-group-title">{{ group.title }}</h2>
            <p class="monitor-view__ledger-group-note">{{ group.note }}</p>
            <!-- 组内操作：网络组的「显示全部网卡」开关 + 时钟组的「立即查询」都随各自的组走，
                 不另立卡片。两者都是 admin 才可操作，非 admin 渲染为 disabled（可见但不可点，
                 避免隐藏造成「功能不存在」的误解）。 -->
            <div v-if="group.id === 'network_clock'" class="monitor-view__ledger-group-actions">
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
              <div class="monitor-view__clock-probe">
                <button
                  class="ds-btn ds-btn--secondary ds-btn--sm"
                  type="button"
                  data-test="monitor-clock-probe"
                  :disabled="clockProbeDisabled"
                  :title="canProbeClock ? '立即触发一轮时钟探测（结果随快照更新）' : '仅管理员可手动查询'"
                  @click="onProbeClock"
                ><Icon name="clock_time" :size="12" />{{ clockProbeLabel }}</button>
              </div>
            </div>
          </div>

          <ul class="monitor-view__ledger-rows">
            <li
              v-for="row in group.rows"
              :key="row.id"
              class="monitor-view__ledger-row"
              :class="{
                'monitor-view__ledger-row--empty': row.state !== 'ok',
                'monitor-view__ledger-row--open': isLedgerRowOpen(row.id),
              }"
              :data-row-id="row.id"
              :data-state="row.state"
              :data-level="row.level"
              :data-test="rowTestId(row)"
              :data-mount="row.id.startsWith('volume:') ? rowDimension(row) : undefined"
              :data-device="row.id.startsWith('disk:') ? rowDimension(row) : undefined"
              :data-interface="row.id.startsWith('nic:') ? rowDimension(row).split(':')[0] : undefined"
              :data-direction="row.id.startsWith('nic:') ? rowDimension(row).split(':')[1] : undefined"
            >
              <!-- ① 状态点 + 等级文字：状态不得只靠颜色（灰度截屏下判断不变） -->
              <span class="monitor-view__ledger-state">
                <span class="monitor-view__ledger-dot" aria-hidden="true"></span>
                <span class="monitor-view__ledger-state-text">{{ rowStateTag(row) }}</span>
              </span>

              <!-- ② 指标名 + 契约键 -->
              <span class="monitor-view__ledger-name">
                <span class="monitor-view__ledger-label" :title="row.label">{{ row.label }}</span>
                <span class="monitor-view__ledger-key">
                  {{ row.contractKey }}
                  <span
                    v-for="badge in row.badges"
                    :key="badge"
                    class="ds-tag monitor-view__ledger-badge"
                    data-test="monitor-network-virtual"
                  >{{ badge }}</span>
                </span>
              </span>

              <!-- ③ 量程轨道：填充比例由 `--ledger-fill` 注入（与 CPU 每核柱同一形态）。
                   缺口只在**有契约阈值**的行上出现，位置取 `useResourceLevel` 的两个展示分档常量
                   （70 / 90）：那是展示规则（ADR 0034），与「阈值」列是两个不同的东西。 -->
              <!-- 无量程的行不画轨道：`ratio === null` 表示这个指标**没有可归一化的分母**
                   （网卡没有链路速率字段、共享内存目录没有总容量）。画一条永远空着的轨道，
                   读者会把它读成「0%」——那是一个凭空造出来的读数。空槽位只占位，不画任何东西。 -->
              <span v-if="row.ratio !== null" class="monitor-view__ledger-track" aria-hidden="true">
                <span class="monitor-view__ledger-track-fill" :style="trackFillStyle(row)"></span>
                <template v-if="hasThreshold(row)">
                  <span class="monitor-view__ledger-gap" :style="gapStyle(RESOURCE_WARN_RATIO)"></span>
                  <span class="monitor-view__ledger-gap" :style="gapStyle(RESOURCE_DANGER_RATIO)"></span>
                </template>
              </span>
              <span v-else class="monitor-view__ledger-track-empty" aria-hidden="true"></span>

              <!-- ④ 当前值：有 historyKey 的行是按钮（跳历史视图并预选该指标，#325 的 title/aria 口径）；
                   空态行的值列是「—」——**不是 0**，四类空态的文字在明细列与状态点旁给出。 -->
              <!-- 等级同时写在值槽上（行级 data-level 之外）：数值自身的着色锚点是这一个格
                    （等级为 null 时属性被省略，不套任何状态色）。 -->
              <span class="monitor-view__ledger-value" :data-level="row.level">
                <button
                  v-if="row.state === 'ok' && row.historyKey !== null"
                  type="button"
                  class="monitor-view__value-btn"
                  :data-test="rowValueTestId(row)"
                  :title="rowHistoryTitle(row)"
                  :aria-label="rowHistoryAriaLabel(row)"
                  @click="openRowHistory(row)"
                >{{ row.value }}</button>
                <!-- 非可点读数也带同一个钩子（`ok` 但没有历史入口 / `unavailable`）：值槽是「读到的
                     当前值」这一事实的锚点，空态槽（empty/disabled/none）不提供它——那里没有读数。 -->
                <span
                  v-else-if="rowHasReading(row)"
                  :data-test="rowValueTestId(row)"
                >{{ rowValueText(row) }}</span>
                <template v-else>{{ rowValueText(row) }}</template>
                <!-- 标量行的内联趋势（#127）：与当前值同源同刷新时刻（末点 == 当前值），点击跳历史。
                     它挂在**值列内部**而不是新开一列——七列是这一页的结构承诺，趋势是读数的一部分。
                     维度行（卷 / 盘 / 网卡）没有归一化分母，不做趋势。 -->
                <SysmonSparkline
                  v-if="row.historyKey !== null && rowSparklinePoints(row).length > 0"
                  :points="rowSparklinePoints(row)"
                  :label="trendMetricLabel(row.historyKey)"
                  :window="trend.windowOf(row.historyKey)"
                  class="monitor-view__sparkline monitor-view__sparkline--ledger"
                  :data-test="`monitor-sparkline-${rowSparklineTestSuffix(row)}`"
                  @open="openRowHistory(row)"
                />
              </span>

              <!-- ⑤ 明细：`ok` 行是一行明细；空态行是四类空态的说明（互不混淆，都不产出数字） -->
              <span class="monitor-view__ledger-detail">
                <span class="monitor-view__ledger-detail-text" :data-test="row.state === 'ok' ? 'ledger-detail' : 'row-state'">
                  <template v-if="row.state === 'ok'">
                    <!-- 有分段形态的行（网卡：错误 / 丢弃各自可寻址）逐段渲染；其余行一句话。
                         分段之间只用列间距分隔，不插入「·」标点：标点会进可读文本，
                         让同一事实在无障碍朗读里多读一个符号。 -->
                    <span
                      v-for="part in row.detailParts"
                      :key="part.id"
                      class="monitor-view__ledger-detail-part"
                      :data-test="`monitor-network-${directionTestId(row)}-${part.id}`"
                    >{{ part.text }}</span>
                    <template v-if="row.detailParts.length === 0">{{ row.detail }}</template>
                  </template>
                  <template v-else>{{ rowStateNote(row) }}</template>
                </span>
                <!-- 明细展开按钮挂在明细列内（不另占一整行）：语义上它是「这段话还有下文」，
                     不是第八列，也不该把每行的行高翻倍。时钟行的按钮在它自己的结构化明细里。 -->
                <button
                  v-if="row.fields.length > 0 && row.id !== 'clock'"
                  class="ds-btn ds-btn--tertiary ds-btn--sm monitor-view__ledger-expand-btn"
                  type="button"
                  :aria-expanded="isLedgerRowOpen(row.id)"
                  :aria-label="`${isLedgerRowOpen(row.id) ? '收起' : '明细'} · ${row.label}`"
                  :aria-controls="isLedgerRowOpen(row.id) ? `ledger-fields-${row.id}` : undefined"
                  :data-test="`ledger-expand-${row.id}`"
                  @click="toggleLedgerRow(row.id)"
                >{{ isLedgerRowOpen(row.id) ? '收起' : '明细' }}</button>
              </span>

              <!-- ⑥ 阈值（只写契约里真实存在的阈值；没有就是 —）。列名只在窄屏出现（见 CSS），
                   且列名**不进** data-test 元素：那是给「值」的锚点，不是给标签的。
                   列名**不能** `aria-hidden`（#363 audit A3）：<1024 时它是「10% 空闲」唯一的归属说明，
                   藏掉之后读屏只剩一个没有主语的数字。≥1024 它是 `display: none`，本就不进无障碍树，
                   所以宽屏不会重复朗读。 -->
              <span class="monitor-view__ledger-threshold">
                <span class="monitor-view__ledger-col-label">阈值</span>
                <span data-test="ledger-threshold">{{ row.thresholdText }}</span>
              </span>

              <!-- ⑦ 余量（与阈值同源，越线时写「已超 X」） -->
              <span class="monitor-view__ledger-headroom">
                <span class="monitor-view__ledger-col-label">余量</span>
                <span data-test="ledger-headroom">{{ row.headroomText }}</span>
              </span>

              <!-- 行明细（#363）：按钮在上面的明细列里（「明细」/「收起」），展开后整行宽度铺开
                   完整字段表——按钮在格内、内容跨满行，因此行高只在真正展开时才长高。
                   折叠状态不持久化，刷新回默认收起（空态行也允许展开：逐台失败原因等可诊断信息
                   藏在展开里是可接受的，但它不是默认视图）。 -->
              <dl
                v-if="row.fields.length > 0 && row.id !== 'clock' && isLedgerRowOpen(row.id)"
                :id="`ledger-fields-${row.id}`"
                class="monitor-view__ledger-fields"
                data-test="ledger-fields"
              >
                <div
                  v-for="field in row.fields"
                  :key="field.label"
                  class="monitor-view__ledger-field"
                  data-test="ledger-field"
                  :data-label="field.label"
                >
                  <dt class="monitor-view__ledger-field-label">{{ field.label }}</dt>
                  <dd class="monitor-view__ledger-field-value" data-test="ledger-field-value">{{ field.value }}</dd>
                </div>
              </dl>

              <!-- 时钟行的辅助信息（#302）：方向说明 / 测量质量提示 / 同步服务检测。
                   偏差着色与质量提示是两个独立判定对象（契约 §2.8）：质量提示不改变着色。 -->
              <div
                v-if="row.id === 'clock'"
                class="monitor-view__ledger-clock"
                data-test="monitor-clock-aux"
              >
                <!-- 三空态各给一句彼此不同的文案（契约 §2.9）：尚未探测 ≠ 全部失败 ≠ 配置关闭。
                     三者都是「没有偏差读数」，但只有「全部失败」是不可用；混用会让用户去查错方向。 -->
                <div v-if="clockState === 'unprobed'" class="ds-empty monitor-view__empty" data-test="monitor-clock-unprobed">
                  <div class="ds-empty__title">{{ CLOCK_UNPROBED_TITLE }}</div>
                  <div class="ds-empty__desc">{{ CLOCK_UNPROBED_DESC }}</div>
                </div>
                <div v-else-if="clockState === 'disabled'" class="ds-empty monitor-view__empty" data-test="monitor-clock-disabled">
                  <div class="ds-empty__title">{{ CLOCK_DISABLED_TITLE }}</div>
                  <div class="ds-empty__desc">{{ CLOCK_DISABLED_DESC }}</div>
                </div>
                <p
                  v-else-if="clockState === 'unavailable'"
                  class="monitor-view__hint"
                  data-test="monitor-clock-unavailable"
                >
                  {{ UNAVAILABLE_TEXT }} · {{ clockFailure }}
                  <template v-if="clockProbeAge"> · 上次探测 {{ clockProbeAge }}前</template>
                </p>
                <p
                  v-if="clockState === 'live'"
                  class="monitor-view__meta"
                  data-test="monitor-clock-summary"
                >
                  误差界 {{ formatMilliseconds(clock?.bound_ms ?? null) }}
                  · 来源 {{ clock?.source }}（stratum {{ clock?.source_stratum ?? UNAVAILABLE_TEXT }}）
                  <template v-if="clockProbeAge"> · 上次探测 {{ clockProbeAge }}前</template>
                  · 正 = 本地快于参考源
                </p>
                <p
                  v-if="clockQualityAlarm"
                  class="monitor-view__hint monitor-view__hint--alert"
                  data-test="monitor-clock-quality"
                >{{ CLOCK_QUALITY_HINT }}（阈值 {{ clockThresholds.bound_alarm_ms }} ms）</p>
                <!-- 每台服务器的明细（契约 §2.9）：六列逐台给出，失败台的四项测量显示「不可用」
                     而不是 0（失败样本不产出任何测量值）。开关是本行自己的「展开明细」。
                     这里不复用通用字段列表：服务器是**一张小表**（每台一行、六列对齐），
                     摊成「标签 / 值」会丢掉横向可比性。 -->
                <div v-if="clockServers.length > 0" class="monitor-view__ledger-clock-detail">
                  <button
                    class="ds-btn ds-btn--tertiary ds-btn--sm"
                    type="button"
                    :aria-expanded="isLedgerRowOpen(row.id)"
                    :aria-label="`${isLedgerRowOpen(row.id) ? '收起明细' : '展开服务器明细'} · ${row.label}`"
                    :aria-controls="isLedgerRowOpen(row.id) ? `ledger-clock-servers-${row.id}` : undefined"
                    data-test="monitor-clock-detail-toggle"
                    @click="toggleLedgerRow(row.id)"
                  >{{ isLedgerRowOpen(row.id) ? '收起明细' : '展开服务器明细' }}</button>
                  <ul
                    v-if="isLedgerRowOpen(row.id)"
                    :id="`ledger-clock-servers-${row.id}`"
                    class="monitor-view__clock-servers"
                  >
                    <li
                      v-for="server in clockServers"
                      :key="server.host"
                      class="monitor-view__clock-server"
                      data-test="monitor-clock-server"
                      :data-host="server.host"
                      :data-ok="server.ok"
                      :title="server.host"
                    >
                      <div class="monitor-view__io-device">
                        {{ server.host }}
                        <span class="ds-tag" :class="server.ok ? 'ds-tag--success' : 'ds-tag--danger'">
                          {{ server.ok ? '成功' : '失败' }}
                        </span>
                      </div>
                      <dl class="monitor-view__io-metrics">
                        <div
                          v-for="cell in clockServerCells(server)"
                          :key="cell.key"
                          class="monitor-view__io-metric"
                        >
                          <dt class="monitor-view__io-label">{{ cell.label }}</dt>
                          <dd
                            class="monitor-view__io-value"
                            :data-test="`monitor-clock-server-${cell.key}`"
                          >{{ cell.value }}</dd>
                        </div>
                      </dl>
                    </li>
                  </ul>
                </div>

                <!-- 同步服务检测（契约 §2.9 daemons[]）：空数组是「未检测到在运行的服务」这一事实，
                     不是能力不可用（§2.5）。尚未探测时不渲染：首轮探测前 daemons 是空数组，
                     把空数组当事实展示会凭空报出一个未做过的检测。 -->
                <div v-if="clockDaemonVisible" class="monitor-view__ledger-daemons" data-test="monitor-clock-daemons">
                  <ul v-if="clockDaemons.length > 0" class="monitor-view__list">
                    <li
                      v-for="daemon in clockDaemons"
                      :key="daemon.name"
                      class="monitor-view__list-row"
                      data-test="monitor-clock-daemon"
                      :data-name="daemon.name"
                    >
                      <span class="monitor-view__list-label">{{ daemon.name }}</span>
                      <span
                        class="monitor-view__list-value"
                        :class="daemon.running ? 'monitor-view__daemon--running' : 'monitor-view__daemon--stopped'"
                      >{{ daemon.running ? '运行中' : '未运行' }}</span>
                    </li>
                  </ul>
                  <p
                    v-else
                    class="monitor-view__hint monitor-view__hint--alert"
                    data-test="monitor-clock-daemon-none"
                  >{{ CLOCK_NO_DAEMON_TEXT }} · {{ CLOCK_NO_DAEMON_DESC }}</p>
                  <!-- 多服务同时运行：行内提示（契约 §2.9 明文「只写一条日志 + 卡片提示，不弹窗、不判故障」） -->
                  <p
                    v-if="clock?.daemon_conflict"
                    class="monitor-view__hint monitor-view__hint--alert"
                    data-test="monitor-clock-daemon-conflict"
                  >{{ CLOCK_DAEMON_CONFLICT_TEXT }}</p>
                </div>
              </div>
            </li>
          </ul>

          <!-- 存储组尾部：无容量挂载（#122）与磁盘 I/O 语义提示（#207 B2）。
               都不进台账主列——前者不是「容量可读的卷」，后者是口径说明而非一行指标。 -->
          <template v-if="group.id === 'storage'">
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
                  <span class="monitor-view__list-label monitor-view__list-label--mount" :title="volume.mount">{{ volume.mount }}</span>
                  <!-- 读不到容量与「总量确实为 0」是两件事，原因列分别说明，都不用 0 冒充 -->
                  <span class="monitor-view__list-value">{{ volume.available ? '—' : UNAVAILABLE_TEXT }}</span>
                  <span class="monitor-view__list-meta" data-test="monitor-disk-zero-reason">
                    {{ volume.available ? '总容量为 0' : volumeReasonDesc(volume.reason) }}
                  </span>
                </li>
              </ul>
            </div>
            <p v-if="diskIoHint" class="monitor-view__hint" data-test="monitor-disk-io-hint" :title="diskIoHint.raw">语义提示：{{ diskIoHint.text }}</p>
          </template>
        </div>
      </section>
    </div>

    <!-- 离开前的草稿确认（#322）：只在有未保存改动时出现；两个分支都是显式动作，
         遮罩点击与 Esc 等同「留下」（不丢草稿）。 -->
    <Modal :open="leaveConfirmOpen" title="有未保存的改动" @close="cancelLeave">
      <p class="monitor-view__leave-text">
        配置项有未保存的修改，离开本页后这些改动会丢失。
      </p>
      <template #footer>
        <button class="ds-btn ds-btn--secondary" type="button" data-test="monitor-leave-cancel" @click="cancelLeave">留下</button>
        <button class="ds-btn ds-btn--primary" type="button" data-test="monitor-leave-confirm" @click="confirmLeave">离开</button>
      </template>
    </Modal>
  </div>
</template>

<style scoped>
.monitor-view {
  flex: 1;
  min-height: 0;
  overflow: auto;
  padding: var(--spacer-32) var(--spacer-24);
}

/* 顶栏（#328）：默认单行「标题组 + 视图切换」，只使用宽度断点（640/768/1024/1280 白名单）。
   <md 时 topbar 退化为 display:contents，视图切换行改由下方 max-width 块单独吸顶。 */
.monitor-view__topbar {
  display: flex;
  align-items: flex-start;
  justify-content: space-between;
  flex-wrap: nowrap;
  gap: var(--spacer-16);
  margin-bottom: var(--spacer-24);
}

/* 实时视图且有主机条时，页头与主机条读成一组（间距 12 而不是 24）；
   历史 / 配置视图与「等待快照」空态没有主机条，保持原有 24 间距（#430）。 */
.monitor-view__topbar--host-follows {
  margin-bottom: var(--spacer-12);
}

/* 标题组可收缩（#328）：中等宽度下让副标题走省略号，而不是把 Tab 顶到下一行。 */
.monitor-view__header {
  min-width: 0;
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

/* 离开确认对话框正文（#322）：与其它对话框正文同档字号，不引入新范式 */
.monitor-view__leave-text {
  margin: 0;
  font-size: var(--body-sm-font-size);
  line-height: var(--body-sm-line-height);
  color: var(--text-secondary);
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

/* 中等宽度页头紧凑化（#328）：768–1279 在单行页头前提下收紧副标题——nowrap + 省略号，
   不再把 Tab 顶到下一行。单行本身由 topbar 基础规则（flex-wrap: nowrap）保证；
   可收缩的是作为 flex item 的 `.monitor-view__header`（其 min-width: 0 在基础规则里，
   `.monitor-view__heading` 是普通块级元素，设 min-width 不会生效）。只用宽度断点。 */
@media (min-width: 768px) and (max-width: 1279.98px) {
  .monitor-view__subtitle {
    overflow: hidden;
    white-space: nowrap;
    text-overflow: ellipsis;
  }
}

/* 实时视图的网格是单列纵向（#363）：CPU 块 / 指标台账跨满整行（主机条在网格之外，
   是页头下方的通栏一条，#430）。列数不再是版面变量——台账自身的七列（≥1024）/ 三行堆叠
   （<1024）才是分段的那一层。
   沿用既有断点体系（xl=1280，#127 明确不使用容器查询），图表用容器自适应重排。
   卡片高度按内容：grid 默认 `align-items: stretch` 会把同一行的卡片拉到最高者高度，
   造成大片卡内留白（实测温度 326px / 内存 169px / 网络 140px，1280 下网络还有 140px；#325）。 */
.monitor-view__grid {
  display: grid;
  align-items: start;
  grid-template-columns: minmax(0, 1fr);
  gap: var(--spacer-16);
}

/* #445：≥lg 两列——宽块（CPU / 台账）跨满整行，半宽块（内存，后续还有别的）各占一列。
   与主机面板插件一致（它也是两列网格 + CPU/磁盘通栏，内存与 GPU 各半宽）；
   <lg 单列，内存卡自然变全宽。 */
@media (min-width: 1024px) {
  .monitor-view__grid {
    grid-template-columns: repeat(2, minmax(0, 1fr));
    column-gap: var(--spacer-10);
  }
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

/* ---- 主机条（#430）：页头下方的一条通栏信息条（主机 / 系统 / 内核 / 已运行 + 右端状态槽）----
   行为照抄主机面板插件（`@local/dsh-host-panel` 的 HostStrip）：容器允许**整体换行**，但每条
   内容**不换行**（值 overflow + ellipsis + title），标签与值分色，spacer 吃掉剩余宽度使状态槽
   恒在**所在那一行**的最右（换到第二行也贴右缘）。字段数量与顺序固定，窄屏不隐藏字段。
   与页头同属页面 chrome：上间距 12、下间距 24，让「标题行 + 主机条」读成一组。 */
.monitor-view__host {
  display: flex;
  align-items: center;
  flex-wrap: wrap;
  gap: var(--spacer-4) var(--spacer-12);
  min-width: 0;
  margin-bottom: var(--spacer-24);
  padding: var(--spacer-8) var(--spacer-12);
  border: 1px solid var(--border-neutral-l1);
  border-radius: var(--radius-8);
  background: var(--bg-base-secondary);
  font-size: var(--body-xs-font-size);
  line-height: var(--body-xs-line-height);
}

/* 单条字段：**标签 + 值**成一个不可拆的整体（基线对齐），宽度不足时整条换行而不是把字拆开。
   min-width: 0 允许这条作为 flex item 收缩，值的省略号才会真的出现。 */
.monitor-view__host-item {
  display: inline-flex;
  align-items: baseline;
  gap: var(--spacer-4);
  min-width: 0;
}

/* 标签次级色、值主色：名字与值在灰度 / 色觉障碍下也能分开（主机面板同一做法）。
   label 用 secondary 而不是 tertiary：11px 且承载信息，tertiary 在两套主题下都够不到 4.5:1。 */
.monitor-view__host-label {
  flex: none;
  color: var(--text-secondary);
}

.monitor-view__host-value {
  min-width: 0;
  overflow: hidden;
  color: var(--text-default);
  font-weight: var(--body-xs-strong-font-weight);
  text-overflow: ellipsis;
  white-space: nowrap;
}

/* spacer 吃掉剩余宽度：状态槽因此永远贴在它所在那一行的右缘（换行后同样成立） */
.monitor-view__host-spacer {
  flex: 1 1 auto;
}

/* 右端状态槽：正常「更新于 HH:MM:SS」，停推「快照已停 N 秒」（数据龄并入，不再单起一行）。
   文字保持中性色（#371：承载数据龄这一事实）；着色只给脉冲点（#432）。
   tabular-nums 让停推时每秒变化的秒数不左右跳。 */
.monitor-view__host-status {
  flex: none;
  color: var(--text-secondary);
  font-variant-numeric: tabular-nums;
}

/* 停推：状态槽只加一档字重，不加色相 / 背景（Paired-Step Rule：色族只给状态，
   这里给的是「数据有多旧」，不是进程状态）。 */
.monitor-view__host--stale .monitor-view__host-status {
  font-weight: var(--body-xs-strong-font-weight);
}

/* 脉冲点：发送方是否仍在推进数据（辅助信号——同一事实由状态槽文案承担，故 aria-hidden）。
   #432（2026-11-22 用户拍板「绿黄」）：数据在流动 = success 绿，停推 = warning 黄。
   #371 的「存活信号一律中性色」是旧方向口径，由本票的拍板记录反转（不是 #429 重定——#429 只定义了
   布局方向）。DESIGN.md 的 One Job Rule 把状态色定义为 process state（DESIGN.md:225），
   Status hues 段（:214）将其展开为「进程与连接状态」；「数据在流动」属于该事实。
   它与台账的**指标绿**同族不同义（存活绿 = 数据在推，指标绿 = 读数健康），语义不得互换，
   见 DESIGN.md「Liveness signals」一句与 GLOSSARY.md「存活信号」词条。
   已知限制（#434）：success/warning 的 `-default` 6px 图形在浅色卡片底上只有 2.79 / 2.76:1，
   低于 3:1——与台账 normal 点同一既存口径；两处信息都不依赖颜色，属冗余图形信号，
   是否引入图形专用 token 由 #434 决定。 */
.monitor-view__host-dot {
  flex-shrink: 0;
  width: 6px;
  height: 6px;
  border-radius: var(--radius-full);
  background: var(--status-success-default);
  animation: monitor-view-host-pulse 2s ease-in-out infinite;
}

@keyframes monitor-view-host-pulse {
  0%, 100% { opacity: 1; }
  50% { opacity: 0.4; }
}

/* 停推时脉冲点停止脉动并转 warning 黄：它代表「在推进」，数据不再推进时继续跳就是在撒谎；
   颜色变化是冗余信号——停推事实本身由状态槽的「快照已停 N 秒」承担（不靠色相才可读）。 */
.monitor-view__host--stale .monitor-view__host-dot {
  background: var(--status-warning-default);
  animation: none;
}

/* 尊重「减少动态效果」：脉冲是装饰，关掉不影响任何信息（点本身仍在） */
@media (prefers-reduced-motion: reduce) {
  .monitor-view__host-dot {
    animation: none;
  }
}

/* ---- CPU 标题行（#435）：标题左、型号右（型号过长省略号 + title，不换行撑破标题行） ---- */
.monitor-view__block-head {
  display: flex;
  align-items: baseline;
  justify-content: space-between;
  gap: var(--spacer-8) var(--spacer-16);
  min-width: 0;
}

/* 卡片标题是行内 flex，作为 flex item 不参与收缩；型号吃掉剩余宽度 */
.monitor-view__block-head .monitor-view__card-title {
  flex: none;
}

/* 标题字号对齐参照（主机面板 13px）：CPU 块是「仪器读数组」，标题不该压过四数字 */
.monitor-view__block-head .monitor-view__card-title {
  font-size: var(--heading-2xs-font-size);
  line-height: var(--heading-2xs-line-height);
  font-weight: var(--heading-2xs-font-weight);
}

/* 型号来自 system.cpu_model：取不到时整行不渲染（v-if），不显示「未知型号」占位 */
.monitor-view__cpu-model {
  min-width: 0;
  margin: 0;
  overflow: hidden;
  color: var(--text-secondary);
  font-size: var(--body-xs-font-size);
  line-height: var(--body-xs-line-height);
  text-overflow: ellipsis;
  white-space: nowrap;
}

/* ---- CPU 四数字（#435）：占用率 / 系统负载 / 当前频率 / 最高温度横排 ----
   容器自适应（auto-fit）而不是固定 4 列：窄屏两列、宽屏四列，一条规则覆盖，不写断点。
   Windows 上频率 / 温度整格不渲染，同一规则自然收成两列。 */
.monitor-view__stats {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(var(--stat-cell-min-width), 1fr));
  gap: var(--spacer-8);
  grid-column: 1 / -1;
}

.monitor-view__stat {
  display: flex;
  flex-direction: column;
  gap: var(--spacer-2);
  min-width: 0;
}

.monitor-view__stat-label {
  margin: 0;
  color: var(--text-secondary);
  font-size: var(--body-xs-font-size);
  line-height: var(--body-xs-line-height);
  font-weight: var(--body-xs-font-weight);
}

.monitor-view__stat-context {
  margin: 0;
  color: var(--text-secondary);
  font-size: var(--body-xs-font-size);
  line-height: var(--body-xs-line-height);
}

/* 占用率主数值按等级着色（#435 决策 7）：与顶栏 CPU 徽章同一展示分档（70 / 90，定义只在
   `useResourceLevel`）。文字必须走 `-on-surface` 文本档：`-default` 图形档在浅色主题的卡片底上
   只有 2.79:1，大字号文本也不够 3:1（Paired-Step Rule）。数字本身始终可读，颜色是冗余信号。 */
.monitor-view__value-btn.monitor-view__cpu-usage[data-level='normal'] {
  color: var(--status-success-on-surface);
}

.monitor-view__value-btn.monitor-view__cpu-usage[data-level='warn'] {
  color: var(--status-warning-on-surface);
}

.monitor-view__value-btn.monitor-view__cpu-usage[data-level='danger'] {
  color: var(--status-error-on-surface);
}

/* ---- 趋势（#435）：固定 0–100% 的单序列折线，整块是按钮 ----
   纵轴固定后不再有左侧刻度列（#435 决策 2）：绘图区占满卡片宽度，网格线（0/25/50/75/100）
   承担尺度感，横轴两端给真实时刻，下方说明只写时间窗。高度是图表几何量，走 `--chart-height-trend`。 */
.monitor-view__trend {
  grid-column: 1 / -1;
  min-width: 0;
}

/* 绘图区底色比卡片略深/略浅一档（主机面板同款）：曲线与网格线要和页面底色分层，
   否则固定 0–100% 轴的低位曲线会糊在卡片背景里。 */
.monitor-view__chart {
  display: block;
  width: 100%;
  height: var(--chart-height-trend);
  padding: 0;
  border: none;
  border-radius: var(--radius-6);
  background: var(--bg-overlay-l1);
  cursor: pointer;
  overflow: hidden;
}

.monitor-view__chart-svg {
  display: block;
  width: 100%;
  height: 100%;
  overflow: visible;
}

/* 网格线：颜色走图表轴 token；`vector-effect` 在非等比拉伸下保持 1px 线宽。
   8080 验收反馈「格子线再弱一点」→ 叠 0.6 不透明度（主机面板同款做法），不新造颜色 token。
   仍保留 `viz-ui-chart-axis` 语义（换轴色时一处生效）。 */
.monitor-view__chart-gridline {
  stroke: var(--viz-ui-chart-axis);
  stroke-width: 1;
  opacity: 0.6;
}

/* forced-colors（Windows 高对比度）：网格线与数据折线都会被系统强制成 CanvasText，
   只剩 1px / 1.5px 的线宽差。信息在数据折线本身，网格线只是尺度感，因此不为它单写
   forced-colors 媒体分支（#381 的反向用例明令禁止：那会剥夺用户的高对比度配色选择）。 */

.monitor-view__chart-line {
  fill: none;
  stroke: var(--status-primary-default);
  stroke-width: 1.5;
  stroke-linecap: round;
  stroke-linejoin: round;
}

/* 横轴两端时刻：与绘图区左右缘对齐；tabular-nums 让推进中的秒数不左右跳 */
.monitor-view__chart-axis-row {
  display: flex;
  justify-content: space-between;
  gap: var(--spacer-8);
  margin-top: var(--spacer-4);
  color: var(--text-secondary);
  font-family: var(--font-family-metric);
  font-size: var(--body-xs-font-size);
  line-height: var(--body-xs-line-height);
  font-variant-numeric: tabular-nums;
}

.monitor-view__chart-caption {
  margin: var(--spacer-2) 0 0;
  color: var(--text-secondary);
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

/* 频率主值：数字与单位分两段排版（主机面板同款：19px 数字 + 12px/secondary 单位） */
.monitor-view__metric--with-unit {
  display: inline-flex;
  align-items: baseline;
  gap: var(--spacer-2);
}

.monitor-view__stat-unit {
  color: var(--text-secondary);
  font-size: var(--body-sm-font-size);
  line-height: var(--body-sm-line-height);
  font-weight: var(--body-sm-strong-font-weight);
}

/* 四数字主值（#435 验收反馈：对齐主机面板的 19px/600 读数比例，不再用 heading-xl 的 26px） */
.monitor-view__metric {
  font-family: var(--font-family-metric);
  font-size: var(--card-stat-value-font-size);
  line-height: var(--card-stat-value-line-height);
  font-weight: var(--heading-2xs-font-weight);
  color: var(--text-default);
  font-variant-numeric: tabular-nums;
}

.monitor-view__meta {
  font-size: var(--body-xs-font-size);
  line-height: var(--body-xs-line-height);
  color: var(--text-tertiary);
}

.monitor-view__sparkline {
  flex: 1;
  min-width: 48px;
  max-width: 160px;
}

/* 台账值列内的 sparkline（#363）：值列是纵向 flex，基础规则的 `flex: 1` 会把 24px 辅助线
   在主轴上拉伸；宽度跟随值列，不超出 `--ledger-col-value`。 */
.monitor-view__sparkline--ledger {
  flex: none;
  margin-top: var(--spacer-2);
}

/* 无容量挂载折叠组（#122）：正文之后、与列表之间留白（折叠按钮自带尺寸，不另设边框） */
.monitor-view__zero-volumes {
  margin-top: var(--spacer-8);
}

/* ---- 磁盘 I/O（#76）：每个物理盘一块，指标网格窄屏自动换行（auto-fit），全部走设计 token ---- */
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

.monitor-view__empty {
  padding: var(--spacer-20) var(--spacer-12);
}

/* ---- 指标台账（#363）：三组、每行七列、共用一条列基线 ----
   列宽由 `--ledger-*` token 定义（列宽是版面几何量，不是间距，故不进 spacer 步进）。
   七列在同一套 `grid-template-columns` 里定义：任意一列可纵向读下来比较，列永不随内容漂移。 */
.monitor-view__ledger-group {
  min-width: 0;
}

.monitor-view__ledger-group + .monitor-view__ledger-group {
  margin-top: var(--spacer-16);
}

/* 组标题条：标题 + 组注 + 组内操作（网络开关 / 立即查询）。 */
.monitor-view__ledger-group-head {
  display: flex;
  align-items: baseline;
  flex-wrap: wrap;
  gap: var(--spacer-4) var(--spacer-12);
  padding: var(--spacer-4) 0;
  border-bottom: 1px solid var(--border-neutral-l1);
}

.monitor-view__ledger-group-title {
  margin: 0;
  color: var(--text-default);
  font-size: var(--body-sm-font-size);
  line-height: var(--body-sm-line-height);
  font-weight: var(--heading-sm-font-weight);
}

.monitor-view__ledger-group-note {
  margin: 0;
  color: var(--text-secondary);
  font-size: var(--body-xs-font-size);
  line-height: var(--body-xs-line-height);
}

.monitor-view__ledger-group-actions {
  display: inline-flex;
  align-items: center;
  flex-wrap: wrap;
  gap: var(--spacer-8) var(--spacer-16);
  margin-left: auto;
}

.monitor-view__ledger-rows {
  list-style: none;
  margin: 0;
  padding: 0;
}

/* 一行 = 七列网格。列宽 token：状态点窄、名称弹性、轨道弹性、数值/阈值/余量按内容、
   明细占剩余。`align-items: baseline` 让不同字号的列落在同一条文字基线上（纵向可读）。
   `column-gap` 是列间距（不是「间距字面量违规」——值走 token）。 */
.monitor-view__ledger-row {
  display: grid;
  /* 名称 / 轨道 / 明细是弹性列，剩余宽度按 1 : 1.8 : 2.2 分：轨道要够长（两行差多少全靠它比），
     明细也要够长（网卡行的「错 … / 丢 …」是一句话，压窄就折成两行 → 行高不齐）。
     七列因此始终铺满整行、任意一列纵向可比。状态 / 读数 / 阈值 / 余量是定宽——
     数字列的宽度必须恒定，否则 13 行的当前值不会落在同一条基线上。 */
  grid-template-columns:
    var(--ledger-col-state)
    minmax(var(--ledger-col-name), 1fr)
    minmax(var(--ledger-col-track), 1.8fr)
    var(--ledger-col-value)
    minmax(var(--ledger-col-detail), 2.2fr)
    var(--ledger-col-threshold)
    var(--ledger-col-headroom);
  align-items: baseline;
  column-gap: var(--spacer-12);
  row-gap: var(--spacer-4);
  padding: var(--spacer-6) 0;
  border-bottom: 1px solid var(--border-neutral-l1);
}

/* 状态点 + 等级文字：状态不得只靠颜色（灰度截屏与色觉障碍下判断不变）。 */
.monitor-view__ledger-state {
  display: inline-flex;
  align-items: center;
  gap: var(--spacer-6);
  min-width: 0;
}

.monitor-view__ledger-dot {
  flex-shrink: 0;
  width: var(--ledger-dot-size);
  height: var(--ledger-dot-size);
  border-radius: var(--radius-full);
  background: var(--border-neutral-l2);
}

/* 正常 / 警戒 / 危险三档色标：与顶栏徽章、卷行共用同一套等级 token。
   等级为 null（无分母可判）时不套色——「没法判」与「一切正常」是两件事。 */
.monitor-view__ledger-row[data-level='normal'] .monitor-view__ledger-dot {
  background: var(--status-success-default);
}

.monitor-view__ledger-row[data-level='warn'] .monitor-view__ledger-dot {
  background: var(--status-alert-default);
}

.monitor-view__ledger-row[data-level='danger'] .monitor-view__ledger-dot {
  background: var(--status-error-default);
}

.monitor-view__ledger-state-text {
  min-width: 0;
  /* 11px 正文：tertiary 在深/浅两套下分别只有 3.19 / 4.35，够不到 4.5 —— 台账内承载信息的
     小字统一用 secondary（深 5.86 / 浅 9.51）。装饰性字仍走 tertiary。 */
  color: var(--text-secondary);
  font-size: var(--body-xs-font-size);
  line-height: var(--body-xs-line-height);
  white-space: nowrap;
}

/* 等级文字用**配对档** -on-surface（DESIGN.md:229 的 Paired-Step Rule：实体 -default 只给点/条/
   图标，文字走 -on-surface）。浅色 warn 实体 #FEA900 在卡片底上只有 1.77:1；点与轨道填充仍保持
   -default —— 这条规则只约束文字。 */
.monitor-view__ledger-row[data-level='warn'] .monitor-view__ledger-state-text {
  color: var(--status-warning-on-surface);
}

.monitor-view__ledger-row[data-level='danger'] .monitor-view__ledger-state-text {
  color: var(--status-error-on-surface);
}

/* 名称 + 契约键：名称可换行（挂载点路径必须完整可读，#321 的教训），契约键是等宽小字。 */
.monitor-view__ledger-name {
  display: flex;
  flex-direction: column;
  gap: var(--spacer-2);
  min-width: 0;
}

.monitor-view__ledger-label {
  color: var(--text-default);
  font-size: var(--body-sm-font-size);
  line-height: var(--body-sm-line-height);
  overflow-wrap: anywhere;
}

.monitor-view__ledger-key {
  /* 契约键是 11px 的**信息**文字（不是装饰）：tertiary 深/浅只有 3.19 / 4.35，不够 4.5 的
     正文口径，台账内承载信息的小字统一升到 secondary。 */
  color: var(--text-secondary);
  font-family: var(--font-family-mono);
  font-size: var(--body-xs-font-size);
  line-height: var(--body-xs-line-height);
  overflow-wrap: anywhere;
}

/* 量程轨道：中性底色给出 0–100% 的参照，填充宽度由 `--ledger-fill` 注入（逐行不同，只能运行时定）。
   缺口是**展示分档**（70 / 90，来自 useResourceLevel 的两个常量），只在有契约阈值的行上出现。 */
.monitor-view__ledger-track {
  position: relative;
  align-self: center;
  width: 100%;
  height: var(--ledger-track-height);
  overflow: hidden;
  border-radius: var(--radius-2);
  background: var(--bg-overlay-l2);
  /* forced-colors 下背景被统一成 Canvas、填充也被统一成同一个色，轨道整条消失（#363 audit A5）。
     用 `outline` 而非 `border`：不占盒模型，`overflow: hidden` 与缺口定位都不受影响。 */
  outline: var(--border-width-default) solid var(--border-neutral-l2);
  outline-offset: 0;
}

.monitor-view__ledger-track-empty {
  align-self: center;
  width: 100%;
}

/* 填充块自身的描边（#381）：轨道有了 outline 只画出一个空框，而 forced-colors 下填充与轨道背景
   被统一成同一个 Canvas 色 —— 「填充到哪里」这条唯一的图形信息随之消失，只剩右边的百分比文字。
   `outline` 不占盒模型（`border` 会在 `--ledger-fill: 0%` 时仍占 1px，把 0.4% 画成 1%），
   且随 `overflow: hidden` 被轨道裁掉四边，只在填充块的边缘留下一圈。
   `transparent` 是这条修法能在正常主题下「零像素」的原因：普通渲染不画任何东西，
   forced-colors 下系统把它换成 `CanvasText`（不用另写 `@media (forced-colors: active)` 分支，
   也就不必在四个等级色上各写一遍描边色）。

   **已知边界（实测，不粉饰）**：填充宽度 < 2px 时（本机存在 0.6% 这种真实取值，宽约 1.47px），
   负 offset 把描边压到元素内部后只剩右缘一条 1px 竖线，「一圈边界」不成立。这一档下
   比例信息由右侧的百分比文字兜底，没有丢；改 `border` 会更糟——实测 1% 会把 0.39px 画成 2px，
   那是把读数改大，比少画一条边严重。 */
.monitor-view__ledger-track-fill {
  display: block;
  width: var(--ledger-fill, 0%);
  height: 100%;
  border-radius: var(--radius-2);
  background: var(--status-primary-default);
  outline: var(--border-width-default) solid transparent;
  outline-offset: calc(-1 * var(--border-width-default));
}

.monitor-view__ledger-row[data-level='normal'] .monitor-view__ledger-track-fill {
  background: var(--status-success-default);
}

.monitor-view__ledger-row[data-level='warn'] .monitor-view__ledger-track-fill {
  background: var(--status-alert-default);
}

.monitor-view__ledger-row[data-level='danger'] .monitor-view__ledger-track-fill {
  background: var(--status-error-default);
}

/* 缺口：一条 1px 的竖线，位置由 `--ledger-gap` 注入（两个常量，不是阈值）。
   `outline` 与 `background` 双写（#363 audit A5）：forced-colors（Windows 高对比度）下
   `background-color` 被系统统一成 Canvas，缺口会与填充同色而消失；`outline-color` 走
   `CanvasText`，因此缺口位置在任何配色下都可读。 */
.monitor-view__ledger-gap {
  position: absolute;
  top: 0;
  bottom: 0;
  left: var(--ledger-gap, 0%);
  width: var(--border-width-default);
  background: var(--text-tertiary);
  outline: var(--border-width-default) solid var(--text-tertiary);
  outline-offset: 0;
}

.monitor-view__ledger-value {
  display: flex;
  flex-direction: column;
  /* 不写 `align-items: flex-end`：值列里的可点读数（`.monitor-view__value-btn`）要铺满整列
     才有 96px 的命中区，`flex-end` 会把它压成文字宽（实测 96 → 34–41px）。右对齐只需
     `text-align: right`，它已经能把数字贴到列的右缘。 */
  gap: var(--spacer-2);
  color: var(--text-default);
  font-family: var(--font-family-metric);
  font-size: var(--body-sm-font-size);
  line-height: var(--body-sm-line-height);
  font-variant-numeric: tabular-nums;
  text-align: right;
  white-space: nowrap;
}

/* 等级色同样走配对档（Paired-Step Rule）：-default 是点/条/图标的实体色，文字用 -on-surface。 */
.monitor-view__ledger-row[data-level='warn'] .monitor-view__ledger-value {
  color: var(--status-warning-on-surface);
}

.monitor-view__ledger-row[data-level='danger'] .monitor-view__ledger-value {
  color: var(--status-error-on-surface);
}

.monitor-view__ledger-detail-part + .monitor-view__ledger-detail-part {
  margin-left: var(--spacer-8);
}

/* 明细列 = 一句话 + 右端一个「明细」按钮。用 flex 而不是自然流：
   自然流下长明细会把按钮挤到第二行，导致有的行一行高、有的两行高，13 行的行高节奏就散了。 */
.monitor-view__ledger-detail {
  display: flex;
  align-items: baseline;
  gap: var(--spacer-8);
  min-width: 0;
  color: var(--text-tertiary);
  font-size: var(--body-xs-font-size);
  line-height: var(--body-xs-line-height);
}

.monitor-view__ledger-detail-text {
  min-width: 0;
  /* 明细列是台账里的**信息**文字（不是装饰性小字）：tertiary 在两套主题下都够不到 11px
     正文的 4.5:1，故升到 secondary。 */
  color: var(--text-secondary);
  overflow-wrap: anywhere;
}

/* 空态行：说明文字占两列（状态点之后到轨道列），仍是同一套网格——列基线不因空态而移动。 */
.monitor-view__ledger-row--empty .monitor-view__ledger-value,
.monitor-view__ledger-row--empty .monitor-view__ledger-threshold,
.monitor-view__ledger-row--empty .monitor-view__ledger-headroom {
  color: var(--text-tertiary);
}

/* 列名只出现在窄屏：≥768 时七列各有固定位置，列名是噪声；<768 时每行拆成三行堆叠，
   阈值与余量离开列位置后必须自带名字，否则「90%」与「12.8 GiB」就是两个无主数字。 */
.monitor-view__ledger-col-label {
  display: none;
  margin-right: var(--spacer-4);
  /* 列名只在窄屏出现，是「90%」与「12.8 GiB」的归属说明 —— 属信息文字，不是装饰。 */
  color: var(--text-secondary);
}

/* 阈值与余量各自成条（不合并选择器）：两格在窄屏落到不同 grid-area，且都要能被单独定位 /
   单独断言「不折行」——合并成一条会让按选择器取规则的检查失效。
   `text-align: right` 是 DESIGN.md:354 的表口径（Numeric columns right-align with tabular
   figures）。窄屏（<1024）两格落在 `threshold headroom` 这一行的左右两端 —— 阈值在行首左对齐
   正好贴住行内容区，余量右端贴住行尾，右对齐在两侧都成立；不另加媒体查询（那会让「七列右对齐」
   这件事在代码里有两种说法）。 */
.monitor-view__ledger-threshold {
  color: var(--text-secondary);
  font-family: var(--font-family-metric);
  font-size: var(--body-xs-font-size);
  line-height: var(--body-xs-line-height);
  font-variant-numeric: tabular-nums;
  text-align: right;
  white-space: nowrap;
}

.monitor-view__ledger-headroom {
  color: var(--text-secondary);
  font-family: var(--font-family-metric);
  font-size: var(--body-xs-font-size);
  line-height: var(--body-xs-line-height);
  font-variant-numeric: tabular-nums;
  text-align: right;
  white-space: nowrap;
}

/* ---- 内存块（#445）：照主机面板插件内存卡——半宽卡片（≥lg 占一列，<lg 全宽）----
   组成带沿用 #363 的中性色阶（DESIGN：只有状态才有颜色；不搬插件的紫/绿分段色），
   Meter 填充仍走状态色 + 70 / 90 分档（与顶栏 MEM 徽章同判据）。 */
.monitor-view__memory {
  min-width: 0;
}

.monitor-view__memory-total {
  min-width: 0;
  margin: 0 0 0 auto;
  overflow: hidden;
  color: var(--text-secondary);
  font-size: var(--body-xs-font-size);
  line-height: var(--body-xs-line-height);
  text-overflow: ellipsis;
  white-space: nowrap;
}

/* Meter：标签 + 百分比同一行两端对齐，轨道在下面，字节小字在最下（插件同款三段结构） */
.monitor-view__meter {
  display: flex;
  flex-direction: column;
  gap: var(--spacer-4);
  min-width: 0;
}

.monitor-view__meter--compact {
  margin-top: var(--spacer-8);
}

.monitor-view__meter-head {
  display: flex;
  align-items: baseline;
  justify-content: space-between;
  gap: var(--spacer-8);
  min-width: 0;
}

.monitor-view__meter-label {
  overflow: hidden;
  color: var(--text-secondary);
  font-size: var(--body-sm-font-size);
  line-height: var(--body-sm-line-height);
  text-overflow: ellipsis;
  white-space: nowrap;
}

.monitor-view__meter-value {
  flex: none;
  color: var(--text-default);
  font-family: var(--font-family-metric);
  font-size: var(--body-sm-font-size);
  line-height: var(--body-sm-line-height);
  font-weight: var(--body-sm-strong-font-weight);
  font-variant-numeric: tabular-nums;
  white-space: nowrap;
}

.monitor-view__meter-track {
  width: 100%;
  height: var(--ledger-track-height);
  overflow: hidden;
  border-radius: var(--radius-full);
  background: var(--bg-overlay-l2);
}

.monitor-view__meter-track--compact {
  height: var(--spacer-4);
}

.monitor-view__meter-fill {
  display: block;
  width: var(--meter-fill, 0%);
  height: 100%;
  border-radius: var(--radius-full);
  background: var(--status-success-default);
}

/* 等级色只给填充条（实体色档）；数值文字保持主色——插件同款。 */
.monitor-view__meter-fill[data-level='warn'] {
  background: var(--status-alert-default);
}

.monitor-view__meter-fill[data-level='danger'] {
  background: var(--status-error-default);
}

.monitor-view__meter-caption {
  margin: 0;
  color: var(--text-secondary);
  font-size: var(--body-xs-font-size);
  line-height: var(--body-xs-line-height);
  font-variant-numeric: tabular-nums;
}

.monitor-view__memory-muted {
  margin: var(--spacer-8) 0 0;
  color: var(--text-secondary);
  font-size: var(--body-xs-font-size);
  line-height: var(--body-xs-line-height);
}

/* 组成带（#363 迁入）：四段横向堆叠，宽度按各自占总量比例（`--memory-seg`），下方图例逐段标注。
   与主数值是两个口径：主数值是「用量比率」，组成带是「总量分解」，因此各自标注自己的分母。 */
.monitor-view__memory-composition {
  min-width: 0;
  margin-top: var(--spacer-8);
}

.monitor-view__memory-band {
  display: flex;
  width: 100%;
  height: var(--memory-band-height);
  overflow: hidden;
  border-radius: var(--radius-2);
  background: var(--bg-overlay-l1);
  /* 整条带子的外框（#381）：段间的缝由 `border-inline-start` 撑起（#363），但首段没有左边框、
     末段没有右边框，forced-colors 下 band 与 seg 同被统一成 Canvas 后，这条图形（总量 15 GiB）
     的左右两端没有边界，比例就被读成「从任意处开始」。透明描边在正常主题下不画任何像素，
     forced-colors 下自动取 `CanvasText` 补上外框。 */
  outline: var(--border-width-default) solid transparent;
  outline-offset: 0;
}

.monitor-view__memory-band-seg {
  display: block;
  flex: 0 0 var(--memory-seg, 0%);
  height: 100%;
}

/* 组成带四段用**中性色阶**而不是状态色：这一页的世界观「只有状态才有颜色」，
   给「缓存 / 缓冲」涂上琥珀与绿会让它们读成「警戒 / 正常」两个状态，而它们只是构成。
   色阶从实到虚表达「这段离可用有多远」：已用最实、空闲最虚。
   #363：原色阶（default / secondary / tertiary / overlay-l3）里最重的那条边界恰好是最要紧的
   那条 —— 已用|缓存只有 1.73:1（浅）/ 2.05:1（深），46% 的用量整块读成 93%。
   改法是把整条阶梯往「虚」的一侧挪一格（cached 接过 tertiary、buffers 接过 disabled），
   让弱边界从**已用|缓存**（压力 / 可回收的分界，必须看清）挪到 **缓存|缓冲**
   （两者都是可回收，语义上本就该接近）。
   实测相邻比（浅 / 深）：已用|缓存 3.78 / 3.76，缓存|缓冲 1.84 / 1.65，缓冲|空闲 1.80 / 1.45
   （末两对接近，因为「缓存 / 缓冲 / 空闲」三者都是可回收或未用，本就不该互相争抢注意力）。
   四段严格单调（实→虚），且已用额外加一圈 1px 内描边把它钉成「实心块」，
   边界不单独依赖色差。 */
.monitor-view__memory-band-seg--used {
  background: var(--text-default);
  /* 内描边用既有底色 token（与段间缝隙同色）：把「已用」圈成一块，边界不依赖相邻段的色差。 */
  box-shadow: inset 0 0 0 var(--border-width-default) var(--bg-layout-1);
  /* 上面那条 `box-shadow` 在 forced-colors 下被强制为 `none`（#381）：「已用」与另外三段同被
     统一成 Canvas，四段读不出哪一段是压力所在。补一圈透明 `outline`（正常主题零像素），
     forced-colors 下自动取 `CanvasText`，把「已用」重新框出来。
     用 `outline` 而不是 `border`：四段宽度按占比注入（`flex-basis`），`border` 会占掉盒模型、
     改变各段比例；`outline-offset` 负值让它落在段内，相邻段的缝仍归 `border-inline-start`。 */
  outline: var(--border-width-default) solid transparent;
  outline-offset: calc(-1 * var(--border-width-default));
}

.monitor-view__memory-band-seg--cached {
  background: var(--text-tertiary);
}

.monitor-view__memory-band-seg--buffers {
  background: var(--text-disabled);
}

.monitor-view__memory-band-seg--free {
  background: var(--bg-overlay-l3);
}

/* 段与段之间留一道底色缝：四段相邻时纯色阶的边界读不出来，而「哪一段占多少」正是这条带子的全部内容。
   缝必须是 `border` 而不是 `box-shadow`（#363 audit A5）：forced-colors（Windows 高对比度）下
   `box-shadow` 被系统强制为 `none`、`background-color` 被统一成 Canvas/ButtonFace ——
   四段会糊成一片白、整条带子消失。`border` 在 forced-colors 下保留并自动取 `CanvasText`，
   段间边界因此仍然可读。 */
.monitor-view__memory-band-seg + .monitor-view__memory-band-seg {
  border-inline-start: var(--border-width-default) solid var(--bg-layout-1);
}

.monitor-view__memory-legend {
  display: flex;
  flex-wrap: wrap;
  gap: var(--spacer-2) var(--spacer-16);
  margin: var(--spacer-4) 0 0;
  padding: 0;
  list-style: none;
}

.monitor-view__memory-legend-item {
  display: inline-flex;
  align-items: center;
  gap: var(--spacer-6);
  min-width: 0;
}

.monitor-view__memory-legend-swatch {
  flex-shrink: 0;
  width: var(--ledger-dot-size);
  height: var(--ledger-dot-size);
  border-radius: var(--radius-2);
}

.monitor-view__memory-legend-label {
  /* 图例是「这一段多少 GiB」的归属说明 —— 信息文字，不是装饰。 */
  color: var(--text-secondary);
  font-size: var(--body-xs-font-size);
  line-height: var(--body-xs-line-height);
}

.monitor-view__memory-legend-value {
  color: var(--text-secondary);
  font-family: var(--font-family-metric);
  font-size: var(--body-xs-font-size);
  line-height: var(--body-xs-line-height);
  font-variant-numeric: tabular-nums;
}

/* 中档（1024–1279）：七列不降级，只把整组数字列一起收窄。容器在这一档约 700px，
   而 ≥1280 那套列宽的下限之和已经超过它——只缩轨道与明细会把名称列压成 0。
   必须写在这里（基础规则之后）：媒体查询不叠加优先级，靠前的同名规则会被后面的基础规则覆盖。 */
@media (min-width: 1024px) and (max-width: 1279.98px) {
  .monitor-view__ledger-row {
    grid-template-columns:
      var(--ledger-col-state-md)
      minmax(var(--ledger-col-name-md), 1fr)
      minmax(var(--ledger-col-track-md), 1fr)
      var(--ledger-col-value-md)
      minmax(var(--ledger-col-detail-md), 1.4fr)
      var(--ledger-col-threshold-md)
      var(--ledger-col-headroom-md);
    column-gap: var(--spacer-8);
  }

}

/* 行明细：字段表跨满整行（七列之上），字段是两列自适应网格；触发它的按钮在明细列里。 */
/* 展开按钮：不参与压缩（列再窄也保持可点，被压扁的是它左边那句话），
   字号跟明细一致——它是明细列的一部分，不是独立的操作区。 */
.monitor-view__ledger-expand-btn {
  flex: none;
  padding: 0;
  /* 11px 且是**控件文字**（点它展开）：tertiary 在两套主题下都够不到 4.5:1（4.35 / 3.19），
     而它不属于 #324 的装饰性小字豁免（浮层提示、图表刻度那类）。升 secondary（9.51 / 5.86）。 */
  color: var(--text-secondary);
  font-size: var(--body-xs-font-size);
  line-height: var(--body-xs-line-height);
  white-space: nowrap;
}

.monitor-view__ledger-expand-btn:hover {
  color: var(--text-default);
}

/* 展开后的字段表跨满整行（它是行内的第二层内容，不是第八列）；auto-fit 让它随宽度自己分栏。 */
.monitor-view__ledger-fields {
  display: grid;
  grid-column: 1 / -1;
  grid-template-columns: repeat(auto-fit, minmax(var(--ledger-field-min-width), 1fr));
  gap: var(--spacer-4) var(--spacer-16);
  margin: var(--spacer-8) 0 0;
}

.monitor-view__ledger-field {
  display: flex;
  align-items: baseline;
  justify-content: space-between;
  gap: var(--spacer-8);
  min-width: 0;
  padding: var(--spacer-2) 0;
  border-bottom: 1px solid var(--border-neutral-l1);
}

.monitor-view__ledger-field-label {
  /* 展开明细里的字段名（「读吞吐」「队列深度」）是信息文字。 */
  color: var(--text-secondary);
  font-size: var(--body-xs-font-size);
  line-height: var(--body-xs-line-height);
  overflow-wrap: anywhere;
}

.monitor-view__ledger-field-value {
  margin: 0;
  color: var(--text-default);
  font-family: var(--font-family-metric);
  font-size: var(--body-xs-font-size);
  line-height: var(--body-xs-line-height);
  font-variant-numeric: tabular-nums;
  text-align: right;
  overflow-wrap: anywhere;
}

/* 台账 <1024 改三行堆叠（#363）：状态 + 当前值 / 名称 / 整行轨道 / 明细 + 阈值 + 余量。
   为什么下界是 1024 而不是 768：768–1023 视口下 app-shell 的侧栏常驻 220px（layout.css
   `@media (min-width: 768px)`），台账实际只剩 ~440px——七列在这个宽度里每个格都折成竖排单字
   （实测 768 视口：名称列 43px、明细列 60px）。容器查询在本仓被白名单门禁禁止，
   档位只能按「侧栏是否常驻」这个视口事实切。此时阈值与余量失去列位置，故给它们补上列名。 */
@media (max-width: 1023.98px) {
  .monitor-view__ledger-row {
    grid-template-columns: minmax(0, 1fr) auto;
    grid-template-areas:
      'state value'
      'name name'
      'track track'
      'detail detail'
      'threshold headroom';
    column-gap: var(--spacer-8);
  }

  .monitor-view__ledger-state {
    grid-area: state;
  }

  .monitor-view__ledger-name {
    grid-area: name;
  }

  .monitor-view__ledger-track {
    grid-area: track;
    margin: var(--spacer-2) 0;
  }

  .monitor-view__ledger-value {
    grid-area: value;
    justify-self: end;
  }

  .monitor-view__ledger-detail {
    grid-area: detail;
  }

  .monitor-view__ledger-threshold {
    grid-area: threshold;
    /* 右对齐是**七列表格**的列口径（基础规则）。堆叠后这一行成了「阈值 90% + 余量 12.8 GiB」
       一行的左右两端：左格若也右对齐会贴住右格，两个数读起来像一对。故窄屏改回左对齐；
       余量格本身 `justify-self: end`，文字左右对齐的差别为零，不另加覆盖。 */
    text-align: start;
  }

  .monitor-view__ledger-headroom {
    grid-area: headroom;
    justify-self: end;
  }

  .monitor-view__ledger-col-label {
    display: inline;
  }

  .monitor-view__ledger-group-head {
    align-items: flex-start;
    flex-direction: column;
  }

  .monitor-view__ledger-group-actions {
    margin-left: 0;
  }
}

/* 时钟行的辅助信息（#302）：方向说明 / 质量提示 / 同步服务检测，同样跨满整行。 */
.monitor-view__ledger-clock {
  grid-column: 1 / -1;
  min-width: 0;
}

.monitor-view__ledger-daemons {
  margin-top: var(--spacer-4);
}

/* ---- CPU 块（#435）：四数字 / 固定 0–100% 单曲线 / 每核条形 三块纵向排布 ---- */
/* 三块都跨满整卡（`grid-column: 1 / -1`），因此这里只需单列；块间距对齐主机面板的 10px。 */
.monitor-view__cpu {
  display: grid;
  grid-template-columns: minmax(0, 1fr);
  gap: var(--spacer-8);
}

/* 每核条形（#435 决策 6）：曲线下方直接渲染、无标题行。
   列数由视图实测容器宽度后按纯函数 `coreBarColumns` 算好、经 `--core-columns` 注入——
   **不用 auto-fit**：auto-fit 会把余数甩给最后一行（16 核在窄屏可能折成 11 + 5），
   而本块要求多行时每行数量尽量相等（整除优先；找不到整除时各行最多差 1 个）。
   `minmax(0, 1fr)` 让格宽平均分配且允许收缩，条形永不横向溢出。 */
.monitor-view__core-bars {
  min-width: 0;
}

.monitor-view__cores {
  display: grid;
  grid-template-columns: repeat(var(--core-columns, 1), minmax(0, 1fr));
  gap: var(--spacer-6) var(--spacer-3);
  align-items: start;
  margin: 0;
  padding: 0;
  list-style: none;
}

/* 单核：只有柱；逐核读数由 title / aria 提供（#435 决策 6 不显示文字）。
   柱高由容器网格统一，同一行柱顶对齐。 */
.monitor-view__core {
  display: flex;
  flex-direction: column;
  align-items: stretch;
  gap: var(--spacer-2);
  min-width: 0;
}

/* 柱：竖向轨道，自底向上按占用率填充——高度差异一眼可读。
   轨道高度走 `--core-bar-height` token（不是间距，不进 spacer 步进）。
   flex + `justify-content: flex-end` 是填充贴底的必要条件：块级容器里 `margin-top: auto`
   会解析成 0，填充会挂在轨道顶部（实测踩过）。 */
.monitor-view__core-bar {
  display: flex;
  flex-direction: column;
  justify-content: flex-end;
  width: 100%;
  height: var(--core-bar-height);
  border-radius: var(--radius-3);
  background: var(--bg-overlay-l2);
  overflow: hidden;
  /* 轨道外框（#381）：forced-colors 下轨道底与填充都被统一成 Canvas，柱的**满格高度**消失，
     只有填充被描边时读者无法区分「柱高 = 轨道高度」与「柱高 = 当前占用」。
     透明描边在正常主题下不画像素，forced-colors 下取 `CanvasText`。
     用 `outline` 而非 `border`：轨道高度是 token 定值，`border` 会缩掉 2px 的画布。 */
  outline: var(--border-width-default) solid transparent;
  outline-offset: 0;
}

/* 柱的填充：高度由 `--core-fill`（占用率）驱动；默认成功色，≥70% / ≥90% 换警示 / 危险色。
   描边同台账轨道填充（#381）：`outline` 不占盒模型、不改变 `--core-fill` 的百分比语义，
   `outline-offset` 负值把它压进填充块内部，四边都随轨道 `overflow: hidden` 收口。

   **已知边界（实测，不粉饰）**：填充高度 < 3px 时（本机 8 核里 4 个核 < 3%），
   负 offset 的描边装不进这一档高度，forced-colors 下**一个像素都不画** ——
   读者看到的仍是空轨道。这一档下逐核读数在 title / aria（「核 N · X%」）里，信息没丢。
   这不是回归（改前同样不生效），写在这里防止下一个人把它当 bug 重报。 */
.monitor-view__core-bar-fill {
  display: block;
  width: 100%;
  height: var(--core-fill, 0%);
  border-radius: var(--radius-3);
  background: var(--status-success-default);
  outline: var(--border-width-default) solid transparent;
  outline-offset: calc(-1 * var(--border-width-default));
}

/* 每核填充按占用率等级着色（#435 8080 验收：饱和标记删除后，颜色仍与顶栏 CPU 徽章同判据——
   70% / 90% 才变橙/红，正常核保持成功色；颜色只是冗余信号，读数在 tooltip / 读屏文案里）。 */
.monitor-view__core-bar-fill[data-level='warn'] {
  background: var(--status-warning-default);
}

.monitor-view__core-bar-fill[data-level='danger'] {
  background: var(--status-error-default);
}

/* 无读数的核（首次采样 / 采集失败）在轨道的**背景**上加斜纹，与高度无关：
   逐核文字按 #435 决策 6 删除后，空轨道若与「真的 0%」同形，图形层就会用 0 冒充缺失
   （仓库反复禁止的「缺失不是 0」）。斜纹只出现在没有读数的格上，0% 永远是干净轨道。 */
.monitor-view__core-bar--unavailable {
  background-image: repeating-linear-gradient(
    135deg,
    transparent 0 var(--spacer-4),
    var(--bg-overlay-l1) var(--spacer-4) var(--spacer-8)
  );
}


.monitor-view__hint {
  margin: var(--spacer-8) 0 0;
  color: var(--text-tertiary);
  font-size: var(--body-xs-font-size);
  line-height: var(--body-xs-line-height);
}

/* 时钟卡片的「需要关注」提示（#302）：测量质量劣化 / 未检测到同步服务 / 多服务冲突。
   与常规语义提示（中性灰）区分——这三条都是「该看一眼」但**不弹窗**的状态（契约 §2.8 静默原则）。 */
.monitor-view__hint--alert {
  color: var(--status-warning-default);
}

/* 时钟卡片的「立即查询」按钮（#302）：与卡片标题同排，不抢主数值的位置 */
.monitor-view__clock-probe {
  display: inline-flex;
  align-items: center;
}

/* 冷却期按钮宽度稳定（#326）：文案在「立即查询」与「N 秒后可再次查询」之间切换，
   原实测宽度 87.1px → 138.5px，按钮自身左缘左移（卡片头是 space-between，标题位置不动）。
   按最长文案预留宽度：`manual_min_interval_sec` 上限 3600 → 最长「3600 秒后可再次查询」，
   实测 151.7px；留 ~6px 余量吸收跨平台字体回退 / DPR 取整后取 158px，内容居中
   （正常态只是左右留白，不再有位移）。 */
.monitor-view__clock-probe .ds-btn {
  min-width: 158px;
}

.monitor-view__clock-servers {
  list-style: none;
  margin: 0;
  padding: 0;
  display: flex;
  flex-direction: column;
  gap: var(--spacer-8);
}

/* 每台服务器一行：与磁盘 I/O 行同一视觉（边框 + 圆角 + io-metrics 网格），不新增范式 */
.monitor-view__clock-server {
  display: grid;
  gap: var(--spacer-8);
  min-width: 0;
  padding: var(--spacer-8) var(--spacer-12);
  border: 1px solid var(--border-neutral-l1);
  border-radius: var(--radius-6);
}

/* 服务运行状态色（#302）：运行中用成功色、未运行用中性色——未运行不是故障，不上告警色 */
.monitor-view__daemon--running {
  color: var(--status-success-default);
}

.monitor-view__daemon--stopped {
  color: var(--text-tertiary);
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

/* 挂载点例外：窄列下宁可换行也不省略，它是本行唯一不可从别处恢复的信息（#321）。 */
.monitor-view__list-label--mount {
  min-width: 0;
  overflow-wrap: anywhere;
  white-space: normal;
}

.monitor-view__list-value {
  color: var(--text-default);
  font-size: var(--body-sm-font-size);
  line-height: var(--body-sm-line-height);
  font-variant-numeric: tabular-nums;
  min-width: 0;
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
    /* 窄屏真正的滚动容器是 document：dashboard.css 在 ≤1024 把 app-shell / workspace 高度释放给
       文档滚动（全局 topbar 因此能基于视口 sticky）。而 .monitor-view 的 overflow:auto 会成为最近的
       scrollport，把视图切换行的 sticky 困在它内部（实测：页面滚动时该行直接跟着消失）。
       放开窄屏 overflow 让 sticky 相对视口生效；横向溢出由页面内容自身约束（#321 已验证无溢出）。 */
    overflow: visible;
    padding: var(--spacer-24) var(--spacer-16);
  }

  /* 窄屏只钉住视图切换那一行（#328）：不钉整块 header（390×844 上会吃掉 90–110px 视口）。
     display:contents 把 topbar 盒去掉，视图切换行成为滚动容器的直接子级，sticky 才能跨越整个
     内容区生效；整行背景 + z-index 防止下方卡片从行下透出。
     top 取全局 `.topbar` 的固定高度 32px（assets/css/layout.css）：行贴在全局顶栏下方，
     内容不从缝隙透出（实测 390×844 滚动 1000px 时行 top=32、其上方 2px 处命中全局顶栏）。 */
  .monitor-view__topbar {
    display: contents;
  }

  .monitor-view__header {
    margin-bottom: var(--spacer-12);
  }

  .monitor-view__view-switch {
    position: sticky;
    top: 32px;
    z-index: 2;
    display: flex;
    width: 100%;
    margin: 0 0 var(--spacer-16);
    padding: var(--spacer-8) 0;
    background: var(--bg-base-default);
  }

  .monitor-view__card--wide {
    grid-column: auto;
  }

  /* 窄屏：CPU 块的内层块让回单列（卡片本身已 `grid-column: auto`）。
     主机条是顶栏下方的独立一条（#430），不再是网格项，不在此处列。 */
  .monitor-view__stats,
  .monitor-view__trend {
    grid-column: auto;
  }
}

/* ═══ 触摸下限（P5-T3，见 components.css:384 的触摸规范）═══
   台账值列里的可点读数是**自绘按钮**（`.monitor-view__value-btn`），不在 `.ds-btn` 族里，
   因此拿不到 `@media (pointer: coarse)` 的 44px 下限：桌面实测 96×18，手指按上去只有一条线。
   触摸优先设备上补纵向点击区——`min-height` 撑开命中区，字号 / 颜色 / 行高不变（视觉零变化）。
   桌面 pointer: fine 不受影响（保持台账的紧凑密度）。 */
@media (pointer: coarse) {
  .monitor-view__value-btn {
    /* --spacer-40 = 44px，与 `.ds-btn` 的触摸下限同值（不新造 token，也不写裸 px） */
    min-height: var(--spacer-40);
  }
}
</style>
