#!/usr/bin/env python3
"""contract_slice.py — 按 Markdown 编号小节切帧契约（只读）。

背景：`docs/frame_contracts/sysmon.md` 已 770 行 / 105 KB，单票被读 13–29 次，其中
`sed -n` 按行号分页 2–15 次/票；行号在每次改契约后即失效。本工具按**编号**切出小节，
用 `§N` 代替行号（契约引用本来就禁止写行号，见 general §11.2）。

用法:
  python3 scripts/contract_slice.py sysmon §2.9            # 切 §2.9 本节（不含子节）
  python3 scripts/contract_slice.py sysmon §2.9 --children # 含全部子节
  python3 scripts/contract_slice.py --list                 # 全部契约的编号标题
  python3 scripts/contract_slice.py --outline sysmon       # 一份契约/文档的节大纲
  python3 scripts/contract_slice.py --outline ticket-closeout          # docs stem
  python3 scripts/contract_slice.py --outline docs/agents/ticket-closeout.md  # 仓库相对路径
  python3 scripts/contract_slice.py ticket-closeout §1     # docs 目标也能切
  python3 scripts/contract_slice.py --selftest             # 临时目录自测，不碰真实契约

规则:
  - 编号标题形态 `## 7.9` / `### 2.9` / `#### 6.1.1`（2–6 个 `#`，编号 1–4 段数字）
  - 节边界与嵌套按**编号深度**判定，不按 `#` 层数（契约里 `### 3.1` 与 `### 3.1.1`
    常用同一层数；按层数切会把整章后面的节都带进来）
  - 代码围栏（``` / ~~~）里的假标题不算节——围栏里的行不参与标题索引
  - `§N` 参数接受 `2` / `2.5` / `2.9` / `7.9`，前导 `§` 可省略
  - 同号重复（同一编号出现多次，含 `## 2.5` 与 `### 2.5` 跨层级冲突）不静默取第一个：
    切片报错并列出全部行号，`--list` 也标出该编号
  - 未知契约 / 未知节号 exit 1 并给候选；参数或环境错误 exit 2
  - `--contract-dir` 只供 selftest/unittest 驱动临时契约；日常用默认目录
  - **目标寻址**：`stem` 可以是契约 stem、docs 文档 stem 或仓库相对 `.md` 路径。
    解析顺序：契约 stem 命中优先；否则在 `docs/agents/<stem>.md`、`docs/components/<stem>.md`
    里找，多个命中 → exit 1 并列出候选；未知 → exit 1 给候选（契约 stem + docs stem）。
    含 `/` 或以 `.md` 结尾的目标按**路径**解析（必须是仓库内已存在的 .md 文件）；
    不存在 / 绝对路径 / 目录 → exit 2。`--outline` 与 `--list`、位置参数互斥 (#316 AC3)
  - `--outline <target>` 输出与 `--list` 单份块**同一形状**：首行 `<stem> (<相对路径>)`，
    随后按编号深度缩进的 `§N <title>`；docs 目录经 `--docs-dir` 覆盖（selftest 用临时夹具）

退出码: 0=命中, 1=未知目标/未知节号/歧义（含候选提示）, 2=参数/环境错误。
"""

from __future__ import annotations

import argparse
import difflib
import re
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_CONTRACT_DIR = REPO_ROOT / "docs" / "frame_contracts"
DEFAULT_DOCS_ROOT = REPO_ROOT / "docs"
USAGE = (
    "usage: contract_slice.py <target> <section> [--children] | --list | --outline <target> "
    "| --selftest [--contract-dir <path>]"
)

_FENCE_RE = re.compile(r"^\s*(`{3,}|~{3,})")
_HEADING_RE = re.compile(r"^(#{2,6})\s+(\d+(?:\.\d+){0,3})(?:\.)?(?:\s+(.*))?$")
_SECTION_ARG_RE = re.compile(r"^\d+(?:\.\d+){0,3}$")


@dataclass(frozen=True)
class Heading:
    """一条编号标题：行号 0 起、编号与标题文本。

    不保留 `#` 层数：真实契约里父子节常用同一层数（sysmon 的 `### 3.1` 与 `### 3.1.1`
    都是 `###`），节边界与缩进一律按**编号深度**判定。
    """

    number: str
    line: int
    title: str


class UsageError(Exception):
    """参数错误（exit 2）。"""


class SliceError(Exception):
    """未知契约/未知节号/重复编号（exit 1），消息里已含候选提示。"""


class ContractDirError(Exception):
    """契约目录不存在/不可读（exit 2）：环境前提不满足，不是「某个契约未知」。"""


class UnknownSectionError(SliceError):
    """节号不存在；调用方补候选提示。"""


class DuplicateSectionError(SliceError):
    """同一编号出现多次（含跨层级冲突）；不静默取第一个。"""


class _Parser(argparse.ArgumentParser):
    def error(self, message):
        raise UsageError(message)


def iter_headings(text: str) -> list[Heading]:
    """返回围栏代码块之外的编号标题；围栏里的 `## 2.9` 是内容，不是节。"""
    headings: list[Heading] = []
    fence: tuple[str, int] | None = None
    for line_no, line in enumerate(text.splitlines()):
        fence_match = _FENCE_RE.match(line)
        if fence_match:
            run = fence_match.group(1)
            if fence is None:
                fence = (run[0], len(run))
            elif fence[0] == run[0] and len(run) >= fence[1]:
                fence = None
            continue
        if fence is not None:
            continue
        heading_match = _HEADING_RE.match(line)
        if heading_match:
            headings.append(
                Heading(
                    number=heading_match.group(2),
                    line=line_no,
                    title=(heading_match.group(3) or "").strip(),
                )
            )
    return headings


def contract_paths(contract_dir: Path) -> dict[str, Path]:
    """契约 stem → 文件路径；README.md 是目录地图，不入契约集。"""
    if not contract_dir.is_dir():
        raise ContractDirError(f"contract directory not found: {contract_dir}")
    return {
        path.stem: path
        for path in sorted(contract_dir.glob("*.md"))
        if path.name != "README.md"
    }


def read_contract(path: Path) -> tuple[str, list[Heading]]:
    text = path.read_text(encoding="utf-8", errors="replace")
    return text, iter_headings(text)


def numbered_sections(path: Path) -> list[Heading]:
    return read_contract(path)[1]


@dataclass(frozen=True)
class Target:
    """一个可切/可列大纲的目标：契约或 docs 文档。"""

    path: Path
    stem: str


def docs_paths(docs_dir: Path) -> dict[str, list[Path]]:
    """docs stem → 路径；同名 stem 可能在 agents 与 components 各有一份（算歧义）。

    `docs_dir` 是 docs 根目录（默认 `docs/`），其下 `agents/` 与 `components/`
    参与目标寻址；目录不存在时返回空表（selftest 夹具可以只造其中一部分）。
    """
    found: dict[str, list[Path]] = {}
    for subdir in ("agents", "components"):
        directory = docs_dir / subdir
        if not directory.is_dir():
            continue
        for path in sorted(directory.glob("*.md")):
            if path.name == "README.md":
                continue
            found.setdefault(path.stem, []).append(path)
    return found


def _looks_like_path(raw: str) -> bool:
    """含 `/` 或以 `.md` 结尾的目标按路径解析，其余按 stem 解析。"""
    return "/" in raw or raw.endswith(".md")


def _relative(path: Path, root: Path) -> Path | str:
    try:
        return path.relative_to(root)
    except ValueError:
        return path


def _resolve_path_target(raw: str, root: Path) -> Target:
    """仓库相对 `.md` 路径；不存在 / 绝对路径 / 目录 / 非 .md 都是用法错误（exit 2）。"""
    candidate = Path(raw)
    if candidate.is_absolute():
        raise UsageError(f"target must be a repo-relative path, not absolute: {raw}")
    path = root / candidate
    if not path.exists():
        raise UsageError(f"target path not found: {raw}")
    if path.is_dir():
        raise UsageError(f"target is a directory, not a .md file: {raw}")
    if path.suffix != ".md":
        raise UsageError(f"target is not a .md file: {raw}")
    resolved = path.resolve()
    if not resolved.is_relative_to(root.resolve()):
        raise UsageError(f"target escapes the repo: {raw}")
    return Target(path=resolved, stem=resolved.stem)


def unknown_target_message(
    raw: str, contracts: dict[str, Path], docs: dict[str, list[Path]]
) -> str:
    """未知目标的候选提示；候选集合 = 契约 stem + docs/agents + docs/components 的 stem。"""
    universe = list(contracts) + list(docs)
    suggestions = difflib.get_close_matches(raw, universe, n=5, cutoff=0.5)
    hint = ",".join(suggestions) if suggestions else ",".join(universe)
    return f"unknown contract or document | target={raw} candidates={hint}"


def resolve_target(raw: str, contract_dir: Path, docs_dir: Path) -> Target:
    """把 CLI 目标解析成文件路径。

    解析顺序（#316 AC3）：含 `/` 或 `.md` 后缀 → 仓库相对路径；否则**契约 stem 命中
    优先**，再退到 `docs/agents/<stem>.md`、`docs/components/<stem>.md`。docs 同名多命中
    报歧义（exit 1，SliceError）；完全未知报候选（exit 1）。用法错误是 UsageError（exit 2）。

    仓库根取 `docs_dir.parent`：`--docs-dir` 夹具因此可以把路径目标也锁在临时目录里，
    不让用例有机会写到真实仓库。
    """
    root = docs_dir.parent
    if _looks_like_path(raw):
        return _resolve_path_target(raw, root)
    contracts = contract_paths(contract_dir)
    path = contracts.get(raw)
    if path is not None:
        return Target(path=path, stem=raw)
    hits = docs_paths(docs_dir).get(raw, [])
    if len(hits) > 1:
        listing = ", ".join(str(_relative(hit, root)) for hit in hits)
        raise SliceError(f"ambiguous target | target={raw} candidates={listing}")
    if hits:
        return Target(path=hits[0], stem=raw)
    raise SliceError(unknown_target_message(raw, contracts, docs_paths(docs_dir)))


def _depth(number: str) -> int:
    return len(number.split("."))


def _number_indent(number: str) -> int:
    return 2 * (len(number.split(".")) - 1)


def _is_descendant(number: str, parent: str) -> bool:
    """编号语义上的子节：`3.1.1` 属于 `3.1`，`2.10` 不属于 `2.9`。

    节边界只认编号、不认 `#` 层数：真实契约里 `### 3.1.1` 与 `### 3.1` 同为 `###`
    （层数只影响渲染缩进），按层数切会把整章后面所有节都带进来。
    """
    return number.startswith(parent + ".")


def _last_segment(number: str) -> int:
    try:
        return int(number.rsplit(".", 1)[-1])
    except ValueError:
        return 0


def candidates_for_section(headings: list[Heading], number: str) -> str:
    """未知节号时的候选提示；候选只在同一父级语义下比较（顶层按数字邻近代）。"""
    parts = number.split(".")
    parent = ".".join(parts[:-1]) if len(parts) > 1 else None
    top = [heading for heading in headings if _depth(heading.number) == 1]
    if parent is not None:
        siblings = [
            heading
            for heading in headings
            if _depth(heading.number) == len(parts)
            and heading.number.rsplit(".", 1)[0] == parent
        ]
        if siblings:
            ranked = sorted(
                siblings, key=lambda heading: abs(_last_segment(heading.number) - _last_segment(number))
            )
            return "sections under §{}: {}".format(
                parent, " ".join(f"§{heading.number}" for heading in ranked[:8])
            )
        parent_heading = next((heading for heading in headings if heading.number == parent), None)
        hints: list[str] = []
        if parent_heading is not None:
            hints.append(f"§{parent} exists but declares no numbered subsections")
        if top:
            hints.append("top-level sections: " + " ".join(f"§{heading.number}" for heading in top[:12]))
        if hints:
            return "; ".join(hints)
    if top:
        ranked_top = sorted(
            top, key=lambda heading: abs(_last_segment(heading.number) - _last_segment(number))
        )
        return "top-level sections: " + " ".join(f"§{heading.number}" for heading in ranked_top[:12])
    return "contract declares no numbered sections"


def slice_text(
    text: str, headings: list[Heading], number: str, include_children: bool
) -> str:
    """切出 number 对应节；默认到第一个子编号标题为止，`--children` 则含全部子节。

    节边界按**编号**判定（`3.1.1` 是 `3.1` 的子节），不按 `#` 层数——契约里同一父级
    与子级常用同一层数（sysmon 的 `### 3.1` 与 `### 3.1.1` 都是 `###`）。
    """
    matches = [heading for heading in headings if heading.number == number]
    if not matches:
        raise UnknownSectionError(f"unknown section §{number}")
    if len(matches) > 1:
        lines = ", ".join(str(heading.line + 1) for heading in matches)
        raise DuplicateSectionError(
            f"duplicate section §{number} at lines {lines}; numbering is ambiguous"
        )
    target = matches[0]
    index = headings.index(target)
    end = len(text.splitlines())
    for heading in headings[index + 1 :]:
        is_child = _is_descendant(heading.number, target.number)
        if not include_children or not is_child:
            # 本节的内容区在第一个「非子节」标题处结束；默认模式连子节也不含，
            # 所以遇到的第一个标题就结束（叶子节因此不会吞掉后面的整章）。
            end = heading.line
            break
    lines = text.splitlines()[target.line : end]
    while lines and not lines[-1].strip():
        lines.pop()
    return "\n".join(lines)


def render_block(path: Path, stem: str, root: Path) -> list[str]:
    """单份目标的大纲块：首行 `<stem> (<相对路径>)`，随后缩进的 `§N <title>`。

    `--list` 的每份契约块与 `--outline` 共用本函数，输出形状因此不会分叉（#316 AC3）。
    """
    out = [f"{stem} ({_relative(path, root)})"]
    headings = numbered_sections(path)
    if not headings:
        out.append("  (no numbered headings)")
        return out
    duplicates = {
        heading.number
        for heading in headings
        if sum(1 for other in headings if other.number == heading.number) > 1
    }
    for heading in headings:
        marker = " [duplicate]" if heading.number in duplicates else ""
        indent = " " * _number_indent(heading.number)
        out.append(f"  {indent}§{heading.number} {heading.title}{marker}")
    return out


def render_list(contract_dir: Path) -> str:
    """全部契约的编号标题，缩进反映层级；重复编号带行号标记。"""
    paths = contract_paths(contract_dir)
    out: list[str] = []
    for stem, path in paths.items():
        out.extend(render_block(path, stem, REPO_ROOT))
    return "\n".join(out)


def render_outline(target: Target, root: Path) -> str:
    """单份目标的大纲；与 `--list` 的单份块同一形状。"""
    return "\n".join(render_block(target.path, target.stem, root))


def build_parser() -> _Parser:
    parser = _Parser(prog="contract_slice.py", usage=USAGE, add_help=True)
    parser.add_argument("stem", nargs="?", help="契约 stem / docs stem / 仓库相对 .md 路径")
    parser.add_argument("section", nargs="?", help="节号，例如 §2.9 / §2 / §6.1.1")
    parser.add_argument("--children", action="store_true", help="含子节（默认只切本节）")
    parser.add_argument("--list", action="store_true", help="列出全部契约的编号标题")
    parser.add_argument(
        "--outline",
        metavar="TARGET",
        help="列出单个目标（契约 stem / docs stem / 仓库相对 .md 路径）的编号节大纲",
    )
    parser.add_argument("--selftest", action="store_true", help="临时目录自测，不碰真实契约")
    parser.add_argument(
        "--contract-dir",
        type=Path,
        help="契约目录覆盖（默认 docs/frame_contracts；供 selftest/test 用，不常用于日常）",
    )
    parser.add_argument(
        "--docs-dir",
        type=Path,
        help="docs 根目录覆盖（默认 docs/；其下 agents/ 与 components/ 参与目标寻址）",
    )
    return parser


def normalize_section(raw: str) -> str:
    value = raw.strip()
    if value.startswith("§"):
        value = value[1:].strip()
    if not _SECTION_ARG_RE.match(value):
        raise UsageError(f"invalid section number: {raw}")
    return value


def run(
    argv: list[str],
    contract_dir: Path = DEFAULT_CONTRACT_DIR,
    docs_dir: Path = DEFAULT_DOCS_ROOT,
) -> tuple[int, str, str]:
    """纯函数入口：返回 (exit code, stdout, stderr)，selftest 因此不碰真实契约。"""
    try:
        args = build_parser().parse_args(argv)
    except UsageError as exc:
        return 2, "", f"error: {exc}\n{USAGE}"
    if args.contract_dir is not None:
        contract_dir = args.contract_dir
    if args.docs_dir is not None:
        docs_dir = args.docs_dir
    if args.selftest:
        if args.stem or args.section or args.list or args.outline or args.children:
            return 2, "", f"error: --selftest takes no other arguments\n{USAGE}"
        if args.contract_dir is not None or args.docs_dir is not None:
            # selftest 自带临时契约与 docs 夹具；再给目录覆盖只会让它被静默忽略（#309 审查 N6）。
            return 2, "", f"error: --selftest takes no --contract-dir or --docs-dir\n{USAGE}"
        return selftest()
    if args.outline is not None:
        # `--outline` 取代「`<stem> --list`」（#316 AC3）：它接受一个目标，且与
        # `--list`、位置参数互斥 —— 三种混用都是用法错误（exit 2）。
        if args.list:
            return 2, "", f"error: --outline and --list are mutually exclusive\n{USAGE}"
        if args.stem or args.section or args.children:
            return 2, "", (
                "error: --outline takes no positional arguments or --children\n" + USAGE
            )
        if not args.outline.strip():
            return 2, "", f"error: --outline requires a target\n{USAGE}"
        try:
            target = resolve_target(args.outline, contract_dir, docs_dir)
        except ContractDirError as exc:
            return 2, "", f"error: {exc}"
        except UsageError as exc:
            return 2, "", f"error: {exc}\n{USAGE}"
        except SliceError as exc:
            return 1, "", f"error: {exc}"
        return 0, render_outline(target, docs_dir.parent) + "\n", ""
    if args.list:
        if args.stem or args.section or args.children:
            return 2, "", (
                "error: --list takes no contract stem or section; "
                "use --outline <stem> for one contract's outline\n" + USAGE
            )
        try:
            return 0, render_list(contract_dir) + "\n", ""
        except ContractDirError as exc:
            return 2, "", f"error: {exc}"
    if not args.stem or not args.section:
        return 2, "", f"error: missing contract stem or section\n{USAGE}"
    try:
        number = normalize_section(args.section)
    except UsageError as exc:
        return 2, "", f"error: {exc}\n{USAGE}"
    try:
        target = resolve_target(args.stem, contract_dir, docs_dir)
    except ContractDirError as exc:
        return 2, "", f"error: {exc}"
    except UsageError as exc:
        return 2, "", f"error: {exc}\n{USAGE}"
    except SliceError as exc:
        return 1, "", f"error: {exc}"
    stem = target.stem
    text, headings = read_contract(target.path)
    try:
        section = slice_text(text, headings, number, args.children)
    except UnknownSectionError:
        hints = candidates_for_section(headings, number)
        if not headings:
            # docs 目标常见形态：整份文档没有编号标题（`## 标题` 不带数字）——切不了，
            # 只能报错；此时候选提示不能沿用契约口径的文案。
            hints = "target declares no numbered headings; sections can only be addressed by number"
        return 1, "", f"error: unknown section | target={stem} section=§{number} {hints}"
    except SliceError as exc:
        return 1, "", f"error: {exc} | target={stem} section=§{number}"
    return 0, section + "\n", ""


def selftest() -> tuple[int, str, str]:
    """临时目录构造 mini 契约，覆盖命中/子节/未知/重复/围栏假标题/未知契约。"""
    failures: list[str] = []
    checks = [0]

    def check(name: str, condition: bool, detail: str = "") -> None:
        checks[0] += 1
        if not condition:
            failures.append(f"{name}: {detail}" if detail else name)

    sample = "\n".join(
        [
            "# 帧契约：示例",
            "",
            "## 1. 总览",
            "",
            "overview body",
            "",
            "### 1.1 子节",
            "",
            "child body",
            "",
            "## 2. 主体",
            "",
            "main body",
            "",
            "### 2.5 第五节",
            "",
            "body five",
            "",
            "### 2.9 第九节",
            "",
            "body nine",
            "",
            "#### 2.9.1 九的子节",
            "",
            "body nine one",
            "",
            "### 2.10 第十节",
            "",
            "body ten",
            "",
            "## 3. 末尾",
            "",
            "tail body",
            "",
        ]
    )
    duplicate = "\n".join(
        [
            "# 帧契约：重复",
            "",
            "## 2.5 冲突层级",
            "",
            "top body",
            "",
            "### 2.5 重复子节",
            "",
            "child body",
            "",
        ]
    )
    fenced = "\n".join(
        [
            "# 帧契约：围栏",
            "",
            "## 1. 真节",
            "",
            "real body",
            "",
            "```text",
            "## 2.9 假标题",
            "```",
            "",
            "## 3. 后续",
            "",
            "tail body",
            "",
            "~~~~",
            "### 2.9.2 也假",
            "~~~~",
            "",
        ]
    )
    nested = "\n".join(
        [
            "# 帧契约：层级",
            "",
            "## 2.5 顶层的二点五",
            "",
            "top body",
            "",
            "### 2.5.1 子节",
            "",
            "child body",
            "",
            "## 7. 第七节",
            "",
            "seven body",
            "",
        ]
    )
    with tempfile.TemporaryDirectory(prefix="dz_contract_slice_") as tmp:
        docs_dir = Path(tmp) / "docs"
        contract_dir = docs_dir / "frame_contracts"
        contract_dir.mkdir(parents=True)
        (contract_dir / "sample.md").write_text(sample, encoding="utf-8")
        (contract_dir / "dup.md").write_text(duplicate, encoding="utf-8")
        (contract_dir / "fenced.md").write_text(fenced, encoding="utf-8")
        (contract_dir / "nested.md").write_text(nested, encoding="utf-8")

        # docs 夹具（#316 AC3）：临时目录里造 docs/agents 与 docs/components，
        # 不碰真实 docs/；`--docs-dir` 覆盖让 stem/path 寻址走这套夹具。
        guide = "\n".join(
            [
                "# 指南",
                "",
                "## 1. 起点",
                "",
                "start body",
                "",
                "### 1.1 子起点",
                "",
                "sub start body",
                "",
                "## 2. 中段",
                "",
                "middle body",
                "",
                "## 7. 第七节",
                "",
                "seven body",
                "",
            ]
        )
        plain = "\n".join(["# 无编号文档", "", "## 标题没有编号", "", "body", ""])
        agents_dir = docs_dir / "agents"
        components_dir = docs_dir / "components"
        agents_dir.mkdir(parents=True)
        components_dir.mkdir(parents=True)
        (agents_dir / "guide.md").write_text(guide, encoding="utf-8")
        (agents_dir / "plain.md").write_text(plain, encoding="utf-8")
        (agents_dir / "shared.md").write_text(guide, encoding="utf-8")
        (components_dir / "shared.md").write_text(guide, encoding="utf-8")

        rc, out, err = run(["sample", "§2.9"], contract_dir, docs_dir)
        expected = "### 2.9 第九节\n\nbody nine"
        check("section hit", rc == 0 and out.strip() == expected, f"rc={rc} out={out!r} err={err!r}")

        rc, out, _err = run(["sample", "2.9"], contract_dir, docs_dir)  # § 前缀可省略
        check("section arg without §", rc == 0 and out.strip() == expected, f"rc={rc} out={out!r}")

        rc, out, _err = run(["sample", "§2.9", "--children"], contract_dir, docs_dir)
        expected_children = "### 2.9 第九节\n\nbody nine\n\n#### 2.9.1 九的子节\n\nbody nine one"
        check("children", rc == 0 and out.strip() == expected_children, f"rc={rc} out={out!r}")

        rc, out, _err = run(["sample", "§2"], contract_dir, docs_dir)
        check("top-level stops before subsections", rc == 0 and "main body" in out and "2.5" not in out,
              f"rc={rc} out={out!r}")

        rc, out, _err = run(["nested", "§2.5"], contract_dir, docs_dir)
        check("duplicate parent number vs child is not a conflict",
              rc == 0 and "top body" in out and "child body" not in out, f"rc={rc} out={out!r}")

        rc, out, _err = run(["nested", "§2.5.1"], contract_dir, docs_dir)
        check("third-level number resolves", rc == 0 and "### 2.5.1 子节" in out, f"rc={rc} out={out!r}")

        rc, _out, err = run(["sample", "§2.11"], contract_dir, docs_dir)
        check("unknown subsection lists siblings under its parent", rc == 1
              and "sections under §2: §2.10 §2.9 §2.5" in err, f"rc={rc} err={err!r}")

        rc, _out, err = run(["sample", "§7.9"], contract_dir, docs_dir)
        check("unknown section exits 1 with candidates", rc == 1
              and "top-level sections" in err and "§1" in err, f"rc={rc} err={err!r}")

        rc, _out, err = run(["dup", "§2.5"], contract_dir, docs_dir)
        check("duplicate number is rejected with lines", rc == 1 and "duplicate" in err
              and "lines 3, 7" in err, f"rc={rc} err={err!r}")

        rc, _out, err = run(["fenced", "§2.9"], contract_dir, docs_dir)
        check("fenced fake heading is not a section", rc == 1 and "unknown section" in err,
              f"rc={rc} err={err!r}")

        rc, _out, err = run(["fenced", "§2.9.2"], contract_dir, docs_dir)
        check("tilde-fenced fake heading is not a section", rc == 1 and "unknown section" in err,
              f"rc={rc} err={err!r}")

        rc, out, _err = run(["--list"], contract_dir, docs_dir)
        check("--list shows numbered headings", rc == 0 and "§2.9 第九节" in out
              and "§3 末尾" in out, f"rc={rc} out={out!r}")
        check("--list flags duplicates", rc == 0 and "§2.5 冲突层级 [duplicate]" in out
              and "§2.5 重复子节 [duplicate]" in out, f"out={out!r}")
        check("--list skips fenced fake headings", "假标题" not in out and "也假" not in out,
              f"out={out!r}")

        rc, _out, err = run(["ghost", "§1"], contract_dir, docs_dir)
        check("unknown contract exits 1 with candidates", rc == 1 and "candidates=" in err
              and "sample" in err, f"rc={rc} err={err!r}")

        rc, _out, err = run(["sample"], contract_dir, docs_dir)
        check("missing section exits 2", rc == 2 and "missing contract stem or section" in err,
              f"rc={rc} err={err!r}")

        rc, _out, err = run(["sample", "§7.x"], contract_dir, docs_dir)
        check("invalid section exits 2", rc == 2 and "invalid section number" in err,
              f"rc={rc} err={err!r}")

        rc, _out, err = run(["--list", "sample"], contract_dir, docs_dir)
        check("--list rejects a stem and points at --outline", rc == 2
              and "--outline <stem>" in err, f"rc={rc} err={err!r}")

        rc, _out, err = run([])
        check("no arguments exits 2 with usage", rc == 2 and USAGE in err, f"rc={rc} err={err!r}")

        # ---- #316 AC3: --outline + docs 目标（docs 夹具在临时目录，不碰真实 docs/）----
        rc, out, err = run(["--outline", "sample"], contract_dir, docs_dir)
        check("--outline renders a contract block with titles", rc == 0
              and out.splitlines()[0].startswith("sample (")
              and "§2.9 第九节" in out, f"rc={rc} out={out!r} err={err!r}")

        rc, out, err = run(["--outline", "guide"], contract_dir, docs_dir)
        check("--outline resolves a docs stem", rc == 0
              and out.splitlines()[0].startswith("guide (")
              and "§7 第七节" in out, f"rc={rc} out={out!r} err={err!r}")

        rc, out, err = run(["--outline", "docs/agents/guide.md"], contract_dir, docs_dir)
        check("--outline resolves a repo-relative docs path", rc == 0
              and "guide (docs/agents/guide.md)" in out.splitlines()[0]
              and "§1 起点" in out, f"rc={rc} out={out!r} err={err!r}")

        rc, out, err = run(["docs/agents/guide.md", "§1", "--children"], contract_dir, docs_dir)
        check("docs path target slices by number", rc == 0 and "sub start body" in out,
              f"rc={rc} out={out!r} err={err!r}")

        rc, out, err = run(["guide", "§1"], contract_dir, docs_dir)
        check("docs stem target slices by number", rc == 0 and "start body" in out
              and "sub start body" not in out, f"rc={rc} out={out!r} err={err!r}")

        rc, out, err = run(["--outline", "plain"], contract_dir, docs_dir)
        check("docs target without numbered headings says so", rc == 0
              and "(no numbered headings)" in out, f"rc={rc} out={out!r} err={err!r}")

        rc, _out, err = run(["--outline", "ghost"], contract_dir, docs_dir)
        check("--outline unknown target exits 1 with candidates", rc == 1
              and "candidates=" in err and "guide" in err, f"rc={rc} err={err!r}")

        rc, _out, err = run(["--outline", "shared"], contract_dir, docs_dir)
        check("ambiguous docs stem lists both candidates", rc == 1
              and "ambiguous" in err and "docs/agents/shared.md" in err
              and "docs/components/shared.md" in err, f"rc={rc} err={err!r}")

        rc, _out, err = run(["--outline", "sample", "--list"], contract_dir, docs_dir)
        check("--outline and --list are mutually exclusive", rc == 2
              and "mutually exclusive" in err, f"rc={rc} err={err!r}")

        rc, _out, err = run(["--outline", "sample", "§1"], contract_dir, docs_dir)
        check("--outline takes no positional arguments", rc == 2
              and "positional" in err, f"rc={rc} err={err!r}")

        rc, _out, err = run(["--outline", "docs/agents/missing.md"], contract_dir, docs_dir)
        check("missing docs path is a usage error", rc == 2 and "not found" in err,
              f"rc={rc} err={err!r}")

        rc, _out, err = run(["docs/agents/missing.md", "§1"], contract_dir, docs_dir)
        check("missing docs path target exits 2", rc == 2 and "not found" in err,
              f"rc={rc} err={err!r}")

        total = checks[0]
    if failures:
        return 1, "", "\n".join(f"FAIL {detail}" for detail in failures)
    return 0, (
        f"selftest OK: {total} cases "
        "(hit / children / unknown / duplicate / fence / list / outline)"
    ), ""


def main(argv: list[str] | None = None) -> int:
    rc, out, err = run(sys.argv[1:] if argv is None else argv)
    if out:
        sys.stdout.write(out if out.endswith("\n") else out + "\n")
    if err:
        sys.stderr.write(err if err.endswith("\n") else err + "\n")
    return rc


if __name__ == "__main__":
    sys.exit(main())
