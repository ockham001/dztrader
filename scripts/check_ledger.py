#!/usr/bin/env python3
"""check_ledger.py — 仓库台账体检（只读，不修改任何东西）。

检查三类问题，供波末 retro / spec 收口前使用：

1. **spec 母票可收口**：open 且标题以 `[spec]` 开头、原生子票全部 completed
   （子票全关后母票必须单独收口，见 docs/agents/ticket-closeout.md §6「spec 母票收口」）。
2. **悬空承诺**：当前真相源文档（`docs/frame_contracts/`、`docs/components/`、README、GLOSSARY）
   里出现「后续票 / 待实现 … #N」，但该 issue 已关闭——契约仍在承诺一件已关票的事。
3. **needs-triage 超期**：open 且带 `needs-triage`、创建超过 `--days` 天（默认 3，warning 级）。

用法:
  python3 scripts/check_ledger.py              # 只报告，退出码 0
  python3 scripts/check_ledger.py --strict     # 有 error 级 finding 时退出 1
  python3 scripts/check_ledger.py --days 7     # needs-triage 超期阈值

退出码: 0=无 error（或非 strict）, 1=error 级 finding（--strict）, 2=环境错误（gh / git 不可用）

注意：本脚本依赖 `gh` 登录态与网络；不接入 CI，由人/agent 在波末与 spec 收口时运行。
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

# 只扫「当前真相源」文档；ADR 是历史决策记录（可能引用已关闭的后续票），不在此列。
DOC_ROOTS = ["docs/frame_contracts", "docs/components", "README.md", "GLOSSARY.md"]
PROMISE_RE = re.compile(r"(后续票|待实现|尚未实现|未实现)")
ISSUE_REF_RE = re.compile(r"#(\d+)")


def run(cmd: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(cmd, capture_output=True, text=True)


def fail(msg: str) -> None:
    print(f"error: {msg}", file=sys.stderr)
    sys.exit(2)


def repo_owner_name() -> tuple[str, str]:
    result = run(["gh", "repo", "view", "--json", "nameWithOwner", "--jq", ".nameWithOwner"])
    if result.returncode != 0 or "/" not in result.stdout:
        fail(f"cannot resolve repository via gh repo view: {result.stderr.strip()}")
    owner, name = result.stdout.strip().split("/", 1)
    return owner, name


def fetch_open_issues(owner: str, name: str) -> list[dict]:
    query = (
        '{repository(owner:"%s",name:"%s"){issues(states:OPEN,first:100){nodes{'
        "number title createdAt labels(first:20){nodes{name}}"
        "subIssuesSummary{total completed}}}}}"
    ) % (owner, name)
    result = run(["gh", "api", "graphql", "-f", f"query={query}"])
    if result.returncode != 0:
        fail(f"gh api graphql failed: {result.stderr.strip()}")
    try:
        return json.loads(result.stdout)["data"]["repository"]["issues"]["nodes"]
    except (KeyError, json.JSONDecodeError) as exc:
        fail(f"unexpected graphql response: {exc}")


def issue_state(number: int, cache: dict[int, str | None]) -> str | None:
    if number not in cache:
        result = run(["gh", "issue", "view", str(number), "--json", "state", "--jq", ".state"])
        cache[number] = result.stdout.strip() if result.returncode == 0 else None
    return cache[number]


def tracked_doc_files() -> list[str]:
    result = run(["git", "ls-files", *DOC_ROOTS])
    if result.returncode != 0:
        fail(f"git ls-files failed: {result.stderr.strip()}")
    return [line for line in result.stdout.splitlines() if line.endswith(".md")]


def dangling_promises() -> list[tuple[str, int, int, str]]:
    findings: list[tuple[str, int, int, str]] = []
    states: dict[int, str | None] = {}
    for rel in tracked_doc_files():
        try:
            lines = Path(rel).read_text(encoding="utf-8").splitlines()
        except OSError:
            continue
        for lineno, line in enumerate(lines, 1):
            if not PROMISE_RE.search(line):
                continue
            for match in ISSUE_REF_RE.finditer(line):
                number = int(match.group(1))
                if issue_state(number, states) == "CLOSED":
                    findings.append((rel, lineno, number, line.strip()))
    return findings


def main() -> int:
    parser = argparse.ArgumentParser(description="仓库台账体检（只读）")
    parser.add_argument("--strict", action="store_true", help="有 error 级 finding 时退出 1")
    parser.add_argument("--days", type=int, default=3, help="needs-triage 超期阈值（天，默认 3）")
    args = parser.parse_args()

    owner, name = repo_owner_name()
    issues = fetch_open_issues(owner, name)
    now = datetime.now(timezone.utc)

    closable = [
        (it["number"], it["title"], it["subIssuesSummary"]["total"])
        for it in issues
        if it["title"].startswith("[spec]")
        and it["subIssuesSummary"]["total"] > 0
        and it["subIssuesSummary"]["completed"] == it["subIssuesSummary"]["total"]
    ]

    stale = []
    for it in issues:
        labels = {node["name"] for node in it["labels"]["nodes"]}
        if "needs-triage" not in labels:
            continue
        created = datetime.fromisoformat(it["createdAt"].replace("Z", "+00:00"))
        age = (now - created).days
        if age >= args.days:
            stale.append((it["number"], it["title"], age))

    dangling = dangling_promises()

    errors = len(closable) + len(dangling)
    warnings = len(stale)

    print(f"== 台账体检（{now.astimezone().strftime('%Y-%m-%d %H:%M %Z')}）==")
    print(f"open issues: {len(issues)}；扫描文档: {len(tracked_doc_files())}")

    print(f"\n[ERROR] spec 母票可收口（{len(closable)}）")
    for number, title, total in closable:
        print(f"  - #{number}（{total}/{total} 子票完成）{title}")

    print(f"\n[ERROR] 悬空承诺：docs 引用已关闭 issue 的「后续票/待实现」（{len(dangling)}）")
    for rel, lineno, number, line in dangling:
        print(f"  - {rel}:{lineno} → #{number} 已关闭：{line[:120]}")

    print(f"\n[WARN] needs-triage 超期 > {args.days} 天（{len(stale)}）")
    for number, title, age in stale:
        print(f"  - #{number}（{age} 天）{title}")

    verdict = "FAIL" if errors else "PASS"
    print(f"\n== 结果：{verdict}（{errors} error, {warnings} warning）==")
    if errors:
        print("提示：spec 母票按 ticket-closeout §6 收口；悬空承诺改文档或重开票；")
    return 1 if (args.strict and errors) else 0


if __name__ == "__main__":
    sys.exit(main())
