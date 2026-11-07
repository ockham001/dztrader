#!/usr/bin/env python3
"""test_sysmon_metric_keys.py — `scripts/check_sysmon_metric_keys.py` 的进程级用例（#309 AC3）

为什么需要：这份 15 键集合在 4 处有副本（`sysmon.h` 的 `SYSMON_METRIC_KEYS`、契约
sysmon §2.5 的键列表行、TS 夹具、webui 共享测试夹具），外加 `snapshot_builder.cpp` 的
15 个显式分支。集合当前一致但无检查器绑定（#295 审查 finding 5 的 13/15 vs 15/15 靠人眼
发现）。本文件锁住检查器的**反例**：四源少键 / 多键 / 顺序漂移、header 声明长度漂移、
builder 缺分支 / 丢兜底、以及解析失败必须 exit 2（不静默跳过）。

驱动方式：与 `test_ci_scope.py` / `test_push_ticket.py` 一致——**进程级**跑真实脚本，
在临时目录里用 `--root` 构造 mini 源树，断言 stdout/stderr 与退出码；不碰真实工作区、
不联网。真实树的正例也跑一次（`--root` 默认 = 本仓），这样「检查器对真实仓库是绿的」
本身也被断言覆盖。

反向验证：把 `compare()` 的四个 source 比较改成只看契约、或把 builder 分支覆盖检查删掉，
`test_missing_*` / `test_builder_misses_branch` 立刻红。

跑法：
    python3 -m unittest discover -s scripts/tests -k metric_keys
    python3 scripts/tests/test_sysmon_metric_keys.py -v

退出码：0=通过，1=有失败。
"""

from __future__ import annotations

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
CHECKER = REPO_ROOT / "scripts" / "check_sysmon_metric_keys.py"

H_HEADER = "libs/platform/include/dztrader/platform/sysmon.h"
CONTRACT = "docs/frame_contracts/sysmon.md"
TS_FIXTURE = "apps/webui/frontend/src/test-utils/sysmonSnapshot.ts"
TEST_FIXTURE = "apps/webui/tests/sysmon_snapshot_fixture.h"
BUILDER = "apps/sysmon/snapshot_builder.cpp"

BASE_KEYS = ["system", "cpu.total", "cpu.cores", "memory", "swap"]


def const_name(key: str) -> str:
    return key.upper().replace(".", "_")


def header_text(keys: list[str], declared: int | None = None) -> str:
    consts = "\n".join(
        f'inline constexpr const char* SYSMON_METRIC_{const_name(k)} = "{k}";' for k in keys)
    entries = "\n".join(f"    SYSMON_METRIC_{const_name(k)}," for k in keys)
    size = len(keys) if declared is None else declared
    return (f"// sample\n{consts}\n\n"
            f"inline constexpr std::array<const char*, {size}> SYSMON_METRIC_KEYS = {{\n"
            f"{entries}\n}};\n")


def contract_text(keys: list[str]) -> str:
    return ("# 契约\n\n## 2. 快照\n\n### 2.5 availability\n\n"
            "`metrics` 键集合（固定，发送方每帧必须覆盖全部键）：\n\n"
            + "、".join(f"`{k}`" for k in keys)
            + "。\n\n`reason` 为自由文案。\n")


def ts_text(keys: list[str]) -> str:
    entries = "\n".join(f"  '{k}'," for k in keys)
    return "// 夹具\nexport const SYSMON_METRIC_KEYS = [\n" + entries + "\n] as const\n"


def cpp_fixture_text(keys: list[str]) -> str:
    entries = "".join(f'{{"{k}", {{"available", true}}}}, ' for k in keys).rstrip(", ")
    return ("#include <nlohmann/json.hpp>\n\ninline nlohmann::json f() {\n    return {\n"
            '        {"availability",\n         {{"metrics",\n           {'
            + entries
            + "}}}},\n    };\n}\n")


def builder_text(keys: list[str], drop: str | None = None, fallback: bool = True) -> str:
    rendered = [k for k in keys if k != drop]
    branches = "\n".join(
        f"        if (metric == platform::SYSMON_METRIC_{const_name(k)}) {{\n"
        f"            mark_available(snapshot.availability, key);\n"
        f"        }} else"
        for k in rendered)
    tail = (" {\n            mark_not_implemented(snapshot.availability, key);\n        }\n"
            if fallback else
            " {\n            // fallback removed\n        }\n")
    return ("void build() {\n"
            "    for (const char* key : platform::SYSMON_METRIC_KEYS) {\n"
            "        const std::string_view metric(key);\n"
            f"{branches}{tail}"
            "    }\n}\n")


def run_cli(*args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(CHECKER), *args],
        capture_output=True,
        text=True,
        cwd=REPO_ROOT,
    )


class RealTreeTest(unittest.TestCase):
    """真实树：四个来源当前一致、builder 分支全覆盖（这是 AC3 的正例判据）。"""

    def test_real_tree_is_consistent(self) -> None:
        result = run_cli()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("15 SYSMON metric keys consistent", result.stdout)

    def test_selftest_passes(self) -> None:
        result = run_cli("--selftest")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("selftest", result.stdout)


class TempTreeTest(unittest.TestCase):
    """反例：每个用例在临时目录里构造 mini 源树，经 `--root` 驱动。"""

    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory(prefix="dz_metric_keys_test_")
        self.root = Path(self.tmp.name)
        for rel in (H_HEADER, CONTRACT, TS_FIXTURE, TEST_FIXTURE, BUILDER):
            (self.root / rel).parent.mkdir(parents=True, exist_ok=True)

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def _write(self, keys: list[str], **overrides) -> None:
        (self.root / H_HEADER).write_text(
            header_text(keys, overrides.get("declared")), encoding="utf-8")
        (self.root / CONTRACT).write_text(
            contract_text(overrides.get("contract_keys", keys)), encoding="utf-8")
        (self.root / TS_FIXTURE).write_text(
            ts_text(overrides.get("ts_keys", keys)), encoding="utf-8")
        (self.root / TEST_FIXTURE).write_text(
            cpp_fixture_text(overrides.get("cpp_keys", keys)), encoding="utf-8")
        (self.root / BUILDER).write_text(
            builder_text(keys, overrides.get("builder_drop"), overrides.get("fallback", True)),
            encoding="utf-8")

    def _run(self) -> subprocess.CompletedProcess[str]:
        return run_cli("--root", str(self.root))

    # --- 正例 -------------------------------------------------------------

    def test_identical_sources_pass(self) -> None:
        self._write(BASE_KEYS)
        result = self._run()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(f"{len(BASE_KEYS)} SYSMON metric keys consistent", result.stdout)

    # --- 四源漂移 ---------------------------------------------------------

    def test_contract_missing_key_fails(self) -> None:
        self._write(BASE_KEYS, contract_keys=BASE_KEYS[:-1])
        result = self._run()
        self.assertEqual(result.returncode, 1)
        self.assertIn("key set differs", result.stderr)
        self.assertIn("swap", result.stderr)

    def test_ts_extra_key_fails(self) -> None:
        self._write(BASE_KEYS, ts_keys=BASE_KEYS + ["extra.metric"])
        result = self._run()
        self.assertEqual(result.returncode, 1)
        self.assertIn("extra.metric", result.stderr)

    def test_cpp_fixture_missing_key_fails(self) -> None:
        self._write(BASE_KEYS, cpp_keys=BASE_KEYS[1:])
        result = self._run()
        self.assertEqual(result.returncode, 1)
        self.assertIn("system", result.stderr)

    def test_order_drift_fails(self) -> None:
        # 集合相同、顺序不同：R1 的数组顺序是发送方顺序，契约与夹具必须跟随。
        reordered = [BASE_KEYS[0], *BASE_KEYS[:0:-1]]
        self._write(BASE_KEYS, contract_keys=reordered)
        result = self._run()
        self.assertEqual(result.returncode, 1)
        self.assertIn("key order differs", result.stderr)

    # --- header 自身 ------------------------------------------------------

    def test_declared_size_mismatch_is_parse_error(self) -> None:
        self._write(BASE_KEYS, declared=len(BASE_KEYS) + 1)
        result = self._run()
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertIn("declared size", result.stderr)

    # --- builder ---------------------------------------------------------

    def test_builder_missing_branch_fails(self) -> None:
        self._write(BASE_KEYS, builder_drop="swap")
        result = self._run()
        self.assertEqual(result.returncode, 1)
        self.assertIn("explicit metric branches miss keys", result.stderr)
        self.assertIn("swap", result.stderr)

    def test_builder_fallback_removed_fails(self) -> None:
        self._write(BASE_KEYS, fallback=False)
        result = self._run()
        self.assertEqual(result.returncode, 1)
        self.assertIn("no mark_not_implemented fallback", result.stderr)

    # --- 解析失败必须是 exit 2 --------------------------------------------

    def test_missing_contract_anchor_is_parse_error(self) -> None:
        self._write(BASE_KEYS)
        (self.root / CONTRACT).write_text("# 契约（没有键列表行）\n", encoding="utf-8")
        result = self._run()
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertIn("availability metric key list line not found", result.stderr)

    def test_missing_source_file_is_parse_error(self) -> None:
        self._write(BASE_KEYS)
        (self.root / TS_FIXTURE).unlink()
        result = self._run()
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertIn("missing source file", result.stderr)


if __name__ == "__main__":
    unittest.main()
