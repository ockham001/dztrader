# 帧契约：交易网关配置

本文件覆盖 `DZ_FRAME_TD_REQ_MODIFY_CONFIG`（配置写请求）与 `DZ_FRAME_TD_RTN_CONFIG`（配置全量回报）两个帧。总则见《帧契约：通用规则》。

两帧均使用 `DzExtInstFrameHeader` 扩展头：请求的 `instance_id` = 目标交易网关进程名，回报的 `instance_id` = 来源交易网关进程名（如 `"dztd_ctp"`）。

本契约仅覆盖**交易网关特有配置**（账户连接参数、查询间隔、柜台特有选项）。通用配置由各自独立契约覆盖：日志 → 契约 log；共享内存 → 契约 shm；进程管理 → 契约 process；自动登录/登出排程 → 契约 auto-login。账户登录状态见《帧契约：账户登录状态》；网关细状态见《帧契约：交易网关状态》。

**类型层真相源（CTP 类型，实现侧）**：`apps/ctp/td/td_config.h`（`TdConfig`/`TdConfigOp`/`TdConfigOpReq` 等）。JSON payload schema 以本契约为唯一语义真相源（总则 §6）。

**实现注记**：通用化（通用层 + `ext` 分层、进程级 `SetExt` 替代旧 `SetLockMode`）与启动主动推一次已随 #251 落地；旧 `SetLockMode` op 已按方案收敛规则删除，不留双实现。dzweb 已消费本帧：按 `instance_id` 维护配置镜像并 WS 推送（映射见契约 webui-ws），镜像域为 `td_config`。**旧扁平格式配置文件不再兼容**（账户连接参数从账户对象顶层移入 `ext`）：开发期无迁移义务，旧文件按 §6 校验失败处理。



<!-- pending-symbol: account_name (#467) -->

---

## 1. 接口类型识别

交易网关配置帧的 payload 格式由**接口类型**决定。接口类型从 `instance_id`（进程名）派生，**payload 中不携带类型字段**。

### 派生算法

1. 去掉 `dztd_` 前缀得到 tail
2. tail 为空时不是合法的交易进程名（退化情况，不处理）
3. tail 中第一个 `_` 之前的部分为接口类型标识（`interface_type`）
4. tail 不含 `_` 时，`interface_type` = tail 本身

### 示例

| instance_id | tail | interface_type |
|---|---|---|
| `dztd_ctp` | `ctp` | `ctp` |
| `dztd_ctp_2` | `ctp_2` | `ctp` |
| `dztd_xtp` | `xtp` | `xtp` |

### 双侧约定

- **后台进程**：每个交易网关进程仅识别自身的接口类型，按自身类型的 schema 解析请求 payload、构造回报 payload；账户级通用字段（`account_id`/`account_name`/`enabled`/`risk_control_enabled`/`currency_id`）与柜台特有字段（账户 `ext`）分层，新增接口类型不改通用层。
- **dzweb**：请求 payload 透传到目标进程（不解析、不校验柜台特有 `ext`），按 `instance_id` 路由；但**账户唯一性校验**读取通用层 `op`/`account_id`（§5）。收到回报后按 `instance_id` 定位进程，更新配置镜像并按网关整组 reconcile 账户注册表。

---

## 2. 数据结构（interface_type = `ctp`）

本节定义 CTP 类型的 JSON schema，是请求 / 回报 payload 的唯一语义真相源。通用层 + `ext` 的分层是跨接口类型约定：`ext` 内部键由各接口类型定义（本节为 CTP）。

### 2.1 配置操作（`op`）

| op | params 字段 | 状态保护 | 说明 |
|---|---|---|---|
| `AddAccount` | `account_id`\*、`account_name`、`enabled`、`risk_control_enabled`、`currency_id`、`ext`\* | 否 | 追加账户配置；`account_id` 在本进程配置内重复即拒绝（全局唯一由平台入口校验，§5）；`ext` 为该接口类型的连接参数 |
| `RemoveAccount` | `account_id`\* | 否 | 删除账户配置；**不删除交易数据**（ADR 0045）；若该账户有活跃会话，执行时销毁会话并推 Offline（ADR 0045 决策 5） |
| `UpdateAccount` | `account_id`\* + 以下通用字段/`ext` 的任意子集 | 见下文 | 字段级 merge：通用字段缺失保留旧值；`ext` 内字段缺失保留旧值，`password` 缺失/空串/`"****"` 保留旧值；`frontends` 提供则整体替换。**`account_name` 不受状态保护**（纯展示字段，任意会话状态可改）；仅当请求触及连接参数（`ext` 内任何键）时才要求目标账户 `Idle` |
| `SetAccountEnabled` | `account_id`\*、`enabled`\* | 否 | 启用/禁用只影响后续连接请求（禁用后连接被拒），不强制断开已有会话（断开由连接控制帧负责） |
| `SetAccountRiskControl` | `account_id`\*、`enabled`\* | 否 | 账户级风控开关（`enabled` = 是否启用风控） |
| `SetAccountCurrency` | `account_id`\*、`currency_id`\* | 否 | 账户币种 |
| `SetQryIntervals` | `qry_min_interval_ms` | 否 | 查询通道最小发送间隔（毫秒）；缺失字段保留旧值，提供值必须 > 0（默认 1100）。同一账户任意两笔 CTP 查询请求的最小间隔，登录链与运行期资金刷新共用，热更新立即作用于后续调度（契约 td-data-sync §4.1 / [ADR 0055](../adr/0055-td-query-channel-event-driven-fund-refresh.md)）。**无周期查询**（[ADR 0049](../adr/0049-position-pure-incremental-no-periodic-query.md)） |
| `SetExt` | `ext`\* | 否 | 进程级柜台特有配置的字段级补丁（缺失字段保留旧值） |

> `*` 标注的字段为必填，缺失或类型不匹配为校验失败。

> **状态保护**：`UpdateAccount` 触及**连接参数**（`ext` 内任何键）时，目标账户存在活跃会话（登录中/已登录/断线重连中）即拒绝，仅 `Idle` 允许（连接参数不可热改）；只改通用层字段（`account_name` 等）时不要求 `Idle`——展示名不是连接参数，改名不该逼用户先登出。`RemoveAccount` 不受状态保护，删除时销毁会话并推 Offline。拒绝走总则 §8 四件套（回滚 + 日志 + NOTIFY_UI 错误弹窗 + 回 `RTN_TD_CONFIG` 旧值）。

### 2.2 账户配置（通用层）

| 字段 | 类型 | 说明 |
|---|---|---|
| `account_id` | string | **柜台真实投资者代码**，不可变 key，全局唯一（跨接口类型，ADR 0045 / ADR 0056）。它是账户身份：下单 / 撤单路由键、交易库四表唯一键、账户级 `instance_id`（总则 §5）。字符集 ASCII 可见字符 `0x21`–`0x7E`（禁空格、禁 `:`）；平台侧 ≤ 31 字节，CTP 侧 ≤ 12 字节（§2.3） |
| `account_name` | string | **展示名**，选填。留空 = 显示时回落 `account_id`（**不做写入时规范化**）。`trim` 后 ≤ 64 字节 UTF-8，无控制字符；比较前 NFC 规范化、字节精确比较（大小写敏感）。全局唯一且与 `account_id` **展平判定**（任意两账户的 id 与 name 两两不等；留空按 `account_id` 参与判定）。纯展示字段：**不参与路由、不进交易库、任何情况下不得影响交易**；取不到就回落 `account_id`。可修改 |
| `enabled` | bool | 是否启用，默认 `true`；禁用后连接请求被拒 |
| `risk_control_enabled` | bool | 账户级风控开关，默认 `false` |
| `currency_id` | string | 账户币种，默认 `CNY` |
| `ext` | object | 该接口类型的连接参数（CTP schema 见 2.3） |

### 2.3 CTP `ext`

账户级 `ext` 键：

| 字段 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `broker_id` | string | 是 | 经纪公司代码 |
| `password` | string | 是 | 登录密码；回报中脱敏为 `"****"`，请求中缺失/空串/`"****"` 表示保留旧值 |
| `product_info` | string | 否 | 产品信息，可空 |
| `frontends` | array\<Frontend\> | 否 | 前置地址列表，默认空数组；连接时至少一个 `enabled` 且地址非空的前置，否则拒连 |
| `auth_code` | string | 否 | CTP 认证码，可空 |
| `app_id` | string | 否 | CTP AppID，可空 |
| `flow_dir` | string | 否 | 流文件目录；空 = 由网关自动构造 |

> CTP 的 `UserID`（登录 / 认证）与 `InvestorID`（下单 / 撤单 / 结算确认）**都取通用层 `account_id`**，没有独立字段（ADR 0056）。**CTP 层校验**：`account_id` ≤ 12 字节（`TThostFtdcInvestorIDType[13]` 可用长度）、`broker_id` 非空；超长必须报错拒绝，**不得静默截断**（截断会把订单打到不存在的账户）。配置加载与每次 `AddAccount`/`UpdateAccount` 都校验，手改配置文件不能绕过；失败走总则 §8 四件套。

`Frontend`：

| 字段 | 类型 | 说明 |
|---|---|---|
| `address` | string | 前置地址，非空，如 `"tcp://180.168.146.187:10131"` |
| `label` | string | 可选人类可读标签，可为空 |
| `enabled` | bool | 是否启用，默认 `true`；多个启用前置同时注册，由接口库自动故障切换 |

进程级 `ext` 键（`SetExt` 补丁目标）：

| 字段 | 类型 | 说明 |
|---|---|---|
| `enable_lock_mode` | bool | 开平仓转换 LOCK 模式，默认 `true` |

### 2.4 配置全量结构（回报 payload）

| 字段 | 类型 | 说明 |
|---|---|---|
| `qry_min_interval_ms` | int | 查询通道最小发送间隔（毫秒），> 0，默认 1100；含义见 §2.1 / 契约 td-data-sync §4.1。字段 qry_account_interval_s / qry_flush_interval_ms 已随 ADR 0055 删除（从未接入 / 从未被消费） |
| `accounts` | array\<AccountConfig\> | 账户配置列表（2.2 + 2.3） |
| `ext` | object | 进程级柜台特有配置（2.3 进程级 ext） |

```json
{
  "qry_min_interval_ms": 1100,
  "accounts": [{
    "account_id": "00001",
    "account_name": "我的主账户",
    "enabled": true,
    "risk_control_enabled": false,
    "currency_id": "CNY",
    "ext": {
      "broker_id": "9999",
      "password": "****",
      "product_info": "",
      "frontends": [{"address": "tcp://180.168.146.187:10131", "label": "telecom", "enabled": true}],
      "auth_code": "",
      "app_id": "",
      "flow_dir": ""
    }
  }],
  "ext": {"enable_lock_mode": true}
}
```

---

## 3. DZ_FRAME_TD_REQ_MODIFY_CONFIG

**语义**：请求目标交易网关执行配置操作（op-based 增量更新）
**数据流**：形态 1（总则 §4.2）——dzweb → 目标交易网关（帧头 `instance_id` = 网关进程名）；dzweb 透传请求 payload（接口类型识别见 §1）；前端入口 = 账户配置写端点（契约 rest）；响应帧 `RTN_TD_CONFIG` → dzweb 配置镜像 → WS（映射见契约 webui-ws，随实现票同步）
**Payload**：JSON

| 字段 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `op` | string | 是 | 操作类型，取值见 2.1 |
| `params` | object | 是 | 操作参数，字段随 `op` 变化（见 2.1） |

```json
{"op": "AddAccount", "params": {"account_id": "00001", "account_name": "我的主账户", "enabled": true, "ext": {"broker_id": "9999", "password": "123456", "product_info": ""}}}
{"op": "UpdateAccount", "params": {"account_id": "00001", "account_name": "改名后的账户名"}}
{"op": "UpdateAccount", "params": {"account_id": "00001", "ext": {"password": "****"}}}
{"op": "SetQryIntervals", "params": {"qry_min_interval_ms": 1000}}
{"op": "SetExt", "params": {"ext": {"enable_lock_mode": false}}}
```

**时序**：前端提交触发 → 必回 `RTN_TD_CONFIG`（总则 §7 兜底适用）

**约束**：

- op-based 语义：每个 op 仅修改自身语义范围内的字段；`account_id` 是不可变 key（真实账户号变了就是另一个账户）。
- **`null` 语义**：`params` 内任何位置出现 `null` 均为校验失败（总则 §8）。
- 校验失败场景：payload JSON 解析失败；`op` 缺失或非合法枚举值；`params` 缺失或非 object；必填字段缺失或类型不匹配（如 `account_id` 为空串）；字段值违反约束（如查询间隔 <= 0；`account_id` 超长 / 含非法字符；`account_name` 超长 / 含控制字符；`account_name` 与任何已配置账户的 id 或 name 重复）；op 目标不存在（`UpdateAccount`/`RemoveAccount`/`SetAccountEnabled` 等的 `account_id` 不在本进程配置中）或已存在（`AddAccount` 重复）；状态保护拒绝。
- 失败四件套：总则 §8（回滚旧值 + 日志 + NOTIFY_UI 错误弹窗 + 回 `RTN_TD_CONFIG` 旧值）。
- 持久化必须先于回报（§6）。

---

## 4. DZ_FRAME_TD_RTN_CONFIG

**语义**：交易网关上报当前配置（全量，脱敏）
**数据流**：形态 4（总则 §4.2）——交易网关（帧头 `instance_id` = 来源）→ dzweb；无前端入口；配置镜像（key = `instance_id`）→ WS（映射见契约 webui-ws）
**Payload**：JSON，**始终全量**，schema 见 2.4

payload 为 2.4 结构的安全 JSON（每个账户 `ext.password` → `"****"`）。成功推新值，失败推回滚后的旧值，失败原因通过 `NOTIFY_UI` 传达。

**触发场景**：

1. `TD_REQ_MODIFY_CONFIG` 应用后（成功=新值，失败=回滚旧值）
2. **交易网关启动初始化完成后主动推一次**（含配置为空的账户集），供 dzweb 账户注册表启动快照
3. `QUERY_FULL_SNAPSHOT` 响应（总则 §7）

**约束**：
- 始终全量：dzweb 直接整体覆盖该实例的配置镜像
- 脱敏：`password` 始终为 `"****"`，不暴露明文
- 回报先于/等于前端生效信号：前端 pending 由 `RTN_TD_CONFIG` 到达清除

**镜像**：dzweb 按 `instance_id` 维护交易网关配置镜像（供账户配置页），并按网关整组 reconcile 账户注册表（§5）；不进交易记录镜像。

---

## 5. 账户唯一性与注册表

`account_id` **全局唯一**（同一时刻一个账户只属于一个交易接口实例；跨接口类型也不冲突，ADR 0045）。`account_name` **同样全局唯一**，且与 `account_id` 展平判定（任意两账户的 id 与 name 两两不等，§2.2）——名字只是展示，但撞名会让界面出现两个同名账户，因此在平台入口一并拦截。

- **平台入口校验**：UI 添加/编辑账户时先查、dzweb 为权威校验点——按网关维护已知 td 配置，并持久化 `account_id → gateway` 注册表（`platform.db` 小表，尽力而为），使 dzweb 重启、网关停机期间仍能校验；已禁用/离线账户同样占位。写入请求帧前拒绝与任何已知账户重复的 `account_id`（同网关或跨网关）**以及重复 / 交叉冲突的 `account_name`**，拒绝不写 `TD_REQ_MODIFY_CONFIG`，失败经 REST 传达（契约 rest）。
- **注册表自愈**：配置回报（§4）到达时按网关整组 reconcile（以帧为权威，自愈错误状态）；内存优先、DB 写失败只记 ERROR、不阻塞账户操作；启动时加载注册表，加载失败则从空开始、随各网关配置回报重建（已知降级：最多少覆盖一个网关）；网关被移除时清掉该网关条目。
- **注册表内容**：持久化的列是 `account_id → gateway` 与 `account_name`（后者仅用于展示回落，不进任何业务判定；重启窗口内的空值回落 `account_id`）。
- **td 侧**：仅保留自身配置文件内的 `account_id` / `account_name` 查重（同一份配置内的自洽），不是跨网关防线。
- **启动期跨网关重复**：dzweb 收到配置帧时若发现同一 `account_id` 出现在两个网关，记 **CRITICAL** + `NOTIFY_UI` 弹窗提示用户处理（帧为权威，注册表按到达顺序改判归属，自愈到单一网关）；**不阻断网关启动、不自动登出**——这种状态只可能由手工修改配置文件造成，风险由改配置的人承担，平台的责任是检测并告警（ADR 0056）。
- **已知绕过**：手工修改配置文件、或从未被 dzweb 感知的网关配置可绕过平台校验——属用户自担风险（不因此引入跨进程锁 / 中心账户注册表 / master 协调，ADR 0045）。

---

## 6. 持久化与加载

- 请求应用成功后持久化到交易网关自身配置文件（默认 section `/td`）；保存失败先回滚内存再回 `RTN_TD_CONFIG` 旧值（四件套）。
- 进程启动时从文件加载并校验全量，分两级：
  - **文件缺失 / section 缺失**：用默认值并修复文件（自愈），不抛异常、不退出——这是"首次启动"的正常形态。
  - **内容非法**（JSON 解析失败 / 校验失败，含旧扁平格式、含已删除的 `ext.user_id` 之外的字段缺失、含 CTP `account_id` 超 12 字节）：**致命**——记 CRITICAL（含原因）后退出非零码，**不**回落默认值。理由：账户连接参数是运维资产，静默清空比启动失败更危险，且自愈后的空账户集无法被运维区分于"本来就空"。**旧格式不做迁移**：ADR 0056 硬切，删掉 `td` section 让进程自愈重生成后重新添加账户。
  - 两种情况下**启动都不发回报**；但启动初始化完成后仍按 §4 场景 2 主动推一次（当前生效值）。
- 查询通道最小间隔（`qry_min_interval_ms`）修改后立即生效——作用于登录链与运行期资金刷新的后续调度（持仓无周期查询，见 ADR 0049 / ADR 0055）；连接参数修改要求目标账户 `Idle`（§2.1 状态保护），`account_name` 不受该保护。
- 配置文件缺失时由进程自动生成默认配置（仓库不保留模板文件）。

---

## 7. 校验（仅写与总则不同的规则）

- `account_id`：非空、ASCII 可见字符 `0x21`–`0x7E`（禁空格、禁 `:`、禁控制字符），平台侧 ≤ 31 字节；CTP 侧 ≤ 12 字节（§2.3）。**分层**：dzweb 校验通用层（长度 / 字符集 / 全局唯一），td 网关校验 CTP 层（12 字节）——dzweb 不解析 `ext`（§1 分层约定），因此 CTP 超限只能由网关报（四件套）。
- `account_name`（通用层，dzweb 权威校验；td 侧也自检同一份配置内不重复）：`trim` 后 ≤ 64 字节 UTF-8、无控制字符（ASCII < 0x20 与 `\t\r\n` 非法）；比较前 NFC 规范化、之后字节精确比较（大小写敏感）；与 `account_id` 展平判定唯一。
- `account_id` **不得等于任何已注册进程名**（`dztraderd` / `dzsysmon` / `dzmd_*` / `dztd_*` / `stg.*`）：账户级帧的 `instance_id` 就是 `account_id`，与进程级 `instance_id` 共用同一个镜像顶层键空间，同名会让两类实例的 `progress` 域互相覆盖。
- `frontends` 元素：`address` 非空才注册；无任何 `enabled` 且地址非空的前置时连接请求被拒（WARN + NOTIFY_UI）。
- 接口实现必须显式校验 JSON 类型（`NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT` 会把缺失字段回退默认值、整体 `null` 解码为全默认结构且不抛错），不得依赖解码错误代替本节校验。

---

## 8. 镜像与前端义务

- dzweb：按 `instance_id`（= 网关进程名）更新镜像 domain `td_config` 并 WS 推送 `td_rtn_config`（映射见契约 webui-ws §2.2）；收到回报后按 §5 整组 reconcile 账户注册表。账户注册表是读端点「已登记账户全集」的第三个来源（契约 rest §2.9.1）。
- 前端：配置页按 `interface_type`（从 `instance_id` 派生）选择账户表单字段；保存后以 `RTN_TD_CONFIG` 清除 pending 并整体覆盖本地镜像；不得展示明文密码；未知字段忽略（总则 §8）。
- 账户配置页的账户列表以注册表（经 REST 读端点返回的账户全集）为视角，而不是单网关的配置镜像——同一页面要同时展示多个交易网关的账户（契约 td-config §5 的跨网关唯一性）。
- 前端展示 `account_name`（留空回落 `account_id`），解析层在契约 rest §2.9.9 / webui-ws §5 定义；`account_name` 只在展示与表单出现，不用于任何路由或过滤。

---

## 9. 变更流程

本契约随交易网关配置实现演进；修改必须执行总则 §11.3 变更 checklist。
