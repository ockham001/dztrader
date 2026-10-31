# REST API 契约：frontend ↔ dzweb

本文件规定 WebUI 前端与 dzweb 后端之间的 REST 端点约定（路径、方法、与 SHM 帧/镜像的联动、生效信号）。总则见《帧契约：通用规则》。

REST 是**请求入口与大数据查询**；状态推送一律走 WS（契约 webui-ws）。REST 端点是总则 §4.2 形态 1/2/3 的前端入口。设置类请求的生效信号是 WS 领域消息（RTN 推送），REST 响应只表示"请求已被 dzweb 处理"。

---

## 1. 通用约定

- 认证：除 `POST /api/login`、`GET /health`、`GET /api/system/info` 外，全部 `/api/*` 需要 JWT（`Authorization: Bearer <jwt>`）；失败回 401 `{"error":"unauthorized"}`（token 缺失）或 `{"error":"invalid_token"}`（校验失败）。这两个值是**历史遗留的固定标识**（前端不据此分支——非登录接口的 401 一律按会话失效处理），属下条列出的已知例外，不适用 `code` 规则。
- 响应：JSON（`application/json`）。
- 错误：`{"error": message}` + 对应 HTTP 状态码；404 为 `{"error":"not found"}`。`error` 承载**人类可读消息**（小写英文开头、无句号），前端可直接兜底展示——**登录失败除外**（下条：其文案由 `code` 映射中文，不得展示 `error`）。
- 需要**机器可判别**的失败额外携带 `code`（snake_case 机器码）：`{"error": <human message>, "code": <snake_case_code>}`。`code` 是机器判别的唯一手段——前端分支只认 `code`，不得解析 `error` 文本；未登记 `code` 的普通错误体按通用失败处理。当前登记 `code` 的错误体仅登录失败（下条，清单 §2.1）；将来需要机器判别时按本通用形状补 `code`。**已知的历史遗留例外**（不适用本形状、也不得再新增同类）：认证 401 的 `unauthorized`/`invalid_token`、以及若干端点把 `admin_only`/`no_user`/`update_failed` 之类的短标识放进 `error`——这些值是**既有实现**，清理需单独一票；新代码不得以它们为先例。
- 登录失败错误体（`POST /api/login` 专用）：`{"error": <human message>, "code": <snake_case_code>}`，HTTP 状态码表达类别（401 凭据/状态/封禁类，423 锁定）。请求体必须为 JSON 对象且 `username`/`password` 均为 string；请求体缺失/非法 JSON/非对象/字段类型错/缺用户名或密码时回 400 `{"error":"bad request"}`（**无 `code`**，前端按通用失败处理）。`account_locked` 额外携带 `locked_until`（**绝对 Unix 秒级时间戳**，与 `GET /api/user*` 的 `locked_until` 同语义；前端按 `locked_until - now` 计算剩余分钟，不得按剩余秒数解释；`locked_until <= now` 表示无剩余时间信息，前端只显示锁定文案、不显示分钟）。前端把 `code` 映射中文文案（单一真相源 `composables/loginErrors.ts`），**绝不把 `code` 或英文 `error` 回显给用户**；未知/缺失 `code` 统一回落「登录失败」。
- 权限：管理类端点需 admin 角色（服务端校验，非前端隐藏）。
- 数据变更联动：变更 DB 的端点成功后由 dzweb 广播 `data_changed{scope}`（契约 webui-ws），前端按 scope 重新 REST 拉取。
- 设置类请求（转发 SHM 帧的端点）：响应只表示"已写事件通道"；生效以 WS 领域消息为准（对应帧契约的 RTN）。超时无 RTN 时按总则 §7 兜底。

---

## 2. 端点清单

### 2.1 鉴权与进程自述

| 方法/路径 | 语义 |
|---|---|
| POST `/api/login` | 登录，返回 JWT；失败计数与锁定策略见实现。失败错误体形状见 §1；机器码 `code`：`invalid_credentials`（用户名或密码错误）/ `account_disabled`（账户已禁用）/ `ip_banned`（IP 被封禁）→ 401；`account_locked`（账户锁定）→ 423 + `locked_until`（绝对 Unix 秒级时间戳）。`error` 为对应英文消息 |
| POST `/api/auth/change-password` | 修改本人密码 |
| GET `/api/system/info` | 免认证。dzweb 进程自述：`{"process_name": <exe 文件名不含扩展名>, "version": <SemVer 版本串>}`。`process_name` 供前端比对 logger 名以禁用自我日志 tail（支持重命名 `dzweb.exe` 部署）；`version` 为工程版本（根 `CMakeLists.txt` 的 `project(VERSION)`，经 `version.h` 的 `DZ_VERSION_STRING` 生成，含 pre-release 后缀），供「关于」页显示，前端不得另存硬编码版本。仅新增只读字段，无帧往返 |

### 2.2 用户与安全

| 方法/路径 | 语义 |
|---|---|
| GET/POST `/api/user`，GET/PUT/DELETE `/api/user/{id}` | 用户 CRUD（admin） |
| GET `/api/user/me` | 当前登录用户自身信息（任意已认证用户）；前端在 WS 连接/重连成功后借此刷新缓存角色，使 `isAdmin` 与服务端实时一致（角色降级时后端踢其 WS 连接，重连即刷新） |
| PUT `/api/user/{id}/status` | 启用/禁用用户；禁用时强制断开其 WS 连接 |
| PUT `/api/user/{id}/password` | 重置密码（admin） |
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

### 2.8 资源监控

| 方法/路径 | 语义 | 帧/镜像联动 |
|---|---|---|
| GET `/api/sysmon/snapshot` | 当前系统监控快照（任意已认证用户）；镜像尚未就绪（dzsysmon 未上报）时回 `503 {"error":"sysmon snapshot not available"}` | 读取镜像 `sysmon_snapshot` 域（固定实例 `dzsysmon`，契约 sysmon §5）；由 `RTN_SYSMON_SNAPSHOT` 更新 |
| GET `/api/sysmon/config` | 当前生效的系统监控配置（任意已认证用户）；镜像尚未就绪时回 `503 {"error":"sysmon config not available"}`。只读镜像，不产生帧往返 | 读取镜像 `sysmon_config` 域（固定实例 `dzsysmon`，契约 sysmon §5）；由 `RTN_SYSMON_CONFIG` 更新 |
| PUT `/api/sysmon/config` | 运行期修改系统监控配置（admin）：body 为 RFC 7386 merge patch（任意位置 `null` 由 dzsysmon 判为非法）。响应仅表示"已写事件通道"（`{"ok":true,"message":"dispatched"}`）；`shm` 未就绪时 `503 {"error":"shm not available"}` | → `SET_SYSMON_CONFIG`（`instance_id=dzsysmon`，契约 sysmon §3）；生效信号：WS `sysmon_config`（`RTN_SYSMON_CONFIG` 全量回推，含失败时回推旧值）。监控页「配置」区下发差异 patch，pending 以 WS 回推清除 |
| GET `/api/sysmon/history` | 历史曲线查询（任意已认证用户）：按指标、时间范围与粒度返回 min / max / avg / last 包络及历史标记。历史库缺失、schema 版本不符或文件损坏时回 `200` + `{"available":false,"reason":...}`（**不**返回伪造或半截数据，也**不**当作 5xx）；参数非法回 `400 {"error":"bad request"}` | 只读打开历史库 `$DZTRADER_HOME/db/sysmon.db`（《帧契约：系统监控》§6.1）；**不走帧、不读镜像** |

---

## 3. 真相源声明

- **行情源列表**：DB（repository）为真相源。行情进程上报 `RTN_MD_CONFIG` 时，dzweb 对未入库的 `dzmd_*` 来源自动对账补录（幂等）。DB 与 master 进程配置（契约 process）的同步通过帧联动实现：添加行情源 + Start 由 master 动态注册（契约 process）。列表条目的生命周期由 DB `is_added` 标记承载（remove 下发成功置 0、再次添加复位 1）。
- **进程存在性与运行状态**：master 进程配置为真相源（契约 process）；REST 列表是投影。
- **配置与状态数据**：进程镜像（MirrorStore）为投影，真相源为对应帧契约的 RTN。
- **用户/安全**：DB 为真相源，变更经 `data_changed` 广播。

---

## 4. 前端义务

- 设置类 REST 请求成功后进入 pending，以对应 WS 领域消息清除（契约 webui-ws §5）；REST 响应成功≠已生效。
- `data_changed{scope}` 到达后按 scope 重新 REST 拉取对应列表；不得用本地缓存回填。
- 行情源列表以 DB 为准（REST 拉取），进程卡片以 `process_config`/`process_status` 为准（WS 推送）。
