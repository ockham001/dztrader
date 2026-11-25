# ADR 0058: `ACCOUNT_STATUS.gateway_name` = 报告网关，空串 = 平台生成（保留在 SDK 公开面）

## Status

Accepted（2026-11-25）

> 收口 [ADR 0056](0056-td-account-identity-investor-code-and-display-name.md) Consequences 登记的遗留项（#483）。本 ADR **不修改 ADR 0056 的决策**：决策 7「`ACCOUNT_STATUS` 两帧无 `instance_id`、保持 basic 帧」保持有效并由此再次确认；本 ADR 只明确该帧 payload 中 `gateway_name` 的单一语义与 master 的写入口径。

## Context

ADR 0056 Consequences 登记：「`DzAccountStatus.gateway_name` 仍在 SDK 公开面…它同时承担 master 兜底权威标记，属独立问题，另开票处理」。核对代码与帧契约后，有三个事实：

1. **它是帧形态的必然结果，不是意外遗留**：`ACCOUNT_STATUS` 是 basic 定长 struct 帧（仅 `DzFrameHeader`，无扩展头）。《帧契约：通用规则》§5 规定「帧头无 `instance_id` 的帧不得依赖'帧来源'路由；来源/目标必须显式在 payload 中携带」，§2 又规定「定长 struct 帧无扩展头」——因此「谁报的这份账户状态」只能在 payload 里。仓内同款：`NOTIFY_UI.source`、`RTN_PROCESS_STATUS.name`、`RTN_SYSMON_SNAPSHOT.source`。
2. **master 是唯一功能性依赖方（其余消费方只把它当日志 / 诊断信息），且拿不到运行态归属的第二个来源**：master 用它做镜像 `网关名→账户集` 的 key；td 退出时需要按该网关枚举账户逐个代推 Offline（audit A-F1：否则 dead-td 下策略静默挂死）。帧头无写者标识、`ProcessConfig` 无账户、ADR 0056 明确 master 不解析 td 配置、dzweb 在「后台可无 UI 运行」下不可依赖；配置态来源（td 配置文件、`TD_RTN_CONFIG` 帧）是配置投影而非运行态，且 master 按分层不消费 td 域帧——删字段即等于要么改帧形态、要么让 master 依赖配置态来源、要么丢能力。
3. **契约原句与实现漂移**：原文「空串 = master 兜底应答（非权威标记），消费方据此区分权威/兜底」，但场景 8 的 td 退出代推帧同样由 master 写出，却填了真实网关名。于是「非空 ⇒ 网关权威」在线上不成立，只有「空 ⇒ 平台生成」这半边成立。这是文档漂移，不是布局缺陷。

## Decision

1. **保留 `DzAccountStatus.gateway_name`**：wire 布局（104B）、帧形态（basic 定长 struct）、SDK 公开面均不变；不引入 `instance_id`、不改成 JSON 变长帧。
2. **单一语义 = 报告该账户状态的网关进程名**（如 `dztd_ctp`）。**空串 ⇔ 该帧由平台（master）生成**：`TD_QUERY_ACCOUNT_STATUS` 兜底应答与 td 退出代推两类都是空串；master 不在代推帧里填网关名（该诊断信息保留在 master 日志与 `RTN_PROCESS_STATUS`）。
3. **把原句做成可实现、可测的承诺**：`gateway_name` 为空 ⇔ 帧由 master 写（非权威兜底）；非空 ⇔ td 网关权威回报。契约保留「消费方据此区分权威 / 兜底」，并写明兜底含「查询兜底」与「td 退出代推」两类。
4. **策略侧不写禁令**：允许把它当只读运行态判断依据（日志 / 告警 / 按接口聚合 / 参数选择）；契约只陈述能力边界（事实）：不是稳定标识（网关名是运行期实例名，改名 / 换实例 / 账户迁移即变）、只覆盖「当前有网关在报」的账户、不改变下单路由（`TD_ORDER_REQ` 按 `account_id` 归属路由）、跨重启不承诺连续（历史帧不重放）。
5. **不做「策略侧权威归属查询」API**：配置级归属的真相源是各 td 配置文件与 dzweb 账户注册表；当前没有策略侧查询需求（Known limitation）。

**为什么不用其他方案**：

- **(A) 从 SDK 公开面移除该字段**：唯一能保住 master 能力的形态是「归属进帧头」，但总则 §2「定长 struct 帧无扩展头」+ §6「struct 编码用于后台高频 / 策略 API 消费」意味着这是帧形态分类学改动（新增混合形态或改 JSON 变长帧），波及四端与冻结表，且要推翻 ADR 0056 决策 7；而兜底标记问题并不会消失，只会从「空 `gateway_name`」变成「空 `instance_id`」。
- **(B) master 改从配置态来源拿归属（读网关配置文件，或解 `TD_RTN_CONFIG`）**：机制可行（`read_md_page_size` 有先例；`TD_RTN_CONFIG` 的 `instance_id` 就是网关名且含 `accounts[]`），但把「帧驱动的运行态」换成「配置投影」——配置文件不可读 / 损坏时 master 直接失明，A-F1 那类静默挂死回归；镜像语义也要从「当前管理的账户集」重写，且要打破「master 不消费 td 域帧」的分层。本决策已保留字段供策略侧使用，省这一个只读字段没有任何收益。
- **(C) 归属语义（代推保留真实网关名，删掉权威区分）**：可以自洽，但留下「非空 ≠ 权威」这个必须反复解释的坑；策略拿到的信号退化为「非空 = 某网关（可能已退出）」。选「空串 = 平台生成」让既有契约声称成真，代价只是代推帧少一个已无消费方的诊断字段。
- **(D) 三态显式标记（reserved 位 / 独立帧号：td 权威 / 平台代推 / 平台兜底）**：要改布局语义或帧号，并让 master / SDK / dzweb 全部按标记分支；当前没有任何消费方需要区分「td 亲口」与「平台代推」（两者都意味着该账户当前不可用），YAGNI。

## Consequences

**正面**：字段语义单一且可测；契约 / 代码 / 注释一致；帧形态、布局、帧号、冻结表全部不动（`public_surface_invariants_test` 与 `td_struct_test` 无需改）；master 代推少一次字符串拷贝；#483 由「待决策」变为「已收口」，跨网关重号问题由 #485 独立跟进。

**负面 / 已知限制**：

- 代推帧不再携带死网关名：`ACCOUNT_STATUS` 流里无法看出「哪个网关退出导致这批 Offline」，该信息只在 master 日志（`td stopped, accounts marked offline | gateway=… count=…`）与 `RTN_PROCESS_STATUS` 中。
- 策略若用「最新帧」判断账户归属，网关退出瞬间会失去归属（帧变空串）；需要时自行缓存最近一次非空值。
- 单帧不区分「td 权威回报」与「平台代推」——两者都是空串，这是本 ADR 的语义选择而非缺陷。
- 配置级（权威）归属没有策略侧 API：真相源是 td 配置与 dzweb 注册表；需要时另立能力。
- ADR 0041 决策 3 的「trade store `gateway` 列与 `ACCOUNT_STATUS.gateway_name` 同义」在本 ADR 后按**非空值**解释（trade store 的 `gateway` 写端恒为 td，不会出现空串）；ADR 0041 正文不改。

**契约 / 文档同步**：《帧契约：账户登录状态》字段语义 / 场景 8 / 镜像 / 约束；`struct.h` 与 `api.h` 注释；`GLOSSARY` 新增「网关归属」「平台兜底应答」。

## References

- 登记来源：[ADR 0056](0056-td-account-identity-investor-code-and-display-name.md)（决策 7 / Consequences）、[ADR 0045](0045-trading-account-identity-lifecycle-and-accuracy.md)（`gateway` 只作审计列、账户身份恒为 `account_id`）、[ADR 0041](0041-sqlite-only-trade-store-schema-v1.md)（`gateway` 审计列）
- 帧形态与身份规则：《帧契约：通用规则》§2 / §5 / §6
- 帧契约：《帧契约：账户登录状态》
- 票据：#483（本 ADR）、#467（ADR 0056 收口差异清单）、#485（跨网关重复 `account_id` → 拒绝启动）
