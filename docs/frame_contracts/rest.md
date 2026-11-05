# REST API 契约：frontend ↔ dzweb

本文件规定 WebUI 前端与 dzweb 后端之间的 REST 端点约定（路径、方法、与 SHM 帧/镜像的联动、生效信号）。总则见《帧契约：通用规则》。

REST 是**请求入口与大数据查询**；状态推送一律走 WS（契约 webui-ws）。REST 端点是总则 §4.2 形态 1/2/3 的前端入口。设置类请求的生效信号是 WS 领域消息（RTN 推送），REST 响应只表示"请求已被 dzweb 处理"。

交易数据读端点（§2.9）直查交易库，不读镜像、不走帧：dzweb 不保留委托/成交/持仓/资金记录镜像，库是唯一真相源（ADR 0043）。

> **实现注记**：`TdDataService` 记录镜像已随 #246 删除；委托列表端点（`GET /api/td/orders`）与 WS 委托事件 / 建连补发已随 #246 落地；统计端点（`GET /api/td/stats`，§2.9.5）与 dzweb 统计计数器已随 #247 落地（`apps/webui/td_stats{,_query}.{h,cpp}`，重算查询与委托列表共用只读连接抽取 `td_read_db.{h,cpp}`）；成交列表端点（`GET /api/td/trades`）与 WS 成交事件已随 #248 落地（`apps/webui/td_trades_query.{h,cpp}`，列表参数 / 游标抽为 `apps/webui/td_list_query.{h,cpp}`、行形状与派生字段抽为 `apps/webui/td_row_json.{h,cpp}`，委托与成交共用同一份）。持仓列表端点（`GET /api/td/positions`，§2.9.4）、WS 持仓事件 `td_position_info` 与 `td_recent` 持仓事件已随 #249 落地（`apps/webui/td_positions_query.{h,cpp}`；账户全集与锚点日来源抽为 `apps/webui/td_account_scope.{h,cpp}`）。资金端点（`GET /api/td/trading_accounts`，§2.9.4）与 WS 资金事件 `td_trading_account`（含 `td_recent` 第四类事件）已随 #250 落地（`apps/webui/td_trading_accounts_query.{h,cpp}`，与持仓共用 `td_account_scope` / `td_row_json` / `td_read_db`；资金端点不分页，`limit`/`cursor` 被忽略）。账户配置写端点（§2.9.7）由 #251 落地，当前未实现的端点在契约里同样有效（实现票随后补齐）。

---

## 1. 通用约定

- 认证：除 `POST /api/login`、`GET /health`、`GET /api/system/info` 外，全部 `/api/*` 需要 JWT（`Authorization: Bearer <jwt>`）；失败回 401 `{"error":"unauthorized"}`（token 缺失）或 `{"error":"invalid_token"}`（校验失败）。这两个值是**历史遗留的固定标识**（前端不据此分支——非登录接口的 401 一律按会话失效处理），属下条列出的已知例外，不适用 `code` 规则。
- 响应：JSON（`application/json`）。
- 错误：`{"error": message}` + 对应 HTTP 状态码；404 为 `{"error":"not found"}`。`error` 承载**人类可读消息**（小写英文开头、无句号），前端可直接兜底展示——**登录失败除外**（下条：其文案由 `code` 映射中文，不得展示 `error`）。
- 需要**机器可判别**的失败额外携带 `code`（snake_case 机器码）：`{"error": <human message>, "code": <snake_case_code>}`。`code` 是机器判别的唯一手段——前端分支只认 `code`，不得解析 `error` 文本；未登记 `code` 的普通错误体按通用失败处理。**当前登记的 `code` 清单**：登录失败（`invalid_credentials` / `account_disabled` / `ip_banned` / `account_locked`，见下条与 §2.1）、口令强度（`password_too_weak`，400）、自助改密旧密码错误（`invalid_old_password`，400）、强制改密门禁（`password_change_required`，403）——后三者见 §1「密码策略」「自助改密 vs 管理员重置」「强制改密」；交易账户配置写（§2.9.7）新增 admin_required / invalid_gateway / gateway_not_running / account_exists / account_not_found（状态码见该节）。将来需要机器判别时按本通用形状补 `code`。**已知的历史遗留例外**（不适用本形状、也不得再新增同类）：认证 401 的 `unauthorized`/`invalid_token`、以及若干端点把 `admin_only`/`no_user`/`update_failed` 之类的短标识放进 `error`——这些值是**既有实现**，清理需单独一票；新代码不得以它们为先例。
- 登录失败错误体（`POST /api/login` 专用）：`{"error": <human message>, "code": <snake_case_code>}`，HTTP 状态码表达类别（401 凭据/状态/封禁类，423 锁定）。请求体必须为 JSON 对象且 `username`/`password` 均为 string；请求体缺失/非法 JSON/非对象/字段类型错/缺用户名或密码时回 400 `{"error":"bad request"}`（**无 `code`**，前端按通用失败处理）。`account_locked` 额外携带 `locked_until`（**绝对 Unix 秒级时间戳**，与 `GET /api/user*` 的 `locked_until` 同语义；前端按 `locked_until - now` 计算剩余分钟，不得按剩余秒数解释；`locked_until <= now` 表示无剩余时间信息，前端只显示锁定文案、不显示分钟）。前端把 `code` 映射中文文案（单一真相源 `composables/loginErrors.ts`），**绝不把 `code` 或英文 `error` 回显给用户**；未知/缺失 `code` 统一回落「登录失败」。
- 权限：管理类端点需 admin 角色（服务端校验，非前端隐藏）。
- **交易数据端点通用规则**（§2.9）：
  - 游标分页：默认 200 行/页、上限 1000 行/页，**禁止 OFFSET 深翻页**；分页游标为不透明字符串，与查询作用域（端点 + 账户过滤 + 当前交易日）绑定，作用域变化必须从首页重新开始（§2.9.2）。
  - 降级：只读端点读交易库失败时统一回 200 `{"available":false,"reason":<code>}`（§2.9.8），不返回 5xx、不阻塞 IO 线程。
  - 权限：读端点全部已认证用户可读全部账户（账户级过滤后续，ADR 0045）；账户配置写端点仅 admin（403 `code=admin_required`，§2.9.7）。
- **密码策略**：凡设置口令的端点（`POST /api/user`、`PUT /api/user/{id}/password`、`POST /api/auth/change-password`）一律由**服务端**强制最小长度 12（唯一常量 `kMinPasswordLength`，`apps/webui/password_policy.h`）；不合格回 400 `{"error":"password too weak","code":"password_too_weak"}`。前端只做提示与按钮禁用，**不是安全边界**。口令值永不出现在响应体、日志与任何用户可见文案中。
- **自助改密 vs 管理员重置**：前者（`POST /api/auth/change-password`）必须提交 `old_password` 并校验，失败回 400 `{"error":"invalid old password","code":"invalid_old_password"}`；后者（`PUT /api/user/{id}/password`）**不需要**旧密码（admin 已鉴权，属正常设计），但同样受强度策略约束。管理员重置成功后置 `must_change_password=1`；管理员创建用户不置位（口令由管理员当场设定）。
- **会话吊销**：JWT 载荷携带 `ver`（签发时刻用户的 `token_version`）。改密 / 管理员重置 / 禁用账户时 `token_version` 自增，该用户**既有**的 JWT 与 WS 连接立即失效（REST 回 401 `{"error":"invalid_token"}`，WS 回 `auth failed` 并断开）。**自助改密后当前会话同样失效**——前端必须让用户用新口令重新登录，不得继续使用旧 token。
- **强制改密**：`must_change_password=1` 的账户登录后只能调用 `POST /api/auth/change-password` 与 `GET /api/user/me`，其余 `/api/*` 一律 403 `{"error":"password change required","code":"password_change_required"}`；WS 连接以 `auth failed` 拒绝。自助改密成功后清除该标志。
- 数据变更联动：变更 DB 的端点成功后由 dzweb 广播 `data_changed{scope}`（契约 webui-ws），前端按 scope 重新 REST 拉取。
- 设置类请求（转发 SHM 帧的端点）：响应只表示"已写事件通道"；生效以 WS 领域消息为准（对应帧契约的 RTN）。超时无 RTN 时按总则 §7 兜底。

---

## 2. 端点清单

### 2.1 鉴权与进程自述

| 方法/路径 | 语义 |
|---|---|
| POST `/api/login` | 登录，返回 JWT；失败计数与锁定策略见实现。失败错误体形状见 §1；机器码 `code`：`invalid_credentials`（用户名或密码错误）/ `account_disabled`（账户已禁用）/ `ip_banned`（IP 被封禁）→ 401；`account_locked`（账户锁定）→ 423 + `locked_until`（绝对 Unix 秒级时间戳）。`error` 为对应英文消息。成功响应另带 `default_password_warning`（`{"message": <中文告警文案>}` 或 `null`；仅 admin 仍在用系统默认口令且未确认告警时非 null，**文案由后端给出，前端不得自行硬编码**）与 **`must_change_password`**（本账户被管理员重置过口令，须先改密，见 §1「强制改密」） |
| POST `/api/auth/change-password` | 修改本人密码。请求体 `{ old_password, new_password }`：`old_password` 必填并校验（错误 → 400 `code=invalid_old_password`），`new_password` 受最小长度 12 约束（→ 400 `code=password_too_weak`）。成功后本账户 `token_version` 自增（既有 JWT/WS 全部失效，含当前会话）并清除 `must_change_password`；响应 `{"ok":true}` 后前端须引导重新登录 |
| GET `/api/system/info` | 免认证。dzweb 进程自述：`{"process_name": <exe 文件名不含扩展名>, "version": <SemVer 版本串>}`。`process_name` 供前端比对 logger 名以禁用自我日志 tail（支持重命名 `dzweb.exe` 部署）；`version` 为工程版本（根 `CMakeLists.txt` 的 `project(VERSION)`，经 `version.h` 的 `DZ_VERSION_STRING` 生成，含 pre-release 后缀），供「关于」页显示，前端不得另存硬编码版本。仅新增只读字段，无帧往返 |

### 2.2 用户与安全

| 方法/路径 | 语义 |
|---|---|
| GET/POST `/api/user`，GET/PUT/DELETE `/api/user/{id}` | 用户 CRUD（admin）。POST 的 `password` 必填且受最小长度 12 约束（→ 400 `code=password_too_weak`） |
| GET `/api/user/me` | 当前登录用户自身信息（任意已认证用户）；前端在 WS 连接/重连成功后借此刷新缓存角色，使 `isAdmin` 与服务端实时一致（角色降级时后端踢其 WS 连接，重连即刷新） |
| PUT `/api/user/{id}/status` | 启用/禁用用户；禁用时强制断开其 WS 连接并自增 `token_version`（既有 JWT 立即失效） |
| PUT `/api/user/{id}/password` | 重置密码（admin）。不需要旧密码；`new_password` 受最小长度 12 约束（→ 400 `code=password_too_weak`）。成功后自增该用户 `token_version`（既有 JWT 与 WS 全失效）并置 `must_change_password=1`（下次登录强制改密） |
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

### 2.9 交易数据

交易数据端点直查交易库（`$DZTRADER_HOME/db/trade.db`，ADR 0041/0043）：每请求独立只读连接、`busy_timeout` 200ms，游标分页、禁止 OFFSET 深翻页；dzweb 不保留委托/成交/持仓/资金记录镜像。端点分三类：只读直查（委托/成交/持仓/资金）、内存计数器（统计）、账户配置写（转发 SHM 帧）；不读任何记录镜像。

| 方法/路径 | 语义 | 帧/镜像联动 |
|---|---|---|
| GET `/api/td/orders` | 当日委托列表（在线账户 + 当前交易日，游标分页） | 直查交易库（§3） |
| GET `/api/td/trades` | 当日成交列表（在线账户 + 当前交易日，游标分页） | 直查交易库 |
| GET `/api/td/positions` | 持仓列表（全部已登记账户，各账户锚点日，游标分页） | 直查交易库 |
| GET `/api/td/trading_accounts` | 资金列表（全部已登记账户，各账户锚点日，不分页） | 直查交易库 |
| GET `/api/td/stats` | 当日「未成交/总」统计（在线账户 + 当前交易日） | dzweb 统计计数器（§2.9.5） |
| POST `/api/td/accounts` | 添加交易账户配置（admin） | → `TD_REQ_MODIFY_CONFIG{AddAccount}`（契约 td-config §2.1/§3）；生效信号 = `RTN_TD_CONFIG` |
| PUT `/api/td/accounts/{account_id}` | 修改交易账户配置（admin，字段级 merge） | → `TD_REQ_MODIFY_CONFIG{UpdateAccount}` |
| DELETE `/api/td/accounts/{account_id}` | 删除交易账户配置（admin；不删除交易数据） | → `TD_REQ_MODIFY_CONFIG{RemoveAccount}` |

> WS 交易事件（契约 webui-ws §2.5）只承载增量，记录基线一律由本节端点拉取。

#### 2.9.1 口径与账户范围

- **真相源**：交易库为唯一真相源（master 建库/重建，dzweb 只读直查）。读端点不读镜像、不走帧；`gateway`/`ext` 等平台内部列不返回（ADR 0045）。
- **当前交易日**（信封 trading_day）：在线（Ready）账户 `date` 的最大值（会话交易日，不是自然日）；无在线账户时为 null。日切窗口内各账户可能短暂处于不同交易日，以最大值为准（列表只含已切到该日的账户，不混入上一交易日数据）。
- **在线账户集合**（信封 online_accounts）：`ACCOUNT_STATUS` 为 Ready 的账户；Ready/Offline 翻转与日切感知由 dzweb 维护（《帧契约：账户登录状态》）。
- **委托 / 成交 / 统计**：只覆盖在线账户 + 当前交易日（记录 `date` = 当前交易日）。`accounts` 过滤与在线集合求交，缺省 = 全部在线账户。
- **持仓 / 资金**：覆盖全部已登记账户（在线 ∪ td 配置已知账户/账户注册表 ∪ 交易库四表中出现过的 account_id，含离线；后两者防注册表丢失时漏账户），按**账户锚点日**取该账户最新行；记录 `date` = 该账户锚点日。`accounts` 过滤作用于已登记账户全集，缺省 = 全部已登记账户；未知账户忽略。**账户注册表部分随 #251 落地**——在此之前 dzweb 的账户全集实现为「在线 ∪ 四表中出现过的 account_id」两者（契约此处保留三源并集的定义，实现按票递增）。
  - **锚点日 = 该账户 `positions` 与 `trading_accounts` 两张快照表的 `MAX(date)`**（不含 `orders` / `trades` 追加流表；`null` = 该账户无任何快照行）。这两张表只在**完整成功的柜台查询**上写入（ADR 0045 决策 2/4，其中资金行的写入受 `position_query_ok_` 守卫），因此锚点日恒等于"最近一次成功查询所属交易日"。
  - **为什么不是四表 `MAX(date)`**：委托 / 成交是追加流，登录重放与实时回报会先于持仓 / 资金查询落库。若锚点含追加流表，登录当天只要有一笔委托/成交，锚点就推进到当日——而持仓查询失败时 `positions` 当日无行，列表将返回**空**。这与 ADR 0045 决策 3「登录不清零、不预置 0、首个成功查询前保留上次已知快照」直接冲突：读者看到的"空仓"是假断言，比带日期的旧快照更危险。改用两张快照表后，查询失败期锚点仍停在上一成功日，列表照常返回旧快照（前端用行内 `date` 标注数据日期）。
  - 当日全平后不显示昨日幽灵持仓仍然成立：平仓走整组原子替换（当日行删除），且资金行只在持仓查询成功后写入、二者同属快照表，锚点推进到当日 + 当日持仓行为空 = 真空仓。
- **列表不接受交易日参数**：委托/成交/统计的当前交易日由 dzweb 解析（见上），客户端不得按自然日或历史交易日查询（跨会话历史回看后续另定）。
- **权限**：全部已认证用户可读全部账户（账户级过滤后续，ADR 0045）；账户配置写端点仅 admin（§2.9.7）。
- **空语义**：items 空数组 = 无数据（不是错误）；资金无记录行 → has_data:false 且资金字段全为 null；持仓 volume=0 的行不返回（绝对态清零帧的库内残留，不是持仓事实）。
- **新鲜度**：交易库是已提交数据（异步持久化，ADR 0042）。WS 交易事件可能先于库提交到达，同一记录按 `seq` 合并（契约 webui-ws §5）；REST 稍后可见属预期，不是丢数据。
- **交易日表示与命名**：帧/DB 内部交易日为 `DzDate`（距纪元天数）或 DB TEXT，REST/WS JSON 一律输出 `"YYYYMMDD"` 字符串（ADR 0041 决策 4 表示层约定）。记录行键名统一为 `date`（记录自身交易日）；信封键名 trading_day 表达**列表作用域的当前交易日**，是 ADR 0041 决策 4「REST/WS 键统一 date」的**登记例外**（非记录字段；改名需新 ADR/契约修订）。委托/成交/统计记录的 `date` 恒等于 trading_day，持仓/资金行 `date` 取该账户锚点日。

#### 2.9.2 响应信封与游标分页

读列表端点的可用响应（不含统计，统计见 §2.9.5）：

```json
{
  "available": true,
  "trading_day": "20260808",
  "online_accounts": ["account_001", "account_002"],
  "items": [],
  "next_cursor": null
}
```

| 字段 | 类型 | 说明 |
|---|---|---|
| available | bool | true = 交易库可读；false = 降级（此时响应只有 available 与 reason，§2.9.8） |
| trading_day | string\|null | 当前交易日 "YYYYMMDD"（§2.9.1）；无在线账户时 null |
| online_accounts | array\<string\> | 在线（Ready）账户集合；顺序不承诺 |
| items | array\<object\> | 行对象（§2.9.3 / §2.9.4）；空数组 = 无数据 |
| next_cursor | string\|null | 下一页游标；null = 没有更多 |

请求参数（列表端点）：

| 参数 | 类型 | 说明 |
|---|---|---|
| limit | int | 可选；默认 200，允许 1–1000；越界或非整数 → 400 `{"error":"bad request"}` |
| cursor | string | 可选；上一页返回的 next_cursor。与查询作用域（端点 + accounts + 该端点的日期口径）绑定；作用域变化必须从首页开始；资金端点忽略本参数；非法或作用域不符 → 400 bad request |
| accounts | string | 可选；逗号分隔账户过滤。空串等价缺省；每项去首尾空白、空项跳过、重复忽略；未知/越界账户忽略（不报错） |

- 查询作用域 = 端点 + accounts + 该端点的日期口径（委托/成交为当前交易日；持仓/资金为各账户锚点日集合）。作用域变化必须丢弃 cursor 从首页重拉。
- 分页**禁止 OFFSET 深翻页**；各端点的游标坐标固定如下：
  - 委托 / 成交：坐标 = 与 sort_key 同源的 (排序时间, 业务唯一键) 降序；下一页从该坐标**之后**（排序更旧）继续。
  - 持仓：坐标 = (account_id, instrument_id, direction) 升序元组，direction 全序 Short(-1) < Net(0) < Long(1)；下一页从该元组之后继续。
  - 资金（trading_accounts）：不适用 cursor——不分页，返回全部已登记账户（next_cursor 恒 null，limit/cursor 被忽略），按 account_id 升序。
- 同一记录在翻页过程中不得重复或遗漏；翻页期间新写入的委托/成交只会出现在首页之前，不影响已取页面。

#### 2.9.3 委托行与成交行

**枚举取值**（字符串，值 = 帧枚举的 PascalCase 名；解析方按精确集合匹配）：

| 字段 | 取值 |
|---|---|
| direction | Long / Short / Net |
| position_effect | Auto / Open / Close / CloseToday / CloseYesterday |
| price_type | Limit / Market / Stop / Fak / Fok / Rfq |
| status | Submitting / NotTraded / PartTraded / AllTraded / Cancelled / Rejected |

**委托行**：

```json
{
  "account_id": "account_001",
  "date": "20260808",
  "order_id": 10231,
  "strategy_id": "stg_demo",
  "instrument_id": "rb2601",
  "exchange_id": "SHFE",
  "direction": "Long",
  "position_effect": "Open",
  "price_type": "Limit",
  "status": "PartTraded",
  "price": 3512.5,
  "volume": 2,
  "volume_traded": 1,
  "remark": "",
  "time": 32400,
  "avg_price": 3512.3,
  "price_decimals": 1,
  "avg_price_decimals": 3,
  "seq": 1842,
  "sort_key": "opaque-sort-key"
}
```

| 字段 | 类型 | 说明 |
|---|---|---|
| account_id | string | 账户标识 |
| date | string | 交易日 "YYYYMMDD"（= 信封 trading_day） |
| order_id | int | 平台委托单 ID（全局唯一） |
| strategy_id | string | 策略 ID；非策略单为保留值 /external、/manual、/sync（契约 td-order） |
| instrument_id | string | 合约代码 |
| exchange_id | string | 交易所 ID |
| direction | string | 买卖方向（上表） |
| position_effect | string | 开平仓（上表） |
| price_type | string | 价格类型（上表） |
| status | string | 委托状态（上表） |
| price | number | 委托价（double 全精度；市价单为 0） |
| volume | int | 委托数量 |
| volume_traded | int | 已成交数量 |
| remark | string | 委托备注；无则空串 |
| time | int | 报单时间：距午夜秒数 0–86399（真实时钟，不含交易日偏移；前端格式化为 HH:MM:SS，不做时区转换；排序坐标见 §2.9.6） |
| avg_price | number\|null | 成交均价（Σ价×量/Σ量，double 全精度）；无成交（volume_traded=0）时为 null |
| price_decimals | int | 委托价显示小数位（§2.9.6） |
| avg_price_decimals | int | 成交均价显示小数位（§2.9.6） |
| seq | int | 不透明版本号（§2.9.6）：合并用，不显示、不参与业务 |
| sort_key | string | 排序键（§2.9.6）：字符串降序 = 列表顺序 |

**成交行**：

```json
{
  "account_id": "account_001",
  "date": "20260808",
  "trade_id": "00000000001",
  "order_id": 10231,
  "strategy_id": "stg_demo",
  "instrument_id": "rb2601",
  "exchange_id": "SHFE",
  "direction": "Long",
  "position_effect": "Open",
  "price": 3512.3,
  "volume": 1,
  "time": 32405,
  "price_decimals": 1,
  "seq": 1843,
  "sort_key": "opaque-sort-key"
}
```

| 字段 | 类型 | 说明 |
|---|---|---|
| account_id | string | 账户标识 |
| date | string | 交易日 "YYYYMMDD" |
| trade_id | string | 交易所成交 ID（仅交易所内唯一） |
| order_id | int | 关联委托单 ID |
| strategy_id | string | 策略 ID；保留值语义同委托行 |
| instrument_id | string | 合约代码 |
| exchange_id | string | 交易所 ID（与 trade_id 组合唯一） |
| direction | string | 买卖方向 |
| position_effect | string | 开平仓 |
| price | number | 成交价（double 全精度） |
| volume | int | 成交量 |
| time | int | 成交时间：距午夜秒数 0–86399（同上格式化为 HH:MM:SS） |
| price_decimals | int | 成交价显示小数位（§2.9.6） |
| seq | int | 不透明版本号（§2.9.6） |
| sort_key | string | 排序键（§2.9.6） |

#### 2.9.4 持仓行与资金行

**持仓行**（positions；volume=0 的行不返回）：

```json
{
  "account_id": "account_001",
  "date": "20260808",
  "instrument_id": "rb2601",
  "exchange_id": "SHFE",
  "direction": "Long",
  "volume": 2,
  "frozen_volume": 0,
  "today_volume": 1,
  "yd_volume": 1,
  "price": 3512.35,
  "price_decimals": 1,
  "seq": 1844
}
```

| 字段 | 类型 | 说明 |
|---|---|---|
| account_id | string | 账户标识 |
| date | string | 该账户锚点日 "YYYYMMDD"（不是全局当前交易日） |
| instrument_id | string | 合约代码 |
| exchange_id | string | 交易所 ID |
| direction | string | 持仓方向（Long / Short / Net） |
| volume | int | 总持仓（含今仓与昨仓） |
| frozen_volume | int | 冻结量 |
| today_volume | int | 今仓 |
| yd_volume | int | 昨仓 |
| price | number | 持仓均价（double 全精度） |
| price_decimals | int | 持仓均价显示小数位（§2.9.6） |
| seq | int | 不透明版本号（§2.9.6） |

**资金行**（trading_accounts）：每个已登记账户一行，含离线与无数据账户。

```json
{
  "account_id": "account_001",
  "date": "20260808",
  "has_data": true,
  "balance": 100000.0,
  "available": 85000.0,
  "frozen": 0.0,
  "commission": 12.5,
  "margin": 15000.0,
  "withdraw_quota": 80000.0,
  "deposit": 0.0,
  "withdraw": 0.0,
  "seq": 1850
}
```

| 字段 | 类型 | 说明 |
|---|---|---|
| account_id | string | 账户标识 |
| date | string\|null | 该账户锚点日 "YYYYMMDD"；无资金记录时 null |
| has_data | bool | false = 该账户无资金记录（离线且从未查询成功等）；此时余额类字段全为 null |
| balance | number\|null | 权益 |
| available | number\|null | 可用资金 |
| frozen | number\|null | 冻结资金 |
| commission | number\|null | 手续费 |
| margin | number\|null | 保证金占用 |
| withdraw_quota | number\|null | 可取资金 |
| deposit | number\|null | 入金金额 |
| withdraw | number\|null | 出金金额 |
| seq | int\|null | 不透明版本号（§2.9.6）；无记录时 null |

> 无记录账户仍返回一行（has_data:false、date/seq 与全部资金字段为 null），使前端能按账户全集展示「无数据」而不是缺失该账户。

#### 2.9.5 统计

`GET /api/td/stats` 读取 dzweb 的统计计数器（不直查库、不接受 `accounts` 过滤）：

```json
{
  "available": true,
  "trading_day": "20260808",
  "online_accounts": ["account_001"],
  "stats": {
    "total_orders": 1240,
    "total_volume": 2480,
    "unfilled_orders": 12,
    "unfilled_volume": 21
  }
}
```

| 字段 | 类型 | 说明 |
|---|---|---|
| total_orders | int | 当前交易日在线账户的全部委托笔数（**含撤单、废单**，与未过滤的委托列表总行数一致） |
| total_volume | int | 上述委托的 volume 之和 |
| unfilled_orders | int | status ∈ {Submitting, NotTraded, PartTraded} 的委托笔数 |
| unfilled_volume | int | 上述委托的 (volume − volume_traded) 之和 |

- 口径固定为**全部在线账户 + 当前交易日**：stats 不接受 `accounts` 过滤（4 个计数器是全局口径）；账户级统计后续另定。stats 不是列表：无 items / next_cursor 字段。
- 计数器由 dzweb 维护：**增量来源** = 转发 `ORDER_REPORT` 交易事件（仅在线账户，按状态迁移增减 4 个值）；**重算触发** = 启动 / 账户 Ready / 在线集合变化（账户 Offline 下线）/ 日切从交易库重算，并周期对账（建议 30s，实现可调）。REST 只读；无 WS 推送，前端按需轮询（契约 webui-ws §5）。
- total_orders 与未过滤委托列表口径一致（同一交易日、同一在线账户集合、含撤单/废单）；异步持久化窗口内计数器可能短暂领先已提交数据，稳态（对账后）一致。
- **已知精度边界（成交均价刷新帧）**：已终态（全成交 / 已撤 / 废单）的委托仍可能收到 `ORDER_REPORT`——每笔成交使该单成交均价变化时，td 会以新 `seq` 刷新委托行并推帧（《帧契约：TD 数据同步》§3）。计数器按「不在活动委托表 = 新委托」处理，此时会**短暂多计 1 笔 +volume**，≤1 个对账周期（默认 30s）后自动纠正。这是增量口径与全量重算口径的必然差，不构成计数器缺陷；不得据此把终态委托重新计入未成交。
- available：true = 计数器已就绪；false = 尚未完成首次成功重算（或重算持续失败），reason=not_ready。计数器就绪后即使交易库暂时不可读也不降级（内存计数器仍有效），与 §2.9.8 的库失败降级区分。
- trading_day / online_accounts 语义同列表信封（§2.9.2）。

#### 2.9.6 精度、排序键与 seq

- 所有价格字段统一带显示精度（ADR 0043 决策 2 的精度语义适用于全部价格展示；委托行另有成交均价精度）。
- **价格小数位（price_decimals）**：由合约 `instruments.price_tick` 推导——取最小整数 d ∈ [0, 8]，使 `price_tick × 10^d` 与最近整数之差 ≤ 1e-9（浮点安全，不做字符串猜测）。查不到合约或 `price_tick ≤ 0` 时，用**该行价格**（委托价 / 成交价 / 持仓均价）的最短十进制表示的小数位数兜底；仍无法确定时取 0。交易库不可用（WS 转发事件无法查合约）时同样走兜底。
  - 例：`price_tick` = 0.5 → 1；0.01 → 2；1 → 0；0.00001 → 5。成交均价小数位例：0.5 → 3；0.01 → 4；1 → 2；0.00001 → 6（封顶）。
- **成交均价小数位（avg_price_decimals，仅委托行）**：`clamp(price_decimals + 2, 2, 6)`。dzweb **不做字符串舍入**，只回原始数值与精度整数；前端用 `toFixed(price_decimals / avg_price_decimals)` 格式化（契约 webui-ws §5）。
- **sort_key（仅委托 / 成交行）**：不透明字符串，满足三条性质：
  1. 同一查询作用域内（同一端点 + 同一 accounts + 同一交易日），items 按 sort_key **字符串降序**排列（首项最大），即列表顺序；
  2. 同一记录在 REST 与 WS 事件中 sort_key 相同（前端据此把增量事件插入正确位置）；
  3. 前端只可做字符串比较，不得解析其结构、不得跨作用域比较。

  排序坐标 = `((time + 21600) % 86400)` **降序**（18:00 锚点：夜盘归入所属交易日且与日盘排序一致），同一秒按业务唯一键**降序**（委托：order_id 按数值降序；成交：依次按 account_id、exchange_id、trade_id 的 UTF-8 字节序降序）。编码格式由实现决定（契约只约束比较语义）。
- **seq**：不透明版本号（账户级、四类共享、跨日累积，契约 td-data-sync §1）。同一业务键 seq 大者胜；业务键：委托 order_id；成交 (account_id, date, exchange_id, trade_id)；持仓 (account_id, instrument_id, direction)；资金 account_id。seq 不显示、不参与业务判断，排序一律用 sort_key / 服务端顺序。

#### 2.9.7 账户配置写

写端点把配置操作转发到目标交易网关（`TD_REQ_MODIFY_CONFIG`，契约 td-config §3）；dzweb 是账户唯一性的**权威校验点**（契约 td-config §5）。

**请求体**：

- POST：`{"gateway": "dztd_ctp", "account": { ... }}`——`gateway` 为目标交易网关进程名；`account` 为契约 td-config §2.2 的账户配置（`account_id` 必填，`enabled`/`risk_control_enabled`/`currency_id`/`ext` 按该节默认与语义；`ext` 由目标网关按接口类型校验）。
- PUT：body 为契约 td-config §2.1 `UpdateAccount` 的 params 子集（字段级 merge，缺失字段保留旧值）；**不得包含 account_id**（出现即 400 bad request，路径是唯一来源）。允许空对象 `{}`。dzweb 转发前以路径值**注入** `params.account_id` 后写帧（`{}` → `{"op":"UpdateAccount","params":{"account_id":<路径>}}`）。
- DELETE：无 body（忽略请求体）；dzweb 写 `{"op":"RemoveAccount","params":{"account_id":<路径>}}`。

**预检**（失败时不写帧）：

1. admin 角色；否则 403 `{"error":"admin required","code":"admin_required"}`。
2. 目标网关（POST 来自 body 的 `gateway`；PUT/DELETE 来自注册表解析结果）必须是已注册的交易网关进程（进程配置中的 `dztd_*`）；否则 400 `{"error":"invalid gateway","code":"invalid_gateway"}`。
3. 目标网关当前必须为 Running（`process_status`，契约 process）；否则 409 `{"error":"td gateway not running","code":"gateway_not_running"}`。
4. POST：`account_id` 非空且 ≤ 31 字节（`DzAccountId` 容量）；否则 400 bad request。`account_id` 已在账户注册表中（**任何**网关，含已禁用/离线账户）→ 409 `{"error":"account already exists","code":"account_exists"}`。
5. PUT / DELETE：`account_id` 由账户注册表解析网关（`account_id → gateway`）；注册表无该账户 → 404 `{"error":"account not found","code":"account_not_found"}`。
6. 其余字段（含 `ext`）dzweb 不校验，透传给目标网关；业务校验失败由目标网关走四件套（契约 td-config §2、总则 §8）。

**响应**：200 `{"ok":true}`——仅表示已写入事件通道，**不代表业务已生效**（与 §1 设置类请求语义一致）。生效/失败以 `RTN_TD_CONFIG`（契约 td-config §4）为准：成功推新值，失败推回滚旧值并经 `NOTIFY_UI` 弹窗；前端 pending 以该回报清除（其 WS 映射随账户配置页实现接入，契约 webui-ws §7）。

**可观察效果**：配置回报到达后，dzweb 按网关整组 reconcile 账户注册表（契约 td-config §5）；账户集合增减时广播 `data_changed{scope:"td"}`（契约 webui-ws §2.3），持仓/资金列表随之重拉。

**约束**：

- 删除账户不删除交易数据（ADR 0045）；离线账户的持仓/资金仍按锚点日可查。
- 手工修改配置文件可绕过平台校验，属用户自担风险（契约 td-config §5）。
- 持久化由目标网关负责（契约 td-config §6）。

#### 2.9.8 降级

- 读端点读交易库失败时统一回 **200** `{"available":false,"reason":<code>}`：响应只有 available 与 reason 两个键，不含 items / next_cursor / trading_day / online_accounts。
- reason 取值：not_found（库文件不存在/尚未建库）、schema_mismatch（库版本不符）、open_failed（打开失败）、busy（200ms 内未取得读锁）、query_failed（查询执行失败）。
- 不使用 5xx、不阻塞 dzweb 单 IO 线程（ADR 0043）；前端按退避重试并展示统一降级提示（契约 webui-ws §5）。
- 写端点不经交易库读取，不因交易库降级而失败；stats 的降级条件（计数器未就绪，reason=not_ready）见 §2.9.5，不适用本节。

---

## 3. 真相源声明

- **行情源列表**：DB（repository）为真相源。行情进程上报 `RTN_MD_CONFIG` 时，dzweb 对未入库的 `dzmd_*` 来源自动对账补录（幂等）。DB 与 master 进程配置（契约 process）的同步通过帧联动实现：添加行情源 + Start 由 master 动态注册（契约 process）。列表条目的生命周期由 DB `is_added` 标记承载（remove 下发成功置 0、再次添加复位 1）。
- **进程存在性与运行状态**：master 进程配置为真相源（契约 process）；REST 列表是投影。
- **配置与状态数据**：进程镜像（MirrorStore）为投影，真相源为对应帧契约的 RTN。
- **用户/安全**：DB 为真相源，变更经 `data_changed` 广播。
- **交易数据**：交易库 `trade.db` 为唯一真相源，dzweb 只读直查（ADR 0043）；WS 交易事件是帧转发（不落记录镜像），不构成记录快照；§2.9 的 REST 读端点是记录基线的唯一查询通路。
- **交易账户配置**：目标 td 网关的配置文件为真相源（契约 td-config §6）；dzweb 的配置镜像与账户注册表是投影（契约 td-config §5）。

---

## 4. 前端义务

- 设置类 REST 请求成功后进入 pending，以对应 WS 领域消息清除（契约 webui-ws §5）；REST 响应成功≠已生效。
- `data_changed{scope}` 到达后按 scope 重新 REST 拉取对应列表；不得用本地缓存回填。
- 行情源列表以 DB 为准（REST 拉取），进程卡片以 `process_config`/`process_status` 为准（WS 推送）。
- 交易数据（委托/成交/持仓/资金）基线一律由 REST 拉取（§2.9）：WS 交易事件与 `snapshot` 都不能替代基线（`snapshot` 不含交易记录，WS 只推增量）。合并、有界窗口、批量刷新、日切清空、精度格式化等行为义务见契约 webui-ws §5。
- 账户筛选变化必须丢弃 cursor 从首页重拉；`available:false` 时展示统一降级提示（不得显示 0 冒充数据），按退避重试。
- 统计（§2.9.5）无 WS 推送：前端按需轮询，建议与列表批量刷新同节流（≤1Hz），日切或 `data_changed{scope:"td"}` 后必须重拉。
