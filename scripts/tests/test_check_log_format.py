#!/usr/bin/env python3
"""test_check_log_format.py — `scripts/check_log_format.py` 的调用形态用例（#506）

为什么需要：#493 把登录链失败助手的日志 prose 从 `SPDLOG_*` 字面量改成
`push_notification(notif, message, prose)` 的**第三实参**，而检查器只扫 `CALL_RES`
（spdlog / LastError::set / throw std::* / std::cerr）——R3（禁中文）与 R5（小写开头、
无句号）对这三句不再静态生效。其中结算确认 / 合约加载 / 连接超时三处的 prose 落在
**跨行调用的下一行**（第二个实参是嵌套调用，形状见下），旧提取逻辑只看调用起始行，
连「同行有字面量」这条都抓不到。

本文件锁三件事：
1. `push_notification` 的 prose 字面量进了扫描面（同行 + 跨行两种形态）；
2. 跨行 lookahead 有边界：只在**实参表区间内**取字面量 —— 调用在本行闭合、或续行闭合后
   同一行还有别的调用字面量时，都不能把别人当 prose；超出扫描窗（`MAX_CALL_LOOKAHEAD_LINES`）
   则明确漏检。否则「覆盖扩了」会变成「误报多了」；
3. 违规行号指向 prose 所在行（跨行时是下一行），NOLINT 写在该行或调用起始行都能豁免。

夹具是临时目录里的 mini `.cpp`，进程级跑真实脚本（与 test_ci_scope / test_push_ticket
同一驱动方式），断言退出码 / 行号 / 规则名；不碰真实工作区、不联网。

反向验证：把 `OTHER_CALL_RES` 里的 `push_notification` 删掉，规则组与跨行组立刻红（合规
正样本组本就不报）；把字面量提取改成「调用起始行之后任意行的首个字面量」，则
`test_closed_call_does_not_borrow_next_line_literal` 与
`test_closed_continuation_does_not_borrow_following_literal` 红。

跑法（被 preflight 的 unittest discovery 自动执行）：
    python3 -m unittest discover -s scripts/tests -k check_log_format
    python3 scripts/tests/test_check_log_format.py -v

退出码：0=通过，1=有失败。
"""

from __future__ import annotations

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
CHECKER = REPO_ROOT / "scripts" / "check_log_format.py"

# 每个用例写一个独立 mini .cpp；行号断言据此固定（fixture 首行是 1）。
SAMPLES = {
    # 同行形态：prose 是第三实参，且是本行首个字面量
    "same_line_ok": """\
void f() {
    push_notification(notif, auth_failed_message(code, reason), "td auth failed notify failed");
}
""",
    "same_line_cn": """\
void f() {
    push_notification(notif, auth_failed_message(code, reason), "认证失败通知失败");
}
""",
    "same_line_upper": """\
void f() {
    push_notification(notif, auth_failed_message(code, reason), "Td auth failed notify failed");
}
""",
    "same_line_period": """\
void f() {
    push_notification(notif, auth_failed_message(code, reason), "td auth failed notify failed.");
}
""",
    # 跨行形态（#493 实际形状）：第二个实参是嵌套调用，prose 在下一行
    "multiline_ok": """\
void f() {
    push_notification(notif, connect_timeout_message(std::string(kConnectTimeoutReason)),
                      "td connect timeout notify failed");
}
""",
    "multiline_bad": """\
void f() {
    push_notification(notif, connect_timeout_message(std::string(kConnectTimeoutReason)),
                      "Td connect timeout notify failed.");
}
""",
    # 调用在本行闭合且无字面量（prose 是变量）：不得把下一行别的字面量当自己的实参
    "closed_call_variable_prose": """\
void f() {
    push_notification(notif, message, prose);
    other_call("Bad next line");
}
""",
    # 续行先闭合本调用，闭合后同一行还有别的调用字面量：不能把 other_call 的实参当 prose
    "continuation_close_then_other": """\
void f() {
    push_notification(notif, message,
                      prose); other_call("Bad next line");
}
""",
    # helper 定义（形参表跨两行，函数体字面量在闭合之后）：定义不是调用点 ——
    # 提取窗若越过配对 ')' 就会把函数体字面量误当 prose
    "definition": """\
void AccountSession::push_notification(const std::optional<TdNotification>& notif,
                                       std::string_view prose) {
    log_with_literal("Bad body line");
}
""",
    # NOLINT：跨行违规的豁免写 prose 行 / 调用起始行都生效
    "multiline_bad_nolint_prose_line": """\
void f() {
    push_notification(notif, connect_timeout_message(std::string(kConnectTimeoutReason)),
                      "Td connect timeout notify failed.");  // NOLINT(log-format)
}
""",
    "multiline_bad_nolint_call_line": """\
void f() {
    push_notification(notif, connect_timeout_message(std::string(kConnectTimeoutReason)),  // NOLINT(log-format)
                      "Td connect timeout notify failed.");
}
""",
    # 扫描窗边界：prose 在第 2 个续行（window 内）被覆盖；第 3 个续行在窗外（已知漏检）
    "multiline_offset2_bad": """\
void f() {
    push_notification(notif,
                      connect_timeout_message(std::string(kConnectTimeoutReason)),
                      "Td connect timeout notify failed.");
}
""",
    "multiline_offset3_unchecked": """\
void f() {
    push_notification(notif,
                      connect_timeout_message(std::string(kConnectTimeoutReason)),
                      std::string(kSecondReason),
                      "Td connect timeout notify failed.");
}
""",
    # 既有形态回归：同行 spdlog + 跨行 throw（跨行 lookahead 的另一条既有收益）
    "existing_forms": """\
void f() {
    SPDLOG_ERROR("认证失败");
    throw std::invalid_argument(
        "Bad cross line");
}
""",
}


def run_checker(source: str) -> subprocess.CompletedProcess[str]:
    """把 source 写成临时目录里的 mini.cpp，进程级跑真实检查器。"""
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "mini.cpp"
        path.write_text(source, encoding="utf-8")
        return subprocess.run(
            [sys.executable, str(CHECKER), str(path)],
            capture_output=True,
            text=True,
            cwd=str(REPO_ROOT),
        )


class PushNotificationProseTest(unittest.TestCase):
    def setUp(self) -> None:
        if not CHECKER.is_file():
            self.fail(f"check_log_format.py not found: {CHECKER}")

    def test_same_line_compliant_prose_passes(self) -> None:
        result = run_checker(SAMPLES["same_line_ok"])
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("no log-format violations", result.stderr)

    def test_same_line_prose_rules_still_apply(self) -> None:
        cases = (
            ("same_line_cn", "log-format/non-english"),
            ("same_line_upper", "log-format/prose-case"),
            ("same_line_period", "log-format/prose-period"),
        )
        for name, rule in cases:
            with self.subTest(name=name):
                result = run_checker(SAMPLES[name])
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertIn("mini.cpp:2:", result.stdout)
                self.assertIn(rule, result.stdout)

    def test_multiline_compliant_prose_passes(self) -> None:
        result = run_checker(SAMPLES["multiline_ok"])
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("no log-format violations", result.stderr)

    def test_multiline_prose_violation_reports_prose_line(self) -> None:
        result = run_checker(SAMPLES["multiline_bad"])
        self.assertEqual(result.returncode, 1, result.stderr)
        # prose 在第三行（调用起始行是 2）：行号指向 prose 行，违反 R5 两条规则
        self.assertIn("mini.cpp:3:", result.stdout)
        self.assertIn("log-format/prose-case", result.stdout)
        self.assertIn("log-format/prose-period", result.stdout)
        self.assertNotIn("mini.cpp:2:", result.stdout)

    def test_closed_call_does_not_borrow_next_line_literal(self) -> None:
        result = run_checker(SAMPLES["closed_call_variable_prose"])
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("no log-format violations", result.stderr)

    def test_closed_continuation_does_not_borrow_following_literal(self) -> None:
        result = run_checker(SAMPLES["continuation_close_then_other"])
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("no log-format violations", result.stderr)

    def test_multiline_nolint_on_prose_line_exempts(self) -> None:
        result = run_checker(SAMPLES["multiline_bad_nolint_prose_line"])
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("no log-format violations", result.stderr)

    def test_multiline_nolint_on_call_line_exempts(self) -> None:
        result = run_checker(SAMPLES["multiline_bad_nolint_call_line"])
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("no log-format violations", result.stderr)

    def test_multiline_prose_within_lookahead_window(self) -> None:
        # 上限边界（正）：prose 在第 2 个续行 = 扫描窗最后一行的位置，仍被覆盖
        result = run_checker(SAMPLES["multiline_offset2_bad"])
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("mini.cpp:4:", result.stdout)
        self.assertIn("log-format/prose-case", result.stdout)

    def test_multiline_prose_beyond_lookahead_window_is_known_gap(self) -> None:
        # 上限边界（已知漏检）：第 3 个续行在 MAX_CALL_LOOKAHEAD_LINES 窗外 —— 钉住边界，
        # 扩窗时同步改本用例与脚本注释
        result = run_checker(SAMPLES["multiline_offset3_unchecked"])
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("no log-format violations", result.stderr)

    def test_helper_definition_is_not_a_call_site(self) -> None:
        result = run_checker(SAMPLES["definition"])
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("no log-format violations", result.stderr)

    def test_existing_call_forms_unaffected(self) -> None:
        result = run_checker(SAMPLES["existing_forms"])
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("mini.cpp:2:", result.stdout)  # spdlog 同行
        self.assertIn("log-format/non-english", result.stdout)
        self.assertIn("mini.cpp:4:", result.stdout)  # throw 跨行
        self.assertIn("log-format/prose-case", result.stdout)


if __name__ == "__main__":
    unittest.main(verbosity=2)
