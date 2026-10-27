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
import subprocess
import sys
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

# 已知违例形态：每个用例只破坏一条规则，确认校验器不是恒真（审查清单 §2.2）。
# 用字符串替换而不是造整套假仓库，保证自测廉价、且锁定的是真实文件形状。
SELFTEST_CASES: tuple[tuple[str, str, Path, str], ...] = (
    # (用例名, 期望命中的规则, 目标文件, 查找串 -> 替换串)
    ("conan_version_drift", "R2", Path(VENDORED_JSON_REL),
     "#define NLOHMANN_JSON_VERSION_MINOR 12", "#define NLOHMANN_JSON_VERSION_MINOR 11"),
    ("banner_version_drift", "R3", Path(VENDORED_JSON_REL),
     "version 3.12.0", "version 3.11.3"),
    ("doc_version_drift", "R4", Path(VENDORED_DOC_REL), "| 版本 | **3.12.0** |",
     "| 版本 | **3.11.3** |"),
    ("doc_sha_drift", "R5", Path(VENDORED_DOC_REL), "`aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63`",
     "`" + "0" * 64 + "`"),
)


def run_selftest(root: Path) -> int:
    baseline = check(root)
    if baseline:
        print("selftest: baseline repo already violates the check, cannot self-test",
              file=sys.stderr)
        for v in baseline:
            print("  " + v.render(), file=sys.stderr)
        return 1

    failures = 0
    for name, expected_rule, rel, old, new in SELFTEST_CASES:
        path = root / rel
        original = path.read_text(encoding="utf-8", errors="replace")
        if old not in original:
            print(f"selftest FAIL {name}: anchor not found in {rel}: {old!r}", file=sys.stderr)
            failures += 1
            continue
        try:
            path.write_text(original.replace(old, new, 1), encoding="utf-8")
            rules = {v.rule for v in check(root)}
        finally:
            path.write_text(original, encoding="utf-8")
        if expected_rule not in rules:
            print(f"selftest FAIL {name}: expected {expected_rule}, got {sorted(rules) or 'none'}",
                  file=sys.stderr)
            failures += 1
        else:
            print(f"selftest ok {name} -> {expected_rule}")

    if failures:
        print(f"selftest FAILED: {failures} case(s)", file=sys.stderr)
        return 1
    print(f"selftest: {len(SELFTEST_CASES)} known-bad forms all detected")
    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="validate vendored nlohmann/json version consistency")
    parser.add_argument("--selftest", action="store_true",
                        help="inject known-bad forms and confirm the checker detects them")
    args = parser.parse_args(argv)

    root = Path(subprocess.run(["git", "rev-parse", "--show-toplevel"],
                               capture_output=True, text=True, check=True).stdout.strip())

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
