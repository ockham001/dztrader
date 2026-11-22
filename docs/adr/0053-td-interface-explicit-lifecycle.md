# ADR 0053: 交易接口进程生命周期由账户页显式管理（行情源同构）

## Status

Accepted（2026-11-22，#436）

契约同步：《帧契约：REST API》新增 §2.10（交易接口进程生命周期）、§2.3 勘误（available 只扫 `dzmd_*`）、§3（真相源）、§4（前端义务）；`GLOSSARY.md` 新增「交易账户」。无帧/WS 变更（复用 `process_config` / `process_status`）。组件文档（dztd_ctp 零账户运行注记、dzweb 端点）随实现票同步。

## Context

- 现状缺口：交易账户页添加账户要求目标网关已注册且 Running（契约 rest §2.9.7），但全仓能创建并启动网关进程的 REST 只有行情源域（`/api/market-sources/*` 硬编码 `dzmd_`，`/available` 也只返回 `dzmd_*`）。`dztd_*` 的唯一创建路径是人工改 `$DZTRADER_HOME/configs/dztraderd.json` 后重启 master——用户看到的「td ctp 加不了 → 账户加不了」是能力缺失，不是 UI 故障。
- 被否决的方案（账户集合驱动）：添加账户时自动动态注册并启动接口，接口最后一个账户删除后自动停止/移除。否决理由：master 启动时无条件拉起全部已注册条目（`start_all`），「删空即停」只有删配置条目才是持久的；且动态注册后网关先推一次空配置回报，「账户尚未写入」与「删空」在 reconcile 视角无法区分，需要防误杀与启动失败回滚，复杂度和误删风险都不值。
- 拍板的新形态：接口可零账户存在并运行；账户是接口内部配置项（与行情源卡片内的经纪商同构）；接口生命周期由页面显式管理。

## Decision

1. **页面 = 交易接口卡片 + 卡片内账户表**（导航与页头仍叫「交易账户」，只改组织形式）。接口是页面一级对象，从 WS `process_config`/`process_status` 派生；账户归属由所在卡片决定，账户表单不再选网关。
2. **接口生命周期显式管理、与账户解耦**：添加 = master 动态注册（写 `dztraderd.json`）+ 启动；接口可零账户运行；删除 = `REQUEST_PROCESS_CONTROL{Remove}`（停止 + 删配置条目）。**不提供持久的「停止但保留」**（停止只是崩溃/关机瞬时状态）；未运行时页面显示「启动进程」恢复（同行情源），不得以删除重加代替恢复。
3. **移除接口的 guard**：该接口在账户注册表里还有任何账户时禁止移除（409 `gateway_has_accounts`），必须先删空账户。理由：账户配置持久化在接口自身 `configs/<name>.json`（契约 td-config §6），删进程条目不动该文件——带账户移除会让账户在重新添加时复活，且期间注册表被清、`account_id` 全局唯一校验出现窗口（ADR 0045）。
4. **不引入持久化期望态（stop-keep）**：master 的模型是「配置条目 = 期望运行」（启动时无条件拉起），持久停止要给 process 配置加 desired-state 字段（process 契约 + master + md 一起改），本期不做。临时停交易 = 登出 + 禁用账户；下线 = 删除接口（先清账户）。

**为什么不用其他方案**：

- **账户集合驱动自动创建/停止**：见 Context；与「接口可零账户运行」的需求直接冲突。
- **带账户删除 = 级联清账户配置**：账户配置归属接口自身文件（td-config §6 的持久化边界），从平台侧删它是跨边界写入；先起进程再逐个 `RemoveAccount` 会引入半途失败。
- **持久化停止（desired_state）**：能力更完整但跨 process 契约/master/md，超出本期；未来需要时另开票，本 ADR 不阻碍。
- **td 复用 `market-sources` 端点**：会污染行情源语义（DB `market_sources` / `is_added` / 卡片机制都是 md 的），改为独立 td 端点（rest §2.10）更清晰。

## Consequences

**正面**：行情源页与交易账户页同构（添加/启动/删除/恢复同一套心智）；接口可零账户先建通道再配账户；账户表单去掉「选网关」，消除误配；不引入账户驱动的生命周期状态机与自动回滚/竞态。

**负面 / 已知限制**：接口有账户时不能直接删除（需先清账户）；无法「暂停接口但保留账户」（临时手段 = 登出 + 禁用账户）；接口有账户且 exe 丢失时无法从 UI 删除（需修复部署或手工改配置）；未运行是瞬时/异常态（启动中、崩溃重启窗口、重启耗尽、spawn 失败、关机中），UI 必须表达并提供「启动进程」。

**契约影响**：《帧契约：REST API》§2.3、§2.10（新）、§3、§4；`GLOSSARY.md`（交易账户）；《dzweb》《dztd_ctp》组件文档随实现票同步。无帧号/WS 变更。

## References

- 议题：#436（spec + 拆票）
- 契约：[REST API](../frame_contracts/rest.md)（§2.10、§2.3、§3、§4）
- 同构参考：行情源生命周期（`apps/webui/market_source_controller.cpp`、`apps/webui/frontend/src/views/MarketSourcesView.vue`、`apps/webui/frontend/src/components/marketSources/ProcessStartButton.vue`）
- 关联决策：ADR 0045（账户身份与注册表）、ADR 0051（会话控制）、契约 td-config §6（账户配置持久化归属）
