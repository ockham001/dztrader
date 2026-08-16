# 帧契约：自动登录/登出排程

本文件覆盖 `DZ_FRAME_SET_AUTO_LOGIN`、`DZ_FRAME_RTN_AUTO_LOGIN` 两个帧。总则见《帧契约：通用规则》。

两帧均使用 `DzExtInstFrameHeader` 扩展头：SET 的 `instance_id` 为目标网关进程名，RTN 的 `instance_id` 为来源网关进程名（如 `"dzmd_ctp"`）。

类型层真相源：`libs/platform/include/dztrader/platform/auto_login.h`（`AutoLoginConfig`，校验/合并/持久化唯一真相源）。

**作用域**：本契约规定排程配置的 schema、校验、下发与持久化。调度评估算法（交易时段窗口、登出优先于登录、已登录态门控等）属网关运行时行为，不由本契约规定。

---

## 数据结构

### AutoLoginSchedule

| 字段 | 类型 | 说明 |
|------|------|------|
| `login_time` | string | 登录时间 `"HH:MM"`（00:00-23:59） |
| `logout_time` | string | 登出时间 `"HH:MM"`（00:00-23:59） |

- `login_time`/`logout_time` 为进程所在机器本地时间（计算机时钟），契约不做时区预测与限制
- 会话区间 `[login_time, logout_time)`：`login_time < logout_time` 为同日；`login_time > logout_time` 为跨午夜（`[login,24:00) ∪ [00:00,logout)`）
- `login_time == logout_time` 非法（会话区间必须非空）

### AutoLoginConfig

| 字段 | 类型 | RTN 必填 | 增量语义 |
|------|------|------|------|
| `enabled` | bool | 是 | 出现则覆盖 |
| `schedules` | array\<AutoLoginSchedule\> | 是 | 出现则整体覆盖（非递归合并） |

- 全量角色（`RTN_AUTO_LOGIN`）：`enabled`/`schedules` 必须出现
- 增量角色（`SET_AUTO_LOGIN`）：RFC 7386 语义，字段缺失=不修改

```json
{
  "enabled": true,
  "schedules": [
    {"login_time": "08:45", "logout_time": "15:30"},
    {"login_time": "20:45", "logout_time": "02:30"}
  ]
}
```

---

## DZ_FRAME_SET_AUTO_LOGIN

**语义**：设置目标网关的自动登录/登出排程（RFC 7386 增量更新）
**数据流**：形态 1（总则 §4.2）——dzweb → 目标网关（帧头 `instance_id` = 目标行情进程名）；前端入口 `PUT /api/market-sources/{id}/auto-login`（契约 rest）；响应帧 `RTN_AUTO_LOGIN` → 镜像 `auto_login` 域 → WS 消息 `auto_login`
**Payload**：JSON（AutoLoginConfig 子集）

```json
{"enabled": false}
{"schedules": [{"login_time": "08:45", "logout_time": "15:30"}]}
{"schedules": []}
{}
```

**时序**：前端提交触发 → 必回 `RTN_AUTO_LOGIN`（总则 §7 兜底适用）

**校验**（与总则 §8 的差异）：
- `schedules` 出现时整体覆盖：清空用空数组 `[]`，不用 `null`
- **`null` 语义**：本帧无 map 字段，任何位置出现 `null` 均为校验失败
- 校验失败场景：payload 非 object；`enabled` 非 bool；`schedules` 非数组；排程元素非 object；`login_time`/`logout_time` 缺失/非字符串/不符合 `"HH:MM"`；`login_time == logout_time`
- 空对象 `{}` 视为无操作，校验通过，仍回 RTN（当前值）
- 失败四件套：总则 §8

**生效时机**：排程为非连接参数，配置修改对运行中网关**立即生效**（下次调度评估周期生效），无需 `Idle` 状态、无需重连/重启；`enabled=false` 时调度器不触发任何登录/登出动作

---

## DZ_FRAME_RTN_AUTO_LOGIN

**语义**：上报当前自动登录/登出排程
**数据流**：形态 4（总则 §4.2）——目标网关（帧头 `instance_id` = 来源）→ dzweb；无前端入口；镜像 `auto_login` 域 → WS 消息 `auto_login`（dzweb 读取先校验，非法忽略）
**Payload**：JSON，**始终全量**

**校验**：网关发送前校验全量；dzweb 读取时校验全量，非法则记日志并忽略（不更新镜像、不广播）

**触发场景**：

1. `SET_AUTO_LOGIN` 应用后（成功=新值，失败=回滚旧值）
2. 网关启动初始化完成后上报
3. `QUERY_FULL_SNAPSHOT` 响应（总则 §7）

**镜像**：
- dzweb 收到后更新该实例镜像的 `auto_login` 域，并 WS 推送（消息名见契约 webui-ws）

## 持久化与默认值

- SET 应用成功后持久化到目标网关配置文件（默认 section `/auto_login`）
- 启动加载：文件/section 缺失或校验非法时用默认值并修复文件（自愈），不抛异常、不发 RTN
- 默认值：`enabled=true`，`schedules=[]`
