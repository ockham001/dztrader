#!/usr/bin/env python3
"""retro_extract.py — 把 DSH 会话日志提取成复盘 digest（只读）。

DSH 会话日志：`~/.dsh/sessions/--<cwd 转义>--/<session>/session.v4.jsonl.zstd`
（zstd 压缩 JSONL；详见 docs/agents/retro-evidence.md）。

产出 `.cache/retro/<session-id>-digest.md`，含：轮次墙钟、人工消息、审批、工具失败、
>=60s 长作业、子代理调用、工具计数、LLM 重试——按复盘候选归类前的原始证据。

工具失败有两类来源（#284 复盘 AC8）：
  failures        `message.isError == true` 的工具结果（原有口径）。
  masked failures `isError == false`，但**输出文本**里已经有失败证据——典型是
                  `cmd; echo rc=$?` 把非零退出码吞掉，宿主看不到 isError。按行匹配
                  `rc=[1-9]` / `FAILED` / `FAIL:` / 行首 `error:`，同一 tool call 只记
                  第一条命中（附命中行前 160 字），最多 40 条，避免 digest 爆掉。
                  该小节与 `## failures` 分开，不改变原有小节与格式；`isError == true`
                  的结果不进本小节（已在 failures 里，避免同一事件记两次）。

用法:
  python3 scripts/retro_extract.py --latest                # 本 cwd 最新主会话
  python3 scripts/retro_extract.py --session <path.zstd>   # 指定会话文件
  python3 scripts/retro_extract.py --all --recent 5        # 最近 5 个主会话逐个出 digest
  python3 scripts/retro_extract.py --latest --out .cache/retro
  python3 scripts/retro_extract.py --latest --decompose    # 追加 turn 内墙钟分解小节

墙钟分解口径（--decompose，只追加 `## decompose` 小节，不改变原有输出）:
  turn_wall = model_gen + tool_exec + idle      守恒关系，每个事件只计一次
  model_gen   = Σ(该 step 内首个 assistant/message − step/start)，每个 step 计一次
  tool_exec   = Σ(tool/result − 对应 tool/call)，按 callId 配对、每个 callId 只计一次；
                同一 turn 内并行的多个调用按区间并集计（不重复叠加）
  idle        = turn_wall − model_gen − tool_exec（下界 0）
  ask 阻塞     = ask_user_question 的 result − call，**是 tool_exec 的子集**，单列不重复计入
 等待型工具   = wait_agent / `job_output(wait=true)` 的 result − call，同样是 tool_exec 的
                子集，单列次数与总时长；非阻塞的 `job_output(wait=false)` 不计入
  未闭合 turn（当前会话尾部，无 turn/end）若已有时间戳就列出 model/tool 秒数与
  ask/wait 子集计数并标注，但**不计入整会话合计**（wall 未知，无法做占比）。

依赖：Python 3.10+ 标准库 + `zstd` CLI（不引入 Python 第三方依赖）。
注意：当前会话在 turn 结束才落盘后续事件；复盘“已结束的会话”最完整。
退出码: 0=成功, 2=找不到会话/zstd/解析失败。
"""

from __future__ import annotations

import argparse
import collections
import json
import re
import subprocess
import sys
from pathlib import Path

LONG_JOB_SEC = 60
SUBAGENT_TOOLS = ("subagent", "subagent_fork", "spawn_teammate", "workflow")
# 墙钟分解（--decompose，#309 AC9）：等待型工具是 tool_exec 的子集，单列次数与总时长。
# `job_output` 只有 `wait=true` 才是等待型（`wait=false` 是即时取结果，按普通工具计）。
ASK_TOOL_NAMES = frozenset({"ask_user_question"})
WAIT_TOOL_NAMES = frozenset({"wait_agent"})
BLOCKING_JOB_TOOL = "job_output"
_SAFE_CHAR_RE = re.compile(r"^[A-Za-z0-9._-]$")
# 自掩码实红（#284 AC8）：isError=false 但工具输出文本里已有失败证据。模式保持有限，
# 只认退出码与非零标记，不做自然语言判定；同一 tool call 只记第一条命中。
# **只对命令类工具生效**：`read` / `grep` / `diff` 的输出里出现 `FAILED`、`error:` 是
# 被查看的内容（日志、测试源码、CI 日志），不是本次调用的失败，否则 digest 全是误报
# （#289 审查 conformance 8）。
MASKED_FAILURE_LIMIT = 40
MASKED_FAILURE_TOOL_NAMES = frozenset({"bash", "shell", "job_output", "wait-ci"})
MASKED_FAILURE_RES = (
    # cmd; echo rc=$? 把非零退出码吞掉的形态
    ("rc!=0", re.compile(r"\brc=([1-9][0-9]*)\b")),
    ("FAILED", re.compile(r"\bFAILED\b")),
    ("FAIL:", re.compile(r"\bFAIL:")),
    ("error:", re.compile(r"^\s*error:")),  # 工具自己打印的 error 行（宿主未标 isError）
)


def die(msg: str) -> None:
    print(f"error: {msg}", file=sys.stderr)
    sys.exit(2)


def _encode_project_key(cwd: str) -> str:
    """照抄 DSH projectKey：分隔符合并为一个 `-`，不安全码位转 `~XXXX`，去前导 `-`、截断 251。"""
    if not cwd:
        raise ValueError("cannot encode an empty project path")
    out: list[str] = []
    separator_run = False
    for ch in cwd:
        if ch in "/\\:":
            if not separator_run:
                out.append("-")
            separator_run = True
        elif ch != "~" and _SAFE_CHAR_RE.match(ch):
            out.append(ch)
            separator_run = False
        else:
            out.append(f"~{ord(ch):04X}")
            separator_run = False
    readable = "".join(out).lstrip("-") or "root"
    return f"--{readable[:251]}--"


def _session_root_for(base: Path) -> Path:
    return Path.home() / ".dsh" / "sessions" / _encode_project_key(str(base.resolve()))


def main_sessions(root: Path) -> list[Path]:
    if not root.is_dir():
        return []
    paths = [p for p in root.glob("session-*/session.v4.jsonl.zstd") if p.is_file()]
    return sorted(paths, key=lambda p: p.stat().st_mtime, reverse=True)


def sessions_root() -> Path:
    """会话按「启动时 cwd」分目录；worktree 内复盘时回退到主仓根（日志通常记在主仓下）。"""
    candidates = [Path.cwd()]
    result = subprocess.run(
        ["git", "rev-parse", "--path-format=absolute", "--git-common-dir"],
        capture_output=True,
        text=True,
    )
    if result.returncode == 0 and result.stdout.strip():
        candidates.append(Path(result.stdout.strip()).parent)  # <repo>/.git → <repo>
    result = subprocess.run(["git", "rev-parse", "--show-toplevel"], capture_output=True, text=True)
    if result.returncode == 0 and result.stdout.strip():
        candidates.append(Path(result.stdout.strip()))

    seen: set[Path] = set()
    for base in candidates:
        root = _session_root_for(base)
        if root in seen:
            continue
        seen.add(root)
        if main_sessions(root):
            return root
    return _session_root_for(candidates[0])


def load_events(path: Path) -> list[dict]:
    if not path.is_file():
        die(f"session file not found: {path}")
    try:
        result = subprocess.run(["zstd", "-dc", str(path)], capture_output=True)
    except FileNotFoundError:
        die("zstd CLI not found; install zstd to extract DSH session logs")
    if result.returncode != 0:
        die(f"zstd -dc failed: {result.stderr.decode('utf-8', 'replace').strip()}")
    events: list[dict] = []
    for line in result.stdout.decode("utf-8", "replace").splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            events.append(json.loads(line))
        except json.JSONDecodeError:
            continue
    if not events or events[0].get("type") != "session":
        die(f"cannot parse session log (expected session header): {path}")
    return events


def content_text(content: list | None) -> str:
    return "\n".join(
        part.get("text", "")
        for part in (content or [])
        if isinstance(part, dict) and part.get("type") == "text"
    )


def masked_failure_line(text: str) -> tuple[str, str] | None:
    """在工具输出文本里找第一条失败证据，返回 (命中模式, 命中行) 或 None。

    只按行匹配有限模式（rc!=0 / FAILED / FAIL: / 行首 error:），不做语义判断：
    命令把非零退出码吞掉后 isError 仍为 false，这里补捞实红（#284 AC8）。
    调用方必须先用 `MASKED_FAILURE_TOOL_NAMES` 过滤工具类型——被 read/grep 出来的
    `FAILED` 文本是内容，不是本次调用的失败。
    """
    for line in text.splitlines():
        stripped = line.strip()
        if not stripped:
            continue
        for label, pattern in MASKED_FAILURE_RES:
            if pattern.search(line):
                return label, stripped[:160].replace("\n", " ")
    return None


def blocking_tool_kind(name: str, args: dict) -> str | None:
    """等待型/ask 阻塞工具的类别；两者都是工具执行的子集，只单列、不重复计入墙钟。

    `job_output` 只有 `wait=true` 才是阻塞等待；`wait=false` 是即时取结果，按普通工具计。
    """
    if name in ASK_TOOL_NAMES:
        return "ask"
    if name in WAIT_TOOL_NAMES:
        return "wait"
    if name == BLOCKING_JOB_TOOL and bool(args.get("wait")):
        return "wait"
    return None


def union_seconds(intervals: list[tuple[int, int]]) -> float:
    """区间并集时长（秒）；同一时刻的多个并行调用只计一次，不重复叠加。"""
    total = 0
    cur_start = cur_end = None
    for start, end in sorted(intervals):
        if end <= start:
            continue
        if cur_start is None:
            cur_start, cur_end = start, end
        elif start > cur_end:
            total += cur_end - cur_start
            cur_start, cur_end = start, end
        else:
            cur_end = max(cur_end, end)
    if cur_start is not None:
        total += cur_end - cur_start
    return total / 1000.0


def _seconds(value: float) -> str:
    return f"{value:.0f}s"


def _share(value: float, wall: float) -> str:
    return f"{value / wall * 100.0:.0f}%" if wall > 0 else "n/a"


def decompose_section(events: list[dict]) -> list[str]:
    """turn 内四分类墙钟 + 等待型/ask 子集单列（口径见模块 docstring）。

    每个事件只计一次：model_gen 每 step 只取该 step 内首个 `assistant/message`；
    tool_exec 按 callId 配对并取区间并集。ask/wait 只单列，不参与守恒等式。
    尾部未闭合 turn（当前会话）单列 model/tool 秒数并标注，不计入整会话合计。
    """
    turn_start: dict[int, int] = {}
    turn_end: dict[int, int] = {}
    step_start: dict[tuple[int, int], int] = {}
    step_model_ms: dict[tuple[int, int], int] = {}
    call_info: dict[str, tuple[str, dict, int, int]] = {}
    result_ts: dict[str, int] = {}
    last_turn: int | None = None
    skipped_events = 0

    for event in events:
        kind = event.get("type")
        data = event.get("data") or {}
        ts = event.get("time")
        if not isinstance(ts, (int, float)):
            # 首行 `session` 元数据没有 time，不是异常；其它类型才计入 warning。
            if kind != "session":
                skipped_events += 1
            continue
        if kind == "turn/start":
            turn = data.get("turn")
            if not isinstance(turn, int):
                skipped_events += 1
                continue
            last_turn = turn
            turn_start.setdefault(turn, int(ts))
        elif kind == "turn/end":
            turn = data.get("turn", last_turn)
            if not isinstance(turn, int):
                skipped_events += 1
                continue
            turn_end.setdefault(turn, int(ts))
        elif kind == "step/start":
            key = (data.get("turn"), data.get("step"))
            if not isinstance(key[0], int) or not isinstance(key[1], int):
                skipped_events += 1
                continue
            step_start.setdefault(key, int(ts))
        elif kind == "assistant/message":
            key = (data.get("turn"), data.get("step"))
            if key in step_start and key not in step_model_ms:
                step_model_ms[key] = int(ts) - step_start[key]
        elif kind == "tool/call":
            call_id = data.get("callId") or ""
            turn = data.get("turn", last_turn)
            if not call_id or not isinstance(turn, int):
                skipped_events += 1
                continue
            try:
                args = json.loads(data.get("arguments") or "{}")
            except json.JSONDecodeError:
                args = {}
            if not isinstance(args, dict):
                args = {}
            # 同一 callId 只认第一次 call（重投/重放不重复计时）；result 同理取第一条。
            call_info.setdefault(call_id, (data.get("name") or "?", args, int(ts), turn))
        elif kind == "tool/result":
            call_id = (data.get("message") or {}).get("toolCallId") or ""
            if call_id:
                # 同一 callId 可能有多条 result（重投/重放）；只认第一条——取最后一条会把
                # 区间拉到会话尾部，单票实测能把「工具执行 40 min」虚报成 106 min。
                result_ts.setdefault(call_id, int(ts))

    paired = {
        call_id: (name, args, start, turn)
        for call_id, (name, args, start, turn) in call_info.items()
        if call_id in result_ts
    }
    unmatched_calls = len(call_info) - len(paired)

    wait_counts: collections.Counter[str] = collections.Counter()
    wait_label_totals: collections.Counter[str] = collections.Counter()
    wait_sum_ms = 0
    ask_counts: collections.Counter[str] = collections.Counter()
    ask_sum_ms = 0

    rows: list[tuple] = []
    for turn, start in sorted(turn_start.items()):
        end = turn_end.get(turn)
        wall_s = (end - start) / 1000.0 if end is not None else None
        model_s = sum(ms for (turn_no, _step), ms in step_model_ms.items() if turn_no == turn) / 1000.0
        tool_intervals: list[tuple[int, int]] = []
        wait_turn_durations: list[int] = []
        ask_turn_durations: list[int] = []
        for call_id, (name, args, call_start, call_turn) in paired.items():
            if call_turn != turn:
                continue
            interval = (call_start, result_ts[call_id])
            tool_intervals.append(interval)
            kind = blocking_tool_kind(name, args)
            duration_ms = interval[1] - interval[0]
            if kind == "ask":
                ask_turn_durations.append(duration_ms)
                ask_counts[name] += 1
                ask_sum_ms += duration_ms
            elif kind == "wait":
                wait_turn_durations.append(duration_ms)
                wait_counts[name] += 1
                wait_label_totals[name] += duration_ms
                wait_sum_ms += duration_ms
        tool_s = union_seconds(tool_intervals)
        # ask / wait 是本 turn 内各调用自身阻塞时长的合计（语义是「等了多久」）；
        # 它们同时已包含在上面 tool_s 的区间并集里，所以只单列、绝不再加进守恒等式。
        ask_s = sum(ask_turn_durations) / 1000.0
        wait_s = sum(wait_turn_durations) / 1000.0
        idle_s = None if wall_s is None else max(0.0, wall_s - model_s - tool_s)
        rows.append((turn, wall_s, model_s, tool_s, idle_s, ask_s, len(wait_turn_durations), wait_s))

    closed = [row for row in rows if row[1] is not None]
    open_rows = [row for row in rows if row[1] is None]
    total_wall = sum(row[1] for row in closed)
    total_model = sum(row[2] for row in closed)
    total_tool = sum(row[3] for row in closed)
    total_idle = sum(row[4] for row in closed)
    clamped = sum(1 for row in closed if row[1] - row[2] - row[3] < 0)

    out: list[str] = ["## decompose (turn wall clock)", ""]
    out.append(
        "- rule: turn_wall = model_gen + tool_exec + idle; "
        "ask / wait-type tools are subsets of tool_exec and not counted twice"
    )
    out.append(
        f"- session: closed_turns={len(closed)} open_turns={len(open_rows)} "
        f"wall={_seconds(total_wall)} model={_seconds(total_model)}({_share(total_model, total_wall)}) "
        f"tool={_seconds(total_tool)}({_share(total_tool, total_wall)}) "
        f"idle={_seconds(total_idle)}({_share(total_idle, total_wall)})"
    )
    if clamped:
        out.append(f"- warning: {clamped} turn(s) had model+tool > wall; idle clamped to 0s")
    if unmatched_calls:
        out.append(f"- warning: {unmatched_calls} tool call(s) had no result and are not counted")
    if skipped_events:
        out.append(f"- warning: {skipped_events} event(s) lacked a usable timestamp/turn and were skipped")
    for turn, wall_s, model_s, tool_s, idle_s, ask_s, wait_n, wait_s in rows:
        if wall_s is None:
            out.append(
                f"- turn {turn}: open turn (no turn/end) model={_seconds(model_s)} "
                f"tool={_seconds(tool_s)} ask={_seconds(ask_s)} wait={wait_n}calls/{_seconds(wait_s)}"
            )
            continue
        out.append(
            f"- turn {turn}: wall={_seconds(wall_s)} "
            f"model={_seconds(model_s)}({_share(model_s, wall_s)}) "
            f"tool={_seconds(tool_s)}({_share(tool_s, wall_s)}) "
            f"idle={_seconds(idle_s)}({_share(idle_s, wall_s)}) "
            f"ask={_seconds(ask_s)} wait={wait_n}calls/{_seconds(wait_s)}"
        )
    out.append("")
    out.append("## wait-type tools (subset of tool_exec)")
    out.append("")
    if not wait_counts:
        out.append("- none")
    else:
        for name, count in wait_counts.most_common():
            out.append(f"- {name}: calls={count} total={_seconds(wait_label_totals[name] / 1000.0)}")
        out.append(f"- total: calls={sum(wait_counts.values())} total={_seconds(wait_sum_ms / 1000.0)}")
    out.append("")
    out.append("## ask blocks (subset of tool_exec)")
    out.append("")
    if not ask_counts:
        out.append("- none")
    else:
        for name, count in ask_counts.most_common():
            out.append(f"- {name}: calls={count} total={_seconds(ask_sum_ms / 1000.0)}")
    out.append("")
    return out


def extract(path: Path, decompose: bool = False) -> str:
    events = load_events(path)
    tools: collections.Counter[str] = collections.Counter()
    calls: dict[str, tuple[str, str, int]] = {}
    human: list[tuple[int, str]] = []
    failures: list[tuple[int, str, str, str]] = []
    masked_failures: list[tuple[int, str, str, str, str]] = []
    long_jobs: list[tuple[int, str]] = []
    approvals: list[tuple[int, str, str]] = []
    subagents: list[tuple[int, str, str]] = []
    retries: collections.Counter[str] = collections.Counter()
    turns: list[list] = []
    patterns: collections.Counter[str] = collections.Counter()

    for event in events:
        kind = event.get("type")
        data = event.get("data") or {}
        ts = event.get("time", 0)
        if kind == "tool/call":
            name = data.get("name", "?")
            tools[name] += 1
            try:
                args = json.loads(data.get("arguments") or "{}")
            except json.JSONDecodeError:
                args = {}
            command = args.get("command") or args.get("description") or args.get("prompt") or ""
            calls[data.get("callId", "")] = (name, command, ts)
            if name in SUBAGENT_TOOLS:
                subagents.append((ts, name, (args.get("description") or "")[:80]))
            lowered = command.lower()
            for key, needle in (
                ("gate", "gate.sh"),
                ("preflight", "preflight"),
                ("wait-ci", "wait-ci"),
                ("git push", "git push"),
                ("gh pr", "gh pr"),
                ("gh issue", "gh issue"),
                ("git worktree", "git worktree"),
            ):
                if needle in lowered:
                    patterns[key] += 1
        elif kind == "tool/result":
            message = data.get("message") or {}
            call_id = message.get("toolCallId", "")
            output = content_text(message.get("content"))
            if message.get("isError"):
                name, command, cts = calls.get(call_id, ("?", "", ts))
                failures.append(
                    (
                        cts,
                        name,
                        command[:220].replace("\n", " "),
                        output[:160].replace("\n", " "),
                    )
                )
            elif len(masked_failures) < MASKED_FAILURE_LIMIT:
                # isError 为 false 但输出里有失败证据（自掩码实红）；isError 的结果
                # 已进 failures，不重复记。只对命令类工具生效（见 MASKED_FAILURE_* 注释）。
                name, command, cts = calls.get(call_id, ("?", "", ts))
                hit = (
                    masked_failure_line(output)
                    if name in MASKED_FAILURE_TOOL_NAMES
                    else None
                )
                if hit is not None:
                    pattern, line = hit
                    masked_failures.append(
                        (cts, name, command[:160].replace("\n", " "), pattern, line)
                    )
        elif kind == "user/message":
            if (data.get("source") or {}).get("kind") == "user":
                human.append((ts, content_text(data.get("content"))[:300].replace("\n", " ")))
        elif kind == "job/settled":
            duration = (data.get("finishedAt") or 0) - (data.get("startedAt") or 0)
            if duration >= LONG_JOB_SEC * 1000:
                long_jobs.append((duration, data.get("label", "")[:160].replace("\n", " ")))
        elif kind == "approval/asked":
            approvals.append((ts, data.get("toolName", "?"), (data.get("reason") or "")[:160]))
        elif kind == "llm/retry":
            retries[(data.get("failure") or {}).get("code", "?")] += 1
        elif kind == "turn/start":
            turns.append([data.get("turn"), ts, None])
        elif kind == "turn/end" and turns:
            turns[-1][2] = ts

    out: list[str] = [f"# retro digest: {path.parent.name}", ""]
    out.append(
        f"- events={len(events)} tool_calls={sum(tools.values())} human_msgs={len(human)} "
        f"turns={len(turns)} approvals={len(approvals)} llm_retry={sum(retries.values())}"
    )
    out.append(f"- tools: {dict(tools.most_common())}")
    out.append(f"- command patterns: {dict(patterns)}")
    out.append(f"- retries: {dict(retries)}")
    out.append("")
    out.append("## turns")
    out.append("")
    for number, start, end in turns:
        wall = (end - start) / 1000.0 if (start and end) else None
        out.append(f"- turn {number}: start={start} wall={wall:.0f}s" if wall else f"- turn {number}: start={start}")
    out.append("")
    out.append("## human messages")
    out.append("")
    for ts, text in human:
        out.append(f"- {ts}: {text}")
    out.append("")
    out.append("## approvals")
    out.append("")
    for ts, tool, reason in approvals:
        out.append(f"- {ts} [{tool}] {reason}")
    out.append("")
    out.append("## failures (isError tool results)")
    out.append("")
    for ts, name, command, error in failures:
        out.append(f"- {ts} [{name}] cmd={command}")
        out.append(f"  err={error}")
    out.append("")
    out.append("## masked failures (rc!=0 / FAIL in tool output)")
    out.append("")
    for ts, name, command, pattern, line in masked_failures:
        out.append(f"- {ts} [{name}] cmd={command} (match={pattern})")
        out.append(f"  hit={line}")
    out.append("")
    out.append("## long jobs (>=60s)")
    out.append("")
    for duration, label in sorted(long_jobs, reverse=True):
        out.append(f"- {duration / 1000.0:.0f}s {label}")
    out.append("")
    out.append("## subagent calls")
    out.append("")
    for ts, name, description in subagents:
        out.append(f"- {ts} [{name}] {description}")
    out.append("")
    if decompose:
        # 只追加小节，不改变上面任何既有小节的顺序与格式（#309 AC9）。
        out.extend(decompose_section(events))
    return "\n".join(out) + "\n"


def write_digest(path: Path, out_dir: Path, decompose: bool = False) -> Path:
    label = path.parent.name
    out_dir.mkdir(parents=True, exist_ok=True)
    target = out_dir / f"{label}-digest.md"
    target.write_text(extract(path, decompose=decompose), encoding="utf-8")
    print(target)
    return target


def main() -> int:
    parser = argparse.ArgumentParser(description="DSH 会话日志 → 复盘 digest（只读）")
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--session", type=Path, help="指定 session.v4.jsonl.zstd 路径")
    source.add_argument("--latest", action="store_true", help="本 cwd 最新主会话")
    source.add_argument("--all", action="store_true", help="本 cwd 最近 N 个主会话")
    parser.add_argument("--recent", type=int, default=5, help="--all 时处理最近几个（默认 5）")
    parser.add_argument("--out", type=Path, default=Path(".cache/retro"), help="输出目录（默认 .cache/retro）")
    parser.add_argument(
        "--decompose",
        action="store_true",
        help="追加 turn 内墙钟分解（模型生成 / 工具执行 / idle，ask 与等待型工具单列）",
    )
    args = parser.parse_args()

    if args.session:
        write_digest(args.session, args.out, decompose=args.decompose)
        return 0

    paths = main_sessions(sessions_root())
    if not paths:
        die(f"no DSH sessions under {sessions_root()}")
    for path in paths[: (1 if args.latest else args.recent)]:
        write_digest(path, args.out, decompose=args.decompose)
    return 0


if __name__ == "__main__":
    sys.exit(main())
