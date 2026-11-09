#!/usr/bin/env python3
"""ci_scope.py — PR 改动面分类器：CI 该走快路径还是全量（#289 AC3）

为什么需要
----------
#284（9 文件 +807/−30，0 C++ / 0 前端）实测 Windows 12m33s + Linux 18m40s，
其中 wait-ci 占会话墙钟 20.3%；#272（只改 1 个测试 cpp）CI 等待 898s = 38.8%。
这类票改的是脚本、文档、githooks，**没有可被 C++/前端构建验证的面**，却要付
全量构建的固定成本。ci.yml 原有的 paths-ignore 只放行 docs/** 与 *.md，
scripts/** 与 .githooks/** 落在外面，所以纯脚本票仍然跑满全量。

本脚本把「这次改动的验证面有多大」做成一个**确定性分类器**，由 CI 的 scope job
调用一次，输出 fast / full；fast 时跳过构建与测试，只保留 preflight（脚本/文档
的真正验证面）。分类必须确定性：同一文件集合永远给同一结论，否则「CI 绿」
这个信号会随分类抖动而失真。

规则
----
denylist（任意命中 → full）—— 这些文件只有真实构建/测试才能验证：
  - 后缀 .c/.cc/.cpp/.cxx/.h/.hh/.hpp/.hxx（C/C++ 源与头）
  - CMakeLists.txt / CMakePresets.json / conanfile.*（构建系统与依赖）
  - profiles/**（conan profile）、apps/webui/frontend/**（前端）
  - .github/workflows/**（CI 自身）
  - scripts/build* / scripts/test* / scripts/setup*（构建/测试入口与构建环境：
    preflight 不覆盖它们的真实行为）、scripts/install-artifacts.sh /
    scripts/ccache-env.sh / scripts/build-lock.sh

fast 允许面（denylist 之外才成立）：
  - docs/**、任意 *.md
  - scripts/**（denylist 之外）、.githooks/**

其余路径（未知）一律按 full —— 保守优先：只有**明确**属于 fast 面才走快路径，
新增目录不会被默认信任。空 diff（两端相同）→ fast。

注意：本脚本自身在 scripts/** 下 → fast。这是有意的：分类器及其 unittest 由
linux job 的 preflight 步（保留运行）验证，不需要跑构建。

用法:
  python3 scripts/ci_scope.py --range "$BASE...$HEAD"    # CI：PR 的三点 diff
  python3 scripts/ci_scope.py --range 9467679^1..9467679
  python3 scripts/ci_scope.py --files-from changed.txt   # 每行一个路径（自测/离线复算）
  python3 scripts/ci_scope.py --selftest

stdout 单行 `mode=fast` / `mode=full`；stderr 一行人类可读理由。

退出码: 0=分类成功, 2=参数/环境错误（range 不可解析、文件缺失等）
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

# denylist 的第一类：C/C++ 源与头文件后缀。与 `scripts/ci_budget.py` 的
# CPP_SUFFIXES/HEADER_SUFFIXES 是同一份事实的两处消费方（那边数规模、这边判 denylist）；
# 两个脚本都是独立单文件检查器，有意各留一份，改后缀集合时两处一起改。
CPP_SUFFIXES = frozenset({".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx"})

# denylist 的第二类：精确文件名（小写比较，.CPP / CMakeLists.TXT 之类也按 full 保守处理）。
DENY_EXACT_NAMES = frozenset({"cmakelists.txt", "cmakepresets.json"})

# denylist 的第三类：路径前缀。顺序无优先级含义，命中任一条即 full。
DENY_PATH_PREFIXES = (
    "profiles/",
    "apps/webui/frontend/",
    ".github/workflows/",
)

# denylist 的第四类：构建/测试入口脚本与构建环境。**按文件名前缀匹配**（`scripts/build*`
# 是 AC 原文的 glob 语义）：build.sh / build.ps1 / build-lock.sh / build-lock.ps1 / test.* /
# setup.* 全部命中。只匹配 scripts/ 的**直接子文件**，不匹配 scripts/tests/** 这类子目录
# ——后者是检查器与 unittest，属 preflight 的验证面（fast）。
DENY_SCRIPT_PREFIXES = ("build", "test", "setup")
# 精确项里包含 AC 原文列出的三个脚本（前缀规则下它们已是冗余，保留是为了让清单可逐条
# 核对），外加两个 AC 原文没点名、但由构建链执行/被 setup·build 脚本 dot-source 的构建
# 环境文件——它们同样只有真实构建才能验证，判 fast 会静默跳过验证面，故按保守方向纳入：
#   scripts/Activate-MSVC.ps1     Office/MSVC 环境激活（build.ps1 / setup.ps1 调用）
#   scripts/npm-ci-if-needed.cmake 前端依赖安装门控（apps/webui/CMakeLists.txt 执行）
DENY_SCRIPT_EXACT = frozenset({
    "scripts/install-artifacts.sh",
    "scripts/ccache-env.sh",
    "scripts/build-lock.sh",
    "scripts/activate-msvc.ps1",
    "scripts/npm-ci-if-needed.cmake",
})

# fast 允许面：只有这些前缀下的文件（且未命中 denylist）才允许 fast。
FAST_PATH_PREFIXES = ("docs/", "scripts/", ".githooks/")
FAST_MARKDOWN_SUFFIX = ".md"


def normalize(path: str) -> str:
    """把 --files-from 的一行 / git 输出规范成 posix 相对路径。"""
    p = path.strip().replace("\\", "/")
    while p.startswith("./"):
        p = p[2:]
    return p


def deny_rule(path: str) -> str | None:
    """返回命中 denylist 的规则描述；未命中返回 None。"""
    lowered = path.lower()
    name = lowered.rsplit("/", 1)[-1]
    suffix = Path(lowered).suffix

    if suffix in CPP_SUFFIXES:
        return f"C/C++ source suffix {suffix}"
    if name in DENY_EXACT_NAMES:
        return "build system file"
    if name.startswith("conanfile."):
        return "conan recipe"
    for prefix in DENY_PATH_PREFIXES:
        if lowered.startswith(prefix):
            return f"path under {prefix}"
    if lowered in DENY_SCRIPT_EXACT:
        return "build/test entrypoint script"
    if lowered.startswith("scripts/") and "/" not in lowered[len("scripts/"):]:
        # 前缀匹配（不是「按第一个 `.` 截 stem 再精确比较」）：AC 原文是 `scripts/build*`
        # 这类 glob，`build-lock.ps1` / `setup.ps1` 都必须命中（#289 审查 conformance 1）。
        for prefix in DENY_SCRIPT_PREFIXES:
            if name.startswith(prefix):
                return f"script matching scripts/{prefix}*"
    return None


def fast_face(path: str) -> str | None:
    """返回所属 fast 面的描述；不在任何 fast 面上返回 None。"""
    lowered = path.lower()
    if lowered.endswith(FAST_MARKDOWN_SUFFIX):
        return "markdown"
    for prefix in FAST_PATH_PREFIXES:
        if lowered.startswith(prefix):
            return f"path under {prefix}"
    return None


def classify(paths) -> tuple[str, str]:
    """对改动文件列表分类，返回 (mode, reason)。mode 为 'fast' 或 'full'。"""
    files = [p for p in (normalize(raw) for raw in paths) if p]
    if not files:
        return "fast", "no changed paths"

    # denylist 优先于 fast 面：例如 apps/webui/frontend/README.md 是 *.md，但仍须 full。
    for path in files:
        rule = deny_rule(path)
        if rule is not None:
            return "full", f"denylist hit: {path} ({rule})"

    for path in files:
        if fast_face(path) is None:
            return "full", f"unknown path not in fast face: {path}"

    return "fast", f"{len(files)} changed file(s), all within fast face"


def files_from_range(rev_range: str, cwd: str | None = None) -> list[str]:
    """用 `git diff --name-only -z <range>` 取改动文件（range 支持两点/三点形态）。

    `cwd` 供调用方在**别的仓库根**下复算（`scripts/gate_status.py` 在临时 git 仓库里
    自测时用它；PR 分类走默认 = 当前工作目录）。同一份「改动文件列表」只在这里定义，
    不要在调用方再写一遍 git diff 解析。
    """
    try:
        proc = subprocess.run(
            ["git", "diff", "--name-only", "-z", rev_range],
            capture_output=True,
            cwd=cwd,
        )
    except OSError as exc:
        raise RuntimeError(f"cannot run git: {exc}") from exc
    if proc.returncode != 0:
        detail = proc.stderr.decode("utf-8", "replace").strip()
        raise RuntimeError(f"git diff failed for range {rev_range!r}: {detail or 'unknown error'}")
    return [entry for entry in proc.stdout.decode("utf-8", "replace").split("\0") if entry]


def files_from_file(path: str) -> list[str]:
    """读取「每行一个路径」的清单文件（供自测与离线复算）。"""
    try:
        text = Path(path).read_text(encoding="utf-8")
    except OSError as exc:
        raise RuntimeError(f"cannot read files-from list {path!r}: {exc}") from exc
    return [line for line in (normalize(raw) for raw in text.splitlines()) if line]


# ---------------------------------------------------------------------------
# 用例表：分类规则本身也会写错，且本文件是 denylist 的**单一定义**，写错会让
# 「CI 绿」失真（fast 误判 = 绕过构建验证）。每个用例覆盖一条规则或一条边界。
# ---------------------------------------------------------------------------
SELFTEST_CASES: tuple[tuple[tuple[str, ...], str, str], ...] = (
    # AC3 明确要求的十类
    ((), "fast", "空 diff"),
    (("docs/frame_contracts/general.md", "docs/README.md"), "fast", "docs-only"),
    (("scripts/ci_scope.py", "scripts/tests/test_ci_scope.py"), "fast", "scripts-only（denylist 之外）"),
    (("scripts/build.sh",), "full", "构建入口脚本"),
    (("libs/core/src/foo.cpp",), "full", "1 个 .cpp"),
    (("docs/a.md", "scripts/preflight.sh", ".githooks/pre-push"), "fast", "docs+scripts+githooks 混合 fast 面"),
    (("docs/a.md", "scripts/build.sh", "libs/x.h"), "full", "fast 面 + ;cpp/构建脚本"),
    ((".github/workflows/ci.yml",), "full", "CI 自身"),
    ((".githooks/pre-push",), "fast", "githooks"),
    (("apps/webui/frontend/src/x.ts",), "full", "前端"),
    # denylist 其余项逐条
    (("CMakeLists.txt",), "full", "CMake 清单"),
    (("CMakePresets.json",), "full", "CMake preset"),
    (("libs/core/include/dztrader/core/foo.h",), "full", "头文件"),
    (("conanfile.py",), "full", "conan 配方"),
    (("conanfile.txt",), "full", "conan 清单"),
    (("profiles/linux-gcc",), "full", "conan profile"),
    (("scripts/test.sh",), "full", "测试入口脚本"),
    (("scripts/setup.ps1",), "full", "setup 入口脚本"),
    (("scripts/install-artifacts.sh",), "full", "发版入口脚本"),
    (("scripts/ccache-env.sh",), "full", "ccache 环境脚本"),
    (("scripts/build-lock.sh",), "full", "构建锁脚本"),
    # 前缀语义（AC 原文 `scripts/build*` / `scripts/test*` / `scripts/setup*` 是 glob）：
    # 同族但扩展名/后缀不同的脚本必须同样命中，不能只看「第一个 `.` 之前的 stem」。
    (("scripts/build-lock.ps1",), "full", "构建锁 PowerShell 版（build* 前缀）"),
    (("scripts/Activate-MSVC.ps1",), "full", "MSVC 环境激活脚本（构建环境）"),
    (("scripts/npm-ci-if-needed.cmake",), "full", "前端依赖安装门控（构建期执行）"),
    # fast 面与优先级
    (("README.md", "AGENTS.md", "GLOSSARY.md"), "fast", "根 markdown"),
    (("scripts/check_docs_links.py", "scripts/tests/test_ci_budget.py"), "fast", "scripts 下检查器与 unittest"),
    (("apps/webui/frontend/README.md",), "full", "denylist 优先于 *.md"),
    # 未知路径保守按 full
    (("unknown/dir/file.txt",), "full", "未知路径"),
    (("libs/core/src/foo.txt",), "full", "未知扩展名"),
)


def run_selftest() -> int:
    failures = 0
    for files, expected, label in SELFTEST_CASES:
        got, reason = classify(files)
        if got != expected:
            print(
                f"selftest FAIL [{label}] {list(files)}: expected {expected}, got {got} ({reason})",
                file=sys.stderr,
            )
            failures += 1
    if failures:
        print(f"selftest FAILED: {failures}/{len(SELFTEST_CASES)} case(s)", file=sys.stderr)
        return 1
    print(f"selftest: {len(SELFTEST_CASES)} cases classify as expected")
    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="classify a PR diff as fast or full CI scope"
    )
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument(
        "--range",
        dest="rev_range",
        help="git revision range, e.g. origin/main...HEAD or <a>..<b>",
    )
    group.add_argument(
        "--files-from",
        dest="files_from",
        help="file with one changed path per line (selftest / offline replay)",
    )
    group.add_argument(
        "--selftest",
        action="store_true",
        help="run the built-in classification selftest",
    )
    args = parser.parse_args(argv)

    if args.selftest:
        return run_selftest()

    try:
        if args.rev_range is not None:
            files = files_from_range(args.rev_range)
        else:
            files = files_from_file(args.files_from)
    except RuntimeError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    mode, reason = classify(files)
    print(f"mode={mode}")
    print(f"ci-scope: {reason}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
