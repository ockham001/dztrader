#!/usr/bin/env python3
"""review_marker.py — 只读验证轮的机器状态标记（#289 AC6）

为什么需要
----------
「修复之后的只读验证轮」到底跑没跑，此前只存在于会话记录里，纯文档约束挡不住跳过：
#253 修完 Spec 轴 BLOCKING 后 `12:18:35` 推送、`12:38:45` 合并，而验证轮的准备工作
`12:29` 才发生、随即被宿主 turn 错误中止 —— **验证从未发生，代码已经合并**。文档写
「最后一轮只读验证返回后才能 push」属于动作点约束，对真正跳过它的执行者无效；把
「某个票的某个审查轴在哪个 commit 上完成过只读验证」落成文件，才能让 `.githooks/pre-push`
在推送时把「验证轮可能没跑」变成可见的提示。

它**只提示、不拦截**，理由有两条：
  1. 写标记的是 verifier / Lead，机器分不清「验证没跑」与「跑完了但忘了写」，拦截会
     误伤正常推送（假告警最终会让信号失效，参见 `scripts/ci_budget.py` 的同类教训）；
  2. `.cache/` 是本地忽略目录，标记不随 commit 走，机器无法把它当成硬门禁的真相源。

规则
----
- 标记文件：`.cache/review/<issue>-<axis>.done`（相对仓库根；`.cache/` 已被 gitignore）
- 内容：一行 commit sha，默认取 `git rev-parse HEAD`；显式传入时以参数为准
- `check <issue>`：读该票全部 `<issue>-*.done` 标记，逐轴比较 HEAD 与标记 sha；不一致
  即 warning，**标记目录里没有该票的任何标记也 warning**（从未跑过验证轮）
- `check <issue> --strict`：同样的判定，但有问题时退出码 3（供 `scripts/push-ticket.sh`
  在 push 前**默认拒绝**，见 #309 AC6）。默认 `check` 的退出码与 warning 文案**一个字都
  不变**——`.githooks/pre-push` 依赖「只提示、不拦截」
- `--expect-axis <axis>`（可重复）：逐轴点名要求标记存在且停在 HEAD；缺标记的轴单独
  warning。不改变 sha 比较口径（前缀比较仍走 `same_commit`）
- sha 按前缀比较（显式传 `git rev-parse --short HEAD` 的结果同样合法）
- 退出码恒 0（默认 `check`）：只提示，不改变 pre-push 的拦截结论

用法
----
  python3 scripts/review_marker.py write 289 standards          # 标记 HEAD 已验证
  python3 scripts/review_marker.py write 289 standards <sha>    # 标记指定 commit
  python3 scripts/review_marker.py check 289                    # pre-push 调用：只 warning
  python3 scripts/review_marker.py check 289 --strict \\
      --expect-axis standards --expect-axis spec                # push-ticket 调用：有问题退出 3
  python3 scripts/review_marker.py --selftest                   # 临时 git 仓库自测，工作区只读

退出码
------
0=成功（默认 `check` 恒为 0，无论有没有 warning）
1=selftest 失败
2=用法或环境错误（缺参数 / 非数字票号 / 非法 axis / 不在 git 仓库 / 标记不可写）
3=`check --strict` 且有问题（缺标记 / 标记为空 / 标记 sha 与 HEAD 不一致）
   （`--strict` 下 git 环境不可用也报 2；默认 `check` 遇到同样情况仍返回 0，只打 skip 提示）
"""

from __future__ import annotations

import argparse
import contextlib
import io
import os
import re
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

MARKER_DIR_REL = Path(".cache") / "review"
MARKER_SUFFIX = ".done"
ISSUE_RE = re.compile(r"^[0-9]+$")
AXIS_RE = re.compile(r"^[a-z][a-z0-9_-]*$")
SHA_RE = re.compile(r"^[0-9a-fA-F]{4,40}$")
SHORT_SHA_LEN = 12


class MarkerError(Exception):
    """环境前提不满足（不是 git 仓库 / HEAD 不可解析 / 标记不可写）。"""


def repo_root() -> Path:
    """仓库根：优先 git（worktree / 子目录调用都正确），失败抛 MarkerError。"""
    result = subprocess.run(["git", "rev-parse", "--show-toplevel"],
                            capture_output=True, text=True)
    if result.returncode != 0:
        raise MarkerError(f"not inside a git repository: {result.stderr.strip()}")
    return Path(result.stdout.strip())


def git_head(root: Path) -> str:
    """当前 HEAD 的完整 sha；不可解析时抛 MarkerError（不再静默当作已标记）。"""
    result = subprocess.run(["git", "rev-parse", "HEAD"], cwd=root,
                            capture_output=True, text=True)
    if result.returncode != 0:
        raise MarkerError(f"cannot resolve HEAD in {root}: {result.stderr.strip()}")
    head = result.stdout.strip()
    if not SHA_RE.fullmatch(head):
        raise MarkerError(f"unexpected HEAD revision in {root}: {head!r}")
    return head.lower()


def marker_dir(root: Path) -> Path:
    return root / MARKER_DIR_REL


def marker_path(root: Path, issue: str, axis: str) -> Path:
    return marker_dir(root) / f"{issue}-{axis}{MARKER_SUFFIX}"


def write_marker(root: Path, issue: str, axis: str, sha: str) -> Path:
    """写标记（内容一行 sha）。目录创建失败/不可写时抛 MarkerError（→ 退出码 2）。"""
    path = marker_path(root, issue, axis)
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(sha + "\n", encoding="utf-8")
    except OSError as exc:
        raise MarkerError(f"cannot write review marker {path}: {exc}") from exc
    return path


@dataclass(frozen=True)
class Marker:
    """一个轴上的验证标记；sha 为空串表示文件为空/内容不是 commit sha。"""

    axis: str
    path: Path
    sha: str = ""

    @property
    def valid(self) -> bool:
        return bool(self.sha)


def read_markers(root: Path, issue: str) -> list[Marker]:
    """读该票全部 `.cache/review/<issue>-*.done` 标记；目录不存在时返回空列表。"""
    directory = marker_dir(root)
    if not directory.is_dir():
        return []
    prefix = f"{issue}-"
    markers: list[Marker] = []
    for path in sorted(directory.iterdir()):
        if not path.is_file() or not path.name.startswith(prefix):
            continue
        if not path.name.endswith(MARKER_SUFFIX):
            continue
        axis = path.name[len(prefix):-len(MARKER_SUFFIX)]
        if not axis:
            continue
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            text = ""
        lines = text.splitlines()
        first = lines[0].strip() if lines else ""
        sha = first.lower() if SHA_RE.fullmatch(first) else ""
        markers.append(Marker(axis=axis, path=path, sha=sha))
    return markers


def short_sha(sha: str) -> str:
    return sha[:SHORT_SHA_LEN]


def same_commit(head: str, marker_sha: str) -> bool:
    """HEAD 与标记 sha 是否指同一 commit；标记允许是 HEAD 的短 sha 前缀。"""
    marker_sha = marker_sha.lower()
    if not SHA_RE.fullmatch(marker_sha):
        return False
    return head == marker_sha or head.startswith(marker_sha)


def check_markers(root: Path, issue: str, head: str,
                  expected_axes: tuple[str, ...] = ()) -> list[str]:
    """返回该票的 warning 列表（纯文本；由调用方决定前缀与输出流）。

    没有任何标记也算 warning：从未跑过验证轮的票不该因为「没留下状态」而静默通过
    —— #253 的失败形态正是「验证轮被中止后没人知道它没跑」。

    `expected_axes` 非空时（#309 AC6：`--expect-axis`）改为**逐轴点名**：每个被点名的轴
    单独判定「标记缺失 / 为空 / sha 与 HEAD 不一致」，不再输出泛化的「没有任何标记」行
    ——push-ticket 要能把两个轴各自的缺失状态贴进报告。
    """
    markers = read_markers(root, issue)
    if expected_axes:
        by_axis = {marker.axis: marker for marker in markers}
        warnings: list[str] = []
        for axis in expected_axes:
            marker = by_axis.get(axis)
            if marker is None:
                warnings.append(
                    f"no review marker for #{issue} ({axis}); "
                    "the read-only verification round for that axis may not have run")
            elif not marker.valid:
                warnings.append(
                    f"review marker for #{issue} ({axis}) is empty or invalid; "
                    "the read-only verification round may not have run")
            elif not same_commit(head, marker.sha):
                warnings.append(
                    f"review marker for #{issue} ({axis}) is at {short_sha(marker.sha)}, "
                    f"HEAD is {short_sha(head)}; the read-only verification round may not have run")
        return warnings
    if not markers:
        return [f"no review marker for #{issue}; "
                "the read-only verification round may not have run"]
    warnings = []
    for marker in markers:
        if not marker.valid:
            warnings.append(
                f"review marker for #{issue} ({marker.axis}) is empty or invalid; "
                "the read-only verification round may not have run")
        elif not same_commit(head, marker.sha):
            warnings.append(
                f"review marker for #{issue} ({marker.axis}) is at {short_sha(marker.sha)}, "
                f"HEAD is {short_sha(head)}; the read-only verification round may not have run")
    return warnings


# ---------------------------------------------------------------------------
# argparse 类型：非法输入统一走 argparse（退出码 2），与其它 scripts/ 检查器同口径
# ---------------------------------------------------------------------------


def issue_arg(text: str) -> str:
    if not ISSUE_RE.fullmatch(text) or int(text) <= 0:
        raise argparse.ArgumentTypeError(
            f"invalid issue number: {text!r} (expected a positive integer, e.g. 289)")
    return text


def axis_arg(text: str) -> str:
    if not AXIS_RE.fullmatch(text):
        raise argparse.ArgumentTypeError(
            f"invalid axis: {text!r} (expected [a-z][a-z0-9_-]*, e.g. standards)")
    return text


def sha_arg(text: str) -> str:
    if not SHA_RE.fullmatch(text.strip()):
        raise argparse.ArgumentTypeError(
            f"invalid commit sha: {text!r} (expected 4-40 hex characters)")
    return text.strip()


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="read-only verification marker for pre-push (warning only)")
    parser.add_argument("--selftest", action="store_true",
                        help="run the checker against temp git repos (worktree untouched)")
    sub = parser.add_subparsers(dest="command", metavar="<command>")

    write_parser = sub.add_parser(
        "write", help="mark <issue>-<axis> as verified at <sha> (default: HEAD)")
    write_parser.add_argument("issue", type=issue_arg)
    write_parser.add_argument("axis", type=axis_arg)
    write_parser.add_argument("sha", nargs="?", default=None, type=sha_arg)

    check_parser = sub.add_parser(
        "check", help="warn when HEAD differs from the marker (default exits 0; --strict exits 3)")
    check_parser.add_argument("issue", type=issue_arg)
    check_parser.add_argument(
        "--strict", action="store_true",
        help="exit 3 when any marker is missing, empty or not at HEAD "
             "(for scripts/push-ticket.sh; the default check stays warning-only)")
    check_parser.add_argument(
        "--expect-axis", action="append", default=[], metavar="AXIS", type=axis_arg,
        help="require a marker for this axis (repeatable); reports each axis separately")
    return parser


# ---------------------------------------------------------------------------
# selftest：临时 git 仓库模拟 write/check 三态，工作区只读（#289 AC6 判据）
# ---------------------------------------------------------------------------


@dataclass
class Case:
    name: str
    problems: list[str]

    def check(self, condition: bool, detail: str) -> None:
        if not condition:
            self.problems.append(detail)


def _selftest_git(root: Path, *args: str) -> str:
    """临时仓库专用 git（自带身份/关签名，避免受用户全局配置干扰）。"""
    result = subprocess.run(
        ["git",
         "-c", "user.name=review-marker-selftest",
         "-c", "user.email=selftest@example.invalid",
         "-c", "commit.gpgsign=false",
         *args],
        cwd=root, capture_output=True, text=True)
    if result.returncode != 0:
        raise MarkerError(f"git {' '.join(args)} failed in {root}: {result.stderr.strip()}")
    return result.stdout.strip()


def _selftest_repo(path: Path) -> Path:
    path.mkdir(parents=True)
    _selftest_git(path, "init", "-q", "-b", "main")
    (path / "tracked.txt").write_text("one\n", encoding="utf-8")
    _selftest_git(path, "add", "tracked.txt")
    _selftest_git(path, "commit", "-q", "-m", "one")
    return path


def _selftest_commit(root: Path, content: str) -> str:
    (root / "tracked.txt").write_text(content, encoding="utf-8")
    _selftest_git(root, "add", "tracked.txt")
    _selftest_git(root, "commit", "-q", "-m", content.strip())
    return _selftest_git(root, "rev-parse", "HEAD").lower()


def _run_cli(cwd: Path, argv: list[str]) -> tuple[int, str, str]:
    """在 cwd 下跑 main()，捕获 stdout/stderr 与退出码（argparse 的 SystemExit 也折算）。"""
    out, err = io.StringIO(), io.StringIO()
    previous = os.getcwd()
    try:
        os.chdir(cwd)
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            try:
                code = main(argv)
            except SystemExit as exc:  # argparse：用法错误走这里（2）
                code = exc.code if isinstance(exc.code, int) else 2
    finally:
        os.chdir(previous)
    return code, out.getvalue(), err.getvalue()


def _warning_lines(stderr: str) -> list[str]:
    return [line for line in stderr.splitlines() if line.startswith("pre-push: review marker")
            or line.startswith("pre-push: no review marker")]


def _case_same_sha(tmp: Path) -> Case:
    case = Case("same sha -> no warning, rc=0", [])
    repo = _selftest_repo(tmp / "same_sha")
    head = _selftest_git(repo, "rev-parse", "HEAD").lower()
    rc, out, err = _run_cli(repo, ["write", "289", "standards"])
    marker = repo / ".cache" / "review" / "289-standards.done"
    case.check(rc == 0, f"write rc={rc}, stderr={err!r}")
    case.check(marker.is_file(), f"marker file missing: {marker}")
    case.check(marker.is_file() and marker.read_text(encoding="utf-8").strip() == head,
               "marker content must be the default HEAD sha")
    case.check(f"marker: wrote .cache/review/289-standards.done @ {head[:SHORT_SHA_LEN]}"
               in out, f"unexpected write output: {out!r}")
    rc, _out, err = _run_cli(repo, ["check", "289"])
    case.check(rc == 0, f"check rc={rc}")
    case.check(not _warning_lines(err), f"same sha must not warn, got: {err!r}")
    return case


def _case_new_commit_warns(tmp: Path) -> Case:
    case = Case("new commit -> warning, rc=0", [])
    repo = _selftest_repo(tmp / "new_commit")
    old_head = _selftest_git(repo, "rev-parse", "HEAD").lower()
    rc, _out, err = _run_cli(repo, ["write", "289", "standards"])
    case.check(rc == 0, f"write rc={rc}, stderr={err!r}")
    new_head = _selftest_commit(repo, "two")
    rc, _out, err = _run_cli(repo, ["check", "289"])
    warnings = _warning_lines(err)
    case.check(rc == 0, f"check must exit 0 even with warnings, got rc={rc}")
    case.check(len(warnings) == 1, f"expected exactly one warning, got: {warnings}")
    if warnings:
        case.check("(standards)" in warnings[0], f"warning must name the axis: {warnings[0]}")
        case.check(old_head[:SHORT_SHA_LEN] in warnings[0],
                   f"warning must show the marker sha: {warnings[0]}")
        case.check(new_head[:SHORT_SHA_LEN] in warnings[0],
                   f"warning must show HEAD: {warnings[0]}")
        case.check("may not have run" in warnings[0],
                   f"warning must explain the risk: {warnings[0]}")
    return case


def _case_no_marker_warns(tmp: Path) -> Case:
    case = Case("no marker -> warning, rc=0", [])
    repo = _selftest_repo(tmp / "no_marker")
    rc, _out, err = _run_cli(repo, ["check", "289"])
    warnings = _warning_lines(err)
    case.check(rc == 0, f"check must exit 0 without a marker, got rc={rc}")
    case.check(len(warnings) == 1, f"expected exactly one warning, got: {warnings}")
    if warnings:
        case.check("no review marker for #289" in warnings[0],
                   f"warning must name the issue: {warnings[0]}")
    # 别的票的标记不能豁免本票（文件按票号前缀隔离）。
    _run_cli(repo, ["write", "290", "standards"])
    rc, _out, err = _run_cli(repo, ["check", "289"])
    case.check(rc == 0, f"check rc={rc}")
    case.check(len(_warning_lines(err)) == 1,
               f"marker for #290 must not cover #289, got: {_warning_lines(err)}")
    return case


def _case_multi_axis_independent(tmp: Path) -> Case:
    case = Case("standards/spec axes are independent", [])
    repo = _selftest_repo(tmp / "multi_axis")
    _run_cli(repo, ["write", "289", "standards"])
    _selftest_commit(repo, "two")
    rc, _out, err = _run_cli(repo, ["write", "289", "spec"])
    case.check(rc == 0, f"write spec rc={rc}, stderr={err!r}")
    rc, _out, err = _run_cli(repo, ["check", "289"])
    warnings = _warning_lines(err)
    case.check(rc == 0, f"check rc={rc}")
    case.check(len(warnings) == 1,
               f"only the stale axis may warn, got: {warnings}")
    if warnings:
        case.check("(standards)" in warnings[0],
                   f"stale axis must be standards: {warnings[0]}")
        case.check("(spec)" not in warnings[0],
                   f"fresh axis must not warn: {warnings[0]}")
    return case


def _case_explicit_sha(tmp: Path) -> Case:
    case = Case("explicit sha wins over HEAD", [])
    repo = _selftest_repo(tmp / "explicit_sha")
    old_head = _selftest_git(repo, "rev-parse", "HEAD").lower()
    new_head = _selftest_commit(repo, "two")
    marker = repo / ".cache" / "review" / "289-spec.done"
    rc, _out, err = _run_cli(repo, ["write", "289", "spec", old_head])
    case.check(rc == 0, f"write rc={rc}, stderr={err!r}")
    case.check(marker.is_file() and marker.read_text(encoding="utf-8").strip() == old_head,
               "marker must store the explicitly passed sha, not HEAD")
    rc, _out, err = _run_cli(repo, ["check", "289"])
    case.check(rc == 0, f"check rc={rc}")
    case.check(len(_warning_lines(err)) == 1,
               f"explicit old sha must warn against HEAD, got: {_warning_lines(err)}")
    # 显式传短 sha（= 当前 HEAD 前缀）应视为同一 commit，不 warning。
    rc, _out, err = _run_cli(repo, ["write", "289", "spec", new_head[:SHORT_SHA_LEN]])
    case.check(rc == 0, f"write short sha rc={rc}, stderr={err!r}")
    case.check(marker.read_text(encoding="utf-8").strip() == new_head[:SHORT_SHA_LEN],
               "short sha must be stored verbatim")
    rc, _out, err = _run_cli(repo, ["check", "289"])
    case.check(rc == 0, f"check rc={rc}")
    case.check(not _warning_lines(err),
               f"short sha equal to HEAD must not warn, got: {err!r}")
    return case


def _case_strict_mode(tmp: Path) -> Case:
    case = Case("--strict -> rc=3 on problems, default check stays rc=0", [])
    repo = _selftest_repo(tmp / "strict")
    # 默认 check 的保真度：无标记时仍然 rc=0（pre-push 依赖这一点）。
    rc, _out, err = _run_cli(repo, ["check", "289"])
    case.check(rc == 0, f"default check must stay rc=0, got rc={rc}")
    case.check(len(_warning_lines(err)) == 1, f"expected the old warning, got: {err!r}")
    # --strict：同一状态 -> 3，warning 文本与默认模式逐字相同。
    rc, _out, err_strict = _run_cli(repo, ["check", "289", "--strict"])
    case.check(rc == 3, f"strict check without marker must exit 3, got rc={rc}")
    case.check(_warning_lines(err) and _warning_lines(err) == _warning_lines(err_strict),
               "strict mode must reuse the default warning text verbatim")
    # 标记停在 HEAD 时 strict 也放行。
    head = _selftest_git(repo, "rev-parse", "HEAD").lower()
    for axis in ("standards", "spec"):
        _run_cli(repo, ["write", "289", axis])
    rc, _out, err = _run_cli(repo, ["check", "289", "--strict"])
    case.check(rc == 0, f"strict check at HEAD sha must exit 0, got rc={rc} ({err!r})")
    # 旧 sha 的标记 -> 3（push-ticket 的核心拒绝条件）。
    _selftest_commit(repo, "two")
    rc, _out, err = _run_cli(repo, ["check", "289", "--strict"])
    case.check(rc == 3, f"strict check with stale marker must exit 3, got rc={rc}")
    case.check(any(head[:SHORT_SHA_LEN] in line for line in _warning_lines(err)),
               f"stale warning must show the marker sha, got: {_warning_lines(err)}")
    # 空标记文件（0 字节）-> 3。
    marker = repo / ".cache" / "review" / "289-spec.done"
    marker.write_text("\n", encoding="utf-8")
    head2 = _selftest_git(repo, "rev-parse", "HEAD").lower()
    _run_cli(repo, ["write", "289", "standards", head2])
    rc, _out, err = _run_cli(repo, ["check", "289", "--strict"])
    case.check(rc == 3, f"strict check with an empty marker must exit 3, got rc={rc}")
    case.check(any("(spec) is empty or invalid" in line for line in _warning_lines(err)),
               f"empty-marker warning must name the axis, got: {_warning_lines(err)}")
    return case


def _case_expect_axis(tmp: Path) -> Case:
    case = Case("--expect-axis -> per-axis missing/empty/stale lines", [])
    repo = _selftest_repo(tmp / "expect_axis")
    head = _selftest_git(repo, "rev-parse", "HEAD").lower()
    axes = ["standards", "spec"]
    rc, _out, err = _run_cli(repo, ["check", "289", "--strict",
                                    *[arg for axis in axes for arg in ("--expect-axis", axis)]])
    warnings = _warning_lines(err)
    case.check(rc == 3, f"strict+expect-axis without markers must exit 3, got rc={rc}")
    case.check(len(warnings) == 2, f"one line per expected axis, got: {warnings}")
    case.check(any("(standards)" in line for line in warnings), f"standards axis missing: {warnings}")
    case.check(any("(spec)" in line for line in warnings), f"spec axis missing: {warnings}")
    # 只有 standards 有标记：spec 那一行仍然报缺失（不因「已有别的标记」而整体豁免）。
    _run_cli(repo, ["write", "289", "standards"])
    rc, _out, err = _run_cli(repo, ["check", "289", "--strict",
                                   "--expect-axis", "standards", "--expect-axis", "spec"])
    warnings = _warning_lines(err)
    case.check(rc == 3, f"partial axis coverage must exit 3, got rc={rc}")
    case.check(len(warnings) == 1 and "(spec)" in warnings[0],
               f"only the missing axis may warn, got: {warnings}")
    _run_cli(repo, ["write", "289", "spec"])
    rc, _out, err = _run_cli(repo, ["check", "289", "--strict",
                                   "--expect-axis", "standards", "--expect-axis", "spec"])
    case.check(rc == 0, f"both axes at HEAD must exit 0, got rc={rc} ({err!r})")
    case.check(not _warning_lines(err), f"no warning expected, got: {err!r}")
    # 短 sha 前缀比较口径必须复用同一套（不把 standards 标记换成 12 位短 sha 就失配）。
    _selftest_commit(repo, "two")
    head2 = _selftest_git(repo, "rev-parse", "HEAD").lower()
    _run_cli(repo, ["write", "289", "standards", head2[:SHORT_SHA_LEN]])
    _run_cli(repo, ["write", "289", "spec", head2[:SHORT_SHA_LEN]])
    rc, _out, err = _run_cli(repo, ["check", "289", "--strict",
                                   "--expect-axis", "standards", "--expect-axis", "spec"])
    case.check(rc == 0, f"short sha prefixes must match HEAD, got rc={rc} ({err!r})")
    return case


def _case_not_git_repo(tmp: Path) -> Case:
    case = Case("outside a git repo: default check rc=0, strict rc=2", [])
    plain = tmp / "plain"
    plain.mkdir()
    rc, _out, err = _run_cli(plain, ["check", "289"])
    case.check(rc == 0, f"default check outside a repo must stay rc=0, got rc={rc}")
    case.check("skipped" in err, f"default check must say it skipped, got: {err!r}")
    rc, _out, err = _run_cli(plain, ["check", "289", "--strict"])
    case.check(rc == 2, f"strict check outside a repo must exit 2, got rc={rc}")
    case.check("skipped" in err, f"strict check must still explain the skip, got: {err!r}")
    return case


def _case_usage_errors(tmp: Path) -> Case:
    case = Case("usage errors -> rc=2", [])
    repo = _selftest_repo(tmp / "usage")
    for argv in (["write", "289"], ["write", "abc", "standards"], ["write", "289", "Standards!"],
                 ["check"], ["check", "abc"], [], ["bogus"],
                 ["check", "289", "--expect-axis", "Standards!"],
                 ["check", "289", "--expect-axis"]):
        rc, _out, _err = _run_cli(repo, argv)
        case.check(rc == 2, f"argv={argv!r} must exit 2, got rc={rc}")
    return case


def run_selftest() -> int:
    cases: list[Case] = []
    with tempfile.TemporaryDirectory(prefix="dz_review_marker_selftest_") as tmp:
        base = Path(tmp)
        for builder in (_case_same_sha, _case_new_commit_warns, _case_no_marker_warns,
                        _case_multi_axis_independent, _case_explicit_sha,
                        _case_strict_mode, _case_expect_axis, _case_not_git_repo,
                        _case_usage_errors):
            case = builder(base)
            cases.append(case)
            if case.problems:
                print(f"selftest FAIL {case.name}", file=sys.stderr)
                for problem in case.problems:
                    print(f"     {problem}", file=sys.stderr)
            else:
                print(f"selftest ok {case.name}")

    failures = sum(1 for case in cases if case.problems)
    if failures:
        print(f"selftest FAILED: {failures}/{len(cases)} case(s)", file=sys.stderr)
        return 1
    print(f"selftest: {len(cases)} cases ok (temp git repos; the worktree is never modified)")
    return 0


def main(argv: list[str]) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)

    if args.selftest:
        return run_selftest()

    if args.command is None:
        parser.error("missing command: expected 'write' or 'check' (see --help)")

    if args.command == "write":
        try:
            root = repo_root()
            sha = (args.sha if args.sha is not None else git_head(root)).lower()
            path = write_marker(root, args.issue, args.axis, sha)
        except MarkerError as exc:
            print(f"error: {exc}", file=sys.stderr)
            return 2
        display = path.relative_to(root).as_posix()
        print(f"marker: wrote {display} @ {short_sha(sha)}")
        return 0

    # check：默认只提示、不拦截（退出码恒 0）。连 git 环境都不可用时也只降级为提示，
    # 因为它挂在 pre-push 上——门禁脚本自身的故障不该改变推送结论（#253 的教训）。
    # `--strict`（#309 AC6）给 scripts/push-ticket.sh 用：同样的判定，有问题时退出 3。
    # 两条路径的唯一差别是退出码，warning 文案一条不改（pre-push 输出保持逐字一致）。
    try:
        root = repo_root()
        head = git_head(root)
    except MarkerError as exc:
        print(f"pre-push: review marker check skipped; {exc}", file=sys.stderr)
        # 默认 check 必须恒 0（pre-push 契约）；`--strict` 是拦截语义，环境坏了不能当成
        # 「验证过」——报 2（环境错误），由 push-ticket 折算成参数/环境错误。
        return 2 if args.strict else 0
    warnings = check_markers(root, args.issue, head, tuple(args.expect_axis))
    for warning in warnings:
        print(f"pre-push: {warning}", file=sys.stderr)
    if args.strict and warnings:
        return 3
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
