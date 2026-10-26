#!/usr/bin/env python3
"""retro_extract.py — 把 DSH 会话日志提取成复盘 digest（只读）。

DSH 会话日志：`~/.dsh/sessions/--<cwd 转义>--/<session>/session.v4.jsonl.zstd`
（zstd 压缩 JSONL；详见 docs/agents/retro-evidence.md）。

产出 `.cache/retro/<session-id>-digest.md`，含：轮次墙钟、人工消息、审批、工具失败、
>=60s 长作业、子代理调用、工具计数、LLM 重试——按复盘候选归类前的原始证据。

用法:
  python3 scripts/retro_extract.py --latest                # 本 cwd 最新主会话
  python3 scripts/retro_extract.py --session <path.zstd>   # 指定会话文件
  python3 scripts/retro_extract.py --all --recent 5        # 最近 5 个主会话逐个出 digest
  python3 scripts/retro_extract.py --latest --out .cache/retro

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
_SAFE_CHAR_RE = re.compile(r"^[A-Za-z0-9._-]$")


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


def extract(path: Path) -> str:
    events = load_events(path)
    tools: collections.Counter[str] = collections.Counter()
    calls: dict[str, tuple[str, str, int]] = {}
    human: list[tuple[int, str]] = []
    failures: list[tuple[int, str, str, str]] = []
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
            if message.get("isError"):
                call_id = message.get("toolCallId", "")
                name, command, cts = calls.get(call_id, ("?", "", ts))
                failures.append(
                    (
                        cts,
                        name,
                        command[:220].replace("\n", " "),
                        content_text(message.get("content"))[:160].replace("\n", " "),
                    )
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
    return "\n".join(out) + "\n"


def write_digest(path: Path, out_dir: Path) -> Path:
    label = path.parent.name
    out_dir.mkdir(parents=True, exist_ok=True)
    target = out_dir / f"{label}-digest.md"
    target.write_text(extract(path), encoding="utf-8")
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
    args = parser.parse_args()

    if args.session:
        write_digest(args.session, args.out)
        return 0

    paths = main_sessions(sessions_root())
    if not paths:
        die(f"no DSH sessions under {sessions_root()}")
    for path in paths[: (1 if args.latest else args.recent)]:
        write_digest(path, args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
