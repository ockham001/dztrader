import { describe, it, expect } from 'vitest'
import { readFileSync } from 'node:fs'
import { join } from 'node:path'
import { fileURLToPath } from 'node:url'
import postcss, { type Root } from 'postcss'

// dashboard.css 作用域护栏（#347）：
// 本文件经 main.ts **全局**导入，且加载顺序在 components.css 之后（同特异性后者胜）。
// 于是任何**无前缀**的组件库类改写（`.ds-table` / `.ds-tag` / `.ds-alert` …）都会静默
// 改写全站，而不是只改仪表盘 —— #347 之前 `.ds-table` 就是这样把全站表头与字号改掉的。
//
// 不变量（回归即红）：
//   1. 引用组件库类（`.ds-*`）的每条选择器都必须带仪表盘作用域（`.dashboard-*` 前缀）；
//   2. 被测文件仍含表格规则（采集健全性护栏，防扫描收窄后静默空转）。
// 反向自证在最后一个 describe：谓词必须能对违规样例返回违规、对合规样例返回空，
// 否则本 spec 会退化成恒绿断言（#309 的恒真断言防范）。

const DASHBOARD_CSS = join(fileURLToPath(import.meta.url), '..', '..', 'dashboard.css')
const SOURCE = readFileSync(DASHBOARD_CSS, 'utf8')

/** 仪表盘作用域前缀：本文件里 `dashboard-panel` / `dashboard-demo-banner` 等 */
const DASHBOARD_SCOPE = /\.dashboard-[a-z0-9-]+/
/** 组件库类前缀：components.css 定义的 `.ds-*` 命名空间 */
const COMPONENT_CLASS = /\.ds-[a-z0-9-]+/

/** 返回引用了组件库类却没有仪表盘作用域前缀的选择器（去重、保持出现顺序） */
function unscopedComponentSelectors(root: Root): string[] {
  const offenders: string[] = []
  root.walkRules((rule) => {
    for (const selector of rule.selectors) {
      const trimmed = selector.trim()
      if (!COMPONENT_CLASS.test(trimmed)) continue
      if (DASHBOARD_SCOPE.test(trimmed)) continue
      const line = rule.source?.start?.line ?? '?'
      offenders.push(`line ${line}: ${trimmed.replace(/\s+/g, ' ')}`)
    }
  })
  return offenders
}

describe('#347 dashboard.css 作用域护栏', () => {
  it('可被标准 CSS 解析器解析', () => {
    expect(() => postcss.parse(SOURCE, { from: DASHBOARD_CSS })).not.toThrow()
  })

  it('引用组件库类（.ds-*）的选择器一律带 .dashboard-* 作用域', () => {
    const offenders = unscopedComponentSelectors(postcss.parse(SOURCE, { from: DASHBOARD_CSS }))
    // 失败信息里带行号与选择器，修复方式唯一：收进作用域，或删掉（若无消费点）
    expect(offenders, `dashboard.css 存在无前缀的组件库改写：\n${offenders.join('\n')}`).toEqual([])
  })

  it('仍包含面板内表格规则（采集健全性护栏）', () => {
    // 若上述断言因「规则被整段删除 / 扫描根收窄」而恒真，本用例会先红
    const root = postcss.parse(SOURCE, { from: DASHBOARD_CSS })
    const tableRules: string[] = []
    root.walkRules((rule) => {
      if (/\.dashboard-[a-z0-9-]+ .*\.ds-table/.test(rule.selector)) tableRules.push(rule.selector.trim())
    })
    expect(tableRules.length).toBeGreaterThan(0)
  })
})

describe('作用域谓词的反向自证（防止恒绿）', () => {
  it('违规样例被判红', () => {
    const root = postcss.parse(`
      .ds-table { font-size: var(--body-xs-font-size); }
      .ds-table thead th { text-transform: none; }
      .ds-tag--alert { color: var(--status-alert-default); }
      .dashboard-panel .ds-table { width: 100%; }
    `)
    const offenders = unscopedComponentSelectors(root)
    // 4 条选择器里只有 1 条合规；另 3 条必须全部被点名
    expect(offenders).toHaveLength(3)
    expect(offenders.some((entry) => entry.endsWith(': .ds-table'))).toBe(true)
    expect(offenders.some((entry) => entry.endsWith(': .ds-table thead th'))).toBe(true)
    expect(offenders.some((entry) => entry.endsWith(': .ds-tag--alert'))).toBe(true)
  })

  it('合规样例被判绿', () => {
    const root = postcss.parse(`
      .dashboard-panel .ds-table { width: 100%; }
      .dashboard-panel .ds-table thead th { text-transform: none; }
      .dashboard-demo-banner .ds-alert__icon { margin-top: 0; }
      .order-status-icon { color: var(--status-success-default); }
    `)
    expect(unscopedComponentSelectors(root)).toEqual([])
  })
})
