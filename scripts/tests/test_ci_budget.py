#!/usr/bin/env python3
"""tests for scripts/ci_budget.py — Windows CI 墙钟分档口径（#267）

这些用例锁定的是**口径**，不是实测墙钟（墙钟受 runner 抖动影响，无法确定性断言）。
口径来自 #267 的观测数据：

    PR     cpp  hdr   Build      Windows job
    #268   0    0     4m06s      11m11s
    #264   1    0     5m32s      13m31s
    #269   12   11    16m59s     25m17s
    #263   21   11    27m33s     34m51s
    #262   0*   0*    32m35s     41m01s   *改了 ci.yml（sccache 配置），缓存 key 全失效

（cnp/hdr 是 `git diff --name-only` 的真实计数。GitHub PR 页面的 files 列表会把
#263 显示成 32 个 C++ 文件——含重命名项与重复项，虚高约 50%，用它会判错档位。）

关键结论：**墙钟由改动规模主导，不是一个固定数字**。`#264` 与 `#268` 证明缓存生效时
~11-14min 可达；`#263`（21 个 .cpp + 11 个 .h）跑 35min 是正常的。
任何单一「≤15min」目标都会把后者判成失败，产生无意义的告警（#260 的遗留问题）。

跑法（本套件是本仓第一个 unittest 套件；其余 Python 检查器用各自脚本内的 `--selftest`。
两者都接到 preflight 的 `check_ci_budget`：
    python3 -m unittest discover -s scripts/tests -v
"""

from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import ci_budget  # noqa: E402


class TestClassifyChangeScale(unittest.TestCase):
    def test_no_cpp_changes_is_small(self):
        self.assertEqual(ci_budget.classify_change_scale(cpp_files=0, header_files=0), "small")

    def test_single_cpp_file_is_small(self):
        # #264 实测：1 个 C++ 文件 -> 13m31s，与 0 个同档
        self.assertEqual(ci_budget.classify_change_scale(cpp_files=1, header_files=0), "small")

    def test_many_headers_alone_promote_to_medium(self):
        # 头文件改动会通过 include 传递失效大量 TU，即便 0 个 .cpp 被改
        self.assertEqual(ci_budget.classify_change_scale(cpp_files=0, header_files=5), "medium")

    def test_medium_band_matches_observed_269(self):
        # #269 真实 diff 计数 (12 cpp, 11 h) -> 实测 25m17s
        self.assertEqual(ci_budget.classify_change_scale(cpp_files=12, header_files=11), "medium")

    def test_large_band_matches_observed_263(self):
        # #263 真实 diff 计数 (21 cpp, 11 h) -> 实测 34m51s
        self.assertEqual(ci_budget.classify_change_scale(cpp_files=21, header_files=11), "large")

    def test_upper_boundary_is_inclusive(self):
        # 边界值必须落在低档（避免「刚好等于阈值」被判高档）
        self.assertEqual(ci_budget.classify_change_scale(cpp_files=2, header_files=0), "small")
        self.assertEqual(ci_budget.classify_change_scale(cpp_files=16, header_files=0), "medium")

    def test_negative_input_rejected(self):
        # 统计来源是 git diff；负数说明调用方算错了，应显式失败而非静默归零
        with self.assertRaises(ValueError):
            ci_budget.classify_change_scale(cpp_files=-1, header_files=0)
        with self.assertRaises(ValueError):
            ci_budget.classify_change_scale(cpp_files=0, header_files=-1)


class TestBudgetFor(unittest.TestCase):
    def test_small_budget_covers_observed_maximum(self):
        # small 档实测最大 13m31s（#264）-> 预算必须 >= 该值
        budget = ci_budget.budget_minutes("small")
        self.assertGreaterEqual(budget, 14, f"small budget {budget} too tight for 13m31s")

    def test_medium_budget_covers_observed_269(self):
        # medium 档实测 25m17s（#269）
        budget = ci_budget.budget_minutes("medium")
        self.assertGreaterEqual(budget, 26, f"medium budget {budget} too tight for 25m17s")

    def test_large_budget_covers_observed_263(self):
        # large 档实测最大 34m51s（#263）
        budget = ci_budget.budget_minutes("large")
        self.assertGreaterEqual(budget, 35, f"large budget {budget} too tight for 34m51s")

    def test_budgets_are_strictly_increasing(self):
        # 档位必须单调：否则分档毫无意义
        small = ci_budget.budget_minutes("small")
        medium = ci_budget.budget_minutes("medium")
        large = ci_budget.budget_minutes("large")
        self.assertLess(small, medium)
        self.assertLess(medium, large)

    def test_unknown_class_rejected(self):
        with self.assertRaises(KeyError):
            ci_budget.budget_minutes("huge")


class TestEvaluate(unittest.TestCase):
    def test_within_budget_passes(self):
        verdict = ci_budget.evaluate(cpp_files=0, header_files=0, actual_minutes=11.2)
        self.assertTrue(verdict.ok)
        self.assertEqual(verdict.change_class, "small")

    def test_over_budget_fails_with_actionable_message(self):
        # small 档跑 35 分钟 = 缓存大概率没命中，需要指出这一点
        verdict = ci_budget.evaluate(cpp_files=1, header_files=0, actual_minutes=35.0)
        self.assertFalse(verdict.ok)
        self.assertIn("cache", verdict.message.lower())

    def test_slow_run_in_large_band_is_not_over_budget(self):
        # 关键回归：#263 的真实 diff 计数 (21 cpp, 11 h) 跑 35min 属正常，**不得**判超预算
        # （这正是 #260 单一 ≤15min 目标造成的假告警）
        verdict = ci_budget.evaluate(cpp_files=21, header_files=11, actual_minutes=35.0)
        self.assertTrue(verdict.ok, verdict.message)


class TestCountFromGit(unittest.TestCase):
    """count_from_git 按扩展名分类——这是档位判定的入口，分类错会让整条链失真。

    用**临时 git 仓库**造确定的 diff，而不是拿真实历史的 hash 断言（后者恒真、
    且依赖当前 clone 的提交图，浅克隆/其它 worktree 下会直接报错）。
    """

    def _repo_with_files(self, names):
        """建一个临时仓库，提交一批文件，返回 (tmpdir, base_sha)。"""
        tmp = tempfile.mkdtemp(prefix="dz_ci_budget_test_")
        self.addCleanup(shutil.rmtree, tmp, True)

        def run(*args):
            subprocess.run(["git", "-C", tmp, *args], check=True,
                           capture_output=True, text=True)

        run("init", "-q", ".")
        run("config", "user.email", "selftest@example.com")
        run("config", "user.name", "selftest")
        for name in names:
            path = Path(tmp) / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("// placeholder\n", encoding="utf-8")
        run("add", "-A")
        run("commit", "-q", "-m", "chore(#267): baseline")
        base = subprocess.run(["git", "-C", tmp, "rev-parse", "HEAD"],
                              check=True, capture_output=True, text=True).stdout.strip()
        return tmp, base

    def _add_and_commit(self, tmp, names, message):
        for name in names:
            path = Path(tmp) / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("// changed\n", encoding="utf-8")
        subprocess.run(["git", "-C", tmp, "add", "-A"], check=True, capture_output=True)
        subprocess.run(["git", "-C", tmp, "commit", "-q", "-m", message],
                       check=True, capture_output=True)
        return subprocess.run(["git", "-C", tmp, "rev-parse", "HEAD"],
                              check=True, capture_output=True, text=True).stdout.strip()

    def test_counts_split_cpp_and_headers_exactly(self):
        tmp, base = self._repo_with_files(["keep.txt"])
        head = self._add_and_commit(
            tmp,
            ["src/a.cpp", "src/b.cc", "include/c.h"],
            "chore(#267): two cpp one header",
        )
        cpp, headers = ci_budget.count_from_git(f"{base}..{head}", cwd=tmp)
        self.assertEqual((cpp, headers), (2, 1))

    def test_non_cpp_files_are_ignored(self):
        tmp, base = self._repo_with_files(["keep.txt"])
        head = self._add_and_commit(
            tmp, ["docs/x.md", "scripts/y.py", "NONE"], "chore(#267): no cpp changes")
        cpp, headers = ci_budget.count_from_git(f"{base}..{head}", cwd=tmp)
        self.assertEqual((cpp, headers), (0, 0))

    def test_bad_range_raises(self):
        with self.assertRaises(RuntimeError):
            ci_budget.count_from_git("not-a-real-range..nope")


class TestSelftest(unittest.TestCase):
    def test_selftest_passes(self):
        self.assertEqual(ci_budget.run_selftest(), 0)


if __name__ == "__main__":
    unittest.main()


class TestParseSccacheStats(unittest.TestCase):
    """解析 sccache --show-stats 文本（#267 AC1：命中率必须可机器读取）。"""

    SAMPLE = """Compile requests                     1234
Compile requests executed            1100
Cache hits                            800
Cache misses                          200
Cache timeouts                          0
Cache read errors                       0
Forced recaches                         0
Cache write errors                      0
Compilation failures                    0
"""

    def test_extracts_hits_and_misses(self):
        stats = ci_budget.parse_sccache_stats(self.SAMPLE)
        self.assertEqual(stats.hits, 800)
        self.assertEqual(stats.misses, 200)

    def test_hit_rate_is_computed(self):
        stats = ci_budget.parse_sccache_stats(self.SAMPLE)
        self.assertAlmostEqual(stats.hit_rate, 0.8, places=6)

    def test_zero_requests_yields_none_not_zero(self):
        # 关键：没有编译请求时命中率是「无意义」而不是 0%——用 0 会让「全 miss」
        # 与「没编译」混为一谈，正是本仓反复强调的「不得用 0 冒充数据」。
        stats = ci_budget.parse_sccache_stats("Cache hits  0\nCache misses  0\n")
        self.assertIsNone(stats.hit_rate)

    def test_real_world_format_with_extra_lines(self):
        # 真实 sccache 输出含非 ASCII 折线图与版本行，解析不得受其影响
        text = (
            "Compile requests                     10\n"
            "Cache hits                            9\n"
            "Cache misses                          1\n"
            "\n"
            "Cache hits rate                   90.00 %\n"
        )
        stats = ci_budget.parse_sccache_stats(text)
        self.assertEqual((stats.hits, stats.misses), (9, 1))

    def test_missing_lines_raise(self):
        with self.assertRaises(ValueError):
            ci_budget.parse_sccache_stats("some unrelated output\n")


class TestEvaluateWithCacheStats(unittest.TestCase):
    """#267 AC1：命中率必须真正参与归因，而不是拿了数据却不用。"""

    def test_low_hit_rate_overrun_blames_cache(self):
        stats = ci_budget.SccacheStats(hits=10, misses=90)   # 10%
        verdict = ci_budget.evaluate(1, 0, 35.0, stats=stats)
        self.assertFalse(verdict.ok)
        self.assertIn("not effective", verdict.message)
        self.assertIn("hit_rate=10.0%", verdict.message)

    def test_healthy_hit_rate_overrun_does_not_blame_cache(self):
        # 关键回归：命中率健康却超时 -> 不得把锅甩给缓存（那会让人白查 warm job）
        stats = ci_budget.SccacheStats(hits=900, misses=100)  # 90%
        verdict = ci_budget.evaluate(1, 0, 35.0, stats=stats)
        self.assertFalse(verdict.ok)
        self.assertIn("healthy", verdict.message)
        self.assertIn("not from caching", verdict.message)

    def test_no_stats_says_collect_them(self):
        # 没数据时明确要求先取证，而不是猜一个原因
        verdict = ci_budget.evaluate(1, 0, 35.0)
        self.assertFalse(verdict.ok)
        self.assertIn("no cache stats available", verdict.message)

    def test_within_budget_includes_hit_rate(self):
        stats = ci_budget.SccacheStats(hits=80, misses=20)
        verdict = ci_budget.evaluate(0, 0, 11.2, stats=stats)
        self.assertTrue(verdict.ok)
        self.assertIn("hit_rate=80.0%", verdict.message)
