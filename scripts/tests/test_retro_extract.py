#!/usr/bin/env python3
"""test_retro_extract.py — `scripts/retro_extract.py` 的自掩码失败采集（#289 AC8）

为什么需要：`retro_extract.py` 原先只收集 `isError == true` 的工具结果，于是
`cmd; echo rc=$?` 这类「退出码被吞、宿主看不到错误」的实红进不了 digest（#284 实测 2 处）。
采集逻辑补上了，但它是**逐行正则**：模式写宽了会把 `read` / `grep` 出来的日志与源码当成
失败（digest 全是误报），写窄了又会漏掉真红。本文件锁三件事：

1. 哪些行算失败证据（`rc=1` 算，`rc=0` 不算，`FAILED`/`FAIL:`/行首 `error:` 算）；
2. 哪些工具算命令类（`bash` 算；`read` / `grep` 不算——它们的输出是**内容**）；
3. digest 里确实出现 `## masked failures` 小节且条目带命中模式。

跑法（被 preflight 的 unittest discovery 自动执行）：
    python3 -m unittest discover -s scripts/tests -k retro_extract
    python3 scripts/tests/test_retro_extract.py -v

退出码：0=通过，1=有失败。
"""

from __future__ import annotations

import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "scripts"))

import retro_extract  # noqa: E402


class MaskedFailureLineTest(unittest.TestCase):
    """逐行判定：模式边界是实测出来的，别让「顺手放宽」把 digest 变成误报集。"""

    def test_swallowed_exit_code_is_caught(self) -> None:
        self.assertEqual(
            retro_extract.masked_failure_line("some output\nrc=1\n")[0], "rc!=0"
        )

    def test_rc_zero_is_not_a_failure(self) -> None:
        self.assertIsNone(retro_extract.masked_failure_line("done\nrc=0\n"))

    def test_failed_and_fail_prefixes(self) -> None:
        self.assertEqual(
            retro_extract.masked_failure_line("FAILED tests/x.py::test_a\n")[0], "FAILED"
        )
        self.assertEqual(retro_extract.masked_failure_line("FAIL: 2 checks\n")[0], "FAIL:")

    def test_tool_printed_error_line(self) -> None:
        self.assertEqual(retro_extract.masked_failure_line("  error: boom\n")[0], "error:")

    def test_clean_output_is_none(self) -> None:
        self.assertIsNone(retro_extract.masked_failure_line("all good\nmode=fast\n"))

    def test_error_word_inside_prose_is_not_a_line(self) -> None:
        # `error:` 只认行首（工具自己的报错行），散文里出现不算——否则每条 diff 都中招。
        self.assertIsNone(retro_extract.masked_failure_line("see error: in the log above\n"))


class ExtractMaskedFailuresTest(unittest.TestCase):
    """端到端：只有命令类工具的失败证据进 digest。"""

    def setUp(self) -> None:
        if shutil.which("zstd") is None:
            self.skipTest("zstd CLI not installed; the end-to-end digest test needs it")
        self.tmp = Path(tempfile.mkdtemp(prefix="dz_retro_extract_test_"))

    def tearDown(self) -> None:
        shutil.rmtree(self.tmp, ignore_errors=True)

    def _write_session(self, events: list[dict]) -> Path:
        raw = self.tmp / "session.v4.jsonl"
        raw.write_text("\n".join(json.dumps(event) for event in events) + "\n", encoding="utf-8")
        packed = self.tmp / "session.v4.jsonl.zstd"
        subprocess.run(
            ["zstd", "-q", "-f", str(raw), "-o", str(packed)],
            check=True,
            capture_output=True,
        )
        return packed

    @staticmethod
    def _call(call_id: str, name: str, command: str) -> dict:
        return {
            "type": "tool/call",
            "time": 1000,
            "data": {"name": name, "callId": call_id, "arguments": json.dumps({"command": command})},
        }

    @staticmethod
    def _result(call_id: str, text: str) -> dict:
        return {
            "type": "tool/result",
            "time": 1100,
            "data": {
                "message": {
                    "isError": False,
                    "toolCallId": call_id,
                    "content": [{"type": "text", "text": text}],
                }
            },
        }

    def test_bash_failure_captured_and_read_content_ignored(self) -> None:
        session = self._write_session(
            [
                {"type": "session", "data": {}},
                self._call("c1", "bash", "false; echo rc=$?"),
                self._result("c1", "noise\nrc=1\n"),
                self._call("c2", "read", "tests/x.cpp"),
                self._result("c2", "EXPECT_EQ(FAILED, actual);\n"),  # 源码内容，不是失败
            ]
        )
        digest = retro_extract.extract(session)
        self.assertIn("## masked failures", digest)
        masked = digest.split("## masked failures", 1)[1].split("\n## ", 1)[0]
        self.assertIn("rc=$?", masked)
        self.assertIn("match=rc!=0", masked)
        # `read` 输出里的 FAILED 不能进 digest（工具类型过滤）。
        self.assertNotIn("tests/x.cpp", masked)

    def test_is_error_results_are_not_double_counted(self) -> None:
        events = [
            {"type": "session", "data": {}},
            self._call("c1", "bash", "exit 2"),
            {
                "type": "tool/result",
                "time": 1100,
                "data": {
                    "message": {
                        "isError": True,
                        "toolCallId": "c1",
                        "content": [{"type": "text", "text": "rc=2\n"}],
                    }
                },
            },
        ]
        digest = retro_extract.extract(self._write_session(events))
        masked = digest.split("## masked failures", 1)[1].split("\n## ", 1)[0]
        self.assertNotIn("match=rc!=0", masked)


if __name__ == "__main__":
    unittest.main()
