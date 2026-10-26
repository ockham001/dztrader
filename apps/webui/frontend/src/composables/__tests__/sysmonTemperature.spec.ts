import { describe, it, expect } from 'vitest'
import {
  chipDisplayName,
  chipRank,
  groupTemperatures,
  normalizeTemperatureReadings,
  parseSensorIdentifier,
} from '../sysmonTemperature'

// 真实夹具取自开发机 hwmon（coretemp / amdgpu / iwlwifi_1_2 / acpitz_0），
// 覆盖票面提到的全部形态：有 label、无 label（tempN）、芯片名带内核去重后缀。

describe('parseSensorIdentifier：从原始标识回退解析（旧版本发送方兼容）', () => {
  it('标准形态 <chip>#<hwmonN>:<label>', () => {
    expect(parseSensorIdentifier('coretemp#hwmon3:Package id 0'))
      .toEqual({ chip: 'coretemp', label: 'Package id 0' })
  })

  it('无 label 时为通道名 tempN', () => {
    expect(parseSensorIdentifier('iwlwifi_1_2#hwmon2:temp1'))
      .toEqual({ chip: 'iwlwifi_1_2', label: 'temp1' })
  })

  it('解析不出 # 时整串当芯片名、标签为空（展示层随后用通道名兜底）', () => {
    expect(parseSensorIdentifier('acpitz')).toEqual({ chip: 'acpitz', label: '' })
  })
})

describe('chipDisplayName：友好映射与未命中回退', () => {
  const cases: Array<[string, string]> = [
    // CPU：Intel / AMD / 通用
    ['coretemp', 'CPU'],
    ['k10temp', 'CPU'],
    ['zenpower', 'CPU'],
    ['cpu_thermal', 'CPU'],
    // GPU
    ['amdgpu', 'GPU'],
    ['nouveau', 'GPU'],
    ['i915', 'GPU'],
    // 无线网卡（内核去重后缀仍命中）
    ['iwlwifi', '无线网卡'],
    ['iwlwifi_1_2', '无线网卡'],
    // 主板 ACPI
    ['acpitz', '主板 ACPI'],
    ['acpitz_0', '主板 ACPI'],
    // 未命中：回退原芯片名，不猜
    ['nct6775', 'nct6775'],
    ['unknown_chip', 'unknown_chip'],
    ['acpitz_extra', 'acpitz_extra'], // 后缀必须是 _<数字>，否则不命中
  ]

  it.each(cases)('%s → %s', (chip, expected) => {
    expect(chipDisplayName(chip)).toBe(expected)
  })
})

describe('chipRank：CPU/GPU 在前、ACPI 在后', () => {
  it('排序权重按契约档位：CPU < GPU < 其余 < 主板 ACPI', () => {
    expect(chipRank('coretemp')).toBeLessThan(chipRank('amdgpu'))
    expect(chipRank('amdgpu')).toBeLessThan(chipRank('nct6775'))
    expect(chipRank('nct6775')).toBeLessThan(chipRank('acpitz'))
  })

  it('未命中映射表的芯片取中间档（不与 CPU/GPU 或 ACPI 混档）', () => {
    expect(chipRank('nct6775')).toBe(2)
  })
})

describe('normalizeTemperatureReadings：补全 chip / label 并过滤非法读数', () => {
  it('快照已给 chip/label 时原样使用，sensor 保持不变（历史库维度键）', () => {
    const [reading] = normalizeTemperatureReadings([
      { sensor: 'coretemp#hwmon3:Package id 0', chip: 'coretemp', label: 'Package id 0', celsius: 45.5 },
    ])
    expect(reading).toEqual({
      sensor: 'coretemp#hwmon3:Package id 0',
      chip: 'coretemp',
      label: 'Package id 0',
      celsius: 45.5,
    })
  })

  it('旧版本发送方（无 chip/label）从 sensor 回退解析', () => {
    const [reading] = normalizeTemperatureReadings([
      { sensor: 'coretemp#hwmon3:Core 0', celsius: 44 },
    ])
    expect(reading.chip).toBe('coretemp')
    expect(reading.label).toBe('Core 0')
  })

  it('label 为空串时回退通道名，不退化为空（同芯片多条读数要能区分）', () => {
    const [reading] = normalizeTemperatureReadings([
      { sensor: 'acpitz#hwmon0:temp1', chip: 'acpitz', label: '', celsius: 27.8 },
    ])
    expect(reading.label).toBe('temp1')
  })

  it('chip 缺失且 sensor 无 # 时整串兜底为芯片名', () => {
    const [reading] = normalizeTemperatureReadings([{ sensor: 'acpitz', celsius: 27.8 }])
    expect(reading.chip).toBe('acpitz')
    expect(reading.label).toBe('acpitz') // 无通道名可用时退到芯片名，仍非空
  })

  it('过滤空 sensor 与非有限温度（不得用 0 冒充）', () => {
    expect(normalizeTemperatureReadings([
      { sensor: '', celsius: 40 },
      { sensor: '  ', celsius: 40 },
      { sensor: 'coretemp#hwmon3:Core 0', celsius: Number.NaN },
      { sensor: 'coretemp#hwmon3:Core 0' }, // celsius 缺失
      { sensor: 'coretemp#hwmon3:Core 1', celsius: 41 },
    ])).toEqual([
      { sensor: 'coretemp#hwmon3:Core 1', chip: 'coretemp', label: 'Core 1', celsius: 41 },
    ])
  })

  it('celsius 为 0 是合法真实值（不是不可用）', () => {
    const readings = normalizeTemperatureReadings([
      { sensor: 'acpitz#hwmon0:temp1', chip: 'acpitz', label: '', celsius: 0 },
    ])
    expect(readings).toHaveLength(1)
    expect(readings[0].celsius).toBe(0)
  })
})

describe('groupTemperatures：按芯片分组并排序', () => {
  it('按 chip 分组，组显示名用友好映射', () => {
    const readings = normalizeTemperatureReadings([
      { sensor: 'coretemp#hwmon3:Package id 0', chip: 'coretemp', label: 'Package id 0', celsius: 45.5 },
      { sensor: 'coretemp#hwmon3:Core 0', chip: 'coretemp', label: 'Core 0', celsius: 41.5 },
      { sensor: 'acpitz#hwmon0:temp1', chip: 'acpitz', label: '', celsius: 27.8 },
    ])
    const groups = groupTemperatures(readings)
    expect(groups.map(g => g.displayName)).toEqual(['CPU', '主板 ACPI'])
    expect(groups[0].readings.map(r => r.label)).toEqual(['Core 0', 'Package id 0'])
    expect(groups[1].readings.map(r => r.label)).toEqual(['temp1'])
  })

  it('排序：CPU 与 GPU 在前、主板 ACPI 在后，未映射芯片居中', () => {
    const readings = normalizeTemperatureReadings([
      { sensor: 'acpitz#hwmon0:temp1', chip: 'acpitz', label: 'temp1', celsius: 27.8 },
      { sensor: 'nct6775#hwmon5:temp1', chip: 'nct6775', label: 'temp1', celsius: 35 },
      { sensor: 'amdgpu#hwmon4:edge', chip: 'amdgpu', label: 'edge', celsius: 52 },
      { sensor: 'coretemp#hwmon3:Package id 0', chip: 'coretemp', label: 'Package id 0', celsius: 45.5 },
    ])
    const groups = groupTemperatures(readings)
    expect(groups.map(g => g.chip)).toEqual(['coretemp', 'amdgpu', 'nct6775', 'acpitz'])
  })

  it('不过滤任何传感器：ACPI 类只是排在后面，读数全部保留', () => {
    const readings = normalizeTemperatureReadings([
      { sensor: 'coretemp#hwmon3:Package id 0', chip: 'coretemp', label: 'Package id 0', celsius: 45.5 },
      { sensor: 'acpitz#hwmon0:temp1', chip: 'acpitz', label: 'temp1', celsius: 27.8 },
      { sensor: 'acpitz#hwmon1:temp1', chip: 'acpitz_1', label: 'temp1', celsius: 26.8 },
    ])
    const groups = groupTemperatures(readings)
    const totalReadings = groups.reduce((sum, group) => sum + group.readings.length, 0)
    expect(totalReadings).toBe(3) // 三条都在（含两条 ACPI）
    expect(groups.map(g => g.chip)).toEqual(['coretemp', 'acpitz', 'acpitz_1'])
  })

  it('同档芯片按芯片名字典序，组内按 label 字典序——顺序确定，不随采集顺序跳动', () => {
    const readingsA = normalizeTemperatureReadings([
      { sensor: 'b_chip#hwmon1:t', chip: 'b_chip', label: 'b', celsius: 30 },
      { sensor: 'a_chip#hwmon0:t', chip: 'a_chip', label: 'a', celsius: 31 },
    ])
    const readingsB = normalizeTemperatureReadings([
      { sensor: 'a_chip#hwmon0:t', chip: 'a_chip', label: 'a', celsius: 31 },
      { sensor: 'b_chip#hwmon1:t', chip: 'b_chip', label: 'b', celsius: 30 },
    ])
    expect(groupTemperatures(readingsA).map(g => g.chip))
      .toEqual(groupTemperatures(readingsB).map(g => g.chip))

    const unsortedInner = normalizeTemperatureReadings([
      { sensor: 'coretemp#hwmon3:Core 1', chip: 'coretemp', label: 'Core 1', celsius: 40 },
      { sensor: 'coretemp#hwmon3:Core 0', chip: 'coretemp', label: 'Core 0', celsius: 41 },
    ])
    expect(groupTemperatures(unsortedInner)[0].readings.map(r => r.label)).toEqual(['Core 0', 'Core 1'])
  })

  it('空输入返回空分组（视图据此显示「暂无数据」）', () => {
    expect(groupTemperatures([])).toEqual([])
  })
})
