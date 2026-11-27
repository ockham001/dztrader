#!/usr/bin/env python3
"""check_build_info.py — 校验 install 树的构建身份（BUILD_INFO）是否对应当前 HEAD（#515）

## 为什么需要

`install/<plat>/<arch>/<cfg>/` 里的二进制与 `web/` 原先**没有任何 commit 标识**，#508 关单
时 handoff 写「启动栈已含新二进制（main 已合并）」，实际二进制 mtime 早于合并、`sha256sum`
与 `build/` 也不同——只能手工推断，结论是宿主 8080 栈跑着旧二进制而前端静态文件已被刷新
（新旧混用，现象难解释）。构建期写出身份（`cmake/write_build_info.cmake`），启动前比对
（本脚本 + `run-dev.sh` / `run-dev.ps1`），把「跑的是哪一版」从推断变成可验证。

## 规则

比对 `BUILD_INFO.commit` 与 `git -C <repo> rev-parse HEAD`：

  - 缺失 / 不可读 / 字段不全 → **warning**：产物没有身份，无法判断新旧
  - commit=unknown（构建时不在 git 仓库里）→ **warning**
  - commit 与 HEAD 不同 → **warning**，打印两侧短 sha、产物的 built_at 与重建提示
  - commit 相同但 `dirty=1` → **note**：工作区在构建时有未提交改动（产物含未提交代码）
  - commit 相同且 dirty=0 → ok

判定**永不阻断启动**（issue #515 AC2 要求「打 warning、不阻断」）：退出码只用于测试与
人工排查，`run-dev` 侧显式容忍非零。`dirty` 用 `git status --porcelain` 口径（含未跟踪、
不含 gitignore），与「工作区是否干净」同义；`build/`、`install/` 已在 `.gitignore` 中。

## 用法

  python3 scripts/check_build_info.py check --info install/linux/x86_64/Release/BUILD_INFO
  python3 scripts/check_build_info.py check            # --info 缺省按本机平台/Release 推
  python3 scripts/check_build_info.py --selftest       # 临时 git 仓库自测，不碰工作区

stdout 一行 `build-info: ok|stale|missing|unknown | ...`，warning / note 走 stderr。

退出码: 0=一致（note 不算问题）, 1=有 warning（不一致/缺失/不可读）, 2=参数或环境错误。
"""

from __future__ import annotations

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

DEFAULT_ROOT = Path(__file__).resolve().parents[1]
REQUIRED_KEYS = ("commit", "dirty", "built_at")
SHORT_SHA_LEN = 12
# 重建提示按平台给：Windows 用户的入口是 .ps1，Linux 是 .sh（与 README 4 命令工作流一致）。
REBUILD_HINT_POSIX = "run ./scripts/build.sh (or ./scripts/run-dev.sh without --no-build)"
REBUILD_HINT_WINDOWS = r"run .\scripts\build.ps1 (or .\scripts\run-dev.ps1 without -NoBuild)"


class EnvError(Exception):
    """参数/环境前提不满足（--selftest 失败也算，见 main）。"""


def default_info_path(root: Path) -> Path:
    """缺省 BUILD_INFO 位置：install/<plat>/<arch>/Release/BUILD_INFO。"""
    if sys.platform.startswith("win"):
        plat = "windows"
    elif sys.platform == "darwin":
        plat = "macos"
    else:
        plat = "linux"
    return root / "install" / plat / "x86_64" / "Release" / "BUILD_INFO"


def parse_build_info(text: str) -> dict[str, str] | None:
    """解析 k=v 纯文本；缺必需字段或格式不可读返回 None。

    写入方是 `cmake/write_build_info.cmake` 的单一 `file(WRITE)`，这里只读解析。
    `#` 开头的注释行与空行跳过；同一个 key 重复时后者覆盖（写入方不会产出，容错而
    不做严格拒绝——目的是给出提示，不是校验器）。
    """
    fields: dict[str, str] = {}
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        key, sep, value = line.partition("=")
        if sep:
            fields[key.strip()] = value.strip()
    if any(not fields.get(key) for key in REQUIRED_KEYS):
        return None
    return fields


def git_head(repo: Path) -> str | None:
    """取仓库 HEAD；不是 git 仓库 / git 不可用时返回 None（调用方按 unknown 处理）。"""
    try:
        result = subprocess.run(
            ["git", "-C", str(repo), "rev-parse", "HEAD"],
            capture_output=True,
            text=True,
            timeout=30,
        )
    except (OSError, subprocess.SubprocessError):
        return None
    if result.returncode != 0:
        return None
    sha = result.stdout.strip()
    return sha or None


def rebuild_hint() -> str:
    return REBUILD_HINT_WINDOWS if sys.platform.startswith("win") else REBUILD_HINT_POSIX


class Report:
    """比对结果：status 决定 stdout 行与退出码，warnings/notes 决定 stderr。"""

    def __init__(self) -> None:
        self.warnings: list[str] = []
        self.notes: list[str] = []
        self.fields: dict[str, str] = {}
        self.head: str | None = None

    @property
    def status(self) -> str:
        if self.warnings:
            if not self.fields:
                return "missing"
            if self.fields.get("commit") in ("", "unknown", None):
                return "unknown"
            return "stale"
        return "ok"

    def warn(self, message: str) -> None:
        self.warnings.append(message)

    def note(self, message: str) -> None:
        self.notes.append(message)

    def summary(self) -> str:
        parts = [f"status={self.status}"]
        for key in ("commit", "dirty", "built_at", "version", "config"):
            value = self.fields.get(key)
            if value:
                parts.append(f"{key}={value}")
        if self.head:
            parts.append(f"head={self.head}")
        return "build-info: " + " ".join(parts)

    def print_stderr(self) -> None:
        for warning in self.warnings:
            print(f"warning: {warning}", file=sys.stderr)
        for note in self.notes:
            print(f"note: {note}", file=sys.stderr)


def compare(info_path: Path, repo: Path) -> Report:
    """读取 BUILD_INFO 并与 repo 的 HEAD 比对，返回带 warning/note 的报告。"""
    report = Report()
    report.head = git_head(repo)
    try:
        text = info_path.read_text(encoding="utf-8", errors="replace")
    except OSError as exc:
        report.warn(
            f"build identity missing (no readable BUILD_INFO at {info_path}): "
            f"{exc.strerror or exc}; the install tree cannot be matched to a commit"
        )
        report.warn(f"rebuild it: {rebuild_hint()}")
        return report

    fields = parse_build_info(text)
    if fields is None:
        report.warn(
            f"build identity unreadable (BUILD_INFO at {info_path} lacks "
            f"{'/'.join(REQUIRED_KEYS)}); regenerate it: {rebuild_hint()}"
        )
        return report
    report.fields = fields

    built_commit = fields["commit"]
    built_at = fields.get("built_at", "unknown")
    if built_commit in ("", "unknown"):
        report.warn(
            f"build identity has no commit (built_at={built_at}); "
            f"the build did not run inside a git repository"
        )
        return report
    if report.head is None:
        report.warn(
            f"cannot resolve HEAD in {repo} (not a git repository?); "
            f"install tree was built from commit={built_commit[:SHORT_SHA_LEN]}"
        )
        return report
    if built_commit != report.head:
        report.warn(
            f"install tree is stale: built from commit={built_commit[:SHORT_SHA_LEN]} "
            f"(built_at={built_at}) but HEAD is {report.head[:SHORT_SHA_LEN]}; "
            f"running the stack now would mix builds"
        )
        report.warn(f"rebuild before launching: {rebuild_hint()}")
        return report
    if fields.get("dirty") == "1":
        report.note(
            f"install tree was built from a dirty worktree (commit={built_commit[:SHORT_SHA_LEN]}, "
            f"built_at={built_at}); it contains uncommitted changes"
        )
    return report


def selftest() -> int:
    checks = 0
    failures: list[str] = []

    def expect(name: str, condition: bool, detail: str = "") -> None:
        nonlocal checks
        checks += 1
        if not condition:
            failures.append(f"{name}: {detail}" if detail else name)

    def git(root: Path, *args: str) -> str:
        result = subprocess.run(
            ["git", "-c", "user.name=build-info-selftest",
             "-c", "user.email=selftest@example.invalid",
             "-c", "commit.gpgsign=false", *args],
            capture_output=True,
            text=True,
            cwd=root,
        )
        if result.returncode != 0:
            raise EnvError(f"git {' '.join(args)} failed: {result.stderr.strip()}")
        return result.stdout.strip()

    def write_info(path: Path, *, commit: str, dirty: str = "0", built_at: str = "2026-01-01T00:00:00Z") -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(
            "# dztrader build identity | generated by the build, do not edit (#515)\n"
            f"commit={commit}\ndirty={dirty}\nbuilt_at={built_at}\n"
            "version=0.1.0\nconfig=Release\n",
            encoding="utf-8",
        )

    with tempfile.TemporaryDirectory(prefix="dz_build_info_") as tmp:
        repo = Path(tmp) / "repo"
        repo.mkdir()
        git(repo, "init", "-q", "-b", "main")
        (repo / "README.md").write_text("# baseline\n", encoding="utf-8")
        git(repo, "add", "README.md")
        git(repo, "commit", "-q", "-m", "chore: baseline")
        head = git(repo, "rev-parse", "HEAD")
        info = Path(tmp) / "install" / "BUILD_INFO"

        # 1) 一致 + dirty=0 → ok，零 warning
        write_info(info, commit=head)
        report = compare(info, repo)
        expect("matching commit is ok", report.status == "ok", report.summary())
        expect("matching commit has no warnings", not report.warnings, str(report.warnings))
        expect("matching commit has no notes", not report.notes, str(report.notes))

        # 2) 旧 commit → stale warning 含两侧短 sha，且给出重建提示
        write_info(info, commit="0" * 40, built_at="2026-01-02T03:04:05Z")
        report = compare(info, repo)
        expect("stale commit is reported", report.status == "stale", report.summary())
        expect(
            "stale warning names both commits",
            any("0" * SHORT_SHA_LEN in w and head[:SHORT_SHA_LEN] in w for w in report.warnings),
            str(report.warnings),
        )
        expect(
            "stale warning carries built_at",
            any("2026-01-02T03:04:05Z" in w for w in report.warnings),
            str(report.warnings),
        )
        expect(
            "stale warning carries a rebuild hint",
            any("rebuild" in w for w in report.warnings),
            str(report.warnings),
        )

        # 3) 文件缺失 → missing
        missing = Path(tmp) / "nope" / "BUILD_INFO"
        report = compare(missing, repo)
        expect("missing file is reported", report.status == "missing", report.summary())
        expect("missing file warns", bool(report.warnings), report.summary())

        # 4) 字段不全（旧格式 / 被截断）→ unreadable
        info.parent.mkdir(parents=True, exist_ok=True)
        info.write_text("version=0.1.0\n", encoding="utf-8")
        report = compare(info, repo)
        expect("unreadable file warns", bool(report.warnings), report.summary())
        expect("unreadable file is not ok", report.status != "ok", report.summary())

        # 5) commit=unknown（构建时不在 git 仓库）→ unknown
        write_info(info, commit="unknown")
        report = compare(info, repo)
        expect("unknown commit is reported", report.status == "unknown", report.summary())
        expect(
            "unknown commit warns about missing git",
            any("no commit" in w for w in report.warnings),
            str(report.warnings),
        )

        # 6) 一致但 dirty=1 → ok + note（不阻断，只提示工作区未提交）
        write_info(info, commit=head, dirty="1")
        report = compare(info, repo)
        expect("dirty build is still ok", report.status == "ok", report.summary())
        expect(
            "dirty build raises a note",
            any("dirty" in n for n in report.notes),
            str(report.notes),
        )
        expect("dirty build raises no warning", not report.warnings, str(report.warnings))

        # 7) 不是 git 仓库 → warning（不崩、不误判为 ok）
        plain = Path(tmp) / "plain"
        plain.mkdir()
        write_info(info, commit=head)
        report = compare(info, plain)
        expect("non-git repo is not ok", report.status != "ok", report.summary())
        expect(
            "non-git repo warns about HEAD",
            any("HEAD" in w for w in report.warnings),
            str(report.warnings),
        )

        # 8) 解析器：注释行与空行跳过、缺 built_at 判不可读
        expect(
            "parse skips comments",
            (parse_build_info("# comment\ncommit=a\ndirty=0\nbuilt_at=t\n") or {}).get("commit") == "a",
        )
        expect(
            "parse rejects missing built_at",
            parse_build_info("commit=a\ndirty=0\n") is None,
        )
        expect("parse rejects empty text", parse_build_info("") is None)

        # 9) 缺省 BUILD_INFO 路径按平台/Release 拼（存在性由调用方判）
        default_path = default_info_path(repo)
        expect(
            "default info path is under install/Release",
            default_path.parent.name == "Release" and default_path.name == "BUILD_INFO",
            str(default_path),
        )

    if failures:
        for failure in failures:
            print(f"selftest FAIL {failure}", file=sys.stderr)
        print(f"selftest FAILED: {len(failures)}/{checks} case(s)", file=sys.stderr)
        return 1
    print(
        f"selftest: {checks} cases (match / stale / missing / unreadable / unknown / "
        "dirty note / non-git / parser / default path)"
    )
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="verify that the install tree's BUILD_INFO matches the current HEAD (#515)"
    )
    parser.add_argument("--selftest", action="store_true", help="temporary git repo self-test")
    sub = parser.add_subparsers(dest="command")
    check_parser = sub.add_parser("check", help="compare BUILD_INFO against the repository HEAD")
    check_parser.add_argument(
        "--info",
        default=None,
        help="path to BUILD_INFO (default: install/<plat>/x86_64/Release/BUILD_INFO)",
    )
    check_parser.add_argument(
        "--repo", default=None, help="repository root for git rev-parse (default: this repo)"
    )
    return parser


def main(argv: list[str]) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    if args.selftest:
        return selftest()
    if args.command != "check":
        parser.print_usage(sys.stderr)
        return 2

    repo = Path(args.repo).resolve() if args.repo else DEFAULT_ROOT
    info = Path(args.info).resolve() if args.info else default_info_path(repo)
    if not repo.is_dir():
        print(f"build-info: error: repo is not a directory: {repo}", file=sys.stderr)
        return 2
    report = compare(info, repo)
    print(report.summary())
    report.print_stderr()
    return 0 if not report.warnings else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
