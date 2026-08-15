#!/usr/bin/env python3
"""
check_log_format.py — dztrader 日志格式 lint 脚本

检测 spdlog 调用是否符合项目规范:
  - 格式: prose | k=v k=v （hybrid 模式）
  - prose 全英文小写开头，无句号
  - k=v 值含空格/等号/引号用双引号包裹
  - 禁止多行 banner（=== 装饰线）
  - 禁止单引号包裹占位符 '{}'（应用双引号或去引号）

用法:
  python scripts/check_log_format.py apps/ libs/
  python scripts/check_log_format.py --help

退出码: 0=通过, 1=有违规

豁免机制:
  行尾 // NOLINT(log-format)         豁免单行
  // NOLINTBEGIN(log-format)         豁免范围开始
  // NOLINTEND(log-format)           豁免范围结束
"""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path

# spdlog 调用正则：匹配 spdlog::info(... / SPDLOG_INFO(... 等
# 捕获组 1 = 函数名（info/warn/error/...），用于后续字符串提取
SPDLOG_CALL_RE = re.compile(
    r'\b(?:spdlog::|SPDLOG_)(trace|debug|info|warn|error|critical|TRACE|DEBUG|INFO|WARN|ERROR|CRITICAL)\s*\('
)

# 字符串字面量提取：从调用起始处找第一个 "..." （支持转义）
STRING_LITERAL_RE = re.compile(r'"((?:[^"\\]|\\.)*)"')

# 中文字符检测（CJK 统一表意文字 + 标点）
NON_ENGLISH_RE = re.compile(r'[\u4e00-\u9fff\u3000-\u303f\uff00-\uffef]')

# banner 装饰线检测
BANNER_RE = re.compile(r'^={5,}|^-{5,}|\*{5,}')


@dataclass
class Violation:
    file: Path
    line: int
    column: int
    rule: str
    message: str
    suggestion: str = ""


def extract_log_string(line: str) -> tuple[str, int] | None:
    """从一行中提取 spdlog 调用的第一个字符串字面量。

    返回 (字符串内容, 起始列号) 或 None。
    支持调用起始在同一行的跨行字符串（仅取首行内容）。
    """
    m = SPDLOG_CALL_RE.search(line)
    if not m:
        return None
    after_call = line[m.end():]
    sm = STRING_LITERAL_RE.search(after_call)
    if not sm:
        return None
    content = sm.group(1)
    column = m.end() + sm.start() + 1  # 1-based
    return content, column


def check_string(content: str) -> list[tuple[str, str, str]]:
    """检查字符串内容，返回 [(rule, message, suggestion), ...]。"""
    violations: list[tuple[str, str, str]] = []

    # R1: missing-pipe — prose 部分含 ": {}" 或 ": '"，应为 " | k={}"
    # 只检查 | 之前的部分（prose），| 之后的 k=v 值中的 : 不报错（如 listen=host:port）
    # 例: "database initialized at {}" → "database initialized | path={}"
    pipe_pos = content.find('|')
    prose_part = content if pipe_pos < 0 else content[:pipe_pos]
    if re.search(r':\s*\{[^}]*\}', prose_part) or re.search(r":\s*'[^']*'", prose_part):
        suggestion = content.replace(': ', ' | ', 1)
        # 尝试把 "at {}" 改为 "| path={}"
        suggestion = re.sub(r'\bat\s+\{([^}]*)\}', r'| path={\1}', suggestion)
        suggestion = re.sub(r'\bfrom:\s*\{([^}]*)\}', r'| path={\1}', suggestion)
        violations.append((
            "log-format/missing-pipe",
            "prose contains ': {}' or ': \"...\"', should use ' | k={}' separator",
            suggestion,
        ))

    # R2: single-quote-placeholder — 含 '{}' 单引号包裹占位符
    # 例: "username '{}' already" → "username={} "
    if "'{}'" in content or "'{}" in content:
        fixed = content.replace("'{}'", "{}").replace("'{}", "{}")
        violations.append((
            "log-format/single-quote-placeholder",
            "single-quoted placeholder '{}', use bare {} or k=\"{}\"",
            fixed,
        ))

    # R3: non-english — 含中文字符
    if NON_ENGLISH_RE.search(content):
        violations.append((
            "log-format/non-english",
            "contains non-English (Chinese) characters, must be all English",
            "translate to English",
        ))

    # R4: banner-style — 纯装饰线
    if BANNER_RE.match(content.strip()):
        violations.append((
            "log-format/banner-style",
            "decorative banner line, merge to single-line k=v",
            "SPDLOG_WARN(\"summary | k=v\")",
        ))

    return violations


def check_file(path: Path) -> list[Violation]:
    """检查单个文件，返回违规列表。"""
    try:
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return []

    violations: list[Violation] = []
    in_nolint_block = False

    for idx, line in enumerate(lines, start=1):
        stripped = line.strip()

        # NOLINT 块控制
        if "NOLINTBEGIN(log-format)" in stripped:
            in_nolint_block = True
            continue
        if "NOLINTEND(log-format)" in stripped:
            in_nolint_block = False
            continue
        # 单行豁免
        if "NOLINT(log-format)" in stripped:
            continue
        if in_nolint_block:
            continue

        result = extract_log_string(line)
        if not result:
            continue

        content, column = result
        for rule, msg, suggestion in check_string(content):
            violations.append(Violation(
                file=path,
                line=idx,
                column=column,
                rule=rule,
                message=msg,
                suggestion=suggestion,
            ))

    return violations


def format_violation(v: Violation) -> str:
    """格式化单条违规为标准 lint 输出。"""
    location = f"{v.file}:{v.line}:{v.column}"
    line1 = f"{location}: {v.rule}: {v.message}"
    if v.suggestion and v.suggestion != "translate to English":
        line2 = f'  suggestion: "{v.suggestion}"'
        return f"{line1}\n{line2}"
    return line1


def main() -> int:
    parser = argparse.ArgumentParser(
        description="dztrader 日志格式 lint（prose | k=v 规范）"
    )
    parser.add_argument(
        "paths",
        nargs="+",
        type=Path,
        help="待扫描的目录或文件（如 apps/ libs/）",
    )
    parser.add_argument(
        "--extensions",
        default=".cpp,.h,.hpp,.cc",
        help="扫描的文件扩展名（逗号分隔，默认 .cpp,.h,.hpp,.cc）",
    )
    args = parser.parse_args()

    exts = set(args.extensions.split(","))
    all_violations: list[Violation] = []
    files_scanned = 0

    for target in args.paths:
        if target.is_file():
            if target.suffix in exts:
                all_violations.extend(check_file(target))
                files_scanned += 1
        elif target.is_dir():
            for path in sorted(target.rglob("*")):
                if path.suffix in exts:
                    all_violations.extend(check_file(path))
                    files_scanned += 1

    if not all_violations:
        print(f"OK: {files_scanned} files scanned, no log-format violations", file=sys.stderr)
        return 0

    for v in all_violations:
        print(format_violation(v))

    print(
        f"\nFAIL: {len(all_violations)} violation(s) in {files_scanned} files",
        file=sys.stderr,
    )
    return 1


if __name__ == "__main__":
    sys.exit(main())
