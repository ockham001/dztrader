#!/usr/bin/env python3
"""check_flow.py — 标准开发循环的顺序守卫（#535）

## 为什么需要

`AGENTS.md`「标准开发循环」要求：设计定稿 → 立 spec → 拆票（**拆完即停**）→
换会话实现 → 审查 → 收尾。2026-11-28 的两次会话都在这里失守：

- 第一次（#528 复盘）补了「拆完即停」的**文字**规则；
- 第二次（#534 配色）文字规则没拦住，agent 在同一会话里完成了
  设计 → spec → **直接改 theme.css / design-tokens.css**，用户当场叫停。

根因不是 agent 不读文档，而是这条链路**没有任何机器判定**。23 步 preflight 里
没有一步检查「实现是否在一个有 spec 与 ADR 的票之下」；`push-ticket.sh` 只查
push-scope、review marker、md lock。于是「先实现后补文档」在机器眼里与
「按流程开发」完全同形。

## 判据（全部机械、无网络）

对一个**准备 push** 的分支，检查它在 `@{u}..HEAD`（无上游则 `origin/main..HEAD`）
里改动的文件是否满足：

1. **实现类文件**（`.css` / `.vue` / `.ts` / `.cpp` / `.h` / `.hpp` / `.py` 等）
   出现时，分支名必须带票号 `issue-<n>`，且该 `<n>` 必须能在**本地证据**里找到
   spec 或 ADR 引用：
   - `docs/adr/*.md` 里出现 `#<n>` 或该票号；或
   - 分支的 commit message 里出现 `[spec]` / `spec #<n>`。
   因为脚本跑在 pre-push、要求离线，**不查 GitHub API**（`gh` 在 CI/无网环境不可用，
   而 check_ledger 那种联网脚本已明确「不接入 CI」）。
2. **纯文档/脚本票**（只改 `docs/`、`*.md`、`scripts/`）豁免第 1 条。

## 与「拆完即停」的关系

本脚本**不**试图判定「是否在同一会话里既拆票又实现」——那需要会话级状态，
git 里看不到。它守的是可观测的后果：**实现没有对应的 spec/ADR 证据就推不上去**。
这条能挡住 #534 那种形态（无 ADR、无 spec 就改 CSS）——当时若跑本脚本会直接失败。

## 用法

    python3 scripts/check_flow.py            # 判定当前分支（pre-push 调用）
    python3 scripts/check_flow.py --strict   # 只在有 finding 时退 1（默认行为）
    python3 scripts/check_flow.py --selftest # 三态自测，不碰工作区
    python3 scripts/check_flow.py --explain  # 打印判据与当前证据

## 退出码

    0 = 通过或豁免
    1 = 有 finding（缺 spec/ADR 证据）
    2 = 环境错误（不是 git 仓库等）

逃生口：`DZ_ALLOW_FLOW=1`（与其它钩子逃生口同族，仅真故障时用，用完在报告里写明）。
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

# ---- 实现类文件的判据 -------------------------------------------------------
# 只列「改了就必须有 spec/ADR」的：样式、模板、源码、测试。
# 刻意不含：docs/、*.md、scripts/、配置模板（这些是流程本身的产物）。
IMPL_SUFFIXES = {
    ".css", ".scss", ".vue", ".ts", ".tsx", ".js", ".mjs",
    ".cpp", ".cc", ".cxx", ".h", ".hpp", ".hxx", ".c",
    ".py", ".sh", ".ps1", ".cmake", ".json", ".yml", ".yaml",
}
# 这些路径即便后缀命中也不要求 spec（工具链与流程文件）
EXEMPT_PREFIXES = (
    "docs/", "scripts/", ".githooks/", ".github/", ".cache/", ".agents/",
)
EXEMPT_FILES = {"AGENTS.md", "README.md", "CLAUDE.md", "GLOSSARY.md", "DESIGN.md", "PRODUCT.md"}

BRANCH_RE = re.compile(r"^(?:feat|fix|docs|test|chore|ci|refactor|perf|style|build)/issue-(\d+)-")
ADR_REF_RE = re.compile(r"#(\d+)\b")


def fail(msg: str) -> None:
    print(f"check_flow: {msg}", file=sys.stderr)


def run(cmd: list[str], cwd: Path | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)


def repo_root() -> Path | None:
    r = run(["git", "rev-parse", "--show-toplevel"])
    if r.returncode != 0:
        return None
    return Path(r.stdout.strip())


def current_branch(root: Path) -> str | None:
    r = run(["git", "rev-parse", "--abbrev-ref", "HEAD"], cwd=root)
    if r.returncode != 0:
        return None
    name = r.stdout.strip()
    return None if name == "HEAD" else name


def changed_files(root: Path, branch: str) -> list[str]:
    """分支相对上游（或 origin/main）的改动文件。"""
    r = run(["git", "rev-parse", "--abbrev-ref", f"{branch}@{{u}}"], cwd=root)
    base = r.stdout.strip() if r.returncode == 0 and r.stdout.strip() else "origin/main"
    r = run(["git", "diff", "--name-only", f"{base}...HEAD"], cwd=root)
    if r.returncode != 0:
        return []
    return [f for f in r.stdout.splitlines() if f.strip()]


def is_impl(path: str) -> bool:
    if path in EXEMPT_FILES:
        return False
    if any(path.startswith(p) for p in EXEMPT_PREFIXES):
        return False
    return Path(path).suffix.lower() in IMPL_SUFFIXES


def ticket_from_branch(branch: str) -> int | None:
    m = BRANCH_RE.match(branch)
    return int(m.group(1)) if m else None


def adr_references_ticket(root: Path, ticket: int) -> bool:
    """ADR 目录里是否有文件引用该票号（`#N` 或 `issue-N`）。"""
    adr_dir = root / "docs" / "adr"
    if not adr_dir.is_dir():
        return False
    needle_issue = f"issue-{ticket}"
    for md in adr_dir.glob("*.md"):
        try:
            text = md.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        if f"#{ticket}" in text or needle_issue in text:
            return True
    return False


def commit_messages(root: Path, branch: str) -> list[str]:
    r = run(["git", "rev-parse", "--abbrev-ref", f"{branch}@{{u}}"], cwd=root)
    base = r.stdout.strip() if r.returncode == 0 and r.stdout.strip() else "origin/main"
    r = run(["git", "log", "--format=%B", f"{base}..HEAD"], cwd=root)
    if r.returncode != 0:
        return []
    return [m for m in r.stdout.split("\n\n") if m.strip()]


def spec_evidence(root: Path, branch: str, ticket: int) -> bool:
    """本地可判定的 spec 证据：commit message 里出现 [spec] 或 spec #N。"""
    for msg in commit_messages(root, branch):
        low = msg.lower()
        if "[spec]" in low:
            return True
        if re.search(rf"\bspec\s*#?{ticket}\b", low):
            return True
    return False


def evaluate(branch: str | None, files: list[str], adr_ok: bool, spec_ok: bool) -> tuple[bool, list[str]]:
    """纯函数：给定证据返回 (通过?, findings)。自测直接喂它。"""
    if branch is None or branch == "main":
        return True, []
    impl = [f for f in files if is_impl(f)]
    if not impl:
        return True, []  # 纯文档/脚本票豁免
    ticket = ticket_from_branch(branch)
    if ticket is None:
        return False, [f"分支名缺票号（期望 <type>/issue-<n>-<slug>，实际 {branch}）"]
    if adr_ok or spec_ok:
        return True, []
    return False, [
        f"分支改了 {len(impl)} 个实现文件，但找不到票 #{ticket} 的 spec/ADR 证据",
        f"  实现文件示例：{', '.join(impl[:5])}",
        "  期望其一：docs/adr/*.md 引用 #%d；或 commit message 含 [spec] / spec #%d" % (ticket, ticket),
    ]


def explain(root: Path, branch: str | None) -> int:
    files = changed_files(root, branch) if branch else []
    impl = [f for f in files if is_impl(f)]
    ticket = ticket_from_branch(branch) if branch else None
    adr_ok = adr_references_ticket(root, ticket) if ticket else False
    spec_ok = spec_evidence(root, branch, ticket) if (ticket and branch) else False
    print(f"branch      : {branch}")
    print(f"ticket      : {ticket}")
    print(f"changed     : {len(files)} 个文件，其中实现类 {len(impl)} 个")
    for f in impl[:10]:
        print(f"  - {f}")
    print(f"ADR 证据    : {'有' if adr_ok else '无'}")
    print(f"spec 证据   : {'有' if spec_ok else '无'}")
    ok, findings = evaluate(branch, files, adr_ok, spec_ok)
    print(f"判定        : {'通过' if ok else '失败'}")
    for f in findings:
        print(f"  {f}")
    return 0 if ok else 1


def selftest() -> int:
    """三态自测：不碰工作区，纯函数级。"""
    cases = [
        # (branch, files, adr_ok, spec_ok, expect_ok, why)
        ("main", ["a.css"], False, False, True, "main 豁免"),
        ("style/issue-9-x", ["docs/a.md"], False, False, True, "纯文档豁免"),
        ("style/issue-9-x", ["app.css"], True, False, True, "有 ADR"),
        ("style/issue-9-x", ["app.css"], False, True, True, "有 spec"),
        ("style/issue-9-x", ["app.css"], False, False, False, "实现无证据 → 拦"),
        ("style/issue-9-x", ["scripts/a.py"], False, False, True, "scripts/ 豁免"),
        ("badname", ["app.css"], False, False, False, "分支缺票号 → 拦"),
    ]
    bad = 0
    for branch, files, adr, spec, want, why in cases:
        ok, findings = evaluate(branch, files, adr, spec)
        if ok != want:
            bad += 1
            print(f"  FAIL {why}: 期望 {want} 实得 {ok} findings={findings}")
        else:
            print(f"  ok   {why}")
    print(f"\nselftest: {'PASS' if bad == 0 else f'{bad} FAILED'}")
    return 0 if bad == 0 else 1


def main() -> int:
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--explain", action="store_true")
    ap.add_argument("--strict", action="store_true", help="（默认即 strict，保留兼容）")
    ap.add_argument("--branch", help="只判给定分支名（排查用）")
    args = ap.parse_args()

    if args.selftest:
        return selftest()

    if os.environ.get("DZ_ALLOW_FLOW") == "1":
        print("check_flow: 逃生口 DZ_ALLOW_FLOW=1 已置，放行", file=sys.stderr)
        return 0

    root = repo_root()
    if root is None:
        fail("不是 git 仓库")
        return 2

    if args.branch:
        branch = args.branch
    else:
        r = run(["git", "symbolic-ref", "--short", "-q", "HEAD"], cwd=root)
        if r.returncode != 0:
            print("check_flow: detached HEAD，跳过", file=sys.stderr)
            return 0
        branch = r.stdout.strip()
        if branch == "main":
            return 0

    if args.explain:
        return explain(root, branch)

    files = changed_files(root, branch)
    ticket = ticket_from_branch(branch)
    adr_ok = adr_references_ticket(root, ticket) if ticket else False
    spec_ok = spec_evidence(root, branch, ticket) if ticket else False
    ok, findings = evaluate(branch, files, adr_ok, spec_ok)
    if ok:
        return 0
    for f in findings:
        fail(f)
    print(
        "\ncheck_flow 拦截：本分支有实现改动但缺少 spec/ADR 证据。\n"
        "  修法：先为该票写 spec（GitHub issue）与 ADR（若满足触发条件），\n"
        "        再提交实现；或确认这是纯文档/脚本改动。\n"
        "  排查：python3 scripts/check_flow.py --explain\n"
        "  逃生口：DZ_ALLOW_FLOW=1（仅真故障，用完在报告里写明）",
        file=sys.stderr,
    )
    return 1


if __name__ == "__main__":
    sys.exit(main())
