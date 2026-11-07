#!/usr/bin/env python3
"""check_sysmon_metric_keys.py — SYSMON 指标键集合的确定性检查（#309 AC3）

为什么需要
----------
同一份 15 键集合在 **5 处**有副本，与 #309 AC3 的口径一致：**4 处数据副本** + **1 处分支
镜像**（`apps/sysmon/snapshot_builder.cpp` 的 15 个显式 `metric ==` 分支，其余键落
`mark_not_implemented` 兜底）。四类数据副本：

  1. `libs/platform/include/dztrader/platform/sysmon.h` 的 `SYSMON_METRIC_KEYS`
     （每条是 `SYSMON_METRIC_X` 常量名，需先解析常量定义）；
  2. `docs/frame_contracts/sysmon.md` §2.5 的反引号键列表（语义真相源）；
  3. `apps/webui/frontend/src/test-utils/sysmonSnapshot.ts` 的 `SYSMON_METRIC_KEYS`；
  4. 测试快照夹具的 `availability.metrics` 键集合——来源清单由扫描根下的实际文件决定：
     #309 AC5 抽取后是共享头 `apps/webui/tests/sysmon_snapshot_fixture.h`（唯一实现），
     抽取前是两份 test .cpp 的内联 JSON 副本（两者都算同一类副本）；两种形态都读，
     不做第三种猜测（见 R4）。

当前四处集合一致，但**没有任何检查器绑定**：#295 的 Standards 审查 finding 5 正是
「两份 webui 夹具 13/15 vs 15/15」，靠人眼发现；修复只做了「补齐两键」，没有抽取也没有
检查器。改一个键的触点实测 15 个文件（#295）、21 个文件（#296），下一波 #301/#302 还要
再动这两键的判定与展示——现在把「集合相等」做成机器可验证的。

规则
----
R1 sysmon.h：解析 `inline constexpr const char* SYSMON_METRIC_X = "a.b";` 常量，再按
   `SYSMON_METRIC_KEYS` 数组的元素求值；`std::array<const char*, N>` 的 N 必须等于条目数。
R2 契约：`docs/frame_contracts/sysmon.md` §2.5 里**唯一**一条反引号键列表行（以
    `` `system` `` 开头、剥掉反引号与顿号后不剩正文的那一行），提取全部反引号 token。
    只在 §2.5 节体内找（§2.6 示例、§7 校验里也有反引号键名）；节内出现两条候选列表行
    视为契约自相矛盾 → 解析错误 exit 2。
R3 TS 夹具：`SYSMON_METRIC_KEYS` 数组字面量（括号配平，跳过注释）。
R4 测试快照夹具：每个来源 `{"metrics", {...}}` 内层的键（括号配平 + 对象键扫描）。
   来源 = 共享头 `sysmon_snapshot_fixture.h`（存在即唯一实现，两份 test .cpp 只 include
   它）否则 `sysmon_controller_test.cpp` / `sysmon_domain_service_test.cpp` 两份内联副本。
R5 snapshot_builder：`for (const char* key : platform::SYSMON_METRIC_KEYS)` 循环体里的
   显式 `metric == platform::SYSMON_METRIC_X` 分支集合必须**等于** `SYSMON_METRIC_KEYS`
   全部键——缺一个即报红并列出缺哪几个键（它们会悄悄落进兜底），多一个未登记的键、
   同一个键分支两次同样报红；**链尾那个无条件兜底 `else` 必须且只能有一个，且只能是
   `mark_not_implemented` 语义**（这正是「加了键但忘了实现」的形态）。
R6 夹具防回归：共享头已是唯一实现时，两份 test .cpp 不得再出现内联 `availability.metrics`
   副本（`check_shared_test_headers.py` 只认 `#include`，拦不住内联字面量；#295 的
   13/15 vs 15/15 正是这么来的）。
四类来源逐元素相等（R1 == R2 == R3 == R4，R4 可能是一处共享头或两份内联夹具）；顺序
也相同（R1 的定义顺序是发送方顺序，契约与夹具必须跟随）。

口径
----
- 解析失败（文件缺失 / 锚点找不到 / 括号不配平）→ exit 2，不静默跳过。
- 集合不一致 / builder 分支缺失或多余 / 兜底漂移 / 内联副本回归 → exit 1，逐条打印
  「哪个源、缺/多哪些键」。
- `--root <dir>`：在临时副本上跑（反向验证用；默认 = 仓库根）。
- `--selftest`：临时目录构造 mini 源树，覆盖四源相等 / 少键 / 多键 / 顺序漂移 / 夹具缺键 /
  builder 缺分支 / 分支重复 / 未登记键分支 / 兜底漂移 / 解析失败 exit 2 / 抽取前内联
  夹具形态（含少键）/ 共享头下内联副本回归 / **注释掉一条登记 → 红**（注释不算登记，
  四种来源各一例），不联网、不碰工作区。

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
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]

H_HEADER = "libs/platform/include/dztrader/platform/sysmon.h"
CONTRACT = "docs/frame_contracts/sysmon.md"
TS_FIXTURE = "apps/webui/frontend/src/test-utils/sysmonSnapshot.ts"
# 夹具来源清单（R4）：共享头存在时它是唯一实现；不存在（抽取前形态）时退回两份内联副本。
# 这份清单由 `--root` 下的实际文件决定（见 `fixture_sources()`），不做第三种猜测。
SHARED_TEST_FIXTURE = "apps/webui/tests/sysmon_snapshot_fixture.h"
INLINE_TEST_FIXTURES = (
    "apps/webui/tests/sysmon_controller_test.cpp",
    "apps/webui/tests/sysmon_domain_service_test.cpp",
)
BUILDER = "apps/sysmon/snapshot_builder.cpp"

# sysmon.h：常量定义与数组定义
H_CONST_RE = re.compile(
    r'inline\s+constexpr\s+const\s+char\s*\*\s*(SYSMON_METRIC_[A-Z0-9_]+)\s*=\s*"([^"]+)"\s*;')
H_ARRAY_START_RE = re.compile(
    r"inline\s+constexpr\s+std::array<const\s+char\s*\*,\s*(\d+)>\s+SYSMON_METRIC_KEYS\s*=\s*\{")
H_ARRAY_TOKEN_RE = re.compile(r"SYSMON_METRIC_[A-Z0-9_]+")

# 契约 §2.5 的键列表行：§2.5 节内第一个以 `system` 开头的反引号行（节外同名行不算）。
DOC_SECTION_25_RE = re.compile(r"^#{1,6}\s*2\.5(?=\s|$)")
DOC_ANY_HEADING_RE = re.compile(r"^#{1,6}\s")
CONTRACT_KEY_LINE_RE = re.compile(r"^\s*`system`(?:[、,]\s*`[^`]+`)*[。.]?\s*$")
CONTRACT_TOKEN_RE = re.compile(r"`([^`]+)`")

# TS 数组：`export const SYSMON_METRIC_KEYS = [ ... ] as const;`
TS_ARRAY_START_RE = re.compile(r"export\s+const\s+SYSMON_METRIC_KEYS\s*=\s*\[")
TS_STRING_RE = re.compile(r"'([^']*)'|\"([^\"]*)\"")

# C++ 夹具：`{"metrics", { { "system", ... } ... } }`
FIXTURE_METRICS_START_RE = re.compile(r'\{\s*"metrics"\s*,\s*\{')
FIXTURE_OBJECT_KEY_RE = re.compile(r'\{\s*"([A-Za-z0-9_.]+)"\s*,\s*\{')

# builder：循环体里的显式分支与兜底
BUILDER_LOOP_RE = re.compile(
    r"for\s*\(\s*const\s+char\s*\*\s*key\s*:\s*platform::SYSMON_METRIC_KEYS\s*\)\s*\{")
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


def _strip_comments(text: str) -> str:
    """把 C/C++/TS 注释替换成等长空格（换行保留），字符串字面量原样保留。

    等长是刻意的：调用方可以先剥注释、再按**同一份偏移**做括号配平与切片，不必两套坐标。
    只在 C++ / TS 来源上用——Markdown 不能这样处理（`https://…` 里的 `//` 会被当注释）。
    数字分隔符 `2'147'483'648` 里的 `'` 不算字符字面量起手：否则一个奇数个分隔符的
    文件会让扫描器把后半段的注释也当字符串内容（尾部的注释就剥不掉了）。
    """
    out = list(text)
    index = 0
    length = len(text)
    while index < length:
        char = text[index]
        if char in "\"'":
            # 数字分隔符：前一个字符是标识符/数字且后一个也是数字时，`'` 不是字面量起手。
            if char == "'" and index > 0 and index + 1 < length \
                    and (text[index - 1].isalnum() or text[index - 1] == "_") \
                    and text[index + 1].isalnum():
                index += 1
                continue
            quote = char
            index += 1
            while index < length:
                if text[index] == "\\":
                    index += 2
                    continue
                if text[index] == quote:
                    index += 1
                    break
                index += 1
            continue
        if char == "/" and index + 1 < length and text[index + 1] in "/*":
            if text[index + 1] == "/":
                stop = text.find("\n", index)
                stop = length if stop < 0 else stop
            else:
                stop = text.find("*/", index + 2)
                stop = length if stop < 0 else stop + 2
            for pos in range(index, stop):
                if out[pos] != "\n":
                    out[pos] = " "
            index = stop
            continue
        index += 1
    return "".join(out)


def _balanced_body(text: str, start: int, label: str) -> str:
    """从 text[start] 的括号起做配平，返回括号内的内容（不含两端括号）。

    三种来源共用（`{` C++ 数组/对象、`[` TS 数组）。调用方已用 `_strip_comments()`
    剥掉注释，这里只需跳过字符串字面量（`"{"` / `"}"` 之类会破坏配平）。
    """
    pairs = {"{": "}", "[": "]"}
    if start >= len(text) or text[start] not in pairs:
        raise CheckError(f"{label}: expected an opening bracket at offset {start}")
    opener, closer = text[start], pairs[text[start]]
    depth = 0
    in_string: str | None = None
    escaped = False
    index = start
    while index < len(text):
        char = text[index]
        if in_string is not None:
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == in_string:
                in_string = None
            index += 1
            continue
        if char in "\"'":
            in_string = char
        elif char == opener:
            depth += 1
        elif char == closer:
            depth -= 1
            if depth == 0:
                return text[start + 1:index]
        index += 1
    raise CheckError(f"{label}: unbalanced {opener!r} at offset {start}")


def parse_header(root: Path) -> list[str]:
    """R1：sysmon.h 的 SYSMON_METRIC_KEYS（常量先解析成字面量，再按数组元素求值）。"""
    # 先剥注释：头文件里出现过「注释掉的旧常量」也不算登记（否则删键后注释残留会假绿）。
    text = _strip_comments(read_text(root, H_HEADER))
    constants = {name: value for name, value in H_CONST_RE.findall(text)}
    match = H_ARRAY_START_RE.search(text)
    if match is None:
        raise CheckError(f"{H_HEADER}: SYSMON_METRIC_KEYS array definition not found")
    declared = int(match.group(1))
    body = _balanced_body(text, text.index("{", match.start()), H_HEADER)
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
    """R2：契约 §2.5 的反引号键列表行（语义真相源）。

    只在 `### 2.5` 节体内找：同一文档的 §2.6 示例、§7 校验规则里也有反引号键名，
    不按节界定会把示例里的键当成列表（或反过来漏掉真实列表）。
    """
    text = read_text(root, CONTRACT)
    lines = text.splitlines()
    starts = [index for index, line in enumerate(lines) if DOC_SECTION_25_RE.match(line)]
    if not starts:
        raise CheckError(f"{CONTRACT}: '### 2.5' section heading not found")
    if len(starts) > 1:
        raise CheckError(f"{CONTRACT}: {len(starts)} '### 2.5' section headings (expected one)")
    start = starts[0]
    end = len(lines)
    for index in range(start + 1, len(lines)):
        if DOC_ANY_HEADING_RE.match(lines[index]):
            end = index
            break
    candidates: list[tuple[int, list[str]]] = []
    for index in range(start + 1, end):
        if not CONTRACT_KEY_LINE_RE.match(lines[index]):
            continue
        keys = CONTRACT_TOKEN_RE.findall(lines[index])
        if keys:
            candidates.append((index, keys))
    if not candidates:
        raise CheckError(f"{CONTRACT}: availability metric key list line not found inside §2.5")
    if len(candidates) > 1:
        # 两个候选列表行 = 契约自己说不清哪份才是真相源；报解析错误而不是默默取第一个。
        raise CheckError(f"{CONTRACT}: {len(candidates)} key list lines inside §2.5 "
                         f"(lines {[index + 1 for index, _ in candidates]}); expected one")
    return candidates[0][1]


def parse_ts_fixture(root: Path) -> list[str]:
    """R3：TS 夹具的 SYSMON_METRIC_KEYS 数组字面量。"""
    text = _strip_comments(read_text(root, TS_FIXTURE))
    match = TS_ARRAY_START_RE.search(text)
    if match is None:
        raise CheckError(f"{TS_FIXTURE}: SYSMON_METRIC_KEYS array not found")
    body = _balanced_body(text, text.index("[", match.start()), TS_FIXTURE)
    keys = []
    for single, double in TS_STRING_RE.findall(body):
        keys.append(single or double)
    if not keys:
        raise CheckError(f"{TS_FIXTURE}: SYSMON_METRIC_KEYS array has no string entries")
    return keys


def parse_cpp_fixture(root: Path, rel: str) -> list[str]:
    """R4：测试快照夹具 `availability.metrics` 的键集合（对象键扫描）。

    先按 `{"metrics",` 定位、再括号配平取该对象体，不在文本里按行号或固定缩进猜
    （两份 cpp 与共享头的缩进不同，行号也随重构变化）。
    """
    text = _strip_comments(read_text(root, rel))
    match = FIXTURE_METRICS_START_RE.search(text)
    if match is None:
        raise CheckError(f"{rel}: availability.metrics literal not found")
    start = text.index("{", match.start())
    body = _balanced_body(text, start, rel)
    keys = FIXTURE_OBJECT_KEY_RE.findall(body)
    if not keys:
        raise CheckError(f"{rel}: availability.metrics has no object keys")
    return keys


def fixture_sources(root: Path) -> list[str]:
    """R4 的来源清单由 `--root` 下的实际文件决定。

    共享头 `sysmon_snapshot_fixture.h` 存在 → 它是唯一实现（#309 AC5 的抽取结果），
    两份 test .cpp 只 `#include` 它、不应再有内联 metrics 字面量。
    共享头不存在（抽取前形态）→ 读两份 cpp 的内联副本。两种形态都读，不做第三种猜测。
    """
    if (root / SHARED_TEST_FIXTURE).is_file():
        return [SHARED_TEST_FIXTURE]
    return list(INLINE_TEST_FIXTURES)


def inline_fixture_residue(root: Path) -> list[str]:
    """共享头已是唯一实现时，内联副本不得再长回来（R6 防回归）。

    `check_shared_test_headers.py` 只认 `#include` 关系，拦不住复制一份内联 JSON；
    #295 的 13/15 vs 15/15 正是这么来的。共享头在、cpp 里却还有 metrics 字面量 → 报红。
    共享头不存在时（抽取前形态）两份 cpp 本就是 R4 的来源，不在此列。
    """
    if not (root / SHARED_TEST_FIXTURE).is_file():
        return []
    residue: list[str] = []
    for rel in INLINE_TEST_FIXTURES:
        if not (root / rel).is_file():
            continue
        # 剥注释后再找：注释里留一句「原内联副本已抽取」不该被判成活动的副本。
        if FIXTURE_METRICS_START_RE.search(_strip_comments(read_text(root, rel))):
            residue.append(rel)
    return residue


def parse_builder(root: Path) -> tuple[list[str], bool]:
    """R5：builder 循环体里的显式分支常量（按出现顺序）与是否只有一个未实现兜底。

    返回 (branch_constants, has_not_implemented_fallback)。新增键悄悄落进兜底 =
    显式分支集合缺该键（由上层比对报出）。
    """
    text = _strip_comments(read_text(root, BUILDER))
    match = BUILDER_LOOP_RE.search(text)
    if match is None:
        raise CheckError(f"{BUILDER}: metric key loop not found")
    body = _balanced_body(text, text.index("{", match.start()), BUILDER)
    branches = BUILDER_BRANCH_RE.findall(body)
    fallbacks = BUILDER_FALLBACK_RE.findall(body)
    return branches, len(fallbacks) == 1


def compare(root: Path) -> list[str]:
    """返回问题列表（空 = 一致）。解析错误抛 CheckError（→ exit 2）。"""
    header_keys = parse_header(root)
    contract_keys = parse_contract(root)
    ts_keys = parse_ts_fixture(root)
    fixture_rels = fixture_sources(root)
    cpp_keys = [(rel, parse_cpp_fixture(root, rel)) for rel in fixture_rels]
    builder_branches, builder_fallback = parse_builder(root)

    problems: list[str] = []
    sources = (
        ("contract §2.5", contract_keys),
        (TS_FIXTURE, ts_keys),
        *((f"{rel} availability.metrics", keys) for rel, keys in cpp_keys),
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

    # 共享头已是唯一实现时，两份 cpp 不得再带内联 metrics 副本（#295 finding 5 的复发土壤）。
    for rel in inline_fixture_residue(root):
        problems.append(
            f"{rel}: still carries an inline availability.metrics literal even though "
            f"{SHARED_TEST_FIXTURE} is the shared implementation; include the fixture header "
            "instead of copying its shape")

    # builder 显式分支集合 == 键集合（双向，判据是「逐元素相等」不是「至少覆盖」）
    constants = {name: value for name, value in
                 H_CONST_RE.findall(_strip_comments(read_text(root, H_HEADER)))}
    branch_literals = [constants.get(name) for name in builder_branches]
    undefined = [name for name, literal in zip(builder_branches, branch_literals)
                 if literal is None]
    if undefined:
        problems.append(
            f"{BUILDER}: branches on constant(s) not declared in {H_HEADER}: {undefined}")
    resolved = [literal for literal in branch_literals if literal is not None]
    uncovered = [key for key in header_keys if key not in resolved]
    if uncovered:
        problems.append(
            f"{BUILDER}: explicit metric branches miss keys {uncovered}"
            f" (they would silently fall into the not_implemented fallback)")
    unexpected = sorted({key for key in resolved if key not in header_keys})
    if unexpected:
        problems.append(
            f"{BUILDER}: explicit metric branches on key(s) not in {H_HEADER} "
            f"SYSMON_METRIC_KEYS: {unexpected}")
    duplicated = sorted({key for key in resolved if resolved.count(key) > 1})
    if duplicated:
        problems.append(
            f"{BUILDER}: explicit metric branch repeated for key(s) {duplicated} (dead branch)")
    if not builder_fallback:
        problems.append(
            f"{BUILDER}: expected exactly one not-implemented fallback branch "
            f"(mark_not_implemented) at the end of the loop, found none or several")
    return problems


def selftest() -> int:
    """在临时目录构造 mini 源树，锁住逐条规则与失败形态；不联网、不碰工作区。

    每条用例断言**精确的返回码**（不是「非 0」）：只判非 0 时，把「应当 exit 1 的数据
    漂移」误报成 exit 2 的退化实现也能自测变绿。注释类用例（`comment_out()`）锁的是
    B1：注释掉的登记不得算数——两种来源各自要红，退化成不剥注释就会立刻失败。
    """
    with tempfile.TemporaryDirectory(prefix="dz_metric_keys_selftest_") as tmp:
        root = Path(tmp)

        def layout(root: Path) -> None:
            for rel in (H_HEADER, CONTRACT, TS_FIXTURE, SHARED_TEST_FIXTURE, BUILDER,
                        *INLINE_TEST_FIXTURES):
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

        def include_only_cpp() -> str:
            # 抽取后的形态：只 include 共享头，不再有内联 metrics 字面量。
            return '#include "sysmon_snapshot_fixture.h"\n\nnlohmann::json f();\n'

        def builder(keys: list[str], drop: str | None = None, fallback: bool = True,
                    duplicate: str | None = None, ghost: bool = False) -> str:
            rendered = [k for k in keys if k != drop]
            branches = "\n".join(
                f'        if (metric == platform::SYSMON_METRIC_{_const_name(k)}) {{\n'
                f'            mark_available(a, key);\n'
                f'        }} else'
                for k in rendered)
            if duplicate is not None:
                # 同一个键被分支两次：第二次是死分支，必须报出来（判据是集合相等）。
                branches += ("\n"
                             f'        if (metric == platform::SYSMON_METRIC_{_const_name(duplicate)}) {{\n'
                             '            mark_available(a, key);\n'
                             '        } else')
            if ghost:
                # 分支引用一个仍未登记进 SYSMON_METRIC_KEYS 的常量（未定义的键）。
                branches += ("\n"
                             '        if (metric == platform::SYSMON_METRIC_GHOST) {\n'
                             '            mark_available(a, key);\n'
                             '        } else')
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
            if over.get("inline_only"):
                # 抽取前形态：没有共享头，两份 cpp 各带一份内联 metrics。
                (root / SHARED_TEST_FIXTURE).unlink(missing_ok=True)
                for rel in INLINE_TEST_FIXTURES:
                    (root / rel).write_text(
                        cpp(over.get("cpp_keys", keys)), encoding="utf-8")
            else:
                (root / SHARED_TEST_FIXTURE).write_text(
                    cpp(over.get("cpp_keys", keys)), encoding="utf-8")
                for rel in INLINE_TEST_FIXTURES:
                    (root / rel).write_text(include_only_cpp(), encoding="utf-8")
            (root / BUILDER).write_text(
                builder(keys, over.get("builder_drop"), over.get("fallback", True),
                        over.get("builder_dup"), over.get("builder_ghost")),
                encoding="utf-8")

        def comment_out(case_root: Path, which: str, key: str) -> bool:
            """把某个来源里该键的登记/分支**注释掉**（B1 的判据：注释不算登记）。

            只注释局部而不是整行，保证括号仍配平、其余内容仍可解析——这样锁定的是
            「被注释的内容不得算数」，而不是「文件变得不可解析」这种更弱的现象。
            """
            name = _const_name(key)
            if which == "header constant":
                rel, anchor, replacement = (
                    H_HEADER,
                    f'inline constexpr const char* SYSMON_METRIC_{name} = ',
                    f'// inline constexpr const char* SYSMON_METRIC_{name} = ')
            elif which == "ts entry":
                rel, anchor, replacement = (TS_FIXTURE, f"  '{key}',", f"  // '{key}',")
            elif which == "cpp fixture member":
                rel, anchor = SHARED_TEST_FIXTURE, f'{{"{key}", {{"available", true}}}},'
                replacement = f"/*{anchor}*/"
            elif which == "builder branch":
                rel, anchor = BUILDER, f"metric == platform::SYSMON_METRIC_{name}"
                replacement = f"/*{anchor}*/"
            else:  # pragma: no cover - 用例表里的值受本函数约束
                raise AssertionError(f"unknown comment form: {which}")
            path = case_root / rel
            text = path.read_text(encoding="utf-8")
            if anchor not in text:
                return False
            path.write_text(text.replace(anchor, replacement, 1), encoding="utf-8")
            return True

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
            ("builder duplicate branch", base, {"builder_dup": "swap"}, 1),
            ("builder ghost key branch", base, {"builder_ghost": True}, 1),
            # 抽取前形态（共享头不存在、两份内联副本）必须照样被校验。
            ("inline fixtures agree", base, {"inline_only": True}, 0),
            ("inline fixture misses a key", base, {"inline_only": True,
                                                   "cpp_keys": base[:-1]}, 1),
            # 抽取后形态下，cpp 里再冒出内联 metrics 副本 → 红（#295 finding 5 的复发形态）。
            ("inline copy reappears", base, {"inline_dup": True}, 1),
            # B1：注释掉的登记不得被当成仍然存在（四种来源各一例；修了必须锁住）。
            ("header constant commented out", base,
             {"comment": ("header constant", "swap")}, 2),
            ("ts entry commented out", base, {"comment": ("ts entry", "swap")}, 1),
            ("cpp fixture member commented out", base,
             {"comment": ("cpp fixture member", "swap")}, 1),
            ("builder branch commented out", base,
             {"comment": ("builder branch", "swap")}, 1),
        ]
        failures: list[str] = []
        for name, keys, over, want in cases:
            # 每个用例都在全新子目录里构造，避免上一用例残留。
            case_root = root / re.sub(r"[^a-z0-9]+", "_", name.lower())
            case_root.mkdir(parents=True, exist_ok=True)
            layout(case_root)
            write(case_root, keys, **over)
            if over.get("inline_dup"):
                # 先写共享头形态，再往一份 cpp 里塞回内联副本。
                (case_root / INLINE_TEST_FIXTURES[0]).write_text(
                    cpp(keys), encoding="utf-8")
            comment = over.get("comment")
            if comment is not None:
                if not comment_out(case_root, *comment):
                    failures.append(f"{name}: comment anchor not found for {comment}")
                    continue
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
          "declared size / builder branch / fallback / parse failure / inline-fixture "
          "fallback / inline-copy regression / commented-out registrations)")
    return 0


def _const_name(key: str) -> str:
    """键 → sysmon.h 常量名后缀（`cpu.total` → `CPU_TOTAL`）。与 sysmon.h 的命名一一对应。"""
    return key.upper().replace(".", "_")


def repo_root() -> Path:
    """仓库根：优先 git（worktree / 子目录调用都正确），退化到脚本自身位置。"""
    try:
        result = subprocess.run(["git", "rev-parse", "--show-toplevel"],
                                capture_output=True, text=True, check=True)
        candidate = Path(result.stdout.strip())
        if candidate.is_dir():
            return candidate
    except (OSError, subprocess.CalledProcessError):
        pass
    return REPO_ROOT


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="SYSMON metric key set checker (#309 AC3)")
    parser.add_argument("--root", type=Path, default=None,
                        help="repository root to scan (default: git top-level)")
    parser.add_argument("--selftest", action="store_true",
                        help="run the checker against a temp fixture tree (worktree untouched)")
    args = parser.parse_args(sys.argv[1:] if argv is None else argv)

    if args.selftest:
        return selftest()

    root = (args.root or repo_root()).resolve()
    if not root.is_dir():
        print(f"error: --root is not a directory: {root}", file=sys.stderr)
        return 2
    try:
        problems = compare(root)
    except CheckError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    if problems:
        for problem in problems:
            print(f"error: {problem}", file=sys.stderr)
        print(f"\nFAIL: {len(problems)} sysmon metric key issue(s); the key set must stay "
              "element-wise identical across every copy (contract sysmon §2.5 is the semantic "
              "source of truth)", file=sys.stderr)
        return 1
    header_keys = parse_header(root)
    fixture_rels = fixture_sources(root)
    print(f"OK: {len(header_keys)} SYSMON metric keys consistent across "
          f"sysmon.h / contract §2.5 / ts fixture / {len(fixture_rels)} test fixture "
          f"source(s) ({', '.join(fixture_rels)}); snapshot_builder has an explicit branch for "
          "every key and the fallback is not-implemented only")
    return 0


if __name__ == "__main__":
    sys.exit(main())
