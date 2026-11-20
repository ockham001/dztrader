#!/usr/bin/env python3
"""check_ledger.py — 仓库台账体检（只读，不修改任何东西）。

检查五类问题，供波末 retro / spec 收口前使用：

1. **spec 母票可收口**：open 且标题以 `[spec]` 开头、原生子票全部 completed
   （子票全关后母票必须单独收口，见 docs/agents/ticket-closeout.md §6「spec 母票收口」）。
2. **悬空承诺**：活跃文档里出现「后续票 / 待实现 / 由 #N 提供 …」类承诺，但该 issue 已关闭
   ——文档仍在承诺一件已关票的事。
3. **needs-triage 超期**：open 且带 `needs-triage`、创建超过 `--days` 天（默认 3，warning 级）。
4. **ready-for-agent 缺拍板记录**：open 且带 `ready-for-agent` 的票正文没有 `## 拍板记录`
   段、或该段为空（warning 级；`--strict` 升 error）。口径见
   docs/agents/triage-labels.md「拍板闭环」——`ready-for-agent` 的前置就是「spec 无未拍板项，
   决策已写进 issue 正文」。理由：#294–#298 波五票正文全部缺该段，于是每票退回
   「PR 创建后提问」，合并授权阻塞合计 403 min（#309 AC1）。
5. **最近关闭的票仍有未勾选 AC**（R6，warning 级）：`- [ ]` 残留说明「验收标准未逐条确认
   即关闭」，spec 收口审计时无法从正文判断达成情况。带 `⏳` 的未勾项视为已确认的
   人工验证项，不计。理由：#392/#393/#394 三票 24 条 AC 全部未勾即关闭，事后只能靠
   人肉读关闭评论复原结论。

用法:
  python3 scripts/check_ledger.py              # 只报告，退出码 0
  python3 scripts/check_ledger.py --strict     # 有 error 级 finding 时退出 1
  python3 scripts/check_ledger.py --days 7     # needs-triage 超期阈值
  python3 scripts/check_ledger.py --ac-window-days 14   # R6 关闭票回溯窗口
  python3 scripts/check_ledger.py --selftest   # 只跑承诺正则 + R4 三态 + R6 计数自测（不联网）

退出码: 0=无 error（或非 strict）, 1=error 级 finding（--strict）, 2=环境错误（gh / git 不可用）

注意：本脚本依赖 `gh` 登录态与网络；不接入 CI，由人/agent 在波末与 spec 收口时运行。
脚本会先 chdir 到 git 仓库根，子目录内运行也扫全量文档。
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
from datetime import datetime, timedelta, timezone
from pathlib import Path

# 只扫可能承诺「后续工作」的活跃文档；ADR 是历史决策记录（引用已关闭的后续票属正常），不在此列。
DOC_ROOTS = [
    "docs/frame_contracts",
    "docs/components",
    "docs/development-checklist.md",
    "docs/unit-test-standard.md",
    "README.md",
    "GLOSSARY.md",
]

# 同一行内同时出现「承诺语气」与 issue 号，才算悬空承诺候选；号码是否已关闭再查 gh。
PROMISE_RES = [
    re.compile(r"(?:后续票|后续由|待实现|尚未实现|未实现|留待|将由|计划)[^\n#]{0,24}(?:#|issues/)(\d+)"),
    re.compile(r"由\s*(?:issue\s*)?#?(\d+)\s*(?:提供|承接|跟踪)"),
    re.compile(r"待\s*#(\d+)\s*(?:落地|实现|补|处理|跟进)"),
]

SELFTEST_MATCH = [
    "后续票 #103 提供映射",
    "由 #103 提供",
    "由 issue #103 提供",
    "待 #103 落地",
    "计划 #103 实现",
    "后续由 issues/103 提供",
    "留待 #103 处理",
]
SELFTEST_NO_MATCH = [
    "由 #120 收口",
    "由 #123 落地（已完成）",
    "#103 关闭为 not planned",
    "历史实现 #103 已完成",
]

# R4：ready-for-agent 票的正文必须带非空的「## 拍板记录」段（#309 AC1）。
# 口径来源 docs/agents/triage-labels.md「拍板闭环」：ready-for-agent 的前置就是
# 「spec 无未拍板项，决策已写进 issue 正文」——缺该段就是「还没拍板就交给 agent」。
# 段标题只认 `## 拍板记录`（可带全角/半角括号后缀，如「（2026-11-07）」）；`###` 三级
# 标题与别的标题（`## 拍板记录结论`）不算本段，因为回填格式与 grep 口径都是二级标题。
DECISION_HEADING = re.compile(r"^##\s+拍板记录\s*(?:（[^）]*）|\([^)]*\))?\s*$")
# 段内容在下一个一/二级标题处终止——按 Markdown 语义，`###` 及更深的小节标题是**本段内部**
# 的结构，不能把一个先写「### 合并授权」再写正文的票判成空段（#309 审查 finding：误报）。
SECTION_END = re.compile(r"^#{1,2}(?!#)")

SELFTEST_LEDGER_HAS = [
    "## 拍板记录\n\n- **合并授权**：CI 绿 + 双轴审查通过即合并关单\n",
    # 先写 `###` 子标题再写正文：`###` 是本段内部结构，不能判成空段（#309 审查 finding）。
    "## 拍板记录\n\n### 合并授权\n\n- 条件式授权：CI 绿即合并\n",
    "# 背景\n\n正文\n\n## 拍板记录\n\n决策：按波一次授权\n\n## 其它\n\n- 内容\n",
    "## 拍板记录\n\n> 引用行不计，但下面有正文\n\n- 授权\n",
    "## 拍板记录（2026-11-07）\n\n- 授权\n",
    "## 背景\n\n## 拍板记录\n\n- 授权\n### 子标题\n\n- 子项\n",
    "## 拍板记录\n\n#### 细节\n\n- 授权\n",
]
SELFTEST_LEDGER_MISSING = [
    "",
    "# 背景\n\n正文，没有任何拍板记录段\n",
    "### 拍板记录\n\n- 三级标题不算本段\n",
    "## 拍板记录结论\n\n- 这不是同一个段\n",
]
SELFTEST_LEDGER_EMPTY = [
    "## 拍板记录\n",
    "## 拍板记录\n\n\n## 其它\n\n- 内容\n",
    "## 拍板记录\n\n> 只有引用行\n> 没有正文\n\n## 其它\n",
    "## 拍板记录\n\n   \n\t\n",
    "## 拍板记录\n\n## 其它\n\n- 内容\n",
]

# R6：闭票前 AC 勾选。口径来源 docs/agents/ticket-closeout.md §6「关闭前勾选验收标准」：
# 关闭评论能证明「交付/验证/审查」，但**正文的 `- [ ]` 复选框**是 spec 收口审计时判断
# 「这条验收标准是否确认达成」的唯一机器可读信号。本波 #392/#393/#394 三票 24 条 AC
# 全部未勾即关闭，事后审计只能靠人肉读关闭评论复原结论——本规则把这种疏漏变成机械可见。
# 未勾但带 `⏳` 的项视为「已确认为人工验证项」，不计入（如实标注优于虚假勾选）。
SELFTEST_TICKS = [
    ("", 0),
    ("- [x] 已达成\n- [x] 也达成\n", 0),
    ("- [ ] 忘了勾\n", 1),
    ("- [ ] 忘了勾一\n- [ ] 忘了勾二\n- [x] 已勾\n", 2),
    # ⏳ 标注的未勾项不计：这是「确认过、需真机验证」，不是疏漏
    ("- [ ] ⏳ **待人工验证**：RTN 上报两条\n", 0),
    ("- [ ] ⏳ 待验证\n- [ ] 真忘了\n", 1),
    # 非列表项/缩进内的方括号不误报（正文行内的 `- [ ]` 才是 AC）
    ("说明：`- [ ]` 是未勾选语法\n", 0),
    ("* [ ] 星号列表也要认\n", 1),
]


def fail(msg: str) -> None:
    print(f"error: {msg}", file=sys.stderr)
    sys.exit(2)


def run(cmd: list[str]) -> subprocess.CompletedProcess[str]:
    try:
        return subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8")
    except OSError as exc:
        fail(f"cannot run {cmd[0]}: {exc}")


def chdir_repo_root() -> Path:
    result = run(["git", "rev-parse", "--show-toplevel"])
    if result.returncode != 0:
        fail(f"not a git repository: {result.stderr.strip()}")
    root = Path(result.stdout.strip())
    os.chdir(root)
    return root


def repo_owner_name() -> tuple[str, str]:
    result = run(["gh", "repo", "view", "--json", "nameWithOwner", "--jq", ".nameWithOwner"])
    if result.returncode != 0 or "/" not in result.stdout:
        fail(f"cannot resolve repository via gh repo view: {result.stderr.strip()}")
    owner, name = result.stdout.strip().split("/", 1)
    return owner, name


def fetch_open_issues(owner: str, name: str) -> list[dict]:
    """分页取全部 open issue（含正文、子票完成度与标签）。

    `body` 是 #309 AC1 加的：R4 要判断「## 拍板记录」段是否存在/为空，只能从正文看。
    每页仍是 first:100，分页口径不变（每页请求量增加正文文本，但 totalCount 与
    rate limit 的消耗按请求次数算，页数不变）。
    """
    nodes: list[dict] = []
    cursor: str | None = None
    while True:
        after = f', after: "{cursor}"' if cursor else ""
        query = (
            '{repository(owner:"%s",name:"%s"){issues(states:OPEN,first:100%s){'
            "pageInfo{hasNextPage endCursor} nodes{"
            "number title body createdAt labels(first:50){nodes{name}}"
            "subIssuesSummary{total completed}}}}}"
        ) % (owner, name, after)
        result = run(["gh", "api", "graphql", "-f", f"query={query}"])
        if result.returncode != 0:
            fail(f"gh api graphql failed: {result.stderr.strip()}")
        try:
            payload = json.loads(result.stdout)["data"]["repository"]["issues"]
        except (KeyError, json.JSONDecodeError) as exc:
            fail(f"unexpected graphql response: {exc}")
        nodes.extend(payload["nodes"])
        if not payload["pageInfo"]["hasNextPage"]:
            return nodes
        cursor = payload["pageInfo"]["endCursor"]


def fetch_recently_closed_issues(owner: str, name: str) -> list[dict]:
    """取**最近关闭**的 issue（R6 用）。

    不扫全部已关闭票（本仓 189 张且持续增长）：关闭前该勾的 AC 只在收口那一刻有意义，
    历史票的复选框状态早已被审计过，重扫只会制造噪音。窗口由 --ac-window-days 控制。
    """
    query = (
        '{repository(owner:"%s",name:"%s"){'
        "issues(states:CLOSED,first:100,orderBy:{field:UPDATED_AT,direction:DESC}){"
        "nodes{number title body closedAt}}}}"
    ) % (owner, name)
    result = run(["gh", "api", "graphql", "-f", f"query={query}"])
    if result.returncode != 0:
        fail(f"gh api graphql failed: {result.stderr.strip()}")
    try:
        return json.loads(result.stdout)["data"]["repository"]["issues"]["nodes"]
    except (KeyError, json.JSONDecodeError) as exc:
        fail(f"unexpected graphql response: {exc}")
    return []


def issue_state(number: int, cache: dict[int, str | None]) -> str | None:
    if number not in cache:
        result = run(["gh", "issue", "view", str(number), "--json", "state", "--jq", ".state"])
        if result.returncode != 0:
            fail(f"cannot query issue #{number} state: {result.stderr.strip()}")
        cache[number] = result.stdout.strip()
    return cache[number]


def doc_files() -> list[str]:
    """活跃文档清单：已跟踪 + 未跟踪（未被 gitignore）都要扫。

    不能只列索引里的文件：台账体检按收尾流程在提交之前跑，新建的 md 在 `git add`
    之前对 `git ls-files`（默认 = 索引）完全隐形，悬空承诺检查会漏扫这些新文档
    （#281；同形问题见 preflight 的 generated.ts 次序陷阱）。
    """
    result = run(["git", "ls-files", "--cached", "--others", "--exclude-standard", *DOC_ROOTS])
    if result.returncode != 0:
        fail(f"git ls-files failed: {result.stderr.strip()}")
    return [line for line in result.stdout.splitlines() if line.endswith(".md")]


def promise_refs(line: str) -> set[int]:
    refs: set[int] = set()
    for pattern in PROMISE_RES:
        for match in pattern.finditer(line):
            refs.add(int(match.group(1)))
    return refs


def decision_ledger_state(body: str) -> str:
    """R4 判定（纯函数，不联网）：返回 'ok' / 'missing' / 'empty'。

    - `missing`：正文没有 `## 拍板记录` 二级标题（回填格式与 grep 口径都是二级标题）
    - `empty`：标题存在，但到下一个一/二/三级标题之前没有任何实质内容（空白行与
      blockquote 行 `>` 开头的行不算内容，代码围栏里的行算内容）
    - `ok`：段内有实质内容
    """
    lines = body.splitlines()
    start = next((i for i, line in enumerate(lines) if DECISION_HEADING.match(line)), None)
    if start is None:
        return "missing"
    in_fence = False
    for line in lines[start + 1:]:
        stripped = line.strip()
        if stripped.startswith(("```", "~~~")):
            in_fence = not in_fence
        elif not in_fence and SECTION_END.match(line):
            break
        elif stripped and not stripped.startswith(">"):
            return "ok"
    return "empty"


def unticked_acceptance_items(body: str) -> int:
    """R6 判定（纯函数，不联网）：返回正文里**未勾选且未标注待人工验证**的 AC 条数。

    报的是数量而非布尔，因为收口评论里要能说清「还剩几条没勾」。

    只数 `- [ ]` 列表项；`- [x]` 不计。**带 `⏳` 的未勾选项不计**——它是「已确认范围但
    需真机/人工验证」的显式标注（如实标注优于虚假勾选），与「忘了勾」必须机器可区分，
    否则规则会逼着人把人工验证项勾成已完成。
    """
    count = 0
    for line in body.splitlines():
        stripped = line.strip()
        if not stripped.startswith(("- [ ]", "* [ ]")):
            continue
        if "⏳" in stripped:
            continue
        count += 1
    return count


def selftest() -> None:
    for body, expected in SELFTEST_TICKS:
        actual = unticked_acceptance_items(body)
        if actual != expected:
            raise SystemExit(
                f"selftest failed: expected {expected} unticked for {body!r}, got {actual}")
    for line in SELFTEST_MATCH:
        if not promise_refs(line):
            raise SystemExit(f"selftest failed: expected promise match: {line}")
    for line in SELFTEST_NO_MATCH:
        if promise_refs(line):
            raise SystemExit(f"selftest failed: unexpected promise match: {line}")
    for body in SELFTEST_LEDGER_HAS:
        if decision_ledger_state(body) != "ok":
            raise SystemExit(f"selftest failed: expected ledger section ok: {body!r}")
    for body in SELFTEST_LEDGER_MISSING:
        if decision_ledger_state(body) != "missing":
            raise SystemExit(f"selftest failed: expected missing ledger section: {body!r}")
    for body in SELFTEST_LEDGER_EMPTY:
        if decision_ledger_state(body) != "empty":
            raise SystemExit(f"selftest failed: expected empty ledger section: {body!r}")
    print(f"selftest OK: {len(SELFTEST_MATCH)} match / {len(SELFTEST_NO_MATCH)} non-match cases")
    print(f"selftest OK: R4 {len(SELFTEST_LEDGER_HAS)} has-section / "
          f"{len(SELFTEST_LEDGER_MISSING)} missing / {len(SELFTEST_LEDGER_EMPTY)} empty-section cases")
    print(f"selftest OK: R6 {len(SELFTEST_TICKS)} AC 勾选计数用例")


def dangling_promises() -> list[tuple[str, int, int, str]]:
    findings: list[tuple[str, int, int, str]] = []
    states: dict[int, str | None] = {}
    for rel in doc_files():
        try:
            lines = Path(rel).read_text(encoding="utf-8").splitlines()
        except OSError:
            continue
        for lineno, line in enumerate(lines, 1):
            for number in sorted(promise_refs(line)):
                if issue_state(number, states) == "CLOSED":
                    findings.append((rel, lineno, number, line.strip()))
    return findings


def main() -> int:
    parser = argparse.ArgumentParser(description="仓库台账体检（只读）")
    parser.add_argument("--strict", action="store_true", help="有 error 级 finding 时退出 1")
    parser.add_argument("--days", type=int, default=3, help="needs-triage 超期阈值（天，默认 3）")
    parser.add_argument("--ac-window-days", type=int, default=7,
                        help="R6 只检查最近 N 天内关闭的票（默认 7；0 = 只查今天）")
    parser.add_argument("--selftest", action="store_true",
                        help="只跑承诺正则 + R4 拍板记录三态自测（不联网）")
    args = parser.parse_args()

    if args.selftest:
        selftest()
        return 0

    chdir_repo_root()
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
        if age > args.days:
            stale.append((it["number"], it["title"], age))

    # R4（#309 AC1）：ready-for-agent 是「spec 无未拍板项」的承诺，缺「## 拍板记录」段
    # 说明决策从未落进 issue 正文，实现会话只能退回「PR 创建后提问」。
    missing_ledger = []
    for it in issues:
        labels = {node["name"] for node in it["labels"]["nodes"]}
        if "ready-for-agent" not in labels:
            continue
        state = decision_ledger_state(it.get("body") or "")
        if state != "ok":
            missing_ledger.append((it["number"], it["title"], state))

    dangling = dangling_promises()
    docs = doc_files()

    # R6：最近关闭的票里，正文仍残留未勾选的 AC（⏳ 标注的人工验证项除外）。
    # 本波 #392/#393/#394 三票 24 条 AC 未勾即关闭，事后只能靠人肉读关闭评论复原。
    unticked = []
    cutoff = now - timedelta(days=args.ac_window_days)
    for it in fetch_recently_closed_issues(owner, name):
        closed_at = it.get("closedAt")
        if not closed_at:
            continue
        if datetime.fromisoformat(closed_at.replace("Z", "+00:00")) < cutoff:
            continue
        count = unticked_acceptance_items(it.get("body") or "")
        if count:
            unticked.append((it["number"], it["title"], count))

    errors = len(closable) + len(dangling)
    warnings = len(stale) + len(missing_ledger) + len(unticked)
    if args.strict:
        # R4 在默认模式下是 warning（口径同 needs-triage 超期），--strict 升 error。
        errors += len(missing_ledger)
        warnings -= len(missing_ledger)

    print(f"== 台账体检（{now.astimezone().strftime('%Y-%m-%d %H:%M %Z')}）==")
    print(f"open issues: {len(issues)}；扫描文档: {len(docs)}")

    print(f"\n[ERROR] spec 母票可收口（{len(closable)}）")
    for number, title, total in closable:
        print(f"  - #{number}（{total}/{total} 子票完成）{title}")

    print(f"\n[ERROR] 悬空承诺：活跃文档引用已关闭 issue 的「后续票/待实现/由 #N 提供」（{len(dangling)}）")
    for rel, lineno, number, line in dangling:
        print(f"  - {rel}:{lineno} → #{number} 已关闭：{line[:120]}")

    print(f"\n[WARN] needs-triage 超期 > {args.days} 天（{len(stale)}）")
    for number, title, age in stale:
        print(f"  - #{number}（{age} 天）{title}")

    # 级别随模式切换：`--strict` 下本项已计入 error，标题就不能再挂 `[WARN]`
    # （gate.sh ledger 默认 --strict，否则门禁日志会出现「error 项标 WARN」的误导）。
    level = "ERROR" if args.strict else "WARN"
    print(f"\n[{level}] ready-for-agent 缺拍板记录（{len(missing_ledger)}）")
    for number, title, state in missing_ledger:
        detail = "无「## 拍板记录」段" if state == "missing" else "「## 拍板记录」段为空"
        print(f"  - #{number}（{title}）{detail}")

    print(f"\n[WARN] 最近 {args.ac_window_days} 天关闭的票仍有未勾选 AC（{len(unticked)}）")
    for number, title, count in unticked:
        print(f"  - #{number}（{count} 条未勾）{title}")

    verdict = "FAIL" if errors else "PASS"
    print(f"\n== 结果：{verdict}（{errors} error, {warnings} warning）==")
    if errors:
        print("提示：spec 母票按 ticket-closeout §6 收口；悬空承诺改文档或重开票；")
    if unticked:
        print("提示：关闭票前逐条勾选 AC（`- [x]`）；确实需人工/真机验证的项标 `- [ ] ⏳`，"
              "规则会跳过它（ticket-closeout §6「关闭前勾选验收标准」）")
    if missing_ledger:
        print("提示：ready-for-agent 票在 issue 正文补「## 拍板记录」段（含条件式合并授权），"
              "回填后本项归零（triage-labels.md「拍板闭环」）")
    return 1 if (args.strict and errors) else 0


if __name__ == "__main__":
    sys.exit(main())
