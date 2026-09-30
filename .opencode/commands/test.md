---
description: 跑 CTest 测试套件（全量或按名字过滤）
agent: build
---

运行 dztrader 测试套件。

**Windows（PowerShell）**

```powershell
.\scripts\test.ps1                       # 全量（跟随粘性 config）
.\scripts\test.ps1 -Config Debug
.\scripts\test.ps1 -TestName TimeTest    # 单个测试
```

**Linux（Bash）**

```bash
./scripts/test.sh                        # 全量
./scripts/test.sh --config debug
./scripts/test.sh --config release --test-name TimeTest
```

**Linux 必须用 `--config` / `--test-name` flag，不能用位置参数** —— `test.sh` 对未知参数会直接 `exit 1`。

**关于 `--test-name` / `-R` 过滤**：测试用 `gtest_discover_tests` 注册且**没有** `TEST_PREFIX`，所以 ctest 里的名字是 `Suite.Test` 形式，**不是可执行文件名**。例如冒烟测试要用 `-R SmokeTest`，用 `-R dz_smoke` 会匹配不到任何测试。

底层等价命令（仅排查脚本问题时用）：`ctest --preset {win,linux}-{release,debug}`

若用户给了测试名（$ARGUMENTS），按上面的 `Suite.Test` 形式过滤。请实际执行并报告失败项，不要只复述命令。
