#!/usr/bin/env python3
"""
check_test_portability.py — 测试源码跨平台路径 lint

扫描 apps/、libs/ 与 strategies/ 下的测试文件，两条独立检查：

1. **禁止写死 POSIX 绝对路径**："/home/..."  "/Users/..."  "/root/..."
   这类字面量在 Linux 上能通过，在 Windows 上会被解析到当前盘符
   （例如 /home/user 变成 C:/home/user），导致 Windows CI 才发现失败。
   测试应使用 std::filesystem::temp_directory_path() / absolute() 构造路径。

2. **禁止固定名临时资源**（#136）：`temp_directory_path() / "dz_xxx"`，以及
   `channel_name = "dz_xxx"` / `.channel_name = "dz_xxx"` 这类固定 SHM 通道名
   （通道名是机器全局命名空间的一部分，并行用例会撞同一个 meta/页文件）
   `gtest_discover_tests` 把每个 TEST 注册成独立 ctest 用例，`ctest -j` 下同一
   二进制的多个用例是并发进程；固定名路径会被并发用例互相 remove_all/覆盖，
   产生与改动无关的偶发失败（实测 `DbConfigTest.StructuralViolationsThrowAndKeepFile`）。
   应改用 `libs/core/tests/test_temp_dir.h` 的 `dztrader::test::unique_temp_dir()`
   / `unique_temp_path()`（进程唯一：pid + 随机数）。

用法:
  python scripts/check_test_portability.py
  python scripts/check_test_portability.py --root <repo-root>

退出码: 0=通过, 1=有违规
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

DEFAULT_ROOT = Path(__file__).resolve().parents[1]
SCAN_DIRS = ("apps", "libs", "strategies")
FORBIDDEN = ('"/home/', '"/Users/', '"/root/')
EXCLUDED_PARTS = {"build", "node_modules", ".git"}

# 单个例外用**行内豁免**表达，不用整文件白名单：白名单会顺带豁免该文件后来新增的
# 真实固定名，而且唯一化实现本身其实并不产生匹配（它拼变量，不写字面量）。
# 用法：在命中行或紧邻上一行写 `// NOLINT(temp-dir-unique): <理由>`
NOLINT_MARKER = "NOLINT(temp-dir-unique)"


def is_test_file(path: Path) -> bool:
    """测试目录下的 C++ 文件，或独立的 *_test.cpp / *_test.h / test_*.cpp。"""
    if any(part in EXCLUDED_PARTS for part in path.parts):
        return False
    if "tests" in path.parts:
        return True
    name = path.name
    return name.endswith(("_test.cpp", "_test.h")) or (
        name.startswith("test_") and path.suffix == ".cpp"
    )


def iter_test_files(root: Path):
    for dirname in SCAN_DIRS:
        base = root / dirname
        if not base.is_dir():
            continue
        for pattern in ("*.cpp", "*.h"):
            for path in base.rglob(pattern):
                if is_test_file(path):
                    yield path


# temp_directory_path(...) / <字面量>，且该字面量是 `/` 的完整右操作数（后面没有 `+` 拼接）
# => 固定名。三种已知变体都要覆盖（复审实测的漏洞）：
#   1. 无参调用 vs 带 `ec` 重载：temp_directory_path() / temp_directory_path(ec)
#   2. 多级路径：temp_directory_path() / "dz_x" / "sub" —— 正是本票要修的原始 bug 形状
#   3. 跨行：`temp_directory_path()` 与字面量分处两行
# 拼接（pid/随机/unique_suffix 等）是运行期唯一，不算违规。
FIXED_TEMP_RE = re.compile(
    r'temp_directory_path\s*\([^)]*\)\s*/\s*'
    r'\(?\s*"([^"]*)"\s*'
    # 字面量之后：可选的闭合括号 + 允许继续 `/ "sub"` 多级；但不得是 `+` 拼接
    r'(?:\)?\s*(?:/\s*"[^"]*"\s*)*[;,)])')


# 固定 SHM 通道名：`channel_name = "dz_x"` / `.channel_name = "test_md"`，
# 后面不跟 `+` 拼接即为固定名。通道名进内核/文件命名空间，并行用例会互踩。
FIXED_CHANNEL_RE = re.compile(
    r'channel_name_?\s*=\s*"([A-Za-z0-9_.\-]+)"\s*[,;}A-Za-z)]')


def strip_comments(line: str) -> str:
    """去掉行尾 // 注释与整行 /* */ 注释，避免把注释里的示例当成违规。"""
    out = []
    i = 0
    in_block = False
    while i < len(line):
        two = line[i:i + 2]
        if in_block:
            if two == '*/':
                in_block = False
                i += 2
            else:
                i += 1
            continue
        if two == '/*':
            in_block = True
            i += 2
            continue
        if two == '//':
            break
        out.append(line[i])
        i += 1
    return ''.join(out)


def check_fixed_temp_paths(root: Path, path: Path,
                           lines: list[str]) -> list[tuple[int, str, str]]:
    """固定名临时路径（#136）：命中且不在白名单则违规。

    判定依据是「字面量直接构成 temp_directory_path() / 的完整右操作数」——
    变量拼接（channel_name、unique_suffix() 等）或字面量后继续 `+` 拼接
    天然唯一，不在此列。
    """
    found: list[tuple[int, str, str]] = []
    # 跨行形态：把整个文件的非注释行按 `;` 语义拼不动，改用「当前行 + 后续 2 行」的窗口，
    # 命中时报告窗口起点行号。
    for idx, raw in enumerate(lines):
        stripped = raw.strip()
        if stripped.startswith(("*", "//")):
            continue  # 注释块里的说明文字
        line = strip_comments(raw)
        if "temp_directory_path" not in line and "channel_name" not in line:
            continue
        window = line + "".join(strip_comments(x) for x in lines[idx + 1:idx + 3])
        # 行内豁免：本行或上一行带 NOLINT 标记
        near = line + (lines[idx - 1] if idx > 0 else "")
        if NOLINT_MARKER in near:
            continue
        match = FIXED_TEMP_RE.search(window) or FIXED_CHANNEL_RE.search(line)
        if match:
            found.append((idx + 1, match.group(1), stripped))
    return found


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=DEFAULT_ROOT,
                        help="仓库根目录（默认：脚本所在仓库）")
    args = parser.parse_args()
    root = args.root.resolve()
    if not root.is_dir():
        print(f"error: root is not a directory: {root}", file=sys.stderr)
        return 2

    violations: list[tuple[Path, int, str]] = []
    fixed_temp: list[tuple[Path, int, str, str]] = []
    scanned = 0
    for path in sorted(set(iter_test_files(root))):
        scanned += 1
        try:
            lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
        except OSError as exc:
            print(f"warning: cannot read {path}: {exc}", file=sys.stderr)
            continue
        for lineno, line in enumerate(lines, 1):
            if any(token in line for token in FORBIDDEN):
                violations.append((path, lineno, line.strip()))
        for lineno, name, line in check_fixed_temp_paths(root, path, lines):
            fixed_temp.append((path, lineno, name, line))

    failed = False
    if violations:
        for path, lineno, line in violations:
            rel = path.relative_to(root)
            print(f"{rel}:{lineno}: hardcoded POSIX absolute path: {line}")
        print(f"FAILED: {len(violations)} hardcoded POSIX path violation(s) "
              f"in {scanned} test files")
        failed = True

    if fixed_temp:
        for path, lineno, name, line in fixed_temp:
            rel = path.relative_to(root)
            print(f"{rel}:{lineno}: fixed-name temp path \"{name}\" collides under "
                  f"ctest -j (use dztrader::test::unique_temp_dir/unique_temp_path): {line}")
        print(f"FAILED: {len(fixed_temp)} fixed-name temp path violation(s) "
              f"in {scanned} test files; see libs/core/tests/test_temp_dir.h (#136)")
        failed = True

    if failed:
        return 1

    print(f"OK: {scanned} test files scanned, no hardcoded POSIX home paths, "
          f"no fixed-name temp paths")
    return 0


def selftest() -> int:
    """检查器自测：确认三种已知绕过形态都被拦住，且唯一化写法不误报。

    存在的理由（#136）：这些形态曾被逐行匹配漏掉——多级路径正是本票要修的原始 bug
    形状，`temp_directory_path(ec)` 是合法重载，折行是 clang-format ColumnLimit=100
    的常规产物。没有自测的话，正则被"简化"回逐行匹配也没人会发现。
    """
    cases = [
        # (说明, 代码, 是否应报)
        ("单行固定名", 'auto a = std::filesystem::temp_directory_path() / "dz_x";', True),
        ("ec 重载", 'auto b = std::filesystem::temp_directory_path(ec) / "dz_x";', True),
        ("多级固定路径", 'auto c = std::filesystem::temp_directory_path() / "dz_x" / "sub";', True),
        ("折行", 'auto d = std::filesystem::temp_directory_path() /\n"dz_x";', True),
        ("固定通道名", 'channel_name_ = "dz_x";', True),
        ("拼接后缀（唯一）", 'auto e = std::filesystem::temp_directory_path() / ("dz_x_" + suffix);', False),
        ("变量（唯一）", 'auto f = std::filesystem::temp_directory_path() / channel_name;', False),
        ("行内注释", 'int z = 0;  // temp_directory_path() / "dz_x";', False),
    ]
    failures = 0
    for label, code, should_flag in cases:
        lines = code.split("\n")
        # 单行用例补一个前导空行，避免命中"上一行 NOLINT"分支
        hits = check_fixed_temp_paths(Path("."), Path("probe_test.cpp"), [""] + lines)
        flagged = bool(hits)
        if flagged != should_flag:
            print(f"selftest FAIL: {label}: expected flagged={should_flag}, got {flagged}")
            failures += 1
    if failures:
        return 1
    print(f"selftest OK: {len(cases)} cases (3 bypass forms caught, 3 unique forms allowed)")
    return 0


if __name__ == "__main__":
    if "--selftest" in sys.argv:
        sys.exit(selftest())
    sys.exit(main())
