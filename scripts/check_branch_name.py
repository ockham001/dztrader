#!/usr/bin/env python3
"""check_branch_name.py — worktree 分支名早检（#424 AC2）

## 为什么需要

`create_worktree` 的 `name` 必须是**完整分支名**（含 `/`）。工具描述的
「a few lowercase words joined by hyphens」只保证目录能建出来，于是 #411 建出了
flat 名 `fix-issue-411-today-position`：`.githooks/pre-push` 的白名单与票号解析
要到**实现完成后的推送**才拒绝，而那时整票工作已经做完。`setup` 是 worktree 里的
第一条命令，把这件事提前到第一秒。

## 规则

与 AGENTS「Worktree 与 Git 禁令」的分支名约定、`.githooks/pre-push` 的白名单一致：

  - `main` → 跳过（共享工作区就是这么用的）
  - detached HEAD（CI 的 `pull_request` / tag 检出、临时查看旧提交）→ 跳过
  - 其余分支必须匹配 `^(feat|fix|docs|test|chore|ci|refactor|perf|style|build)/issue-[0-9]+-`
    否则报错，并打印 `git branch -m` 修复命令（worktree 目录可不动）

flat 名能自动给出建议：`fix-issue-411-today-position` → `fix/issue-411-today-position`。

## 用法

    python3 scripts/check_branch_name.py                  # 按当前仓库的 HEAD 判定（setup.sh 调用）
    python3 scripts/check_branch_name.py --branch <name>  # 只判给定分支名（排查 / 自测）
    python3 scripts/check_branch_name.py --selftest

退出码: 0=合规或跳过, 1=分支名不合规, 2=环境错误（不在 git 工作树内）
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]

# 与 .githooks/pre-push 的 BRANCH_RE 同一组类型前缀；此处额外要求 `issue-<n>-`，
# 因为 pre-push 的票号解析（check-push-scope）依赖它，而缺票号只 warning、不拦。
# **有意不与 pre-push 合并**：那一步只校验类型前缀（`docs/foo` 无票号也放行，历史行为），
# 这里校验完整约定，两者语义不同；放宽 pre-push 会在推送时才拒掉已完成的票，收紧它又会
# 挡住无票号的分支。改类型集合（feat/fix/... ）时**两处一起改**：这里与 .githooks/pre-push。
BRANCH_TYPES = ("feat", "fix", "docs", "test", "chore", "ci", "refactor", "perf", "style", "build")
BRANCH_RE = re.compile(
    r"^(?:" + "|".join(BRANCH_TYPES) + r")/issue-[0-9]+-.+$"
)
SKIP_BRANCHES = frozenset({"main"})
# flat 形态（#411 实测）：`<type>-issue-<n>-<slug>`，可自动还原成 `<type>/issue-<n>-<slug>`。
FLAT_RE = re.compile(
    r"^(" + "|".join(BRANCH_TYPES) + r")-issue-([0-9]+)-(.+)$"
)

SELFTEST_COMPLIANT = (
    "fix/issue-411-today-position",
    "chore/issue-424-retro-hardening",
    "feat/issue-1-a",
    "docs/issue-316-adr",
    "refactor/issue-999-x-y",
)
SELFTEST_REJECTED = (
    "fix-issue-411-today-position",
    "fix/issue-411",  # 缺 slug
    "fix/411-today-position",  # 缺 issue 前缀
    "feature/issue-411-x",  # 前缀不在白名单
    "fix/issue-abc-x",  # 票号不是数字
    "main-fix",
    "HEAD",
)


class EnvError(Exception):
    """环境前提不满足（不在 git 工作树内 / git 不可用）。"""


def branch_name_state(branch: str) -> str:
    """纯函数：返回 'ok' / 'skip' / 'invalid'（空串按 detached 处理 → skip）。

    `--selftest` 与 `scripts/tests/test_check_branch_name.py` 都直接断言这三态，
    不需要真实 git 仓库。
    """
    if not branch or branch in SKIP_BRANCHES:
        return "skip"
    return "ok" if BRANCH_RE.match(branch) else "invalid"


def suggested_branch_name(branch: str) -> str | None:
    """flat 名 → 合规名；还原不出来时返回 None（打印占位符）。"""
    match = FLAT_RE.match(branch)
    if not match:
        return None
    return f"{match.group(1)}/issue-{match.group(2)}-{match.group(3)}"


def run_git(root: Path, *args: str) -> subprocess.CompletedProcess[str]:
    try:
        return subprocess.run(
            ["git", "-C", str(root), *args], capture_output=True, text=True, encoding="utf-8"
        )
    except OSError as exc:
        raise EnvError(f"cannot run git: {exc}") from exc


def current_branch(root: Path) -> str | None:
    """当前分支名；detached HEAD 返回 None。

    用 `symbolic-ref` 而不是 `rev-parse --abbrev-ref`：后者在 detached 时返回字面量
    `HEAD`，会与真叫 HEAD 的分支混淆（预推送模板与审计脚本都踩过这条）。
    """
    inside = run_git(root, "rev-parse", "--is-inside-work-tree")
    if inside.returncode != 0 or inside.stdout.strip() != "true":
        raise EnvError(f"not a git work tree: {root}")
    ref = run_git(root, "symbolic-ref", "-q", "--short", "HEAD")
    if ref.returncode != 0:
        return None
    return ref.stdout.strip() or None


def selftest() -> int:
    for name in SELFTEST_COMPLIANT:
        if branch_name_state(name) != "ok":
            raise SystemExit(f"selftest failed: compliant branch rejected: {name}")
    for name in SELFTEST_REJECTED:
        if branch_name_state(name) != "invalid":
            raise SystemExit(f"selftest failed: non-compliant branch accepted: {name}")
    for name in ("main", ""):
        if branch_name_state(name) != "skip":
            raise SystemExit(f"selftest failed: expected skip for {name!r}")
    if suggested_branch_name("fix-issue-411-today-position") != "fix/issue-411-today-position":
        raise SystemExit("selftest failed: flat branch name was not rewritten to a compliant one")
    if suggested_branch_name("fix/issue-411-today-position") is not None:
        raise SystemExit("selftest failed: compliant branch name should not be rewritten")
    print(
        f"selftest OK: {len(SELFTEST_COMPLIANT)} compliant / "
        f"{len(SELFTEST_REJECTED)} rejected / 2 skipped branch names"
    )
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="worktree 分支名早检（只读）")
    parser.add_argument("--branch", help="只判给定分支名，不读仓库 HEAD")
    parser.add_argument("--root", type=Path, default=REPO_ROOT, help="仓库根（默认本脚本上一级）")
    parser.add_argument("--selftest", action="store_true", help="只跑纯函数自测（不碰仓库）")
    args = parser.parse_args(argv)

    if args.selftest:
        return selftest()

    try:
        branch = args.branch if args.branch is not None else current_branch(args.root)
    except EnvError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    state = branch_name_state(branch or "")
    if state == "skip":
        if not branch:
            print("branch name check skipped | reason=detached_head")
        else:
            print(f"branch name check skipped | branch={branch}")
        return 0
    if state == "ok":
        print(f"branch name ok | branch={branch}")
        return 0

    suggestion = suggested_branch_name(branch or "")
    fix_target = suggestion or "<type>/issue-<n>-<slug>"
    print(
        f"error: branch name is not <type>/issue-<n>-<slug> | branch={branch}",
        file=sys.stderr,
    )
    print(
        "       allowed types: " + "/".join(BRANCH_TYPES) + "; a flat name delays the "
        "rejection until pre-push",
        file=sys.stderr,
    )
    print(f"       fix: git branch -m {fix_target}   (the worktree directory can stay)", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
