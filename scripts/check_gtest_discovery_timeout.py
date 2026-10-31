#!/usr/bin/env python3
"""check_gtest_discovery_timeout.py — gtest 用例发现超时纪律 lint（#227）

为什么需要：`gtest_discover_tests` 在 **POST_BUILD** 阶段执行测试二进制枚举用例，
CMake 的默认发现超时只有 5 秒（`Modules/GoogleTest.cmake`：
`if(NOT arg_DISCOVERY_TIMEOUT) set(arg_DISCOVERY_TIMEOUT 5)`）。CMake 只提供全局的
`CMAKE_GTEST_DISCOVER_TESTS_DISCOVERY_MODE`，**没有**全局超时变量 —— 只能逐个调用点
显式传参。漏传就退回 5 秒，这个退化不可见：Linux 上从不接近边界，只在 Windows CI
冷启动（sccache READ_ONLY、近千 target 并发链接、杀毒/IO 抖动）时炸成
`Process terminated due to timeout` —— 已两次与改动内容无关地变红
（run 40856647679 / run 41167289411）。

本仓的收敛形态是 `cmake/DzTest.cmake` 的 `dz_add_test()`：`add_executable` +
`RUNTIME_OUTPUT_DIRECTORY` + `gtest_discover_tests(... DISCOVERY_TIMEOUT ...)` 收进一处，
调用点只写一行。因此本检查器守三条：

  R1 除 `cmake/DzTest.cmake` 外，任何 CMake 文件不得直接调 `gtest_discover_tests`
     —— 直接调用必然绕过宏里的超时参数（这正是「改一处漏一处」的入口）
  R2 `cmake/DzTest.cmake` 里的 `gtest_discover_tests` 必须显式带 `DISCOVERY_TIMEOUT`
  R3 `DZ_GTEST_DISCOVERY_TIMEOUT` 默认值必须 ≥ 30 秒（#227 两次 red 的实测结论；
     改小等于把已知的 flaky 重新引入）

刻意不校验的东西（已知边界，均有意的，不是漏洞）：
  - 不做 CMake 变量展开：`set(F gtest_discover_tests)` 再用变量名调用的间接形态查不到
  - token 与 `(` 分处两行、或宏内把超时写死成 `DISCOVERY_TIMEOUT 5`（不是读默认值）不报
  - 同一条语句内出现多次调用只按「语句包含 token」计一次；因为 R2 只要求语句里
    出现 DISCOVERY_TIMEOUT，多调用叠加时仍会逐个通过各自的参数检查
  这些边界都是「不解析 CMake 语义」的代价与有意取舍：CMake 是配置期语言，过度解析的
  检查器本身会成为维护负担；三条规则覆盖的正是 #227 的真实失败形态

用法:
  python scripts/check_gtest_discovery_timeout.py                # 扫本仓 tracked CMake 文件
  python scripts/check_gtest_discovery_timeout.py <file> [...]   # 显式文件（自测用）
  python scripts/check_gtest_discovery_timeout.py --selftest     # 检查器自测（临时目录）

退出码: 0=通过, 1=有违规
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

DEFAULT_ROOT = Path(__file__).resolve().parents[1]

# 唯一允许直接调用 gtest_discover_tests 的地方（宏的定义处）。
MODULE_REL = "cmake/DzTest.cmake"

# #227 的实测下限：Windows runner 冷启动下 5 秒不够，30 秒 = 正常耗时的数百倍，
# 又远小于用例级 TIMEOUT（60~120s），不会把真挂死伪装成正常等待。
MIN_DISCOVERY_TIMEOUT_SEC = 30

DISCOVER_RE = "gtest_discover_tests"
TIMEOUT_ARG_RE = "DISCOVERY_TIMEOUT"
DEFAULT_TIMEOUT_SET_RE = r"set(DZ_GTEST_DISCOVERY_TIMEOUT"
EXCLUDED_PARTS = {"build", "install", ".cache", ".git", "node_modules",
                  "third_party", ".worktrees", "__pycache__"}


def is_cmake_file(rel: str) -> bool:
    path = Path(rel)
    if any(part in EXCLUDED_PARTS for part in path.parts):
        return False
    return path.name == "CMakeLists.txt" or path.suffix == ".cmake"


def strip_comment(line: str) -> str:
    """去掉 CMake 行注释（`#` 起），双引号内的 `#` 是字面量。

    注释里的示例、历史说明不算违规 —— 否则为了通过门禁只能删掉解释性注释，
    正好和「注释随实现漂移」的审查条款打架。
    """
    in_quotes = False
    i = 0
    while i < len(line):
        ch = line[i]
        if ch == '"':
            in_quotes = not in_quotes
        elif ch == "#" and not in_quotes:
            return line[:i]
        i += 1
    return line


def code_lines(text: str) -> list[str]:
    return [strip_comment(line) for line in text.splitlines()]


def strip_string_literals(line: str) -> str:
    """把双引号字符串的内容清空（保留引号），用于区分「调用」与「文案里的名字」。

    CMake 里 `CACHE STRING` 的帮助文本、`message()` 文案都可能出现标识符；
    只看「行内含 token」会把这些字面量当成调用点，进而误报「漏了超时参数」
    （实测：宏自己的 CACHE 帮助文本就让本检查器在冻结树上自红）。
    """
    out = []
    in_quotes = False
    i = 0
    while i < len(line):
        ch = line[i]
        if ch == '"':
            out.append(ch)
            in_quotes = not in_quotes
        elif not in_quotes:
            out.append(ch)
        i += 1
    return "".join(out)


def is_call_site(line: str, token: str) -> bool:
    """该行是否出现一次**调用** `token(...)`（不在注释里、不在字符串字面量里）。

    判据是「去掉注释与字符串后，token 以标识符形式后接 `(`」。这比「行首即 token」
    更宽松（同一行前面可以写 `if(WIN32)` 之类的语句，CMake 允许紧凑写法），
    又不会像纯子串匹配那样把帮助文本误报成调用点。
    """
    code = strip_string_literals(strip_comment(line))
    return re.search(rf"(?<![\w-]){re.escape(token)}\s*\(", code) is not None


def statements_containing(lines: list[str], token: str) -> list[str]:
    """返回「以 token 开头的调用语句」的完整文本（按括号深度拼到配对右括号）。

    用于 R2：`gtest_discover_tests(...)` 常跨行书写，只看命中行会误判超时参数缺失。
    """
    found: list[str] = []
    for idx, line in enumerate(lines):
        if not is_call_site(line, token):
            continue
        chunk: list[str] = []
        depth = 0
        started = False
        for cur in lines[idx:]:
            chunk.append(cur)
            for ch in cur:
                if ch == "(":
                    depth += 1
                    started = True
                elif ch == ")":
                    depth -= 1
            if started and depth <= 0:
                break
        found.append("\n".join(chunk))
    return found


def timeout_default(text: str) -> int | None:
    """解析 `set(DZ_GTEST_DISCOVERY_TIMEOUT <n> ...)` 的默认值（只看代码行）。"""
    for line in code_lines(text):
        stripped = line.strip()
        if not stripped.startswith(DEFAULT_TIMEOUT_SET_RE):
            continue
        rest = stripped[len(DEFAULT_TIMEOUT_SET_RE):].strip()
        value = rest.split()[0] if rest.split() else ""
        if value.isdigit():
            return int(value)
    return None


def check_file(root: Path, path: Path) -> list[str]:
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError as exc:  # pragma: no cover - 读失败按环境错误处理
        return [f"{path}: cannot read: {exc}"]
    lines = code_lines(text)
    rel = path.relative_to(root).as_posix()
    errors: list[str] = []

    if rel != MODULE_REL:
        # R1: 直接调用绕过了 dz_add_test 里的 DISCOVERY_TIMEOUT
        for lineno, line in enumerate(lines, 1):
            if is_call_site(line, DISCOVER_RE):
                errors.append(
                    f"{rel}:{lineno}: R1 direct gtest_discover_tests call -> use "
                    f"dz_add_test() from {MODULE_REL} instead (#227: the macro is the only "
                    f"place that sets DISCOVERY_TIMEOUT)")
        return errors

    # R2: 宏内部的调用必须显式给超时
    for stmt in statements_containing(lines, DISCOVER_RE):
        if TIMEOUT_ARG_RE not in stmt:
            errors.append(
                f"{rel}: R2 gtest_discover_tests without DISCOVERY_TIMEOUT -> falls back to "
                f"CMake's 5s default, which reddened Windows CI twice (#227)")

    # R3: 默认值不得低于实测下限
    value = timeout_default(text)
    if value is None:
        errors.append(
            f"{rel}: R3 missing set(DZ_GTEST_DISCOVERY_TIMEOUT <seconds>) default (#227)")
    elif value < MIN_DISCOVERY_TIMEOUT_SEC:
        errors.append(
            f"{rel}: R3 DZ_GTEST_DISCOVERY_TIMEOUT={value}s is below the {MIN_DISCOVERY_TIMEOUT_SEC}s "
            f"floor measured in #227; a smaller value reintroduces the Windows flake")

    return errors


def default_files(root: Path) -> list[Path]:
    """本仓 tracked 的 CMake 文件（build/、.cache/ 等运行产物不扫）。"""
    out = subprocess.run(["git", "-C", str(root), "ls-files", "-z"],
                         capture_output=True, check=True).stdout
    rels = [name for name in out.decode().split("\0") if name and is_cmake_file(name)]
    return [root / rel for rel in rels]


def run(root: Path, files: list[Path]) -> int:
    errors: list[str] = []
    for path in files:
        errors.extend(check_file(root, path))

    if errors:
        for err in errors:
            print(err)
        print(f"FAILED: {len(errors)} gtest discovery-timeout violation(s) in "
              f"{len(files)} CMake file(s); see {MODULE_REL} (#227)")
        return 1

    print(f"OK: {len(files)} CMake file(s) scanned, all test discovery goes through "
          f"dz_add_test with DISCOVERY_TIMEOUT >= {MIN_DISCOVERY_TIMEOUT_SEC}s")
    return 0


def selftest() -> int:
    """检查器自测：三条规则各注入一次已知违例，并确认合法形态与注释示例不误报。

    存在的理由与其它 check 脚本一致：规则本身也会写错（#191 的路径检查曾恒真）。
    这些样本就是 #227 的真实失败形态 —— 直接调用（绕过宏）、宏里漏参数、默认值被改小，
    外加「标识符只出现在字符串/注释里」这种必须**不**误报的形态。
    """
    failures = 0
    cases = [
        # (文件名, 内容, 是否应报)
        ("libs/x/CMakeLists.txt",
         "add_executable(t a.cpp)\ngtest_discover_tests(t PROPERTIES LABELS fast)\n", True),
        ("libs/x/CMakeLists.txt",
         "dz_add_test(t SOURCES a.cpp LABELS fast)\n", False),
        ("libs/x/CMakeLists.txt",
         "# 历史写法示例：gtest_discover_tests(t PROPERTIES LABELS fast)\n"
         "dz_add_test(t SOURCES a.cpp LABELS fast)\n", False),
        (MODULE_REL,
         "set(DZ_GTEST_DISCOVERY_TIMEOUT 30 CACHE STRING \"x\")\n"
         "function(dz_add_test target)\n"
         "  gtest_discover_tests(${target} PROPERTIES LABELS fast)\n"
         "endfunction()\n", True),
        (MODULE_REL,
         "set(DZ_GTEST_DISCOVERY_TIMEOUT 30 CACHE STRING \"x\")\n"
         "function(dz_add_test target)\n"
         "  gtest_discover_tests(${target}\n"
         "    PROPERTIES LABELS fast\n"
         "    DISCOVERY_TIMEOUT ${DZ_GTEST_DISCOVERY_TIMEOUT})\n"
         "endfunction()\n", False),
        (MODULE_REL,
         "set(DZ_GTEST_DISCOVERY_TIMEOUT 5 CACHE STRING \"x\")\n"
         "function(dz_add_test target)\n"
         "  gtest_discover_tests(${target} PROPERTIES LABELS fast"
         " DISCOVERY_TIMEOUT ${DZ_GTEST_DISCOVERY_TIMEOUT})\n"
         "endfunction()\n", True),
        (MODULE_REL,
         "function(dz_add_test target)\n"
         "  gtest_discover_tests(${target} PROPERTIES LABELS fast"
         " DISCOVERY_TIMEOUT 30)\n"
         "endfunction()\n", True),
        # 标识符出现在字符串字面量里不是调用点：CACHE 帮助文本 / message 文案。
        # 这条曾让检查器在冻结树上自红（宏自己的帮助文本被当成了漏参数的调用）。
        (MODULE_REL,
         "set(DZ_GTEST_DISCOVERY_TIMEOUT 30 CACHE STRING\n"
         "    \"gtest_discover_tests discovery timeout in seconds\")\n"
         "function(dz_add_test target)\n"
         "  # gtest_discover_tests 是本宏唯一允许的调用点\n"
         "  gtest_discover_tests(${target} PROPERTIES LABELS fast"
         " DISCOVERY_TIMEOUT ${DZ_GTEST_DISCOVERY_TIMEOUT})\n"
         "endfunction()\n", False),
        ("libs/x/CMakeLists.txt",
         "# 说明文本里提到 gtest_discover_tests(...) 不算直接调用\n"
         "dz_add_test(t SOURCES a.cpp LABELS fast)\n", False),
        # 字符串字面量里出现带括号的完整调用形态：仅靠「token 后接 (」会误报，
        # 必须先去字符串再判定（CACHE 帮助文本/告警文案会这么写）。
        (MODULE_REL,
         "set(DZ_GTEST_DISCOVERY_TIMEOUT 30 CACHE STRING\n"
         "    \"passed to gtest_discover_tests(target) by dz_add_test\")\n"
         "function(dz_add_test target)\n"
         "  gtest_discover_tests(${target} PROPERTIES LABELS fast"
         " DISCOVERY_TIMEOUT ${DZ_GTEST_DISCOVERY_TIMEOUT})\n"
         "endfunction()\n", False),
        # 紧凑写法：调用前同一行还有别的语句。早期用「行首即 token」判定会漏掉它
        # （复审实测暴露），故判据改为「去注释/去字符串后以标识符形式后接 (」。
        ("libs/x/CMakeLists.txt",
         "if(WIN32)\n  gtest_discover_tests(t PROPERTIES LABELS fast)\nendif()\n", True),
        ("libs/x/CMakeLists.txt",
         "if(WIN32) gtest_discover_tests(t PROPERTIES LABELS fast) endif()\n", True),
    ]

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        for idx, (rel, content, should_flag) in enumerate(cases):
            path = root / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content, encoding="utf-8")
            errors = check_file(root, path)
            flagged = bool(errors)
            if flagged != should_flag:
                print(f"selftest FAIL: case {idx} ({rel}): expected flagged={should_flag}, "
                      f"got {flagged}: {errors}")
                failures += 1

    if failures:
        return 1
    print(f"selftest OK: {len(cases)} cases (direct call / missing arg / low default caught; "
          f"macro form and comment examples allowed)")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("files", nargs="*", type=Path,
                        help="显式文件列表（默认扫描本仓 tracked CMake 文件）")
    parser.add_argument("--root", type=Path, default=DEFAULT_ROOT,
                        help="仓库根目录（默认：脚本所在仓库）")
    parser.add_argument("--selftest", action="store_true",
                        help="检查器自测（临时目录，不碰工作区）")
    args = parser.parse_args()

    if args.selftest:
        return selftest()

    root = args.root.resolve()
    if not root.is_dir():
        print(f"error: root is not a directory: {root}", file=sys.stderr)
        return 2

    if args.files:
        files = [path if path.is_absolute() else (root / path) for path in args.files]
    else:
        files = default_files(root)

    return run(root, sorted(files))


if __name__ == "__main__":
    sys.exit(main())
