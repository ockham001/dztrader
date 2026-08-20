# REST API 契约：frontend ↔ dzweb

本文件规定 WebUI 前端与 dzweb 后端之间的 REST 端点约定（路径、方法、与 SHM 帧/镜像的联动、生效信号）。总则见《帧契约：通用规则》。

REST 是**请求入口与大数据查询**；状态推送一律走 WS（契约 webui-ws）。REST 端点是总则 §4.2 形态 1/2/3 的前端入口。设置类请求的生效信号是 WS 领域消息（RTN 推送），REST 响应只表示"请求已被 dzweb 处理"。

---

## 1. 通用约定

- 认证：除 `POST /api/login`、`GET /health`、`GET /api/system/info` 外，全部 `/api/*` 需要 JWT（`Authorization: Bearer <jwt>`）；失败回 401 `{"error":"unauthorized"}`（token 缺失）或 `{"error":"invalid_token"}`（校验失败）。
- 响应：JSON（`application/json`）。
- 错误：`{"error": message}` + 对应 HTTP 状态码；404 为 `{"error":"not found"}`。
- 权限：管理类端点需 admin 角色（服务端校验，非前端隐藏）。
- 数据变更联动：变更 DB 的端点成功后由 dzweb 广播 `data_changed{scope}`（契约 webui-ws），前端按 scope 重新 REST 拉取。
- 设置类请求（转发 SHM 帧的端点）：响应只表示"已写事件通道"；生效以 WS 领域消息为准（对应帧契约的 RTN）。超时无 RTN 时按总则 §7 兜底。

---

## 2. 端点清单

### 2.1 鉴权

| 方法/路径 | 语义 |
|---|---|
| POST `/api/login` | 登录，返回 JWT；失败计数与锁定策略见实现 |
| POST `/api/auth/change-password` | 修改本人密码 |

### 2.2 用户与安全

| 方法/路径 | 语义 |
|---|---|
| GET/POST `/api/user`，GET/PUT/DELETE `/api/user/{id}` | 用户 CRUD（admin） |
| PUT `/api/user/{id}/status` | 启用/禁用用户；禁用时强制断开其 WS 连接 |
| PUT `/api/user/{id}/password` | 重置密码（admin） |
| GET/PUT `/api/user/{id}/permissions` | 权限查询/修改 |
| GET/PUT `/api/security/config` | 安全配置（IP 限制/黑白名单模式等） |
| GET/POST `/api/security/blacklist`，DELETE `/api/security/blacklist/{id}` | 黑名单 |
| GET/POST `/api/security/whitelist`，DELETE `/api/security/whitelist/{id}` | 白名单 |
| GET `/api/security/login-history` | 登录历史 |
| POST `/api/security/ack-default-password` | 确认已知晓默认密码 |

### 2.3 行情源

| 方法/路径 | 语义 | 帧/镜像联动 |
|---|---|---|
| GET `/api/market-sources` | 行情源列表（DB 真相源，返回 `is_added=1` 的 `dzmd_*` 行；运行状态由 WS `process_status` 表达） | 读取 DB（真相源，见 §3） |
| GET `/api/market-sources/available` | 扫描可用网关 exe（`dzmd_*`/`dztd_*`）列表（`added` = DB 存在且 `is_added=1`） | 文件系统扫描 |
| GET `/api/market-sources/{id}` | 详情 | DB + 进程镜像 |
| POST `/api/market-sources` | 创建行情源条目 | DB；变更后广播 `data_changed{market_sources}` |
| PUT `/api/market-sources/{id}` | 更新条目（DB 显示名等字段） | DB；变更后广播 `data_changed{market_sources}` |
| DELETE `/api/market-sources/{id}` | 移除进程条目 | → `REQUEST_PROCESS_CONTROL{Remove}`（契约 process）；DB 主表记录保留（历史/审计），进程配置条目消失 = 移除完成的权威信号；成功下发后 DB 行标记 `is_added=0`（主表记录保留，列表不再返回；再次添加时复用该行并复位 `is_added=1`，display_name 保留 DB 现值不覆盖）；`is_added` 置 0 后广播 `data_changed{market_sources}` |
| POST `/api/market-sources/{id}/login` | 行情连接请求 | → `REQUEST_MD_CONNECT`（`instance_id` = 进程名）；生效信号：`progress`（契约 md-subscription） |
| POST `/api/market-sources/{id}/logout` | 行情断开请求 | → `REQUEST_MD_DISCONNECT`；生效信号同上 |
| PUT `/api/market-sources/{id}/auto-login` | 全量设置自动登录排程 `{enabled, schedules}` | → `SET_AUTO_LOGIN`；生效信号：`auto_login` 推送（契约 auto-login） |
| POST `/api/market-sources/{id}/brokers` | 添加经纪商 | → `SET_MD_CONFIG{AddBroker}`；生效信号：`md_rtn_config`（契约 md-config） |
| DELETE `/api/market-sources/{id}/brokers/{name}` | 删除经纪商 | → `SET_MD_CONFIG{RemoveBroker}` |
| PUT `/api/market-sources/{id}/brokers/{name}` | 更新经纪商 | → `SET_MD_CONFIG{UpdateBroker}` |
| PUT `/api/market-sources/{id}/brokers/{name}/frontends` | 更新前置地址 | → `SET_MD_CONFIG{SetFrontends}` |
| PUT `/api/market-sources/{id}/current-broker` | 切换当前经纪商 | → `SET_MD_CONFIG{SetCurrentBroker}` |
| PUT `/api/market-sources/{id}/subscribe-params` | 修改订阅参数 | → `SET_MD_CONFIG{SetSubscribeParams}`（契约 md-config，无状态保护，缺失字段保留旧值）；生效信号：`md_rtn_config` |
| PUT `/api/market-sources/{id}/shm-config` | 修改 SHM 行情通道配置 | → `SET_MD_SHM_CONFIG`（契约 shm，RFC 7386 递归合并；`page_size_mb` 不可变被网关跳过；`preload_points` 内 key 的 value 为 `null` 表示删除该时间点）；生效信号：`md_shm_config`（契约 shm） |
| POST `/api/market-sources/{id}/start` | 启动进程 | → `REQUEST_PROCESS_CONTROL{Start}`；生效信号：`process_status` 带 `event`（契约 process） |
| POST `/api/market-sources/{id}/stop` | 停止进程 | → `REQUEST_PROCESS_CONTROL{Stop}` |
| GET `/api/market-sources/{id}/config` | 读取行情配置 | 进程镜像（`md_config` 域） |
| POST `/api/market-sources/refresh` | 刷新（对账/重扫） | 实现定义 |

### 2.4 进程

| 方法/路径 | 语义 |
|---|---|
| GET `/api/processes` | 全部进程状态列表（读取进程镜像） |

### 2.5 日志

| 方法/路径 | 语义 | 帧/镜像联动 |
|---|---|---|
| GET `/api/logs/files` | 日志文件列表 | — |
| GET `/api/logs/content`、`/stats`、`/aggregate`、`/timeline` | 日志查询/统计 | — |
| POST `/api/logs/level` | 设置日志级别 | → `SET_LOG_CONFIG`；dzweb 自身直调；生效信号：`log_config` 推送（契约 log） |
| POST `/api/logs/flush` | flush 日志缓冲 | → `FLUSH_LOG`；无反馈（契约 log） |

### 2.6 通知

| 方法/路径 | 语义 |
|---|---|
| GET `/api/notifications` | 最近通知（读取 NotifyCache，契约 notify-ui） |

### 2.7 系统设置

| 方法/路径 | 语义 | 帧/镜像联动 |
|---|---|---|
| PUT `/api/settings/event-shm-config` | 事件通道 SHM 配置 merge patch（admin） | → `SET_EVENT_SHM_CONFIG`（契约 shm，`page_size_mb` 由 master 跳过）；生效信号：WS `event_shm_config`（`RTN_EVENT_SHM_CONFIG`，镜像挂 `dztraderd`）。前端当前仅提供 `check_interval_min`/`check_pages`/`check_bytes` 编辑，`preload_points` 仅展示（维护依赖配置文件） |
| GET `/api/settings/master` | 只读展示 dztraderd.json `[master]`/`[shm]` 段（admin）；固定读默认路径 `$DZTRADER_HOME/configs/dztraderd.json`，master 以 argv[1] 自定义配置启动时展示值不代表实际生效配置 | 直接读文件，清理策略/停止超时/`page_size_mb`/`meta_file_size` 仅配置文件可改 |
| GET/PUT `/api/settings/webui` | 读取 / 修改 webui.json（admin；仅 `token_ttl_sec` 与 `notify_cache_size` 可改，`jwt_secret` 仅回传是否存在） | `token_ttl_sec` 热生效（下次登录签发生效），`notify_cache_size` 持久化重启生效 |

---

## 3. 真相源声明

- **行情源列表**：DB（repository）为真相源。行情进程上报 `RTN_MD_CONFIG` 时，dzweb 对未入库的 `dzmd_*` 来源自动对账补录（幂等）。DB 与 master 进程配置（契约 process）的同步通过帧联动实现：添加行情源 + Start 由 master 动态注册（契约 process）。列表条目的生命周期由 DB `is_added` 标记承载（remove 下发成功置 0、再次添加复位 1）。
- **进程存在性与运行状态**：master 进程配置为真相源（契约 process）；REST 列表是投影。
- **配置与状态数据**：进程镜像（MirrorStore）为投影，真相源为对应帧契约的 RTN。
- **用户/安全/权限**：DB 为真相源，变更经 `data_changed` 广播。

---

## 4. 前端义务

- 设置类 REST 请求成功后进入 pending，以对应 WS 领域消息清除（契约 webui-ws §5）；REST 响应成功≠已生效。
- `data_changed{scope}` 到达后按 scope 重新 REST 拉取对应列表；不得用本地缓存回填。
- 行情源列表以 DB 为准（REST 拉取），进程卡片以 `process_config`/`process_status` 为准（WS 推送）。
