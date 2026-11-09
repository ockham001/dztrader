#!/usr/bin/env python3
"""check_orphan_exports.py — 孤儿导出 lint（#274）

为什么需要：本仓的收敛手法是「抽共用模块 + 保留旧名转发」（AGENTS 三.1「同一能力不允许
两套实现并存」）。这个手法天然生产孤儿——旧名被转发出来后没人再用，而现有护栏全都不管：

  - TS 侧 `tsconfig.json` 的 `noUnusedLocals` 只查**局部变量**，不查 `export`
  - C++ 侧头文件里的 `inline constexpr` 未使用，编译器不给任何告警

实测复发 4 处，全部由 reviewer 事后抓出（#246 孤儿函数；#248 的
`kTdOrdersDefaultLimit` / `kTdOrdersMaxLimit` 别名 + `TD_ORDERS_PREFETCH_THRESHOLD`，
最后一处到第 2 轮只读验证才抓到）。

判据分两级（只对一级报错，二级只提示）：

  - **一级（失败）真孤儿**：符号在**任何文件**里都不再出现——只剩定义本身。这是「抽走后
    忘了删旧名」的直接产物（#248 的 `kTdOrdersDefaultLimit` / `TD_ORDERS_PREFETCH_THRESHOLD`
    就是这一级），必然是可删的死定义。
  - **二级（仅提示）过度导出**：符号只在定义文件内使用，`export` 没有外部消费者。不是死
    代码，只是导出面多余；按 AGENTS「发现无关死代码只提示，不删除」只打 `note:` 不失败。

测试文件里的引用算「已用」——被测试直接引用的导出不是孤儿，只是没有生产消费者。

刻意不校验的东西（已知边界，有意取舍，不是漏洞）：
  - 只认 TS 的 `export const` / `export function` 与 C++ 头文件的 `inline constexpr`。
    TS 的 `export type` / `export interface` 不做：类型孤儿要靠类型检查器才判得准，
    正则会把「只用于类型标注」误判成孤儿。C++ 非成员函数声明同理——重载、模板、
    `extern "C"` 都会让「名字出现即引用」的近似失效。
  - 用「标识符出现」近似「引用」：字符串/注释里出现同名 token 也算已用（偏保守，
    宁可漏报不误报）。
  - 不做跨语言判定：TS 符号只在 TS/Vue 里找引用，C++ 符号只在 C++ 里找。
  - 宏（`#define`）不做：宏的引用形态（`#ifdef`、字符串化）无法用 token 近似。

用法:
  python scripts/check_orphan_exports.py              # 扫本仓
  python scripts/check_orphan_exports.py --root <dir>  # 扫指定根（自测用）
  python scripts/check_orphan_exports.py --selftest    # 检查器自测（临时目录）

退出码: 0=通过, 1=有孤儿, 2=环境错误
"""

from __future__ import annotations

import argparse
import collections
import re
import sys
import tempfile
from pathlib import Path

DEFAULT_ROOT = Path(__file__).resolve().parents[1]

# 扫描面：定义候选 + 引用语料都限定在这两个子树内（apps 下的进程代码）。
TS_SRC_REL = "apps/webui/frontend/src"
CPP_APPS_REL = "apps"

# 定义候选的文件形态。
TS_DEF_SUFFIX = ".ts"
CPP_DEF_SUFFIX = ".h"

# 引用语料：TS 符号可能在 .vue 的 <script> 里被 import。
TS_REF_SUFFIXES = (".ts", ".vue")
CPP_REF_SUFFIXES = (".h", ".cpp")

# 永不参与判定的路径片段（生成物 / 类型声明 / 三方 / 构建产物）。
SKIP_PARTS = {
    "node_modules",
    "dist",
    "build",
    "install",
    ".cache",
    ".worktrees",
    ".agents",
    "third_party",
}

# 永不参与判定的相对路径前缀。`libs/strategy_api` 是策略 SDK 的 C API——
# 导出面是给外部用户的，仓内零引用是正常的（ADR 0003 的公开面）。
SKIP_REL_PREFIXES = ("libs/strategy_api/",)

# 生成物：`gen:types` 的产物，符号由 schema 决定而不是由消费者决定。
SKIP_REL_SUFFIXES = ("/src/types/generated.ts", "/dist/")

# TS 定义候选：行首（允许缩进）的 `export const NAME` / `export function NAME`。
TS_DEF_RE = re.compile(
    r"^[ \t]*export\s+(?:const|let|function)\s+(?P<name>[A-Za-z_$][\w$]*)",
    re.MULTILINE,
)

# C++ 定义候选：头文件里的 `inline constexpr TYPE NAME = ...;`（允许跨行到 `=`）。
# 只认这一种形态：它是「常量单源」被转发后最典型的遗留物（#248 的实际形态）。
CPP_DEF_RE = re.compile(
    r"^[ \t]*inline\s+constexpr\s+[^;=\n]*?(?P<name>[A-Za-z_]\w*)\s*=",
    re.MULTILINE,
)

# 标识符抽取（引用近似的分子）。数字前缀的 $ 由 TS 允许，这里一并接受。
IDENT_RE = re.compile(r"[A-Za-z_$][\w$]*")

# 已知且**有意**的孤儿导出：符号 -> 理由。留空是默认状态；每条都要写明为什么不是
# 死代码，否则就是该删的。
ALLOWLIST: dict[str, str] = {}


def _skipped(rel: str) -> bool:
    if any(part in SKIP_PARTS for part in rel.split("/")):
        return True
    if rel.startswith(SKIP_REL_PREFIXES):
        return True
    return any(rel.endswith(suffix) for suffix in SKIP_REL_SUFFIXES)


def _walk(root: Path, sub: str, suffixes: tuple[str, ...]) -> list[Path]:
    """遍历 `root/sub` 下指定后缀的文件（跳过 SKIP_PARTS 与不可读目录）。"""
    base = root / sub
    if not base.is_dir():
        return []
    out: list[Path] = []
    for path in base.rglob("*"):
        if not path.is_file() or path.suffix not in suffixes:
            continue
        if _skipped(path.relative_to(root).as_posix()):
            continue
        out.append(path)
    return sorted(out)


def _read(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""


def collect_definitions(root: Path) -> list[tuple[str, int, str, str]]:
    """收集定义候选：(相对路径, 行号, 语言, 符号名)。"""
    defs: list[tuple[str, int, str, str]] = []

    for path in _walk(root, TS_SRC_REL, (TS_DEF_SUFFIX,)):
        text = _read(path)
        rel = path.relative_to(root).as_posix()
        for match in TS_DEF_RE.finditer(text):
            line = text.count("\n", 0, match.start("name")) + 1
            defs.append((rel, line, "ts", match.group("name")))

    for path in _walk(root, CPP_APPS_REL, (CPP_DEF_SUFFIX,)):
        text = _read(path)
        rel = path.relative_to(root).as_posix()
        for match in CPP_DEF_RE.finditer(text):
            line = text.count("\n", 0, match.start("name")) + 1
            defs.append((rel, line, "cpp", match.group("name")))

    return defs


def build_reference_index(root: Path) -> dict[str, dict[str, int]]:
    """符号 -> {文件: 该标识符出现次数}（含定义文件自身）。

    用**次数**而不是集合：定义行本身必然命中一次，所以「只出现一次且全在定义文件」
    才能区分「死定义」与「只在内部使用的过度导出」。
    """
    index: dict[str, dict[str, int]] = {}

    corpora = [
        _walk(root, TS_SRC_REL, TS_REF_SUFFIXES),
        _walk(root, CPP_APPS_REL, CPP_REF_SUFFIXES),
    ]
    for files in corpora:
        for path in files:
            rel = path.relative_to(root).as_posix()
            for token, count in collections.Counter(IDENT_RE.findall(_read(path))).items():
                index.setdefault(token, {})[rel] = count

    return index


def find_orphans(
    root: Path,
) -> tuple[list[tuple[str, int, str, str]], list[tuple[str, int, str, str]]]:
    """返回 (一级死定义, 二级过度导出)；两者都是 (相对路径, 行号, 语言, 符号名)。"""
    definitions = collect_definitions(root)
    index = build_reference_index(root)

    dead: list[tuple[str, int, str, str]] = []
    over_exported: list[tuple[str, int, str, str]] = []
    seen: set[tuple[str, str]] = set()
    for rel, line, lang, name in definitions:
        if name in ALLOWLIST:
            continue
        key = (rel, name)
        if key in seen:
            continue
        refs = index.get(name, {})
        outside = {path: count for path, count in refs.items() if path != rel}
        if outside:
            continue  # 有外部消费者，正常
        seen.add(key)
        if refs.get(rel, 0) <= 1:
            # 定义文件里也只出现这一次（就是定义行本身）→ 纯死定义。
            dead.append((rel, line, lang, name))
        else:
            # 定义文件内还有别的使用 → 不是死代码，只是 export 没有外部消费者。
            over_exported.append((rel, line, lang, name))
    return sorted(dead), sorted(over_exported)


def run(root: Path) -> int:
    dead, over_exported = find_orphans(root)
    files = len(_walk(root, TS_SRC_REL, (TS_DEF_SUFFIX,))) + len(
        _walk(root, CPP_APPS_REL, (CPP_DEF_SUFFIX,))
    )

    for rel, line, lang, name in over_exported:
        print(f"note: over-exported (used only in its own file) {rel}:{line}: {name} ({lang})")

    if not dead:
        print(f"OK: {files} file(s) scanned, no dead exports")
        return 0

    print(f"error: {len(dead)} dead export(s) — 任何文件里都不再出现", file=sys.stderr)
    for rel, line, lang, name in dead:
        print(f"  {rel}:{line}: {name} ({lang})", file=sys.stderr)
    print(
        "hint: 抽共用模块后遗留的旧名转发即死定义；确认无消费者后删除"
        "（若确为有意保留，登记到 ALLOWLIST 并写明理由）",
        file=sys.stderr,
    )
    return 1


def selftest() -> int:
    """临时目录里构造已知形态，验证判据方向。"""
    # (说明, {相对路径: 内容}, 期望 dead, 期望 over_exported)
    cases: list[tuple[str, dict[str, str], list[str], list[str]]] = [
        (
            "死定义命中（只在定义行出现一次）",
            {
                "apps/webui/frontend/src/a.ts": "export const UNUSED_THING = 1\n",
            },
            ["UNUSED_THING"],
            [],
        ),
        (
            "#248 回归形态：旧名别名指向已删除的常量",
            {
                "apps/webui/frontend/src/a.ts": (
                    "export const LIVE_CONST = 30\n"
                    "export const OLD_ALIAS = LIVE_CONST\n"
                ),
                "apps/webui/frontend/src/b.ts": (
                    "import { LIVE_CONST } from './a'\nvoid LIVE_CONST\n"
                ),
            },
            ["OLD_ALIAS"],
            [],
        ),
        (
            "只在定义文件内使用 → 过度导出（不是死代码，不失败）",
            {
                "apps/webui/frontend/src/a.ts": (
                    "export const INNER = 1\nvoid INNER\nvoid INNER\n"
                ),
            },
            [],
            ["INNER"],
        ),
        (
            "测试文件引用算已用",
            {
                "apps/webui/frontend/src/a.ts": "export const USED_BY_TEST = 1\n",
                "apps/webui/frontend/src/__tests__/a.spec.ts": (
                    "import { USED_BY_TEST } from '../a'\nvoid USED_BY_TEST\n"
                ),
            },
            [],
            [],
        ),
        (
            "生产文件引用算已用（含 .vue 消费方）",
            {
                "apps/webui/frontend/src/a.ts": "export const USED_BY_VUE = 1\n",
                "apps/webui/frontend/src/V.vue": (
                    "<script setup lang=\"ts\">\nimport { USED_BY_VUE } from './a'\n"
                    "void USED_BY_VUE\n</script>\n"
                ),
            },
            [],
            [],
        ),
        (
            "C++ 头文件 inline constexpr 死定义命中",
            {
                "apps/ctp/td/x.h": "inline constexpr int kOrphanLimit = 200;\n",
            },
            ["kOrphanLimit"],
            [],
        ),
        (
            "C++ 被 cpp 消费则不算孤儿",
            {
                "apps/ctp/td/x.h": "inline constexpr int kUsedLimit = 200;\n",
                "apps/ctp/td/x.cpp": "int f() { return kUsedLimit; }\n",
            },
            [],
            [],
        ),
        (
            "排除目录不报（libs/strategy_api 是对外 C API 面）",
            {
                "libs/strategy_api/x.h": "inline constexpr int kPublicApi = 1;\n",
            },
            [],
            [],
        ),
        (
            "生成物不参与判定（generated.ts 的符号由 schema 决定）",
            {
                "apps/webui/frontend/src/types/generated.ts": (
                    "export interface TdGeneratedRow { a: string }\n"
                    "export const GENERATED_CONST = 1\n"
                ),
            },
            [],
            [],
        ),
        (
            "ALLOWLIST 登记的有意保留不报",
            {
                "apps/webui/frontend/src/a.ts": "export const DELIBERATE = 1\n",
            },
            [],
            [],
        ),
        (
            "非 export 的定义不参与判定",
            {
                "apps/webui/frontend/src/a.ts": "const LOCAL_ONLY = 1\nvoid LOCAL_ONLY\n",
            },
            [],
            [],
        ),
    ]

    allow = {"DELIBERATE": "selftest: 模拟有意保留"}
    failures = 0
    global ALLOWLIST
    saved = ALLOWLIST
    ALLOWLIST = {**saved, **allow}
    try:
        with tempfile.TemporaryDirectory() as tmp:
            for idx, (label, files, want_dead, want_over) in enumerate(cases):
                root = Path(tmp) / f"case{idx}"
                for rel, content in files.items():
                    path = root / rel
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_text(content, encoding="utf-8")
                dead, over = find_orphans(root)
                got_dead = sorted({name for _, _, _, name in dead})
                got_over = sorted({name for _, _, _, name in over})
                if got_dead != sorted(want_dead) or got_over != sorted(want_over):
                    print(
                        f"selftest FAIL: case {idx} ({label}): "
                        f"expected dead={sorted(want_dead)} over={sorted(want_over)}, "
                        f"got dead={got_dead} over={got_over}"
                    )
                    failures += 1
    finally:
        ALLOWLIST = saved

    if failures:
        return 1
    print(
        f"selftest OK: {len(cases)} cases (dead definition caught / alias-to-live const caught / "
        f"in-file-only reported as over-exported / test+vue references allowed / "
        f"C++ dead const caught / exclusions and allowlist honored)"
    )
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--root", type=Path, default=DEFAULT_ROOT, help="仓库根目录（默认：脚本所在仓库）"
    )
    parser.add_argument(
        "--selftest", action="store_true", help="检查器自测（临时目录，不碰工作区）"
    )
    args = parser.parse_args()

    if args.selftest:
        return selftest()

    root = args.root.resolve()
    if not root.is_dir():
        print(f"error: root is not a directory: {root}", file=sys.stderr)
        return 2
    return run(root)


if __name__ == "__main__":
    sys.exit(main())
