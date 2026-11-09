#!/usr/bin/env python3
"""gate_status.py — 门禁结果按命令分槽读取 + 推送放行判定（#316 AC2）

## 为什么需要

`scripts/gate.sh` 原先把所有子命令的结果写进**同一个** `.cache/gate/last-status`，
`.githooks/pre-push` 只读 `rc=` / `head=`、不看 `cmd=`。而
[`docs/agents/ticket-closeout.md`](../docs/agents/ticket-closeout.md) §6 规定**每次关票前
必须跑 `gate ledger`**——ledger 必然绿，于是「build 在 HEAD 上红 → 跑 ledger → last-status
变 rc=0 → pre-push 认为最近一次门禁通过 → 放行」的闭环漏洞成立。本波 #303 在 push 之后
跑了 2 次 ledger，直接把 preflight 记录覆盖掉（#316 AC2 顺序证据：三票都是「先 push/合并、
后 ledger」）。

## 规则

分槽写：`gate.sh` 每次结果同时写 `.cache/gate/last-status-<cmd>`（`build` / `test` /
`preflight` / `ledger`）与兼容视图 `last-status`（「最近一次任意门禁」；本脚本不读它）。

放行判定按**票型**（= `scripts/ci_scope.py` 对 `base...head` 改动面的 fast / full 分类，
与 CI 的验证面**同一份定义**，不在这里另立一套）：

  - fast（docs / scripts / githooks，验证面 = preflight）→ 要求 `preflight` 记录
  - full（含 C/C++ / 前端 / 构建系统 / workflow / 构建脚本）→ 要求 `build` 与 `test` 记录

`ledger` **永不参与放行判定**（它是台账体检，与代码是否可推送无关）。

每条要求的语义（沿用 #190 的保守口径）：

  - 记录缺失 / 文件不可读 → **只警告**（agent 常在 commit 前跑门禁、commit 后直接推）
  - 记录在**当前 HEAD** 且 `rc!=0` → **拒绝推送**
  - 记录来自别的 HEAD → **只警告**（结论不是关于被推 commit 的）

应急逃生口 `DZ_ALLOW_RED_GATE=1`：拒绝态降级为 WARNING 并放行（沿用既有含义）。

## 用法

  python3 scripts/gate_status.py check [--head <sha>] [--base <ref>]
  python3 scripts/gate_status.py --selftest

`check` 的默认 head = HEAD，base = origin/main（不可解析时按「票型未知」处理：只警告，
不误拦）。`.githooks/pre-push` 以被推的 local sha 调本脚本；三态自测在本脚本的
`--selftest`（临时 git 仓库 + 假状态文件，不碰工作区）。

退出码：0=放行（可带 warning）、1=拒绝、2=参数/环境错误（--selftest 失败也是 1）。
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    # 票型分类的唯一定义在 ci_scope.py（CI 的 scope job 也调它）：不在这里复制一份
    # denylist，否则两处定义必然漂移（#289 AC3 的既有教训）。
    sys.path.insert(0, str(SCRIPT_DIR))

import ci_scope  # noqa: E402

STATUS_DIR_REL = Path(".cache") / "gate"
STATUS_PREFIX = "last-status-"
# 分槽文件名的取值范围与 gate.sh 的子命令一致；ledger 永不参与放行判定。
REQUIRED_FOR_MODE = {"fast": ("preflight",), "full": ("build", "test")}
ALLOW_RED_GATE_ENV = "DZ_ALLOW_RED_GATE"
SHORT_SHA_LEN = 12


class EnvError(Exception):
    """环境前提不满足（不在 git 仓库 / head 不可解析）。"""


@dataclass(frozen=True)
class Status:
    """一条门禁记录：命令、退出码、HEAD、时间戳。"""

    cmd: str
    rc: int
    head: str
    time: str


def _git(*args: str, cwd: Path | str | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(["git", *args], capture_output=True, text=True, cwd=cwd)


def repo_root() -> Path:
    result = _git("rev-parse", "--show-toplevel")
    if result.returncode != 0:
        raise EnvError(f"not inside a git repository: {result.stderr.strip()}")
    return Path(result.stdout.strip())


def parse_status(text: str, cmd: str) -> Status | None:
    """解析 `record_last_status` 写出的 k=v 纯文本；格式不可读返回 None。

    格式由 scripts/gate.sh 单一写入（cmd/rc/head/time），这里只读解析。
    """
    fields: dict[str, str] = {}
    for line in text.splitlines():
        key, sep, value = line.partition("=")
        if sep:
            fields[key.strip()] = value.strip()
    rc_text = fields.get("rc", "")
    if not rc_text.isdigit():
        return None
    return Status(
        cmd=cmd,
        rc=int(rc_text),
        head=fields.get("head", ""),
        time=fields.get("time", ""),
    )


def read_status(gate_dir: Path, cmd: str) -> Status | None:
    path = gate_dir / f"{STATUS_PREFIX}{cmd}"
    if not path.is_file():
        return None
    try:
        return parse_status(path.read_text(encoding="utf-8", errors="replace"), cmd)
    except OSError:
        return None


def resolve_head(requested: str | None, root: Path) -> str:
    """校验并解析 head；缺省 HEAD。浅克隆/分离头指针也照常工作。"""
    raw = requested or "HEAD"
    result = _git("rev-parse", "--verify", "--quiet", f"{raw}^{{commit}}", cwd=root)
    if result.returncode != 0 or not result.stdout.strip():
        raise EnvError(f"head ref not found: {raw}")
    return result.stdout.strip()


def resolve_base(requested: str, root: Path) -> tuple[str | None, str]:
    """解析 base ref；失败返回 (None, 原因)，调用方按「票型未知」处理（只警告）。"""
    result = _git("rev-parse", "--verify", "--quiet", f"{requested}^{{commit}}", cwd=root)
    sha = result.stdout.strip()
    if result.returncode != 0 or not sha:
        return None, f"base ref '{requested}' is not resolvable"
    return sha, ""


def classify_ticket(base_sha: str | None, head: str, root: Path) -> tuple[str, str]:
    """票型（= 验证面）由 ci_scope.py 分类；base 不可解析时返回 ('unknown', 原因)。

    改动文件列表复用 `ci_scope.files_from_range`（加 `cwd` 指向本仓根）：同一事实只定义
    一处，不在这里再写一遍 git diff 解析。
    """
    if base_sha is None:
        return "unknown", "base is unknown"
    try:
        paths = ci_scope.files_from_range(f"{base_sha}...{head}", cwd=str(root))
    except RuntimeError as exc:
        return "unknown", f"cannot compute changed paths: {exc}"
    mode, reason = ci_scope.classify(paths)
    return mode, reason


def judge(
    gate_dir: Path,
    head: str,
    mode: str,
    *,
    allow_red_gate: bool = False,
) -> tuple[list[str], list[str]]:
    """按票型判定放行；返回 (blocking, warnings)。`unknown` 票型零要求、只给提示。"""
    blocking: list[str] = []
    warnings: list[str] = []
    if mode == "unknown":
        warnings.append(
            "ticket type is unknown (base diff not resolvable); gate records are not "
            "checked against HEAD"
        )
        return blocking, warnings

    required = REQUIRED_FOR_MODE[mode]
    for cmd in required:
        status = read_status(gate_dir, cmd)
        if status is None:
            warnings.append(
                f"no {cmd} gate record ({STATUS_DIR_REL}/{STATUS_PREFIX}{cmd} missing); "
                f"run ./scripts/gate.sh {cmd} before pushing"
            )
            continue
        if status.head == head and status.rc != 0:
            blocking.append(f"{cmd} gate FAILED (rc={status.rc}) at HEAD {head[:SHORT_SHA_LEN]}")
            continue
        if status.head != head:
            warnings.append(
                f"{cmd} gate record is from {status.head[:SHORT_SHA_LEN] or 'unknown'}, "
                f"not HEAD {head[:SHORT_SHA_LEN]}; its result is not about the pushed commit"
            )
    if allow_red_gate and blocking:
        warnings.append(
            f"WARNING: pushing with a failing gate allowed by {ALLOW_RED_GATE_ENV}=1 "
            "(emergency only)"
        )
        blocking = []
    return blocking, warnings


def check(
    root: Path,
    *,
    head_ref: str | None,
    base_ref: str,
    gate_dir: Path,
    allow_red_gate: bool,
) -> int:
    head = resolve_head(head_ref, root)
    base_sha, base_note = resolve_base(base_ref, root)
    mode, reason = classify_ticket(base_sha, head, root)
    blocking, warnings = judge(gate_dir, head, mode, allow_red_gate=allow_red_gate)
    if mode == "unknown":
        print(f"gate-status: ticket unknown ({base_note or reason}); no required gate records")
    else:
        required = ",".join(REQUIRED_FOR_MODE[mode])
        print(f"gate-status: ticket {mode} (validation face={required}) via {reason}")
    for warning in warnings:
        print(f"gate-status: warning: {warning}")
    if blocking:
        for message in blocking:
            print(f"gate-status: BLOCKED {message}")
        print(
            "gate-status: blocking push; rerun the failing gate on the pushed HEAD "
            f"(escape: {ALLOW_RED_GATE_ENV}=1)"
        )
        return 1
    print("gate-status: ok")
    return 0


# ---------------------------------------------------------------------------
# --selftest：临时 git 仓库 + 假状态文件，锁住三态与逃生口（不碰工作区）
# ---------------------------------------------------------------------------


def _selftest_git(root: Path, *args: str) -> str:
    result = _git(
        "-c",
        "user.name=gate-status-selftest",
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


def _selftest_write_status(
    gate_dir: Path, name: str, *, rc: int, head: str, cmd: str
) -> None:
    gate_dir.mkdir(parents=True, exist_ok=True)
    content = f"cmd={cmd}\nrc={rc}\nhead={head}\ntime=1000\n"
    (gate_dir / f"{STATUS_PREFIX}{name}").write_text(content, encoding="utf-8")


def _selftest_clear(gate_dir: Path) -> None:
    shutil.rmtree(gate_dir, ignore_errors=True)


def _selftest_cli(repo: Path, *args: str, allow_red_gate: bool = False) -> tuple[int, str]:
    """在临时仓库里跑本脚本的 `check` 子命令（子进程，取真实退出码与输出）。"""
    env = dict(os.environ)
    env.pop(ALLOW_RED_GATE_ENV, None)
    if allow_red_gate:
        env[ALLOW_RED_GATE_ENV] = "1"
    result = subprocess.run(
        [sys.executable, str(Path(__file__).resolve()), "check", *args],
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

    with tempfile.TemporaryDirectory(prefix="dz_gate_status_") as tmp:
        repo = Path(tmp)
        _selftest_git(repo, "init", "-q", "-b", "main")
        (repo / "README.md").write_text("# baseline\n", encoding="utf-8")
        _selftest_git(repo, "add", "README.md")
        _selftest_git(repo, "commit", "-q", "-m", "chore: baseline")
        base_docs = _selftest_git(repo, "rev-parse", "HEAD")
        (repo / "docs").mkdir()
        (repo / "docs" / "frame_contracts").mkdir()
        (repo / "docs" / "frame_contracts" / "sample.md").write_text("# doc\n", encoding="utf-8")
        _selftest_git(repo, "add", "docs/frame_contracts/sample.md")
        _selftest_git(repo, "commit", "-q", "-m", "docs: sample contract")
        docs_head = _selftest_git(repo, "rev-parse", "HEAD")
        (repo / "libs").mkdir()
        (repo / "libs" / "core").mkdir()
        (repo / "libs" / "core" / "sample.cpp").write_text("int x = 0;\n", encoding="utf-8")
        _selftest_git(repo, "add", "libs/core/sample.cpp")
        _selftest_git(repo, "commit", "-q", "-m", "feat: sample code")
        code_head = _selftest_git(repo, "rev-parse", "HEAD")
        gate_dir = repo / STATUS_DIR_REL

        # 态 1：ledger 绿 + build 红，同一 HEAD，代码票 → 拒绝（ledger 覆盖不掉 build 的红）
        _selftest_clear(gate_dir)
        _selftest_write_status(gate_dir, "ledger", rc=0, head=code_head, cmd="ledger")
        _selftest_write_status(gate_dir, "build", rc=1, head=code_head, cmd="build")
        _selftest_write_status(gate_dir, "test", rc=0, head=code_head, cmd="test")
        rc, out = _selftest_cli(repo, "--head", code_head, "--base", docs_head)
        expect(
            "ledger-green/build-red at HEAD is refused",
            rc == 1,
            f"rc={rc} out={out!r}",
        )
        expect(
            "refusal names build",
            "BLOCKED build gate FAILED" in out,
            f"out={out!r}",
        )

        # 逃生口：同一态 + DZ_ALLOW_RED_GATE=1 → 放行（带 WARNING）
        rc, out = _selftest_cli(repo, "--head", code_head, "--base", docs_head, allow_red_gate=True)
        expect("escape hatch allows red gate", rc == 0, f"rc={rc} out={out!r}")
        expect("escape hatch is announced", "DZ_ALLOW_RED_GATE=1" in out, f"out={out!r}")

        # 态 2：build/test 绿但记录停在旧 HEAD → 只警告、放行
        _selftest_clear(gate_dir)
        _selftest_write_status(gate_dir, "build", rc=0, head=docs_head, cmd="build")
        _selftest_write_status(gate_dir, "test", rc=0, head=docs_head, cmd="test")
        rc, out = _selftest_cli(repo, "--head", code_head, "--base", docs_head)
        expect("green record from older HEAD only warns", rc == 0, f"rc={rc} out={out!r}")
        expect("warning names the stale record", "not HEAD" in out, f"out={out!r}")

        # 态 3：纯文档票（diff 只有 docs/*.md）+ preflight 绿 → 放行
        _selftest_clear(gate_dir)
        _selftest_write_status(gate_dir, "preflight", rc=0, head=docs_head, cmd="preflight")
        rc, out = _selftest_cli(repo, "--head", docs_head, "--base", base_docs)
        expect("docs ticket with green preflight is allowed", rc == 0, f"rc={rc} out={out!r}")
        expect("docs ticket requires only preflight", "validation face=preflight" in out, f"out={out!r}")

        # 态 3b：纯文档票 preflight 在 HEAD 上红 → 拒绝（票型判定不吞真红）
        _selftest_write_status(gate_dir, "preflight", rc=1, head=docs_head, cmd="preflight")
        rc, out = _selftest_cli(repo, "--head", docs_head, "--base", base_docs)
        expect("docs ticket with red preflight is refused", rc == 1, f"rc={rc} out={out!r}")

        # 态 4：代码票只跑过 ledger（绿）→ 缺失只警告，放行（保持 #190 的保守语义）
        _selftest_clear(gate_dir)
        _selftest_write_status(gate_dir, "ledger", rc=0, head=code_head, cmd="ledger")
        rc, out = _selftest_cli(repo, "--head", code_head, "--base", docs_head)
        expect("ledger alone never admits a code ticket", rc == 0, f"rc={rc} out={out!r}")
        expect("missing build/test records are warned", "no build gate record" in out, f"out={out!r}")
        expect("ledger is not consulted", "ledger gate" not in out, f"out={out!r}")

        # 态 5：记录文件损坏（格式不可读）→ 按缺失处理，只警告
        _selftest_clear(gate_dir)
        gate_dir.mkdir(parents=True)
        (gate_dir / f"{STATUS_PREFIX}build").write_text("garbage\n", encoding="utf-8")
        rc, out = _selftest_cli(repo, "--head", code_head, "--base", docs_head)
        expect("unreadable record only warns", rc == 0, f"rc={rc} out={out!r}")

        # 态 6：base 不可解析（未知票型）→ 零要求、只警告，绝不误拦
        rc, out = _selftest_cli(repo, "--head", code_head, "--base", "origin/does-not-exist")
        expect("unknown ticket type only warns", rc == 0, f"rc={rc} out={out!r}")

        # 解析器：k=v 形态与 rc 非数字
        expect(
            "parse_status accepts the gate.sh format",
            parse_status("cmd=build\nrc=0\nhead=abc\ntime=1\n", "build") == Status("build", 0, "abc", "1"),
        )
        expect("parse_status rejects non-numeric rc", parse_status("rc=oops\n", "build") is None)
        # 不带 head 的合法记录：解析必须成功且 head 为空串。写成 `or Status(...)` 兜底会
        # 让「parse_status 恒返 None」时该断言仍绿（恒真断言，code-review 实测）。
        missing_head = parse_status("rc=1\n", "build")
        expect(
            "parse_status tolerates missing head",
            missing_head is not None and missing_head.head == "",
            f"got {missing_head!r}",
        )

    if failures:
        for failure in failures:
            print(f"selftest FAIL {failure}", file=sys.stderr)
        print(f"selftest FAILED: {len(failures)}/{checks} case(s)", file=sys.stderr)
        return 1
    print(
        f"selftest: {checks} cases (ledger-vs-build / stale HEAD / docs-only / "
        "missing / unreadable / unknown ticket)"
    )
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="per-command gate status + push admission")
    parser.add_argument("--selftest", action="store_true", help="临时 git 仓库自测，不碰工作区")
    sub = parser.add_subparsers(dest="command")
    check_parser = sub.add_parser("check", help="judge whether the pushed HEAD may be pushed")
    check_parser.add_argument("--head", default=None, help="pushed sha (default HEAD)")
    check_parser.add_argument(
        "--base", default="origin/main", help="base ref for the ticket type diff (default origin/main)"
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
    try:
        root = repo_root()
    except EnvError as exc:
        print(f"gate-status: error: {exc}", file=sys.stderr)
        return 2
    gate_dir = root / STATUS_DIR_REL
    allow_red_gate = os.environ.get(ALLOW_RED_GATE_ENV, "0") == "1"
    try:
        return check(
            root,
            head_ref=args.head,
            base_ref=args.base,
            gate_dir=gate_dir,
            allow_red_gate=allow_red_gate,
        )
    except EnvError as exc:
        print(f"gate-status: error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
