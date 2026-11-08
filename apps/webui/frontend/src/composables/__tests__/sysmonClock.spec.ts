import { describe, it, expect } from 'vitest'
import {
  CLOCK_DEFAULT_BOUND_ALARM_MS,
  CLOCK_DEFAULT_MANUAL_MIN_INTERVAL_SEC,
  CLOCK_DEFAULT_OFFSET_ERROR_MS,
  CLOCK_DEFAULT_OFFSET_WARN_MS,
  clockBoundAlarm,
  clockDomainState,
  clockFailureSummary,
  clockFailureText,
  clockOffsetLevel,
  clockProbeAgeSec,
  clockServerCells,
  resolveClockManualIntervalSec,
  resolveClockThresholds,
  sortClockDaemons,
} from '../sysmonClock'
import {
  SYSMON_CLOCK_ALL_FAILED,
  SYSMON_CLOCK_LIVE,
  SYSMON_CLOCK_UNPROBED,
} from '@/test-utils/sysmonSnapshot'
import { makeSysmonConfig } from '@/test-utils/sysmonSnapshot'

// #302 展示口径的纯函数单测：契约 sysmon §2.9（三种空态 / 明细字段）、§2.8（两个时间判定对象）、
// §3.1（clock 配置段默认值）。视图测试只验渲染，口径判据在这里逐条锁死。

describe('resolveClockThresholds / resolveClockManualIntervalSec：配置阈值解析（§3.1）', () => {
  it('读取生效配置的三个阈值与手动限流间隔', () => {
    const clock = makeSysmonConfig({
      clock: { offset_warn_ms: 50, offset_error_ms: 500, bound_alarm_ms: 120, manual_min_interval_sec: 30 },
    }).clock
    expect(resolveClockThresholds(clock)).toEqual({ warn_ms: 50, error_ms: 500, bound_alarm_ms: 120 })
    expect(resolveClockManualIntervalSec(clock)).toBe(30)
  })

  it('配置未到达（null）时回退契约默认值 100 / 1000 / 200，限流 10 秒', () => {
    expect(resolveClockThresholds(null)).toEqual({
      warn_ms: CLOCK_DEFAULT_OFFSET_WARN_MS,
      error_ms: CLOCK_DEFAULT_OFFSET_ERROR_MS,
      bound_alarm_ms: CLOCK_DEFAULT_BOUND_ALARM_MS,
    })
    expect(resolveClockManualIntervalSec(null)).toBe(CLOCK_DEFAULT_MANUAL_MIN_INTERVAL_SEC)
  })

  it('畸形字段逐个回退默认值（不让一个坏字段把整张卡片变成不显示）', () => {
    const clock = makeSysmonConfig({
      clock: { offset_warn_ms: 0, offset_error_ms: Number.NaN, bound_alarm_ms: -5 },
    }).clock
    expect(resolveClockThresholds(clock)).toEqual({
      warn_ms: CLOCK_DEFAULT_OFFSET_WARN_MS,
      error_ms: CLOCK_DEFAULT_OFFSET_ERROR_MS,
      bound_alarm_ms: CLOCK_DEFAULT_BOUND_ALARM_MS,
    })
    // 负数/非有限同样回退；0 是限流的合法值，不能被兜底吞掉
    expect(resolveClockManualIntervalSec(makeSysmonConfig({ clock: { manual_min_interval_sec: -1 } }).clock))
      .toBe(CLOCK_DEFAULT_MANUAL_MIN_INTERVAL_SEC)
    expect(resolveClockManualIntervalSec(makeSysmonConfig({ clock: { manual_min_interval_sec: 0 } }).clock)).toBe(0)
  })
})

describe('clockDomainState：三种空态分类（§2.9）', () => {
  it('null = 配置关闭（唯一含义）', () => {
    expect(clockDomainState(null)).toBe('disabled')
  })

  it('last_probe_unix_ms = 0 = 尚未探测（不是不可用）', () => {
    expect(clockDomainState(SYSMON_CLOCK_UNPROBED)).toBe('unprobed')
  })

  it('探测过但无聚合值 = 不可用（全部失败），不得回落成「尚未探测」', () => {
    expect(clockDomainState(SYSMON_CLOCK_ALL_FAILED)).toBe('unavailable')
  })

  it('有聚合偏差 = live', () => {
    expect(clockDomainState(SYSMON_CLOCK_LIVE)).toBe('live')
  })
})

describe('clockOffsetLevel：偏差按绝对值定档（§2.8，边界含）', () => {
  const thresholds = { warn_ms: 100, error_ms: 1000, bound_alarm_ms: 200 }

  it.each([
    [0, 'normal'],
    [99, 'normal'],
    [100, 'warn'],       // 恰好等于 warn 阈值算进入（契约 §2.8：≥）
    [-100, 'warn'],      // 负偏差同样按绝对值定档
    [999, 'warn'],
    [1000, 'danger'],    // 恰好等于 error 阈值算进入
    [-1000, 'danger'],
  ] as Array<[number | null, string]>)('|offset|=%s → %s', (offsetMs, expected) => {
    expect(clockOffsetLevel(offsetMs, thresholds)).toBe(expected)
  })

  it('无聚合值（null）不套色标（不可用不是 0 毫秒，不得冒充 normal）', () => {
    expect(clockOffsetLevel(null, thresholds)).toBeNull()
  })
})

describe('clockBoundAlarm：误差界质量判据独立于偏差着色（§2.8）', () => {
  const thresholds = { warn_ms: 100, error_ms: 1000, bound_alarm_ms: 200 }

  it('bound > 阈值才提示（恰好等于不算）', () => {
    expect(clockBoundAlarm(200, thresholds)).toBe(false)  // 契约 §2.8：> 才算越界
    expect(clockBoundAlarm(200.1, thresholds)).toBe(true)
    expect(clockBoundAlarm(0, thresholds)).toBe(false)
  })

  it('无误差界（null）不提示（测不到不是测不准）', () => {
    expect(clockBoundAlarm(null, thresholds)).toBe(false)
  })

  it('与偏差档位互不影响：偏差 danger + 质量正常、偏差 normal + 质量劣化都能同时成立', () => {
    expect(clockOffsetLevel(-1500, thresholds)).toBe('danger')
    expect(clockBoundAlarm(10, thresholds)).toBe(false)
    expect(clockOffsetLevel(3, thresholds)).toBe('normal')
    expect(clockBoundAlarm(500, thresholds)).toBe(true)
  })
})

describe('clockFailureText：失败原因中文映射（§2.9 的五个取值）', () => {
  it('五个约定取值各有中文文案', () => {
    expect(clockFailureText('timeout')).toBe('超时未响应')
    expect(clockFailureText('kod_rate')).toBe('服务端限流（KoD RATE）')
    expect(clockFailureText('kod_deny')).toBe('服务端拒绝（KoD DENY）')
    expect(clockFailureText('bad_echo')).toBe('报文回显不匹配')
    expect(clockFailureText('unreachable')).toBe('无法解析或连接失败')
  })

  it('null（无失败原因）显示占位破折号，不冒充不可用', () => {
    expect(clockFailureText(null)).toBe('—')
    expect(clockFailureText('')).toBe('—')
  })

  it('非约定值回退原文（不吞可诊断信息）', () => {
    expect(clockFailureText('some_new_error')).toBe('some_new_error')
  })
})

describe('clockServerCells：每台服务器明细列（AC：主机/偏差/误差界/RTT/stratum/失败原因）', () => {
  it('成功台：四项测量按格式化模块呈现（偏差带符号）', () => {
    const cells = clockServerCells(SYSMON_CLOCK_LIVE.servers[0])
    expect(cells.map(cell => [cell.label, cell.value])).toEqual([
      ['偏差', '+12.5 ms'],
      ['误差界', '18.3 ms'],
      ['RTT', '24.1 ms'],
      ['Stratum', '2'],
      ['失败原因', '—'],
    ])
  })

  it('失败台的测量列一律「不可用」，绝不显示 0', () => {
    const cells = clockServerCells(SYSMON_CLOCK_ALL_FAILED.servers[0])
    expect(cells.map(cell => [cell.label, cell.value])).toEqual([
      ['偏差', '不可用'],
      ['误差界', '不可用'],
      ['RTT', '不可用'],
      ['Stratum', '不可用'],
      ['失败原因', '超时未响应'],
    ])
  })
})

describe('clockFailureSummary：全部失败时的原因汇总', () => {
  it('逐台原因去重后并列，保留可诊断信息', () => {
    const summary = clockFailureSummary([
      { host: 'a', ok: false, offset_ms: null, bound_ms: null, rtt_ms: null, stratum: null, error: 'timeout' },
      { host: 'b', ok: false, offset_ms: null, bound_ms: null, rtt_ms: null, stratum: null, error: 'timeout' },
      { host: 'c', ok: false, offset_ms: null, bound_ms: null, rtt_ms: null, stratum: null, error: 'unreachable' },
    ])
    expect(summary).toBe('最近一轮探测全部失败：超时未响应、无法解析或连接失败')
  })

  it('servers 为空时给中性说明，不编造原因', () => {
    expect(clockFailureSummary([])).toBe('最近一轮探测未获得可用参考源')
  })
})

describe('clockProbeAgeSec：上次探测距今的秒数', () => {
  const nowMs = Date.UTC(2026, 9, 21, 15, 0, 0)

  it('快照未到达 / 尚未探测（0）时没有相对时间（null，不是 0 秒前）', () => {
    expect(clockProbeAgeSec(0, nowMs)).toBeNull()
    expect(clockProbeAgeSec(Number.NaN, nowMs)).toBeNull()
  })

  it('按毫秒差换算秒；探测时刻晚于本机墙钟时钳到 0（不显示负值）', () => {
    expect(clockProbeAgeSec(nowMs - 30_000, nowMs)).toBe(30)
    expect(clockProbeAgeSec(nowMs - 90_000, nowMs)).toBe(90)
    expect(clockProbeAgeSec(nowMs + 3_000, nowMs)).toBe(0)
    expect(clockProbeAgeSec(nowMs, nowMs)).toBe(0)
  })

  it('本模块只算秒数：「多久以前」的文案复用视图既有 formatAge（不复制第二份实现）', () => {
    // 这里锁住「只算秒」，文案口径由 MonitorView 的 `formatAge` 承担（视图侧断言见
    // MonitorClockCard.spec.ts 的「显示带符号偏差…上次探测相对时间」用例）。
    expect(clockProbeAgeSec(nowMs - 31_000, nowMs)).toBe(31)
    expect(clockProbeAgeSec(nowMs - 60_000, nowMs)).toBe(60)
    expect(clockProbeAgeSec(nowMs - 90_000, nowMs)).toBe(90)
  })
})

describe('sortClockDaemons：同步服务列表展示序', () => {
  it('在运行的排前面，同组按名称升序（不依赖发送端顺序）', () => {
    expect(sortClockDaemons([
      { name: 'ntpd', running: false },
      { name: 'systemd-timesyn', running: true },
      { name: 'chronyd', running: true },
    ])).toEqual([
      { name: 'chronyd', running: true },
      { name: 'systemd-timesyn', running: true },
      { name: 'ntpd', running: false },
    ])
  })

  it('不修改入参（快照读到的数组不得被就地排序）', () => {
    const input = [{ name: 'b', running: false }, { name: 'a', running: true }]
    const sorted = sortClockDaemons(input)
    expect(input.map(daemon => daemon.name)).toEqual(['b', 'a'])
    expect(sorted.map(daemon => daemon.name)).toEqual(['a', 'b'])
  })
})
