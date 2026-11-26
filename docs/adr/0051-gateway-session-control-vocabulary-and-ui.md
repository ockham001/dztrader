# ADR 0051: 网关会话控制统一——登录/登出、双按钮与进度端点

## Status

Accepted（2026-11-22，#409）

> 决策 3 的「失败不是端点（md 登录失败态与已连接同为中间值）」与 Consequences 中「登录 pending 的失败清除依赖「`current` 回退」判据（进度骨架声明建立期单调、失败回退）」两句，已被 [ADR 0059](0059-login-failure-stops-at-failed-stage.md)（2026-11-26，登录链失败停在失败阶段）部分取代：链内失败停在失败阶段、`current` 不回退，回退收窄为「会话尝试就此结束」（主动中止、或登录链之前的前置连接超时）；失败原因改由状态帧 `last_error` 承载。本 ADR 其余决策（命名分层、双按钮、端点语义与相对判定、恢复两段式、td 控制链路镜像 md）全部保留——精确范围见 ADR 0059 的 Supersedes 段。

契约同步：《帧契约：进度推送》新增「网关会话进度骨架」；《帧契约：行情网关状态》前端义务③修订（失败态不再原地重试）；新增《帧契约：交易账户连接控制》；《帧契约：REST API》§2.9.10、《WebSocket 协议》§2.4/§3/§4/§5 补 td 会话控制；`GLOSSARY.md` 新增网关会话/登录/登出/连接·断开。代码侧实现见 #409。

## Context

- #409：td 账户没有连接/断开 UI 入口，登录失败后用户零自救路径；md 已有完整参考实现（REST `/login|/logout` → 帧 `REQUEST_MD_CONNECT/DISCONNECT`、WS `md_connect/md_disconnect`、`RTN_PROGRESS` 端点判定、登录/登出双按钮 + 批量）。
- 两种候选 UI：(A) 登录/登出双按钮，登出在任意非未登录状态可用；(B) 单按钮，点击后延迟 5 秒翻成「登出」、仅在完全未登录时显示登录。
- 两种候选命名：会话动作叫「连接/断开」还是「登录/登出」。CTP 的链路是连接→认证→登录→结算确认→初始化，未来柜台（XTP/易盛/IB 等）阶段数不同，甚至没有独立登出请求。
- 进度现状：md `progress_max=4`（0=Idle、1=连接或重连中、2=已连接或登录失败、3=登录中、4=已登录）；td `progress_max=10`（0/1/2 同义，3–8 为认证/登录/结算确认/合约加载，10=Ready）。两端**开头 0/1/2 与端点语义已一致**，只差 max 数值；前端已按 `current == min/max` 相对判定（`composables/marketSourceView.ts`）。
- 既有拍板：#406 确定 td 恢复路径 = 先登出再登录（不做 `can_login` 重登语义）；#407 确定 td 登录失败先经 `NOTIFY_UI` 上浮，`td-status.last_error` 另议；`apps/ctp/md/` 生产代码锁定（AGENTS 硬门禁 1）。

## Decision

1. **命名分两层**：会话动作 = **登录 / 登出**（按钮、REST 路径、通知、自动登录排程）；传输阶段 = **连接 / 断开**（`RTN_PROGRESS.desc`、状态名 Connecting/Disconnected、日志）。帧与 WS 保持既有 `*_CONNECT / *_DISCONNECT`（传输层命令名），与动作名一一对应并写进契约：登录 → `*_CONNECT`，登出 → `*_DISCONNECT`。不新增第三套词，未来接口类型照抄；帧改名 `*_LOGIN/*_LOGOUT` 不做。
2. **UI = 双按钮**：登录仅在 `progress.current <= progress.min`（完全未登录）可用；登出在 `current > min` 的任意状态（连接中/登录中/重连中/失败回滚/就绪）可用，即强制中断。两按钮始终渲染、只切换 disabled；不做「单按钮 + 延迟翻转」。
3. **进度端点语义固定、数值不固定**：`min = 0` 恒为未登录（唯一可发起登录的状态）；`current == max` 为就绪；`0 < current < max` 为会话建立中/重连中（不可登录）。中间值由接口类型定义；失败不是端点（md 登录失败态与已连接同为中间值）；消费方只按端点相对判定，禁止依赖具体数值。md/td 的 max 数值差异（4 / 10）保留，不为此改动 `apps/ctp/md/`。
4. **恢复统一两段式**：失败/卡死一律「先登出、再登录」。md 后端的 `can_login()` 重登能力保留给自动调度与 `--recover`，但前端不再提供失败态原地重试（契约 md-status 前端义务③相应修订）。理由：md 四类失败里只有 `login_timeout` 明显受益；`login_rejected`/`login_parse_error` 重试大概率重复失败，且改配置要求回到 Idle——登出本来就是必经步骤；与 #406 的 td 决策一致，md/td 共用一套 UI 规则。
5. **td 控制链路镜像 md**：`TD_CONNECT`/`TD_DISCONNECT`（payload `{"account_id"}`）生产端落在 dzweb；REST `POST /api/td/accounts/{account_id}/login|logout`、WS `td_connect/td_disconnect`（ack 携带 `account_id`）；无 RTN，结果经 `RTN_PROGRESS` 与 `RTN_TD_STATUS` 体现。守卫 = admin + 账户已登记（注册表解析网关）+ 目标网关 Running + 写通道可用。

**为什么不用其他方案**：

- **(B) 单按钮 + 5 秒延迟**：5 秒无业务依据（连接超时 30s + 登录超时 10s，可能 1s 也可能 12s+）；单按钮在连接中/登录中/重连中/失败/就绪下标签含义不同，「鼠标底下的按钮改变含义」正是误点来源；与批量登录/登出及行内其它操作不兼容。固定位置的双按钮从结构上消除该问题，无须时间补丁。
- **动作名用「连接/断开」**：会把传输阶段与「账号当下能不能用」的会话语义混在一起。FIX（Logon/Logout 是会话级管理消息）、CTP（`ReqUserLogin`/`ReqUserLogout`）、XTP（Login/Logout/Release，重连也是再 Login）、IB（brokerage session = 已登录 username）都以登录/登出作为会话建立/结束动词；仓库既有词汇（REST `/login`、自动登录排程 `login_time/logout_time`、#406/#407 文案）亦如此。
- **帧改名 `*_LOGIN/*_LOGOUT`**：帧是传输层命令，且 md 侧改名要动锁定目录、契约、前端与测试；收益仅为字面统一。
- **保留 md 失败态原地重试**：见决策 4；它会让 md/td 各有一套 UI 规则，抵消本 ADR 的主要收益。
- **把 md `progress_max` 改成 10**：只影响进度条刻度；消费方已按端点相对判定，代价是锁定目录改动 + 测试/契约同步，收益低。
- **td 再加一个「强制断开」按钮**：当前 md/td 的登出实现就是直接 `Release()`（不发送柜台登出请求），已是强制；将来真做优雅登出时，再把「强制断开」作为次要逃生动作，不与「登出」并列。

## Consequences

**正面**：md/td 共用一套会话模型与按钮规则，一套前端状态判定与 pending 规则；未来接口类型只需实现「登录/登出动作 ↔ `*_CONNECT/*_DISCONNECT` 帧」的映射并遵循进度端点骨架；td 登录失败/卡死有了唯一逃生路径（登出任意状态可用）；命名与实现不再分叉（用户看到「登录/登出」，内部是 CONNECT/DISCONNECT）。同时补齐了 td 网关在状态字段变化时推送 `RTN_TD_STATUS` / `RTN_PROGRESS` 的义务（此前只由快照与配置变更触发）——否则本 ADR 的「按进度端点判定」在 td 侧不可达。

**负面 / 已知限制**：md 登录失败后的恢复从 1 次点击变 2 次（多一次 Release+重连，几秒）——失败本身是低频事件，且与 td 行为一致；「连接/断开」仍出现在进度/状态/日志里，读者需理解两层命名的分工（由 GLOSSARY 与契约固定）；td 登录失败的持久原因展示在 `td-status.last_error` 子票落地前只有 `NOTIFY_UI` 弹窗，登录 pending 的失败清除依赖「`current` 回退」判据（进度骨架声明建立期单调、失败回退）。

**契约影响**：《帧契约：进度推送》（新增会话进度骨架）、《帧契约：行情网关状态》（失败语义与前端义务③）、《帧契约：行情连接与订阅》（动作名映射注记）、《帧契约：交易账户连接控制》（新）、《帧契约：REST API》§2.9.10 与 §4、《WebSocket 协议》§2.4/§3/§4/§5、《行情网关进程》/《交易网关进程》/《dzweb》组件文档、`GLOSSARY.md`。

## References

- 议题：#409（td 手动连接/断开入口）、#406（td 登录链无自动重试，恢复两段式拍板）、#407 / #408（登录失败上浮、`last_error` 另议）
- 契约：[进度推送](../frame_contracts/progress.md)、[行情网关状态](../frame_contracts/md-status.md)、[行情连接与订阅](../frame_contracts/md-subscription.md)、[账户登录状态](../frame_contracts/account-status.md)、[交易账户连接控制](../frame_contracts/td-connection.md)、[REST API](../frame_contracts/rest.md)（§2.9.10）、[WebSocket 协议](../frame_contracts/webui-ws.md)（§2.4/§3/§4/§5）
- 实现参考：`apps/webui/frontend/src/components/marketSources/LoginPanel.vue`（md 双按钮现状）、`apps/webui/frontend/src/composables/marketSourceView.ts`（`progressLoginState` 端点判定）
