#!/usr/bin/env python3
"""test_check_branch_name.py — worktree 分支名早检的三态（#424 AC2）

为什么需要：`create_worktree` 的 `name` 被写 flat（`fix-issue-411-today-position`）时，
`.githooks/pre-push` 要到**实现完成后的推送**才拒绝；#411 因此到收尾才发现要改名。
`scripts/setup.sh` 开头调 `scripts/check_branch_name.py`，把拒绝提前到 worktree 的第一条
命令。本文件锁三态 + 一条修复命令：

1. 合规分支（`<type>/issue-<n>-<slug>`）通过；
2. flat 名拒绝，且 stderr 打印可执行的 `git branch -m`（含自动还原的正确名字）；
3. `main` 与 detached HEAD 跳过（CI 的 pull_request / tag 检出必须不被拦）。

前三条走 CLI（`--branch`），最后一条在临时 git 仓库里验证真实 HEAD 解析路径
（`symbolic-ref` 而不是 `rev-parse --abbrev-ref` 的字面量 HEAD）。

跑法（被 preflight 的 unittest discovery 自动执行）：
    python3 -m unittest discover -s scripts/tests -k check_branch_name
    python3 scripts/tests/test_check_branch_name.py -v

退出码：0=通过，1=有失败。
"""

from __future__ import annotations

import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "scripts" / "check_branch_name.py"
PRE_PUSH = REPO_ROOT / ".githooks" / "pre-push"
sys.path.insert(0, str(REPO_ROOT / "scripts"))

import check_branch_name  # noqa: E402


def run_checker(*args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(SCRIPT), *args], capture_output=True, text=True, cwd=str(REPO_ROOT)
    )


class BranchNameCheckTest(unittest.TestCase):
    def setUp(self) -> None:
        if not SCRIPT.is_file():
            self.fail(f"check_branch_name.py not found: {SCRIPT}")

    def test_compliant_branch_passes(self) -> None:
        for branch in (
            "fix/issue-411-today-position",
            "chore/issue-424-retro-hardening",
            "docs/issue-316-adr-map",
        ):
            result = run_checker("--branch", branch)
            self.assertEqual(result.returncode, 0, f"{branch}: {result.stderr}")
            self.assertIn("branch name ok", result.stdout)

    def test_flat_branch_is_rejected_with_fix_command(self) -> None:
        result = run_checker("--branch", "fix-issue-411-today-position")
        self.assertEqual(result.returncode, 1)
        self.assertIn("git branch -m fix/issue-411-today-position", result.stderr)
        self.assertIn("pre-push", result.stderr)

    def test_main_is_skipped(self) -> None:
        result = run_checker("--branch", "main")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("skipped", result.stdout)

    def test_selftest_passes(self) -> None:
        result = run_checker("--selftest")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("selftest OK", result.stdout)


class BranchTypeSingleSourceTest(unittest.TestCase):
    """类型前缀与 `.githooks/pre-push` 的白名单必须一致（#424 审查 finding）。

    两处语义不同（那边只校验前缀、这里校验完整约定），所以不合并成一份实现；但**类型集合**
    是同一个事实，改一处漏一处会让「setup 放行、pre-push 拒」再次出现。本用例把
    「两处一起改」的纪律变成机械检查。
    """

    def test_types_match_pre_push_whitelist(self) -> None:
        if not PRE_PUSH.is_file():
            self.skipTest(f"pre-push hook not found: {PRE_PUSH}")
        text = PRE_PUSH.read_text(encoding="utf-8")
        match = re.search(r"BRANCH_RE='\^\(([^)]*)\)/'", text)
        self.assertIsNotNone(
            match,
            "pre-push 的 BRANCH_RE 形态变了：请同步本用例与 scripts/check_branch_name.py",
        )
        self.assertEqual(tuple(match.group(1).split("|")), check_branch_name.BRANCH_TYPES)


class CurrentBranchResolutionTest(unittest.TestCase):
    """真实 HEAD 解析：flat → 拒 / 合规 → 过 / main → 跳过 / detached → 跳过。"""

    def setUp(self) -> None:
        if shutil.which("git") is None:
            self.skipTest("git CLI not installed")
        self.tmp = Path(tempfile.mkdtemp(prefix="dz_branch_name_test_"))

    def tearDown(self) -> None:
        shutil.rmtree(self.tmp, ignore_errors=True)

    def _git(self, *args: str) -> None:
        subprocess.run(
            ["git", "-C", str(self.tmp), *args],
            check=True,
            capture_output=True,
        )

    def _check(self) -> subprocess.CompletedProcess[str]:
        return run_checker("--root", str(self.tmp))

    def test_branch_resolution_three_states(self) -> None:
        self._git("init", "-q")
        self._git("symbolic-ref", "HEAD", "refs/heads/fix-issue-411-today-position")
        result = self._check()
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("git branch -m fix/issue-411-today-position", result.stderr)

        self._git("symbolic-ref", "HEAD", "refs/heads/fix/issue-411-today-position")
        result = self._check()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("branch=fix/issue-411-today-position", result.stdout)

        self._git("symbolic-ref", "HEAD", "refs/heads/main")
        result = self._check()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("skipped", result.stdout)

    def test_detached_head_is_skipped(self) -> None:
        # CI 的 pull_request / tag 检出都是 detached HEAD：必须跳过，不能拦。
        self._git("init", "-q")
        self._git(
            "-c",
            "user.name=branch-name-test",
            "-c",
            "user.email=test@example.invalid",
            "-c",
            "commit.gpgsign=false",
            "commit",
            "-q",
            "--allow-empty",
            "-m",
            "chore(#424): baseline",
        )
        self._git("checkout", "-q", "--detach")
        result = self._check()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("reason=detached_head", result.stdout)


if __name__ == "__main__":
    unittest.main()
