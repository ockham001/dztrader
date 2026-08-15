import type { Component } from 'vue'
import CtpCard from '@/components/marketSources/CtpCard.vue'
import GenericCard from '@/components/marketSources/GenericCard.vue'

// UI 卡片映射表: ui_card -> Component (契约 06)
// 未命中映射表时使用 GenericCard
// 新增大类只需在此添加映射 + card 组件, 不需改后端
const marketSourcesCardRegistry: Record<string, Component> = {
  ctp: CtpCard,
  // xtp: XtpCard,    // 暂未实现, 使用 GenericCard 回退
  // femas: FemasCard,
}

export function resolveCard(uiCard: string): Component {
  return marketSourcesCardRegistry[uiCard] ?? GenericCard
}
