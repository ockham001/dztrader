#!/usr/bin/env python3
"""ci_budget.py — Windows CI 墙钟的**分档预算**（#267）

为什么不再用单一「≤15min」目标
------------------------------
#260 曾把「PR 墙钟 ≤10min build / ≤15min job」写成目标。实测数据（2026-11-04，六个
含代码改动的 PR）证明这个数字**不可能恒成立**，因为它假设缓存完全命中——而 PR 的语义
就是改源码，改动必然让对应 TU 的缓存失效：

（下表 `cpp`/`hdr` 是 **`git diff --name-only` 的真实计数**，不是 GitHub PR 页面的
files 列表——后者含重命名项与重复项，实测虚高约 50%，用它校准会判错档位。）

    PR     cpp  hdr  Build      Windows job   命中情况
    #268   0    0    4m06s      11m11s        全命中（只改脚本/文档）
    #264   1    0    5m32s      13m31s        全命中（1 个 TU）
    #269   12   11   16m59s     25m17s        部分
    #263   21   11   27m33s     34m51s        部分（头文件传递失效大量 TU）
    #262   0*   0*   32m35s     41m01s        零命中（*改了 ci.yml 的 sccache 配置，
                                             warm 写下的旧 key 全部失配）

结论：**墙钟由改动规模主导**。0/1 个 C++ 文件时 ~11–14min 可达（缓存生效的证据）；
21 个文件时 35min 属正常。用 15min 卡所有 PR，会把 #263 判成失败——假告警，而假告警
会让「CI 绿」这个信号失真（#260 遗留的正是这个问题）。

同时记录一条**不成立**的改进方向：把 PR 的 `SCCACHE_GHA_RW_MODE` 从 READ_ONLY 改成
READ_WRITE 不会改善后续 PR 的命中率——GitHub Actions 缓存按 ref 作用域，PR 写在
`refs/pull/N/merge` 下，其它 PR 读不到（正是 #260 把 warm job 放在 main 上的原因）。
所以本票不改这个 env。

口径
----
改动规模按 `git diff --name-only` 的扩展名统计成两档输入：
  - `cpp_files`：`.cpp/.cc/.cxx/.c` 数量（直接编译单元）
  - `header_files`：`.h/.hpp/.hh/.hxx` 数量（经 include 传递失效，放大因子高）
档位边界取真实样本间的自然间隙（0–1 / 12 / 21 三簇；权威值是下方常量，
改边界必须同步本段）：

    small  : cpp_files <= 2  且 header_files <= 4
    medium : cpp_files <= 16 且 header_files <= 20
    large  : 其余

预算取该档**实测最大值再留一档余量**（不追求贴合实测，避免 runner 抖动误报）：

    small  : 18min（实测 13m31s）
    medium : 30min（实测 25m17s）
    large  : 45min（实测 34m51s；ci.yml 的 job timeout 是 90min，不冲突）

用法:
  python3 scripts/ci_budget.py --cpp 1 --headers 0 --actual 13.5
  python3 scripts/ci_budget.py --from-git origin/main...HEAD --actual 25.3
  # 从 CI artifact 取命中率并入归因（AC1 要的命中率-墙钟关联）
  gh run download <run-id> -n windows-sccache-stats -D .cache/ci-stats
  python3 scripts/ci_budget.py --from-git origin/main...HEAD --actual 25.3 \
      --stats-file .cache/ci-stats/sccache-stats.txt
  python3 scripts/ci_budget.py --selftest

退出码: 0=在预算内, 1=超预算, 2=参数错误
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

# 档位边界：由**真实 diff 计数**校准，不是估计（#267）。
#
# 初版按 PR 页面 files 列表估的边界（MEDIUM_MAX_CPP=24）在真实数据上判错了 #263：
# PR 列表显示 32 个 C++ 文件，但 `git diff --name-only` 只有 21 个 .cpp + 11 个 .h ——
# 列表含被重命名的路径与重复项。用错口径会让 (21,11) 落进 medium 而实测 34.9min 超预算。
#
# 真实计数与实测墙钟：
#   PR     cpp  headers  实测
#   #268   0    0        11.2min
#   #264   1    0        13.5min
#   #269   12   11       25.3min
#   #263   21   11       34.9min
#
# 边界取样本间的自然间隙：0–1 / 12 / 21 三簇 → small ≤2、medium ≤16、其余 large。
SMALL_MAX_CPP = 2
SMALL_MAX_HEADERS = 4
MEDIUM_MAX_CPP = 16
MEDIUM_MAX_HEADERS = 20

# 各档预算（分钟）：实测最大值再留余量（不贴合实测，避免 runner 抖动误报）
BUDGETS_MINUTES = {
    "small": 18,   # 实测 13.5min
    "medium": 30,  # 实测 25.3min
    "large": 45,   # 实测 34.9min
}

# 后缀集合：与 `scripts/ci_scope.py` 的 `CPP_SUFFIXES` 是同一份事实的两处消费方——这里
# 用来**数改动规模**（分档预算），那里用来**判 denylist**（fast/full）。两个脚本都是独立
# 单文件检查器（本仓约定：可 `python3 scripts/x.py --selftest` 单跑，不互相 import），
# 所以有意各留一份；改后缀集合时两处一起改（#289 审查 conformance 7 指出过这条）。
CPP_SUFFIXES = (".cpp", ".cc", ".cxx", ".c")
HEADER_SUFFIXES = (".h", ".hpp", ".hh", ".hxx")


def classify_change_scale(cpp_files: int, header_files: int) -> str:
    """按改动的 C++ 文件数返回 'small' / 'medium' / 'large'。

    头文件单独计入：改 5 个头文件可能牵连上百个 TU（#263 的 21 个 .cpp 之外还有 11 个
    .h，真正失效的 TU 远多于 21），只看 .cpp 数会低估。
    """
    if cpp_files < 0 or header_files < 0:
        raise ValueError(
            f"file counts must be non-negative: cpp_files={cpp_files} "
            f"header_files={header_files}")
    if cpp_files <= SMALL_MAX_CPP and header_files <= SMALL_MAX_HEADERS:
        return "small"
    if cpp_files <= MEDIUM_MAX_CPP and header_files <= MEDIUM_MAX_HEADERS:
        return "medium"
    return "large"


def budget_minutes(change_class: str) -> int:
    """该档位的 Windows job 墙钟预算（分钟）。未知档位抛 KeyError。"""
    if change_class not in BUDGETS_MINUTES:
        raise KeyError(f"unknown change class: {change_class!r}")
    return BUDGETS_MINUTES[change_class]


@dataclass(frozen=True)
class Verdict:
    ok: bool
    change_class: str
    budget: int
    actual: float
    message: str


# 命中率低于此值视为「缓存基本没生效」：改 ci.yml/sccache 配置的那次（#262）是零命中；
# 正常 PR 应显著高于它（#268/#264 的全命中 run 可达 90%+）。
CACHE_HIT_RATE_FLOOR = 0.5


def evaluate(cpp_files: int,
             header_files: int,
             actual_minutes: float,
             stats: SccacheStats | None = None) -> Verdict:
    """把一次实测墙钟与该档预算比对，给出可用作 CI 判据的结论。

    `stats` 可选：给了就把命中率并入结论，让「超预算」的归因有数据支撑，而不是靠猜
    （#267 AC1 要的正是这个关联）。没给则退回纯墙钟判定。
    """
    change_class = classify_change_scale(cpp_files, header_files)
    budget = budget_minutes(change_class)
    rate = stats.hit_rate if stats is not None else None
    rate_part = "" if rate is None else f" hit_rate={rate:.1%}"

    if actual_minutes <= budget:
        return Verdict(
            ok=True,
            change_class=change_class,
            budget=budget,
            actual=actual_minutes,
            message=(f"within budget | class={change_class} budget={budget}min "
                     f"actual={actual_minutes:.1f}min{rate_part}"),
        )

    # 超预算时按**可用数据**给归因，而不是固定话术：
    #   命中率低 -> 缓存问题（查 warm job / 是否改了 sccache 配置）
    #   命中率高但超时 -> 规模或 runner 抖动，不是缓存
    if rate is not None and rate < CACHE_HIT_RATE_FLOOR:
        hint = (f"cache is not effective (hit_rate={rate:.1%} < "
                f"{CACHE_HIT_RATE_FLOOR:.0%}); check the warm job ran on main and that "
                "this PR does not touch ci.yml/sccache config")
    elif rate is not None:
        hint = (f"cache is healthy (hit_rate={rate:.1%}); overrun comes from change size "
                "or runner jitter, not from caching")
    else:
        hint = ("no cache stats available; collect the windows-sccache-stats artifact "
                "before attributing the overrun")
    return Verdict(
        ok=False,
        change_class=change_class,
        budget=budget,
        actual=actual_minutes,
        message=(f"over budget | class={change_class} budget={budget}min "
                 f"actual={actual_minutes:.1f}min over={actual_minutes - budget:.1f}min "
                 f"{rate_part.strip()} hint=\"{hint}\"".replace("  ", " ")),
    )


def count_from_git(rev_range: str, cwd: str | None = None) -> tuple[int, int]:
    """从 `git diff --name-only <range>` 统计 (cpp_files, header_files)。

    `-c core.quotepath=false`：git 默认把非 ASCII 路径转义成 `"\344\270\255.cpp"`
    （带引号），`endswith` 会失配导致少计；真实仓库有中文路径的历史文件。
    `cwd` 仅供测试指定临时仓库。
    """
    out = subprocess.run(
        ["git", "-c", "core.quotepath=false", "diff", "--name-only", rev_range],
        capture_output=True,
        text=True,
        cwd=cwd,
    )
    if out.returncode != 0:
        raise RuntimeError(f"git diff failed for {rev_range!r}: {out.stderr.strip()}")
    cpp = headers = 0
    for path in out.stdout.splitlines():
        lowered = path.lower()
        if lowered.endswith(CPP_SUFFIXES):
            cpp += 1
        elif lowered.endswith(HEADER_SUFFIXES):
            headers += 1
    return cpp, headers


# sccache --show-stats 的文本形态（稳定）：`Cache hits            800`，中间是空格对齐。
# 只取这两行算命中率；其余行（Cache writes / errors）本票不用。
SCCACHE_HIT_RE = re.compile(r"^\s*Cache hits\s+(\d+)\s*$", re.MULTILINE)
SCCACHE_MISS_RE = re.compile(r"^\s*Cache misses\s+(\d+)\s*$", re.MULTILINE)


@dataclass(frozen=True)
class SccacheStats:
    hits: int
    misses: int

    @property
    def total(self) -> int:
        return self.hits + self.misses

    @property
    def hit_rate(self) -> float | None:
        """命中率 0.0–1.0；没有编译请求时返回 None（不是 0，避免与「全 miss」混淆）。"""
        if self.total == 0:
            return None
        return self.hits / self.total


def parse_sccache_stats(text: str) -> SccacheStats:
    """从 `sccache --show-stats` 的文本里取 hits/misses。

    为什么需要它（#267 AC1）：命中率原本只打在 job 日志里，而 Windows 日志 99% 是
    /showIncludes 噪声、且步骤是 continue-on-error —— 实测无法稳定取到。改成 artifact
    后需要有人能解析，否则数据落了也没人用。
    """
    hit = SCCACHE_HIT_RE.search(text)
    miss = SCCACHE_MISS_RE.search(text)
    if hit is None or miss is None:
        raise ValueError(
            "sccache stats not found: expected 'Cache hits <n>' and 'Cache misses <n>'; "
            f"got {len(text)} chars")
    return SccacheStats(hits=int(hit.group(1)), misses=int(miss.group(1)))


# ---------------------------------------------------------------------------
# selftest：锁定档位边界与本票的核心回归（大改动慢是正常的，不得判失败）
# ---------------------------------------------------------------------------
SELFTEST_CASES: list[tuple[int, int, str]] = [
    # (cpp_files, header_files, expected class) —— 用 **git diff 真实计数**
    (0, 0, "small"),      # #268 实测算术点（11.2min）
    (1, 0, "small"),      # #264 实测点（13.5min）
    (2, 4, "small"),      # small 上边界
    (0, 5, "medium"),     # 纯头文件改动即升档
    (12, 11, "medium"),   # #269 实测点（25.3min）
    (3, 0, "medium"),     # small 的下一个值——锁 SMALL_MAX_CPP 不被上调
    (16, 20, "medium"),   # medium 上边界
    (16, 21, "large"),    # 锁 MEDIUM_MAX_HEADERS 不被上调
    (17, 0, "large"),     # 锁 MEDIUM_MAX_CPP 不被上调
    (21, 11, "large"),    # #263 实测点（34.9min）
]


def run_selftest() -> int:
    failures = 0

    for cpp, headers, expected in SELFTEST_CASES:
        got = classify_change_scale(cpp, headers)
        if got != expected:
            print(f"selftest FAIL class({cpp},{headers}): expected {expected}, got {got}",
                  file=sys.stderr)
            failures += 1

    # 预算必须单调且覆盖实测最大值
    observed = {"small": 13.5, "medium": 25.3, "large": 34.9}
    budgets = [budget_minutes(k) for k in ("small", "medium", "large")]
    if budgets != sorted(budgets) or len(set(budgets)) != 3:
        print(f"selftest FAIL budgets not strictly increasing: {budgets}", file=sys.stderr)
        failures += 1
    for cls, actual in observed.items():
        if budget_minutes(cls) < actual:
            print(f"selftest FAIL budget({cls})={budget_minutes(cls)} < observed {actual}",
                  file=sys.stderr)
            failures += 1

    # 核心回归：#263 的真实计数 (21 cpp, 11 hdr) / 34.9min 在大档**不得**判超预算。
    # 这正是残留的单一 ≤15min 目标会产生的假告警（#260 遗留问题）。
    big = evaluate(21, 11, 34.9)
    if not big.ok:
        print(f"selftest FAIL #263-scale run judged over budget: {big.message}", file=sys.stderr)
        failures += 1

    # 小档超时必须是失败且提示缓存
    small_over = evaluate(1, 0, 35.0)
    if small_over.ok or "cache" not in small_over.message.lower():
        print(f"selftest FAIL small-band overrun not flagged with cache hint: "
              f"{small_over.message}", file=sys.stderr)
        failures += 1

    # 非法输入
    for bad in ((-1, 0), (0, -1)):
        try:
            classify_change_scale(*bad)
        except ValueError:
            pass
        else:
            print(f"selftest FAIL negative input not rejected: {bad}", file=sys.stderr)
            failures += 1
    try:
        budget_minutes("huge")
    except KeyError:
        pass
    else:
        print("selftest FAIL unknown class not rejected", file=sys.stderr)
        failures += 1

    if failures:
        print(f"selftest FAILED: {failures} case(s)", file=sys.stderr)
        return 1
    print(f"selftest: {len(SELFTEST_CASES)} scale bands + budgets + edge cases ok")
    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Windows CI wall-clock budget by change scale")
    parser.add_argument("--cpp", type=int, help="number of changed .cpp files")
    parser.add_argument("--headers", type=int, help="number of changed .h files")
    parser.add_argument("--from-git", metavar="RANGE",
                        help="derive counts from 'git diff --name-only RANGE'")
    parser.add_argument("--actual", type=float, metavar="MINUTES",
                        help="measured Windows job wall clock in minutes")
    parser.add_argument("--stats-file", metavar="PATH",
                        help="sccache --show-stats text (the windows-sccache-stats artifact)")
    parser.add_argument("--selftest", action="store_true", help="run rule selftest")
    args = parser.parse_args(argv)

    if args.selftest:
        return run_selftest()

    stats = None
    if args.stats_file:
        try:
            stats = parse_sccache_stats(Path(args.stats_file).read_text(encoding="utf-8"))
        except (OSError, ValueError) as exc:
            print(f"error: cannot read stats file: {exc}", file=sys.stderr)
            return 2

    if args.from_git:
        try:
            cpp, headers = count_from_git(args.from_git)
        except RuntimeError as exc:
            print(f"error: {exc}", file=sys.stderr)
            return 2
    elif args.cpp is not None and args.headers is not None:
        cpp, headers = args.cpp, args.headers
    else:
        parser.error("need either --from-git or both --cpp and --headers")

    change_class = classify_change_scale(cpp, headers)
    budget = budget_minutes(change_class)

    if args.actual is None:
        # 只报告档位与预算（观测用）
        print(f"class={change_class} budget={budget}min cpp={cpp} headers={headers}")
        return 0

    verdict = evaluate(cpp, headers, args.actual, stats=stats)
    print(verdict.message)
    return 0 if verdict.ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
