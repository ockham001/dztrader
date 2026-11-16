import { describe, it, expect } from 'vitest'
import { readFileSync, readdirSync, statSync } from 'node:fs'
import { dirname, join, extname } from 'node:path'
import { fileURLToPath } from 'node:url'
import stylelint from 'stylelint'
import {
  classifyInlineStyleValue,
  scanInlineStyleViolations,
} from '@/test-utils/inlineStyleScan'

// 内联样式护栏（#346）：
// `dz/token-literal` 只扫 `.vue` 的 `<style>` 块，模板里的 `:style="{ fontSize: '10px' }"`
// 完全绕过它 —— 而 DashboardView.vue / PlaceholderView.vue 连 `<style>` 块都没有，
// stylelint 连规则都不会调用（postcss-html 对无样式块的 SFC 产出 0 个 root）。
//
// 本 spec 守两件事：
//   1. 判定口径（3 类）：字面量拦截 / 纯动态放行 / 显式注释放行；
//   2. 全仓实际状态：`src` 下所有 .vue 的模板 `:style` 里**零**静态声明。
//      第 2 条是 AC1 的机器化守卫，防「今天清零、明天又长回来」。
//
// 反向自证在 `判定口径` 段：拦截类必须真的产出 finding，放行类必须真的为空，
// 否则本 spec 会退化成恒绿断言（#309 的恒真断言防范）。

const SPEC_DIR = dirname(fileURLToPath(import.meta.url))
const FRONTEND_ROOT = join(SPEC_DIR, '..', '..', '..', '..')
const SRC_ROOT = join(FRONTEND_ROOT, 'src')

/** 递归收集 src 下全部 .vue（跳过 node_modules/dist 与测试目录） */
function collectVueFiles(dir: string, out: string[] = []): string[] {
  for (const name of readdirSync(dir)) {
    if (name === 'node_modules' || name === 'dist' || name === '__tests__') continue
    const full = join(dir, name)
    if (statSync(full).isDirectory()) collectVueFiles(full, out)
    else if (extname(name) === '.vue') out.push(full)
  }
  return out
}

/** 把一段模板片段包成最小 SFC —— 带 `<style>` 块。
 *  为什么必须有 style 块：postcss-html 对**不含** `<style>` 的 SFC 产出 0 个 postcss root，
 *  stylelint 因而根本不会调用任何规则（这正是不含 style 块的视图文件成为盲区的原因，
 *  由下面「全仓零静态内联样式」段用源码扫描兜住）。 */
const sfc = (template: string): string =>
  `<template>\n${template}\n</template>\n\n<script setup lang="ts">\nconst n = 1\n</script>\n\n<style scoped>\n.x { color: var(--text-default); }\n</style>\n`

const violationsOf = (template: string): ReturnType<typeof scanInlineStyleViolations> =>
  scanInlineStyleViolations(sfc(template), { filename: 'fixture.vue' })

describe('dz/no-inline-style-literal 判定口径（#346）', () => {
  describe('拦截：静态字面量', () => {
    it('字面量颜色被拦截', () => {
      const found = violationsOf(`  <div :style="{ color: 'red' }">x</div>`)
      expect(found).toHaveLength(1)
      expect(found[0].prop).toBe('color')
      expect(found[0].reason).toContain('静态字面量')
    })

    it('px 字号被拦截', () => {
      const found = violationsOf(`  <span :style="{ fontSize: '10px' }">x</span>`)
      expect(found.map((f) => f.prop)).toEqual(['fontSize'])
    })

    it('token 引用的静态值同样被拦截（静态就是静态，与是否已用 token 无关）', () => {
      const found = violationsOf(`  <div :style="{ color: 'var(--text-tertiary)' }">x</div>`)
      expect(found).toHaveLength(1)
    })

    it('三元表达式里的字面量分支被拦截', () => {
      const found = violationsOf(`  <div :style="{ opacity: disabled ? 0.5 : 1 }">x</div>`)
      expect(found.map((f) => f.prop)).toEqual(['opacity'])
    })

    it('字符串拼接里的字面量片段被拦截', () => {
      const found = violationsOf(`  <div :style="{ width: pct + '%' }">x</div>`)
      expect(found.map((f) => f.prop)).toEqual(['width'])
    })

    it('同一站点多个静态属性逐条报出（便于一次改完）', () => {
      const found = violationsOf(
        `  <div :style="{ display: 'flex', alignItems: 'center', color: 'var(--text-default)' }">x</div>`,
      )
      expect(found.map((f) => f.prop).sort()).toEqual(['alignItems', 'color', 'display'])
    })
  })

  describe('放行：纯动态值', () => {
    it('标识符/成员访问放行', () => {
      expect(violationsOf(`  <div :style="{ color: processStateColor(src.state) }">x</div>`)).toHaveLength(0)
    })

    it('整体绑定（非对象字面量）放行：判不了也不该判', () => {
      // 扫描口径文档化取舍：值整体是 ref/computed 时无法静态判定，交人工；
      // 它在 <script> 里构造的对象不在本扫描范围（票面 AC1 的口径是「模板里字面量形态」）。
      expect(violationsOf(`  <div :style="panelStyle">x</div>`)).toHaveLength(0)
      expect(violationsOf(`  <div :style="panelStyle" style="color: red">x</div>`)).toHaveLength(1)
    })

    it('带插值的模板串，插值外只有单位胶水 → 放行', () => {
      expect(violationsOf(`  <div :style="{ flex: \`\${flexGrow} 1 0%\` }">x</div>`)).toHaveLength(0)
      expect(violationsOf(`  <div :style="{ width: \`\${pct}%\` }">x</div>`)).toHaveLength(0)
      expect(violationsOf(`  <div :style="{ width: \`\${size}px\` }">x</div>`)).toHaveLength(0)
    })

    it('CSS 自定义属性赋值按值判定：动态放行', () => {
      expect(violationsOf(`  <div :style="{ '--icon-url': \`url('\${url}')\` }">x</div>`)).toHaveLength(0)
      expect(violationsOf(`  <div :style="{ '--fill': \`\${pct}%\` }">x</div>`)).toHaveLength(0)
    })

    it('模板串插值外含静态样式片段 → 拦截（不是真动态）', () => {
      const found = violationsOf(`  <div :style="{ border: \`1px solid \${color}\` }">x</div>`)
      expect(found.map((f) => f.prop)).toEqual(['border'])
      expect(found[0].reason).toContain('插值外')
    })
  })

  describe('放行：显式注释豁免', () => {
    it('注释紧邻 + 有理由 → 放行', () => {
      const found = violationsOf(
        `  <!-- dz-allow-inline-style: 拖拽位置必须逐实例计算 -->\n  <div :style="{ left: '10px', top: '20px' }">x</div>`,
      )
      expect(found).toHaveLength(0)
    })

    it('理由为空不算豁免（防万能开关）', () => {
      const found = violationsOf(
        `  <!-- dz-allow-inline-style:   -->\n  <div :style="{ left: '10px' }">x</div>`,
      )
      expect(found.map((f) => f.prop)).toEqual(['left'])
    })

    it('多行豁免注释（理由里带换行）同样生效', () => {
      const found = violationsOf(
        `  <!-- dz-allow-inline-style: 第一行理由\n       第二行补充说明 -->\n  <div :style="{ left: '10px' }">x</div>`,
      )
      expect(found).toHaveLength(0)
    })

    it('多行注释但理由为空 → 不豁免', () => {
      const found = violationsOf(
        `  <!-- dz-allow-inline-style:\n     -->\n  <div :style="{ left: '10px' }">x</div>`,
      )
      expect(found.map((f) => f.prop)).toEqual(['left'])
    })

    it('注释与元素之间隔着别的内容 → 不算紧邻，不豁免', () => {
      const found = violationsOf(
        `  <!-- dz-allow-inline-style: 理由 -->\n  <span>杂项</span>\n  <div :style="{ left: '10px' }">x</div>`,
      )
      expect(found.map((f) => f.prop)).toEqual(['left'])
    })
  })

  describe('多行与嵌套形态', () => {
    it('多行对象字面量按属性逐条报出', () => {
      const found = violationsOf(`  <div
    class="a"
    :style="{
      display: 'flex',
      gap: 'var(--spacer-6)',
    }"
  >x</div>`)
      expect(found.map((f) => f.prop).sort()).toEqual(['display', 'gap'])
    })

    it('行号指向 `:style` 属性本身（而不是模板首行）', () => {
      const source = `<template>\n  <div>\n    <span :style="{ color: 'red' }">x</span>\n  </div>\n</template>\n`
      const found = scanInlineStyleViolations(source, { filename: 'fixture.vue' })
      expect(found).toHaveLength(1)
      expect(found[0].line).toBe(3)
    })

    it('值是对象/数组字面量时不判（不是合法 CSS 值），只报确定的属性', () => {
      const found = violationsOf(`  <div :style="{ color: '『red』'.replace(/[『』]/g, ''), '--x': { a: 1 } }">x</div>`)
      expect(found.map((f) => f.prop)).toEqual(['color'])
    })
  })
})

describe('classifyInlineStyleValue 单元口径', () => {
  it('纯字面量字符串 → 拦截', () => {
    expect(classifyInlineStyleValue(`'flex'`)).toContain('静态字面量')
  })

  it('纯数字 → 拦截', () => {
    expect(classifyInlineStyleValue('0')).toContain('静态字面量')
  })

  it('标识符 → 放行', () => {
    expect(classifyInlineStyleValue('someValue')).toBeNull()
  })

  it('无插值模板串 → 拦截（等于静态字面量）', () => {
    expect(classifyInlineStyleValue('`flex`')).toContain('没有插值')
  })

  it('插值外含 var() → 拦截', () => {
    expect(classifyInlineStyleValue('`1px solid var(--x) ${c}`')).toContain('插值外')
  })
})

describe('扫描器健壮性（标签解析不走正则）', () => {
  // 这两条是 Standards 轴审查实测出来的缺陷回归用例：
  //   (a) `:style` 分支原先没限定 `<template>` 块 → `<style>` 注释里的旧写法会误报；
  //   (b) 静态 `style=` 原先用 `/<[a-zA-Z][^>]*?\sstyle=/` 匹配标签，属性值里出现 `>`
  //       （`@click="() => f()"`、`v-if="a > b"`）就会把其后的 `style="…"` 整段漏掉。
  it('`<style>` 块注释里的旧写法不误报', () => {
    const source = `<template>\n  <div class="x">ok</div>\n</template>\n\n<style scoped>\n/* 原内联 :style="{ color: 'red' }" 已归位 */\n.x { color: var(--text-default); }\n</style>\n`
    expect(scanInlineStyleViolations(source, { filename: 'f.vue' })).toEqual([])
  })

  it('`<script>` 字符串里的旧写法不误报', () => {
    const source = `<script setup lang="ts">\nconst doc = ':style="{ color: \\'red\\' }"'\n</script>\n<template>\n  <div class="x">ok</div>\n</template>\n`
    expect(scanInlineStyleViolations(source, { filename: 'f.vue' })).toEqual([])
  })

  it('属性值里含 `>` 时其后的静态 style 仍被检出（旧正则漏判）', () => {
    const arrow = violationsOf(`  <button @click="() => f()" style="color: var(--text-tertiary)">x</button>`)
    expect(arrow.map((f) => f.prop)).toEqual(['color'])
    const comparison = violationsOf(`  <div v-if="count > 3" style="padding: var(--spacer-8)">x</div>`)
    expect(comparison.map((f) => f.prop)).toEqual(['padding'])
  })

  it('多行标签里靠后的静态 style 仍被检出', () => {
    const found = violationsOf(
      `  <div\n    @click="() => go()"\n    style="padding: var(--spacer-8)"\n  >x</div>`,
    )
    expect(found.map((f) => f.prop)).toEqual(['padding'])
  })

  it('嵌套 <template v-if> 里的站点照样扫到', () => {
    const found = violationsOf(`  <template v-if="ok">\n    <div style="padding: 0">x</div>\n  </template>`)
    expect(found.map((f) => f.prop)).toEqual(['padding'])
  })
})

describe('拦截：静态 style 属性（无冒号形态，同属旁路）', () => {
  it('多声明静态 style 逐条报出', () => {
    const found = violationsOf(`  <div style="color: var(--text-tertiary); padding: var(--spacer-24)">x</div>`)
    expect(found.map((f) => f.prop).sort()).toEqual(['color', 'padding'])
    expect(found[0].reason).toContain('静态 style 属性')
  })

  it('列宽 style 也被拦截（不因属性「只是宽度」而放行）', () => {
    const found = violationsOf(`  <th style="width: 40px">x</th>`)
    expect(found.map((f) => f.prop)).toEqual(['width'])
  })

  it(':style 与 style 同时存在时各报各的', () => {
    const found = violationsOf(
      `  <div style="text-align: center" :style="{ color: 'var(--text-default)' }">x</div>`,
    )
    expect(found.map((f) => f.prop).sort()).toEqual(['color', 'text-align'])
  })

  it('只扫 <template> 块：<style> 块里的注释与字符串不算违规', () => {
    const source = `<template>\n  <div class="x">ok</div>\n</template>\n\n<style scoped>\n/* 原内联 style="padding: var(--spacer-16)" 已归位 */\n.x { color: var(--text-default); }\n</style>\n`
    expect(scanInlineStyleViolations(source, { filename: 'fixture.vue' })).toEqual([])
  })

  it('动态 :style 不因静态 style 的检测而误报', () => {
    const found = violationsOf(`  <div :style="{ width: pct + '%' }">x</div>`)
    expect(found.map((f) => f.prop)).toEqual(['width'])
  })
})

describe('dz/no-inline-style-literal 已接入 lint:css（#346 AC2）', () => {
  it('stylelint 对 .vue 的模板 :style 报出该规则，且**只报一次**', async () => {
    const result = await stylelint.lint({
      code: sfc(`  <div :style="{ color: 'red' }">x</div>`),
      codeFilename: join(FRONTEND_ROOT, 'src', 'views', '__lint_fixture.vue'),
      configFile: join(FRONTEND_ROOT, 'stylelint.config.mjs'),
      configBasedir: FRONTEND_ROOT,
    })
    const warnings = (result.results[0]?.warnings ?? []).filter((w) => w.rule === 'dz/no-inline-style-literal')
    // 条数必须精确为 1：postcss-html 会把一个 SFC 拆成多个 postcss root、对同一 result
    // 多次调用规则，插件的 scannedResults 去重专门解决这个 —— 断条数才能守住它
    // （只断言「含有该规则名」时，1 条 finding 报 2 条 warning 也会绿）。
    expect(warnings).toHaveLength(1)
  })

  it('规则对静态 style 属性同样报出', async () => {
    const result = await stylelint.lint({
      code: sfc(`  <th style="width: 40px">x</th>`),
      codeFilename: join(FRONTEND_ROOT, 'src', 'views', '__lint_fixture_static.vue'),
      configFile: join(FRONTEND_ROOT, 'stylelint.config.mjs'),
      configBasedir: FRONTEND_ROOT,
    })
    const warnings = (result.results[0]?.warnings ?? []).filter((w) => w.rule === 'dz/no-inline-style-literal')
    expect(warnings).toHaveLength(1)
  })

  it('规则对干净文件保持沉默（不是恒报）', async () => {
    const result = await stylelint.lint({
      code: sfc(`  <div :style="{ flex: \`\${n} 1 0%\` }">x</div>`),
      codeFilename: join(FRONTEND_ROOT, 'src', 'views', '__lint_fixture_clean.vue'),
      configFile: join(FRONTEND_ROOT, 'stylelint.config.mjs'),
      configBasedir: FRONTEND_ROOT,
    })
    const warnings = result.results[0]?.warnings ?? []
    expect(warnings.filter((w) => w.rule === 'dz/no-inline-style-literal')).toHaveLength(0)
  })
})

describe('全仓零静态内联样式（#346 AC1 机器化守卫）', () => {
  const vueFiles = collectVueFiles(SRC_ROOT)

  it('采集健全性：扫描到全部 .vue 且其中确有不含 <style> 块的文件', () => {
    expect(vueFiles.length).toBeGreaterThan(40)
    const withoutStyleBlock = vueFiles.filter((f) => !/<style/.test(readFileSync(f, 'utf8')))
    // 这个分支正是 stylelint 的盲区（无 <style> 块 → postcss-html 产出 0 个 root），
    // 名单变空说明采集失效，本守卫会静默空转。
    expect(withoutStyleBlock.length).toBeGreaterThan(0)
  })

  it('src 下所有 .vue 的模板内静态样式声明为 0 处（:style 字面量 + 静态 style 属性）', () => {
    const offenders: string[] = []
    for (const file of vueFiles) {
      const source = readFileSync(file, 'utf8')
      for (const finding of scanInlineStyleViolations(source, { filename: file })) {
        offenders.push(`${file}:${finding.line} <${finding.tag}> ${finding.prop}: ${finding.value}`)
      }
    }
    expect(offenders).toEqual([])
  })
})
