# ADR 0043: dzweb 交易数据读路径：REST 直查 + WS 推送 + seq 合并

## Status

Accepted（2026-11-03）

## Context

- UI 需要当天委托 / 成交 / 持仓 / 资金与"未成交 / 总"统计（委托与成交放在同一页面的两个 tab，默认委托）；压力口径 20 账户、20 万行 / 日。
- dzweb 是单网络 IO 线程（`setThreadNum(1)`）；现有 `TdDataService` 维护四类内存镜像但没有 REST / WS 出口；镜像与 DB 是两套历史，随运行必然漂移。
- 账户登录状态由 `ACCOUNT_STATUS` 提供；TD 配置 / 细状态帧（1029 / 1030）已由 td 上报，但 dzweb 未消费、也无契约。

## Decision

1. **不保留四类数据的全量内存镜像**；历史 / 基线由 REST 只读直查 `trade.db`（每请求独立只读连接、游标分页 200 条 / 页、禁 OFFSET 深翻页、读连接 `busy_timeout` 200ms）。dzweb 只保留：在线账户集合（来自 `ACCOUNT_STATUS`）、统计计数器（4 个整数）、WS 建连补发环形缓冲（300 条 / 账户）、td 配置已知状态（账户注册表）；委托 / 成交 / 持仓 / 资金记录一律现查 DB 或转发帧，不落任何记录镜像。
2. **范围**：委托 / 成交 / 统计仅覆盖在线账户（`ACCOUNT_STATUS = Ready`）与当前交易日；持仓 / 资金覆盖全部账户（含离线），按账户锚点日取最新行；无记录时资金返回 `null` + `has_data:false`、持仓为空。列表接口不传交易日参数，由 dzweb 解析当前交易日。UI 同一页面两个 tab：「委托」（默认，展示订单行上的派生 `avg_price`）与「成交」（逐笔成交，按成交时间倒序）。委托列表展示订单行上的派生 `avg_price`（成交均价，见 [ADR 0041](0041-sqlite-only-trade-store-schema-v1.md) 决策 3）；本轮 UI 只有委托页，不做成交明细页，`trades` 仍全量落库（均价重建、重放去重、seq、对账与将来明细页都用它）。REST / WS 的委托行返回原始 `avg_price`（double，无成交为 `null`）、`price_decimals`（整数，dzweb 从 `instruments.price_tick` 推导，浮点安全）与 `avg_price_decimals`（均价显示小数位，dzweb 计算：基准精度 + 2，夹在 [2, 6]）。**查不到合约 / `price_tick` 时退化**：用该单委托价 / 成交价 / 均价的运行时小数位作为基准再 +2（如价格 552 → 均价 2 位，551.12；552.5 → 3 位，551.123）。舍入在 UI 的 `toFixed(avg_price_decimals)` 完成——dzweb 只给原始值和精度整数（保留全精度用于排序 / 导出 / tooltip，避免服务端产生字符串真相）；均价随订单记录帧 / DB 实时更新（[ADR 0041](0041-sqlite-only-trade-store-schema-v1.md) 决策 3）。
3. **实时**：WS 新增委托 / 成交 / 持仓 / 资金四类交易事件消息（不进 `snapshot`）；建连先补发最近事件（每账户上限 300 条）再转 live；前端按业务键合并、同键 `seq` 大者胜；日切 / 在线集合变化广播 `data_changed{scope:"td"}` 触发重拉；td 在会话交易日变化时强制重推一次 `ACCOUNT_STATUS`（绕过三态去重），使 dzweb 感知日切。
4. **统计**：dzweb 内存计数器维护 4 个值（总 / 未成交 × 笔 / 手），启动 / Ready / 日切从 DB 重算并定时对账；不做成交额与手续费统计（手续费在账户卡片用账户级 commission）。
5. **降级**：统一 `200 {available:false, reason}`，不返回 5xx、不阻塞 IO 线程。
6. `seq` 出现在 REST / WS payload 中，仅作不透明版本号用于合并，不参与业务。
7. **前端列表是有界窗口，不在浏览器里保存全量镜像**：REST 游标分页（首屏 + 滚动预取），WS 增量在顶部合并（按业务键去重、`seq` 大者胜）；单个 tab 的内存行数设上限（建议 2,000 行，可配），超出丢弃最旧行，更早数据靠筛选 / 再次分页拉取；DOM 用虚拟滚动只渲染可视行；WS 事件批量合并刷新（约 10Hz，避免逐条触发重渲染）；日切 / `data_changed` 清空重拉。淘汰按容量触发，不用定时清理定时器。

**为什么不用"dzweb 内存镜像 + WS 全量快照"**：镜像快照随历史线性增长，且与 DB 两套历史无法保证一致；直查 + 版本号合并更简单、可维护。

## Consequences

- 契约：`rest`（新端点）、`webui-ws`（新消息 / 建连补发 / `data_changed`）、`td-data-sync`（seq 暴露与镜像节改写）、新增 `td-config` / `td-status`（`td-config` 覆盖 1026 配置写 + 1029 全量含启动主动推，`td-status` 覆盖 1030；通用化：`interface_type` + 柜台 ext）。
- 单 IO 线程需分页与超时护栏；前端页面（委托 + 成交 tab / 持仓 / 资金 / 统计）另票实现，列表需虚拟滚动 + 批量刷新 + 行数上限。
- 已知限制：持久化延迟窗口由建连补发兜底；跨会话历史回看走结算页（后续）。
