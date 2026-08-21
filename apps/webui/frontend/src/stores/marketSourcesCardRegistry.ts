import type { Component } from 'vue'
import CtpCard from '@/components/marketSources/CtpCard.vue'
import GenericCard from '@/components/marketSources/GenericCard.vue'

// =====================================================================
// 行情源 UI 卡片注册表（P3 任务 3 领域功能模板：新增行情源类型的唯一注册点）
//
// 四件套契约（新增一个行情源类型，如 XTP）：
//   1. store —— 无需改：marketSources/mdConfig/process 均为通用聚合，由复合 computed
//      `sources` 拼出 MarketSourceView，前端卡片无感知；
//   2. api   —— 无需改：marketSourcesApi 全部为通用端点（login/broker/frontend…）；
//   3. view  —— 无需改：MarketSourcesView 用 `resolveCard(src.ui_card)` 动态渲染；
//   4. 组件注册 —— 唯一要动的点：新建 <Type>Card.vue（复制 MarketSourceCardTemplate.vue）
//      并在下方映射表中加一行。后端 ui_card 由 `extract_ui_card(source_name)` 自动算出
//      （dzmd_xtp → "xtp"，恒小写），前端注册表 key 必须与其一致（小写）。
//
// 键约定（机器强制项，见 marketSourcesCardRegistry.spec.ts）：
//   - key = extract_ui_card 输出，恒小写（xtp / femas / …）；
//   - resolveCard 会先归一化（trim+toLowerCase），DB 里误存 'CtpCard' 也不会回退 GenericCard。
// =====================================================================
const marketSourcesCardRegistry: Record<string, Component> = {
  ctp: CtpCard,
  // xtp: XtpCard,    // 暂未实现, 使用 GenericCard 回退
  // femas: FemasCard,
}

/** 解析 ui_card → 卡片组件；未命中映射表时回退 GenericCard（未知大类兜底展示）。
 *  归一化处理 trim + 小写，使键契约对大小写/空白免疫（防 DB 值误存样式导致悄悄回退）。 */
export function resolveCard(uiCard: string): Component {
  const key = uiCard.trim().toLowerCase()
  return marketSourcesCardRegistry[key] ?? GenericCard
}