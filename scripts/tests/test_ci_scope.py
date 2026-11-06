#!/usr/bin/env python3
"""tests for scripts/ci_scope.py — PR 改动面分类器（#289 AC3）

这些用例锁定的是**分类口径**，不是实现细节：全部通过**进程级** CLI 驱动
（`python3 scripts/ci_scope.py ...`），断言真实 stdout 单行 `mode=...`、
stderr 理由行与退出码——而不是 import 内部函数。理由是分类器的唯一消费者是
ci.yml 的 scope job 与 preflight step，它们看到的只有 CLI 契约；只测内部函数
会放过「参数接线写错 / 退出码吞掉 / 输出混进 stderr」这类真实故障。

denylist 命中即 full 是安全方向：误判 fast = 绕过构建验证，所以每个 denylist
类别都有用例，未知路径必须 full。反向验证：把 `scripts/build.sh` 从 denylist
临时删掉，`test_build_entrypoint_is_full` 立即红。

跑法（会被 preflight 的 unittest discovery 与 ci_budget step 一起跑）：
    python3 -m unittest discover -s scripts/tests -k ci_scope -v
"""

from __future__ import annotations

import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "scripts" / "ci_scope.py"

# PR #254 的 merge commit：实测只含 docs/frame_contracts/*.md（10 个纯文档文件）。
# 这是 AC3 判据里的真实样本，比自造 diff 更能防「分类器只在小样本上对」。
DOCS_ONLY_COMMIT = "9467679"

MODE_LINE_RE = re.compile(r"^mode=(fast|full)$")


def run_cli(*args: str) -> subprocess.CompletedProcess:
    """以进程级方式运行分类器，返回完整结果（不经过 shell）。"""
    return subprocess.run(
        [sys.executable, str(SCRIPT), *args],
        capture_output=True,
        text=True,
        cwd=REPO_ROOT,
    )


def run_files_from(paths: list[str]) -> subprocess.CompletedProcess:
    """用 --files-from 临时文件驱动分类器（每行一个路径）。"""
    with tempfile.NamedTemporaryFile(
        "w", suffix=".txt", encoding="utf-8", delete=False
    ) as handle:
        handle.write("".join(f"{path}\n" for path in paths))
        list_path = handle.name
    try:
        return run_cli("--files-from", list_path)
    finally:
        Path(list_path).unlink(missing_ok=True)


class ScopeAssertions(unittest.TestCase):
    def assert_mode(self, result: subprocess.CompletedProcess, expected: str) -> None:
        """断言退出码 0、stdout 恰好一行 mode=<expected>、stderr 有理由行。"""
        self.assertEqual(result.returncode, 0, result.stderr)
        stdout_lines = result.stdout.splitlines()
        self.assertEqual(len(stdout_lines), 1, f"stdout must be single line: {result.stdout!r}")
        self.assertRegex(stdout_lines[0], MODE_LINE_RE)
        self.assertEqual(stdout_lines[0], f"mode={expected}")
        self.assertIn("ci-scope:", result.stderr, "a human-readable reason must go to stderr")


class TestSelftest(ScopeAssertions):
    def test_selftest_passes(self):
        result = run_cli("--selftest")
        self.assertEqual(result.returncode, 0, result.stderr)
        match = re.search(r"selftest: (\d+) cases", result.stdout)
        self.assertIsNotNone(match, result.stdout)
        self.assertGreaterEqual(int(match.group(1)), 10)


class TestClassifyByFilesFrom(ScopeAssertions):
    """AC3 判据列出的全部分类面（进程级，--files-from 驱动）。"""

    def test_empty_diff_is_fast(self):
        self.assert_mode(run_files_from([]), "fast")

    def test_docs_only_is_fast(self):
        result = run_files_from(["docs/frame_contracts/general.md", "docs/README.md"])
        self.assert_mode(result, "fast")

    def test_scripts_only_is_fast(self):
        # 本脚本自身在 scripts/** 下 -> fast；分类器与新增检查器由 preflight 验证。
        result = run_files_from(
            ["scripts/ci_scope.py", "scripts/tests/test_ci_scope.py", "scripts/preflight.sh"]
        )
        self.assert_mode(result, "fast")

    def test_build_entrypoint_is_full(self):
        # 反向验证点：从 denylist 删掉 scripts/build* 会让本用例立即失败。
        self.assert_mode(run_files_from(["scripts/build.sh"]), "full")

    def test_single_cpp_is_full(self):
        self.assert_mode(run_files_from(["libs/core/src/foo.cpp"]), "full")

    def test_docs_plus_scripts_mix_is_fast(self):
        self.assert_mode(
            run_files_from(["docs/a.md", "scripts/check_docs_links.py", ".githooks/pre-push"]),
            "fast",
        )

    def test_fast_face_plus_cpp_is_full(self):
        self.assert_mode(
            run_files_from(["docs/a.md", "scripts/check_docs_links.py", "libs/core/src/foo.cpp"]),
            "full",
        )

    def test_workflow_file_is_full(self):
        self.assert_mode(run_files_from([".github/workflows/ci.yml"]), "full")

    def test_githooks_is_fast(self):
        self.assert_mode(run_files_from([".githooks/pre-push"]), "fast")

    def test_frontend_file_is_full(self):
        self.assert_mode(run_files_from(["apps/webui/frontend/src/x.ts"]), "full")


class TestDenylistCategories(ScopeAssertions):
    """denylist 每一类各一条，防止某条规则被改坏而无人发现。"""

    def test_header_is_full(self):
        self.assert_mode(run_files_from(["libs/core/include/dztrader/core/foo.h"]), "full")

    def test_cmake_files_are_full(self):
        self.assert_mode(run_files_from(["CMakeLists.txt"]), "full")
        self.assert_mode(run_files_from(["CMakePresets.json"]), "full")

    def test_conanfile_is_full(self):
        self.assert_mode(run_files_from(["conanfile.py"]), "full")
        self.assert_mode(run_files_from(["conanfile.txt"]), "full")

    def test_profiles_are_full(self):
        self.assert_mode(run_files_from(["profiles/linux-gcc"]), "full")

    def test_test_and_setup_scripts_are_full(self):
        self.assert_mode(run_files_from(["scripts/test.sh"]), "full")
        self.assert_mode(run_files_from(["scripts/setup.ps1"]), "full")

    def test_install_ccache_buildlock_scripts_are_full(self):
        for path in (
            "scripts/install-artifacts.sh",
            "scripts/ccache-env.sh",
            "scripts/build-lock.sh",
        ):
            with self.subTest(path=path):
                self.assert_mode(run_files_from([path]), "full")

    def test_denylist_wins_over_markdown_face(self):
        # apps/webui/frontend/README.md 是 *.md，但它在 denylist 前缀下 -> full。
        self.assert_mode(run_files_from(["apps/webui/frontend/README.md"]), "full")

    def test_unknown_path_is_full(self):
        # 保守：只有明确属于 fast 面才 fast，新增目录不会被默认信任。
        self.assert_mode(run_files_from(["unknown/dir/file.txt"]), "full")


class TestCliErrors(ScopeAssertions):
    def test_bad_range_exits_2(self):
        result = run_cli("--range", "not-a-real-range..nope")
        self.assertEqual(result.returncode, 2, result.stdout)
        self.assertIn("error:", result.stderr)
        self.assertEqual(result.stdout, "")

    def test_missing_files_from_exits_2(self):
        result = run_cli("--files-from", str(REPO_ROOT / "no-such-list.txt"))
        self.assertEqual(result.returncode, 2, result.stdout)
        self.assertIn("error:", result.stderr)

    def test_no_arguments_exits_2(self):
        # argparse 的用法错误退出码也是 2：CI 把 2 视为「分类失败」并应回退全量语义。
        result = run_cli()
        self.assertEqual(result.returncode, 2, result.stdout)


class TestRealCommitRange(ScopeAssertions):
    """AC3 判据：`--range 9467679^1..9467679` 必须输出 mode=fast（该 commit 纯 docs）。"""

    def _range_resolvable(self) -> bool:
        rev = f"{DOCS_ONLY_COMMIT}^1"
        proc = subprocess.run(
            ["git", "rev-parse", "--verify", "--quiet", rev],
            cwd=REPO_ROOT,
            capture_output=True,
            text=True,
        )
        return proc.returncode == 0

    def test_docs_only_commit_range_is_fast(self):
        if not self._range_resolvable():
            # 浅克隆（无父提交）时跳过：CI linux job 是 fetch-depth: 0，不会走到这里。
            self.skipTest(f"{DOCS_ONLY_COMMIT}^1 not available in this clone (shallow?)")
        result = run_cli("--range", f"{DOCS_ONLY_COMMIT}^1..{DOCS_ONLY_COMMIT}")
        self.assert_mode(result, "fast")


if __name__ == "__main__":
    unittest.main()
