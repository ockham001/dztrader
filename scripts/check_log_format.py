#!/usr/bin/env python3
"""
check_log_format.py — dztrader 日志格式 lint 脚本

检测 spdlog / LastError::set / throw std::* / std::cerr 的文本是否符合项目规范:
  - 格式: prose | k=v k=v （hybrid 模式）
  - prose 全英文小写开头，无句号
  - k=v 值含空格/等号/引号用双引号包裹
  - 禁止多行 banner（=== 装饰线）
  - 禁止单引号包裹占位符 '{}'（应用双引号或去引号）

`std::cerr` 是 header-only 策略 SDK 唯一的诊断出口（ADR 0021 无日志 API；组件文档
bar_strategy §6 记录了这个缺口），因此与 spdlog 同一套文本规则。#163/#165 的审查各抓到
一次只靠人眼发现的措辞漂移，本检查把它机械化（issue #213）。

`std::cerr` 的**引号规则**（`k={}` 该写成 `k="{}"`）暂不启用：现存 24 处历史站点，需要一次
单独清扫（见 #213）。其余规则（非英文 / 大写开头 / 句号结尾 / banner / 单引号占位符 /
missing-pipe）在现存站点上 backlog 为 0，直接生效。

不在本 lint 范围：UI 通知（notify_ui_*）与策略输出等**面向用户的展示文案**允许中文，
不按日志/错误文本规范检查；需要时由 review 按契约单独裁决。

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

# 其他非日志但受"错误文本规范"约束的调用（AGENTS §四：所有错误描述/异常消息）：
#   LastError::set(code, "fmt | k={}", args...)   —— C 接口边界的上下文消息
#   throw std::*("fmt | k={}")                     —— 异常消息
#   std::cerr << std::format("fmt | k={}", ...)    —— header-only SDK 的诊断出口（无 spdlog）
# UI 通知（notify_ui_*）面向用户，可中文，故意不在此列。
OTHER_CALL_RES = (
    re.compile(r'\bLastError::set\s*\('),
    re.compile(r'\bthrow\s+std::[A-Za-z_][A-Za-z0-9_]*(?:<[^>]*>)?\s*\('),
    # `std::cerr << "..."` 与 `std::cerr << std::format("...")`：两个形态都要抓
    # （前者是预先拼好的字符串，后者是格式化串）。
    re.compile(r'\bstd::cerr\s*<<\s*(?:std::format\s*\(\s*)?'),
)

CALL_RES = (SPDLOG_CALL_RE, *OTHER_CALL_RES)

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
    """从一行中提取受检调用的第一个字符串字面量。

    依次尝试 spdlog / LastError::set / throw std::*，返回 (字符串内容, 起始列号) 或 None。
    支持调用起始在同一行的跨行字符串（仅取首行内容）。
    """
    for call_re in CALL_RES:
        m = call_re.search(line)
        if not m:
            continue
        after_call = line[m.end():]
        sm = STRING_LITERAL_RE.search(after_call)
        if not sm:
            continue
        content = sm.group(1)
        column = m.end() + sm.start() + 1  # 1-based
        return content, column
    return None


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

    # R5: prose-shape — AGENTS §四「小写开头、无句号」。
    # 只看 prose（`|` 之前）：k=v 的值由 R1/R2 与引号规则管，值里出现句号是数据不是 prose。
    if prose_part and prose_part.strip():
        prose = prose_part.strip()
        if prose[0].isupper():
            violations.append((
                "log-format/prose-case",
                "prose must start lowercase (AGENTS §四)",
                prose[0].lower() + prose[1:],
            ))
        if prose.endswith(".") and not prose.endswith(".."):
            violations.append((
                "log-format/prose-period",
                "prose must not end with a period (AGENTS §四)",
                prose.rstrip("."),
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
