# 帧契约目录

本目录是 dztrader 进程间帧协议与 frontend↔dzweb 协议的**唯一语义真相源**。只规定协议，不约束内部实现。

## 目录地图

| 主题 | 覆盖帧 | 类型层真相源 |
|------|--------|--------------|
| [general](general.md) | 通用规则 | 帧布局/传输与数据流（§4 链路形态）/请求响应总则 + `QUERY_FULL_SNAPSHOT` | `libs/strategy_api/include/dztrader/struct.h`、`data_type.h`、`libs/core/.../core_data_type.h` |
| [log](log.md) | 日志 | `SET_LOG_CONFIG`/`FLUSH_LOG`/`RTN_LOG_CONFIG` | `libs/platform/.../log_config.h` |
| [shm](shm.md) | SHM 通道配置 | `SET/RTN_EVENT_SHM_CONFIG`、`SET/RTN_MD_SHM_CONFIG`、`PRELOAD_EVENT/MD_SHM`、`UPDATE_SHM_EVENT/MD_SUBSCRIBER`、`REQUEST/RTN_MD_READER_REGISTER`、`REQUEST/RTN_MD_READER_UNREGISTER` | `libs/platform/.../shm_config.h` |
| [notify-ui](notify-ui.md) | UI 通知 | `NOTIFY_UI` | `libs/platform/.../notify_ui.h` |
| [process](process.md) | 进程 | `REQUEST_PROCESS_CONTROL`、`RTN_PROCESS_STATUS`、`SET/RTN_PROCESS_CONFIG`、`REQUEST_SHUTDOWN` | `libs/platform/.../process.h` |
| [auto-login](auto-login.md) | 自动登录/登出排程 | `SET/RTN_AUTO_LOGIN` | `libs/platform/.../auto_login.h` |
| [progress](progress.md) | 进度推送 | `RTN_PROGRESS` | `libs/platform/.../progress.h` |
| [md-subscription](md-subscription.md) | 行情连接与订阅 | `REQUEST_MD_CONNECT/DISCONNECT`、`REQUEST_MD_SUBSCRIBE`、`QUERY/RTN_MD_SUBSCRIPTIONS`、`NOTIFY_MD_STARTED/STOPPED` | `libs/platform/.../subscription_manager.h`、`libs/core/.../core_struct.h` |
| [md-config](md-config.md) | 行情网关配置 | `SET/RTN_MD_CONFIG` | `libs/platform/.../ctp_md_config.h` |
| [md-status](md-status.md) | 行情网关状态 | `RTN_MD_STATUS` | 发送方进程（CTP：`apps/ctp/md/md_state.h`） |
| [webui-ws](webui-ws.md) | WebSocket 协议 | frontend ↔ dzweb 的 WS 信封、消息全集、前端行为义务 | `apps/webui/ws_controller.*` |
| [rest](rest.md) | REST API | frontend ↔ dzweb 的 REST 端点与帧联动 | `apps/webui/*_controller.h` |
| [td-order](td-order.md) | 交易委托请求 | `TD_ORDER_REQ`/`TD_ORDER_CANCEL_REQ` | `libs/core/.../core_struct.h` |
| [strategy](strategy.md) | 策略帧 | `STG_USER_INPUT`/`STG_USER_OUTPUT`/`SET_LOGICAL_POSITION` | `libs/core/.../core_struct.h` |

## 阅读顺序

先读 [general](general.md)（总则），再按需读各契约。每份契约只写与总则不同的部分；跨契约引用格式：《帧契约：\<主题\>》§N（**禁止以行号引用契约**，见 general §11.2）。

## 单一真相源原则

| 内容 | 真相源 | 契约角色 |
|------|--------|----------|
| 帧号、帧头布局 | `data_type.h` / `core_data_type.h` / `struct.h` | 引用，不抄写 |
| struct payload 字段 | 对应头文件（`struct.h`/`core_struct.h`/`platform/*.h`） | 引用，不抄写字段表 |
| JSON payload schema 与校验语义 | **本目录对应契约** | 唯一定义处；代码校验函数引用契约 |
| WS/REST 协议 | 契约 webui-ws 与 rest | 唯一定义处 |

## 范围与遗留

- 本目录覆盖事件通道的低频控制/配置/通知帧。
- 策略帧契约已收录（见 [strategy](strategy.md)）；`STG_USER_OUTPUT`/`SET_LOGICAL_POSITION` 的 dzweb 消费与 WS/REST 映射未接线（契约定义语义，实现滞后由 general §11.3 checklist 跟踪）。
- **未覆盖**（后续独立契约）：交易帧（除契约 td-order 已覆盖的 `TD_ORDER_REQ`/`TD_ORDER_CANCEL_REQ` 外，其余 2000-2114）、行情/交易数据帧（`RTN_MD_TICK`、TD 推送 2000-2004）、`SYS_SCHED`（帧类型保留未用，见 general §10）。

## 变更流程

修改契约必须执行 [general §11.3](general.md) 的变更 checklist（platform 头文件、帧号登记、dzweb 领域服务、前端、测试、契约 webui-ws 与 rest 映射表同步检查）。
