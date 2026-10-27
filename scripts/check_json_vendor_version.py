#!/usr/bin/env python3
"""check_json_vendor_version.py — 内置 nlohmann/json 与 Conan 固定版本的单一真相校验

为什么需要：ADR 0026 决策 1 要求「SDK 内置单头版本与仓库 Conan 固定版本一致」，
决策 6 要求「内置单头与 Conan 版本在同一次提交升级」。这条纪律原本只写在文档里——
升级时改了 `conanfile.py` 却忘了替换内置单头（或反过来），编译与测试全绿，只有读到
版本号的策略作者才会发现两边不一致（且症状是策略侧行为差异，极难定位）。

本脚本把两处版本＋vendored 记录的实际来源对齐校验：

  R1 `conanfile.py` 的 `self.requires("nlohmann_json/<version>")` 存在且可解析
  R2 内置单头的 `NLOHMANN_JSON_VERSION_MAJOR/MINOR/PATCH` 宏与 R1 一致
  R3 内置单头头注释里的 `version <x.y.z>` 与 R1 一致
  R4 `third_party/nlohmann/VENDORED.md` 记录表的「版本」行与 R1 一致
  R5 `VENDORED.md` 的 SHA-256 与实际内置单头字节一致（升级后忘记更新记录即红）

刻意不校验的东西：
  - 不联网核对上游发布物的哈希（CI 需要确定性、不依赖网络；哈希只用于「记录与实际
    文件同步」，上游真伪由升级时的取用步骤保证，见 VENDORED.md 升级纪律）

用法:
  python scripts/check_json_vendor_version.py            # 仓库根自动定位
  python scripts/check_json_vendor_version.py --selftest # 校验器自测（注入已知违例形态）

退出码: 0=一致, 1=有不一致
"""

from __future__ import annotations

import argparse
import hashlib
import re
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

CONANFILE_REL = "conanfile.py"
VENDORED_JSON_REL = "libs/strategy_api/include/dztrader/third_party/nlohmann/json.hpp"
VENDORED_DOC_REL = "libs/strategy_api/include/dztrader/third_party/nlohmann/VENDORED.md"

CONAN_REQUIRE_RE = re.compile(r'requires\(\s*["\']nlohmann_json/([0-9]+\.[0-9]+\.[0-9]+)["\']')
HEADER_VERSION_RE = re.compile(r"^\s*//.*?\bversion\s+([0-9]+\.[0-9]+\.[0-9]+)\s*$", re.MULTILINE)
MACRO_RES = {
    part: re.compile(rf"^#define\s+NLOHMANN_JSON_VERSION_{part}\s+([0-9]+)\b", re.MULTILINE)
    for part in ("MAJOR", "MINOR", "PATCH")
}
DOC_VERSION_RE = re.compile(r"^\|\s*版本\s*\|\s*\*\*([0-9]+\.[0-9]+\.[0-9]+)\*\*\s*\|", re.MULTILINE)
DOC_SHA_RE = re.compile(r"^\|\s*SHA-256\s*\|\s*`([0-9a-f]{64})`\s*\|", re.MULTILINE)


@dataclass
class Violation:
    rule: str
    message: str

    def render(self) -> str:
        return f"JSON-VENDOR {self.rule}: {self.message}"


def conan_version(root: Path) -> tuple[str | None, list[Violation]]:
    path = root / CONANFILE_REL
    if not path.is_file():
        return None, [Violation("R1", f"missing {CONANFILE_REL}")]
    match = CONAN_REQUIRE_RE.search(path.read_text(encoding="utf-8", errors="replace"))
    if not match:
        return None, [
            Violation("R1", f"no nlohmann_json requirement in {CONANFILE_REL} (expected "
                            "'self.requires(\"nlohmann_json/<x.y.z>\")')")
        ]
    return match.group(1), []


def check(root: Path) -> list[Violation]:
    version, violations = conan_version(root)
    if version is None:
        return violations

    header_path = root / VENDORED_JSON_REL
    if not header_path.is_file():
        return violations + [Violation("R2", f"missing vendored header {VENDORED_JSON_REL}")]
    header_bytes = header_path.read_bytes()
    header_text = header_bytes.decode("utf-8", errors="replace")

    # R2: 版本宏
    parts: list[str] = []
    for part, macro_re in MACRO_RES.items():
        match = macro_re.search(header_text)
        if not match:
            violations.append(
                Violation("R2", f"vendored header has no NLOHMANN_JSON_VERSION_{part} macro")
            )
            break
        parts.append(match.group(1))
    else:
        vendored_version = ".".join(parts)
        if vendored_version != version:
            violations.append(
                Violation("R2", f"version drift: conanfile.py has nlohmann_json/{version}, "
                                f"vendored header macros say {vendored_version}; upgrade both "
                                "in the same commit (ADR 0026 decision 6)")
            )

    # R3: 头注释里的版本（上游生成物两处都有，不同步说明文件被手工改过）
    banner = HEADER_VERSION_RE.search(header_text)
    if not banner:
        violations.append(Violation("R3", "vendored header banner has no version line"))
    elif banner.group(1) != version:
        violations.append(
            Violation("R3", f"vendored header banner says version {banner.group(1)}, "
                            f"conanfile.py has {version}")
        )

    doc_path = root / VENDORED_DOC_REL
    if not doc_path.is_file():
        return violations + [Violation("R4", f"missing {VENDORED_DOC_REL}")]
    doc_text = doc_path.read_text(encoding="utf-8", errors="replace")

    # R4: 记录表版本
    doc_version = DOC_VERSION_RE.search(doc_text)
    if not doc_version:
        violations.append(Violation("R4", f"{VENDORED_DOC_REL} has no '| 版本 |' row"))
    elif doc_version.group(1) != version:
        violations.append(
            Violation("R4", f"{VENDORED_DOC_REL} records version {doc_version.group(1)}, "
                            f"conanfile.py has {version}")
        )

    # R5: 记录表哈希 vs 实际文件
    doc_sha = DOC_SHA_RE.search(doc_text)
    actual_sha = hashlib.sha256(header_bytes).hexdigest()
    if not doc_sha:
        violations.append(Violation("R5", f"{VENDORED_DOC_REL} has no '| SHA-256 |' row"))
    elif doc_sha.group(1) != actual_sha:
        violations.append(
            Violation("R5", f"{VENDORED_DOC_REL} records sha256 {doc_sha.group(1)}, "
                            f"actual header sha256 is {actual_sha}")
        )

    return violations


# ---- 自测 ----------------------------------------------------------------

# 已知违例形态的注入字面量**从工作区当前内容推导**（build_selftest_cases），不硬编码
# 版本号：硬编码会让"正确升级"本身变红——把 conanfile + 单头 + VENDORED.md 三方一致
# 升到新版本后 check() 干净，但硬编码的旧版本字面量会全部 "anchor not found"，preflight
# 在升级路径上失败且报错信息看起来像文件被改坏了（实测 3.12.0→3.13.0 复现）。
# 推导后升级不需要再改本脚本，"版本号不在其它源硬编码"才真正成立。


def _bumped_patch(version: str) -> str:
    """patch 位 +1 的版本串，仅用于构造自测的"已知错误"字面量。"""
    major, minor, patch = (int(part) for part in version.split("."))
    return f"{major}.{minor}.{patch + 1}"


def build_selftest_cases(root: Path) -> list[tuple[str, frozenset[str], Path, str, str]]:
    """推导（用例名, **期望命中的规则集合**, 目标文件, 查找串, 替换串）列表。

    期望值是**精确集合**而不是"包含某条"：用 `in` 断言时，一个塌缩成"永远报 R5"的退化
    校验器仍能自测变绿（审查清单 §2.2 恒真断言）。精确集合把"注入点之外不得有别的规则
    被牵连"也纳入断言，任何多余的规则命中都会让自测变红。

    R2/R3 改的是单头内容，字节变化必然连带 R5——这是**预期**的规则集合，不是噪音：
    R5 的职责就是盯住"单头字节变了但记录没更新"。
    """
    version, _ = conan_version(root)
    if version is None:
        return []
    try:
        conan_text = (root / CONANFILE_REL).read_text(encoding="utf-8", errors="replace")
        header_text = (root / VENDORED_JSON_REL).read_text(encoding="utf-8", errors="replace")
        doc_text = (root / VENDORED_DOC_REL).read_text(encoding="utf-8", errors="replace")
    except OSError:
        return []

    bumped = _bumped_patch(version)
    require_match = CONAN_REQUIRE_RE.search(conan_text)
    patch_match = MACRO_RES["PATCH"].search(header_text)
    banner_match = HEADER_VERSION_RE.search(header_text)
    doc_version_match = DOC_VERSION_RE.search(doc_text)
    doc_sha_match = DOC_SHA_RE.search(doc_text)
    if not all((require_match, patch_match, banner_match, doc_version_match, doc_sha_match)):
        return []

    patch_value = int(patch_match.group(1))
    # 改单头字节 -> R5 必随（字节变了记录没更新）；改 conanfile / VENDORED.md 不影响
    # 单头字节 -> 只应有那一条。R2 只读版本宏、R3 只读 banner，故两者各自 + R5。
    return [
        ("conan_requirement_missing", frozenset({"R1"}), Path(CONANFILE_REL),
         require_match.group(0), 'requires("boost/1.90.0"'),
        ("header_patch_macro_drift", frozenset({"R2", "R5"}), Path(VENDORED_JSON_REL),
         patch_match.group(0),
         f"#define NLOHMANN_JSON_VERSION_PATCH {patch_value + 1}"),
        ("header_banner_drift", frozenset({"R3", "R5"}), Path(VENDORED_JSON_REL),
         f"version {version}", f"version {bumped}"),
        ("doc_version_drift", frozenset({"R4"}), Path(VENDORED_DOC_REL),
         doc_version_match.group(0), f"| 版本 | **{bumped}** |"),
        ("doc_sha_drift", frozenset({"R5"}), Path(VENDORED_DOC_REL),
         doc_sha_match.group(0), "`" + "0" * 64 + "`"),
    ]


def run_selftest(root: Path) -> int:
    """在**临时副本**上注入已知违例形态，确认校验器不是恒真。

    #136 的教训：没有自测，正则被"简化"回去也没人发现。但自测绝不能改工作区的
    受版本控制文件——本脚本最初直接在仓库里改文件再还原，一旦运行被 Ctrl-C /
    SIGKILL / CI 取消打断（preflight 正是 agent 常被杀的地方），就会把内置单头或
    VENDORED.md 留在改坏的状态：`json.hpp` 半改会让后续构建报错，`VENDORED.md`
    被截断会静默丢记录。改为把校验所需的最小文件集复制进临时目录后在副本上折腾，
    原工作区只读。
    """
    baseline = check(root)
    if baseline:
        print("selftest: baseline repo already violates the check, cannot self-test",
              file=sys.stderr)
        for v in baseline:
            print("  " + v.render(), file=sys.stderr)
        return 1

    cases = build_selftest_cases(root)
    if not cases:
        print("selftest: cannot derive anchors from the current files", file=sys.stderr)
        return 1

    failures = 0
    with tempfile.TemporaryDirectory(prefix="dz_json_vendor_selftest_") as tmp:
        fake_root = Path(tmp)
        for rel in (CONANFILE_REL, VENDORED_JSON_REL, VENDORED_DOC_REL):
            dst = fake_root / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(root / rel, dst)
        # 副本必须本身就通过，否则后面的"破坏一条规则"断言没有意义
        if check(fake_root):
            print("selftest: copied baseline already violates the check", file=sys.stderr)
            return 1

        for name, expected_rules, rel, old, new in cases:
            path = fake_root / rel
            original = path.read_text(encoding="utf-8", errors="replace")
            if old not in original:
                print(f"selftest FAIL {name}: anchor not found in {rel}: {old!r}",
                      file=sys.stderr)
                failures += 1
                continue
            try:
                path.write_text(original.replace(old, new, 1), encoding="utf-8", newline="\n")
                rules = {v.rule for v in check(fake_root)}
            finally:
                path.write_text(original, encoding="utf-8", newline="\n")
            # 精确集合相等：既要求期望规则全部命中，也要求没有多余规则被牵连。
            # 用 `in` 只能挡住"漏报"，挡不住"塌缩成永远报同一条"的退化校验器。
            if rules != expected_rules:
                print(f"selftest FAIL {name}: expected {sorted(expected_rules)}, "
                      f"got {sorted(rules) or 'none'}", file=sys.stderr)
                failures += 1
            else:
                print(f"selftest ok {name} -> {sorted(expected_rules)}")

    if failures:
        print(f"selftest FAILED: {failures} case(s)", file=sys.stderr)
        return 1
    print(f"selftest: {len(cases)} known-bad forms all detected "
          "(on a temp copy; the worktree is never modified)")
    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="validate vendored nlohmann/json version consistency")
    parser.add_argument("--selftest", action="store_true",
                        help="inject known-bad forms and confirm the checker detects them")
    args = parser.parse_args(argv)

    # 定位仓库根：优先 git（在 worktree / 子目录调用都正确），退化到脚本自身位置
    # （git 不可用或不在工作树内时不要抛 traceback——兄弟检查脚本也是干净退出）。
    root: Path | None = None
    try:
        top = subprocess.run(["git", "rev-parse", "--show-toplevel"],
                             capture_output=True, text=True, check=True)
        candidate = Path(top.stdout.strip())
        if candidate.is_dir():
            root = candidate
    except (OSError, subprocess.CalledProcessError):
        root = None
    if root is None:
        root = Path(__file__).resolve().parents[1]

    if args.selftest:
        return run_selftest(root)

    violations = check(root)
    if violations:
        for v in violations:
            print(v.render())
        print(f"\nFAIL: {len(violations)} JSON vendor inconsistency(ies)", file=sys.stderr)
        return 1
    print("OK: vendored nlohmann/json matches conanfile.py and VENDORED.md", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
