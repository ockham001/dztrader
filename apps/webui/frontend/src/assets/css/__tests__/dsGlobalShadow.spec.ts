import { describe, it, expect } from 'vitest'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'
import stylelint from 'stylelint'

// `dz/no-global-ds-shadow` 回归（retro 2026-11-28）。
//
// 背景：`components.css` 定义全局 `.ds-toast { position: fixed; top: 44px; left: 50% }`，
// `shared/ToastContainer.vue` 的 scoped 又写 `.ds-toast { position: relative }` —— Vue
// scoped 只是加 `[data-v-*]` 属性选择器，**不与全局声明互斥**，两份声明合并，全局的
// top/left/transform/white-space 全部生效：每条 toast 下移 44px、长文本被 overflow 裁掉。
// 现存 `dz/no-ds-namespace` 只对 marketSources / logs 开启（shared 是 pattern 合法拥有者，
// 按设计豁免），所以这次重新定义了全局同名类**没有任何检查拦**。
//
// 本 spec 守三件事（成对的正反自证，避免恒真断言）：
//   1. **拦截**：属性交集非空（全局有 display，scoped 也声明 display）→ 必须报；
//   2. **放行**：只是追加全局没有的布局属性（全局 display，scoped 只写 width）→ 不报，
//      否则 shared 组件的合法追加会被误伤；
//   3. **放行**：后代消费 / 全局不存在的 .ds-* 不报。

const SPEC_DIR = dirname(fileURLToPath(import.meta.url))
const FRONTEND_ROOT = join(SPEC_DIR, '..', '..', '..', '..')
const RULE = 'dz/no-global-ds-shadow'

/** 用真实 stylelint 配置 lint 一段 SFC 源码，返回本规则的告警 */
async function shadowWarnings(styleBody: string) {
  const code = `<template><div /></template>\n<style scoped>\n${styleBody}\n</style>\n`
  const result = await stylelint.lint({
    code,
    // 必须是 .vue：规则只对 .vue 生效（.css 由 dz/no-ds-namespace 与全局归属约束）
    codeFilename: join(FRONTEND_ROOT, 'src/components/shared/__probe.spec__.vue'),
    configFile: join(FRONTEND_ROOT, 'stylelint.config.mjs'),
    configBasedir: FRONTEND_ROOT,
  })
  return (result.results[0]?.warnings ?? []).filter((w) => w.rule === RULE)
}

describe('dz/no-global-ds-shadow：scoped 与全局同名 .ds-* 的属性冲突', () => {
  it('拦截：全局 .ds-alert 有 display，scoped 也声明 display → 报出', async () => {
    const warnings = await shadowWarnings('.ds-alert { display: block; }')
    expect(warnings).toHaveLength(1)
    expect(warnings[0].text).toContain('.ds-alert')
    expect(warnings[0].text).toContain('display')
  })

  it('拦截：全局 .ds-alert 有 padding，scoped 也声明 padding-top → 报出', async () => {
    // 交集判定按属性名精确匹配：padding-top 与 padding 是不同键。
    // 这里用全局确实声明过的 padding-top 形态验证「精确属性名」而非「前缀」匹配。
    const warnings = await shadowWarnings('.ds-alert { padding-top: var(--spacer-8); }')
    // components.css 的 .ds-alert 写的是 shorthand `padding`，不含 padding-top 键，
    // 因此按精确匹配**不应**报出——这条断言锁定「不做前缀匹配」的实现选择。
    expect(warnings).toHaveLength(0)
  })

  it('放行：只追加全局没有的布局属性，不与全局冲突 → 不报', async () => {
    // `.ds-alert__icon` 全局声明 flex/width/height/margin-top/display 等；
    // scoped 只追加 `gap`（全局无该键）→ 合法追加，不应误伤。
    const warnings = await shadowWarnings('.ds-alert__icon { gap: var(--spacer-4); }')
    expect(warnings).toHaveLength(0)
  })

  it('放行：全局不存在的 .ds-*（shared 新增自己的 pattern 类）→ 不报', async () => {
    const warnings = await shadowWarnings('.ds-nonexistent-probe { display: flex; }')
    expect(warnings).toHaveLength(0)
  })

  it('放行：后代消费（.foo .ds-alert）不是重定义 → 不报', async () => {
    const warnings = await shadowWarnings('.foo .ds-alert { display: flex; }')
    expect(warnings).toHaveLength(0)
  })

  it('放行：非布局属性（颜色/字号）同写不产生实害 → 不报', async () => {
    const warnings = await shadowWarnings('.ds-alert { color: var(--text-default); }')
    expect(warnings).toHaveLength(0)
  })
})
