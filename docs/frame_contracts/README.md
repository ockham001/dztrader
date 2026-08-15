# 帧契约目录

本目录是 dztrader 进程间帧协议与 frontend↔dzweb 协议的**唯一语义真相源**。只规定协议，不约束内部实现。

## 目录地图

| 编号 | 主题 | 覆盖帧 | 类型层真相源 |
|------|------|--------|--------------|
| [00-general](00-general.md) | 通用规则 | 帧布局/传输/请求响应总则 + `QUERY_FULL_SNAPSHOT` | `libs/strategy_api/include/dztrader/struct.h`、`data_type.h`、`libs/core/.../core_data_type.h` |
| [01-log](01-log.md) | 日志 | `SET_LOG_CONFIG`/`FLUSH_LOG`/`RTN_LOG_CONFIG` | `libs/platform/.../log_config.h` |
| [02-shm](02-shm.md) | SHM 通道配置 | `SET/RTN_EVENT_SHM_CONFIG`、`SET/RTN_MD_SHM_CONFIG`、`PRELOAD_EVENT/MD_SHM`、`UPDATE_SHM_EVENT/MD_SUBSCRIBER` | `libs/platform/.../shm_config.h` |
| [03-notify-ui](03-notify-ui.md) | UI 通知 | `NOTIFY_UI` | `libs/platform/.../notify_ui.h` |
| [04-process](04-process.md) | 进程 | `REQUEST_PROCESS_CONTROL`、`RTN_PROCESS_STATUS`、`SET/RTN_PROCESS_CONFIG`、`REQUEST_SHUTDOWN(_ALL)` | `libs/platform/.../process.h` |
| [05-auto-login](05-auto-login.md) | 自动登录/登出排程 | `SET/RTN_AUTO_LOGIN` | `libs/platform/.../auto_login.h` |
| [06-progress](06-progress.md) | 进度推送 | `RTN_PROGRESS` | `libs/platform/.../progress.h` |
| [07-md-subscription](07-md-subscription.md) | 行情连接与订阅 | `REQUEST_MD_CONNECT/DISCONNECT`、`REQUEST_MD_SUBSCRIBE`、`QUERY/RTN_MD_SUBSCRIPTIONS`、`NOTIFY_MD_STARTED/CONNECTED/DISCONNECTED/STOPPED` | `libs/platform/.../subscription_manager.h`、`libs/core/.../core_struct.h` |
| [08-md-config](08-md-config.md) | 行情网关配置 | `SET/RTN_MD_CONFIG` | `libs/platform/.../ctp_md_config.h` |
| [09-md-status](09-md-status.md) | 行情网关状态 | `RTN_MD_STATUS` | 发送方进程（CTP：`apps/ctp/md/md_state.h`） |
| [10-webui-ws](10-webui-ws.md) | WebSocket 协议 | frontend ↔ dzweb 的 WS 信封、消息全集、前端行为义务 | `apps/webui/ws_controller.*` |
| [11-rest](11-rest.md) | REST API | frontend ↔ dzweb 的 REST 端点与帧联动 | `apps/webui/*_controller.h` |
| [12-td-order](12-td-order.md) | 交易委托请求 | `TD_ORDER_REQ`/`TD_ORDER_CANCEL_REQ` | `libs/core/.../core_struct.h` |

## 阅读顺序

先读 [00-general](00-general.md)（总则），再按需读各契约。每份契约只写与总则不同的部分；跨契约引用格式：《帧契约：\<主题\>》§N（**禁止以行号引用契约**，见 00-general §11.2）。

## 单一真相源原则

| 内容 | 真相源 | 契约角色 |
|------|--------|----------|
| 帧号、帧头布局 | `data_type.h` / `core_data_type.h` / `struct.h` | 引用，不抄写 |
| struct payload 字段 | 对应头文件（`struct.h`/`core_struct.h`/`platform/*.h`） | 引用，不抄写字段表 |
| JSON payload schema 与校验语义 | **本目录对应契约** | 唯一定义处；代码校验函数引用契约 |
| WS/REST 协议 | 契约 10/11 | 唯一定义处 |

## 范围与遗留

- 本目录覆盖事件通道的低频控制/配置/通知帧。
- **未覆盖**（后续独立契约）：交易帧（除契约 12 已覆盖的 `TD_ORDER_REQ`/`TD_ORDER_CANCEL_REQ` 外，其余 2000-2114）、策略帧（3001+、`OUTPUT_UI`/`SET_LOGICAL_POSITION`）、行情/交易数据帧（`RTN_MD_TICK`、TD 推送 2000-2004）、`SYS_SCHED`（帧类型保留未用，见 00-general §10）。

## 变更流程

修改契约必须执行 [00-general §11.3](00-general.md) 的变更 checklist（platform 头文件、帧号登记、dzweb 领域服务、前端、测试、契约 10/11 映射表同步检查）。
