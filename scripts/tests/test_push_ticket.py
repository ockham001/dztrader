#!/usr/bin/env python3
"""tests for scripts/push-ticket.sh — 推送纪律脚本（#309 AC6）

背景：本波唯一违例 #297 —— `git push` 在相对 4640 s、首个 review spawn 于 4659 s（晚
19 s），随后修复又 `--force-with-lease`，产生第二个 CI run（19 min）。`.githooks/pre-push`
的 `review_marker.py check` 打印了 warning，但 warning-only 被无视。`push-ticket.sh` 把
「scope 检查 → review 标记严格判定 → git push」串成一条默认拒绝的路径。

这些用例全部**进程级**驱动真实脚本（临时 git 仓库 + 假 `git`/`gh` 注入 PATH），断言
退出码、stdout/stderr 与「push 到底跑没跑」——而不是 import 内部函数。理由同
`test_ci_scope.py`：脚本的唯一消费者是 shell 与 pre-push，只测内部函数会放过「顺序接错 /
退出码被吞 / 检查失败仍然调了 push」这类真实故障。假 git 是转发式的：只有 `push`
子命令被截获并记录，其余转给真 git（review_marker 与 check-push-scope 的判定依赖真实
仓库状态）；假 gh 返回空 run 列表，不联网。

反向验证：把主流程里 review 那段的默认值从 `--strict` 改回 warning-only，或把 `git push`
前移一步，`test_no_marker_refuses` / `test_scope_violation_refuses_before_push` 立即红。

跑法：
    python3 -m unittest discover -s scripts/tests -k push_ticket -v
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
PUSH_TICKET = REPO_ROOT / "scripts" / "push-ticket.sh"
SCRIPTS = ("push-ticket.sh", "check-push-scope.sh", "review_marker.py")
ISSUE = "309"

REAL_GIT = shutil.which("git")
if REAL_GIT is None:  # pragma: no cover - 环境错误，不是被测行为
    raise unittest.SkipTest("git not available")

# 假 git：只截获 push（记录 argv 并按 DZ_FAKE_PUSH_RC 退出），其余转发给真 git。
FAKE_GIT = """#!/usr/bin/env bash
if [ "${1:-}" = "push" ]; then
    printf '%s\\n' "$@" >"${DZ_FAKE_DIR:?}/push.log"
    exit "${DZ_FAKE_PUSH_RC:-0}"
fi
exec "${DZ_REAL_GIT:?}" "$@"
"""

# 假 gh：`gh run list` 输出空 = 没有 in-progress CI run，check-push-scope 不拦截。
FAKE_GH = """#!/usr/bin/env bash
exit 0
"""


class TempRepo:
    """一次性临时仓库：两个 commit（baseline 提交到 origin/main，head 是本票提交）+ 脚本副本。"""

    def __init__(self) -> None:
        self.root = Path(tempfile.mkdtemp(prefix="dz_push_ticket_test_"))
        self.repo = self.root / "repo"
        self.bin = self.root / "bin"
        self.fake = self.root / "fake"
        self.repo.mkdir()
        self.bin.mkdir()
        self.fake.mkdir()
        (self.repo / "scripts").mkdir()
        for name in SCRIPTS:
            shutil.copy2(REPO_ROOT / "scripts" / name, self.repo / "scripts" / name)
        (self.repo / "scripts" / "push-ticket.sh").chmod(0o755)
        (self.bin / "git").write_text(FAKE_GIT, encoding="utf-8")
        (self.bin / "gh").write_text(FAKE_GH, encoding="utf-8")
        for name in ("git", "gh"):
            (self.bin / name).chmod(0o755)
        self.git("init", "-q", "-b", "chore/issue-309-demo")
        self.git("commit", "-q", "--allow-empty", "-m", "chore(#309): test baseline")
        self.base = self.rev("HEAD")
        self.git("commit", "-q", "--allow-empty", "-m", "fix(#309): test head")
        self.head = self.rev("HEAD")
        # 无上游时 check-push-scope 的 base 回退到 origin/main：指到 baseline，区间只含 head。
        self.git("update-ref", "refs/remotes/origin/main", self.base)

    def git(self, *args: str) -> subprocess.CompletedProcess:
        return subprocess.run(
            [REAL_GIT, "-c", "user.name=push-ticket-test",
             "-c", "user.email=test@example.invalid", "-c", "commit.gpgsign=false", *args],
            cwd=self.repo, capture_output=True, text=True, timeout=120, check=True)

    def rev(self, ref: str) -> str:
        return self.git("rev-parse", ref).stdout.strip()

    def write_marker(self, axis: str, sha: str) -> None:
        directory = self.repo / ".cache" / "review"
        directory.mkdir(parents=True, exist_ok=True)
        (directory / f"{ISSUE}-{axis}.done").write_text(sha + "\n", encoding="utf-8")

    def run(self, *args: str, allow_unreviewed: bool = False,
            push_rc: int = 0) -> subprocess.CompletedProcess:
        """跑脚本副本（在临时仓库内）；push 由假 git 记录而不联网。"""
        (self.fake / "push.log").unlink(missing_ok=True)
        env = os.environ.copy()
        env["PATH"] = f"{self.bin}{os.pathsep}{env.get('PATH', '')}"
        env["DZ_FAKE_DIR"] = str(self.fake)
        env["DZ_REAL_GIT"] = REAL_GIT
        env["DZ_FAKE_PUSH_RC"] = str(push_rc)
        env.pop("DZ_ALLOW_UNREVIEWED_PUSH", None)
        if allow_unreviewed:
            env["DZ_ALLOW_UNREVIEWED_PUSH"] = "1"
        result = subprocess.run(
            [str(self.repo / "scripts" / "push-ticket.sh"), *args],
            cwd=self.repo, capture_output=True, text=True, env=env,
            timeout=120, check=False)
        result.push_ran = (self.fake / "push.log").is_file()  # type: ignore[attr-defined]
        result.push_argv = ((self.fake / "push.log").read_text(encoding="utf-8").splitlines()
                            if result.push_ran else [])  # type: ignore[attr-defined]
        return result

    def cleanup(self) -> None:
        shutil.rmtree(self.root, ignore_errors=True)


def _both_markers(repo: TempRepo, sha: str) -> None:
    for axis in ("standards", "spec"):
        repo.write_marker(axis, sha)


class PushTicketCase(unittest.TestCase):
    def setUp(self) -> None:
        self.repo = TempRepo()

    def tearDown(self) -> None:
        self.repo.cleanup()

    def _assert_refused(self, result: subprocess.CompletedProcess, *fragments: str) -> None:
        self.assertEqual(result.returncode, 3, f"stderr={result.stderr!r}")
        self.assertFalse(result.push_ran, "git push must not run when the checks refuse")
        for fragment in fragments:
            self.assertIn(fragment, result.stderr)

    # ---- 拒绝路径 -----------------------------------------------------

    def test_no_marker_refuses(self) -> None:
        """无标记 → 拒绝（exit 3），两个轴各自点名，且打印当前 HEAD。"""
        result = self.repo.run(ISSUE, "-u", "origin", "chore/issue-309-demo")
        self._assert_refused(result, "(standards)", "(spec)", self.repo.head)
        self.assertIn("nothing was pushed", result.stderr)
        # push scope 必须已经先跑过（顺序：scope -> review -> push）。
        self.assertIn("push scope ok", result.stdout)

    def test_stale_marker_refuses(self) -> None:
        """标记停在旧 sha → 拒绝，warning 同时给出标记 sha 与 HEAD 前缀。"""
        _both_markers(self.repo, self.repo.base)
        result = self.repo.run(ISSUE, "-u", "origin", "chore/issue-309-demo")
        self._assert_refused(result, "(standards)", "HEAD is")
        self.assertIn(self.repo.base[:12], result.stderr)
        self.assertIn(self.repo.head[:12], result.stderr)

    def test_empty_marker_refuses(self) -> None:
        """标记文件为空（0 字节 / 只有换行）→ 拒绝（不能当成「验证过」）。"""
        self.repo.write_marker("standards", self.repo.head)
        self.repo.write_marker("spec", "")
        result = self.repo.run(ISSUE, "-u", "origin", "chore/issue-309-demo")
        self._assert_refused(result, "(spec) is empty or invalid")

    def test_only_one_axis_marked_refuses(self) -> None:
        self.repo.write_marker("standards", self.repo.head)
        result = self.repo.run(ISSUE, "-u", "origin", "chore/issue-309-demo")
        self._assert_refused(result, "(spec)")

    def test_scope_violation_refuses_before_review(self) -> None:
        """scope 违规先于 review 判定：即使标记齐全也拒绝，且不 push。"""
        _both_markers(self.repo, self.repo.head)
        self.repo.git("commit", "-q", "--allow-empty", "-m", "chore: out-of-scope commit")
        result = self.repo.run(ISSUE, "-u", "origin", "chore/issue-309-demo")
        self.assertEqual(result.returncode, 3, f"stderr={result.stderr!r}")
        self.assertFalse(result.push_ran)
        self.assertIn("push scope", result.stderr)

    # ---- 放行路径 -----------------------------------------------------

    def test_matching_markers_push(self) -> None:
        """两个轴都停在 HEAD → 执行 git push，参数原样透传。"""
        _both_markers(self.repo, self.repo.head)
        result = self.repo.run(ISSUE, "-u", "origin", "chore/issue-309-demo")
        self.assertEqual(result.returncode, 0, f"stderr={result.stderr!r}")
        self.assertTrue(result.push_ran)
        self.assertEqual(result.push_argv, ["push", "-u", "origin", "chore/issue-309-demo"])
        self.assertIn("review markers ok", result.stdout)

    def test_short_sha_prefix_marker_pushes(self) -> None:
        """复核 sha 前缀口径复用 review_marker.py：12 位短 sha 与 HEAD 视为同一 commit。"""
        _both_markers(self.repo, self.repo.head[:12])
        result = self.repo.run(ISSUE, "-u", "origin", "chore/issue-309-demo")
        self.assertEqual(result.returncode, 0, f"stderr={result.stderr!r}")
        self.assertTrue(result.push_ran)

    def test_push_exit_code_passthrough(self) -> None:
        """git push 的真实退出码原样透传（脚本不改写推送结论）。"""
        _both_markers(self.repo, self.repo.head)
        result = self.repo.run(ISSUE, "-u", "origin", "chore/issue-309-demo", push_rc=7)
        self.assertEqual(result.returncode, 7, f"stderr={result.stderr!r}")
        self.assertTrue(result.push_ran)

    def test_escape_hatch_pushes_and_reports_axes(self) -> None:
        """DZ_ALLOW_UNREVIEWED_PUSH=1：两轴缺失标记 + 当前 HEAD 打印后仍放行。"""
        result = self.repo.run(ISSUE, "-u", "origin", "chore/issue-309-demo",
                               allow_unreviewed=True)
        self.assertEqual(result.returncode, 0, f"stderr={result.stderr!r}")
        self.assertTrue(result.push_ran)
        self.assertIn("DZ_ALLOW_UNREVIEWED_PUSH=1", result.stderr)
        self.assertIn(self.repo.head, result.stderr)
        self.assertIn("(standards)", result.stderr)
        self.assertIn("(spec)", result.stderr)

    def test_escape_hatch_does_not_bypass_scope(self) -> None:
        """逃生口只覆盖 review 轴；scope 违规仍是硬拒绝（避免把别人的票推走）。"""
        self.repo.git("commit", "-q", "--allow-empty", "-m", "chore: out-of-scope commit")
        result = self.repo.run(ISSUE, "-u", "origin", "chore/issue-309-demo",
                               allow_unreviewed=True)
        self.assertEqual(result.returncode, 3, f"stderr={result.stderr!r}")
        self.assertFalse(result.push_ran)

    # ---- 参数 / 环境 --------------------------------------------------

    def test_usage_errors(self) -> None:
        for args, fragment in (((), "用法"), (("abc",), "issue number must be numeric"),
                               (("0",), "issue number must be positive")):
            result = self.repo.run(*args)
            self.assertEqual(result.returncode, 2, f"args={args!r} stderr={result.stderr!r}")
            self.assertIn(fragment, result.stderr + result.stdout)
            self.assertFalse(result.push_ran, f"args={args!r} must not push")

    def test_help(self) -> None:
        result = self.repo.run("--help")
        self.assertEqual(result.returncode, 0, f"stderr={result.stderr!r}")
        self.assertIn("push-ticket.sh", result.stdout)
        self.assertIn("DZ_ALLOW_UNREVIEWED_PUSH", result.stdout)
        self.assertFalse(result.push_ran)


class PushTicketSelftestCase(unittest.TestCase):
    """脚本自带 --selftest 必须在真实仓库里跑绿（临时仓库 + 假 git/gh，不联网）。"""

    def test_selftest_green(self) -> None:
        result = subprocess.run(
            [str(PUSH_TICKET), "--selftest"], cwd=REPO_ROOT,
            capture_output=True, text=True, timeout=300, check=False)
        self.assertEqual(result.returncode, 0, f"stderr={result.stderr!r}")
        self.assertIn("cases ok", result.stdout)


class ReviewMarkerContractCase(unittest.TestCase):
    """push-ticket 依赖的 review_marker 契约，以及 pre-push 依赖的 warning-only 保真度。"""

    def setUp(self) -> None:
        self.repo = TempRepo()

    def tearDown(self) -> None:
        self.repo.cleanup()

    def _marker(self, *args: str) -> subprocess.CompletedProcess:
        env = os.environ.copy()
        env["PATH"] = f"{self.repo.bin}{os.pathsep}{env.get('PATH', '')}"
        env["DZ_FAKE_DIR"] = str(self.repo.fake)
        env["DZ_REAL_GIT"] = REAL_GIT
        return subprocess.run(
            [sys.executable, str(self.repo.repo / "scripts" / "review_marker.py"), *args],
            cwd=self.repo.repo, capture_output=True, text=True, env=env,
            timeout=120, check=False)

    def test_default_check_stays_warning_only(self) -> None:
        """`.githooks/pre-push` 的契约：默认 check 缺标记也是 exit 0（只 warning）。"""
        result = self._marker("check", ISSUE)
        self.assertEqual(result.returncode, 0, f"stderr={result.stderr!r}")
        self.assertIn("no review marker", result.stderr)

    def test_strict_check_rejects_missing_marker(self) -> None:
        result = self._marker("check", ISSUE, "--strict")
        self.assertEqual(result.returncode, 3, f"stderr={result.stderr!r}")

    def test_expect_axis_reports_each_axis(self) -> None:
        result = self._marker("check", ISSUE, "--strict",
                              "--expect-axis", "standards", "--expect-axis", "spec")
        self.assertEqual(result.returncode, 3, f"stderr={result.stderr!r}")
        self.assertIn("(standards)", result.stderr)
        self.assertIn("(spec)", result.stderr)

    def test_strict_check_accepts_head_markers(self) -> None:
        _both_markers(self.repo, self.repo.head)
        result = self._marker("check", ISSUE, "--strict")
        self.assertEqual(result.returncode, 0, f"stderr={result.stderr!r}")


if __name__ == "__main__":
    unittest.main(verbosity=2)
