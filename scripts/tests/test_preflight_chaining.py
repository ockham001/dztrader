#!/usr/bin/env python3
"""test_preflight_chaining.py — preflight.sh 检查步骤的串联审计（#289 AC1）

为什么需要：`preflight.sh` 的每个 `check_*()` 函数是「先跑检查器自测、再扫真实树」的两条
命令。`set -u -o pipefail` **没有 `-e`**，`run_step` 只取函数返回值，所以第二条命令的 0
会把自测失败掩盖掉——step 打印 ok，而检查器已经坏了（门禁恒真）。#281/#284 修过 3 处，
#289 AC1 又发现 `check_test_portability` 漏网：同一失效形态已经复发，纯靠人眼是守不住的。

本文件把「每条 `check_*()` 函数里，除最后一条检查器调用外都必须显式 `|| return`」变成
可机械验证的断言，并顺带验证 `--help` 的步骤清单与脚本里的 `run_step` 一一对应
（#289 AC7：覆盖清单单源化，文档不再逐项手抄）。

跑法（被 preflight 的 check_ci_budget 里的 unittest discovery 自动执行）：
    python3 -m unittest discover -s scripts/tests -k chaining
    python3 scripts/tests/test_preflight_chaining.py -v

退出码：0=通过，1=有失败。
"""

from __future__ import annotations

import re
import subprocess
import sys
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
PREFLIGHT = REPO_ROOT / "scripts" / "preflight.sh"

# `check_xxx() {` 到行首 `}` 之间的函数体（preflight.sh 的 check 函数都不嵌套函数，
# 且 `}` 独占一行——这是本脚本的既有形态，改动该形态时本审计会解析失败并报错）。
FUNC_RE = re.compile(r"^(check_[a-z_]+)\(\) \{\n(.*?)^\}", re.MULTILINE | re.DOTALL)

# 函数体内的「检查器调用」：行首（**任意缩进**）就是引号包的变量展开（`"$PYTHON"` /
# `"$ROOT/..."` / `"$CF"`）。赋值（`ut_out="$(...)"`）、`if`/`printf`/`return` 与续行
# 都不算独立调用。缩进不设上限：`check_*` 里的调用可能落在 `if ...; then` 分支（8 空格），
# 早期版本只认 0–4 空格，于是 `check_ci_scope` 的 range 调用整个落在审计盲区里
# （#289 审查 blocking：删掉它前面的 `|| return $?` 审计仍全绿）。
INVOCATION_RE = re.compile(r'^\s*"\$[A-Za-z_{]')
CHAINED_RE = re.compile(r"\|\|\s*return\b")


def check_functions(script_text: str) -> dict[str, list[tuple[int, str]]]:
    """返回 {函数名: [(行号, 该行是否为检查器调用/是否有 || return)]}。

    返回的是**顶层语句行**（含非调用行），供调用方按需过滤；行号是脚本内 1-based。
    """
    line_starts = [0]
    for index, char in enumerate(script_text):
        if char == "\n":
            line_starts.append(index + 1)

    def line_of(offset: int) -> int:
        lo, hi = 0, len(line_starts) - 1
        while lo < hi:
            mid = (lo + hi + 1) // 2
            if line_starts[mid] <= offset:
                lo = mid
            else:
                hi = mid - 1
        return lo + 1

    functions: dict[str, list[tuple[int, str]]] = {}
    for match in FUNC_RE.finditer(script_text):
        name = match.group(1)
        body = match.group(2)
        body_start = match.start(2)
        statements: list[tuple[int, str]] = []
        offset = 0
        for raw in body.splitlines(keepends=True):
            stripped = raw.strip()
            if stripped and not stripped.startswith("#"):
                statements.append((line_of(body_start + offset), raw.rstrip("\n")))
            offset += len(raw)
        functions[name] = statements
    return functions


def unchained_check_calls(script_text: str) -> list[str]:
    """返回违反串联纪律的调用：`<函数名>:<行号>: <命令行>`。"""
    problems: list[str] = []
    functions = check_functions(script_text)
    for name, statements in functions.items():
        invocations = [
            (lineno, line) for lineno, line in statements if INVOCATION_RE.match(line)
        ]
        for lineno, line in invocations[:-1]:
            if not CHAINED_RE.search(line):
                problems.append(f"{name}:{lineno}: {line.strip()}")
    return problems


def invocation_count(script_text: str) -> int:
    return sum(
        1
        for statements in check_functions(script_text).values()
        for _lineno, line in statements
        if INVOCATION_RE.match(line)
    )


def _assert_audit_detects_bad_function(test: unittest.TestCase) -> None:
    """审计器自身必须先红后绿：注入的已知违例必须被拦住，串联形态不得误报。"""
    bad = "".join(
        [
            'check_bad() {\n',
            '    "$PYTHON" scripts/check_x.py --selftest\n',
            '    "$PYTHON" scripts/check_x.py\n',
            '}\n',
        ]
    )
    problems = unchained_check_calls(bad)
    test.assertEqual(len(problems), 1, f"expected the unchained form to be flagged: {problems}")
    test.assertIn("check_bad", problems[0])

    good = "".join(
        [
            'check_good() {\n',
            '    "$PYTHON" scripts/check_x.py --selftest || return $?\n',
            '    "$PYTHON" scripts/check_x.py\n',
            '}\n',
        ]
    )
    test.assertEqual(unchained_check_calls(good), [])

    # 单条调用不需要串联；续行（8 空格）不算独立调用，不能误报。
    single = "".join(
        [
            'check_single() {\n',
            '    "$ROOT/scripts/ci-log.sh" --selftest\n',
            '}\n',
        ]
    )
    test.assertEqual(unchained_check_calls(single), [])
    pipeline = "".join(
        [
            'check_pipe() {\n',
            '    find libs apps \\( -name \'*.h\' \\) -print0 |\n',
            '        xargs -0 "$CF" --dry-run --Werror\n',
            '}\n',
        ]
    )
    test.assertEqual(unchained_check_calls(pipeline), [])

    # if 分支里的调用（8 空格缩进）必须同样被审计：#289 审查实测的盲区是
    # `check_ci_scope` 的 range 调用藏在 `if git rev-parse ...; then` 里，
    # 旧正则只认 0–4 空格，删掉它前面的 `|| return $?` 审计仍然全绿。
    indented_bad = "".join(
        [
            'check_indented() {\n',
            '    "$PYTHON" scripts/check_x.py --selftest\n',
            '    if git rev-parse --verify --quiet origin/main >/dev/null 2>&1; then\n',
            '        "$PYTHON" scripts/check_x.py --range origin/main...HEAD\n',
            '    fi\n',
            '    "$PYTHON" scripts/check_x.py\n',
            '}\n',
        ]
    )
    problems = unchained_check_calls(indented_bad)
    test.assertEqual(
        len(problems), 2, f"nested/indented invocations must be audited too: {problems}"
    )
    indented_good = indented_bad.replace(
        '    "$PYTHON" scripts/check_x.py --selftest\n',
        '    "$PYTHON" scripts/check_x.py --selftest || return $?\n',
    ).replace(
        '        "$PYTHON" scripts/check_x.py --range origin/main...HEAD\n',
        '        "$PYTHON" scripts/check_x.py --range origin/main...HEAD || return $?\n',
    )
    test.assertEqual(unchained_check_calls(indented_good), [])


class PreflightChainingTest(unittest.TestCase):
    def setUp(self) -> None:
        if not PREFLIGHT.is_file():
            self.fail(f"preflight.sh not found: {PREFLIGHT}")
        self.script_text = PREFLIGHT.read_text(encoding="utf-8")

    def test_audit_detects_known_bad_form(self) -> None:
        _assert_audit_detects_bad_function(self)

    def test_every_modifier_check_function_chains(self) -> None:
        problems = unchained_check_calls(self.script_text)
        self.assertEqual(
            problems,
            [],
            "preflight.sh 的检查器调用缺少 `|| return $?`（自测失败会被后一条命令的 0 覆盖，"
            "门禁变恒真）：\n" + "\n".join(f"  {problem}" for problem in problems),
        )

    def test_audit_covers_the_known_check_functions(self) -> None:
        """防审计器退化成恒绿：解析到的函数与多调用函数数量都必须有下界。"""
        functions = check_functions(self.script_text)
        self.assertGreaterEqual(
            len(functions), 10, f"parsed too few check functions: {sorted(functions)}"
        )
        multi = [
            name
            for name, statements in functions.items()
            if sum(1 for _lineno, line in statements if INVOCATION_RE.match(line)) >= 2
        ]
        self.assertGreaterEqual(
            len(multi),
            6,
            f"too few two-command check functions parsed ({sorted(multi)}); "
            "the invocation pattern may have drifted",
        )
        self.assertGreaterEqual(invocation_count(self.script_text), 10)

    def test_help_lists_every_run_step(self) -> None:
        """#289 AC7：`--help` 的清单必须与脚本里的 run_step 一一对应（单一真相源）。"""
        result = subprocess.run(
            ["bash", str(PREFLIGHT), "--help"],
            capture_output=True,
            text=True,
            cwd=str(REPO_ROOT),
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        # 允许缩进：前端阶段那一步在 `if [ "$C_ONLY" -eq 0 ]` 里。
        script_steps = [
            match.group(1)
            for match in re.finditer(
                r'^[ \t]*run_step "([^"]+)"', self.script_text, re.MULTILINE
            )
        ]
        self.assertGreaterEqual(len(script_steps), 14)
        listed = [
            line[4:]
            for line in result.stdout.splitlines()
            if line.startswith("  - ") and line[4:] in script_steps
        ]
        self.assertEqual(
            listed,
            script_steps,
            "--help 打印的步骤清单与脚本里的 run_step 不一致（顺序或名称漂移）",
        )
        # 头部的步骤数与清单必须自洽：只断言「与 grep -c 相等」会被函数定义行蒙对。
        count_lines = [
            line for line in result.stdout.splitlines() if line.startswith("步骤（")
        ]
        self.assertEqual(len(count_lines), 1, "--help 应恰好打印一行步骤汇总")
        match = re.search(r"共 (\d+) 步", count_lines[0])
        self.assertIsNotNone(match, count_lines[0])
        self.assertEqual(int(match.group(1)), len(script_steps), count_lines[0])


if __name__ == "__main__":
    unittest.main()
