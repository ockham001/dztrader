import { describe, it, expect } from 'vitest'
import { visibleItemCount } from '../topbarOverflow'

// ADR 0048 顶栏资源组折叠算法：纯函数，逐条锁死「放得下多少项」。
// 组件测试只能验接入（jsdom 无布局），真正的边界在这里。

describe('visibleItemCount：按可用宽度决定直显项数量', () => {
  it('全部放得下 → 返回全部，且不预留溢出入口宽度', () => {
    // 60*3 + 14*2 = 208 ≤ 300
    expect(visibleItemCount([60, 60, 60], 300, 14, 34)).toBe(3)
    // 恰好相等也算放得下（不留 0px 余量给溢出入口）
    expect(visibleItemCount([60, 60, 60], 60 * 3 + 14 * 2, 14, 34)).toBe(3)
  })

  it('放不下 → 预算先扣溢出入口与一个间距，再从头部逐项累加', () => {
    // 5 项各 60 + 4 个间距 = 356 > 300；预算 300 - 34 - 14 = 252
    // 前 3 项 = 60*3 + 14*2 = 208 ≤ 252；前 4 项 = 282 > 252 → 3
    expect(visibleItemCount([60, 60, 60, 60, 60], 300, 14, 34)).toBe(3)
    // 预算 240-34-14 = 192：前 2 项 134 ≤ 192，前 3 项 208 > 192 → 2
    expect(visibleItemCount([60, 60, 60, 60], 240, 14, 34)).toBe(2)
  })

  it('连一项都放不下 → 0（调用方据此让全部项进浮层）', () => {
    // 预算 240-34-14 = 192 < 第一项 200
    expect(visibleItemCount([200, 200], 240, 14, 34)).toBe(0)
  })

  it('宽度未测量（0 / 负数 / NaN）→ 全部可见（jsdom 与首帧不得把徽章全折掉）', () => {
    expect(visibleItemCount([60, 60], 0, 14, 34)).toBe(2)
    expect(visibleItemCount([60, 60], -10, 14, 34)).toBe(2)
    expect(visibleItemCount([60, 60], Number.NaN, 14, 34)).toBe(2)
  })

  it('空项集合 → 0（不渲染溢出入口）', () => {
    expect(visibleItemCount([], 300, 14, 34)).toBe(0)
  })

  it('间距 / 溢出宽度非法时退化为 0，不产生负数预算把项误折掉', () => {
    // gap 非法 → 视为 0；overflowWidth 非法 → 视为 0
    expect(visibleItemCount([60, 60, 60], 200, Number.NaN, Number.NaN)).toBe(3)
    // 该项集合 180 ≤ 200 全可见
    expect(visibleItemCount([60, 60, 60], 179, 0, 0)).toBe(2)
  })
})
