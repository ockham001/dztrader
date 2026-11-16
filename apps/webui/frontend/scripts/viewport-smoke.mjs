#!/usr/bin/env node
// 五档视口冒烟（#348 检查 3，母票 #345 T0-c）
//
// 为什么单独成脚本、不进 CI：它需要一台真实浏览器 + 一个跑起来的 dev 栈（8080）。
// 在 CI 里起栈 + 起浏览器的成本远高于它抓到的东西，所以本波只要求「本地可跑」，
// CI 接入留给后续票（#348「不做」段）。vitest 侧的检查 1/2 是纯静态分析，已经覆盖
// token 层；本脚本覆盖的是**布局层**——静态分析看不到的横向溢出与「文字被底色吞掉」。
//
// 零依赖：只用 Node 内置模块（node:fs / node:child_process / 内置 fetch / 内置 WebSocket，
// 需 Node ≥ 22）。仓库没有 playwright / puppeteer 依赖，装不了也不该为脚本新增重依赖，
// 所以直接说 CDP（Chrome DevTools Protocol）。
//
// 用法：
//   node scripts/viewport-smoke.mjs --help
//
// 举例：
//   # 栈已在 8080 跑着；用 dev home 里配好的口令登录（默认口令账户首登强制改密，
//   # 那种情况本脚本直接报错退出，不代做改密流程）
//   DZ_SMOKE_USER=admin DZ_SMOKE_PASS=... node scripts/viewport-smoke.mjs
//   node scripts/viewport-smoke.mjs --token "$JWT" --routes /,/logs
//   node scripts/viewport-smoke.mjs --chrome /path/to/chrome --json result.json
//
// 断言（每个 主题 × 视口 × 路由 组合）：
//   1. 无横向溢出：documentElement.scrollWidth ≤ clientWidth（允许 1px 亚像素误差）
//   2. 无「文字色 == 有效底色」的元素：正文自身色与合成后的背景色对比度 ≥ --min-ratio
//      （默认 1.15，即只抓「基本看不见」，不重复 token 门禁的 4.5:1 判定）
//   3. 主题真的生效：<html> 的 class 与请求的主题一致（抓 useTheme 未挂载的顺序问题）
//
// 已知限制（写在这免得被当成 bug 追）：
//   - 只检查元素自身文本（childNodes 里的文本节点），不检查 ::before/::after 伪元素文本
//   - 不检查 canvas / img 内部的像素（ECharts 图表不在覆盖范围）
//   - 不检查键盘可达性 / 焦点环（#332 的范围）
//   - 需要**已登录**的会话：脚本不处理「默认口令强制改密」，请用改了密的 dev home 起栈

import { spawn } from 'node:child_process'
import { existsSync, writeFileSync } from 'node:fs'

// ---------------------------------------------------------------------------
// 参数
// ---------------------------------------------------------------------------

const HELP = `五档视口冒烟（#348 检查 3）——零依赖 CDP 客户端

用法
  node scripts/viewport-smoke.mjs [选项]

连接
  --base <url>         站点根，默认 http://127.0.0.1:8080（dzweb 的 HTTP 端口，不是 5173）
  --token <jwt>        已登录会话 token；也可用环境变量 DZ_SMOKE_TOKEN
  --user <name>        用账号密码登录；也可用环境变量 DZ_SMOKE_USER（默认 admin）
  --pass <word>        配合 --user；也可用环境变量 DZ_SMOKE_PASS
  --cdp <url>          复用已开着的浏览器调试端点（如 http://127.0.0.1:9222），不自己启动 Chrome
  --chrome <path>      Chrome 可执行文件；默认依次探测 CHROME_PATH 与常见安装路径

视口（--viewport 可重复指定 name 或 name=宽x高）
  默认跑五档：desktop1600=1600x900、laptop1366=1366x768、tablet1024=1024x768、
             tablet768=768x1024、mobile390=390x844

范围
  --routes <a,b,c>     只跑这些路由（默认全部已登记路由）
  --themes <light,dark> 只跑这些主题（默认浅色 + 深色）
  --settle <ms>        每次导航后等待渲染稳定，默认 1200
  --min-ratio <n>      正文/底色对比度下限，默认 1.15（低于它视为「看不见」）
  --max-findings <n>   每页最多列出多少条 finding，默认 8
  --keep-open          跑完不关浏览器（配合 --cdp 调试用）
  --json [path]        结果另存为 JSON（不写路径则打印到 stdout）
  --verbose            打印每个组合的通过情况（默认只打印失败）
  -h, --help           显示本帮助

退出码
  0 = 全部通过    1 = 有 finding    2 = 环境/参数错误（浏览器或栈不可用）

前置条件
  - Node ≥ 22（需要内置 WebSocket）；无第三方依赖
  - dev 栈已起来且 8080 可访问：DZTRADER_HOME=<项目内目录> ./scripts/run-dev.sh
  - 账号已改过密（系统默认口令的首登强制改密不在本脚本处理范围）
`

/** 极简参数解析：只支持 --key value / --flag，够用且不引依赖 */
function parseArgs(argv) {
  const opts = {
    base: 'http://127.0.0.1:8080',
    token: process.env.DZ_SMOKE_TOKEN,
    user: process.env.DZ_SMOKE_USER || 'admin',
    pass: process.env.DZ_SMOKE_PASS,
    cdp: null,
    chrome: process.env.CHROME_PATH || null,
    viewports: null,
    routes: null,
    themes: ['light', 'dark'],
    settle: 1200,
    minRatio: 1.15,
    maxFindings: 8,
    keepOpen: false,
    json: null,
    verbose: false,
  }
  for (let i = 0; i < argv.length; i++) {
    const arg = argv[i]
    const next = () => {
      const v = argv[++i]
      if (v === undefined) fail(`缺少参数值: ${arg}`)
      return v
    }
    switch (arg) {
      case '-h':
      case '--help':
        process.stdout.write(HELP)
        process.exit(0)
        break
      case '--base': opts.base = next().replace(/\/+$/, ''); break
      case '--token': opts.token = next(); break
      case '--user': opts.user = next(); break
      case '--pass': opts.pass = next(); break
      case '--cdp': opts.cdp = next().replace(/\/+$/, ''); break
      case '--chrome': opts.chrome = next(); break
      case '--viewport': (opts.viewports ??= []).push(next()); break
      case '--routes': opts.routes = next().split(',').map((s) => s.trim()).filter(Boolean); break
      case '--themes': opts.themes = next().split(',').map((s) => s.trim()).filter(Boolean); break
      case '--settle': opts.settle = Number(next()); break
      case '--min-ratio': opts.minRatio = Number(next()); break
      case '--max-findings': opts.maxFindings = Number(next()); break
      case '--keep-open': opts.keepOpen = true; break
      case '--json': {
        const maybePath = argv[i + 1]
        if (maybePath && !maybePath.startsWith('-')) { opts.json = next() } else { opts.json = '-' }
        break
      }
      case '--verbose': opts.verbose = true; break
      default:
        fail(`未知参数: ${arg}（--help 看用法）`)
    }
  }
  return opts
}

function fail(message) {
  process.stderr.write(`viewport-smoke: ${message}\n`)
  process.exit(2)
}

// ---------------------------------------------------------------------------
// 常量：视口与路由
// ---------------------------------------------------------------------------

/** 默认五档视口（#348 验收标准 5）。高度对 1024/768 两档取竖屏尺寸（平板手持场景）。 */
const DEFAULT_VIEWPORTS = [
  { name: 'desktop1600', width: 1600, height: 900 },
  { name: 'laptop1366', width: 1366, height: 768 },
  { name: 'tablet1024', width: 1024, height: 768 },
  { name: 'tablet768', width: 768, height: 1024 },
  { name: 'mobile390', width: 390, height: 844 },
]

/**
 * 已落地的应用路由（与 src/router/routes.ts 的 appRoutes 对齐）。
 * 刻意不解析 routes.ts：那需要 TS 解析依赖；新增路由时同步这里，漏了不会报错但会少覆盖。
 * 排除 /login（无需登录态，另有其专项验收）与 4 个 wip 占位页（同一 PlaceholderView）。
 */
const DEFAULT_ROUTES = [
  '/',
  '/market-sources',
  '/trading',
  '/trading-data',
  '/logs',
  '/monitor',
  '/user-management',
  '/settings',
]

/** 探测 Chrome：环境变量 → 本机 chrome-for-testing → 常见包管理器路径 */
function detectChrome(explicit) {
  if (explicit) {
    if (!existsSync(explicit)) fail(`--chrome 指向的文件不存在: ${explicit}`)
    return explicit
  }
  const candidates = [
    '/home/ockham/.local/share/chrome-for-testing/chrome-linux64/chrome',
    '/usr/bin/google-chrome',
    '/usr/bin/google-chrome-stable',
    '/usr/bin/chromium',
    '/usr/bin/chromium-browser',
    '/snap/bin/chromium',
  ]
  const found = candidates.find((p) => existsSync(p))
  if (!found) {
    fail(
      '找不到 Chrome/Chromium。用 --chrome <path> 指定，或设置 CHROME_PATH。\n' +
        '（本仓未引入 playwright/puppeteer 依赖：脚本走原生 CDP）',
    )
  }
  return found
}

// ---------------------------------------------------------------------------
// CDP 客户端（内置 WebSocket，无依赖）
// ---------------------------------------------------------------------------

const sleep = (ms) => new Promise((r) => setTimeout(r, ms))

async function getJson(url, init) {
  const res = await fetch(url, init)
  if (!res.ok) throw new Error(`${url} → HTTP ${res.status}`)
  return res.json()
}

/** 启动 headless Chrome，返回 { proc, endpoint } */
async function launchChrome(chromePath, port) {
  const userDataDir = `/tmp/dz-viewport-smoke-${process.pid}`
  const proc = spawn(
    chromePath,
    [
      '--headless=new',
      `--remote-debugging-port=${port}`,
      '--no-sandbox',
      '--disable-gpu',
      '--disable-dev-shm-usage',
      '--hide-scrollbars',
      '--no-first-run',
      '--no-default-browser-check',
      `--user-data-dir=${userDataDir}`,
      'about:blank',
    ],
    { stdio: 'ignore', detached: false },
  )
  const endpoint = `http://127.0.0.1:${port}`
  const deadline = Date.now() + 20000
  while (Date.now() < deadline) {
    try {
      await getJson(`${endpoint}/json/version`)
      return { proc, endpoint }
    } catch {
      await sleep(250)
    }
  }
  proc.kill('SIGKILL')
  fail(`Chrome 启动超时（20s），端点 ${endpoint} 不可达`)
}

/** 打开一个 CDP 会话，返回 { send, on, close } */
function connect(wsUrl) {
  return new Promise((resolve, reject) => {
    const ws = new WebSocket(wsUrl)
    let nextId = 1
    const pending = new Map()
    const handlers = new Map()
    ws.addEventListener('open', () => {
      resolve({
        send(method, params = {}) {
          const id = nextId++
          return new Promise((res, rej) => {
            pending.set(id, { res, rej })
            ws.send(JSON.stringify({ id, method, params }))
          })
        },
        on(event, handler) {
          if (!handlers.has(event)) handlers.set(event, [])
          handlers.get(event).push(handler)
        },
        close() {
          ws.close()
        },
      })
    })
    ws.addEventListener('message', (ev) => {
      const msg = JSON.parse(ev.data)
      if (msg.id && pending.has(msg.id)) {
        const { res, rej } = pending.get(msg.id)
        pending.delete(msg.id)
        if (msg.error) rej(new Error(`${msg.error.message ?? JSON.stringify(msg.error)}`))
        else res(msg.result)
        return
      }
      if (msg.method && handlers.has(msg.method)) {
        for (const h of handlers.get(msg.method)) h(msg.params)
      }
    })
    ws.addEventListener('error', (e) => reject(new Error(`WebSocket 失败: ${e.message ?? e}`)))
  })
}

/** 连到第一个 page target（Chrome 启动时带 about:blank，必然存在） */
async function connectPage(endpoint) {
  const targets = await getJson(`${endpoint}/json/list`)
  const page = targets.find((t) => t.type === 'page')
  if (!page) fail('浏览器里没有 page target（Chrome 版本差异？）')
  const client = await connect(page.webSocketDebuggerUrl)
  await client.send('Page.enable')
  await client.send('Runtime.enable')
  return client
}

async function evaluate(client, expression) {
  const result = await client.send('Runtime.evaluate', {
    expression,
    awaitPromise: true,
    returnByValue: true,
  })
  if (result.exceptionDetails) {
    const desc = result.exceptionDetails.exception?.description ?? JSON.stringify(result.exceptionDetails)
    throw new Error(`页面内脚本抛错: ${desc}`)
  }
  return result.result.value
}

/** 导航并等 load 事件（不依赖固定 sleep 判定加载完成） */
async function navigate(client, url, timeoutMs = 20000) {
  let loadFired = false
  const onLoad = () => {
    loadFired = true
  }
  client.on('Page.loadEventFired', onLoad)
  await client.send('Page.navigate', { url })
  const deadline = Date.now() + timeoutMs
  while (!loadFired && Date.now() < deadline) await sleep(60)
  if (!loadFired) throw new Error(`导航超时（${timeoutMs}ms）: ${url}`)
}

// ---------------------------------------------------------------------------
// 页面内审计（作为字符串注入，只依赖浏览器 API）
// ---------------------------------------------------------------------------

/**
 * 在页面里跑的三项检查。写成独立函数再 toString() 注入，便于本地阅读与改。
 * 返回 { overflow, invisible, themeClass, docWidth, scrollWidth, findings }
 */
function pageAudit({ minRatio, maxFindings }) {
  const parseColor = (text) => {
    const m = /rgba?\(([^)]+)\)/.exec(text || '')
    if (!m) return null
    const parts = m[1].split(/[,\s/]+/).filter(Boolean).map(Number)
    if (parts.length < 3) return null
    return { r: parts[0], g: parts[1], b: parts[2], a: parts.length > 3 ? parts[3] : 1 }
  }
  const composite = (fg, bg) => {
    const a = fg.a
    return {
      r: fg.r * a + bg.r * (1 - a),
      g: fg.g * a + bg.g * (1 - a),
      b: fg.b * a + bg.b * (1 - a),
      a: 1,
    }
  }
  const channel = (c) => {
    const s = c / 255
    return s <= 0.03928 ? s / 12.92 : ((s + 0.055) / 1.055) ** 2.4
  }
  const luminance = (c) => 0.2126 * channel(c.r) + 0.7152 * channel(c.g) + 0.0722 * channel(c.b)
  const ratio = (a, b) => {
    const la = luminance(a)
    const lb = luminance(b)
    return (Math.max(la, lb) + 0.05) / (Math.min(la, lb) + 0.05)
  }

  // 有效底色：从根元素往下逐层合成（canvas 默认白）
  const canvas = { r: 255, g: 255, b: 255, a: 1 }
  const effectiveBackground = (el) => {
    const chain = []
    for (let node = el; node && node.nodeType === 1; node = node.parentElement) chain.push(node)
    chain.reverse()
    let bg = canvas
    for (const node of chain) {
      const own = parseColor(getComputedStyle(node).backgroundColor)
      if (own && own.a > 0) bg = own.a >= 1 ? own : composite(own, bg)
    }
    return bg
  }

  const describe = (el) => {
    const id = el.id ? `#${el.id}` : ''
    const cls = typeof el.className === 'string' && el.className
      ? `.${el.className.trim().split(/\s+/).slice(0, 3).join('.')}`
      : ''
    const text = (el.textContent || '').trim().replace(/\s+/g, ' ').slice(0, 40)
    return `${el.tagName.toLowerCase()}${id}${cls} "${text}"`
  }

  const vw = window.innerWidth
  const docEl = document.documentElement
  const scrollWidth = Math.max(docEl.scrollWidth, document.body ? document.body.scrollWidth : 0)
  const clientWidth = docEl.clientWidth
  const overflow = scrollWidth > clientWidth + 1

  // 溢出元素：右边界超出视口 1px 以上，或左边界为负（排除 fixed 装饰层与 0 尺寸）
  const overflowEls = []
  const invisible = []
  const seen = new Set()
  for (const el of document.querySelectorAll('*')) {
    const rect = el.getBoundingClientRect()
    if (overflow && rect.width > 0 && rect.height > 0) {
      if (rect.right > vw + 1 || rect.left < -1) {
        overflowEls.push({ selector: describe(el), right: Math.round(rect.right), left: Math.round(rect.left) })
      }
    }
    // 自身文本节点非空白 → 参与「文字不可见」检查
    const hasOwnText = [...el.childNodes].some(
      (n) => n.nodeType === 3 && (n.textContent || '').trim().length > 0,
    )
    if (!hasOwnText) continue
    if (el.getClientRects().length === 0) continue
    if (rect.width < 4 || rect.height < 4) continue
    const style = getComputedStyle(el)
    if (style.visibility === 'hidden' || style.display === 'none' || style.opacity === '0') continue
    const fgRaw = parseColor(style.color)
    if (!fgRaw || fgRaw.a === 0) continue
    const bg = effectiveBackground(el)
    const fg = fgRaw.a < 1 ? composite(fgRaw, bg) : fgRaw
    const r = ratio(fg, bg)
    if (r < minRatio) {
      const key = `${describe(el)}|${r.toFixed(2)}`
      if (seen.has(key)) continue
      seen.add(key)
      invisible.push({
        selector: describe(el),
        ratio: Number(r.toFixed(3)),
        color: style.color,
        background: `rgb(${Math.round(bg.r)}, ${Math.round(bg.g)}, ${Math.round(bg.b)})`,
      })
    }
  }
  overflowEls.sort((a, b) => b.right - a.right)
  invisible.sort((a, b) => a.ratio - b.ratio)
  return {
    themeClass: docEl.className,
    viewportWidth: vw,
    clientWidth,
    scrollWidth,
    overflow,
    overflowEls: overflowEls.slice(0, maxFindings),
    invisible: invisible.slice(0, maxFindings),
    totalInvisible: invisible.length,
  }
}

// ---------------------------------------------------------------------------
// 主流程
// ---------------------------------------------------------------------------

async function login(base, user, pass) {
  const res = await fetch(`${base}/api/login`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ username: user, password: pass }),
  })
  const body = await res.json().catch(() => ({}))
  if (!res.ok || !body.token) {
    fail(
      `登录失败（HTTP ${res.status} ${body.code ?? ''}）。\n` +
        '  若是默认口令账户，首登会被强制改密——本脚本不处理该流程，请用改过密的 dev home。',
    )
  }
  if (body.must_change_password) {
    fail('该账户被标记「必须改密」（must_change_password=1），请先改密再跑本脚本。')
  }
  return body
}

function parseViewportArg(text) {
  const m = /^([a-z0-9_-]+)=(\d+)x(\d+)$/i.exec(text)
  if (m) return { name: m[1], width: Number(m[2]), height: Number(m[3]) }
  const preset = DEFAULT_VIEWPORTS.find((v) => v.name === text)
  if (!preset) fail(`未知视口: ${text}（用 name=宽x高，或 --help 看预设名）`)
  return preset
}

function summarize(results) {
  const failed = results.filter((r) => r.findings.length > 0)
  const lines = []
  lines.push('')
  lines.push(
    `视口冒烟结果：${results.length} 个组合，${results.length - failed.length} 通过，${failed.length} 失败`,
  )
  for (const r of failed) {
    lines.push('')
    lines.push(`✗ ${r.theme} ${r.viewport.name} (${r.viewport.width}x${r.viewport.height}) ${r.route}`)
    for (const f of r.findings) lines.push(`    ${f}`)
  }
  return lines.join('\n')
}

async function main() {
  const opts = parseArgs(process.argv.slice(2))
  const routes = opts.routes ?? DEFAULT_ROUTES
  const viewports = (opts.viewports ?? DEFAULT_VIEWPORTS.map((v) => v.name)).map(parseViewportArg)
  const themes = opts.themes
  for (const theme of themes) {
    if (!['light', 'dark'].includes(theme)) fail(`未知主题: ${theme}（只支持 light / dark）`)
  }

  // 栈可用性（先探一次，避免起了浏览器才发现 8080 没开）。
  // /health 返回纯文本 "ok" 而非 JSON，所以这里直接 fetch 判状态码。
  try {
    const res = await fetch(`${opts.base}/health`)
    if (!res.ok) throw new Error(`HTTP ${res.status}`)
  } catch (err) {
    fail(`无法访问 ${opts.base}/health：${err.message}\n  先起栈：DZTRADER_HOME=<项目内目录> ./scripts/run-dev.sh`)
  }

  const auth = opts.token ? { token: opts.token, user: { username: opts.user, role: 'admin' } } : await login(opts.base, opts.user, opts.pass)
  if (!opts.token) auth.user = (await getJson(`${opts.base}/api/user/me`, { headers: { Authorization: `Bearer ${auth.token}` } }).catch(() => null)) ?? auth.user

  const port = 9333
  let chromeProc = null
  let endpoint = opts.cdp
  if (!endpoint) {
    const chromePath = detectChrome(opts.chrome)
    const launched = await launchChrome(chromePath, port)
    chromeProc = launched.proc
    endpoint = launched.endpoint
  }
  const client = await connectPage(endpoint)

  const auditSource = `(${pageAudit.toString()})(${JSON.stringify({ minRatio: opts.minRatio, maxFindings: opts.maxFindings })})`
  const results = []

  try {
    for (const theme of themes) {
      for (const viewport of viewports) {
        await client.send('Emulation.setDeviceMetricsOverride', {
          width: viewport.width,
          height: viewport.height,
          deviceScaleFactor: 1,
          mobile: viewport.width <= 390,
        })
        for (const route of routes) {
          // 先落到同源的 /login（public）写 localStorage，再进目标路由：
          // token / 主题都必须在应用挂载**之前**就位，否则首帧用默认主题渲染。
          await navigate(client, `${opts.base}/login`)
          await evaluate(
            client,
            `(() => {
              localStorage.setItem('jwt_token', ${JSON.stringify(auth.token)});
              localStorage.setItem('user_info', ${JSON.stringify(JSON.stringify(auth.user ?? {}))});
              localStorage.removeItem('must_change_password');
              localStorage.setItem('theme', ${JSON.stringify(theme)});
              return true;
            })()`,
          )
          await navigate(client, `${opts.base}${route}`)
          await sleep(opts.settle)

          const raw = await evaluate(client, auditSource)
          const findings = []
          const expectedClass = theme
          if (!raw.themeClass.split(/\s+/).includes(expectedClass)) {
            findings.push(
              `主题未生效：<html class="${raw.themeClass}"> 期望包含 "${expectedClass}"（useTheme 未挂载？）`,
            )
          }
          if (raw.overflow) {
            const worst = raw.overflowEls
              .slice(0, 3)
              .map((e) => `${e.selector} right=${e.right}`)
              .join('；')
            findings.push(
              `横向溢出：scrollWidth=${raw.scrollWidth} > clientWidth=${raw.clientWidth}${worst ? `｜${worst}` : ''}`,
            )
          }
          for (const item of raw.invisible) {
            findings.push(`文字与底色不可区分 ${item.ratio}:1｜${item.selector} color=${item.color} bg=${item.background}`)
          }
          results.push({ theme, viewport, route, findings, raw })
          if (opts.verbose) {
            process.stdout.write(
              `${findings.length === 0 ? '✓' : '✗'} ${theme} ${viewport.name} ${route}` +
                `${findings.length === 0 ? '' : ` (${findings.length} findings)`}\n`,
            )
          }
        }
      }
    }
  } finally {
    if (!opts.keepOpen) {
      client.close()
      if (chromeProc) chromeProc.kill('SIGKILL')
    }

    process.stdout.write(summarize(results) + '\n')

    const payload = {
      generatedAt: new Date().toISOString(),
      base: opts.base,
      minRatio: opts.minRatio,
      total: results.length,
      failed: results.filter((r) => r.findings.length > 0).length,
      results: results.map((r) => ({
        theme: r.theme,
        viewport: r.viewport,
        route: r.route,
        findings: r.findings,
        scrollWidth: r.raw.scrollWidth,
        clientWidth: r.raw.clientWidth,
        invisibleCount: r.raw.totalInvisible,
      })),
    }
    if (opts.json === '-') {
      process.stdout.write(JSON.stringify(payload, null, 2) + '\n')
    } else if (opts.json) {
      writeFileSync(opts.json, JSON.stringify(payload, null, 2) + '\n', 'utf8')
      process.stdout.write(`JSON 结果已写入 ${opts.json}\n`)
    }

    process.exitCode = payload.failed > 0 ? 1 : 0
  }
}

main().catch((err) => {
  process.stderr.write(`viewport-smoke: 未捕获异常: ${err.stack ?? err.message}\n`)
  process.exit(2)
})
