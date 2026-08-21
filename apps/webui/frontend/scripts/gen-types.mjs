// JSON Schema → TypeScript 生成器（自研迷你编译器，零依赖）
// 输入：schema/domain-payloads.schema.json（WS 领域载荷单源）
// 输出：src/types/generated.ts（前端类型，禁止手改）
// 支持：$defs 容器内 object（required/properties/$ref/array-of-$ref）与 string enum → 字面量联合。
import { readFileSync, writeFileSync } from 'node:fs'
import { dirname, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'

const here = dirname(fileURLToPath(import.meta.url))
const schemaPath = resolve(here, '../schema/domain-payloads.schema.json')
const outPath = resolve(here, '../src/types/generated.ts')

const schema = JSON.parse(readFileSync(schemaPath, 'utf8'))

// 标量/引用类型到 TS 的映射（不含可选标记）
function tsType(propDef) {
  if (propDef.$ref) return propDef.$ref.split('/').pop()
  if (propDef.items?.$ref) return `${propDef.items.$ref.split('/').pop()}[]`
  if (propDef.type === 'string') return 'string'
  if (propDef.type === 'integer' || propDef.type === 'number') return 'number'
  if (propDef.type === 'boolean') return 'boolean'
  if (propDef.type === 'array') return 'unknown[]'
  if (propDef.type === 'object' && propDef.additionalProperties) {
    return `Record<string, ${tsType(propDef.additionalProperties)}>`
  }
  return 'unknown'
}

const lines = []
lines.push('// 由 scripts/gen-types.mjs 从 schema/domain-payloads.schema.json 生成 — 请勿手改')
lines.push('// 真源：docs/frame_contracts/*.md 与 libs/platform 头文件；改 schema 后重跑 npm run gen:types')
lines.push('')

for (const [name, def] of Object.entries(schema.$defs ?? {})) {
  if (def.type === 'string' && Array.isArray(def.enum)) {
    if (def.description) lines.push(`// ${def.description}`)
    lines.push(`export type ${name} = ${def.enum.map(v => `'${v}'`).join(' | ')}`)
    lines.push('')
  } else if (def.type === 'object') {
    if (def.description) lines.push(`// ${def.description}`)
    lines.push(`export interface ${name} {`)
    const props = def.properties ?? {}
    const required = new Set(def.required ?? [])
    for (const [k, v] of Object.entries(props)) {
      if (v.description) lines.push(`    /** ${v.description} */`)
      const opt = required.has(k) ? '' : '?'
      lines.push(`    ${k}${opt}: ${tsType(v)}`)
    }
    lines.push('}')
    lines.push('')
  }
}

writeFileSync(outPath, lines.join('\n'))
console.log(`generated: ${outPath}`)