# ADR 0056: 交易账户身份 = 柜台投资者代码，展示名独立且可改

## Status

Accepted（2026-11-24）

Supersedes（精确到句；未列出的决策与句子全部保留）：无。本 ADR 修订的是 [ADR 0045](0045-trading-account-identity-lifecycle-and-accuracy.md) 的**字段语义**（`account_id` 由「平台自造标识」改为「柜台真实投资者代码」），ADR 0045 的**结构与生命周期决策**（全局唯一、`gateway` 只作审计列、删除不删数据、快照原子替换、锚点日顺序约束、注册表投影）**全部保留**——0045 正文不改，只在 Status 段加引用注记。

契约同步：《帧契约：交易网关配置》§2.1 / §2.2 / §2.3 / §5 / §7；《帧契约：通用规则》§5；《帧契约：交易网关状态》§1 / §2.1 / §3；《帧契约：进度推送》；《帧契约：行情网关配置》；契约 rest §1 / §2.9.7 / §2.9.9；契约 webui-ws §2.1 / §2.2。`GLOSSARY` 拆分「账户 ID」「账户名称」并新增「账户身份」相关词条。代码侧实现见后续子票（本 ADR 与契约先于实现；变更 checklist 见总则 §11.3）。

## Context

**问题**：当前 `account_id` 是平台自造的标识（示例值 `account_001`），与柜台真实账户号是两个概念；表单里另外有一个 `ext.user_id`（标着「用户代码」），它同时填 CTP 的 `UserID`（登录 / 认证）与 `InvestorID`（下单 / 撤单 / 结算确认，见 `td_account_session.cpp` 与 `td_ctp_mapping.cpp`）。结果：

- **同一件事有两个名字**（`account_id` 给平台看，`user_id` 给柜台看），配置表单要用户填两遍；
- **`user_id` 是伪字段**——CTP 的 `UserID`（操作员代码）与 `InvestorID`（投资者代码）本是两个字段，但本平台永远用同一个真实账户号填两处，"拆分"没有语义收益，只是让用户多填一格；
- **展示不可读**：账户表、委托 / 成交 / 持仓 / 资金各列只显示 `account_id`，真实账户号（多数是数字串）人记不住，多账户时难以区分；
- **复合 `instance_id` 引入了不稳定的身份维度**：账户级帧（`TD_RTN_STATUS`、TD 的 `RTN_PROGRESS`、`NOTIFY_TD_CONNECTED/DISCONNECTED`）用 `网关名:账户ID` 作 `instance_id`，前端要按第一个 `:` 拆键（`tdAccounts.ts` 5 处），而网关名是**运行期可变的进程实例名**——ADR 0045 已明确「`gateway_name` 不能作为账户身份」。

**CTP 事实**（`third_party/ctp` 头文件）：`TThostFtdcInvestorIDType[13]`（可用 12 字节）、`TThostFtdcUserIDType[16]`（15 字节）。当前对 `account_id` **没有任何长度或字符集校验**，超长会被 `copy_string(..., truncate=true)` 静默截断——下单会打到不存在的账户上。

**用户需求**：`account_id` 就是柜台真实账户号（下单、回报、交易记录全用它，也是"哪个账户"的唯一真相源）；另有一个用户自己起的、可中文的名字用于展示，可改，改后仍唯一；`user_id` 取消（CTP 两处都用 `account_id`）。

**已核对的边界事实**：`MirrorStore` 是 `instance_id → {domain → payload}` 的一维键，一个网关 N 个账户的状态无法挂在网关名下（会互相覆盖），要支持「td 级」必须重建镜像与 WS 快照形状；而 `TD_RTN_STATUS` / `RTN_PROGRESS` **不被策略 SDK 消费**（SDK 事件分发只处理四类交易帧 + `ACCOUNT_STATUS` + 调度 / 输入 / 退出），所以账户级 `instance_id` 不会暴露交易接口。本机 `trade.db` 四表 0 行、`dztd_ctp.json` 账户集为空——硬切成本处于历史最低点。

## Decision

1. **`account_id` = 柜台真实投资者代码**（CTP 填 `InvestorID` 与 `UserID` 两处，同值）。它是账户身份：路由键、交易库四表唯一键的一部分、注册表主键、账户级 `instance_id`。创建后**不可变**（真实账户号变了就是另一个账户，删除重加、历史另起）。
2. **删除 `ext.user_id`**：CTP 的 `UserID`（登录 / 认证）与 `InvestorID`（下单 / 撤单 / 结算确认）都取 `account_id`。不保留别名、不做双读兼容（方案收敛规则：不留双实现）。行情侧 `md-config` 的 `BrokerEntry.user_id` 同步改名为 `account_id`——纯字段改名，语义仍是"行情登录账号"，**不参与任何唯一性**（行情不需要按账户路由，md 的业务键仍是 `name`）。
3. **新增通用层字段 `account_name`（展示名）**：选填；留空则显示时回落 `account_id`（**不做写入时规范化**——"未设置"与"设置成和 id 一样"是两件事，配置文件不存派生态）；可修改，且**任何会话状态下都可改**（纯展示字段），连接参数仍要求 `Idle`（`UpdateAccount` 的既有状态保护只作用于连接参数）。
4. **`account_name` 硬约束**：`trim` 后 ≤ 64 字节 UTF-8、无控制字符（ASCII < 0x20 与 `\t\r\n` 均为非法）；比较前做 **NFC 规范化**，之后字节精确比较（**大小写敏感**，不做折叠）；**全局唯一**（含已禁用 / 离线账户），且与 `account_id` 同一命名空间——任意两个账户的 `account_id` 与 `account_name` **两两不等**（`X.name ≠ Y.name`、`X.name ≠ Y.id`、`X.id ≠ Y.name`）；留空的名字按 `account_id` 参与判定。
5. **权限与清空**：`account_name` 不是身份，**任何情况下都不得影响交易**——任何展示点取不到名字就回落 `account_id`；REST / WS / 交易库都不因名字缺失或非法而失败。
6. **校验分层（各自只校验自己知道的东西）**：
   - **dzweb（通用层，写帧前拒绝）**：`account_id` 非空、≤ 31 字节（`DzAccountId` 容量）、字符集 = ASCII 可见字符 `0x21`–`0x7E`（**禁空格、禁 `:`**、禁控制字符，不限定纯数字）、不得等于任何已注册进程名（`dztd_*`/`dzmd_*`/`dztraderd`/`dzsysmon`/`stg.*`）；`account_name` 按决策 4；`(id, name)` 展平唯一性。
   - **td 网关（CTP 层）**：`account_id` ≤ **12 字节**（`InvestorID` 容量，超长必须报错而不是静默截断）、`broker_id` 非空；配置加载与每次 `AddAccount`/`UpdateAccount` 都校验（手改配置文件不能绕过）。
   - dzweb **不解析 `ext`**（分层不变），因此 CTP 特有的 12 字节超限只能由网关报（四件套：回滚 + 日志 + `NOTIFY_UI` + 回旧配置），不走 REST 错误码。
7. **`instance_id` 分级，取消复合键**：**进程级**帧（`TD_REQ_MODIFY_CONFIG`/`TD_RTN_CONFIG`、`TD_CONNECT`/`TD_DISCONNECT`、md 全部帧）用进程名；**账户级**帧用 **`account_id`**——`TD_RTN_STATUS`、TD 的 `RTN_PROGRESS`、`NOTIFY_TD_CONNECTED`、`NOTIFY_TD_DISCONNECTED`；**无 `instance_id`** 的帧（四类交易推送、`ACCOUNT_STATUS` 两帧、`TD_ORDER_REQ`/`CANCEL`、`TD_REFRESH_REQ`、`td-account-ops` 五帧、`RISK_REJECT`、`NOTIFY_UI`）保持现状。判据：**一次操作 / 一条状态作用于整个网关进程还是单个账户**。`td-status` §1 的 `interface_type` 派生改走「`account_id` → 注册表 → 网关 → 派生」。
8. **账户级镜像的归属与清理**：镜像 key 改为 `account_id` 后，网关停止 / 崩溃 / 被移除时由 dzweb 按注册表反查该网关账户集，逐个清 `td_status` 与 `progress` 并广播（注册表降级时**放弃清理**——宁可残留也不猜）；账户从配置帧消失时同样清掉其账户级镜像。
9. **`account_name` 只走配置链路**：出现在 `RTN_TD_CONFIG`（dzweb 靠它建注册表名字）、`platform.db` 注册表（加列迁移，旧行空串回落）与 REST 读端点；**不进四类交易推送帧、不进 `trade.db`**。显示名由**前端解析层**统一映射（来源 = 注册表 + 配置镜像），改名立刻全站生效（含历史行）；解析不到回落 `account_id`。

**为什么不用其他方案**：

- **(A) 维持「平台自造 id + 柜台 user_id」双字段**：用户要填两遍同一串数字，且容易填错位（`account_id` 填成券商号）——这正是本 ADR 要消除的歧义；`user_id` 删掉后 CTP 两处同值，语义没有损失。
- **(B) 账户级帧继续用 `网关名:账户ID` 复合键**：网关名运行期可变（改名 / 换实例），ADR 0045 已判定它不能作身份；前端要按 `:` 拆键、契约要规定"账户 ID 内不含 `:`"，都是为一个不需要的维度付费。
- **(C) 全部统一为 td 级 `instance_id` + payload 携带 `account_id`**：`MirrorStore` 一维键装不下多账户状态（第二个账户会覆盖第一个），要救就得重建镜像与 WS 快照形状 —— 用协议级重建换**零**速度收益（td 进程写帧两种方案都是取现成字段），且 `NOTIFY_TD_CONNECTED/DISCONNECTED` 是空 payload，改成 td 级会直接丢失账户身份。**可扩展性也更差**：将来一个网关管理更多账户时，嵌套镜像会是持续负担。
- **(D) 用 `account_name` 做路由 / 身份**：名字可改、可中文、可留空，做身份等于把可变展示层焊进业务键。它是外观，永远不进业务路径。
- **(E) 唯一性只查 `account_id`**：会出现"A 的 id 恰好等于 B 的名字"，界面上两行同名。展平判定成本是一行集合运算，不值得留这个坑。
- **(F) 保留 `account_name` 与 `account_id` 的双向兼容读取**：开发期无迁移义务（契约 td-config §6 两级语义：内容非法即致命退出），旧配置删 section 自愈重生成即可；留兼容层就是留屎山。

## Consequences

**正面**：账户概念只剩一个真相源（真实账户号），下单 / 回报 / 交易记录 / 路由 / 注册表全部对齐；配置表单少填一格且不会填错位；账户可读（中文名），多账户可区分，改名立刻全站生效；账户级帧的身份不再随网关改名漂移，前端 5 处 `:` 拆键删除；补上 CTP 长度校验，消灭静默截断导致的错账户下单。

**负面 / 已知限制**：

- **同一真实账户号不能在两家券商同时挂**：真实投资者代码不跨券商唯一（A 公司的 `00001` 与 B 公司的 `00001` 是不同账户），而 `account_id` 仍是全局唯一单键。若要支持，必须把唯一键改为 `(broker_id, account_id)` 复合并推翻 ADR 0045 的单键身份（路由、四表键、注册表、策略挂靠全动）——本 ADR 明确**不做**，作为配置层的使用约束（用户自行避免）。将来确有需求时写新 ADR 取代本条。
- **启动期跨网关重复只告警、不阻断**：平台唯一由 dzweb 在收到配置帧时检查（CRITICAL + `NOTIFY_UI`），master 拉起网关前不校验（master 不解析 td 配置，是为分层；手工改配置文件造成的冲突由改配置的人负责）。
- **被删除账户的历史行**：注册表条目随账户删除消失，历史委托 / 成交回落显示 `account_id`（原始代码，可读，不会显示错误的名字）。
- **注册表加列迁移的已知降级**：`platform.db` 旧行 `account_name` 为空，重启窗口内名字回落 `account_id`，待配置帧 reconcile 出真值。
- **`DzAccountStatus.gateway_name` 仍在 SDK 公开面**（`struct.h`，104B 布局被 `public_surface_invariants_test.cpp` 钉住）：它同时承担 master 兜底权威标记，属独立问题，**不在本 ADR 范围**，另开票处理。
- 旧配置文件（`account_id` 是自造名、有 `ext.user_id`）**硬切**：按「内容非法」致命退出，删 section 让进程自愈重生成后重新添加账户。

## References

- 被修订的字段语义：[ADR 0045](0045-trading-account-identity-lifecycle-and-accuracy.md)（账户身份、生命周期、注册表投影；本 ADR 不改其决策）
- 交易库四表键：[ADR 0041](0041-sqlite-only-trade-store-schema-v1.md) 决策 2
- CTP 类型真相源：`third_party/ctp/{linux,windows}/ThostFtdcUserApiDataType.h`（`InvestorID[13]` / `UserID[16]`）
- 帧契约：`docs/frame_contracts/{general,td-config,td-status,progress,md-config,rest,webui-ws}.md`
