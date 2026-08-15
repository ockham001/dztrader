# ADR 0003: 帧契约目录重整与编号迁移（frame_contracts v2）

## Status

Accepted

## Context

`docs/frame_contracts/` 原按"一帧（或一组帧）一文件"编号 00-06，存在三类问题：

1. **公共规则重复**：请求-响应约定、校验失败处理、`instance_id` 规范、dzweb 镜像规则在各契约文件中各自表述，不同步时产生歧义。
2. **06-misc 无归属感**：收纳"未分类"帧的契约文件持续膨胀，缺少长期归属。
3. **协议存在两种表述**：`docs/flow_contracts/`（UI 流程视角）与 frame_contracts（帧视角）并存，同一协议两处描述，引述易错；REST 端点与交易委托请求长期无契约，实现先于契约。

2026-08 的 webui 架构重构（P1-P4）要求 frontend↔dzweb 协议有唯一、可锚定引用的语义真相源，触发本次重整。

## Decision

1. **新增 `00-general` 总则**：帧布局、帧号分段、`instance_id` 规范、JSON/struct 编码判定、请求-响应总则（必回 RTN/快照查询/并发语义）、校验失败"四件套"、镜像总则、契约模板与变更 checklist（§11）。各契约只写与总则不同的部分。

2. **重编号（最后一次）**：

   | 旧编号 | 主题 | 新编号 |
   |--------|------|--------|
   | 00-log | 日志 | 01-log |
   | 01-shm | SHM 通道配置 | 02-shm |
   | 02-notify-ui | UI 通知 | 03-notify-ui |
   | 03-process | 进程控制/配置 | 04-process |
   | 04-auto-login | 自动登录排程 | 05-auto-login |
   | 05-progress | 进度推送 | 06-progress |
   | 06-misc | 未分类 | **拆解归位** |
   | 07-md-subscription | 行情连接与订阅 | 07（不变，修订） |
   | 08-md-config | 行情网关配置 | 08（不变，修订） |
   | 09-md-status | 行情网关状态 | 09（不变，修订） |
   | 10-webui-ws | WebSocket 协议 | 10（不变，修订） |

   06-misc 拆解去向：`QUERY_FULL_SNAPSHOT` → 00-general §7；`REQUEST_MD_CONNECT/DISCONNECT`、行情服务生命周期通知（`NOTIFY_MD_*`）→ 07-md-subscription；`REQUEST_SHUTDOWN(_ALL)` → 04-process；`UPDATE_SHM_EVENT/MD_SUBSCRIBER` → 02-shm。

3. **新增契约**：`11-rest`（REST 端点清单与帧联动）、`12-td-order`（`TD_ORDER_REQ`/`TD_ORDER_CANCEL_REQ` 改为 basic 广播帧，payload `account_id` 归属路由，`DzOrderCancelReq` 增加 `account_id`）。

4. **删除 `docs/flow_contracts/` 01-06**：内容由 10-webui-ws / 11-rest 取代。

5. **配套实现变更**（与契约同批落地）：
   - 删除 `frame_has_instance_id()` 集中映射表；改由接收方注册帧监听时自行声明该帧是否含 `instance_id`（00-general §3）。
   - 新增 `DZ_FRAME_REQUEST_MD_READER_REGISTER/UNREGISTER`（1013/1014，02-shm）；策略 SDK 在 `dz_create_md_source`/`dz_destroy_md_source` 主动注册/注销读者。
   - `frame_codec` 写入辅助函数返回 `bool`（写入 + 唤醒订阅者成功），唤醒语义并入写函数。
   - `dz_cancel_order` 签名增加 `account_id`（与 `dz_place_order` 对称，撤单帧按账户路由）。
   - `DZ_FRAME_REQUEST_SHUTDOWN` 由广播改为定向（`instance_id` = 目标进程名）。

6. **编号规则**：契约编号一经发布**只增不改**，本次为最后一次编号迁移；引用契约一律用《帧契约：\<主题\>》§N 锚点，禁止行号引用。

## Consequences

- **历史文档不改**：specs/plans 等历史文档中的旧编号引用按 docs/README 冲突裁决规则处理——不修改，以新契约为准。
- **旧编号映射**：阅读 2026-08 之前的 plans/specs 时需按上表换算编号。
- **帧布局变化**：`DzOrderCancelReq` 增加 `account_id` 字段、`REQUEST_SHUTDOWN` 语义变化——新旧版本进程混跑时不兼容，需整组升级。
- **契约变更门槛**：任何契约修改必须执行 00-general §11.3 checklist（platform 头文件、帧号登记、dzweb 领域服务、前端、测试、契约 10/11 映射表）。

## References

- [00-general](../../docs/frame_contracts/00-general.md)（§1 历史注记、§11 变更规则）
- [frame_contracts README](../../docs/frame_contracts/README.md)
- 相关提交：契约重整分片（docs → libs → apps/ctp → apps/master → apps/webui → frontend）
