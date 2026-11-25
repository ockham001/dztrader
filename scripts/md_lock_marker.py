#!/usr/bin/env python3
"""md_lock_marker.py — `apps/ctp/md/` 生产锁定确认标记（#480）

## 为什么需要

AGENTS §一硬门禁 1：`apps/ctp/md/` 下所有文件的改动**必须先向用户说明并取得确认**。
`.opencode/opencode.json` 把这条机器化成了 `edit` 的 `ask`，但**只在 opencode 会话生效**；
DSH 等其它宿主没有这层约束（AGENTS §一第 7 条已声明「规则强度不变」）。

实际后果（#471）：确认发生在**实现全部做完之后**（`ask_user_question` 阻塞 187s），
而且真正发现漏项的是 Standards 审查的 BLOCKING 第 1 条，不是实现者——规则写在文档里，
动作点却看不见。

## 规则

与 `review_marker.py` 同形态：用户确认后写一个标记文件，推送前**机器检查**它存在。

  - 标记文件：`.cache/lock/<issue>-md-lock.done`（`.cache/` 已 gitignore，不污染仓）
  - 内容：被确认时的 commit sha（一行）
  - `write` 在**任何** sha 下都能写（确认可能发生在首个提交前），`check` 只判「有没有」

为什么 `check` 只判存在、不判 sha：确认的对象是「这次要改 md 目录」这件事，而确认往往
发生在写代码之前（此时还没有 commit）。要求 sha 对齐会把「先确认后实现」这个唯一正确
顺序判成失败——与 `review_marker.py`（验证的是**已有 diff**，所以必须判 sha）的语义不同。

## 用法

  python3 scripts/md_lock_marker.py write <issue> [--sha <sha>]   # 用户确认后由 Lead 写
  python3 scripts/md_lock_marker.py check <issue> [--strict]      # 缺失/为空 → warning；--strict 时 exit 3
  python3 scripts/md_lock_marker.py --selftest

`scripts/push-ticket.sh` 在 diff 触及 `apps/ctp/md/` 时以 `--strict` 调用本脚本，
缺失即拒绝推送；逃生口 `DZ_ALLOW_MD_LOCK=1`（仅真故障，用完在报告里写明）。

退出码：0=通过（可带 warning）、3=拒绝（`--strict` 且标记缺失/为空）、2=参数或环境错误。
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

LOCK_DIR_REL = Path(".cache") / "lock"
LOCK_SUFFIX = "-md-lock.done"
# 被锁定的生产目录（AGENTS §一硬门禁 1）。push-ticket.sh 用它判「这次 diff 是否触及」。
LOCKED_PATH_PREFIX = "apps/ctp/md/"
ALLOW_ENV = "DZ_ALLOW_MD_LOCK"
REFUSED = 3

USAGE = f"""usage: md_lock_marker.py write <issue> [--sha <sha>]
       md_lock_marker.py check <issue> [--strict] [--base <ref>] [--head <sha>]
       md_lock_marker.py --selftest

`{ALLOW_ENV}=1` allows a push without the marker (emergency only).
"""


class EnvError(Exception):
    """环境前提不满足（不在 git 仓库等）。"""


def _git(*args: str, cwd: Path | str | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(["git", *args], capture_output=True, text=True, cwd=cwd)


def repo_root() -> Path:
    result = _git("rev-parse", "--show-toplevel")
    if result.returncode != 0 or not result.stdout.strip():
        raise EnvError(f"not inside a git repository: {result.stderr.strip()}")
    return Path(result.stdout.strip())


def resolve_head(sha: str | None, root: Path) -> str:
    raw = sha or "HEAD"
    result = _git("rev-parse", "--verify", "--quiet", f"{raw}^{{commit}}", cwd=root)
    if result.returncode != 0 or not result.stdout.strip():
        raise EnvError(f"sha not resolvable: {raw}")
    return result.stdout.strip()


def marker_path(root: Path, issue: str) -> Path:
    return root / LOCK_DIR_REL / f"{issue}{LOCK_SUFFIX}"


def write_marker(root: Path, issue: str, sha: str) -> Path:
    path = marker_path(root, issue)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(sha + "\n", encoding="utf-8")
    return path


def read_marker(root: Path, issue: str) -> str:
    """返回标记里的 sha；文件缺失返回空串。"""
    path = marker_path(root, issue)
    if not path.is_file():
        return ""
    try:
        return path.read_text(encoding="utf-8", errors="replace").strip()
    except OSError:
        return ""


def touches_locked_path(root: Path, base: str, head: str) -> tuple[bool, str]:
    """`base...head` 的改动是否触及生产锁定目录。返回 (是否触及, 命中的路径或原因)。"""
    result = _git("diff", "--name-only", f"{base}...{head}", cwd=root)
    if result.returncode != 0:
        return False, f"cannot compute changed paths: {result.stderr.strip()}"
    for line in result.stdout.splitlines():
        path = line.strip().replace("\\", "/")
        if path.startswith(LOCKED_PATH_PREFIX):
            return True, path
    return False, ""


def default_base(root: Path) -> str:
    """把「当前分支上游」解析成可用于 `git diff base...head` 的 ref。

    三种失败形态都归到 `origin/main`（与 `scripts/check-push-scope.sh` 的 `resolve_base()`
    同一口径，故意保持逐条一致——两处判定的对象不同，但「本票的 diff 从哪算起」必须是
    同一个答案，否则 scope 说通过、md-lock 却算出一个不同的改动面）：
      1) 没有上游（rev-parse 非零退出，无输出）
      2) 上游指向 URL（`git push -u <URL>` 的后果）——rev-parse 打印字面量 "@{u}"
      3) 上游 ref 不可解析
    """
    result = _git("rev-parse", "--abbrev-ref", "--symbolic-full-name", "@{u}", cwd=root)
    resolved = result.stdout.strip() if result.returncode == 0 else ""
    if not resolved or resolved == "@{u}":
        return "origin/main"
    probe = _git("rev-parse", "--verify", "--quiet", f"{resolved}^{{commit}}", cwd=root)
    if probe.returncode != 0 or not probe.stdout.strip():
        return "origin/main"
    return resolved


def check(
    root: Path, issue: str, *, strict: bool, allow_env: bool, base: str, head: str
) -> int:
    touched, path = touches_locked_path(root, base, head)
    if not touched:
        # 不触及锁定目录的票不该被要求写标记（否则每个票都多一步机械动作）。path 非空时
        # 是「diff 算不出来」——那种情况按触及处理（宁可要求确认，不静默放行生产目录改动）。
        if path:
            print(f"md-lock: warning: {path}; treating as locked-path change for safety")
        else:
            print(f"md-lock: not applicable (#{issue} does not change {LOCKED_PATH_PREFIX})")
            return 0

    sha = read_marker(root, issue)
    if sha:
        print(f"md-lock: confirmed for #{issue} at {sha[:12]} (change: {path})")
        return 0

    message = (
        f"no md production-lock marker for #{issue} "
        f"({LOCK_DIR_REL}/{issue}{LOCK_SUFFIX} missing or empty) while the diff changes "
        f"{path}: AGENTS §一 hard gate 1 requires telling the user what changes in "
        "apps/ctp/md/ and getting their confirmation before touching it"
    )
    if allow_env:
        print(f"md-lock: warning: {message} (allowed by {ALLOW_ENV}=1, emergency only)")
        return 0
    if strict:
        print(f"md-lock: BLOCKED {message}", file=sys.stderr)
        print(
            f"md-lock: after the user confirms, run "
            f"`python3 scripts/md_lock_marker.py write {issue}` "
            f"(escape: {ALLOW_ENV}=1)",
            file=sys.stderr,
        )
        return REFUSED
    print(f"md-lock: warning: {message}")
    return 0


# ---------------------------------------------------------------------------
# --selftest：临时 git 仓库，锁住 write/check/触及判定/逃生口（不碰工作区、不联网）
# ---------------------------------------------------------------------------


def _selftest_git(root: Path, *args: str) -> str:
    result = _git(
        "-c",
        "user.name=md-lock-selftest",
        "-c",
        "user.email=selftest@example.invalid",
        "-c",
        "commit.gpgsign=false",
        *args,
        cwd=root,
    )
    if result.returncode != 0:
        raise EnvError(f"git {' '.join(args)} failed: {result.stderr.strip()}")
    return result.stdout.strip()


def _selftest_cli(repo: Path, *args: str, allow_env: bool = False) -> tuple[int, str]:
    env = dict(os.environ)
    env.pop(ALLOW_ENV, None)
    if allow_env:
        env[ALLOW_ENV] = "1"
    result = subprocess.run(
        [sys.executable, str(Path(__file__).resolve()), *args],
        capture_output=True,
        text=True,
        cwd=repo,
        env=env,
    )
    return result.returncode, result.stdout + result.stderr


def selftest() -> int:
    checks = 0
    failures: list[str] = []

    def expect(name: str, condition: bool, detail: str = "") -> None:
        nonlocal checks
        checks += 1
        if not condition:
            failures.append(f"{name}: {detail}" if detail else name)

    with tempfile.TemporaryDirectory(prefix="dz_md_lock_") as tmp:
        repo = Path(tmp)
        _selftest_git(repo, "init", "-q", "-b", "main")
        (repo / "README.md").write_text("# baseline\n", encoding="utf-8")
        _selftest_git(repo, "add", "README.md")
        _selftest_git(repo, "commit", "-q", "-m", "chore: baseline")
        base = _selftest_git(repo, "rev-parse", "HEAD")

        # 不触及 md 目录的提交：触及判定必须为 False（不然每个票都要写标记）
        (repo / "docs").mkdir()
        (repo / "docs" / "note.md").write_text("# note\n", encoding="utf-8")
        _selftest_git(repo, "add", "docs/note.md")
        _selftest_git(repo, "commit", "-q", "-m", "docs: note")
        docs_head = _selftest_git(repo, "rev-parse", "HEAD")
        hits, path = touches_locked_path(repo, base, docs_head)
        expect("docs-only diff does not touch the locked path", not hits, f"hit={path}")

        # 触及 md 目录的提交：必须命中
        (repo / "apps" / "ctp" / "md").mkdir(parents=True)
        (repo / "apps" / "ctp" / "md" / "md_api.cpp").write_text("int x = 0;\n", encoding="utf-8")
        _selftest_git(repo, "add", "apps/ctp/md/md_api.cpp")
        _selftest_git(repo, "commit", "-q", "-m", "feat: touch md")
        md_head = _selftest_git(repo, "rev-parse", "HEAD")
        hits, path = touches_locked_path(repo, docs_head, md_head)
        expect("md diff is detected", hits and path.startswith(LOCKED_PATH_PREFIX), f"hit={path}")

        # check：无标记 + --strict → 拒绝（exit 3）
        rc, out = _selftest_cli(
            repo, "check", "480", "--strict", "--base", docs_head, "--head", md_head
        )
        expect("missing marker is refused in strict mode", rc == REFUSED, f"rc={rc} out={out!r}")
        expect("refusal explains the hard gate", "hard gate 1" in out, f"out={out!r}")

        # check：无标记 + 默认（warning）→ 放行
        rc, out = _selftest_cli(repo, "check", "480", "--base", docs_head, "--head", md_head)
        expect("missing marker only warns without --strict", rc == 0, f"rc={rc} out={out!r}")

        # 逃生口：无标记 + --strict + DZ_ALLOW_MD_LOCK=1 → 放行且明示
        rc, out = _selftest_cli(
            repo, "check", "480", "--strict", "--base", docs_head, "--head", md_head, allow_env=True
        )
        expect("escape hatch allows the push", rc == 0, f"rc={rc} out={out!r}")
        expect("escape hatch is announced", ALLOW_ENV in out, f"out={out!r}")

        # 不触及锁定目录的票：即使没有标记也放行，且**不**要求确认（不打扰无关票）
        rc, out = _selftest_cli(
            repo, "check", "480", "--strict", "--base", base, "--head", docs_head
        )
        expect("non-touching ticket needs no marker", rc == 0, f"rc={rc} out={out!r}")
        expect("non-touching ticket says not applicable", "not applicable" in out, f"out={out!r}")

        # write → check 通过；标记内容 = 指定 sha
        rc, out = _selftest_cli(repo, "write", "480", "--sha", docs_head)
        expect("write succeeds", rc == 0, f"rc={rc} out={out!r}")
        expect("marker records the sha", read_marker(repo, "480") == docs_head, read_marker(repo, "480"))
        rc, out = _selftest_cli(
            repo, "check", "480", "--strict", "--base", docs_head, "--head", md_head
        )
        expect("marker admits the push", rc == 0, f"rc={rc} out={out!r}")

        # 标记按票号隔离：别的票号仍然缺失
        rc, _ = _selftest_cli(
            repo, "check", "999", "--strict", "--base", docs_head, "--head", md_head
        )
        expect("marker is per-issue", rc == REFUSED, f"rc={rc}")

        # 空标记文件按缺失处理（写了一半/被截断不能当成确认过）
        marker_path(repo, "481").parent.mkdir(parents=True, exist_ok=True)
        marker_path(repo, "481").write_text("   \n", encoding="utf-8")
        rc, _ = _selftest_cli(
            repo, "check", "481", "--strict", "--base", docs_head, "--head", md_head
        )
        expect("empty marker is treated as missing", rc == REFUSED, f"rc={rc}")

        # 默认 write 用 HEAD（不传 --sha）
        rc, _ = _selftest_cli(repo, "write", "482")
        expect(
            "write defaults to HEAD",
            rc == 0 and read_marker(repo, "482") == md_head,
            f"rc={rc} marker={read_marker(repo, '482')!r}",
        )

    if failures:
        for failure in failures:
            print(f"selftest FAIL {failure}", file=sys.stderr)
        print(f"selftest FAILED: {len(failures)}/{checks} case(s)", file=sys.stderr)
        return 1
    print(
        f"selftest: {checks} cases (touched-path / non-touching / strict / warning / "
        "escape / write / per-issue / empty)"
    )
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="apps/ctp/md production-lock confirmation marker",
        add_help=True,
    )
    parser.add_argument("--selftest", action="store_true", help="临时 git 仓库自测，不碰工作区")
    sub = parser.add_subparsers(dest="command")
    write_parser = sub.add_parser("write", help="record the user's confirmation for <issue>")
    write_parser.add_argument("issue")
    write_parser.add_argument("--sha", default=None, help="confirmed sha (default HEAD)")
    check_parser = sub.add_parser("check", help="push-time admission check")
    check_parser.add_argument("issue")
    check_parser.add_argument(
        "--strict", action="store_true", help=f"refuse (exit {REFUSED}) when the marker is missing"
    )
    check_parser.add_argument(
        "--base",
        default=None,
        help="base ref for the changed-path diff (default: branch upstream, else origin/main)",
    )
    check_parser.add_argument("--head", default=None, help="pushed sha (default HEAD)")
    return parser


def main(argv: list[str]) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    if args.selftest:
        return selftest()
    if args.command not in ("write", "check"):
        print(USAGE, file=sys.stderr)
        return 2
    if not args.issue.isdigit():
        print(f"md-lock: error: issue must be a number, got {args.issue!r}", file=sys.stderr)
        return 2
    try:
        root = repo_root()
        if args.command == "write":
            sha = resolve_head(args.sha, root)
            path = write_marker(root, args.issue, sha)
            print(f"md-lock: wrote {path.relative_to(root)} @ {sha[:12]}")
            return 0
        allow_env = os.environ.get(ALLOW_ENV, "0") == "1"
        head = resolve_head(args.head, root)
        base = args.base or default_base(root)
        return check(
            root, args.issue, strict=args.strict, allow_env=allow_env, base=base, head=head
        )
    except EnvError as exc:
        print(f"md-lock: error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
