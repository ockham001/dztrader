# 文档地图

本目录按"活跃真相源 / 架构参考"分层。开发时先看活跃真相源。

## 活跃真相源（与代码同步维护，以它们为准）

| 文档 | 内容 |
|------|------|
| [frame_contracts/](frame_contracts/README.md) | **帧协议与 frontend↔dzweb 协议的唯一语义真相源**（无编号；**先读 [general](frame_contracts/general.md) 总则**）。开发 IPC/UI 联动前必读 |
| [development-checklist.md](development-checklist.md) | **新功能开发门禁清单**（P6）：A 前端/B 后台/C 提交前/D 策略 SDK 四段，每项映射到自动化检查 |
| [unit-test-standard.md](unit-test-standard.md) | **单元测试判据的唯一来源**：有效性/覆盖/一致性/成本/基建五组条目，供撰写与审查逐条对照 |
| 仓库根 `README.md` | 构建/运行/部署指南 |

## 架构参考（蓝图级，方向性）

| 文档 | 内容 | 与现状的关系 |
|------|------|--------------|
| [architecture.md](architecture.md) | 进程模型、依赖方向、静态库、IPC 通道、策略接口约束 | 早期蓝图，方向有效；细节以代码为准（如订阅请求已由"master 转发"改为策略直发行情进程，见《帧契约：md-subscription》） |
| [ui-trading.md](ui-trading.md) | UI 系统与交易接口规划 | 蓝图（Qt 后端、部分接口为规划项）；现状实现为 WebUI（Vue3 + dzweb），交易接口开发顺序以实际为准 |
| [components/](components/) | 组件级架构设计（线程模型、通道、会话、设计原则） | 与代码同步维护；按组件一档，如 [dzmd_ctp.md](components/dzmd_ctp.md)、[dzsysmon.md](components/dzsysmon.md) |
| [adr/](adr/) | 架构决策记录 | 按编号追溯决策 |

## 代理约定（工具无关）

编码 agent 的项目约定。`AGENTS.md` 是**根入口**，任何支持 [AGENTS.md](https://agents.md/) 约定的工具都能自动发现它（Claude Code / OpenCode / Codex / Cursor / …）。

| 文档 | 内容 |
|------|------|
| 仓库根 [AGENTS.md](../AGENTS.md) | 硬门禁（md 生产锁定、`DZTRADER_HOME`、编译期门禁、4 命令工作流、禁 `npm run build`）、架构不变量、方案收敛规则、代码约定、工作方式 |
| [agents/build-appendix.md](agents/build-appendix.md) | 构建外围细节：工具链、MSVC 激活、clangd、预设、输出布局、单测约定、**性能基准与基线记录**、install、conan、前端机制、部署产物分析、跨平台 `#error` 模板、沙箱缓存/临时目录 |
| [agents/ticket-closeout.md](agents/ticket-closeout.md) | 每票收尾流程：冻结审查 / 门禁 / push scope / PR 与合并 / CI 等待 / 关 issue / 清理与工作区交还 |
| [agents/review-checklist.md](agents/review-checklist.md) | 审查清单：AC 逐条核对 + 6 类高频 finding（注释漂移 / 恒真弱断言 / 删除不彻底 / 同一事实两处定义 / 术语口径 / 诊断消息形态），每类附本波实例 |
| [agents/retro-evidence.md](agents/retro-evidence.md) | 复盘取证：本项目 DSH 会话日志位置、事件格式、按票号映射会话与提取命令 |

与 [development-checklist.md](development-checklist.md) 的分工：checklist 是**门禁清单的权威来源**（前端/后台/提交前三段），AGENTS.md 是硬门禁与工作方式的权威来源。

## 冲突裁决规则

1. 帧协议/WS/REST → `frame_contracts/`（当前版本）
2. 代码与契约冲突时，契约是语义真相源；修实现需走对应模块流程
3. 硬门禁与工作方式 → 根 `AGENTS.md`；门禁清单细节 → `development-checklist.md`；测试判据 → `unit-test-standard.md`
4. 架构参考（architecture.md / ui-trading.md / components/）为蓝图级：与代码和契约冲突时以后者为准
5. 本地历史归档（docs/plans|specs|superpowers，不在版本控制内）：与现状冲突时以契约与代码为准，归档内容不修改
