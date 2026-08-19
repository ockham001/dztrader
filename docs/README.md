# 文档地图

本目录按"活跃真相源 / 架构参考"分层。开发时先看活跃真相源。

## 活跃真相源（与代码同步维护，以它们为准）

| 文档 | 内容 |
|------|------|
| [frame_contracts/](frame_contracts/README.md) | **帧协议与 frontend↔dzweb 协议的唯一语义真相源**（00-12，含通用规则、WS、REST）。开发 IPC/UI 联动前必读 |
| 仓库根 `README.md` | 构建/运行/部署指南 |

## 架构参考（蓝图级，方向性）

| 文档 | 内容 | 与现状的关系 |
|------|------|--------------|
| [architecture.md](architecture.md) | 进程模型、依赖方向、静态库、IPC 通道、策略接口约束 | 早期蓝图，方向有效；细节以代码为准（如订阅请求已由"master 转发"改为策略直发行情进程，见 frame_contracts 07） |
| [ui-trading.md](ui-trading.md) | UI 系统与交易接口规划 | 蓝图（Qt 后端、部分接口为规划项）；现状实现为 WebUI（Vue3 + dzweb），交易接口开发顺序以实际为准 |
| [components/](components/) | 组件级架构设计（线程模型、通道、会话、设计原则） | 与代码同步维护；按组件一档，如 [dzmd_ctp.md](components/dzmd_ctp.md) |
| [adr/](adr/) | 架构决策记录 | 按编号追溯决策 |

## 冲突裁决规则

1. 帧协议/WS/REST → `frame_contracts/`（当前版本）
2. 代码与契约冲突时，契约是语义真相源；修实现需走对应模块流程
