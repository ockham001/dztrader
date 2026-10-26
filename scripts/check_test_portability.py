#!/usr/bin/env python3
"""
check_test_portability.py — 测试源码跨平台路径 lint

扫描 apps/、libs/ 与 strategies/ 下的测试文件，两条独立检查：

1. **禁止写死 POSIX 绝对路径**："/home/..."  "/Users/..."  "/root/..."
   这类字面量在 Linux 上能通过，在 Windows 上会被解析到当前盘符
   （例如 /home/user 变成 C:/home/user），导致 Windows CI 才发现失败。
   测试应使用 std::filesystem::temp_directory_path() / absolute() 构造路径。

2. **禁止固定名临时目录/文件**（#136）：`temp_directory_path() / "dz_xxx"`
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

# 允许的固定名临时路径（白名单：唯一化实现本身，以及有意共享的只读目录根）。
# 新增条目必须写明理由 —— 白名单是审查结论，不是垃圾桶。
FIXED_TEMP_ALLOWLIST = {
    # 唯一化实现本身（全仓唯一出现固定名的合法位置）
    "libs/core/tests/test_temp_dir.h",
    # 进程唯一父根：返回 root / channel_name，root 由 unique_temp_dir 生成
    "libs/shm/tests/test_util.h",
    # boost::filesystem::unique_path 模板（%%%% 由 unique_path 展开，本身即唯一化）
    "libs/log/tests/test_main.cpp",
}


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


# temp_directory_path() 直接接一个完整字符串字面量 => 固定名。
# 只要字面量后面还有 `+` 拼接（pid/随机/unique_suffix 等），就是运行期唯一，不算违规。
FIXED_TEMP_RE = re.compile(r'temp_directory_path\(\)\s*/\s*\(?\s*"([^"]*)"\s*\)?\s*[;,)]')


def check_fixed_temp_paths(root: Path, path: Path,
                           lines: list[str]) -> list[tuple[int, str, str]]:
    """固定名临时路径（#136）：命中且不在白名单则违规。

    判定依据是「字面量直接构成 temp_directory_path() / 的完整右操作数」——
    变量拼接（channel_name、unique_suffix() 等）或字面量后继续 `+` 拼接
    天然唯一，不在此列。
    """
    rel = path.relative_to(root).as_posix()
    if rel in FIXED_TEMP_ALLOWLIST:
        return []
    found: list[tuple[int, str, str]] = []
    for lineno, line in enumerate(lines, 1):
        stripped = line.strip()
        if stripped.startswith(("*", "//")):
            continue  # 注释里的说明文字
        match = FIXED_TEMP_RE.search(line)
        if match:
            found.append((lineno, match.group(1), stripped))
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


if __name__ == "__main__":
    sys.exit(main())
