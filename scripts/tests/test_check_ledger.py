#!/usr/bin/env python3
"""test_check_ledger.py — R4 提示必须带恢复路径（#424 AC1）

为什么需要：`check_ledger.py` 的 R4 对「open + `ready-for-agent` 但正文缺非空
`## 拍板记录`」报警，级别是 **warning**。#411 的废弃会话把这条 warning 读成开工前置：
开工 7.6 分钟发起合并授权提问、阻塞 3,280s（54m40s），用户跳过（空选项）后整段会话作废。
根因不是「报警太多」，而是提示只说「补段」、没说「照样继续实现」。所以这里锁三件事：

1. 提示文本含恢复路径关键字（继续实现 / PR 创建后 / 禁止开工即问）；
2. 这段提示真的被 `main()` 打印（只判常量存在会让「补段」提示盖回来）；
3. `--selftest` 自己也会断言同一组关键字（离线、不联网）。

跑法（被 preflight 的 unittest discovery 自动执行）：
    python3 -m unittest discover -s scripts/tests -k check_ledger
    python3 scripts/tests/test_check_ledger.py -v

退出码：0=通过，1=有失败。
"""

from __future__ import annotations

import ast
import subprocess
import sys
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "scripts" / "check_ledger.py"
sys.path.insert(0, str(REPO_ROOT / "scripts"))

import check_ledger  # noqa: E402

RECOVERY_KEYWORDS = ("继续实现", "PR 创建后", "禁止开工即问")


class R4RecoveryHintTest(unittest.TestCase):
    def test_hint_text_carries_recovery_path(self) -> None:
        text = "\n".join(check_ledger.r4_hint_lines())
        for keyword in RECOVERY_KEYWORDS:
            self.assertIn(keyword, text, "R4 提示缺少恢复路径关键字（#424 AC1）")

    def test_hint_is_printed_by_main(self) -> None:
        # #424 审查 finding：只数 `r4_hint_lines` 出现次数会被 def / selftest 的引用蒙对
        # （删掉 main 的打印仍满足阈值）。用 AST 直接断言 `main()` 里调用它。
        tree = ast.parse(SCRIPT.read_text(encoding="utf-8"))
        main_fn = next(
            (
                node
                for node in ast.walk(tree)
                if isinstance(node, ast.FunctionDef) and node.name == "main"
            ),
            None,
        )
        self.assertIsNotNone(main_fn, "check_ledger.py 缺少 main()")
        calls = [
            node
            for node in ast.walk(main_fn)
            if isinstance(node, ast.Call)
            and isinstance(node.func, ast.Name)
            and node.func.id == "r4_hint_lines"
        ]
        self.assertTrue(
            calls,
            "main() 没有打印 r4_hint_lines()：R4 报告会只剩「补段」提示",
        )

    def test_selftest_asserts_the_same_keywords(self) -> None:
        result = subprocess.run(
            [sys.executable, str(SCRIPT), "--selftest"],
            capture_output=True,
            text=True,
            cwd=str(REPO_ROOT),
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("恢复路径", result.stdout)
        for keyword in RECOVERY_KEYWORDS:
            self.assertIn(keyword, result.stdout)


if __name__ == "__main__":
    unittest.main()
