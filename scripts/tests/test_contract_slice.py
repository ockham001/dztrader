#!/usr/bin/env python3
"""test_contract_slice.py — `scripts/contract_slice.py` 的进程级覆盖（#309 AC4 / #316 AC3）

为什么需要：AC4 的判据只要求 `--selftest` 绿，但只测内部函数会放过「参数接线写错 /
退出码吞掉 / 候选塞进 stdout」这类真实故障——本票要替代的正是 `sed -n` 分页，
工具被误用一次就会让下一次读契约拿到错误内容。所以这里全部**进程级**驱动 CLI
（`python3 scripts/contract_slice.py ...`），断言 stdout / stderr / 退出码。

锁住的契约：
  - `§2.9` 命中：首行是带节号的标题，且默认**不含子节**
  - `--children` 含子节
  - 未知节号 exit 1 且候选在 **stderr**（stdout 必须为空，不能混进管道的 head）
  - 未知契约 exit 1 且给候选
  - `--list` 列出全部契约 stem，并带编号标题；`--list <stem>` exit 2 且指路 `--outline`
  - 代码围栏里的假标题不算节（``` 与 ~~~ 两种围栏）
  - `--outline` 单份大纲（契约 stem / docs stem / 仓库相对路径），与 `--list` 同一形状；
    未知目标 exit 1 给候选、与 `--list`/位置参数互斥为 exit 2（#316 AC3）
  - docs 目标（`docs/agents/*.md`）既能 `--outline` 也能按节切

真实契约取自 `docs/frame_contracts/`（只读）；边界用例在临时目录构造 mini 契约，
经 `--contract-dir` 专用参数驱动，不碰工作区。

跑法（被 preflight 的 unittest discovery 自动执行）：
    python3 -m unittest discover -s scripts/tests -k contract_slice
    python3 scripts/tests/test_contract_slice.py -v

退出码：0=通过，1=有失败。
"""

from __future__ import annotations

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "scripts" / "contract_slice.py"
CONTRACT_DIR = REPO_ROOT / "docs" / "frame_contracts"

# `MiniContractTest` 自己渲染 mini 契约文本，与 `contract_slice.py --selftest` 内部的渲染器
# 长得像但**不合并**（unit-test-standard §5.2「重复基建需有据」）：selftest 必须自造夹具才能
# 在无仓库时跑；本文件必须自造夹具才能断言「未知契约/围栏/重复」的出错路径。若让测试调用
# 被测模块的渲染器，夹具形状就由实现决定——实现把假标题也当节时，测试会跟着一起错而仍绿
# （独立 oracle 的价值正是「两边不一致时才红」）。两处各自演化是有意的。


def run_cli(*args: str) -> subprocess.CompletedProcess[str]:
    """进程级驱动切节工具；不经过 shell，取真实 stdout / stderr / 退出码。"""
    return subprocess.run(
        [sys.executable, str(SCRIPT), *args],
        capture_output=True,
        text=True,
        cwd=REPO_ROOT,
    )


class RealContractTest(unittest.TestCase):
    """真实契约：sysmon 已被本波读 13–29 次，它的节结构是工具的主要输入。"""

    def test_section_hit_starts_with_numbered_heading(self) -> None:
        result = run_cli("sysmon", "§2.9")
        self.assertEqual(result.returncode, 0, result.stderr)
        first = result.stdout.splitlines()[0]
        self.assertTrue(first.startswith("### 2.9 "), f"unexpected first line: {first!r}")
        self.assertIn("时钟偏差域", first)

    def test_default_stops_before_subsections(self) -> None:
        result = run_cli("sysmon", "§3.1")
        self.assertEqual(result.returncode, 0, result.stderr)
        # §3.1 的唯一子节是 §3.1.1，默认输出不能把它带进来。
        self.assertNotIn("### 3.1.1", result.stdout)

    def test_children_includes_subsections(self) -> None:
        result = run_cli("sysmon", "§3.1", "--children")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("### 3.1.1", result.stdout)

    def test_children_stops_at_next_non_child_section(self) -> None:
        # `--children` 只应把「编号子节」带进来，不能顺带吞掉后面的同级章（§3.2）。
        result = run_cli("sysmon", "§3.1", "--children")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn("### 3.2", result.stdout)

    def test_leaf_section_does_not_swallow_rest_of_document(self) -> None:
        # §2.9 是叶子节：默认与 --children 输出必须一致（不因后续章节而变长）。
        default = run_cli("sysmon", "§2.9")
        children = run_cli("sysmon", "§2.9", "--children")
        self.assertEqual(default.returncode, 0, default.stderr)
        self.assertEqual(default.stdout, children.stdout)
        self.assertNotIn("## 3.", default.stdout)

    def test_unknown_section_exits_1_with_candidates_on_stderr(self) -> None:
        result = run_cli("sysmon", "§7.9")
        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stdout, "", "diagnostics must not pollute stdout (head -3 pipes)")
        self.assertIn("unknown section", result.stderr)
        # 候选只在同一父级语义下给出：§7 存在但无编号子节 → 退到顶层候选。
        self.assertIn("§7", result.stderr)
        self.assertIn("top-level sections", result.stderr)

    def test_unknown_section_lists_siblings_of_its_parent(self) -> None:
        result = run_cli("sysmon", "§2.11")
        self.assertEqual(result.returncode, 1)
        self.assertIn("sections under §2", result.stderr)
        self.assertIn("§2.9", result.stderr)

    def test_unknown_contract_exits_1_with_candidates(self) -> None:
        result = run_cli("sysmo", "§2.9")
        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stdout, "")
        self.assertIn("unknown contract", result.stderr)
        self.assertIn("sysmon", result.stderr)

    def test_list_covers_every_contract_stem(self) -> None:
        result = run_cli("--list")
        self.assertEqual(result.returncode, 0, result.stderr)
        expected = {
            path.stem for path in CONTRACT_DIR.glob("*.md") if path.name != "README.md"
        }
        listed = {
            line.split(" (", 1)[0]
            for line in result.stdout.splitlines()
            if line and not line.startswith(" ")
        }
        self.assertEqual(listed, expected, "--list must not silently drop a contract")
        self.assertIn("§2.9 时钟偏差域（clock）", result.stdout)

    def test_selftest_passes(self) -> None:
        result = run_cli("--selftest")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("selftest OK", result.stdout)


class OutlineTest(unittest.TestCase):
    """`--outline`（#316 AC3）：一份契约/文档的节大纲，与 `--list` 单份块同一形状。

    为什么需要进程级：agent 的真实诉求是「给我 sysmon 这一份契约的节大纲」，
    旧入口只能 `--list`（全部契约）或 `grep '^## '` 试探。这里锁住目标寻址
    （契约 stem / docs stem / 仓库相对路径）、输出形状、以及与 `--list` 的互斥。
    """

    def test_outline_contract_stem(self) -> None:
        result = run_cli("--outline", "sysmon")
        self.assertEqual(result.returncode, 0, result.stderr)
        first = result.stdout.splitlines()[0]
        self.assertTrue(first.startswith("sysmon ("), f"unexpected first line: {first!r}")
        self.assertIn("§2.9 时钟偏差域（clock）", result.stdout)

    def test_outline_docs_path(self) -> None:
        result = run_cli("--outline", "docs/agents/ticket-closeout.md")
        self.assertEqual(result.returncode, 0, result.stderr)
        first = result.stdout.splitlines()[0]
        self.assertIn("docs/agents/ticket-closeout.md", first)
        self.assertIn("§7 清理、交还与报告", result.stdout)

    def test_outline_docs_stem_matches_docs_path(self) -> None:
        path_result = run_cli("--outline", "docs/agents/ticket-closeout.md")
        stem_result = run_cli("--outline", "ticket-closeout")
        self.assertEqual(stem_result.returncode, 0, stem_result.stderr)
        self.assertIn("§7 清理、交还与报告", stem_result.stdout)
        self.assertEqual(stem_result.stdout, path_result.stdout)

    def test_outline_unknown_stem_exits_1_with_candidates(self) -> None:
        result = run_cli("--outline", "ghost")
        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stdout, "", "diagnostics must not pollute stdout")
        self.assertIn("candidates=", result.stderr)

    def test_list_with_stem_points_at_outline(self) -> None:
        """`sysmon --list` 曾是最常见的误用（#316 AC3 要消灭它）：报错必须指路 `--outline`。"""
        result = run_cli("--list", "sysmon")
        self.assertEqual(result.returncode, 2)
        self.assertEqual(result.stdout, "")
        self.assertIn("--outline", result.stderr)

    def test_outline_and_list_are_mutually_exclusive(self) -> None:
        result = run_cli("--outline", "sysmon", "--list")
        self.assertEqual(result.returncode, 2)
        self.assertIn("mutually exclusive", result.stderr)

    def test_outline_rejects_positional_arguments(self) -> None:
        result = run_cli("--outline", "sysmon", "§2.9")
        self.assertEqual(result.returncode, 2)
        self.assertIn("positional", result.stderr)

    def test_outline_missing_docs_path_is_usage_error(self) -> None:
        result = run_cli("--outline", "docs/agents/not-a-real-file.md")
        self.assertEqual(result.returncode, 2)
        self.assertEqual(result.stdout, "")
        self.assertIn("not found", result.stderr)

    def test_docs_path_target_slices_by_number(self) -> None:
        """切片入口同样接受 docs 目标（#316 AC3 第 2 条）。"""
        result = run_cli("docs/agents/ticket-closeout.md", "§7")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("## 7. 清理、交还与报告", result.stdout)

    def test_docs_stem_target_slices_by_number(self) -> None:
        result = run_cli("ticket-closeout", "§1")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("## 1. 冻结提交与审查", result.stdout)

    def test_docs_target_without_numbered_headings_exits_1(self) -> None:
        """真实 docs 里没有编号标题的文档：exit 1 + 提示，不是静默空切片。

        反向断言刻意留在真实文档上（不搬去夹具）：它锁的是「**当前真实 docs** 的节结构
        形态」——`docs/components/` 多数文档没有编号标题，是数据现状而不是实现契约。
        夹具侧的同一判定见 `MiniDocsTargetTest.test_docs_without_numbered_headings_exits_1`。
        该文档将来加了编号标题，这个用例会红，正好提示把夹具断言改成正向用例。
        """
        result = run_cli("dzsysmon", "§1")
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("no numbered headings", result.stderr)


class MiniContractTest(unittest.TestCase):
    """边界用例：临时目录构造 mini 契约，经 `--contract-dir` 驱动（不碰真实契约）。"""

    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory(prefix="dz_contract_slice_test_")
        self.contract_dir = Path(self.tmp.name) / "docs" / "frame_contracts"
        self.contract_dir.mkdir(parents=True)

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def _write(self, stem: str, lines: list[str]) -> None:
        (self.contract_dir / f"{stem}.md").write_text("\n".join(lines) + "\n", encoding="utf-8")

    def _run(self, *args: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, str(SCRIPT), "--contract-dir", str(self.contract_dir), *args],
            capture_output=True,
            text=True,
            cwd=REPO_ROOT,
        )

    def test_backtick_fenced_fake_heading_is_not_a_section(self) -> None:
        self._write(
            "fenced",
            [
                "# 帧契约：围栏",
                "",
                "## 1. 真节",
                "",
                "real body",
                "",
                "```text",
                "## 2.9 假标题",
                "```",
                "",
                "## 3. 后续",
                "",
                "tail body",
            ],
        )
        result = self._run("fenced", "§2.9")
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("unknown section", result.stderr)

    def test_tilde_fenced_fake_heading_is_not_a_section(self) -> None:
        self._write(
            "tilde",
            [
                "# 帧契约：波浪围栏",
                "",
                "## 1. 真节",
                "",
                "real body",
                "",
                "~~~~",
                "### 2.9.2 假标题",
                "~~~~",
                "",
                "## 3. 后续",
            ],
        )
        result = self._run("tilde", "§2.9.2")
        self.assertEqual(result.returncode, 1, result.stdout)
        result_list = self._run("--list")
        self.assertNotIn("假标题", result_list.stdout)

    def test_duplicate_number_is_rejected_with_both_lines(self) -> None:
        self._write(
            "dup",
            [
                "# 帧契约：重复",
                "",
                "## 2.5 冲突层级",
                "",
                "top body",
                "",
                "### 2.5 重复子节",
                "",
                "child body",
            ],
        )
        result = self._run("dup", "§2.5")
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("duplicate section §2.5", result.stderr)
        self.assertIn("lines 3, 7", result.stderr)

    def test_parent_number_does_not_conflict_with_child_number(self) -> None:
        self._write(
            "nested",
            [
                "# 帧契约：层级",
                "",
                "## 2.5 顶层",
                "",
                "top body",
                "",
                "### 2.5.1 子节",
                "",
                "child body",
            ],
        )
        result = self._run("nested", "§2.5")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("top body", result.stdout)
        self.assertNotIn("child body", result.stdout)

    def test_selftest_rejects_contract_dir(self) -> None:
        """`--selftest` 自带临时契约，再给 `--contract-dir` 只能被静默忽略——明确拒绝。"""
        result = subprocess.run(
            [sys.executable, str(SCRIPT), "--selftest", "--contract-dir", str(self.contract_dir)],
            capture_output=True,
            text=True,
            cwd=REPO_ROOT,
        )
        self.assertEqual(result.returncode, 2, result.stdout)
        self.assertIn("--contract-dir", result.stderr)

    def test_missing_contract_dir_is_env_error(self) -> None:
        """契约目录不存在是环境错误（exit 2），不是「未知契约」（exit 1）。"""
        result = subprocess.run(
            [sys.executable, str(SCRIPT), "--contract-dir", str(self.contract_dir / "nope"),
             "ghost", "§1"],
            capture_output=True,
            text=True,
            cwd=REPO_ROOT,
        )
        self.assertEqual(result.returncode, 2, result.stdout)
        self.assertIn("contract directory not found", result.stderr)

    def test_exit_codes_separate_unknown_from_usage(self) -> None:
        # 1 = 未知契约/未知节（含候选）；2 = 参数错误。
        self.assertEqual(self._run("ghost", "§1").returncode, 1)
        self.assertEqual(self._run("ghost", "§x").returncode, 2)
        self.assertEqual(self._run("ghost").returncode, 2)
        self.assertEqual(self._run("--list", "ghost").returncode, 2)


class MiniDocsTargetTest(unittest.TestCase):
    """docs 目标寻址的边界：临时目录造 `docs/agents` + `docs/components`，经 `--docs-dir`。

    真实 docs/ 只用于「已知存在的文档」正向用例（只读）；歧义、无编号标题这类边界
    必须在夹具里造，否则真实 doc 一改名用例就红。
    """

    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory(prefix="dz_contract_slice_docs_")
        self.docs_dir = Path(self.tmp.name) / "docs"
        self.contract_dir = self.docs_dir / "frame_contracts"
        self.contract_dir.mkdir(parents=True)
        agents = self.docs_dir / "agents"
        components = self.docs_dir / "components"
        agents.mkdir(parents=True)
        components.mkdir()
        agents.joinpath("numbered.md").write_text(
            "\n".join(["# 文档", "", "## 1. 起点", "", "start body", "",
                       "### 1.1 子节", "", "sub body", "", "## 7. 收尾", "", "tail body", ""]),
            encoding="utf-8",
        )
        agents.joinpath("plain.md").write_text(
            "\n".join(["# 无编号", "", "## 标题没编号", "", "body", ""]), encoding="utf-8"
        )
        for directory in (agents, components):
            directory.joinpath("twin.md").write_text("## 1. 同名的节\n\nbody\n", encoding="utf-8")
        self.contract_dir.joinpath("sample.md").write_text(
            "## 1. 契约节\n\ncontract body\n", encoding="utf-8"
        )
        # 同名 docs 文档：用来验证「契约 stem 命中优先」（#316 AC3 的解析顺序）。
        agents.joinpath("sample.md").write_text(
            "## 1. 文档节\n\ndocs body\n", encoding="utf-8"
        )

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def _run(self, *args: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, str(SCRIPT),
             "--contract-dir", str(self.contract_dir),
             "--docs-dir", str(self.docs_dir), *args],
            capture_output=True, text=True, cwd=REPO_ROOT,
        )

    def test_outline_docs_stem_from_fixture(self) -> None:
        result = self._run("--outline", "numbered")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(result.stdout.splitlines()[0].startswith("numbered ("))
        self.assertIn("§7 收尾", result.stdout)

    def test_outline_docs_relative_path_from_fixture(self) -> None:
        result = self._run("--outline", "docs/agents/numbered.md")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("§1 起点", result.stdout)

    def test_ambiguous_docs_stem_lists_candidates(self) -> None:
        """同名 stem 在 agents 与 components 各一份 → exit 1 并列出两份，不静默取第一个。"""
        result = self._run("--outline", "twin")
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("ambiguous", result.stderr)
        self.assertIn("docs/agents/twin.md", result.stderr)
        self.assertIn("docs/components/twin.md", result.stderr)

    def test_docs_slice_from_fixture(self) -> None:
        result = self._run("numbered", "§1")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("start body", result.stdout)
        self.assertNotIn("sub body", result.stdout)  # 默认不含子节
        children = self._run("numbered", "§1", "--children")
        self.assertIn("sub body", children.stdout)

    def test_docs_without_numbered_headings_exits_1(self) -> None:
        result = self._run("plain", "§1")
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("no numbered headings", result.stderr)

    def test_path_target_outside_repo_exits_2(self) -> None:
        absolute = str(Path(self.tmp.name) / "docs" / "agents" / "numbered.md")
        result = self._run("--outline", absolute)
        self.assertEqual(result.returncode, 2, result.stdout)
        self.assertIn("repo-relative", result.stderr)

    def test_contract_stem_wins_over_docs_stem(self) -> None:
        """契约 stem 命中优先（#316 AC3 解析顺序）：sample 在契约与 docs/agents 都有同名文件。"""
        result = self._run("--outline", "sample")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("frame_contracts/sample.md", result.stdout)
        self.assertNotIn("docs/agents/sample.md", result.stdout)


if __name__ == "__main__":
    unittest.main()
