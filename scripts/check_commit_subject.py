#!/usr/bin/env python3
"""check_commit_subject.py — 提交标题（subject）形态校验：前缀 + 票号 + GitHub auto-close 禁用词

为什么需要（#265，复盘 #246/#258/#260）：两条纪律在提交标题上正面冲突，冲突的代价是
**关闭评论被静默丢弃**。

    check-push-scope.sh 要求区间内每个非 merge commit 的 subject 含 `#<issue>`
    GitHub 把 `close`/`fix`/`resolve` + `#N` 当 closing keyword 解析

于是 `fix: #246 第 1 轮审查收敛` 这种完全合规的标题，在 PR 合并的一瞬间由 GitHub 关掉
#246；紧接着按 ticket-closeout §6 跑 `gh issue close 246 --comment "..."`，只会得到
`! Issue ... is already closed`——**评论从未发出**。实测 #246（合并后 2s 关闭，
closed_by.commit_id = dbf1ef3）、#260、#243 三票同形态；#260 的补充评论开头甚至写着
「本票被 GitHub 按 `fix:` 提交关键字自动关闭」，说明当时的操作者把它当成了偶发。

治本的做法是让「含票号」与「不触发 auto-close」同时成立：**票号写进作用域**
`fix(#246): ...`。GitHub 的 closing keyword 解析要求关键字后紧跟空白/冒号再接 `#N`，
括号形态不匹配；而 check-push-scope 的正则 `#<issue>([^0-9]|$)` 照样命中。

规则：

  R1 prefix          subject 必须匹配 `<type>[(scope)]!?: <描述>`，type 取仓库白名单
  R2 subject-length  subject 长度不超过 72 字符（GitHub/Git 日志可读性；warn 级不阻断）
  R3 closing-keyword subject 含 auto-close 形态（见下）→ 违规
  R4 pr-body-closing PR 正文含 auto-close 形态（#316 AC4）→ 违规

R3 的正则刻意写得比 GitHub 实际解析**更严**（不区分大小写、容忍 `fixes #1, #2` 这类
列表），因为本检查的目标是让作者改标题，不是复刻 GitHub 的解析器；宁可多报一条让人
加作用域，也不要漏报一次静默吞评论。

R4 的由来（#316 AC4）：#301–#303 波的 5 个 PR 里 3 个正文写了 `Closes #N`
（#313 `Closes #301`、#314 `Closes #302`、#315 `Closes #303`），#301 的关闭评论因此被
GitHub 自动关闭吞掉、事后补发。教训写在 #301 的评论里，却没有变成检查——R3 只管**提交
标题**，PR 正文是另一个面（`ci.yml` 里此前 `grep -c 'pull_request.body'` = 0）。
R4 与 R3 共用同一个 `CLOSING_KEYWORD_RE`（单一真相源），逐行报行号。

用法:
  python3 scripts/check_commit_subject.py --range origin/main..HEAD   # CI 用：逐 commit 校验
  python3 scripts/check_commit_subject.py --message-file .git/COMMIT_EDITMSG  # hook 用
  python3 scripts/check_commit_subject.py --subject "fix(#265): x"    # 单条
  python3 scripts/check_commit_subject.py --pr-body-file body.md      # PR 正文（CI / 本地）
  python3 scripts/check_commit_subject.py --pr-body "Closes #1"       # PR 正文（内联）
  python3 scripts/check_commit_subject.py --selftest

退出码: 0=通过, 1=有违规, 2=参数/环境错误
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys

TYPES = (
    "fix",
    "feat",
    "refactor",
    "docs",
    "test",
    "chore",
    "perf",
    "style",
    "build",
    "ci",
)

# `<type>` / `<type>(<scope>)` / `<type>!` / `<type>(<scope>)!` + `: ` + 非空描述。
# scope 允许含 `#`，因为本仓把票号写进作用域（fix(#246): ...）。
PREFIX_RE = re.compile(
    r"^(?P<type>" + "|".join(TYPES) + r")(?:\((?P<scope>[^()\s]+)\))?!?: \S"
)

# auto-close 形态：closing keyword 后（可隔着标点/空白/逗号列表）跟 `#N`。
# 覆盖：`fix: #246 x`、`Fixes #246`、`close #1, #2`、`resolved #7`。
CLOSING_KEYWORD_RE = re.compile(
    r"(?i)\b(?:close[sd]?|fix(?:e[sd])?|resolve[sd]?)\b[:;,.\s-]*#\d+"
)

SKIP_SUBJECT_PREFIXES = ("Merge ", "Revert ", "fixup! ", "squash! ")

MAX_SUBJECT_LEN = 72


def check_subject(subject: str) -> list[tuple[str, str]]:
    """返回 [(rule, message), ...]。subject 为单行提交标题。"""
    violations: list[tuple[str, str]] = []

    for prefix in SKIP_SUBJECT_PREFIXES:
        if subject.startswith(prefix):
            return violations

    match = PREFIX_RE.match(subject)
    if match is None:
        violations.append((
            "prefix",
            "subject must match '<type>[(<scope>)]!: <description>' with type in "
            + "/".join(TYPES)
            + f"; got {subject!r}",
        ))

    # R3 与 R1 独立：即使前缀不合法也要报 auto-close 形态（两者修复动作不同）。
    closing = CLOSING_KEYWORD_RE.search(subject)
    if closing is not None:
        scope_form = "fix(#<issue>): <描述>"
        violations.append((
            "closing-keyword",
            f"subject contains GitHub auto-close form {closing.group(0)!r}; merging the PR "
            "would close the issue before the close comment can be posted, silently "
            f"dropping it. Move the issue number into the scope: '{scope_form}'",
        ))

    if len(subject) > MAX_SUBJECT_LEN:
        violations.append((
            "subject-length",
            f"subject is {len(subject)} chars (limit {MAX_SUBJECT_LEN})",
        ))

    return violations


def render(violations: list[tuple[str, str]], subject: str) -> int:
    for rule, message in violations:
        print(f"commit-subject/{rule}: {message}", file=sys.stderr)
    if violations:
        print(f"FAIL: {len(violations)} commit subject violation(s) in: {subject}",
              file=sys.stderr)
        return 1
    return 0


def check_pr_body(body: str) -> list[tuple[int, str]]:
    """PR 正文里每一处 auto-close 形态 -> [(1-based 行号, 命中的片段)]。

    R4（#316 AC4）：PR 正文与提交标题是同一个 GitHub auto-close 面。正文里一句
    `Closes #301` 会让合并瞬间关掉 #301，§6 的「先评论后关闭」两步法就只剩
    `! Issue ... is already closed`——评论从未发出。规则复用 `CLOSING_KEYWORD_RE`，
    不另写一套正则（两套必然漂移）。
    """
    hits: list[tuple[int, str]] = []
    for lineno, line in enumerate(body.splitlines(), start=1):
        match = CLOSING_KEYWORD_RE.search(line)
        if match is not None:
            hits.append((lineno, match.group(0)))
    return hits


def render_pr_body(hits: list[tuple[int, str]]) -> int:
    """逐行打印 R4 命中；有命中返回 1。"""
    if not hits:
        print("OK: PR body has no GitHub auto-close form")
        return 0
    for lineno, fragment in hits:
        print(
            f"commit-subject/pr-body-closing:line {lineno}: PR body contains GitHub "
            f"auto-close form {fragment!r}; merging would close the issue before the "
            "close comment can be posted, silently dropping it. Post the close comment "
            "before merging, or drop the closing keyword from the PR body",
            file=sys.stderr,
        )
    print(
        f"FAIL: {len(hits)} GitHub auto-close form(s) in the PR body (line(s): "
        + ", ".join(str(lineno) for lineno, _ in hits)
        + ")",
        file=sys.stderr,
    )
    return 1


def subjects_from_range(rev_range: str) -> list[tuple[str, str]]:
    """`base..head` 内的非 merge commit -> [(sha, subject), ...]。"""
    out = subprocess.run(
        ["git", "log", "--no-merges", "--format=%H%x09%s", rev_range],
        capture_output=True,
        text=True,
    )
    if out.returncode != 0:
        raise RuntimeError(f"git log failed for {rev_range!r}: {out.stderr.strip()}")
    rows = []
    for line in out.stdout.splitlines():
        if not line.strip():
            continue
        sha, _, subject = line.partition("\t")
        rows.append((sha, subject))
    return rows


def read_message_file(path: str) -> str:
    """取 COMMIT_EDITMSG 的首个非空、非注释行作为 subject。"""
    with open(path, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            stripped = line.strip()
            if not stripped or stripped.startswith("#"):
                continue
            return stripped
    return ""


# ---------------------------------------------------------------------------
# selftest：规则本身也会写错（同 check_json_vendor_version.py 的理由）。
# 每条样本是 (subject, 期望违规规则集合)；精确集合相等，既挡漏报也挡"塌缩成永远同一条"。
# ---------------------------------------------------------------------------
SELFTEST_CASES: list[tuple[str, set[str]]] = [
    # 本轮的根因：完全合规但会触发 auto-close
    ("fix: #246 第 1 轮审查收敛（恒真断言）", {"closing-keyword"}),
    ("fix: #260 第 1 轮审查收敛", {"closing-keyword"}),
    # 治本形态：票号进作用域
    ("fix(#246): 第 1 轮审查收敛（恒真断言）", set()),
    ("feat(#246): 委托垂直切片", set()),
    ("chore(#265): 复盘护栏硬化", set()),
    # 中文全角括号里带票号：不触发 auto-close（关键字后不是 #N 紧邻）
    ("feat: 委托垂直切片（#246）", set()),
    # 其它 closing keyword 变体
    ("docs: closes #246", {"closing-keyword"}),
    # 大写的 "Fixes #246" 同时缺合法前缀（白名单全是小写），两条都该报——这正是精确
    # 集合相等断言的价值：用 `expected <= got` 会漏掉"前缀规则整体失效"这种退化。
    ("Fixes #246 and #247", {"closing-keyword", "prefix"}),
    ("fix: Fixes #246 and #247", {"closing-keyword"}),
    ("chore: resolve #9", {"closing-keyword"}),
    ("test: fixed #13", {"closing-keyword"}),
    # 关键字但没票号 -> 不是 auto-close
    ("fix: 修复登录失败", set()),
    ("fix: fixed the resolver", set()),
    # 前缀缺失
    ("委托垂直切片", {"prefix"}),
    ("Fix: 大写前缀", {"prefix"}),
    # merge/revert/fixup 跳过
    ("Merge pull request #263 from ockham001/feat/issue-246", set()),
    ("Revert \"feat(#246): 委托垂直切片\"", set()),
    ("fixup! fix(#246): x", set()),
    # 超长
    ("feat(#265): " + "x" * 70, {"subject-length"}),
]


PR_BODY_SELFTEST_CASES: list[tuple[str, int]] = [
    # R4（#316 AC4）：与 R3 同一个正则，但按行号逐处报。
    ("## 摘要\n\nCloses #301\n", 1),
    ("body\n\nCloses #301\nCloses #302\n", 2),
    ("no keyword here\n\nRelated to #301\n", 0),
    ("Closes #301 before the close comment is posted\n", 1),
    # 括号形态与全角括号不触发（与 R3 同一口径）。
    ("fix(#301): x\n", 0),
    ("feat: 收口（#303）\n", 0),
    # 大小写与列表形态同样命中（正则刻意比 GitHub 更严）。
    ("FIXES #7, #8\n", 1),
]


def run_selftest() -> int:
    failures = 0
    for subject, expected in SELFTEST_CASES:
        got = {rule for rule, _ in check_subject(subject)}
        if got != expected:
            print(f"selftest FAIL {subject!r}: expected {sorted(expected) or 'none'}, "
                  f"got {sorted(got) or 'none'}", file=sys.stderr)
            failures += 1
    for body, expected_hits in PR_BODY_SELFTEST_CASES:
        hits = check_pr_body(body)
        if len(hits) != expected_hits:
            print(f"selftest FAIL pr-body {body!r}: expected {expected_hits} hit(s), "
                  f"got {len(hits)}", file=sys.stderr)
            failures += 1
    if failures:
        print(f"selftest FAILED: {failures} case(s)", file=sys.stderr)
        return 1
    print(
        f"selftest: {len(SELFTEST_CASES)} commit-subject forms + "
        f"{len(PR_BODY_SELFTEST_CASES)} PR-body forms behave as expected"
    )
    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="validate commit subject prefix and auto-close forms")
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--range", dest="rev_range", help="git rev range, e.g. origin/main..HEAD")
    group.add_argument("--message-file", help="commit message file (pre-commit hook)")
    group.add_argument("--subject", help="a single subject line")
    group.add_argument("--pr-body-file", help="PR body markdown file (R4, #316 AC4)")
    group.add_argument("--pr-body", help="PR body text inline (R4, #316 AC4)")
    group.add_argument("--selftest", action="store_true", help="run rule selftest")
    args = parser.parse_args(argv)

    if args.selftest:
        return run_selftest()

    if args.pr_body is not None:
        return render_pr_body(check_pr_body(args.pr_body))

    if args.pr_body_file:
        try:
            with open(args.pr_body_file, encoding="utf-8", errors="replace") as fh:
                body = fh.read()
        except OSError as exc:
            print(f"error: cannot read PR body file {args.pr_body_file}: {exc}", file=sys.stderr)
            return 2
        return render_pr_body(check_pr_body(body))

    if args.subject is not None:
        return render(check_subject(args.subject), args.subject)

    if args.message_file:
        subject = read_message_file(args.message_file)
        if not subject:
            print(f"commit-subject: empty commit message in {args.message_file}", file=sys.stderr)
            return 1
        return render(check_subject(subject), subject)

    try:
        rows = subjects_from_range(args.rev_range)
    except RuntimeError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    if not rows:
        print(f"OK: no commits in range {args.rev_range}")
        return 0

    failed = 0
    for sha, subject in rows:
        if render(check_subject(subject), subject) != 0:
            print(f"  -> {sha}", file=sys.stderr)
            failed += 1
    if failed:
        print(f"FAIL: {failed}/{len(rows)} commit(s) in {args.rev_range} have bad subjects",
              file=sys.stderr)
        return 1
    print(f"OK: {len(rows)} commit subject(s) in {args.rev_range} are well-formed")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
