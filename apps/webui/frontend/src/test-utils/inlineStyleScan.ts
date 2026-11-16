// `scripts/inline-style-scan.mjs` 的 TS 转发层（#346）。
//
// 为什么需要这一层：
//   判定逻辑必须与 stylelint 插件共用同一份实现（插件本身是 .mjs，只能 import JS），
//   而 vitest spec 是 TS，直接用相对路径 import .mjs 会因 tsconfig 的 moduleResolution
//   + 无类型声明而报 TS2307。在这里做一次带类型的转发，spec 侧就只依赖本文件。
export {
  classifyInlineStyleValue,
  scanInlineStyleViolations,
  type InlineStyleFinding,
} from '../../scripts/inline-style-scan.mjs'
