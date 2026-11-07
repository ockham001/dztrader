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


def _event(kind: str, time_ms: int, **data) -> dict:
    return {"type": kind, "time": time_ms, "data": data}


def _tool_call(call_id: str, name: str, time_ms: int, turn: int, arguments: dict | None = None) -> dict:
    return _event(
        "tool/call",
        time_ms,
        name=name,
        callId=call_id,
        turn=turn,
        arguments=json.dumps(arguments or {}),
    )


def _tool_result(call_id: str, time_ms: int, turn: int) -> dict:
    return _event("tool/result", time_ms, message={"toolCallId": call_id, "content": []})


def _assistant(turn: int, step: int, time_ms: int) -> dict:
    return _event("assistant/message", time_ms, turn=turn, step=step, message={"content": []})


class DecomposeTest(unittest.TestCase):
    """`--decompose` 的三态：单 turn / 多 turn 含轮间 idle / 含 ask 阻塞（#309 AC9）。

    口径锁死 `turn_wall = model_gen + tool_exec + idle`。ask 与等待型工具**是
    tool_exec 的子集**：它们只单列，绝不再加进守恒等式——实现若把它们重复计入总墙钟，
    下面的守恒断言立刻红。
    """

    @staticmethod
    def _decompose(events: list[dict]) -> list[str]:
        return retro_extract.decompose_section(events)

    @staticmethod
    def _turn_line(lines: list[str], turn: int) -> str:
        for line in lines:
            if line.startswith(f"- turn {turn}:"):
                return line
        raise AssertionError(f"turn {turn} missing from decompose output: {lines}")

    def test_single_turn_splits_model_and_tool(self) -> None:
        events = [
            _event("turn/start", 0, turn=1),
            _event("step/start", 0, turn=1, step=1),
            _assistant(1, 1, 2000),  # 2s 模型生成
            _tool_call("c1", "bash", 2000, 1),  # 2s → 4s 工具执行
            _tool_result("c1", 4000, 1),
            _event("step/end", 4000, turn=1, step=1),
            _event("turn/end", 6000, turn=1),
            {"type": "session", "data": {}},
        ]
        lines = self._decompose(events)
        turn1 = self._turn_line(lines, 1)
        self.assertIn("wall=6s", turn1)
        self.assertIn("model=2s(33%)", turn1)
        self.assertIn("tool=2s(33%)", turn1)
        self.assertIn("idle=2s(33%)", turn1)
        self.assertTrue(any("closed_turns=1" in line and "wall=6s" in line for line in lines))

    def test_idle_between_turns_is_not_charged_to_either_turn(self) -> None:
        # turn1 结束后到 turn2 开始前有 5s 轮间空档（人读结果/等待），不该进任何 turn。
        events = [
            _event("turn/start", 0, turn=1),
            _event("step/start", 0, turn=1, step=1),
            _assistant(1, 1, 1000),
            _event("step/end", 1000, turn=1, step=1),
            _event("turn/end", 2000, turn=1),
            _event("turn/start", 7000, turn=2),
            _event("step/start", 7000, turn=2, step=1),
            _assistant(2, 1, 9000),
            _event("step/end", 9000, turn=2, step=1),
            _event("turn/end", 10000, turn=2),
        ]
        lines = self._decompose(events)
        turn1 = self._turn_line(lines, 1)
        turn2 = self._turn_line(lines, 2)
        self.assertIn("wall=2s", turn1)
        self.assertIn("model=1s(50%)", turn1)
        self.assertIn("wall=3s", turn2)
        self.assertIn("model=2s(67%)", turn2)
        self.assertTrue(any("closed_turns=2" in line and "wall=5s" in line for line in lines))

    def test_ask_block_is_a_subset_not_added_to_tool_wall(self) -> None:
        events = [
            _event("turn/start", 0, turn=1),
            _event("step/start", 0, turn=1, step=1),
            _assistant(1, 1, 1000),  # 1s 模型生成
            _tool_call("c1", "ask_user_question", 1000, 1, {"question": "merge?"}),
            _tool_result("c1", 9000, 1),  # 8s 人工应答 = ask 阻塞，已在工具区间内
            _tool_call("c2", "bash", 9000, 1),
            _tool_result("c2", 10000, 1),  # 1s 工具执行
            _event("step/end", 10000, turn=1, step=1),
            _event("turn/end", 12000, turn=1),
        ]
        lines = self._decompose(events)
        turn1 = self._turn_line(lines, 1)
        # tool_exec 是 1s–10s 的区间并集 = 9s（含 ask）；ask 只单列 8s，绝不再加。
        self.assertIn("wall=12s", turn1)
        self.assertIn("model=1s(8%)", turn1)
        self.assertIn("tool=9s(75%)", turn1)
        self.assertIn("idle=2s(17%)", turn1)
        self.assertIn("ask=8s", turn1)
        ask_section = "\n".join(lines).split("## ask blocks", 1)[1]
        self.assertIn("ask_user_question: calls=1 total=8s", ask_section)
        # 守恒：若实现把 ask 也加进 tool，turn 行里的 tool 会变成 17s 之类的错值。
        self.assertNotIn("tool=17s", turn1)

    def test_wait_type_tool_listed_with_calls_and_total(self) -> None:
        events = [
            _event("turn/start", 0, turn=1),
            _event("step/start", 0, turn=1, step=1),
            _assistant(1, 1, 1000),
            _tool_call("c1", "job_output", 1000, 1, {"job_id": "j1", "wait": True}),
            _tool_result("c1", 7000, 1),
            _tool_call("c2", "job_output", 7000, 1, {"job_id": "j2", "wait": False}),
            _tool_result("c2", 8000, 1),  # wait=false 是即时取结果，不算等待型
            _tool_call("c3", "wait_agent", 8000, 1, {}),
            _tool_result("c3", 10000, 1),
            _event("step/end", 10000, turn=1, step=1),
            _event("turn/end", 11000, turn=1),
        ]
        lines = self._decompose(events)
        wait_section = "\n".join(lines).split("## wait-type tools", 1)[1].split("\n## ", 1)[0]
        self.assertIn("job_output: calls=1 total=6s", wait_section)
        self.assertIn("wait_agent: calls=1 total=2s", wait_section)
        self.assertIn("total: calls=2 total=8s", wait_section)
        self.assertNotIn("calls=3", wait_section)

    def test_parallel_calls_are_counted_once(self) -> None:
        # 两个 bash 同时跑 2s：区间并集 2s，不是相加的 4s。
        events = [
            _event("turn/start", 0, turn=1),
            _event("step/start", 0, turn=1, step=1),
            _assistant(1, 1, 1000),
            _tool_call("c1", "bash", 1000, 1),
            _tool_call("c2", "bash", 1000, 1),
            _tool_result("c1", 3000, 1),
            _tool_result("c2", 3000, 1),
            _event("step/end", 3000, turn=1, step=1),
            _event("turn/end", 4000, turn=1),
        ]
        turn1 = self._turn_line(self._decompose(events), 1)
        self.assertIn("tool=2s(50%)", turn1)

    def test_open_turn_is_listed_but_not_summed(self) -> None:
        events = [
            _event("turn/start", 0, turn=1),
            _event("step/start", 0, turn=1, step=1),
            _assistant(1, 1, 1000),
            _event("step/end", 1000, turn=1, step=1),
            _event("turn/end", 2000, turn=1),
            _event("turn/start", 3000, turn=2),  # 当前会话尾部：没有 turn/end
            _event("step/start", 3000, turn=2, step=1),
            _assistant(2, 1, 4000),
        ]
        lines = self._decompose(events)
        session_line = next(line for line in lines if "closed_turns" in line)
        self.assertIn("closed_turns=1", session_line)
        self.assertIn("open_turns=1", session_line)
        self.assertIn("wall=2s", session_line)  # 只合计已闭合的 turn 1
        open_line = self._turn_line(lines, 2)
        self.assertIn("open turn", open_line)
        self.assertIn("model=1s", open_line)

    def test_duplicate_results_do_not_inflate_tool_time(self) -> None:
        # 真实会话里同一 callId 会出现多条 result（重投/重放）。取最后一条会把区间拉到
        # 会话尾部——#297 实测把「工具执行 40.3 min」虚报成 106 min。只认第一条 result。
        events = [
            _event("turn/start", 0, turn=1),
            _event("step/start", 0, turn=1, step=1),
            _assistant(1, 1, 1000),
            _tool_call("c1", "bash", 1000, 1),
            _tool_result("c1", 2000, 1),  # 真结果：1s
            _tool_result("c1", 60000, 1),  # 重投：若取这条，tool 会变成 59s
            _event("step/end", 2000, turn=1, step=1),
            _event("turn/end", 3000, turn=1),
        ]
        turn1 = self._turn_line(self._decompose(events), 1)
        self.assertIn("tool=1s(33%)", turn1)
        self.assertNotIn("tool=59s", turn1)

    def test_decompose_flag_appends_section_without_touching_default_output(self) -> None:
        if shutil.which("zstd") is None:
            self.skipTest("zstd CLI not installed; the end-to-end digest test needs it")
        events = [
            {"type": "session", "data": {}},
            _event("turn/start", 0, turn=1),
            _event("step/start", 0, turn=1, step=1),
            _assistant(1, 1, 2000),
            _tool_call("c1", "bash", 2000, 1),
            _tool_result("c1", 4000, 1),
            _event("step/end", 4000, turn=1, step=1),
            _event("turn/end", 6000, turn=1),
        ]
        tmp = Path(tempfile.mkdtemp(prefix="dz_retro_decompose_"))
        try:
            raw = tmp / "session.v4.jsonl"
            raw.write_text("\n".join(json.dumps(e) for e in events) + "\n", encoding="utf-8")
            packed = tmp / "session.v4.jsonl.zstd"
            subprocess.run(
                ["zstd", "-q", "-f", str(raw), "-o", str(packed)], check=True, capture_output=True
            )
            plain = retro_extract.extract(packed)
            decomposed = retro_extract.extract(packed, decompose=True)
            # 默认输出一字不改；--decompose 是纯追加。
            self.assertTrue(decomposed.startswith(plain))
            self.assertNotIn("## decompose", plain)
            self.assertIn("## decompose (turn wall clock)", decomposed)
        finally:
            shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    unittest.main()
