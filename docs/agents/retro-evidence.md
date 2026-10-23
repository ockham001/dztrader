# 复盘取证（retro evidence）

`retro` 要读**主会话的原始日志**，而不是凭记忆或摘要。本文记录本仓库在 DSH 下的日志位置、格式与提取方法，省掉每次重新发现。

> 前提：会话日志在 workspace 的 `~/.dsh/sessions/` 下，**即使 UI 里删除了会话，文件通常仍在磁盘上**（UI 删除只是从列表隐藏）。

---

## 1. 日志位置

```
~/.dsh/sessions/--<cwd 路径以 - 连接>--/<session-id>/session.v4.jsonl.zstd
```

本仓库：

```bash
BASE="$HOME/.dsh/sessions/--$(pwd -P | sed 's#^/##; s#/#-#g')--"
# 等价于 ~/.dsh/sessions/--home-ockham-projects-dztrader--
ls "$BASE"        # 每个目录一个会话（含主会话与子代理会话）
```

- 同一 cwd 的会话共享一个父目录；目录名带 `session-` 前缀的通常是主会话，其余为子代理/派生会话。
- 文件是 **zstd 压缩的 JSONL**（一行一个事件）：`zstd -dc "$BASE/<id>/session.v4.jsonl.zstd"`。不要用 `cat` 直接读；先 `zstd -dc` 再 `jq` / `grep`。

## 2. 事件格式（jsonl.zstd）

首行是会话元数据；之后每行 `{"type": "...", "time": <ms>, "data": {...}}`。复盘常用事件：

| `type` | 关键字段 | 用途 |
|---|---|---|
| `session` | `id` / `createdAt` / `cwd` / `parentSession` / `origin` | 会话元数据（parentSession 可区分主会话/子代理） |
| `session/title` | `data.title` | 会话标题（常用于把票号映射到会话） |
| `user/message` | `data.source.kind`（`user`/`subagent`/…）、`data.content[].text` | 人工消息 vs 子代理回报 |
| `assistant/message` | `data.message.content[]`（`text` / `tool-call`） | 助手文本与工具调用（`tool-call.arguments` 是 JSON 字符串） |
| `tool/result` | `data.message.content[].text`、`toolCallId` | 工具输出（bash 输出里含 `[exit code: N]`） |
| `job/settled` | `data.status` / `startedAt` / `finishedAt` | 后台作业耗时与成败（等 CI 的长作业都在这里） |
| `llm/retry` | `data.failure.code` / `message` | 模型重试（`TRANSPORT` / `TIMEOUT`，复盘 provider 稳定性用） |
| `step/start`、`turn/start` | — | 统计步骤/轮次 |

## 3. 按票号找到对应会话

优先看 `session/title` 与首条 human 消息：

```bash
sid=<session-id>
f="$BASE/$sid/session.v4.jsonl.zstd"
zstd -dc "$f" | jq -r 'select(.type=="session/title") | .data.title'
zstd -dc "$f" | jq -r 'select(.type=="user/message" and .data.source.kind=="user") | .data.content[]? | select(.type=="text") | .text' | head -3
```

一次扫全部会话的标题：

```bash
for d in "$BASE"/*/; do
  zstd -dc "$d/session.v4.jsonl.zstd" 2>/dev/null \
    | jq -r --arg id "$(basename "$d")" 'select(.type=="session/title") | "\($id)\t\(.data.title)"'
done
```

## 4. 提取成时间线/摘要

上一轮 retro（#69–#74、#83–#93）用过的提取脚本是 `.cache/retro/extract.py` + `digest.py`（**gitignored、可删**；丢了按上面的事件表重写也不难）。做法：

1. `zstd -dc` 解压到内存/临时文件；
2. 按事件类型过滤出：human 消息、无 `#<票号>` 的 bash 失败（`[exit code: N]`）、`subagent` 结果、`job/settled` 里 ≥60s 的长作业、`llm/retry`；
3. 汇总到 `.cache/retro/<label>-timeline.md` / `-digest.md`，再按复盘候选归类。

**注意**：`.cache/` 不在版本控制内，跨会话保留但可随时被 `rm -rf`；需要长期保留的结论写进 issue/ADR，不写进 `.cache/`。

## 5. 其他取证源

- CI：`gh run list --limit N --json ...`；失败日志用 `./scripts/ci-log.sh <run-id>`（完整日志落 `.cache/ci/`）。
- issue/PR 时间线：`gh issue view <n> --json state,closedAt,comments`、`gh pr view <n> --json state,mergedAt,mergeCommit`——复盘「关单是否早于合并」这类问题必须用时间戳，不靠记忆。
- 会话里的临时产物（handoff、close 报告、审查缓存）一般在 `.cache/`；同样是 gitignored。
