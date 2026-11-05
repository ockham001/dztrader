#!/usr/bin/env python3
"""test_script_args.py — build.sh / test.sh 参数解析自测（#281）

为什么需要：仓库已为「门禁假绿」建了两道机械防线（#190 verdict 固定末尾 + last-status +
pre-push 拦截；#75/#81 的管道吞退出码），但都只覆盖**管道**，不覆盖**参数**。旧实现
`TARGET="${2:?}"` / `TEST_NAME="${2:?}"` 是覆盖式赋值：重复传同一 flag 只有最后一个生效，
且退出码 0。#250 实际发生过——7 个 target 只构建了 1 个，其余 6 个从未编译，`gate: OK`。

本脚本把假 `cmake` / `ctest` / `flock` 注入 PATH（不构建、不取真实构建锁、不联网），
断言两个脚本**真实构造出的命令行**包含全部 flag 值。反向验证：把实现改回「只留最后
一个」，多 target / 多 test-name 用例立刻红。

跑法：python3 scripts/tests/test_script_args.py --selftest
（preflight 的 check_script_args step 调用；位于 scripts/tests/ 与 unittest 套件同目录，
但用 --selftest 显式驱动，因此也被 unittest discover 安全忽略。）

退出码：0=全部用例通过，1=有失败，2=参数/环境错误。
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

# 假可执行文件的内容：把 argv 写进 $DZ_FAKE_DIR/<程序名>.log，然后成功退出。
# flock 也造假：脚本里的 `flock -n 9` 因此立即返回 0，selftest 不会等待真实构建锁
# （preflight 与 build 并行是常态，等锁会让只读门禁被无关构建拖住）。
_FAKE_TOOL = """#!/usr/bin/env sh
kind="$(basename "$0")"
printf '%s\\n' "$@" >> "${DZ_FAKE_DIR:?}/$kind.log"
exit 0
"""

FAKE_TOOLS = ("cmake", "ctest", "flock")


class CaseResult:
    def __init__(self, name: str) -> None:
        self.name = name
        self.failures: list[str] = []

    def check(self, condition: bool, detail: str) -> None:
        if not condition:
            self.failures.append(detail)


def _prepare_fake_tools(tmp: Path) -> Path:
    fake_dir = tmp / "fake-bin"
    fake_dir.mkdir()
    for tool in FAKE_TOOLS:
        path = fake_dir / tool
        path.write_text(_FAKE_TOOL, encoding="utf-8")
        path.chmod(0o755)
    return fake_dir


def _read_argv(fake_dir: Path, tool: str) -> list[str]:
    log = fake_dir / f"{tool}.log"
    if not log.exists():
        return []
    return [line for line in log.read_text(encoding="utf-8").splitlines() if line]


def _run(script: str, args: list[str], fake_dir: Path) -> subprocess.CompletedProcess[str]:
    # 清掉上一用例的命令记录；同一 fake_dir 顺序复用。
    for tool in FAKE_TOOLS:
        (fake_dir / f"{tool}.log").unlink(missing_ok=True)
    env = os.environ.copy()
    env["PATH"] = f"{fake_dir}{os.pathsep}{env.get('PATH', '')}"
    env["DZ_FAKE_DIR"] = str(fake_dir)
    # 指向临时目录：build.sh 的 ~/.npm 可写性探测因此整段跳过（不碰真实 HOME）。
    env["npm_config_cache"] = str(fake_dir / "npm")
    # 假 flock 不取真实锁：禁止 test.sh 清 test_home，否则会抹掉并行真实测试的
    # DZTRADER_HOME（#281 审查 B1）。
    env["DZ_SKIP_TEST_HOME_RESET"] = "1"
    return subprocess.run(
        [str(REPO_ROOT / "scripts" / script), *args],
        capture_output=True,
        text=True,
        env=env,
        timeout=120,
        check=False,
    )


def _subsequence(haystack: list[str], needle: list[str]) -> bool:
    it = iter(haystack)
    return all(item in it for item in needle)


def _usage_failure(result: subprocess.CompletedProcess[str], case: CaseResult, flag: str) -> None:
    """缺参数 / 未知 flag 的共同断言：非 0 退出 + 含用法提示。"""
    case.check(result.returncode != 0, f"expected non-zero rc, got {result.returncode}")
    combined = result.stderr + result.stdout
    case.check("Usage:" in combined, f"expected usage text, got: {combined!r}")
    if flag:
        case.check(
            flag in result.stderr,
            f"expected {flag!r} in stderr, got: {result.stderr!r}",
        )
    case.check(
        "parameter null or not set" not in combined,
        f"bash ${{2:?}} internal error leaked instead of usage: {combined!r}",
    )


# ---- build.sh 用例 --------------------------------------------------------


def case_build_single_target(fake_dir: Path) -> CaseResult:
    case = CaseResult("build: single --target")
    result = _run("build.sh", ["--target", "dzweb"], fake_dir)
    argv = _read_argv(fake_dir, "cmake")
    case.check(result.returncode == 0, f"rc={result.returncode} stderr={result.stderr!r}")
    case.check(
        _subsequence(argv, ["--build", "--preset", "linux-release", "--target", "dzweb"]),
        f"cmake argv missing expected sequence: {argv}",
    )
    case.check("dzweb" in result.stdout, f"echoed command line missing target: {result.stdout!r}")
    return case


def case_build_multi_target(fake_dir: Path) -> CaseResult:
    case = CaseResult("build: repeated --target keeps all targets")
    result = _run(
        "build.sh",
        ["--target", "dzweb", "--target", "dzcore", "--target", "dztd_ctp_td_config_test"],
        fake_dir,
    )
    argv = _read_argv(fake_dir, "cmake")
    case.check(result.returncode == 0, f"rc={result.returncode} stderr={result.stderr!r}")
    case.check(
        argv.count("--target") == 1,
        f"expected exactly one --target flag carrying all values, got {argv}",
    )
    for target in ("dzweb", "dzcore", "dztd_ctp_td_config_test"):
        case.check(target in argv, f"target {target!r} dropped from cmake argv: {argv}")
        case.check(target in result.stdout, f"target {target!r} missing from echoed line")
    case.check(
        _subsequence(argv, ["--target", "dzweb", "dzcore", "dztd_ctp_td_config_test"]),
        f"targets not preserved in order after --target: {argv}",
    )
    return case


def case_build_config_and_target(fake_dir: Path) -> CaseResult:
    case = CaseResult("build: --config debug + --target")
    result = _run("build.sh", ["--config", "debug", "--target", "dzx"], fake_dir)
    argv = _read_argv(fake_dir, "cmake")
    case.check(result.returncode == 0, f"rc={result.returncode} stderr={result.stderr!r}")
    case.check(
        _subsequence(argv, ["--build", "--preset", "linux-debug", "--target", "dzx"]),
        f"unexpected cmake argv: {argv}",
    )
    return case


def case_build_missing_value(fake_dir: Path) -> CaseResult:
    case = CaseResult("build: --target without value -> usage error")
    _usage_failure(_run("build.sh", ["--target"], fake_dir), case, "--target")
    return case


def case_build_missing_config_value(fake_dir: Path) -> CaseResult:
    case = CaseResult("build: --config without value -> usage error")
    _usage_failure(_run("build.sh", ["--config"], fake_dir), case, "--config")
    return case


def case_build_unknown_flag(fake_dir: Path) -> CaseResult:
    case = CaseResult("build: unknown flag -> usage error")
    result = _run("build.sh", ["--bogus"], fake_dir)
    _usage_failure(result, case, "")
    case.check("unknown option" in result.stderr, f"missing unknown-option message: {result.stderr!r}")
    return case


def case_build_short_flags(fake_dir: Path) -> CaseResult:
    case = CaseResult("build: short flags -c/-t")
    result = _run("build.sh", ["-c", "debug", "-t", "dzx"], fake_dir)
    argv = _read_argv(fake_dir, "cmake")
    case.check(result.returncode == 0, f"rc={result.returncode} stderr={result.stderr!r}")
    case.check(
        _subsequence(argv, ["--build", "--preset", "linux-debug", "--target", "dzx"]),
        f"unexpected cmake argv: {argv}",
    )
    return case


def case_build_empty_value(fake_dir: Path) -> CaseResult:
    case = CaseResult("build: empty --target value -> usage error")
    _usage_failure(_run("build.sh", ["--target", ""], fake_dir), case, "--target")
    return case


def case_build_empty_config_value(fake_dir: Path) -> CaseResult:
    case = CaseResult("build: empty --config value -> usage error")
    _usage_failure(_run("build.sh", ["--config", ""], fake_dir), case, "--config")
    return case


# ---- test.sh 用例 ---------------------------------------------------------


def case_test_single_name(fake_dir: Path) -> CaseResult:
    case = CaseResult("test: single --test-name")
    result = _run("test.sh", ["--test-name", "TdConfigTest"], fake_dir)
    argv = _read_argv(fake_dir, "ctest")
    case.check(result.returncode == 0, f"rc={result.returncode} stderr={result.stderr!r}")
    case.check(
        _subsequence(argv, ["--preset", "linux-release", "-R", "TdConfigTest"]),
        f"ctest argv unexpected: {argv}",
    )
    return case


def case_test_multi_name(fake_dir: Path) -> CaseResult:
    case = CaseResult("test: repeated --test-name joined into one -R")
    result = _run(
        "test.sh",
        ["--test-name", "TdConfigTest", "--test-name", "SmokeTest"],
        fake_dir,
    )
    argv = _read_argv(fake_dir, "ctest")
    case.check(result.returncode == 0, f"rc={result.returncode} stderr={result.stderr!r}")
    case.check(argv.count("-R") == 1, f"ctest accepts only one -R, got argv: {argv}")
    case.check(
        _subsequence(argv, ["--preset", "linux-release", "-R", "TdConfigTest|SmokeTest"]),
        f"regexes not joined with '|': {argv}",
    )
    case.check("TdConfigTest|SmokeTest" in result.stdout, "joined regex missing from echoed line")
    return case


def case_test_list(fake_dir: Path) -> CaseResult:
    case = CaseResult("test: --list passes through to ctest -N without lock")
    result = _run("test.sh", ["--list"], fake_dir)
    argv = _read_argv(fake_dir, "ctest")
    case.check(result.returncode == 0, f"rc={result.returncode} stderr={result.stderr!r}")
    case.check(
        _subsequence(argv, ["--preset", "linux-release", "-N"]),
        f"ctest argv missing -N: {argv}",
    )
    # --list 是只读查询，不应取构建锁（否则 preflight 与 build 并行时会白等）。
    case.check(
        not (fake_dir / "flock.log").exists(),
        "--list must not acquire the build lock",
    )
    return case


def case_test_list_with_filter(fake_dir: Path) -> CaseResult:
    case = CaseResult("test: --list + --test-name")
    result = _run("test.sh", ["--list", "--test-name", "TdConfigTest\\."], fake_dir)
    argv = _read_argv(fake_dir, "ctest")
    case.check(result.returncode == 0, f"rc={result.returncode} stderr={result.stderr!r}")
    case.check(
        _subsequence(argv, ["--preset", "linux-release", "-R", "TdConfigTest\\.", "-N"]),
        f"ctest argv unexpected: {argv}",
    )
    return case


def case_test_missing_value(fake_dir: Path) -> CaseResult:
    case = CaseResult("test: --test-name without value -> usage error")
    _usage_failure(_run("test.sh", ["--test-name"], fake_dir), case, "--test-name")
    return case


def case_test_missing_label_value(fake_dir: Path) -> CaseResult:
    case = CaseResult("test: --label without value -> usage error")
    _usage_failure(_run("test.sh", ["--label"], fake_dir), case, "--label")
    return case


def case_test_unknown_flag(fake_dir: Path) -> CaseResult:
    case = CaseResult("test: unknown flag -> usage error")
    result = _run("test.sh", ["--bogus"], fake_dir)
    _usage_failure(result, case, "")
    case.check("unknown option" in result.stderr, f"missing unknown-option message: {result.stderr!r}")
    return case


def case_test_short_flags(fake_dir: Path) -> CaseResult:
    case = CaseResult("test: short flags -L/-t")
    result = _run("test.sh", ["-L", "slow", "-t", "TdConfigTest"], fake_dir)
    argv = _read_argv(fake_dir, "ctest")
    case.check(result.returncode == 0, f"rc={result.returncode} stderr={result.stderr!r}")
    case.check(
        _subsequence(argv, ["--preset", "linux-release-slow", "-R", "TdConfigTest"]),
        f"unexpected ctest argv: {argv}",
    )
    return case


def case_test_empty_value(fake_dir: Path) -> CaseResult:
    case = CaseResult("test: empty --test-name value -> usage error")
    _usage_failure(_run("test.sh", ["--test-name", ""], fake_dir), case, "--test-name")
    return case


CASES = (
    case_build_single_target,
    case_build_multi_target,
    case_build_config_and_target,
    case_build_short_flags,
    case_build_empty_value,
    case_build_empty_config_value,
    case_build_missing_value,
    case_build_missing_config_value,
    case_build_unknown_flag,
    case_test_single_name,
    case_test_short_flags,
    case_test_multi_name,
    case_test_empty_value,
    case_test_list,
    case_test_list_with_filter,
    case_test_missing_value,
    case_test_missing_label_value,
    case_test_unknown_flag,
)


def selftest() -> int:
    tmp = Path(tempfile.mkdtemp(prefix="dz_script_args_"))
    try:
        fake_dir = _prepare_fake_tools(tmp)
        total = 0
        failed = 0
        for case_fn in CASES:
            result = case_fn(fake_dir)
            total += 1
            if result.failures:
                failed += 1
                print(f"FAIL {result.name}", file=sys.stderr)
                for detail in result.failures:
                    print(f"     {detail}", file=sys.stderr)
            else:
                print(f"ok   {result.name}")
        if failed:
            print(
                f"selftest FAILED: {failed}/{total} cases "
                "(build.sh/test.sh 参数解析回归——重复 flag 只能累积，不能覆盖)",
                file=sys.stderr,
            )
            return 1
        print(f"selftest OK: {total} cases (多 target / 多 test-name / 缺参数 / 未知 flag / --list)")
        return 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="build.sh / test.sh 参数解析自测（#281）；只读、不构建"
    )
    parser.add_argument("--selftest", action="store_true", help="跑全部参数解析用例")
    args = parser.parse_args()
    if not args.selftest:
        parser.print_help()
        return 2
    return selftest()


if __name__ == "__main__":
    sys.exit(main())
