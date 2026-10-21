#!/usr/bin/env python3
"""
check_test_portability.py — 测试源码跨平台路径 lint

扫描 apps/、libs/ 与 strategies/ 下的测试文件，禁止写死 POSIX 绝对路径：
  "/home/..."  "/Users/..."  "/root/..."

这类字面量在 Linux 上能通过，在 Windows 上会被解析到当前盘符
（例如 /home/user 变成 C:/home/user），导致 Windows CI 才发现失败。
测试应使用 std::filesystem::temp_directory_path() / absolute() 构造路径。

用法:
  python scripts/check_test_portability.py
  python scripts/check_test_portability.py --root <repo-root>

退出码: 0=通过, 1=有违规
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

DEFAULT_ROOT = Path(__file__).resolve().parents[1]
SCAN_DIRS = ("apps", "libs", "strategies")
FORBIDDEN = ('"/home/', '"/Users/', '"/root/')
EXCLUDED_PARTS = {"build", "node_modules", ".git"}


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

    if violations:
        for path, lineno, line in violations:
            rel = path.relative_to(root)
            print(f"{rel}:{lineno}: hardcoded POSIX absolute path: {line}")
        print(f"FAILED: {len(violations)} violation(s) in {scanned} test files")
        return 1

    print(f"OK: {scanned} test files scanned, no hardcoded POSIX home paths")
    return 0


if __name__ == "__main__":
    sys.exit(main())
