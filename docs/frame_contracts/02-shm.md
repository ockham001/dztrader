# 帧契约：SHM 通道配置

本文件覆盖 `DZ_FRAME_SET_EVENT_SHM_CONFIG`、`DZ_FRAME_RTN_EVENT_SHM_CONFIG`、`DZ_FRAME_SET_MD_SHM_CONFIG`、`DZ_FRAME_RTN_MD_SHM_CONFIG`、`DZ_FRAME_PRELOAD_EVENT_SHM`、`DZ_FRAME_PRELOAD_MD_SHM`、`DZ_FRAME_UPDATE_SHM_EVENT_SUBSCRIBER`、`DZ_FRAME_UPDATE_SHM_MD_SUBSCRIBER`、`DZ_FRAME_REQUEST_MD_READER_REGISTER`、`DZ_FRAME_REQUEST_MD_READER_UNREGISTER` 十个帧。总则见《帧契约：通用规则》。

- 事件通道（SET/RTN/预加载/订阅者刷新）：帧头无 `instance_id`（`DzExtFrameHeader`），目标/来源均为 master（唯一），所有进程可见，dzweb 消费。
- 行情通道：SET/RTN/预加载/订阅者刷新均含 `instance_id`（`DzExtInstFrameHeader`）= 行情进程名（与通道名一致：一个进程一个数据通道）。

类型层真相源：`libs/platform/include/dztrader/platform/shm_config.h`（`EventShmConfig`/`MdShmConfig`，校验/合并/持久化唯一真相源）。

---

## 数据结构

### ShmConfig（JSON，schema 真相源在本契约）

| 字段 | 类型 | 说明 |
|------|------|------|
| `page_size_mb` | uint64 | 页大小（MB），仅人工编辑配置文件修改，启动后不可变 |
| `preload_points` | object | 预加载点，key=`"HH:MM"`（00:00-23:59），value = `{pages, bytes}` |
| `check_interval_min` | int32 | 周期检查间隔（分钟），0=不周期检查，范围 [0, 1440] |
| `check_pages` | uint32 | 周期检查时预加载页数，范围 [0, 8] |
| `check_bytes` | uint64 | 周期检查时预加载字节数，范围 [0, 2^40 (1TB)] |

`preload_points` 每个 key 的 value 为 `ShmPreloadPoint`：

| 字段 | 类型 | 说明 |
|------|------|------|
| `pages` | uint32 | 预加载页数，范围 [0, 8] |
| `bytes` | uint64 | 预加载字节数，范围 [0, 2^40 (1TB)] |

全量 JSON 示例：

```json
{
  "page_size_mb": 32,
  "preload_points": {
    "08:45": {"pages": 1, "bytes": 0}
  },
  "check_interval_min": 5,
  "check_pages": 1,
  "check_bytes": 0
}
```

---

## DZ_FRAME_SET_EVENT_SHM_CONFIG / DZ_FRAME_SET_MD_SHM_CONFIG

**语义**：设置目标 SHM 通道配置，RFC 7386 JSON Merge Patch 递归合并
**数据流**：形态 1（总则 §4.2）——dzweb → master（事件通道，帧头无 `instance_id`，master 唯一）/ dzweb → 各行情进程（行情通道，帧头 `instance_id` = 行情进程名）；前端入口：当前未定义（shm_config 类消息暂无 UI 消费，见契约 10 §2.2）；响应帧 `RTN_*_SHM_CONFIG` → 镜像 `event_shm_config`（挂 `dztraderd`）/ `md_shm_config`（挂行情实例）域 → WS 消息同名
**Payload**：JSON（ShmConfig 子集）

```json
{"check_interval_min": 10}                                  // 增量：只改 check_interval_min
{"preload_points": {"08:45": {"pages": 2}}}                  // 递归合并：08:45 的 pages 改为 2，bytes 保留
{"preload_points": {"08:45": null}}                          // 删除 preload_points 中 "08:45" 这个 key
{"page_size_mb": 64}                                          // 无操作：page_size_mb 不可变，忽略
{}                                                            // 空对象：无操作，仍回 RTN（当前值）
```

**时序**：前端提交触发 → 必回对应 RTN（总则 §7 极端情况兜底适用）

**校验**（与总则 §8 的差异）：
- `page_size_mb` 不可变：SET 中完全跳过该字段（不解析、不校验、不报错，RTN 仍带原值）
- `preload_points` 值为空 object `{}` 时为无操作（递归合并，无 key 改动）
- 新增 `preload_points` 的 key 时，缺失的 `pages`/`bytes` 补默认值 0 后再校验范围
- **`null` 语义**（本契约唯一允许 null 的位置）：仅 `preload_points` 内部某个时间点 key 的 value 为 `null` 时表示删除该 key；其余任何位置 `null` 均为校验失败
- 校验失败场景：payload 非 object；字段类型不匹配；值超范围（`check_interval_min` ∉ [0,1440]、`pages` ∉ [0,8]、`bytes` ∉ [0,2^40]）；`preload_points` 的 key 不符合 `"HH:MM"` 格式或其 value 非 object 且非 null
- `preload_points` 的 key 数量上限由实现限制（防御性），契约不规定具体值
- 校验失败四件套：总则 §8（回滚旧值 + 日志 + NOTIFY_UI 错误弹窗 + 回 RTN 旧值）

---

## DZ_FRAME_RTN_EVENT_SHM_CONFIG / DZ_FRAME_RTN_MD_SHM_CONFIG

**语义**：上报当前 SHM 通道配置
**数据流**：形态 4（总则 §4.2）——master（帧头无 `instance_id`，镜像挂固定 `dztraderd`）/ 各行情进程（帧头 `instance_id` = 来源）→ dzweb；无前端入口；镜像与 WS 消息名见下方"镜像"
**Payload**：JSON，**始终全量**

**触发场景**：

1. 对应 SET 应用后（成功=新值，失败=回滚旧值）
2. 进程启动初始化完成后上报
3. `QUERY_FULL_SNAPSHOT` 响应（总则 §7）

**镜像**：
- 事件通道：帧头无 `instance_id`，dzweb 挂固定实例 `dztraderd`，更新镜像 `event_shm_config` 域
- 行情通道：按 `instance_id` 更新该实例镜像的 `md_shm_config` 域
- 更新后 WS 推送（消息名见契约 10）

---

## DZ_FRAME_PRELOAD_EVENT_SHM / DZ_FRAME_PRELOAD_MD_SHM

**语义**：广播预加载 SHM 通道（通知接收方对相应 SHM 通道执行预加载）
**数据流**：形态 5（总则 §4.2）——发送方完成自身通道预加载后广播；接收方过滤（`PRELOAD_MD_SHM` 按 `instance_id` 匹配）；无前端入口、无 RTN、不进镜像
**Payload**：struct `DzShmPreload`（真相源：`libs/strategy_api/include/dztrader/struct.h`，非 JSON；字段表不重复）

**两帧差异**：

| 帧类型 | 逻辑方向 | 帧头 |
|--------|------|------|
| `PRELOAD_EVENT_SHM` | master → 所有子进程 | `DzExtFrameHeader`（无 `instance_id`） |
| `PRELOAD_MD_SHM` | 行情进程 → 订阅该行情源的进程 | `DzExtInstFrameHeader`（`instance_id` = 通道名） |

**触发场景**（两帧一致）：

1. `check_interval_min` 周期检查定时器到期（payload = `check_pages`/`check_bytes`）
2. `preload_points` 时间点匹配（payload = 该时间点的 `pages`/`bytes`）

**约束**：
- 广播方在自身完成对应通道预加载后广播
- 接收方过滤：`PRELOAD_MD_SHM` 按 `instance_id` 匹配（仅订阅该行情源的进程处理）；`PRELOAD_EVENT_SHM` 不区分 `instance_id`
- 无 RTN

---

## DZ_FRAME_UPDATE_SHM_EVENT_SUBSCRIBER / DZ_FRAME_UPDATE_SHM_MD_SUBSCRIBER

**语义**：通知各进程刷新事件/行情通道 writer 的订阅者列表缓存
**数据流**：形态 5（总则 §4.2）——master → 广播（事件通道，帧头无 `instance_id`）/ master → 定向（行情通道，帧头 `instance_id` = 行情进程名）；无前端入口、无 RTN、不进镜像（dzweb 明确忽略 `UPDATE_SHM_MD_SUBSCRIBER`）
**Payload**：空

**触发场景**：
- `UPDATE_SHM_EVENT_SUBSCRIBER`：master 注册/注销事件通道订阅者、重置订阅者列表后
- `UPDATE_SHM_MD_SUBSCRIBER`：master 注册/注销某行情通道订阅者后

**约束**：
- 接收方收到后刷新自身 writer 的订阅者缓存（下次写帧通知新列表）
- 无 RTN
- `UPDATE_SHM_MD_SUBSCRIBER` 属于行情通道刷新，dzweb 明确忽略（不更新镜像、不 WS 推送）

---

## DZ_FRAME_REQUEST_MD_READER_REGISTER / DZ_FRAME_REQUEST_MD_READER_UNREGISTER

**语义**：请求 master 注册/注销本进程为指定行情通道的**读者**（用于唤醒与页删除下限保护，与合约订阅无关）
**数据流**：形态 5（总则 §4.2）——策略/数据存储等订阅进程 → master（`DzExtInstFrameHeader`，帧头 `instance_id` = 目标行情进程名 = 通道名，payload `subscriber` = 读者身份）；无前端入口、无 RTN（校验拒绝仅记日志）；成功副作用为 master 广播 `UPDATE_SHM_MD_SUBSCRIBER`
**Payload**：JSON

```json
{"subscriber": "stg.alpha"}
```

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `subscriber` | string | 是 | 读者身份 = 策略实例 ID（`stg.<name>`，总则 §5），即该进程的事件信号量名 |

**约束**：
- master 校验 `subscriber` 形如 `stg.<name>` 且 `name` 为已注册策略条目（registry）；通道不存在（md 未启动/已移除）时拒绝——均仅记日志，无 RTN
- **无 RTN 兜底**（总则 §7）：注册失败不阻断数据消费（reader 游标独立于注册）；唤醒缺失由"单信号量 + 任意事件帧唤醒后排空"兜底，注册成功的 `UPDATE_SHM_MD_SUBSCRIBER` 帧本身即是一次唤醒
- **启动顺序**：md 先于策略启动（master `start_all` 两趟），保证注册时目标通道已存在
- 注册/注销成功后 master 更新行情通道 readers 并广播 `UPDATE_SHM_MD_SUBSCRIBER`（该帧的触发场景见上方"UPDATE_SHM_*_SUBSCRIBER"节）
- **注销三路径**：读者主动注销；读者进程退出时 master 代清理（对全部 md 通道幂等移除）；md 通道删除时随通道清空
- 重复注册/注销幂等（add 已存在跳过、remove 缺失 key 为 no-op），多路径叠加无害

## 持久化与默认值

- SET 应用成功后持久化到目标进程配置文件（默认 section：事件 `/event_shm`、行情 `/md_shm`）
- 启动加载：文件/section 缺失或内容非法时用默认值并修复文件（自愈），不抛异常、不发 RTN
- 默认值：事件通道 `page_size_mb=64`，行情通道 `page_size_mb=1024`；其余字段两者相同（`preload_points={}`、`check_interval_min=0`、`check_pages=0`、`check_bytes=0`）
