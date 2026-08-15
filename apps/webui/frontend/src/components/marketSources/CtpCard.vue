<script setup lang="ts">
import { ref, computed } from 'vue'
import Icon from '@/components/shared/Icon.vue'
import Modal from '@/components/shared/Modal.vue'
import TimePicker from '@/components/shared/TimePicker.vue'
import StatusIndicator from '@/components/shared/StatusIndicator.vue'
import { processStateText, processStateColor } from '@/composables/useProcessState'
import { useMarketSourcesStore } from '@/stores/marketSources'
import type { MarketSourceView } from '@/stores/marketSources'
import type { SubscribeParamsBody, ShmConfigPatch } from '@/api/marketSources'
import LoginPanel from './LoginPanel.vue'
import ScheduleManager from './ScheduleManager.vue'
import BrokerSelector from './BrokerSelector.vue'
import BrokerCard from './BrokerCard.vue'

// CtpCard 接收 MarketSource (契约 06 ui_card 机制)
// CTP 大类专用卡片壳: 头部 + 登录/时段/经纪商各子组件 + 删除 + 添加经纪商 Modal
// 卡片体拆分为 5 子组件 (LoginPanel/ScheduleManager/BrokerSelector/BrokerCard/FrontendTable)
// 全局 error toast 由 MarketSourcesView 统一 watch store.error 处理, 卡片内不重复
const props = defineProps<{ source: MarketSourceView }>()
const store = useMarketSourcesStore()
const src = computed(() => props.source)

// ===== Add broker modal (F-C10: name 创建后不可改 + broker_id/user_id/password/product_info) =====
const brokerModalOpen = ref(false)
const brokerModalSourceId = ref<number | null>(null)
const newBrokerName = ref('')
const newBrokerBrokerId = ref('')
const newBrokerUserId = ref('')
const newBrokerPassword = ref('')
const newBrokerProductInfo = ref('')
const brokerSubmitting = ref(false)

function openBrokerModal(sourceId: number): void {
  brokerModalSourceId.value = sourceId
  newBrokerName.value = ''
  newBrokerBrokerId.value = ''
  newBrokerUserId.value = ''
  newBrokerPassword.value = ''
  newBrokerProductInfo.value = ''
  brokerModalOpen.value = true
}

function closeBrokerModal(): void {
  brokerModalOpen.value = false
  brokerModalSourceId.value = null
  newBrokerName.value = ''
  newBrokerBrokerId.value = ''
  newBrokerUserId.value = ''
  newBrokerPassword.value = ''
  newBrokerProductInfo.value = ''
  brokerSubmitting.value = false
}

/// F-C10: 失败时保持 Modal 打开, 成功时关闭 (不乐观添加, 等 WS 推送)
async function confirmAddBroker(): Promise<void> {
  if (!brokerModalSourceId.value || !newBrokerName.value.trim()) return
  brokerSubmitting.value = true
  try {
    await store.addBroker(brokerModalSourceId.value, {
      name: newBrokerName.value.trim(),
      broker_id: newBrokerBrokerId.value.trim(),
      user_id: newBrokerUserId.value.trim(),
      password: newBrokerPassword.value,
      product_info: newBrokerProductInfo.value.trim(),
    })
    closeBrokerModal()
  } catch {
    // 错误已由 store 设置 error 字段, 由 View 触发 toast; 保持 Modal 打开让用户修改后重试
    brokerSubmitting.value = false
  }
}

// ===== Header 辅助函数 =====
// 订阅统计配色: expected=0（无订阅需求）为中性; 有需求且未满足为告警
const subscribeClass = (s: MarketSourceView): string => {
  if (s.subscribeTotal === 0) return 'source-card__info-sub--ok'
  return s.subscribeCount === s.subscribeTotal
    ? 'source-card__info-sub--ok'
    : 'source-card__info-sub--warn'
}
// header 展示自动登录状态; 控件内状态见 LoginPanel
const autoLoginStatusText = (s: MarketSourceView): string =>
  s.autoLoginPending ? '切换中' : (s.auto_login ? '已启用' : '未启用')

// ===== 高级段（契约 02 SHM 行情通道配置, dzmd_* 通用层; 默认折叠）=====
// page_size_mb 启动后不可变（仅配置文件、启动前修改）→ 只读;
// check_* 三字段失焦单字段提交; preload_points 行编辑（null 删除语义）。
// 范围对照契约 02: interval ∈ [0,1440], pages ∈ [0,8], bytes ∈ [0, 2^40]
const advancedOpen = ref(false)
const shmCfg = computed(() => store.shmConfigs[src.value.source_name])
const preloadEntries = computed(() =>
  Object.entries(shmCfg.value?.preload_points ?? {})
    .sort(([a], [b]) => a.localeCompare(b)))

// check_* 三字段上限（契约 02 范围）: interval ∈ [0,1440], pages ∈ [0,8], bytes ∈ [0, 2^40]
const SHM_FIELD_MAX = {
  check_interval_min: 1440,
  check_pages: 8,
  check_bytes: 2 ** 40,
} as const

function onShmNumBlur(field: 'check_interval_min' | 'check_pages' | 'check_bytes', event: Event): void {
  const raw = (event.target as HTMLInputElement).value.trim()
  if (raw === '') return
  const parsed = Number(raw)
  if (!Number.isInteger(parsed) || parsed < 0 || parsed > SHM_FIELD_MAX[field]) return
  const old = shmCfg.value?.[field]
  if (old !== undefined && parsed === old) return
  // rethrow 型薄代理: blur 路径吞 rejection（错误已由 store.error → View toast 承载）
  store.setShmConfig(src.value.id, { [field]: parsed } as ShmConfigPatch).catch(() => {})
}

// 预加载点行内 pages/bytes 失焦: 从镜像读当前值拼完整 value 下发（递归合并按 key 覆盖）。
// pages ∈ [0,8]; bytes ∈ [0, 2^40]（字节数可达 TB 级, 不与 pages 同限）
function onPreloadBlur(time: string, field: 'pages' | 'bytes', event: Event): void {
  const raw = (event.target as HTMLInputElement).value.trim()
  if (raw === '') return
  const parsed = Number(raw)
  if (!Number.isInteger(parsed) || parsed < 0) return
  if (field === 'pages' ? parsed > 8 : parsed > 2 ** 40) return
  const cur = shmCfg.value?.preload_points[time]
  if (cur && parsed === cur[field]) return
  const next = { pages: cur?.pages ?? 0, bytes: cur?.bytes ?? 0, [field]: parsed }
  store.setShmConfig(src.value.id, { preload_points: { [time]: next } }).catch(() => {})
}

// 删除预加载点: 契约 02 唯一合法 null 位置（preload_points 内 key 的 value=null）
function removePreloadPoint(time: string): void {
  store.setShmConfig(src.value.id, { preload_points: { [time]: null } }).catch(() => {})
}

// 添加预加载点 Modal（TimePicker 选 HH:MM + pages/bytes 输入）
const preloadModalOpen = ref(false)
const preloadTime = ref('08:45')
const preloadPages = ref('1')
const preloadBytes = ref('0')
const preloadError = ref<string | null>(null)

function openPreloadModal(): void {
  preloadTime.value = '08:45'
  preloadPages.value = '1'
  preloadBytes.value = '0'
  preloadError.value = null
  preloadModalOpen.value = true
}

async function confirmAddPreload(): Promise<void> {
  const time = preloadTime.value
  const pages = Number(preloadPages.value)
  const bytes = Number(preloadBytes.value)
  if (preloadEntries.value.some(([t]) => t === time)) {
    preloadError.value = '该时间点已存在'
    return
  }
  if (!Number.isInteger(pages) || pages < 0 || pages > 8
    || !Number.isInteger(bytes) || bytes < 0 || bytes > 2 ** 40) {
    preloadError.value = 'pages 需为 0-8 的整数，bytes 需为 0-2^40 的整数'
    return
  }
  try {
    await store.setShmConfig(src.value.id, { preload_points: { [time]: { pages, bytes } } })
    preloadModalOpen.value = false
  } catch {
    // 错误由 store error 字段承载（View 层 toast）；保持 Modal 打开
  }
}

// 订阅参数失焦提交: 值改变且满足契约范围才下发单字段 patch（契约 08 缺失=保留旧值）。
// 范围对照契约 08: batch_size > 0, delay_ms >= 0, interval_ms > 0, retry >= 0
// （与输入框 min 属性一致; 手输越界值前端拦截, 后端校验为最终防线）
type SubParamKey = 'subscribe_batch_size' | 'subscribe_batch_delay_ms'
  | 'sub_check_interval_ms' | 'sub_max_retry'
const SUB_PARAM_MIN: Record<SubParamKey, number> = {
  subscribe_batch_size: 1,
  subscribe_batch_delay_ms: 0,
  sub_check_interval_ms: 1,
  sub_max_retry: 0,
}
const subParamCurrent = (s: MarketSourceView, key: SubParamKey): number | null =>
  key === 'subscribe_batch_size' ? s.subscribeBatchSize
    : key === 'subscribe_batch_delay_ms' ? s.subscribeBatchDelayMs
    : key === 'sub_check_interval_ms' ? s.subCheckIntervalMs
    : s.subMaxRetry

function onSubParamBlur(s: MarketSourceView, key: SubParamKey, event: Event): void {
  const raw = (event.target as HTMLInputElement).value.trim()
  const old = subParamCurrent(s, key)
  if (raw === '') return                    // 清空不下发（无"删除参数"语义）
  const parsed = Number(raw)
  if (!Number.isInteger(parsed) || parsed < SUB_PARAM_MIN[key]) return  // 越界不下发
  if (old !== null && parsed === old) return // 值未改变不下发
  void store.setSubscribeParams(s.id, { [key]: parsed } as SubscribeParamsBody)
}
</script>

<template>
  <div class="source-card ds-card">
    <!-- Card Header -->
    <div class="source-card__header" @click="store.toggleExpand(src.id)">
      <span class="icon source-card__chevron" :class="{ 'is-expanded': src.expanded }" :style="{ width: '16px', height: '16px', color: 'var(--icon-tertiary)' }" aria-hidden="true">
        <Icon name="arrow-right" :size="16" />
      </span>
      <div class="source-card__info">
        <span class="source-card__info-name">{{ src.display_name || src.source_name }}</span>
        <span class="source-card__info-meta">{{ src.source_name }}</span>
      </div>
      <div class="source-card__meta-row">
        <span class="source-card__info-meta">交易日：<span>{{ src.tradingDay }}</span></span>
        <span class="source-card__info-sub" :class="subscribeClass(src)">订阅：<span>{{ src.subscribeCount }}/{{ src.subscribeTotal }}</span></span>
        <span class="source-card__info-meta" :style="{ color: src.auto_login ? 'var(--status-success-default)' : 'var(--status-alert-default)' }">自动登录：<span>{{ autoLoginStatusText(src) }}</span></span>
        <span class="source-card__info-meta" :style="{ color: processStateColor(src.process_state) }">进程：<span>{{ processStateText(src.process_state) }}</span></span>
      </div>
      <StatusIndicator
        :current="src.progressView.current"
        :max="src.progressView.max"
        :min="src.progressView.min"
        :desc="src.progressView.desc"
        :idle-text="'未登录'"
        :loading-text="'登录中'"
        :done-text="'已登录'"
        :mini="true"
      />
    </div>

    <!-- Card Body -->
    <div v-if="src.expanded" class="source-card__body is-open">
      <LoginPanel :source="src" />
      <ScheduleManager :source="src" />

      <!-- ── 网关信息（契约 09 CTP 类型只读状态，登录后由 RTN_MD_STATUS 填充；
               属 interface_type=ctp 范畴，xtp 等其他接口类型卡片自行设计展示）── -->
      <div class="card-section">
        <div class="card-section__row">
          <span class="card-section__title">网关信息</span>
        </div>
        <div class="gateway-info">
          <span class="gateway-info__item">API 版本：<span>{{ src.apiVersion || '--' }}</span></span>
          <span class="gateway-info__item">系统版本：<span>{{ src.sysVersion || '--' }}</span></span>
          <span class="gateway-info__item">登录时间：<span>{{ src.loginTime || '--' }}</span></span>
        </div>
      </div>

      <!-- ── 订阅参数（契约 08 SetSubscribeParams，无状态保护，缺失字段保留旧值）── -->
      <div class="card-section">
        <div class="card-section__row">
          <span class="card-section__title">订阅参数</span>
          <span v-if="src.subscribeParamsPending" class="card-section__title">保存中…</span>
        </div>
        <div class="sub-params">
          <div class="sub-params__field">
            <label>每批订阅数</label>
            <div class="ds-input">
              <input type="number" min="1" :value="src.subscribeBatchSize ?? ''"
                :disabled="src.subscribeParamsPending"
                @blur="onSubParamBlur(src, 'subscribe_batch_size', $event)">
            </div>
          </div>
          <div class="sub-params__field">
            <label>批间延迟(ms)</label>
            <div class="ds-input">
              <input type="number" min="0" :value="src.subscribeBatchDelayMs ?? ''"
                :disabled="src.subscribeParamsPending"
                @blur="onSubParamBlur(src, 'subscribe_batch_delay_ms', $event)">
            </div>
          </div>
          <div class="sub-params__field">
            <label>补订检查间隔(ms)</label>
            <div class="ds-input">
              <input type="number" min="1" :value="src.subCheckIntervalMs ?? ''"
                :disabled="src.subscribeParamsPending"
                @blur="onSubParamBlur(src, 'sub_check_interval_ms', $event)">
            </div>
          </div>
          <div class="sub-params__field">
            <label>补订最大重试</label>
            <div class="ds-input">
              <input type="number" min="0" :value="src.subMaxRetry ?? ''"
                :disabled="src.subscribeParamsPending"
                @blur="onSubParamBlur(src, 'sub_max_retry', $event)">
            </div>
          </div>
        </div>
      </div>

      <!-- ── 经纪商 ── -->
      <div class="card-section">
        <div class="card-section__row">
          <span class="card-section__title">经纪商</span>
          <!-- F-C4: 添加经纪商不受状态保护 (只扩展候选列表, 不改变当前连接) -->
          <button class="ds-btn ds-btn--tertiary ds-btn--sm" type="button" @click="openBrokerModal(src.id)">
            添加经纪商
          </button>
        </div>
        <div class="card-section__body">
          <BrokerSelector :source="src" />
          <BrokerCard v-for="broker in src.brokers" :key="broker.name" :source="src" :broker="broker" />
          <div v-if="src.brokers.length === 0" class="card-hint">
            暂无经纪商，点击"添加经纪商"创建
          </div>
        </div>
      </div>

      <!-- ── 高级（契约 02 SHM 行情通道配置, dzmd_* 通用层; 默认折叠）── -->
      <div class="card-section">
        <div class="card-section__row">
          <span class="card-section__title">高级</span>
          <button class="ds-btn ds-btn--tertiary ds-btn--sm" type="button" @click="advancedOpen = !advancedOpen">
            {{ advancedOpen ? '收起' : '展开' }}
          </button>
        </div>
        <template v-if="advancedOpen">
          <div class="gateway-info">
            <span class="gateway-info__item" title="仅可通过配置文件修改，进程启动前生效（契约 02）">
              页大小：<span>{{ shmCfg ? `${shmCfg.page_size_mb} MB` : '--' }}</span>
            </span>
            <span class="gateway-info__item">周期检查间隔(分)：
              <input class="shm-num-input" type="number" min="0" max="1440" :value="shmCfg?.check_interval_min ?? ''"
                :disabled="src.shmConfigPending" @blur="onShmNumBlur('check_interval_min', $event)">
            </span>
            <span class="gateway-info__item">检查预载页数：
              <input class="shm-num-input" type="number" min="0" max="8" :value="shmCfg?.check_pages ?? ''"
                :disabled="src.shmConfigPending" @blur="onShmNumBlur('check_pages', $event)">
            </span>
            <span class="gateway-info__item">检查预载字节：
              <input class="shm-num-input" type="number" min="0" :value="shmCfg?.check_bytes ?? ''"
                :disabled="src.shmConfigPending" @blur="onShmNumBlur('check_bytes', $event)">
            </span>
          </div>
          <div class="preload-list">
            <div class="card-section__row" style="margin-top: var(--spacer-8)">
              <span class="card-section__title">预加载点</span>
              <button class="ds-btn ds-btn--tertiary ds-btn--sm" type="button" @click="openPreloadModal">添加</button>
            </div>
            <div v-for="[time, pt] in preloadEntries" :key="time" class="preload-item">
              <span class="preload-item__time">{{ time }}</span>
              <label class="preload-item__field">页
                <input class="shm-num-input" type="number" min="0" max="8" :value="pt.pages"
                  :disabled="src.shmConfigPending" @blur="onPreloadBlur(time, 'pages', $event)">
              </label>
              <label class="preload-item__field">字节
                <input class="shm-num-input" type="number" min="0" :value="pt.bytes"
                  :disabled="src.shmConfigPending" @blur="onPreloadBlur(time, 'bytes', $event)">
              </label>
              <button class="preload-item__remove" type="button"
                :disabled="src.shmConfigPending" @click="removePreloadPoint(time)">删除</button>
            </div>
            <div v-if="preloadEntries.length === 0" class="card-hint">无预加载点</div>
          </div>
        </template>
      </div>

      <button
        class="ds-btn ds-btn--danger-subtle"
        :style="{ width: '100%', marginTop: 'var(--spacer-16)', justifyContent: 'center' }"
        type="button"
        :disabled="src.stopPending"
        @click="store.removeSource(src.id)"
      >
        <span v-if="src.stopPending" class="ds-btn__spinner"></span>
        {{ src.stopPending ? '停止中…' : '删除此行情源' }}
      </button>
    </div>

    <!-- ===== Add Broker Modal ===== -->
    <Modal :open="brokerModalOpen" title="添加经纪商" @close="closeBrokerModal">
      <div class="dialog-form">
        <div class="dialog-row">
          <label class="dialog-row__label">名称</label>
          <div class="ds-input dialog-row__control">
            <input v-model="newBrokerName" type="text" placeholder="例如：上海中期（创建后不可修改）">
          </div>
        </div>
        <div class="dialog-row">
          <label class="dialog-row__label">BrokerID</label>
          <div class="ds-input dialog-row__control">
            <input v-model="newBrokerBrokerId" type="text" placeholder="经纪商编号">
          </div>
        </div>
        <div class="dialog-row">
          <label class="dialog-row__label">UserID</label>
          <div class="ds-input dialog-row__control">
            <input v-model="newBrokerUserId" type="text" placeholder="用户代码">
          </div>
        </div>
        <div class="dialog-row">
          <label class="dialog-row__label">密码</label>
          <div class="ds-input dialog-row__control">
            <input v-model="newBrokerPassword" type="password" placeholder="密码">
          </div>
        </div>
        <div class="dialog-row">
          <label class="dialog-row__label">ProductInfo</label>
          <div class="ds-input dialog-row__control">
            <input v-model="newBrokerProductInfo" type="text" placeholder="客户端产品信息（可选）">
          </div>
        </div>
      </div>
      <template #footer>
        <button class="ds-btn ds-btn--secondary" type="button" @click="closeBrokerModal">取消</button>
        <button class="ds-btn ds-btn--primary" type="button"
          :disabled="brokerSubmitting || !newBrokerName.trim()"
          @click="confirmAddBroker">
          <span v-if="brokerSubmitting" class="ds-btn__spinner"></span>
          {{ brokerSubmitting ? '添加中…' : '确认' }}
        </button>
      </template>
    </Modal>

    <!-- ===== Add Preload Point Modal ===== -->
    <Modal :open="preloadModalOpen" title="添加预加载点" @close="preloadModalOpen = false">
      <div class="dialog-form">
        <div class="dialog-row">
          <label class="dialog-row__label">时间</label>
          <div class="dialog-row__control"><TimePicker v-model="preloadTime" /></div>
        </div>
        <div class="dialog-row">
          <label class="dialog-row__label">预加载页数</label>
          <div class="ds-input dialog-row__control">
            <input v-model="preloadPages" type="number" min="0" max="8" placeholder="0-8">
          </div>
        </div>
        <div class="dialog-row">
          <label class="dialog-row__label">预加载字节</label>
          <div class="ds-input dialog-row__control">
            <input v-model="preloadBytes" type="number" min="0" placeholder="字节数">
          </div>
        </div>
        <div v-if="preloadError" class="dialog-row__error">{{ preloadError }}</div>
      </div>
      <template #footer>
        <button class="ds-btn ds-btn--secondary" type="button" @click="preloadModalOpen = false">取消</button>
        <button class="ds-btn ds-btn--primary" type="button" @click="confirmAddPreload">添加</button>
      </template>
    </Modal>
  </div>
</template>

<style scoped>
.source-card { margin-bottom: var(--spacer-12); overflow: hidden; color: var(--text-default); min-width: 0; }
.source-card__header { display: flex; align-items: center; gap: var(--spacer-12); padding: var(--spacer-16) var(--spacer-20); cursor: pointer; flex-wrap: nowrap; overflow-x: auto; }
.source-card__header:hover { background: var(--bg-overlay-l1); }
.source-card__chevron { transition: transform 0.2s ease; flex-shrink: 0; }
.source-card__chevron.is-expanded { transform: rotate(90deg); }
.source-card__body { padding: 0 var(--spacer-20) var(--spacer-20); overflow-x: auto; }
.source-card__body.is-open { display: block; }

.source-card__info { display: flex; flex-direction: column; gap: 1px; flex-shrink: 0; }
.source-card__info-name { font-weight: var(--font-weight-medium, 500); color: var(--text-secondary); white-space: nowrap; }
.source-card__info-meta { font-size: 10px; color: var(--text-tertiary); white-space: nowrap; }
.source-card__info-sub { font-size: 10px; white-space: nowrap; }
.source-card__info-sub--ok { color: var(--text-tertiary); }
.source-card__info-sub--warn { color: var(--status-error-default); }
.source-card__meta-row { display: flex; align-items: center; gap: var(--spacer-12); flex-shrink: 1; flex-wrap: wrap; row-gap: 2px; }

/* 尾部 LoginPanel/ScheduleManager 为独立子组件, 本组件内最后一个 .card-section 是经纪商段, 去掉底边框 */
.card-section { padding-bottom: var(--spacer-12); border-bottom: 1px solid var(--border-neutral-l1); margin-bottom: var(--spacer-12); }
.card-section:last-of-type { border-bottom: none; margin-bottom: 0; padding-bottom: 0; }
.card-section__row { display: flex; align-items: center; justify-content: space-between; gap: var(--spacer-12); margin-bottom: var(--spacer-8); }
.card-section__title { font-size: var(--body-sm-font-size); font-weight: var(--font-weight-medium, 500); color: var(--text-default); }
.card-section__body { margin-top: var(--spacer-4); }
.card-hint { font-size: var(--body-sm-font-size); color: var(--text-tertiary); padding: var(--spacer-4) 0; }

/* 网关信息只读行 */
.gateway-info { display: flex; flex-wrap: wrap; gap: var(--spacer-4) var(--spacer-16); }
.gateway-info__item { font-size: var(--body-sm-font-size); color: var(--text-tertiary); white-space: nowrap; }
.gateway-info__item span { color: var(--text-secondary); font-variant-numeric: tabular-nums; }

/* 订阅参数网格 */
.sub-params { display: grid; grid-template-columns: repeat(auto-fit, minmax(180px, 1fr)); gap: var(--spacer-8) var(--spacer-16); }
.sub-params__field { display: flex; align-items: center; gap: var(--spacer-8); }
.sub-params__field label { font-size: var(--body-sm-font-size); color: var(--text-tertiary); flex-shrink: 0; }
.sub-params__field .ds-input { flex: 1; min-width: 0; }
.sub-params__field .ds-input input { font-family: var(--code-editor-font-family); font-variant-numeric: tabular-nums; }

/* 高级段: SHM 数字输入（无边框内联风格, 聚焦显现）与预加载点行 */
.shm-num-input {
  width: 72px; padding: 1px var(--spacer-4); font-size: var(--body-sm-font-size);
  font-family: var(--code-editor-font-family); font-variant-numeric: tabular-nums;
  color: var(--text-secondary); background: transparent;
  border: 1px solid transparent; border-radius: var(--radius-4);
}
.shm-num-input:focus { outline: none; border-color: var(--border-neutral-l1); background: var(--bg-base-secondary); }
.shm-num-input:disabled { opacity: 0.6; }
.preload-list { margin-top: var(--spacer-4); }
.preload-item { display: flex; align-items: center; gap: var(--spacer-8); height: 30px; padding: 0 var(--spacer-4); border-radius: var(--radius-6); }
.preload-item:hover { background: var(--bg-overlay-l1); }
.preload-item__time { font-family: var(--code-editor-font-family); font-variant-numeric: tabular-nums; font-size: var(--body-base-font-size); color: var(--text-default); min-width: 44px; }
.preload-item__field { display: inline-flex; align-items: center; gap: var(--spacer-4); font-size: var(--body-xs-font-size); color: var(--text-tertiary); }
.preload-item__remove { margin-left: auto; background: none; border: none; color: var(--text-tertiary); font-size: var(--body-sm-font-size); cursor: pointer; padding: var(--spacer-2) var(--spacer-6); border-radius: var(--radius-4); }
.preload-item__remove:not(:disabled):hover { color: var(--status-error-default); background: var(--status-error-surface-l1); }
.preload-item__remove:disabled { cursor: default; opacity: 0.6; }

/* 对话框错误提示（scoped 不跨组件, 本组件 Modal 自带） */
.dialog-row__error {
  font-size: var(--body-sm-font-size);
  color: var(--status-error-default);
  padding-left: 122px; /* 与 control 列对齐 (label 宽度 + gap) */
}

.dialog-form { display: flex; flex-direction: column; gap: var(--spacer-12); }
.dialog-row { display: flex; align-items: center; gap: var(--spacer-12); }
.dialog-row__label { font-size: var(--body-sm-font-size); color: var(--text-tertiary); font-weight: var(--font-weight-medium, 500); flex-shrink: 0; width: 110px; white-space: nowrap; }
.dialog-row__control { flex: 1; min-width: 0; }

.ds-btn__spinner { display: inline-block; width: 12px; height: 12px; border: 1.5px solid currentColor; border-top-color: transparent; border-radius: 50%; animation: ds-btn-spin 0.6s linear infinite; opacity: 0.7; flex-shrink: 0; }
@keyframes ds-btn-spin { to { transform: rotate(360deg); } }

@media (max-width: 768px) {
  .source-card__header { gap: var(--spacer-8); padding: var(--spacer-12); }
  .source-card__body { padding: 0 var(--spacer-12) var(--spacer-12); }
  .card-section__row { flex-wrap: wrap; }
}
</style>