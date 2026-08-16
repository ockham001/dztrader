# 帧契约：行情网关状态

本文件覆盖 `DZ_FRAME_RTN_MD_STATUS` 一个帧。总则见《帧契约：通用规则》。

帧使用 `DzExtInstFrameHeader` 扩展头，`instance_id` 为来源行情进程名（如 `"dzmd_ctp"`）。

仅 RTN 推送，无 SET、无持久化、无文件 IO。行情进程在网关元信息或订阅统计变化时主动推送。

网关的动态状态（连接/登录/断开等）与步骤进度由 `RTN_PROGRESS`（契约 progress）覆盖，本帧不包含状态与进度字段。

---

## 接口类型识别

与契约 md-config（《行情网关配置》）完全一致：payload 格式由 `instance_id` 派生的 `interface_type` 决定，payload 中不携带类型字段。当前仅定义 CTP 类型。

---

## CTP 类型（interface_type = `ctp`）

### 数据结构

#### MdStatus

行情网关状态（全量结构，RTN payload 用）。

| 字段 | 类型 | 说明 |
|---|---|---|
| `api_version` | string | CTP API 版本字符串，构造时设置一次 |
| `sys_version` | string | 登录响应中的系统版本；未登录/进入 Idle 时为空串 |
| `trading_day` | string | 交易日 `"YYYYMMDD"`，登录响应填充；未登录/进入 Idle 时为空串 |
| `login_time` | string | 登录时间 `"YYYY-MM-DD HH:MM:SS"`（服务器本地时间，由登录响应时间戳转换），登录响应填充；未登录/进入 Idle 时为空串 |
| `expected_subscribe_count` | uint | 期望订阅合约数（有订阅者的合约条目数） |
| `subscribed_count` | uint | 已确认订阅合约数 |

JSON 示例：

```json
{
  "api_version": "v6.7.2",
  "sys_version": "v6.7.2_20240105",
  "trading_day": "20260808",
  "login_time": "2026-08-08 08:45:32",
  "expected_subscribe_count": 5000,
  "subscribed_count": 5000
}
```

**清空语义**：进入未连接态（`Idle`）时 `sys_version`/`trading_day`/`login_time` 清空、订阅统计归零并推送；登录失败/登录响应解析失败时 `login_time` 清空并推送（防止前端误显示过期登录信息）。前端依赖全量覆盖，不自行推断清空时机。

**断线（`Disconnected`，SDK 自动重连中）期间的保留语义**：上述字段**不清空**——`trading_day` 的语义是当前交易日（由登录响应确定，是本交易日内全部业务数据的时间锚），断线不改变交易日；`sys_version`/`login_time` 同样保留，重连成功后由新登录响应覆盖。断线时推送本帧：`subscribed_count` 归零、`expected_subscribe_count` 保留。网关的连接/登录**状态**不经本帧传达（本帧无状态字段），前端状态感知以 `RTN_PROGRESS`（契约 progress）为准；断线时本帧推送的作用是同步订阅统计变化。

---

### DZ_FRAME_RTN_MD_STATUS

**语义**：行情进程上报当前网关状态（全量）
**数据流**：形态 4（总则 §4.2）——行情进程（帧头 `instance_id` = 来源）→ dzweb；无前端入口；镜像 `md_status` 域 → WS 消息 `md_rtn_status`
**Payload**：JSON（MdStatus），**始终全量**

**触发场景**：

1. 网关元信息或订阅统计变化时推送（如登录成功填充版本/交易日/登录时间，订阅统计随订阅进度变化，未连接态清空）
2. 行情进程启动初始化完成后上报
3. `QUERY_FULL_SNAPSHOT` 响应（总则 §7）

**约束**：
- 始终全量：前端直接整体覆盖本地状态镜像

**镜像**：
- dzweb 更新该实例镜像的 `md_status` 域，并 WS 推送
