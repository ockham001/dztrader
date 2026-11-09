#!/usr/bin/env python3
"""check_shared_test_headers.py — 共享测试头（夹具/工具）登记的确定性检查

为什么需要：docs/unit-test-standard.md §5.4 的「共享测试头（夹具/工具）索引」原本是
手工台账——新写一个测试头、两个以上测试 .cpp 开始包含它，却忘了在同一次提交里登记，
编译与测试全绿，只有人工 review 才会发现（#281 审查 B2 就是这样被判 BLOCKING）。本
脚本把「哪些头被 >=2 个测试 .cpp 包含」变成可机械验证的集合，与 §5.4 表格对齐。

规则:
  R1 扫描 apps/ + libs/ 下任意 tests/ 目录（含嵌套，如 .../src/db/tests/）中的 *.h
  R2 统计每个头被「测试 .cpp」include 的次数；测试 .cpp = apps/libs 下任意 tests/ 目录
     中的 *.cpp（含 smoke 等嵌套子目录）。include 归属先按路径解析——带路径的 include 先
     相对 .cpp 所在目录、再相对仓库根解析，命中测试头就只归属那一个；解析不到才退回
     basename 匹配（裸 basename include 在同名头并存时仍有歧义，新增同名头请用带路径 include）
  R3 已登记集合从 §5.4 表格解析：第一列的反引号 token，含 '/' 的按仓库路径；裸文件名
     按同一行前一个完整路径的目录解析（如 `date_range.h` / `date_exhaustive_cases.h`）
  R4 使用者 >=2 且未登记 → 报错（列出头路径、计数、使用者示例），提示二选一：
     登记到 §5.4，或在头文件里写显式豁免标记 + 理由；使用者 <2 → 忽略（单一用途头
     无需登记）
  R5 表格缺失 / 解析为空 → 报错（含文档路径与原因）
  R6 全列清单字面量（#247 人工发现 / #272 修复类缺陷）：共享行写入夹具存在的原因是
      「列清单唯一实现」（§5.4 的 `td_order_row_writer.h` / `td_trade_row_writer.h` /
      `td_position_row_writer.h` 覆盖 orders/trades/positions/trading_accounts 四表）。但
      测试 .cpp 里仍可以手抄一份**全列** INSERT 而不被任何检查发现。R6 把列清单真相源
      （`libs/tdstore/src/schema_catalog.cpp` 的 `kOrdersColumns[]` / `kTradesColumns[]` /
      `kPositionsColumns[]` / `kTradingAccountsColumns[]`）解析出来，扫描 §5.4 已登记的
      `*_row_writer.h` **所在目录**下的 `*.cpp`：INSERT 的列集合（忽略顺序）若等于该表
      全列集合 → 报错。范围只到登记头所在目录（夹具头自己写明「唯一实现范围是本目录」），
      不扫跨模块目录。**列子集**的 INSERT 不报（#253 的既有形态依赖它）；登记的夹具头
      自身、或文件里有显式豁免标记时不报。表清单解析失败报 `error: cannot parse ...`，
      不静默跳过。

R6 豁免标记：手抄全列是**有意**的（例如用例要逐列验证列序）时，在文件里写一行：

    // row-writer-exempt: reason=<为什么必须手抄全列>

标记只认行注释的该形态，`reason=` 不得为空（缺理由报错）。

单一用途头豁免：使用者 <2 的头自动忽略。若一个头故意不共享、却被 >=2 个测试 .cpp
包含，必须在头文件里写一行显式豁免标记并给出理由：

    // shared-test-header: exempt | reason=<为什么是有意不共享>

标记只认行注释的该形态，reason= 不得为空（缺理由会报错）。

用法:
  python3 scripts/check_shared_test_headers.py                    # 默认扫仓库根与默认文档
  python3 scripts/check_shared_test_headers.py --root <dir> --doc <path>  # 手工红绿验证
  python3 scripts/check_shared_test_headers.py --selftest         # 检查器自测（临时夹具）

退出码: 0=绿, 1=有未登记的头或文档问题, 2=参数或环境错误
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path

SCAN_ROOTS = ("apps", "libs")
SKIP_DIR_NAMES = {
    "node_modules",
    "dist",
    "build",
    "install",
    ".cache",
    ".git",
    ".worktrees",
    ".agents",
    "__pycache__",
}
HEADER_SUFFIX = ".h"
TEST_CPP_SUFFIX = ".cpp"
DOC_REL = Path("docs") / "unit-test-standard.md"
DOC_DISPLAY = "docs/unit-test-standard.md"
SHARED_THRESHOLD = 2
EXEMPT_MARKER = "// shared-test-header: exempt | reason=<why>"
# R6 全列清单字面量：列清单真相源 = schema_catalog.cpp 的 ColumnSchema 数组。
SCHEMA_CATALOG_REL = Path("libs") / "tdstore" / "src" / "schema_catalog.cpp"
SCHEMA_CATALOG_DISPLAY = "libs/tdstore/src/schema_catalog.cpp"
ROW_WRITER_SUFFIX = "_row_writer.h"
ROW_WRITER_EXEMPT_EXAMPLE = "// row-writer-exempt: reason=<why>"
COLUMN_ARRAY_TO_TABLE = {
    "kOrdersColumns": "orders",
    "kTradesColumns": "trades",
    "kPositionsColumns": "positions",
    "kTradingAccountsColumns": "trading_accounts",
}

INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]')
SECTION_54_ROW_RE = re.compile(r"^\|\s*5\.4\s*\|")
TABLE_HEADER_RE = re.compile(r"^\|\s*头\s*\|")
TABLE_SEPARATOR_RE = re.compile(r"^\|\s*:?-{1,}:?\s*(\|\s*:?-{1,}:?\s*)*\|?\s*$")
BACKTICK_RE = re.compile(r"`([^`]+)`")
EXEMPT_MARKER_RE = re.compile(
    r"^\s*//\s*shared-test-header:\s*exempt(?:\s*\|\s*reason=(?P<reason>.+?))?\s*$",
    re.MULTILINE,
)
# R6：列清单字面量识别。字符串字面量先按「相邻字面量拼接」合并成逻辑串（SQL 常见
# 跨行 `"a, b,"\n "c)"` 形态），再从中抓 INSERT INTO <table> ( col, col, ... )。
STRING_LITERAL_RUN_RE = re.compile(r'"(?:[^"\\\n]|\\.)*"(?:\s+"(?:[^"\\\n]|\\.)*")*')
STRING_LITERAL_RE = re.compile(r'"((?:[^"\\]|\\.)*)"')
INSERT_COLUMNS_RE = re.compile(
    r"INSERT\s+INTO\s+([A-Za-z_][A-Za-z0-9_]*)\s*\(([^)]*)\)", re.IGNORECASE)
INSERT_COLUMN_IDENT_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
ROW_WRITER_EXEMPT_RE = re.compile(
    r"^\s*//\s*row-writer-exempt:\s*reason=(?P<reason>.+?)\s*$", re.MULTILINE)
ROW_WRITER_EXEMPT_PREFIX_RE = re.compile(r"^\s*//\s*row-writer-exempt", re.MULTILINE)


class ScanError(Exception):
    """扫描前提不满足（根目录/文档/测试源码不可读）。"""


@dataclass(frozen=True)
class Violation:
    """一条红：unregistered（未登记且 >=2 使用者）或 exempt-without-reason（标记缺理由）。"""

    code: str
    header: str
    users: tuple[str, ...]

    def render(self) -> list[str]:
        shown = ", ".join(self.users[:3])
        if len(self.users) > 3:
            shown += f", ... (+{len(self.users) - 3} more)"
        if self.code == "exempt-without-reason":
            return [
                f"error: invalid shared-test-header exemption: {self.header} "
                f"({len(self.users)} test .cpp users; e.g. {shown})",
                f"  fix: write '{EXEMPT_MARKER}' with a non-empty reason, "
                f"or register the header in {DOC_DISPLAY} §5.4",
            ]
        return [
            f"error: unregistered shared test header: {self.header} "
            f"({len(self.users)} test .cpp users; e.g. {shown})",
            f"  fix: register it in {DOC_DISPLAY} §5.4, or add '{EXEMPT_MARKER}' "
            "to the header if it is intentionally not shared",
        ]


@dataclass
class Report:
    headers: list[str] = field(default_factory=list)
    shared: list[str] = field(default_factory=list)
    doc_errors: list[str] = field(default_factory=list)
    violations: list[Violation] = field(default_factory=list)
    table_errors: list[str] = field(default_factory=list)
    literal_errors: list[str] = field(default_factory=list)


def repo_root() -> Path:
    """仓库根：优先 git（worktree / 子目录调用都正确），退化到脚本自身位置。"""
    try:
        result = subprocess.run(
            ["git", "rev-parse", "--show-toplevel"],
            capture_output=True,
            text=True,
            check=True,
        )
        candidate = Path(result.stdout.strip())
        if candidate.is_dir():
            return candidate
    except (OSError, subprocess.CalledProcessError):
        pass
    return Path(__file__).resolve().parents[1]


def iter_test_files(base: Path, suffix: str) -> list[Path]:
    """base 下任意名为 tests 的目录（嵌套不限）里的 suffix 文件，按路径排序。"""
    found: list[Path] = []
    for dirpath, dirnames, filenames in os.walk(base):
        dirnames[:] = sorted(name for name in dirnames if name not in SKIP_DIR_NAMES)
        current = Path(dirpath)
        if "tests" not in current.relative_to(base).parts:
            continue
        for name in sorted(filenames):
            if name.endswith(suffix):
                found.append(current / name)
    return found


def collect_test_files(root: Path) -> tuple[list[Path], list[Path]]:
    """返回 (测试头, 测试 .cpp) 的绝对路径列表，均已排序。"""
    headers: list[Path] = []
    cpps: list[Path] = []
    for base_name in SCAN_ROOTS:
        base = root / base_name
        if not base.is_dir():
            continue
        headers.extend(iter_test_files(base, HEADER_SUFFIX))
        cpps.extend(iter_test_files(base, TEST_CPP_SUFFIX))
    return sorted(headers), sorted(cpps)


def rel_to_root(path: Path, root: Path) -> str:
    return path.relative_to(root).as_posix()


def include_users(root: Path, headers: list[Path], cpps: list[Path]) -> dict[Path, list[str]]:
    """每个头 -> 包含它的测试 .cpp 相对路径（去重、排序）。

    归属顺序（#284 审查收敛）：带路径的 include 先相对 .cpp 所在目录、再相对仓库根解析；
    命中测试头就只归属那一个，避免跨模块同名头互相虚增计数。解析不到（include 目录前缀
    与仓内相对关系对不上）才退回 basename 匹配。
    """
    by_basename: dict[str, list[Path]] = {}
    for header in headers:
        by_basename.setdefault(header.name, []).append(header)
    resolved = {header.resolve(): header for header in headers}
    users: dict[Path, set[str]] = {header: set() for header in headers}
    for cpp in cpps:
        try:
            text = cpp.read_text(encoding="utf-8", errors="replace")
        except OSError as exc:
            raise ScanError(f"cannot read test source {rel_to_root(cpp, root)}: {exc}") from exc
        cpp_rel = rel_to_root(cpp, root)
        for line in text.splitlines():
            match = INCLUDE_RE.match(line)
            if match is None:
                continue
            raw = match.group(1).replace("\\", "/")
            candidates: list[Path] = []
            if "/" in raw:
                for base in (cpp.parent, root):
                    try:
                        candidate = (base / raw).resolve()
                    except (OSError, RuntimeError):
                        continue
                    header = resolved.get(candidate)
                    if header is not None:
                        candidates = [header]
                        break
            if not candidates:
                candidates = by_basename.get(raw.rsplit("/", 1)[-1], [])
            for header in candidates:
                users[header].add(cpp_rel)
    return {header: sorted(user_set) for header, user_set in users.items()}


def parse_registered(doc_text: str, doc_label: str) -> tuple[set[str], list[str]]:
    """从 §5.4 表格第一列解析已登记头路径集合；解析问题以 (集合, 错误) 返回。"""
    lines = doc_text.splitlines()
    errors: list[str] = []

    section_index = next(
        (i for i, line in enumerate(lines) if SECTION_54_ROW_RE.match(line)), None)
    if section_index is None:
        errors.append(
            f"{doc_label}: no §5.4 row found (expected a markdown row starting with '| 5.4 |')")
        return set(), errors

    header_index: int | None = None
    for i in range(section_index + 1, len(lines)):
        if TABLE_HEADER_RE.match(lines[i]):
            header_index = i
            break
        if lines[i].startswith("## "):
            break
    if header_index is None:
        errors.append(
            f"{doc_label}: §5.4 row found but no shared-test-header table follows it "
            "(expected a header row '| 头 | 提供什么 |')")
        return set(), errors

    registered: set[str] = set()
    for i in range(header_index + 1, len(lines)):
        line = lines[i]
        if not line.startswith("|"):
            break
        if TABLE_SEPARATOR_RE.match(line):
            continue
        cells = line.split("|")
        if len(cells) < 3:
            errors.append(f"{doc_label}:{i + 1}: malformed §5.4 table row: {line.strip()}")
            continue
        tokens = BACKTICK_RE.findall(cells[1])
        if not tokens:
            errors.append(
                f"{doc_label}:{i + 1}: §5.4 row has no backticked header path: {line.strip()}")
            continue
        row_dir: str | None = None
        for raw in tokens:
            token = raw.strip().replace("\\", "/")
            if not token:
                continue
            if "/" in token:
                path = os.path.normpath(token).replace(os.sep, "/")
                row_dir = os.path.dirname(path)
                registered.add(path)
            elif row_dir is not None:
                # 裸文件名按同一行前一个完整路径的目录解析（date_range.h / a.h 形态）。
                registered.add(os.path.normpath(os.path.join(row_dir, token)).replace(os.sep, "/"))
            else:
                errors.append(
                    f"{doc_label}:{i + 1}: bare filename '{token}' has no preceding full "
                    f"path on the same row: {line.strip()}")

    if not registered:
        errors.append(f"{doc_label}: §5.4 table registers no header paths")
    return registered, errors


def read_text(path: Path, what: str, root: Path) -> str:
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except OSError as exc:
        try:
            label = rel_to_root(path, root)
        except ValueError:
            label = str(path)
        raise ScanError(f"cannot read {what} {label}: {exc}") from exc


def parse_table_columns(schema_text: str, label: str) -> tuple[dict[str, tuple[str, ...]], list[str]]:
    """从 schema_catalog.cpp 解析四张表的全列清单；解析失败返回 errors（不静默跳过）。

    真相源是 ``ColumnSchema kXxxColumns[] = { {"<列名>", ...}, ... };``——按数组块归属表名
    （``kOrdersColumns`` → orders 等）。只认块内形如 ``{"<列名>",`` 的行；块结束（下一个
    ``};``）即停。缺数组、块内取不到列、或列名重复都算解析失败。
    """
    lines = schema_text.splitlines()
    tables: dict[str, tuple[str, ...]] = {}
    errors: list[str] = []
    for array_name, table in COLUMN_ARRAY_TO_TABLE.items():
        head_re = re.compile(
            r"^\s*(?:constexpr\s+)?ColumnSchema\s+" + re.escape(array_name) + r"\s*\[\s*\]")
        start = next((i for i, line in enumerate(lines) if head_re.match(line)), None)
        if start is None:
            errors.append(f"cannot parse {label}: missing ColumnSchema array {array_name}")
            continue
        columns: list[str] = []
        closed = False
        for line in lines[start + 1:]:
            stripped = line.strip()
            if stripped.startswith("};"):
                closed = True
                break
            match = re.match(r'^\{"([A-Za-z_][A-Za-z0-9_]*)",', stripped)
            if match is not None:
                columns.append(match.group(1))
        if not closed:
            errors.append(f"cannot parse {label}: column array {array_name} is not terminated")
            continue
        if not columns:
            errors.append(f"cannot parse {label}: column array {array_name} yields no columns")
            continue
        duplicates = sorted({name for name in columns if columns.count(name) > 1})
        if duplicates:
            errors.append(f"cannot parse {label}: column array {array_name} repeats "
                          f"{', '.join(duplicates)}")
            continue
        tables[table] = tuple(columns)
    return tables, errors


def row_writer_dirs(registered: set[str]) -> list[str]:
    """§5.4 里登记的 ``*_row_writer.h`` 所在目录集合（R6 扫描范围，排序）。"""
    dirs = {
        os.path.dirname(os.path.normpath(rel).replace(os.sep, "/"))
        for rel in registered
        if os.path.basename(rel.replace("\\", "/")).endswith(ROW_WRITER_SUFFIX)
    }
    return sorted(d for d in dirs if d)


def _sql_insert_columns(text: str) -> list[tuple[str, tuple[str, ...]]]:
    """从 C++ 文本里抓 (表名, 列元组)：先把相邻字符串字面量拼成逻辑串再匹配 INSERT。

    跨行 ``"INSERT INTO t (a, b,"`` + ``" c) VALUES ..."`` 是本题最常见的形态，因此按
    「一条字符串字面量 run」合并后再匹配，列清单内的空白/换行自然被吃掉。
    """
    found: list[tuple[str, tuple[str, ...]]] = []
    for run in STRING_LITERAL_RUN_RE.finditer(text):
        joined = " ".join(STRING_LITERAL_RE.findall(run.group(0)))
        for match in INSERT_COLUMNS_RE.finditer(joined):
            table = match.group(1).lower()
            raw_columns = [part.strip() for part in match.group(2).split(",")]
            if not raw_columns or not all(raw_columns):
                continue
            if not all(INSERT_COLUMN_IDENT_RE.match(name) for name in raw_columns):
                continue  # 不是纯标识符列清单（如 VALUES 里的表达式），不参与判定
            found.append((table, tuple(raw_columns)))
    return found


def check_full_column_literals(
    root: Path, registered: set[str]
) -> tuple[list[str], list[str]]:
    """R6：登记夹具头所在目录下的 *.cpp 手抄全列 INSERT → 报红；返回 (错误, 表错误)。

    列子集不报（#253 的既有形态依赖列子集 INSERT）；登记的夹具头自身不扫；文件里写
    ``// row-writer-exempt: reason=<why>`` 可豁免（缺 reason 仍报）。没有登记任何
    row-writer 头 → 不做本规则（没有「唯一实现」可对照的表清单来源）。
    """
    directories = row_writer_dirs(registered)
    if not directories:
        return [], []
    tables, table_errors = parse_table_columns(
        read_text(root / SCHEMA_CATALOG_REL, "schema catalog", root), SCHEMA_CATALOG_DISPLAY)
    if table_errors:
        return [], table_errors

    errors: list[str] = []
    for directory in directories:
        base = root / directory
        if not base.is_dir():
            errors.append(f"cannot scan registered row-writer directory: {directory}")
            continue
        for path in sorted(base.rglob("*" + TEST_CPP_SUFFIX)):
            if not path.is_file():
                continue
            rel = rel_to_root(path, root)
            # 只扫 *.cpp：登记的夹具是**头文件**（`.h`，见 §5.4 表），它自己写全列清单正是
            # 「唯一实现」本体，不能报。早期版本这里还有一条「登记项自身跳过」的分支，
            # 但登记集合里不可能出现 .cpp，那个分支是死代码（#289 审查 conformance 6），已删。
            text = read_text(path, "test source", root)
            literals = _sql_insert_columns(text)
            if not literals:
                continue
            exempt = ROW_WRITER_EXEMPT_RE.search(text)
            if exempt is not None and (exempt.group("reason") or "").strip():
                continue
            if exempt is None and ROW_WRITER_EXEMPT_PREFIX_RE.search(text):
                errors.append(
                    f"{rel}: invalid row-writer exemption: missing or empty reason "
                    f"(expected '{ROW_WRITER_EXEMPT_EXAMPLE}')")
                continue
            for table, columns in literals:
                full = tables.get(table)
                if full is None or set(columns) != set(full):
                    continue
                errors.append(
                    f"{rel}: full column list for '{table}' written by hand "
                    f"({len(columns)} columns); use the shared row-writer fixture in "
                    f"{directory} instead, or add '{ROW_WRITER_EXEMPT_EXAMPLE}'")
    return errors, []


def check(root: Path, doc: Path) -> Report:
    """扫描 root，返回文档错误 / 未登记头 / 全列清单字面量 / 统计信息。"""
    if not any((root / base).is_dir() for base in SCAN_ROOTS):
        raise ScanError(f"no {'/ or '.join(SCAN_ROOTS)} directory under root: {root}")

    headers, cpps = collect_test_files(root)
    doc_text = read_text(doc, "document", root)
    try:
        doc_label = rel_to_root(doc, root)
    except ValueError:
        doc_label = str(doc)
    registered, doc_errors = parse_registered(doc_text, doc_label)

    header_rels = [rel_to_root(header, root) for header in headers]
    if doc_errors:
        return Report(headers=header_rels, doc_errors=doc_errors)

    users = include_users(root, headers, cpps)
    shared: list[str] = []
    violations: list[Violation] = []
    for header in headers:
        header_rel = rel_to_root(header, root)
        user_list = users[header]
        if len(user_list) < SHARED_THRESHOLD:
            continue  # 单一用途头：无需登记，也不要求豁免标记
        shared.append(header_rel)
        marker = EXEMPT_MARKER_RE.search(read_text(header, "test header", root))
        if marker is not None:
            if (marker.group("reason") or "").strip():
                continue
            violations.append(
                Violation("exempt-without-reason", header_rel, tuple(user_list)))
            continue
        if header_rel not in registered:
            violations.append(Violation("unregistered", header_rel, tuple(user_list)))

    violations.sort(key=lambda violation: violation.header)
    literal_errors, table_errors = check_full_column_literals(root, registered)
    return Report(headers=header_rels, shared=shared, violations=violations,
                  table_errors=table_errors, literal_errors=literal_errors)


# ---- 自测 ----------------------------------------------------------------


def _write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8", newline="\n")


def _header_body(name: str, extra: str = "") -> str:
    guard = "DZTRADER_SELFTEST_" + name.upper().replace(".", "_") + "_H_"
    return f"#ifndef {guard}\n#define {guard}\n{extra}inline int helper() {{ return 1; }}\n#endif\n"


def _cpp(*include_lines: str) -> str:
    return "\n".join(include_lines) + "\n"


def _selftest_doc(rows: list[str]) -> str:
    """最小 §5.4 文档：编号表 + 共享测试头索引表（rows 为数据行）。"""
    lines = [
        "# 单元测试判据（selftest fixture）",
        "",
        "## 5. 基建",
        "",
        "| # | 判据 | 不合格长什么样 |",
        "|---|---|---|",
        "| 5.4 | **先查共享测试头再写** | 重写一份已有的夹具 |",
        "",
        "**共享测试头（夹具/工具）索引**：",
        "",
        "| 头 | 提供什么 |",
        "|---|---|",
        *rows,
        "",
        "## 明确不做",
        "",
    ]
    return "\n".join(lines)


def _build_case(
    case_dir: Path,
    headers: dict[str, str],
    cpps: dict[str, str],
    doc_text: str,
    schema_text: str | None = None,
) -> tuple[Path, Path]:
    root = case_dir / "repo"
    for rel, content in headers.items():
        _write(root / rel, content)
    for rel, content in cpps.items():
        _write(root / rel, content)
    if schema_text is not None:
        _write(root / SCHEMA_CATALOG_REL, schema_text)
    doc = case_dir / "docs" / "unit-test-standard.md"
    _write(doc, doc_text)
    return root, doc


# R6 自测用的最小列清单：与真实 schema_catalog.cpp 的形态一致（{"<列名>", ...} 行），
# 四张表各取少而可分辨的列，够验证「全列 vs 子集」判定而不复制真实清单。
_SELFTEST_COLUMNS = {
    "orders": ("account_id", "date", "order_id", "seq"),
    "trades": ("account_id", "date", "trade_id", "order_id", "seq"),
    "positions": ("account_id", "date", "instrument_id", "seq"),
    "trading_accounts": ("account_id", "date", "seq"),
}


def _selftest_schema(omit: str | None = None) -> str:
    """最小 schema_catalog.cpp 夹具；omit 指定一张表不写（模拟解析失败）。"""
    lines = ["// selftest fixture: minimal schema catalog", ""]
    for array_name, table in COLUMN_ARRAY_TO_TABLE.items():
        if table == omit:
            continue
        lines.append(f"constexpr ColumnSchema {array_name}[] = {{")
        for column in _SELFTEST_COLUMNS[table]:
            lines.append(f'    {{"{column}", ColumnType::Text, true, false, ""}},')
        lines.append("};")
        lines.append("")
    return "\n".join(lines)


def _insert_cpp(*literals: str, comment: str | None = None) -> str:
    """构造一段带 INSERT 字面量的测试 .cpp（literals 为相邻字符串，模拟跨行拼接）。"""
    body = ['#include "td_trade_row_writer.h"', "", "namespace {", "void seed() {"]
    body.append("    constexpr const char* kSql =")
    for literal in literals:
        body.append(f'        "{literal}"')
    body.append("        ;")
    body.append("    (void)kSql;")
    body.append("}")
    body.append("}  // namespace")
    if comment is not None:
        body.insert(4, comment)
    return "\n".join(body) + "\n"


def run_selftest() -> int:
    """在临时夹具树上验证检查器判据，确认它不是恒绿/恒红；工作区只读。"""
    failures = 0
    cases = 0

    def run_case(
        name: str,
        headers: dict[str, str],
        cpps: dict[str, str],
        doc_text: str,
        expected_violations: list[str],
        expected_shared: list[str],
    ) -> None:
        nonlocal failures, cases
        cases += 1
        root, doc = _build_case(base / name, headers, cpps, doc_text)
        try:
            report = check(root, doc)
        except ScanError as exc:
            print(f"selftest FAIL {name}: unexpected scan error: {exc}", file=sys.stderr)
            failures += 1
            return
        problems: list[str] = []
        got_violations = [violation.header for violation in report.violations]
        if got_violations != expected_violations:
            problems.append(f"violations {got_violations} != {expected_violations}")
        if report.shared != expected_shared:
            problems.append(f"shared {report.shared} != {expected_shared}")
        if report.doc_errors:
            problems.append(f"unexpected document error(s): {report.doc_errors}")
        if report.literal_errors:
            problems.append(f"unexpected full-column literal error(s): {report.literal_errors}")
        if report.table_errors:
            problems.append(f"unexpected schema table error(s): {report.table_errors}")
        if problems:
            print(f"selftest FAIL {name}: " + "; ".join(problems), file=sys.stderr)
            failures += 1
        else:
            print(f"selftest ok {name}")

    with tempfile.TemporaryDirectory(prefix="dz_shared_test_headers_selftest_") as tmp:
        base = Path(tmp)

        nested_shared = "libs/strategy_api/src/db/tests/shared_fixture.h"
        run_case(
            "registered_green",
            headers={nested_shared: _header_body("shared_fixture")},
            cpps={
                "libs/strategy_api/src/db/tests/a_test.cpp": _cpp(
                    '#include "shared_fixture.h"'),
                "apps/master/tests/smoke/b_test.cpp": _cpp(
                    '#include "../../../../' + nested_shared + '"'),
            },
            doc_text=_selftest_doc([f"| `{nested_shared}` | 共享夹具 |"]),
            expected_violations=[],
            expected_shared=[nested_shared],
        )

        unregistered = "apps/webui/tests/fake_thing.h"
        run_case(
            "unregistered_red",
            headers={
                unregistered: _header_body("fake_thing"),
                "apps/webui/tests/solo.h": _header_body("solo"),
            },
            cpps={
                "apps/webui/tests/one_test.cpp": _cpp('#include "fake_thing.h"'),
                "apps/webui/tests/two_test.cpp": _cpp('#include <tests/fake_thing.h>'),
                "apps/webui/tests/solo_test.cpp": _cpp('#include "solo.h"'),
            },
            doc_text=_selftest_doc(["| `apps/webui/tests/solo.h` | 单一用途 |"]),
            expected_violations=[unregistered],
            expected_shared=[unregistered],
        )

        run_case(
            "single_use_ignored",
            headers={
                "libs/core/tests/solo.h": _header_body("solo"),
                "libs/core/tests/registered_other.h": _header_body("registered_other"),
            },
            cpps={"libs/core/tests/solo_test.cpp": _cpp('#include "solo.h"')},
            doc_text=_selftest_doc(["| `libs/core/tests/registered_other.h` | 未使用 |"]),
            expected_violations=[],
            expected_shared=[],
        )

        exempt_header = "libs/shm/tests/exempt_fixture.h"
        run_case(
            "exempt_marker_green",
            headers={
                exempt_header: _header_body(
                    "exempt_fixture",
                    "// shared-test-header: exempt | reason=two use cases share it by design\n",
                ),
            },
            cpps={
                "libs/shm/tests/x_test.cpp": _cpp('#include "exempt_fixture.h"'),
                "libs/shm/tests/y_test.cpp": _cpp('#include "exempt_fixture.h"'),
            },
            doc_text=_selftest_doc(["| `libs/shm/tests/other.h` | 未使用 |"]),
            expected_violations=[],
            expected_shared=[exempt_header],
        )

        no_reason_header = "libs/shm/tests/no_reason.h"
        run_case(
            "exempt_without_reason_red",
            headers={
                no_reason_header: _header_body(
                    "no_reason", "// shared-test-header: exempt\n"),
            },
            cpps={
                "libs/shm/tests/p_test.cpp": _cpp('#include "no_reason.h"'),
                "libs/shm/tests/q_test.cpp": _cpp('#include "no_reason.h"'),
            },
            doc_text=_selftest_doc(["| `libs/shm/tests/other.h` | 未使用 |"]),
            expected_violations=[no_reason_header],
            expected_shared=[no_reason_header],
        )

        run_case(
            "multi_path_row_green",
            headers={
                "libs/strategy_api/tests/date_range.h": _header_body("date_range"),
                "libs/strategy_api/tests/date_exhaustive_cases.h": _header_body(
                    "date_exhaustive_cases"),
            },
            cpps={
                "libs/strategy_api/tests/date_test.cpp": _cpp(
                    '#include "date_range.h"', '#include "date_exhaustive_cases.h"'),
                "libs/strategy_api/tests/date_exhaustive_test.cpp": _cpp(
                    '#include "date_range.h"', '#include "date_exhaustive_cases.h"'),
            },
            doc_text=_selftest_doc([
                "| `libs/strategy_api/tests/date_range.h` / `date_exhaustive_cases.h` "
                "| 日期数据集 |",
            ]),
            expected_violations=[],
            expected_shared=[
                "libs/strategy_api/tests/date_exhaustive_cases.h",
                "libs/strategy_api/tests/date_range.h",
            ],
        )

        # 跨模块同名头（#284 审查收敛）：带路径的 include 按 .cpp 相对路径归属，不会靠
        # basename 把两个头各算成 2 个使用者；各 1 个使用者 → 都不是共享头。
        run_case(
            "same_basename_path_resolved_green",
            headers={
                "libs/alpha/tests/common_fixture.h": _header_body("alpha_common"),
                "libs/beta/tests/common_fixture.h": _header_body("beta_common"),
            },
            cpps={
                "libs/alpha/tests/a_test.cpp": _cpp('#include "../tests/common_fixture.h"'),
                "libs/beta/tests/b_test.cpp": _cpp('#include "../tests/common_fixture.h"'),
            },
            doc_text=_selftest_doc(["| `libs/alpha/tests/solo.h` | 未使用 |"]),
            expected_violations=[],
            expected_shared=[],
        )

        # 文档问题：§5.4 行存在但索引表缺失 / 表存在但没有数据行。
        doc_missing_table = "\n".join([
            "# 单元测试判据（selftest fixture）",
            "",
            "## 5. 基建",
            "",
            "| # | 判据 | 不合格长什么样 |",
            "|---|---|---|",
            "| 5.4 | **先查共享测试头再写** | 重写一份已有的夹具 |",
            "",
            "## 明确不做",
            "",
        ])
        cases += 1
        root, doc = _build_case(
            base / "missing_table",
            {"libs/core/tests/one.h": _header_body("one")},
            {"libs/core/tests/one_test.cpp": _cpp('#include "one.h"')},
            doc_missing_table,
        )
        report = check(root, doc)
        if not report.doc_errors:
            print("selftest FAIL missing_table: expected a document error, got none",
                  file=sys.stderr)
            failures += 1
        elif str(doc) not in " ".join(report.doc_errors):
            print("selftest FAIL missing_table: document error does not name the doc path",
                  file=sys.stderr)
            failures += 1
        else:
            print("selftest ok missing_table")

        cases += 1
        root, doc = _build_case(
            base / "empty_table",
            {"libs/core/tests/one.h": _header_body("one")},
            {"libs/core/tests/one_test.cpp": _cpp('#include "one.h"')},
            _selftest_doc([]),
        )
        report = check(root, doc)
        if not report.doc_errors:
            print("selftest FAIL empty_table: expected a document error, got none",
                  file=sys.stderr)
            failures += 1
        else:
            print("selftest ok empty_table")

        # R6 全列清单字面量（#247 / #272）：登记夹具头所在目录下 *.cpp 手抄**全列** INSERT
        # → 报红；列子集绿；登记的夹具头自身不扫；带 reason 的豁免绿、缺 reason 红；
        # 表清单解析失败红（不静默跳过）。
        row_writer = "apps/webui/tests/td_trade_row_writer.h"
        row_writer_dir = "apps/webui/tests"
        row_writer_doc = _selftest_doc([f"| `{row_writer}` | 交易行夹具 |"])

        def run_literal_case(
            name: str,
            cpps: dict[str, str],
            headers: dict[str, str],
            schema_text: str | None,
            expected_literal_substrings: list[str],
            expected_table_substring: str | None,
        ) -> None:
            nonlocal failures, cases
            cases += 1
            root, doc = _build_case(
                base / name,
                {row_writer: _header_body("td_trade_row_writer"), **headers},
                cpps,
                row_writer_doc,
                schema_text=schema_text if schema_text is not None else _selftest_schema(),
            )
            report = check(root, doc)
            problems: list[str] = []
            if report.doc_errors or report.violations:
                problems.append(f"unexpected doc/header error: "
                                f"{report.doc_errors}{[v.header for v in report.violations]}")
            got_literal = " ".join(report.literal_errors)
            for needle in expected_literal_substrings:
                if needle not in got_literal:
                    problems.append(f"literal errors {report.literal_errors} lack '{needle}'")
            if expected_table_substring is None:
                if report.table_errors:
                    problems.append(f"unexpected table errors: {report.table_errors}")
            else:
                if not any(expected_table_substring in message
                           for message in report.table_errors):
                    problems.append(f"table errors {report.table_errors} lack "
                                    f"'{expected_table_substring}'")
            if problems:
                print(f"selftest FAIL {name}: " + "; ".join(problems), file=sys.stderr)
                failures += 1
            else:
                print(f"selftest ok {name}")

        run_literal_case(
            "r6_full_column_red",
            cpps={
                f"{row_writer_dir}/handwritten_test.cpp": _insert_cpp(
                    "INSERT INTO trades (account_id, date, trade_id, order_id,",
                    " seq) VALUES (?,?,?,?,?)"),
            },
            headers={},
            schema_text=None,
            expected_literal_substrings=["handwritten_test.cpp", "full column list", "trades"],
            expected_table_substring=None,
        )

        run_literal_case(
            "r6_column_subset_green",
            cpps={
                f"{row_writer_dir}/subset_test.cpp": _insert_cpp(
                    "INSERT INTO trades (account_id, date, trade_id) VALUES (?,?,?)"),
            },
            headers={},
            schema_text=None,
            expected_literal_substrings=[],
            expected_table_substring=None,
        )

        # 嵌套子目录也要扫（rglob）：夹具目录下再分 `queries/`、`controllers/` 之类子目录
        # 时，手抄全列仍然要报。原先这里放的是「夹具头自身绿」用例——把 INSERT 放在 .h 里
        # 而扫描只认 .cpp，该用例恒真（#289 审查 conformance 6），换成真正能红的折叠用例。
        run_literal_case(
            "r6_nested_subdir_red",
            cpps={
                f"{row_writer_dir}/queries/nested_handwritten_test.cpp": _insert_cpp(
                    "INSERT INTO trades (account_id, date, trade_id, order_id,",
                    " seq) VALUES (?,?,?,?,?)"),
            },
            headers={},
            schema_text=None,
            expected_literal_substrings=["nested_handwritten_test.cpp", "full column list"],
            expected_table_substring=None,
        )

        run_literal_case(
            "r6_exempt_with_reason_green",
            cpps={
                f"{row_writer_dir}/exempt_test.cpp": _insert_cpp(
                    "INSERT INTO trades (account_id, date, trade_id, order_id,",
                    " seq) VALUES (?,?,?,?,?)",
                    comment="// row-writer-exempt: reason=this case asserts the exact column "
                            "order, so the literal must stay"),
            },
            headers={},
            schema_text=None,
            expected_literal_substrings=[],
            expected_table_substring=None,
        )

        run_literal_case(
            "r6_exempt_without_reason_red",
            cpps={
                f"{row_writer_dir}/no_reason_test.cpp": _insert_cpp(
                    "INSERT INTO trades (account_id, date, trade_id, order_id,",
                    " seq) VALUES (?,?,?,?,?)",
                    comment="// row-writer-exempt:"),
            },
            headers={},
            schema_text=None,
            expected_literal_substrings=["no_reason_test.cpp", "missing or empty reason"],
            expected_table_substring=None,
        )

        run_literal_case(
            "r6_table_parse_failure_red",
            cpps={},
            headers={},
            schema_text=_selftest_schema(omit="trades"),
            expected_literal_substrings=[],
            expected_table_substring="cannot parse",
        )

    if failures:
        print(f"selftest FAILED: {failures} case(s)", file=sys.stderr)
        return 1
    print(f"selftest: {cases} cases passed (on a temp fixture tree; the worktree is never "
          "modified)")
    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="check that shared test headers are registered in "
                    "docs/unit-test-standard.md §5.4")
    parser.add_argument("--selftest", action="store_true",
                        help="run the checker against a temp fixture tree (worktree untouched)")
    parser.add_argument("--root", type=Path, default=None,
                        help="repository root to scan (default: git top-level)")
    parser.add_argument("--doc", type=Path, default=None,
                        help="unit-test-standard.md to parse "
                             "(default: <root>/docs/unit-test-standard.md)")
    args = parser.parse_args(argv)

    if args.selftest:
        return run_selftest()

    root = (args.root or repo_root()).resolve()
    if not root.is_dir():
        print(f"error: --root is not a directory: {root}", file=sys.stderr)
        return 2
    doc = (args.doc or (root / DOC_REL))
    if not doc.is_file():
        print(f"error: document not found: {doc}", file=sys.stderr)
        return 2

    try:
        report = check(root, doc)
    except ScanError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    if report.doc_errors:
        for message in report.doc_errors:
            print(f"error: {message}")
        print(f"\nFAIL: {len(report.doc_errors)} document problem(s) in {doc}",
              file=sys.stderr)
        return 1
    if report.table_errors:
        for message in report.table_errors:
            print(f"error: {message}")
        print(f"\nFAIL: {len(report.table_errors)} schema table parse problem(s)",
              file=sys.stderr)
        return 1
    if report.violations or report.literal_errors:
        for violation in report.violations:
            for line in violation.render():
                print(line)
        for message in report.literal_errors:
            print(f"error: {message}")
        failed = len(report.violations) + len(report.literal_errors)
        print(f"\nFAIL: {failed} shared test header issue(s)", file=sys.stderr)
        return 1

    print(f"OK: {len(report.headers)} test header(s) checked, {len(report.shared)} shared "
          f"(>= {SHARED_THRESHOLD} users), all registered or exempt; "
          f"no hand-written full column list under registered row-writer directories",
          file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
