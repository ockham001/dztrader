---
name: 帧契约变更
description: 改帧号/帧头布局/JSON payload schema/WS 或 REST 协议/跨进程数据流语义前，按本技能走：先改帧契约，再按 general §11.3 六项同步清单落地并验证。
---

# 帧契约变更

`docs/frame_contracts/` 是唯一语义真相源：契约先行，实现随后（《帧契约：通用规则》§11.3）。

## 何时用

改动命中任一分支即适用：

- 新增/修改/删除帧（`DZ_FRAME_*`）或帧头布局
- 修改 JSON payload schema 或其校验语义
- 修改 frontend ↔ dzweb 的 WS 消息或 REST 端点
- 修改跨进程数据流语义（订阅、回报、状态等）

## 流程

1. 读 `docs/frame_contracts/general.md`（总则）与相关主题契约；确认改动落在哪几份契约。
2. 修改契约文档：按 general §11.1 模板与 §11.2 锚点规则；引用一律《帧契约：\<主题\>》§N。
3. 按 general §11.3 六项同步清单逐项落地（细节以 §11.3 为准，本节不复制）：
   platform 头文件类型与校验 → 帧号/帧头登记 → dzweb 领域服务 → 前端 → 单测 + `frame_types` 测试 → WS/REST 契约映射表。
4. 新增主题契约时，更新 `docs/frame_contracts/README.md` 目录地图。
5. 跑测试：`./scripts/test.sh`（至少覆盖 `frame_types` 与相关模块）。
6. 契约与实现随同小步提交；提交信息写明契约面。

## 完成判据

- §11.3 六项逐项已落地，或已在契约内标注「未接线」（写明实现状态与接线触发条件）
- `ctest` 相关测试全绿
- 目录地图与 `docs/README.md` 无悬空引用

## 硬约束

- 涉及 `apps/ctp/md/`：改前先向用户说明并取得确认（AGENTS.md 硬门禁 1）
- 帧号在段内密集追加、不复用、不在文档里写帧号数值（general §3）
- 契约只描述协议语义（不写内部函数名与实现细节）
