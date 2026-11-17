import { describe, it, expect } from 'vitest'
import { readFileSync, readdirSync, statSync } from 'node:fs'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'
import stylelint from 'stylelint'

// token 红线护栏的**取值层**回归（#354）：
//
// #354 收窄了 `src/assets/css/**` 的 `dz/token-literal` 豁免 —— 原先整个目录豁免，等于在
// CSS 层留着一条与模板 `:style`（#346 已关）同构的旁路；收窄后 components/dashboard/layout
// 三个**消费方**文件落回红线，只有 design-tokens.css / theme.css 两个 **token 定义**文件保留
// 豁免（它们的正文就是 `--x: <字面量>`，字面量是产物而非违规）。
//
// 本 spec 守两件事：
//   1. 豁免边界：定义文件豁免、消费方文件纳管。收窄被误改回去（或新消费方文件被顺手加进
//      豁免 glob）时立刻红 —— 这是「旁路重新打开」的唯一机器信号；
//   2. `env()` 白名单的**双向**判定：`env(safe-area-inset-bottom, 0px)` 的 fallback px 必须
//      放行（它是 CSS 函数语法，不是设计字面量），但同一属性里的**其它** px 仍必须报出。
//      只测放行不测拦截，白名单会退化成「含 env() 就整条跳过」的假豁免。
//
// 反向自证在同一 describe 内成对出现（#309 的恒真断言防范）。

const SPEC_DIR = dirname(fileURLToPath(import.meta.url))
const FRONTEND_ROOT = join(SPEC_DIR, '..', '..', '..', '..')
const CSS_DIR = join(FRONTEND_ROOT, 'src', 'assets', 'css')

/** 用真实 stylelint 配置 lint 一段 CSS，返回 dz/token-literal 的告警 */
async function tokenLiteralWarnings(code: string, codeFilename: string) {
  const result = await stylelint.lint({
    code,
    codeFilename: join(FRONTEND_ROOT, codeFilename),
    configFile: join(FRONTEND_ROOT, 'stylelint.config.mjs'),
    configBasedir: FRONTEND_ROOT,
  })
  return (result.results[0]?.warnings ?? []).filter((w) => w.rule === 'dz/token-literal')
}

describe('#354 token 红线：豁免边界', () => {
  it('消费方文件（components/dashboard/layout）落回红线：裸 px 与裸色都被报出', async () => {
    const warnings = await tokenLiteralWarnings(
      '.x { gap: 2px; border-radius: 3px; font-size: 10px; background: #FFFFFF; }\n',
      'src/assets/css/__fixture_consumer.css',
    )
    // 4 条声明全是字面量，必须 4 条全报 —— 少报任何一条都说明某类判定失效
    expect(warnings).toHaveLength(4)
  })

  it('token 定义文件仍豁免（design-tokens.css 的 `--x: #FFFFFF` 不报）', async () => {
    const warnings = await tokenLiteralWarnings(
      ':root { --special-white: #FFFFFF; --spacer-1: 1px; }\n',
      'src/assets/css/design-tokens.css',
    )
    expect(warnings).toHaveLength(0)
  })

  it('token 定义文件仍豁免（theme.css 同理）', async () => {
    const warnings = await tokenLiteralWarnings(
      ':root { --bg-base-default: #FFFFFF; --spacer-8: 10px; }\n',
      'src/assets/css/theme.css',
    )
    expect(warnings).toHaveLength(0)
  })

  it('豁免名单不得扩张：只有两个 token 定义文件在豁免里', () => {
    // 按**行**定位，不做注释剥离 —— 注释里含 `**/*.vue` 这类文本，正则剥注释会把 glob
    // 里的 `/*` 当成注释起点一并吃掉（实测只剩 1 个锚点且匹配错位置）。
    const lines = readFileSync(join(FRONTEND_ROOT, 'stylelint.config.mjs'), 'utf8').split('\n')
    const exempt: string[][] = []
    for (let i = 0; i < lines.length; i++) {
      const m = lines[i].match(/^\s*files:\s*\[([^\]]*)\]/)
      if (!m) continue
      // 该 override 的 rules 体一直延伸到本段结束（遇到下一个 `files:` 行或 `]` 收尾）
      let body = ''
      for (let j = i + 1; j < Math.min(i + 30, lines.length); j++) {
        if (/^\s*files:\s*\[/.test(lines[j])) break
        body += lines[j] + '\n'
      }
      if (body.includes("'dz/token-literal': null")) {
        exempt.push(
          m[1]
            .split(',')
            .map((s) => s.trim().replace(/^['"]|['"]$/g, ''))
            .filter(Boolean)
            .sort(),
        )
      }
    }
    expect(exempt.length, '应当只有一段 override 关掉 dz/token-literal').toBe(1)
    expect(exempt[0]).toEqual(['src/assets/css/design-tokens.css', 'src/assets/css/theme.css'])
  })

  it('实测：三个消费方文件当前零违规（收窄后确实无需再豁免）', async () => {
    const offenders: string[] = []
    for (const name of ['components.css', 'dashboard.css', 'layout.css']) {
      const warnings = await stylelint.lint({
        files: join(CSS_DIR, name),
        configFile: join(FRONTEND_ROOT, 'stylelint.config.mjs'),
        configBasedir: FRONTEND_ROOT,
      })
      for (const r of warnings.results) {
        for (const w of r.warnings) {
          if (w.rule === 'dz/token-literal') offenders.push(`${name}:${w.line} ${w.text}`)
        }
      }
    }
    expect(offenders).toEqual([])
  })
})

describe('#354 env() 白名单：双向判定', () => {
  it('env() 的 fallback px 放行（iOS 安全区形态）', async () => {
    const warnings = await tokenLiteralWarnings(
      '.x { padding-bottom: calc(var(--spacer-16) + env(safe-area-inset-bottom, 0px)); }\n',
      'src/assets/css/__fixture_env_ok.css',
    )
    expect(warnings).toHaveLength(0)
  })

  it('env() 里嵌套 var()/calc() 同样放行（括号配对正确，非贪婪正则在此会漏）', async () => {
    const warnings = await tokenLiteralWarnings(
      '.x { padding: calc(env(safe-area-inset-top, var(--spacer-0, 0px)) + 2px); }\n',
      'src/assets/css/__fixture_env_nested.css',
    )
    // 外层 calc 里的 `2px` 是**真**字面量，必须仍被报出；env() 内的两个 px 放行。
    expect(warnings).toHaveLength(1)
  })

  it('同一属性里 env() 之外的 px 仍报出（白名单不是整条豁免）', async () => {
    const warnings = await tokenLiteralWarnings(
      '.x { padding: calc(4px + env(safe-area-inset-bottom, 0px)); }\n',
      'src/assets/css/__fixture_env_mixed.css',
    )
    expect(warnings).toHaveLength(1)
  })

  it('无 env() 的裸 px 照常报出（白名单不影响常规路径）', async () => {
    const warnings = await tokenLiteralWarnings(
      '.x { padding-bottom: calc(var(--spacer-16) + 0px); }\n',
      'src/assets/css/__fixture_no_env.css',
    )
    expect(warnings).toHaveLength(1)
  })
})
