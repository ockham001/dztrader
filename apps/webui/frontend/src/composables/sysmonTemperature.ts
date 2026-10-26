/**
 * 温度读数的分组与友好命名（#126；契约 sysmon §2.4）
 *
 * 背景：hwmon 的原始标识形如 `coretemp#hwmon3:Package id 0` / `iwlwifi_1_2#hwmon2:temp1`。
 * `_2` 之类后缀是内核为避免重名加的，不含任何设备信息——直接展示等于要求用户解码 sysfs。
 * 因此展示层按 `chip` 分组、把常见芯片映射成可读名（CPU / GPU / 无线网卡 / 主板 ACPI），
 * 原始 `sensor` 只在 tooltip 里保留（需要与系统工具输出对照时仍可查到）。
 *
 * 两条不可动摇的约定：
 * 1. **不过滤任何传感器**。主流实现（psutil、Netdata、btop）都不隐藏 `acpitz`，本模块只
 *    决定**排序**（ACPI 类靠后），不决定去留——「读数都在」是契约事实，隐藏它就是撒谎。
 * 2. **`sensor` 字段值不改**。它是历史库 `temperature` 域维度键，改格式会让已落库历史失配
 *    （契约 §2.4）；本模块只做展示，不回写数据。
 */

/** 展示用的温度读数（比快照多出补全后的 chip / label，仍保留原始 sensor） */
export interface TemperatureReading {
  /** 原始稳定标识，仅用于 tooltip 与 React key，不参与展示排序 */
  sensor: string
  /** 芯片名（快照 `chip`；旧版本发送方缺失时从 `sensor` 回退解析） */
  chip: string
  /** 通道标签（快照 `label`；缺失时回退为通道名 `tempN`，绝不退化为空串） */
  label: string
  /** 摄氏温度 */
  celsius: number
}

/** 按芯片分组后的展示单元 */
export interface TemperatureGroup {
  /** 分组键（芯片名原值，用于 v-for :key） */
  chip: string
  /** 分组显示名（友好映射命中时为 CPU / GPU 等，未命中回退原芯片名） */
  displayName: string
  /** 组内读数，按 label 字典序稳定排序 */
  readings: TemperatureReading[]
}

/**
 * 芯片名 → 友好显示名。
 *
 * 只覆盖「用户认得出、且确实常见」的四类（AC 明列）：CPU 温度芯片（`coretemp` / `k10temp`
 * / `zenpower` / `cpu_thermal`）、GPU（`amdgpu` / `nouveau` / `radeon` / `i915`）、
 * 无线网卡（`iwlwifi*`，内核会加 `_N` 后缀）、主板 ACPI（`acpitz*` / `acpi`）。
 * 未命中的芯片回退显示原芯片名——**不猜**，显示原名比显示错误名好。
 */
const CHIP_DISPLAY_NAMES: Array<{ pattern: RegExp; displayName: string }> = [
  // CPU（Linux 上 Intel/AMD 各自常见的 hwmon 芯片名）
  { pattern: /^(coretemp|k10temp|zenpower|cpu_thermal|cpu-thermal)$/, displayName: 'CPU' },
  // GPU（AMD / NVIDIA 开源驱动 / Intel 集显）
  { pattern: /^(amdgpu|nouveau|radeon|i915)$/, displayName: 'GPU' },
  // 无线网卡：内核为避免重名会追加 `_N`（如 `iwlwifi_1_2`），故按前缀匹配
  { pattern: /^iwlwifi(_\d+)*$/, displayName: '无线网卡' },
  // 主板 ACPI 热区：`acpitz` / `acpitz_N`
  { pattern: /^acpitz(_\d+)*$/, displayName: '主板 ACPI' },
]

/**
 * 分组排序权重：数值小的排前面。
 *
 * CPU 与 GPU 在前（这两颗芯片的温度是值班时最常看的），主板 ACPI 类排最后（它通常只报
 * 机箱/环境的粗粒度读数，不该抢在 CPU 前面）。**权重相同不是"丢数据"**，其余芯片落在中间档。
 */
const CHIP_ORDER: Array<{ pattern: RegExp; rank: number }> = [
  { pattern: /^(coretemp|k10temp|zenpower|cpu_thermal|cpu-thermal)$/, rank: 0 },
  { pattern: /^(amdgpu|nouveau|radeon|i915)$/, rank: 1 },
  { pattern: /^acpitz(_\d+)*$/, rank: 3 },
]
/** 未命中映射表的芯片：排在 CPU/GPU 之后、ACPI 之前 */
const DEFAULT_CHIP_RANK = 2

/** 芯片名 → 排序权重（未命中映射表取中间档） */
export function chipRank(chip: string): number {
  for (const entry of CHIP_ORDER) {
    if (entry.pattern.test(chip)) return entry.rank
  }
  return DEFAULT_CHIP_RANK
}

/** 芯片名 → 友好显示名（未命中回退原芯片名，不猜） */
export function chipDisplayName(chip: string): string {
  for (const entry of CHIP_DISPLAY_NAMES) {
    if (entry.pattern.test(chip)) return entry.displayName
  }
  return chip
}

/**
 * 从原始 `sensor` 标识回退解析芯片名与通道标签（旧版本发送方的兼容路径）。
 *
 * 格式（契约 §2.4）：`<chip>#<hwmonN>:<label或tempN>`。解析不出 `#` / `:` 时整串当芯片名、
 * 标签退化为空串——展示层随后用 {@link normalizeTemperatureReadings} 的通道名兜底。
 */
export function parseSensorIdentifier(sensor: string): { chip: string; label: string } {
  const hash = sensor.indexOf('#')
  const colon = sensor.indexOf(':', hash >= 0 ? hash : 0)
  const chip = hash > 0 ? sensor.slice(0, hash) : sensor
  const label = colon >= 0 ? sensor.slice(colon + 1) : ''
  return { chip, label }
}

/**
 * 把快照读数归一为展示读数：补全 chip / label，过滤空 sensor 与非有限温度。
 *
 * 归一顺序（chip 与 label 各自独立回退）：
 * 1. `chip` 取快照字段；缺失/空时从 `sensor` 解析（新旧版本并存时不会渲染成空分组）。
 * 2. `label` 取快照字段；缺失/空时用 `sensor` 冒号后的通道名（`temp1`）兜底——**不退化为空**，
 *    否则同一芯片的多条读数会在界面上变成无法区分的空行。
 */
export function normalizeTemperatureReadings(
  readings: ReadonlyArray<{ sensor?: unknown; chip?: unknown; label?: unknown; celsius?: unknown }>,
): TemperatureReading[] {
  const normalized: TemperatureReading[] = []
  for (const reading of readings) {
    const sensor = typeof reading?.sensor === 'string' ? reading.sensor.trim() : ''
    if (!sensor) continue
    const celsius = typeof reading?.celsius === 'number' && Number.isFinite(reading.celsius)
      ? reading.celsius
      : null
    if (celsius === null) continue

    const parsed = parseSensorIdentifier(sensor)
    const snapshotChip = typeof reading.chip === 'string' ? reading.chip.trim() : ''
    const snapshotLabel = typeof reading.label === 'string' ? reading.label.trim() : ''
    const chip = snapshotChip || parsed.chip || sensor
    // label 缺失（旧版本发送方）或为空串（该通道没有标签）都回退到通道名。
    const label = snapshotLabel || parsed.label || chip
    normalized.push({ sensor, chip, label, celsius })
  }
  return normalized
}

/**
 * 按芯片分组并排序（AC：按 `chip` 分组、CPU/GPU 在前、主板 ACPI 类在后、不过滤）。
 *
 * 排序规则（两层，均为确定性）：
 * 1. 分组按 {@link chipRank} 升序；同档按芯片名字典序——同一台机器每次打开顺序一致。
 * 2. 组内读数按 label 字典序（同芯片的多个通道在帧间顺序稳定，不随采集顺序跳动）。
 */
export function groupTemperatures(readings: TemperatureReading[]): TemperatureGroup[] {
  const byChip = new Map<string, TemperatureReading[]>()
  for (const reading of readings) {
    const bucket = byChip.get(reading.chip)
    if (bucket) {
      bucket.push(reading)
    } else {
      byChip.set(reading.chip, [reading])
    }
  }

  const groups: TemperatureGroup[] = []
  for (const [chip, groupReadings] of byChip) {
    groups.push({
      chip,
      displayName: chipDisplayName(chip),
      readings: [...groupReadings].sort((lhs, rhs) => lhs.label.localeCompare(rhs.label)),
    })
  }

  groups.sort((lhs, rhs) => {
    const rankDelta = chipRank(lhs.chip) - chipRank(rhs.chip)
    if (rankDelta !== 0) return rankDelta
    return lhs.chip.localeCompare(rhs.chip)
  })
  return groups
}
