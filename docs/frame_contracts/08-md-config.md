# 帧契约：行情网关配置

本文件覆盖 `DZ_FRAME_SET_MD_CONFIG`、`DZ_FRAME_RTN_MD_CONFIG` 两个帧。总则见《帧契约：通用规则》。

本契约仅覆盖**行情网关特有配置**（经纪商、前置地址、订阅参数）。通用配置由各自独立契约覆盖：
- 日志 → 契约 01
- 共享内存 → 契约 02
- 进程管理 → 契约 04
- 自动登录/登出排程 → 契约 05

两帧均使用 `DzExtInstFrameHeader` 扩展头：SET 的 `instance_id` 为目标行情进程名，RTN 的 `instance_id` 为来源行情进程名（如 `"dzmd_ctp"`）。

类型层真相源：`libs/platform/include/dztrader/platform/ctp_md_config.h`（`CtpMdConfig`/`CtpMdConfigOp` 等，校验/应用/脱敏唯一真相源）。

---

## 接口类型识别

行情网关配置帧的 payload 格式由**接口类型**决定。接口类型从 `instance_id`（进程名）派生，**payload 中不携带类型字段**。

### 派生算法

`interface_type` 与 UI 组件的 `ui_card` 是同一个值（前者从 payload schema 角度命名，后者从 UI 组件角度命名）：

1. 去掉 `dzmd_` 前缀得到 tail
2. tail 为空时不是合法的行情进程名（退化情况，不处理）
3. tail 中第一个 `_` 之前的部分为接口类型标识（`interface_type`）
4. tail 不含 `_` 时，`interface_type` = tail 本身

### 示例

| instance_id | tail | interface_type |
|---|---|---|
| `dzmd_ctp` | `ctp` | `ctp` |
| `dzmd_ctp_2` | `ctp_2` | `ctp` |
| `dzmd_ctp_options` | `ctp_options` | `ctp` |
| `dzmd_xtp` | `xtp` | `xtp` |

### 双侧约定

- **后台进程**：每个行情进程仅识别自身的接口类型，按自身类型的 schema 解析 SET payload、构造 RTN payload。
- **dzweb**：透传 SET payload 到目标进程（不解析、不校验 payload 内容），按 `instance_id` 路由；收到 RTN 后按 `instance_id` 定位进程，更新镜像并 WS 推送。

---

## CTP 类型（interface_type = `ctp`）

### 数据结构

本节定义 CTP 类型的 JSON schema，是 SET / RTN payload 的唯一语义真相源。

#### MdConfigOp

| 值 | params 字段 | 状态保护 | 说明 |
|---|---|---|---|
| `AddBroker` | `name`\*, `broker_id`, `user_id`, `password`, `product_info` | 否 | 追加经纪商，`name` 不可与现有重复，不改当前连接。`frontends` 默认为空数组，需后续通过 `SetFrontends` 设置 |
| `RemoveBroker` | `name`\* | 是 | 删除经纪商，子进程自动清空无效选中 |
| `UpdateBroker` | `name`\*, `broker_id`, `user_id`, `password`, `product_info` | 是 | 更新经纪商字段；缺失字段保留旧值；`password` 缺失/空/`"****"` 均保留旧值 |
| `SetFrontends` | `name`\*, `frontends[]`\* | 是 | 整体替换前置地址列表；空数组 `[]` 合法（清空） |
| `SetCurrentBroker` | `name` | 是 | 设置当前选中经纪商；`name` 缺失或为空字符串均表示清空选中；非空时必须指向已存在的经纪商 |
| `SetSubscribeParams` | `subscribe_batch_size`, `subscribe_batch_delay_ms`, `sub_check_interval_ms`, `sub_max_retry` | 否 | 修改订阅参数；缺失字段保留旧值 |

> `*` 标注的字段为必填，缺失或类型不匹配为校验失败。未标注字段为可选，缺失时按 op 语义处理（`UpdateBroker` 缺失=保留旧值，`AddBroker` 缺失=空字符串）。

> `UpdateBroker` 的 `password` 缺失/空/`"****"` 均保留旧值。若用户真实密码恰好为 `"****"`，则无法通过 `UpdateBroker` 设置此密码（会被解释为保留旧值）。

> **状态保护**：标记"是"的 op 在网关状态非 `Idle` 时拒绝。`Idle` = 无 API 实例的未连接态（初始状态或手动断开后）；其余任何状态（连接中/已连接/登录中/已登录）均拒绝。各接口类型的连接态集合由发送方进程状态机定义（CTP：`Idle`/`Connecting`/`Connected`/`LoggingIn`/`LoggedIn`）。

#### BrokerFrontend

| 字段 | 类型 | 说明 |
|---|---|---|
| `address` | string | 前置地址，非空，如 `"tcp://180.168.146.187:10131"` |
| `label` | string | 可选人类可读标签，可为空 |
| `enabled` | bool | 是否启用（注册到 CTP），默认 `true`。多个 enabled 的前置地址同时注册，由 CTP 自动故障切换 |

#### BrokerEntry

| 字段 | 类型 | 说明 |
|---|---|---|
| `name` | string | 不可变 key（仅创建时设置） |
| `broker_id` | string | 经纪公司代码 |
| `user_id` | string | 用户代码 |
| `password` | string | 登录密码 |
| `product_info` | string | 产品信息 |
| `frontends` | array\<BrokerFrontend\> | 前置地址列表 |

#### MdConfig

MD 网关配置（全量结构，RTN payload 用）。

| 字段 | 类型 | 说明 |
|---|---|---|
| `brokers` | array\<BrokerEntry\> | 经纪商列表 |
| `current_broker_name` | string | 当前选中经纪商名，必须为空或指向 `brokers` 中存在的经纪商 |
| `subscribe_batch_size` | int | 每批订阅数量，默认 1000，\> 0 |
| `subscribe_batch_delay_ms` | int | 批间延迟（毫秒），默认 1000，≥ 0 |
| `sub_check_interval_ms` | int | 补订检查间隔（毫秒），默认 3000，\> 0 |
| `sub_max_retry` | int | 补订最大重试，默认 3，≥ 0 |

---

### DZ_FRAME_SET_MD_CONFIG

**语义**：请求目标行情进程执行配置操作（op-based 增量更新）
**方向**：dzweb → 目标行情进程（定向，`instance_id` = 行情进程名）
**Payload**：JSON

| 字段 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `op` | MdConfigOp | 是 | 操作类型 |
| `params` | object | 是 | 操作参数，字段随 `op` 变化（见 MdConfigOp 表） |

```json
{"op": "AddBroker", "params": {"name": "simnow", "broker_id": "9999", "user_id": "00001", "password": "123456", "product_info": ""}}
{"op": "SetCurrentBroker", "params": {"name": "simnow"}}
{"op": "SetFrontends", "params": {"name": "simnow", "frontends": [{"address": "tcp://180.168.146.187:10131", "label": "电信", "enabled": true}]}}
{"op": "UpdateBroker", "params": {"name": "simnow", "user_id": "00002", "password": "****"}}
{"op": "SetSubscribeParams", "params": {"subscribe_batch_size": 500}}
```

**时序**：前端提交触发 → 必回 `RTN_MD_CONFIG`（总则 §7 兜底适用）

**约束**：

- op-based 语义：每个 op 仅修改自身语义范围内的字段
- **`null` 语义**：`params` 内任何位置出现 `null` 均为校验失败（总则 §8）
- 校验失败场景：payload JSON 解析失败；`op` 缺失或非合法枚举值；`params` 缺失或非 object；`params` 内必填字段缺失或类型不匹配（如 `name` 为空字符串）；字段值违反约束（如 `subscribe_batch_size` <= 0）；op 目标不存在（`RemoveBroker`/`UpdateBroker`/`SetFrontends` 的 `name` 不在列表中；`SetCurrentBroker` 的 `name` 非空且不在列表中）；op 目标已存在（`AddBroker` 的 `name` 重复）
- 失败（校验失败/状态保护拒绝/持久化失败）四件套：总则 §8（回滚旧值 + 日志 + NOTIFY_UI 错误弹窗 + 回 `RTN_MD_CONFIG` 旧值）

---

### DZ_FRAME_RTN_MD_CONFIG

**语义**：行情进程上报当前配置（全量，脱敏）
**方向**：行情进程 → dzweb（定向，`instance_id` = 行情进程名）
**Payload**：JSON，**始终全量**

payload 为上方 MdConfig schema 的脱敏 JSON（`password` → `"****"`）。成功推新值，失败推回滚后的旧值，失败原因通过 `NOTIFY_UI` 传达。

```json
{
  "brokers": [{
    "name": "simnow", "broker_id": "9999", "user_id": "00001",
    "password": "****", "product_info": "",
    "frontends": [{"address": "tcp://180.168.146.187:10131", "label": "电信", "enabled": true}]
  }],
  "current_broker_name": "simnow",
  "subscribe_batch_size": 1000,
  "subscribe_batch_delay_ms": 1000,
  "sub_check_interval_ms": 3000,
  "sub_max_retry": 3
}
```

**触发场景**：

1. `SET_MD_CONFIG` 应用后（成功=新值，失败=回滚旧值）
2. 行情进程启动初始化完成后上报
3. `QUERY_FULL_SNAPSHOT` 响应（总则 §7）

**约束**：
- 始终全量：前端直接整体覆盖本地配置镜像
- 脱敏：`password` 字段始终为 `"****"`，不暴露明文

**镜像**：
- dzweb 更新该实例镜像的 `md_config` 域，并 WS 推送

---

## 持久化与加载

- SET 应用成功后持久化到行情进程自身配置文件（默认 section `/md`）
- 进程启动时从文件加载并校验全量：文件缺失/section 缺失/校验非法时用默认值并修复文件（自愈），不抛异常、不发 RTN
- `current_broker_name` 指向不存在的经纪商时自动清空（字段级自愈，不触发全量重置）
- 持久化必须先于 RTN（保存失败则回滚内存后上报旧值）
