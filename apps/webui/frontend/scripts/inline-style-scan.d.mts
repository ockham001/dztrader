// `scripts/inline-style-scan.mjs` 的类型声明。
//
// 为什么声明与实现分离：
//   - 实现必须是纯 JS（.mjs）：stylelint 插件以 Node ESM 直接 import 它，插件本身也是 .mjs；
//   - 测试侧是 TS（vitest spec + 转发层 src/test-utils/inlineStyleScan.ts），需要类型。
// 文件名用 `.d.mts` 与 `.mjs` 配对，TS 的 moduleResolution: bundler 会自动取用。
// tsconfig 的 include 需含 `scripts/**/*.d.mts` —— 程序里没有这条 include 时，本文件不会
// 进入编译，导入方会退化成 TS7016（隐式 any）。

export interface InlineStyleFinding {
  /** 诊断用文件名（调用方传入，缺省 `<source>`） */
  file: string
  /** `:style` 属性起点所在行（1-based） */
  line: number
  /** `:style` 属性起点所在列（1-based） */
  column: number
  /** `:style` 属性起点的字符偏移 */
  offset: number
  /** 宿主元素标签名（小写，未知名以 `?` 表示） */
  tag: string
  /** 被判为静态的 CSS 属性名 */
  prop: string
  /** 该属性的表达式原文（空白已折叠） */
  value: string
  /** 拦截理由（给人看的说明，也是 spec 断言的稳定描述） */
  reason: string
}

/**
 * 判定单条内联样式属性值：放行返回 null，拦截返回理由。
 * 放行 = 纯动态值 / 带插值的模板串且插值外只有单位胶水；拦截 = 含静态字面量。
 */
export function classifyInlineStyleValue(raw: string): string | null

/** 扫描一份 SFC 源码里的内联静态样式违规（不含被 `dz-allow-inline-style` 豁免的站点）。 */
export function scanInlineStyleViolations(
  source: string,
  options?: { filename?: string },
): InlineStyleFinding[]
