#!/usr/bin/env python3
"""check_sysmon_metric_keys.py — SYSMON 指标键集合的确定性检查（#309 AC3）

为什么需要
----------
同一份 15 键集合在 5 处有副本：
  1. `libs/platform/include/dztrader/platform/sysmon.h` 的 `SYSMON_METRIC_KEYS`
     （每条是 `SYSMON_METRIC_X` 常量名，需先解析常量定义）；
  2. `docs/frame_contracts/sysmon.md` §2.5 的反引号键列表（语义真相源）；
  3. `apps/webui/frontend/src/test-utils/sysmonSnapshot.ts` 的 `SYSMON_METRIC_KEYS`；
  4. `apps/webui/tests/sysmon_snapshot_fixture.h` 的 `availability.metrics` 键集合
     （#309 AC5 把原先两份内联夹具抽成共享头，夹具来源因此只剩这一处）。
另有 `apps/sysmon/snapshot_builder.cpp` 的 15 个显式 `metric ==` 分支（其余键落
`mark_not_implemented` 兜底）。

当前四处集合一致，但**没有任何检查器绑定**：#295 的 Standards 审查 finding 5 正是
「两份 webui 夹具 13/15 vs 15/15」，靠人眼发现；修复只做了「补齐两键」，没有抽取也没有
检查器。改一个键的触点实测 15 个文件（#295）、21 个文件（#296），下一波 #301/#302 还要
再动这两键的判定与展示——现在把「集合相等」做成机器可验证的。

规则
----
R1 sysmon.h：解析 `inline constexpr const char* SYSMON_METRIC_X = "a.b";` 常量，再按
   `SYSMON_METRIC_KEYS` 数组的元素求值；`std::array<const char*, N>` 的 N 必须等于条目数。
R2 契约：`docs/frame_contracts/sysmon.md` §2.5 里第一个反引号键列表行（以 `` `system` `` 开头
   的那一行），提取全部反引号 token。
R3 TS 夹具：`SYSMON_METRIC_KEYS` 数组字面量（括号配平，跳过注释）。
R4 共享测试夹具：`{"metrics", {...}}` 内层的键（括号配平 + 对象键扫描）。
R5 snapshot_builder：`for (const char* key : platform::SYSMON_METRIC_KEYS)` 循环体里的
   显式 `metric == platform::SYSMON_METRIC_X` 分支必须覆盖全部键；**兜底分支只能是
   `mark_not_implemented` 语义**——新增键悄悄落进兜底即报错（这正是「加了键但忘了实现」
   的形态）。
四个集合逐元素相等（R1 == R2 == R3 == R4）；顺序也相同（R1 的定义顺序是发送方顺序，
契约与夹具必须跟随）。

口径
----
- 解析失败（文件缺失 / 锚点找不到 / 括号不配平）→ exit 2，不静默跳过。
- 集合不一致 / builder 分支缺失 / 兜底漂移 → exit 1，逐条打印「哪个源、缺/多哪些键」。
- `--root <dir>`：在临时副本上跑（反向验证用；默认 = 仓库根）。
- `--selftest`：临时目录构造四个 mini 源，覆盖四源相等 / 少键 / 多键 / 顺序漂移 /
  夹具缺键 / builder 缺分支 / 兜底漂移 / 解析失败 exit 2，不联网、不碰工作区。

用法
----
  python3 scripts/check_sysmon_metric_keys.py               # 扫真实树
  python3 scripts/check_sysmon_metric_keys.py --root <dir>  # 扫临时副本
  python3 scripts/check_sysmon_metric_keys.py --selftest    # 检查器自测

退出码: 0=一致, 1=有漂移, 2=解析/环境错误
"""

from __future__ import annotations

import argparse
import re
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]

H_HEADER = "libs/platform/include/dztrader/platform/sysmon.h"
CONTRACT = "docs/frame_contracts/sysmon.md"
TS_FIXTURE = "apps/webui/frontend/src/test-utils/sysmonSnapshot.ts"
TEST_FIXTURE = "apps/webui/tests/sysmon_snapshot_fixture.h"
BUILDER = "apps/sysmon/snapshot_builder.cpp"

# sysmon.h：常量定义与数组定义
H_CONST_RE = re.compile(r'inline\s+constexpr\s+const\s+char\s*\*\s*(SYSMON_METRIC_[A-Z0-9_]+)\s*=\s*"([^"]+)"\s*;')
H_ARRAY_START_RE = re.compile(r"inline\s+constexpr\s+std::array<const\s+char\s*\*,\s*(\d+)>\s+SYSMON_METRIC_KEYS\s*=\s*\{")
H_ARRAY_TOKEN_RE = re.compile(r"SYSMON_METRIC_[A-Z0-9_]+")

# 契约 §2.5 的键列表行：第一个以 `system` 开头的反引号行
CONTRACT_KEY_LINE_RE = re.compile(r"^\s*`system`(?:[、,]\s*`[^`]+`)*[。.]?\s*$", re.MULTILINE)
CONTRACT_TOKEN_RE = re.compile(r"`([^`]+)`")

# TS 数组：`export const SYSMON_METRIC_KEYS = [ ... ] as const;`
TS_ARRAY_START_RE = re.compile(r"export\s+const\s+SYSMON_METRIC_KEYS\s*=\s*\[")
TS_STRING_RE = re.compile(r"'([^']*)'|\"([^\"]*)\"")

# C++ 夹具：`{"metrics", { { "system", ... } ... } }`
FIXTURE_METRICS_START_RE = re.compile(r'\{\s*"metrics"\s*,\s*\{')
FIXTURE_OBJECT_KEY_RE = re.compile(r'\{\s*"([A-Za-z0-9_.]+)"\s*,\s*\{')

# builder：循环体里的显式分支与兜底
BUILDER_LOOP_RE = re.compile(r"for\s*\(\s*const\s+char\s*\*\s*key\s*:\s*platform::SYSMON_METRIC_KEYS\s*\)\s*\{")
BUILDER_BRANCH_RE = re.compile(r"metric\s*==\s*platform::(SYSMON_METRIC_[A-Z0-9_]+)")
BUILDER_FALLBACK_RE = re.compile(r"\}\s*else\s*\{\s*mark_not_implemented\s*\(")


class CheckError(Exception):
    """解析/环境错误 → exit 2（不静默跳过）。"""


def read_text(root: Path, rel: str) -> str:
    path = root / rel
    if not path.is_file():
        raise CheckError(f"missing source file: {rel}")
    try:
        return path.read_text(encoding="utf-8")
    except OSError as exc:
        raise CheckError(f"cannot read {rel}: {exc}") from exc


def _braced_body(text: str, start: int, label: str) -> str:
    """从 text[start] 位置的 `{` 起做括号配平，返回花括号内的内容（不含两端花括号）。

    跳过字符串字面量与行注释，避免 `"{"` / `// }` 破坏配平。反斜杠转义的引号按 C 习惯处理。
    """
    if start >= len(text) or text[start] != "{":
        raise CheckError(f"{label}: expected '{{' at offset {start}")
    depth = 0
    in_string: str | None = None
    escaped = False
    line_comment = False
    for index in range(start, len(text)):
        char = text[index]
        if line_comment:
            if char == "\n":
                line_comment = False
            continue
        if in_string is not None:
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == in_string:
                in_string = None
            continue
        if char == "/" and index + 1 < len(text) and text[index + 1] == "/":
            line_comment = True
            continue
        if char in "\"'":
            in_string = char
            continue
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return text[start + 1:index]
    raise CheckError(f"{label}: unbalanced braces")


def parse_header(root: Path) -> list[str]:
    """R1：sysmon.h 的 SYSMON_METRIC_KEYS（常量先解析成字面量，再按数组元素求值）。"""
    text = read_text(root, H_HEADER)
    constants = {name: value for name, value in H_CONST_RE.findall(text)}
    match = H_ARRAY_START_RE.search(text)
    if match is None:
        raise CheckError(f"{H_HEADER}: SYSMON_METRIC_KEYS array definition not found")
    declared = int(match.group(1))
    body = _braced_body(text, text.index("{", match.start()), H_HEADER)
    tokens = H_ARRAY_TOKEN_RE.findall(body)
    missing = [token for token in tokens if token not in constants]
    if missing:
        raise CheckError(f"{H_HEADER}: array references undefined constants: {missing}")
    keys = [constants[token] for token in tokens]
    if declared != len(keys):
        raise CheckError(
            f"{H_HEADER}: declared size {declared} != {len(keys)} entries")
    return keys


def parse_contract(root: Path) -> list[str]:
    """R2：契约 §2.5 的反引号键列表行（语义真相源）。"""
    text = read_text(root, CONTRACT)
    match = CONTRACT_KEY_LINE_RE.search(text)
    if match is None:
        raise CheckError(f"{CONTRACT}: availability metric key list line not found")
    keys = CONTRACT_TOKEN_RE.findall(match.group(0))
    if not keys:
        raise CheckError(f"{CONTRACT}: key list line has no backticked tokens")
    return keys


def parse_ts_fixture(root: Path) -> list[str]:
    """R3：TS 夹具的 SYSMON_METRIC_KEYS 数组字面量。"""
    text = read_text(root, TS_FIXTURE)
    match = TS_ARRAY_START_RE.search(text)
    if match is None:
        raise CheckError(f"{TS_FIXTURE}: SYSMON_METRIC_KEYS array not found")
    body = _bracket_body(text, text.index("[", match.start()), TS_FIXTURE)
    keys = []
    for single, double in TS_STRING_RE.findall(body):
        keys.append(single or double)
    if not keys:
        raise CheckError(f"{TS_FIXTURE}: SYSMON_METRIC_KEYS array has no string entries")
    return keys


def _bracket_body(text: str, start: int, label: str) -> str:
    """方括号配平（TS 数组用）；跳过字符串与行注释。"""
    if start >= len(text) or text[start] != "[":
        raise CheckError(f"{label}: expected '[' at offset {start}")
    depth = 0
    in_string: str | None = None
    escaped = False
    line_comment = False
    for index in range(start, len(text)):
        char = text[index]
        if line_comment:
            if char == "\n":
                line_comment = False
            continue
        if in_string is not None:
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == in_string:
                in_string = None
            continue
        if char == "/" and index + 1 < len(text) and text[index + 1] == "/":
            line_comment = True
            continue
        if char in "\"'":
            in_string = char
            continue
        if char == "[":
            depth += 1
        elif char == "]":
            depth -= 1
            if depth == 0:
                return text[start + 1:index]
    raise CheckError(f"{label}: unbalanced brackets")


def parse_cpp_fixture(root: Path) -> list[str]:
    """R4：共享测试夹具 `availability.metrics` 的键集合（对象键扫描）。"""
    text = read_text(root, TEST_FIXTURE)
    match = FIXTURE_METRICS_START_RE.search(text)
    if match is None:
        raise CheckError(f"{TEST_FIXTURE}: availability.metrics literal not found")
    start = text.index("{", match.start())
    body = _braced_body(text, start, TEST_FIXTURE)
    keys = FIXTURE_OBJECT_KEY_RE.findall(body)
    if not keys:
        raise CheckError(f"{TEST_FIXTURE}: availability.metrics has no object keys")
    return keys


def parse_builder(root: Path) -> tuple[set[str], set[str]]:
    """R5：builder 循环体里的显式分支常量集合，以及兜底是否存在。

    返回 (branch_constants, fallback_kinds)。兜底必须只出现一次且为 mark_not_implemented；
    新增键悄悄落进兜底 = 显式分支集合缺该键（由上层比对报出）。
    """
    text = read_text(root, BUILDER)
    match = BUILDER_LOOP_RE.search(text)
    if match is None:
        raise CheckError(f"{BUILDER}: metric key loop not found")
    body = _braced_body(text, text.index("{", match.start()), BUILDER)
    branches = set(BUILDER_BRANCH_RE.findall(body))
    fallbacks = BUILDER_FALLBACK_RE.findall(body)
    return branches, {fallback for fallback in fallbacks}


def compare(root: Path) -> list[str]:
    """返回问题列表（空 = 一致）。解析错误抛 CheckError（→ exit 2）。"""
    header_keys = parse_header(root)
    contract_keys = parse_contract(root)
    ts_keys = parse_ts_fixture(root)
    cpp_keys = parse_cpp_fixture(root)
    builder_branches, builder_fallback = parse_builder(root)

    problems: list[str] = []
    sources = (
        ("contract §2.5", contract_keys),
        (TS_FIXTURE, ts_keys),
        (TEST_FIXTURE, cpp_keys),
    )
    for label, keys in sources:
        if set(keys) != set(header_keys):
            problems.append(
                f"{label}: key set differs from {H_HEADER} SYSMON_METRIC_KEYS"
                f" | missing={sorted(set(header_keys) - set(keys)) or '-'}"
                f" extra={sorted(set(keys) - set(header_keys)) or '-'}")
        elif keys != header_keys:
            # 顺序也要一致：R1 的数组顺序是发送方顺序，契约与夹具必须跟随。
            problems.append(f"{label}: key order differs from {H_HEADER}")

    # builder 显式分支覆盖全部键（常量名 → 字面量反查）
    constants = {name: value for name, value in H_CONST_RE.findall(read_text(root, H_HEADER))}
    branch_literals = {constants.get(name, f"<undefined {name}>") for name in builder_branches}
    uncovered = [key for key in header_keys if key not in branch_literals]
    if uncovered:
        problems.append(
            f"{BUILDER}: explicit metric branches miss keys {uncovered}"
            f" (they would silently fall into the not_implemented fallback)")
    if not builder_fallback:
        problems.append(
            f"{BUILDER}: no mark_not_implemented fallback branch found (expected exactly one)")
    return problems


def selftest() -> int:
    """在临时目录构造四个 mini 源，锁住逐条规则与失败形态；不联网、不碰工作区。"""
    with tempfile.TemporaryDirectory(prefix="dz_metric_keys_selftest_") as tmp:
        root = Path(tmp)

        def layout(root: Path) -> None:
            for rel in (H_HEADER, CONTRACT, TS_FIXTURE, TEST_FIXTURE, BUILDER):
                (root / rel).parent.mkdir(parents=True, exist_ok=True)

        def header(keys: list[str], declared: int | None = None, drop_const: str | None = None) -> str:
            consts = "\n".join(
                f'inline constexpr const char* SYSMON_METRIC_{_const_name(k)} = "{k}";'
                for k in keys if k != drop_const)
            entries = "\n".join(f"    SYSMON_METRIC_{_const_name(k)}," for k in keys if k != drop_const)
            size = declared if declared is not None else len(keys)
            return (f"// sample\n{consts}\n\n"
                    f"inline constexpr std::array<const char*, {size}> SYSMON_METRIC_KEYS = {{\n"
                    f"{entries}\n}};\n")

        def contract(keys: list[str]) -> str:
            line = "、".join(f"`{k}`" for k in keys) + "。"
            return ("# 契约\n\n## 2. 快照\n\n### 2.5 availability\n\n"
                    "`metrics` 键集合（固定）：\n\n" + line + "\n\n`reason` 为自由文案。\n")

        def ts(keys: list[str]) -> str:
            entries = "\n".join(f"  '{k}'," for k in keys)
            return ("// 夹具\nexport const SYSMON_METRIC_KEYS = [\n" + entries + "\n] as const\n")

        def cpp(keys: list[str]) -> str:
            entries = "\n".join(
                f'            {{"{k}", {{"available", true}}}},'
                for k in keys)
            return ("#include <nlohmann/json.hpp>\ninline nlohmann::json f() {\n  return {\n"
                    '    {"availability",\n     {{"metrics",\n       {'
                    + entries.replace("\n", "") + "}}}},\n  };\n}\n")

        def builder(keys: list[str], drop: str | None = None, fallback: bool = True) -> str:
            rendered = [k for k in keys if k != drop]
            branches = "\n".join(
                f'        if (metric == platform::SYSMON_METRIC_{_const_name(k)}) {{\n'
                f'            mark_available(a, key);\n'
                f'        }} else'
                for k in rendered)
            # 兜底形态与真实文件一致：最后一个 `} else {` 分支里是 mark_not_implemented。
            tail = (" {\n            mark_not_implemented(a, key);\n        }\n"
                    if fallback else
                    " {\n            // fallback removed\n        }\n")
            return ("void build() {\n"
                    "    for (const char* key : platform::SYSMON_METRIC_KEYS) {\n"
                    "        const std::string_view metric(key);\n"
                    f"{branches}{tail}"
                    "    }\n}\n")

        def write(root: Path, keys: list[str], **over) -> None:
            (root / H_HEADER).write_text(
                header(keys, over.get("declared"), over.get("drop_const")), encoding="utf-8")
            (root / CONTRACT).write_text(contract(over.get("contract_keys", keys)), encoding="utf-8")
            (root / TS_FIXTURE).write_text(ts(over.get("ts_keys", keys)), encoding="utf-8")
            (root / TEST_FIXTURE).write_text(cpp(over.get("cpp_keys", keys)), encoding="utf-8")
            (root / BUILDER).write_text(
                builder(keys, over.get("builder_drop"), over.get("fallback", True)), encoding="utf-8")

        base = ["system", "cpu.total", "cpu.cores", "memory", "swap"]

        cases: list[tuple[str, list[str], dict, int]] = [
            ("all four sources agree", base, {}, 0),
            ("contract misses a key", base, {"contract_keys": base[:-1]}, 1),
            ("ts fixture has an extra key", base, {"ts_keys": base + ["extra"]}, 1),
            ("cpp fixture misses a key", base, {"cpp_keys": base[:-1]}, 1),
            ("contract order differs", base, {"contract_keys": [base[0]] + base[:0:-1]}, 1),
            ("header declared size mismatch", base, {"declared": len(base) + 1}, 2),
            ("builder misses a branch", base, {"builder_drop": "swap"}, 1),
            ("builder fallback removed", base, {"fallback": False}, 1),
        ]
        failures: list[str] = []
        for name, keys, over, want in cases:
            # 每个用例都在全新子目录里构造，避免上一用例残留。
            case_root = root / re.sub(r"[^a-z0-9]+", "_", name.lower())
            case_root.mkdir(parents=True, exist_ok=True)
            layout(case_root)
            write(case_root, keys, **over)
            try:
                problems = compare(case_root)
                rc = 0 if not problems else 1
                detail = "; ".join(problems)
            except CheckError as exc:
                rc, detail = 2, str(exc)
            if rc != want:
                failures.append(f"{name}: expected rc={want}, got rc={rc} ({detail})")
                continue
            print(f"selftest ok {name} -> rc={rc}")

        # 解析失败必须 exit 2（文件缺失 / 锚点缺失），不静默跳过。
        missing_root = root / "missing_anchor"
        missing_root.mkdir(parents=True)
        layout(missing_root)
        write(missing_root, base)
        (missing_root / CONTRACT).write_text("# 没有键列表行的契约\n", encoding="utf-8")
        try:
            compare(missing_root)
            failures.append("missing contract key list: expected rc=2, got rc=0")
        except CheckError as exc:
            print(f"selftest ok missing contract key list -> rc=2 ({exc})")

    if failures:
        for line in failures:
            print(f"selftest FAIL {line}", file=sys.stderr)
        return 1
    print(f"selftest: {len(cases) + 1} cases ok (four-source equality / missing / extra / order / "
          "declared size / builder branch / fallback / parse failure)")
    return 0


def _const_name(key: str) -> str:
    """键 → sysmon.h 常量名后缀（`cpu.total` → `CPU_TOTAL`）。与 sysmon.h 的命名一一对应。"""
    return key.upper().replace(".", "_")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="SYSMON metric key set checker (#309 AC3)")
    parser.add_argument("--root", type=Path, default=REPO_ROOT,
                        help="repository root to scan (default: this repository)")
    parser.add_argument("--selftest", action="store_true",
                        help="run the checker against a temp fixture tree (worktree untouched)")
    args = parser.parse_args(sys.argv[1:] if argv is None else argv)

    if args.selftest:
        return selftest()

    try:
        problems = compare(args.root)
    except CheckError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    if problems:
        for problem in problems:
            print(f"error: {problem}", file=sys.stderr)
        return 1
    header_keys = parse_header(args.root)
    print(f"OK: {len(header_keys)} SYSMON metric keys consistent across "
          "sysmon.h / contract §2.5 / ts fixture / cpp fixture; "
          "snapshot_builder branches cover all keys")
    return 0


if __name__ == "__main__":
    sys.exit(main())
