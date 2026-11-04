#!/usr/bin/env python3
"""tests for scripts/ci_budget.py — Windows CI 墙钟分档口径（#267）

这些用例锁定的是**口径**，不是实测墙钟（墙钟受 runner 抖动影响，无法确定性断言）。
口径来自 #267 的观测数据：

    PR     C++ 文件  Build      Windows job
    #268   0        4m06s      11m11s
    #264   1        5m32s      13m31s
    #269   23       16m59s     25m17s
    #263   32       27m33s     34m51s
    #262   0*       32m35s     41m01s   *改了 ci.yml（sccache 配置），缓存 key 全失效

关键结论：**墙钟由改动规模主导，不是一个固定数字**。`#264` 与 `#268` 证明缓存生效时
~11-14min 可达；`#263` 证明 32 个 C++ 文件时 35min 是正常的。任何单一「≤15min」
目标都会把后者判成失败，产生无意义的告警（#260 的遗留问题）。

跑法（与仓库其它 Python 检查器一致，不引入 pytest 依赖）：
    python3 -m unittest discover -s scripts/tests -v
"""

from __future__ import annotations

import sys
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
        # 关键回归：#263 的真实计数 (21 cpp, 11 h) 跑 35min 属正常，**不得**判超预算
        # （这正是 #260 单一 ≤15min 目标造成的假告警）
        verdict = ci_budget.evaluate(cpp_files=21, header_files=11, actual_minutes=35.0)
        self.assertTrue(verdict.ok, verdict.message)


class TestCountFromGit(unittest.TestCase):
    """count_from_git 按扩展名分类——这是档位判定的入口，分类错会让整条链失真。"""

    def test_counts_split_cpp_and_headers(self):
        # 用真实历史的知名改动：dbf1ef3 那次改了 .cpp/.h/测试
        cpp, headers = ci_budget.count_from_git("b2d8e66^1..b2d8e66")
        self.assertGreaterEqual(cpp, 0)
        self.assertGreaterEqual(headers, 0)

    def test_bad_range_raises(self):
        with self.assertRaises(RuntimeError):
            ci_budget.count_from_git("not-a-real-range..nope")


class TestSelftest(unittest.TestCase):
    def test_selftest_passes(self):
        self.assertEqual(ci_budget.run_selftest(), 0)


if __name__ == "__main__":
    unittest.main()
