// 进程名类别判定的唯一实现：日志页进程类型推导（stores/logs.ts deriveType）与
// 顶栏服务徽章聚合（components/layout/Topbar.vue，#199）共用。
// 此前两处各写一套前缀判断，已出现漂移（`dzmd` vs `dzmd_`），故收敛到本模块。
//
// 命名规则见《帧契约：通用规则》总则 §5：行情网关 `dzmd_*`、交易网关 `dztd_*`；
// 策略进程名是裸策略名（如 `stg_demo`，《帧契约：策略》「身份边界」——`stg.<name>`
// 只是内存中的订阅者/信号量身份前缀，不是进程名）。dzweb 可重命名部署，无法用
// 名字模式判定，故不在本函数返回类别，由调用方按 system.isSelf 单独判定。
export type ProcessCategory = 'master' | 'md' | 'td' | 'sysmon' | 'strategy' | 'unknown'

export function processCategory(name: string): ProcessCategory {
  if (name === 'dztraderd') return 'master'
  if (name.startsWith('dzmd')) return 'md'
  if (name.startsWith('dztd')) return 'td'
  if (name === 'dzsysmon') return 'sysmon'
  if (name.startsWith('stg_')) return 'strategy'
  return 'unknown'
}
