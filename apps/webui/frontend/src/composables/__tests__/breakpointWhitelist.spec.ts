import { describe, it, expect } from 'vitest'
import { readFileSync, readdirSync, statSync } from 'node:fs'
import { join, extname, basename } from 'node:path'
import { fileURLToPath } from 'node:url'
import { BREAKPOINTS } from '../useBreakpoint'

// P5-T6 断点白名单（机器强制）：扫描 src 下全部 .css/.vue 的 @media 宽度断点，
// 档位必须 ∈ Tailwind 档 {sm 640, md 768, lg 1024, xl 1280}（767.98/639.98 等
// max-width 变体归一到上一档）。防止未来再冒出 720/900/480 这类野断点
// （"散断点不统一"是 P5 立项痛点之一）。
// 豁免：prefers-* / hover / pointer / orientation / -height 等非宽度媒体特征；
// 高度断点仅允许出现在 dashboard.css（监控页专属，见 layout.css 断点规范头）。

const FRONTEND_ROOT = join(fileURLToPath(import.meta.url), '..', '..', '..', '..')

/** 递归收集 .css/.vue 文件（跳过 node_modules/dist 与测试目录） */
function collectFiles(dir: string, out: string[] = []): string[] {
  for (const name of readdirSync(dir)) {
    if (name === 'node_modules' || name === 'dist' || name === '__tests__') continue
    const full = join(dir, name)
    if (statSync(full).isDirectory()) {
      collectFiles(full, out)
    } else if (extname(name) === '.css' || extname(name) === '.vue') {
      out.push(full)
    }
  }
  return out
}

/** 从 @media 条件文本提取宽度断点值（归一 .98 变体） */
function extractWidthBreakpoints(mediaText: string): number[] {
  const values: number[] = []
  const re = /\((?:min|max)-width:\s*([\d.]+)px\)/g
  let m: RegExpExecArray | null
  while ((m = re.exec(mediaText)) !== null) {
    let v = Number.parseFloat(m[1])
    // 767.98 这类"档位减 0.02"变体归一：向上取整到档位边界
    v = Math.ceil(v)
    values.push(v)
  }
  return values
}

describe('P5 断点白名单（machine-enforce）', () => {
  const files = collectFiles(join(FRONTEND_ROOT, 'src'))
  expect(files.length).toBeGreaterThan(10)  // 采集健全性护栏

  const violations: string[] = []
  const heightBreakpoints: string[] = []

  for (const file of files) {
    const text = readFileSync(file, 'utf8')
    // 匹配 @media(...)，容忍换行与组合条件
    const mediaRe = /@media[^{]+/g
    let mm: RegExpExecArray | null
    while ((mm = mediaRe.exec(text)) !== null) {
      const cond = mm[0]
      if (/prefers-|hover:|pointer:|orientation:/.test(cond)) continue  // 非宽度特征豁免
      if (/-height:/.test(cond)) {
        if (basename(file) !== 'dashboard.css') {
          heightBreakpoints.push(`${file}: ${cond.trim()}`)
        }
        continue  // 高度断点单独审计（仅 dashboard.css 允许）
      }
      for (const w of extractWidthBreakpoints(cond)) {
        if (!Object.values(BREAKPOINTS).includes(w as never)) {
          violations.push(`${file}: ${cond.trim()} → ${w}px 不在标准档 {640,768,1024,1280}`)
        }
      }
    }
  }

  it('全部宽度断点 ∈ Tailwind 标准档（sm/md/lg/xl）', () => {
    expect(violations, violations.join('\n')).toEqual([])
  })

  it('高度断点仅允许出现在 dashboard.css（监控页专属）', () => {
    expect(heightBreakpoints, heightBreakpoints.join('\n')).toEqual([])
  })
})