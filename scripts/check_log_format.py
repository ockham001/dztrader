#!/usr/bin/env python3
"""
check_log_format.py — dztrader 日志格式 lint 脚本

检测 spdlog / LastError::set / throw std::* / std::cerr 的文本是否符合项目规范:
  - 格式: prose | k=v k=v （hybrid 模式）
  - prose 全英文小写开头，无句号
  - 自由文本类 k=v 的值必须双引号包裹（R6，见下）
  - 禁止多行 banner（=== 装饰线）
  - 禁止单引号包裹占位符 '{}'（应用双引号或去引号）

`std::cerr` 是 header-only 策略 SDK 唯一的诊断出口（ADR 0021 无日志 API；组件文档
bar_strategy §6 记录了这个缺口），因此与 spdlog 同一套文本规则。#163/#165 的审查各抓到
一次只靠人眼发现的措辞漂移，本检查把它机械化（issue #213）。

**R6 引号规则**（#266 启用）：`error=` / `err=` / `detail=` / `msg=` / `exception=` /
`what=` / `text=` 的值一律要求 `k="{}"`。这条规则此前只在文档里写着、检查器从未实现
（#213 关闭时遗留，当时理由是「现存 24 处历史站点需单独清扫」——清扫最终在 #266 做完，
实际是 67 处站点 / 49 条日志字符串）。

`reason=` / `name=` / `path=` / `target=` / `value=` / `message=` **有意排除**：它们既
用于自由文本也用于枚举标识符（如 `reason=negative_volume`，有测试断言锁定），静态不可
判定；强行纳入会让测试变红（#266 第一版实测 8 个失败）。这几类 key 仍需人工判断。

不在本 lint 范围：UI 通知（notify_ui_*）与策略输出等**面向用户的展示文案**允许中文，
不按日志/错误文本规范检查；需要时由 review 按契约单独裁决。

用法:
  python scripts/check_log_format.py apps/ libs/
  python scripts/check_log_format.py --selftest     # 只跑规则自测（正/反样本）
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

# R6（#266）：自由文本类 k=v 的值必须双引号包裹（AGENTS §四「值含空格/等号/引号用双引号」）。
#
# 只覆盖「值**必然**是自由文本」的 key —— 它们的来源固定是 e.what() / sqlite3_errmsg() /
# 异常消息，几乎一定含空格或冒号（如 `no such table: orders`）。
#
# 边界是**实测**出来的，不是猜的（#266 第一版把 reason 也纳入，直接让 8 个测试变红）：
#   - `reason=`   两种用法并存：`reason="{}"` 传 e.what()（自由文本）与 `reason={}` 传
#     `negative_volume` / `invalid_price` 一类**枚举标识符**（AGENTS 规定枚举不加引号）。
#     静态无法区分，故排除。`libs/strategy_api/tests/bar_strategy_position_test.cpp` 有断言
#     `reason=negative_volume` 锁定这一点。
#   - `name/path/target/value/message=` 同理：可能是标识符也可能是自由文本，排除。
#     （`message=` 在本仓多为 UI 文案/结构化消息字段，不按日志 k=v 规则强制引号。）
# 纳入规则的 key，其全部现存站点在启用时都已核对过取值来源（见 #266 issue 的 AC）。
FREE_TEXT_KEYS = ("error", "err", "detail", "msg", "exception", "what", "text")
UNQUOTED_FREE_TEXT_RE = re.compile(r'(?<![\w"])(' + "|".join(FREE_TEXT_KEYS) + r')=\{\}')

# selftest 样本：(文本, 期望违规规则集合)。精确集合相等——既挡漏报，也挡「塌缩成永远报
# 同一条」的退化（与 check_json_vendor_version.py 同一套做法）。
SELFTEST_CASES: list[tuple[str, set[str]]] = [
    # 必须报
    ("db open failed | path={} error={}", {"log-format/unquoted-free-text"}),
    ("query failed | error={}", {"log-format/unquoted-free-text"}),
    ("io failed | detail={} msg={}", {"log-format/unquoted-free-text"}),
    ("cerr path | err={}", {"log-format/unquoted-free-text"}),
    # 已经合规
    ('db open failed | path={} error="{}"', set()),
    ('failed | error="{}" detail="{}"', set()),
    # **排除**的 key：枚举标识符与自由文本混用，静态不可判定 —— 现实证据见
    # bar_strategy_position_test.cpp 的 `reason=negative_volume` 断言
    ("intent rejected | instrument={} intent={} reason={}", set()),
    ("db rebuilt | path={} reason={} backup={}", set()),
    ("changed | name={} target={} value={} message={}", set()),
    # 不相关 key：ID/数字/枚举
    ("placed | order_id={} error_id={}", set()),
    ("frame | type={} state={} ret={} count={}", set()),
    # 单词里碰巧含 key 名不算（\b 锚定）
    ("not_a_key | myerror={} error_code={}", set()),
]


def run_selftest() -> int:
    """把已知正/反样本喂给 check_string，确认 R6 与既有规则的行为没漂移。"""
    failures = 0
    for text, expected in SELFTEST_CASES:
        got = {rule for rule, _, _ in check_string(text)}
        if got != expected:
            print(f"selftest FAIL {text!r}: expected {sorted(expected) or 'none'}, "
                  f"got {sorted(got) or 'none'}", file=sys.stderr)
            failures += 1
    if failures:
        print(f"selftest FAILED: {failures} case(s)", file=sys.stderr)
        return 1
    print(f"selftest: {len(SELFTEST_CASES)} log-string forms behave as expected")
    return 0



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

    # R6: unquoted-free-text — 自由文本类 k=v 的值必须双引号包裹（#266）
    for m in UNQUOTED_FREE_TEXT_RE.finditer(content):
        key = m.group(1)
        violations.append((
            "log-format/unquoted-free-text",
            f"free-text value '{key}={{}}' must be double-quoted: these values come "
            "from e.what()/sqlite3_errmsg() and usually contain spaces or colons "
            "(AGENTS §四)",
            content.replace(f"{key}={{}}", f'{key}="{{}}"'),
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
        nargs="*",
        type=Path,
        help="待扫描的目录或文件（如 apps/ libs/）；--selftest 时可省略",
    )
    parser.add_argument(
        "--extensions",
        default=".cpp,.h,.hpp,.cc",
        help="扫描的文件扩展名（逗号分隔，默认 .cpp,.h,.hpp,.cc）",
    )
    parser.add_argument(
        "--selftest",
        action="store_true",
        help="只跑规则自测（注入已知正/反样本，不扫仓库）",
    )
    args = parser.parse_args()

    if args.selftest:
        return run_selftest()
    if not args.paths:
        parser.error("paths are required unless --selftest is given")

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
