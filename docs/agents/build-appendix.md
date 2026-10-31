# 构建与平台附录

`AGENTS.md` 的补充材料。这里只放**漏读也不会立刻出事**的外围细节：工具链、环境激活、IDE 配置、输出布局、部署分析。日常命令与参数见 [README.md](../../README.md)。

CMake + Conan 2.x + Ninja + GTest。本文档记录**项目特有规则与非主流约定**；通用 CMake/Conan 用法不重复。

---

## 运行环境：临时目录与缓存（DSH 沙箱）

agent 会话可能跑在 workspace-write 沙箱里：只有仓库目录可写，`~` 下的共享缓存可能只读。

- **`/tmp` 每次 bash 调用隔离**：跨调用要用的日志/快照写 `.cache/`（仓库已 gitignore），不要写 `/tmp`
- **Conan 缓存**：默认 `~/.conan2`；不可写时 `scripts/setup.sh` 自动回退 `CONAN_HOME=<repo>/.cache/conan2`，并从可读的 `~/.conan2` **播种一次**（1.8G 本地拷贝，避免每个新 worktree 重新下载依赖；本波 #88 冷装实测约 50 分钟）
- **ccache**：本机装了 `ccache` 时 `scripts/setup.sh` 自动给 Linux CMake 配置 `CMAKE_CXX_COMPILER_LAUNCHER=ccache`；缓存目录固定为**主仓** `.cache/ccache`（`scripts/ccache-env.sh` 从 git common dir 解析，所有 worktree 共享同一份缓存）。`CCACHE_DIR` 是环境变量、不随 CMakeCache 持久，所以 `build.sh` 每次自行导出；漏掉会让新 worktree 构建直接报 `ccache: error: Read-only file system`（#81 实测，修法见该文件）。CI 尚未接入 ccache
- **npm 缓存**：默认 `~/.npm`；不可写时 `scripts/build.sh` / `scripts/preflight.sh` 自动设 `npm_config_cache=<repo>/.cache/npm`
- **gh 缓存**：`~/.cache/gh` 不可写时 `scripts/wait-ci.sh` / `scripts/ci-log.sh` 自动设 `XDG_CACHE_HOME=<repo>/.cache`，避免 `gh run view --log-failed` 失败
- 需要写共享缓存的 worktree 首建若仍被拒，再向用户申请一次 full access；不要反复重试同一命令

---

## 工具链

| 项 | 要求 |
|---|---|
| CMake | >= 3.28（`CMakeLists.txt` 的 `cmake_minimum_required`；`CMakePresets.json` 里的 3.23 只是 preset schema 下限，不冲突） |
| Conan | 2.x |
| 生成器 | Ninja（双平台统一） |
| Windows 编译器 | MSVC，**静态 CRT**（MT/MTd，conan profile `compiler.runtime=static`） |
| Linux 编译器 | GCC 13 |

Linux profile（`profiles/linux-gcc`）用 `tools.build:compiler_executables` 把编译器**显式钉在 `/usr/bin/gcc-13`**。这不是冗余配置：它让 conan 源码包（boost / drogon / spdlog / gtest / sqlitecpp）的 `package_id` 与 CI 的 gcc-13 预编译包保持一致。**不要"顺手"改成系统默认编译器** —— 那会造成混链，CI 才暴露。

工具版本对齐（本地与 CI 必须一致；升级时同一次提交改两边）：

- **clang-format 固定 21.x**：本机从 Ubuntu 官方归档取 `clang-format-21`（`apt-get download clang-format-21` 后 `dpkg -x` 解包，二进制放 `~/.local/bin/clang-format`）；CI 从 [apt.llvm.org](https://apt.llvm.org/) 的 `llvm-toolchain-noble-21` 安装。CI 对 `libs/`+`apps/` 全量 `--dry-run --Werror`
- **conan 固定 `==2.32.0`**：CI 两个 job 钉死；本地升级 conan 时同步改 `.github/workflows/ci.yml`
- 2026-10-16 的格式基线全仓格式化提交已记录于 [`.git-blame-ignore-revs`](../../.git-blame-ignore-revs)

---

## MSVC 环境激活（仅 Windows）

Ninja 不像 VS 生成器那样自动激活 MSVC，需设 `INCLUDE` / `LIB` / `PATH`。两条路径互补，**缺一不可**：

- **命令行**：`scripts/Activate-MSVC.ps1` 用 `vswhere` 调 `vcvarsall.bat x64`；`setup.ps1` / `build.ps1` / `test.ps1` 会自动 dot-source 它
- **VSCode**：`CMakePresets.json` 的 `win-base` 设 `architecture` / `toolset`（`strategy=external` —— CMake 忽略，但 IDE 会读）+ `.vscode/settings.json` 设 `cmake.useVsDeveloperEnvironment=always`（默认 `auto` 在 Preset 模式下不可靠，见 CMake issue #4117）

---

## Windows 编译陷阱（MSVC-only，Linux 门禁拦不到）

Linux 的 GCC/clang 门禁（含 Release `-Werror`）**复现不了**下列 MSVC-only 失败，它们实际红过 CI（#77×2、#78×1、#159×1）。改 Windows 分支前先对照；`scripts/preflight.sh` 的 `msvc hazard lint`（`scripts/check_msvc_hazards.py`）会机械检查 R1–R3，R4 与 R5 见下：

- **IP Helper 头顺序（R1）**：`winsock2.h → ws2tcpip.h → windows.h → iphlpapi.h`。`ws2tcpip.h` 负责引入 `ws2ipdef.h`（`MIB_IF_ROW2` 依赖的接口类型），缺它会报 `'PMIB_IF_TABLE2': undeclared identifier` 等 31 条级联。**禁止直接 `#include <netioapi.h>`**：用户态它依赖 `iphlpapi.h` 提供的 winnt/NDIS 上下文，单引会报 `missing ';' before '__cdecl'` / `NTSTATUS` 重定义。
- **PDH 状态常量（R2）**：`PDH_CSTATUS_*` 不保证由 `<pdh.h>` 引入，需显式 `#include <pdhmsg.h>`。
- **`SPDLOG_DEBUG/SPDLOG_TRACE` 在 Release 被编译掉（R3）**：若 catch 参数只在该宏里被引用，Release 下成为未引用局部变量 → MSVC `C4101` → `/WX` 升 `C2220`。异常路径改用 `SPDLOG_WARN`，或显式引用参数。
- **公开 SDK 头里的弃用 CRT 调用（R4，`check_msvc_hazards.py`）**：`_wfopen` / `strcpy` / `sprintf` 等在 MSVC 下 `C4996` → `/WX` 升 `C2220`。内部 target 有 `_CRT_SECURE_NO_WARNINGS` 掩盖，但 `libs/strategy_api/include/` 是 header-only 公开面，下游工程没有那个宏（#157）。
- **成员被同名参数/局部遮蔽（R5：C4458）**：MSVC `/W4` 报 `declaration of 'x' hides class member` → `/WX` 升 `C2220`；GCC **默认不报**（只有 `-Wshadow` 才报），所以 Linux 门禁本来是盲的（#159：`Volume volume` 参数遮蔽 `InstrumentContext::volume` 成交量序列成员）。机械守卫**不是** lint 脚本而是编译器本身：`libs/strategy_api/CMakeLists.txt` 给两个 SDK-only 探针（`json_sdk_probe_{builtin,external}`）加了 `-Wshadow` / `/we4458`；它们包含**每个**公开头且只链 `strategy_api_headers`，等价于下游策略工程。只加在探针上而不是全部 SDK 测试目标——后者编入 `src/api.cpp`，会把内部代码的既有遮蔽一并卷进来。
- 排查办法：对比失败 TU 与同 job 通过 TU 的 **`showIncludes` 包含树**（CI 日志搜 `Note: including file:`），差异通常就是根因（#77 靠它定位）。Windows job 日志可达数十 MB，`scripts/ci-log.sh` 优先取 `last-test-log` artifact；若日志流式拉取被截断（`stream error: stream ID 1; CANCEL`），用 `curl -H "Authorization: token $(gh auth token)"` 直接下到文件再 grep —— 截断的日志会停在构建中途、**没有任何 error 行**，看起来像「无故失败」（#159 实测）。

## clangd / compile_commands.json

- `CMAKE_EXPORT_COMPILE_COMMANDS=ON`，Ninja 在 binaryDir 生成
- `setup` 脚本把它拷到**项目根**
- `.clangd` 显式 `CompilationDatabase: .`，避免误命中 `build/<plat>/x86_64/<config>/` 下的原始文件
- **根目录的 `compile_commands.json` 属于"最后跑 configure 的那个 config"** —— 切档都会覆盖它。日常只有 Release 一个档
- `.vscode/settings.json` 设 `clangd.arguments: []`，覆盖全局可能存在的过时 `--compile-commands-dir`

---

## 预设（CMakePresets.json）

Ninja 单配置，每个 config 独立 binaryDir：`build/<plat>/x86_64/<Release|Debug|Asan>/`。

- conan install 只装单个 config，toolchain 锁定 `build_type`。**一次 setup 装一个 config**，需要另一个就再跑一次
- conan profile 禁用 `CMakeUserPresets.json` 生成（`tools.cmake.cmaketoolchain:user_presets=`），conan 只生成 toolchain

预设名：`win-release` / `win-debug` / `win-asan` / `linux-release` / `linux-debug` / `linux-asan`（build 与 test 共用）。

**只有 `release` 是日常档**（#168 / [ADR 0037](../adr/0037-release-only-build-and-install-as-sole-artifact.md)）。`debug` 与 `asan` 是显式指定的本地档：不进门禁、不进 CI、禁止部署。

**版本与 SDK 命名**（ADR 0038）：版本遵循 SemVer 2.0.0，单一真相源是 `CMakeLists.txt` 的 `project(VERSION)`（`conanfile.py` 用 `set_version()` 读取）。预发布标识经 `DZ_PRERELEASE` 注入（`DZ_PRERELEASE=rc1 ./scripts/setup.sh` → `dz_version_string()` 返回 `"0.1.0-rc1"`）。策略 SDK 的 target 是 `dztrader_sdk`、产物是 `libdztrader.so`/`dztrader.dll`（`OUTPUT_NAME` 分离），带 `SOVERSION` 版本化，导出符号由 `libs/strategy_api/exported_symbols.map` 白名单收敛为仅 `dz_*`（49 个）。

**`debug` preset 可能随时间腐烂**：仓内有 7 处 `#ifndef NDEBUG` / `#if !defined(NDEBUG)` 分支（`log.h` 的调试日志默认值、`string_util.h` 的 0xFE 毒化填充），Release-only 下永不参与编译，改坏了没人发现。需要运行时检查时优先用 `asan` 档，而不是 `debug`。其中 `asan` 的 `CMAKE_BUILD_TYPE` 也是 Release（ASan 文档要求 `-O1` 以上可用速度），只从 build 树运行、永不 install。

test 预设另有两档后缀：`-slow`（穷举/慢速）与 `-perf`（无阈值基准），例如 `linux-release-slow`。默认预设在测试侧只跑 `fast` 标签，见下方「单元测试约定」。

---

## 输出目录布局

`DZ_OUTPUT_DIR = CMAKE_BINARY_DIR`。**不要再追加 `$<CONFIG>`** —— binaryDir 里已经含 config，追加会产生 `Release/Release` 重复路径。（这是已踩过的坑，属回归防护。）install 目录则仍用 `$<CONFIG>` genex。

| 变量 | 路径 | 用途 |
|---|---|---|
| `DZ_OUTPUT_DIR` | `${CMAKE_BINARY_DIR}` | 主进程、网关、webui、CTP DLL（扁平化） |
| `DZ_TESTS_DIR` | `${DZ_OUTPUT_DIR}/tests` | 单元测试、test_worker |

**网关扫描**：主进程从 `this_process::exe_dir()` 直接扫描（扁平化，**不是** `DZTRADER_HOME`），按 `dzmd_` / `dztd_` 前缀识别网关二进制。

---

## 单元测试约定

> 本节讲**机制**（怎么编译、注册、跑）；**判据**（测试该怎么写、怎么判）见 [../unit-test-standard.md](../unit-test-standard.md)。

- 每个 `*_test.cpp` 独立编译运行，经 `dz_add_test()` 自动注册（`cmake/DzTest.cmake`），输出到 `${DZ_TESTS_DIR}`。**不要在模块 `CMakeLists.txt` 里直接调 `add_executable` + `gtest_discover_tests`** —— 超时参数会被漏掉（见下条）
- **用例发现超时（#227）**：`gtest_discover_tests` 在 **POST_BUILD** 阶段执行测试二进制枚举用例，CMake 默认发现超时只有 **5 秒**，且**没有**全局超时变量（只有全局 `CMAKE_GTEST_DISCOVER_TESTS_DISCOVERY_MODE`），只能逐调用点传参。5 秒在 Windows runner 冷启动下不够（sccache READ_ONLY + 近千 target 并发链接 + 杀毒/IO 抖动），已两次与改动无关地把 Windows CI 打成红（run 40856647679 / 41167289411，报 `Process terminated due to timeout` / `Timeout: '5'`）。因此 `dz_add_test()` 统一带 `DISCOVERY_TIMEOUT`（默认 30s，`-DDZ_GTEST_DISCOVERY_TIMEOUT=<n>` 可覆盖），`scripts/check_gtest_discovery_timeout.py` 在 preflight 里守住三条：除宏定义处不得直接调 `gtest_discover_tests`、宏内调用必须带超时参数、默认值不得低于 30s
  - `dz_add_test(<target> SOURCES <src...> [LABELS <label...>] [PROPERTIES <key value...>] [POST_BUILD <command...>])`；`LABELS` 缺省为 `fast`
  - 测试目标自己的 POST_BUILD 命令（如 `strategies/demo` 把 `dztrader_sdk` DLL 复制到测试 exe 旁边）**必须**走 `POST_BUILD` 参数，不能写在宏调用之后：同一目标的 POST_BUILD 按声明顺序执行，后发现者在前者之后，而发现阶段本身就需要 DLL 已就位
- **部分模块的测试有意只编译源码子集**，换取与外部依赖解耦：
  - `strategy_api` 测试约一半只编译内部类；`SDK_API_TESTS` 名单（notify_ui/trade/md/schedule/engine/db/db_instruments/td_ingest_*）额外编入 `api.cpp` + `db_database.cpp`，覆盖 C 接口完整写读路径（名单以 `libs/strategy_api/CMakeLists.txt` 为准）
  - `ctp` 测试不含 `md_main.cpp` / `md_api.cpp` / `md_spi.cpp`（避免链接 CTP 库）
  - 代价：这些源文件被重复编译。**这是有意取舍，不要当成待优化的重复劳动去"修复"**
- 需要 `DZTRADER_HOME` 的测试在 `SetUp` 里创建临时目录并设置该变量、`TearDown` 恢复。它们是 `test-base` 强制注入规则的**唯一豁免**（见 AGENTS.md 硬门禁 2）
- `libs/log` 的测试文件共用 `tests/test_main.cpp`；其中三个通过 `extern test_root()` 使用进程级唯一临时根目录，`atexit` 清理并在失败时记录 `error_code`。这是[单元测试判据 §5.2](../unit-test-standard.md)「重复基建需有据」的有意先例：daily sink 的文件名含日期，固定目录会跨运行残留、并行 ctest 互相干扰；根目录放系统临时目录，不写 `DZTRADER_HOME`。**死亡测试注意**：Windows 上死亡用例会重新执行 `main()` 并得到另一个临时根目录，日志目录须由父进程经环境变量传给子进程（见 `terminate_handler_test.cpp`），父进程再从自己的根目录读日志断言
- **ctest 测试名不等于可执行文件名**：`gtest_discover_tests` 不设 `TEST_PREFIX`，所以按可执行名过滤（例如 `-R dz_smoke`）会匹配不到任何测试。用 `-R` 时按 `Suite.Test` 形式过滤
- **并行 ctest（#136）**：`gtest_discover_tests` 把每个 `TEST`/`TEST_F` 注册成**独立 ctest 进程**，`test.sh` / `test.ps1` 默认 `CTEST_PARALLEL_LEVEL=4`（显式设置优先；fast 档实测 150.7s → ~110s）。并发安全的前提是**用例级资源唯一化**：
  - 临时目录/文件统一用 `libs/core/tests/test_temp_dir.h` 的 `dztrader::test::unique_temp_dir()` / `unique_temp_path()`（进程唯一 `pid_随机`；进程内恒等——`paths::home()` 等按进程缓存，同一进程内必须多次取到同一路径）。该头经 CMake `INTERFACE` 目标 `dztest_support` 暴露，`dzshm_test_support` 传递链接它
  - 新增固定名 `temp_directory_path()/dz_xxx` 会被 preflight 的 `scripts/check_test_portability.py` 拦下（白名单只含唯一化实现本身）
  - **进程级/机器全局资源必须 `RUN_SERIAL`**：真实进程 E2E（`apps/sysmon` 的 `*_e2e_test`、`apps/master` 的 `dz_smoke_test`）、机器全局 `NamedSemaphore` 通道名（`SDK_API_TESTS`、`stg_demo_test`）。判据是「该用例能否与同机另一个测试进程同时安全运行」，不是「跑得慢不慢」
- **分层标签**（与 `RUN_SERIAL` / `TIMEOUT` 正交）：
  - `fast`：默认单测门禁，秒级用例；`dz_add_test` 的 `LABELS` 缺省值就是 `fast`，显式写与省略等价
  - `slow`：显式穷举/慢速档。`libs/strategy_api` 的 `SDK_SLOW_TESTS`（`date_exhaustive_test` / `date_time_exhaustive_test` / `bar_diff_exhaustive_test`）单独成 target 并打 `slow`；对应抽样版留在 `date_test.cpp` / `date_time_test.cpp` / `bar_diff_test.cpp` 的 `fast` 门禁。`bar_diff_exhaustive_test` 是 tick→bar 差分验收（ADR 0031 决策 10）的全量档：用例逻辑在 `bar_diff_cases.h` 只写一份，fast 档每条路径各跑一次、slow 档用多 seed/多日/高密度覆盖
  - `perf`：无阈值基准项（`PerfBench`（SHM 往返）、`BarPerfBench`（tick→bar 四路径）、#164 的五个 BarStrategy 框架基准），不进默认门禁；数值与记录方式见下节「性能基准」
  - 运行：`./scripts/test.sh`（fast）/ `-L slow` / `-L perf`（Windows 用 `-Label`）；等价直调 `ctest --preset linux-release[-slow|-perf]`。测试 `DZTRADER_HOME` 由 `test-base` 注入 `.cache/test_home`，**运行请走 `test.sh` / `gate.sh test`**（每次清空重建，避免上一轮残留 shm/DB 污染无关测试）；绕过脚本直调 `ctest --preset` 不复位该目录
  - `slow` / `perf` 不在 CI 默认门禁内（CI 只跑 fast），按需本地手动运行；`fast` 过滤依赖构建期发现，必须先 build 再 test（未构建 target 的 `_NOT_BUILT` 回退不带标签）

---

## 性能基准

基准与秒级单测分层（[单元测试判据 §4.3](../unit-test-standard.md)）：基准只**报告数值**、不做阈值断言，稳定后才升级为 CI 门禁。当前七项，都在 `perf` 标签下：

| 基准 | 源码 | ctest 名 | 测什么 |
|---|---|---|---|
| `dz_shm_perf_bench` | `libs/shm/tests/perf_bench.cpp` | `PerfBench` | SHM 写帧→唤醒→读出往返延迟（README 30μs 目标的同进程下限） |
| `dztrader_sdk_bar_perf_bench` | `libs/strategy_api/tests/bar_perf_bench.cpp` | `BarPerfBench` | tick→bar 四条热路径（update / 新 bar / 跨日 / 竞价）的 ns/tick 与 p99 |
| `bench_tick_update` | `libs/strategy_api/tests/bench_tick_update.cpp` | `bench_tick_update` | BarStrategy 框架 tick 路径（聚合 + 回调分发）的 ns/tick 与 p99（#164） |
| `bench_bar_close_dual_ma` | `libs/strategy_api/tests/bench_bar_close_dual_ma.cpp` | `bench_bar_close_dual_ma` | bar close 路径：分发 + 双均线 + 意图 fanout + 上报（不含 shm 写）的 ns/close 与 p99（#164） |
| `bench_series_read` | `libs/strategy_api/tests/bench_series_read.cpp` | `bench_series_read` | Series `[n]` 回溯读与 `span()` 连续扫描的单操作 ns（#164） |
| `bench_relocate` | `libs/strategy_api/tests/bench_relocate.cpp` | `bench_relocate` | 18:00 回卷（memmove 布局变换）成本与停顿：单品种 + 50 品种预算两口径（#164） |
| `bench_warmup_replay` | `libs/strategy_api/tests/bench_warmup_replay.cpp` | `bench_warmup_replay` | 历史预热回放吞吐（bars/s、µs/bar，含 B 回调与虚拟成交）（#164） |

### 五个框架基准（#164）的测量边界

`bench_tick_update` / `bench_bar_close_dual_ma` / `bench_warmup_replay` 量的是**框架自身**的计算路径（组件文档 §10 的 1µs 预算不含 SDK 帧解析、shm 写与网络）。它们经 `bench_framework_harness.h` 在 **SDK/C API 边界打桩**：`dz_place_order` / `dz_set_logical_position` / `dz_subscribe` / `dz_schedule_daily` / `dz_next_md` / 归档库查询等入口只记调用计数并返回成功，不写帧、不碰 shm；历史 bar 与合约元数据经 `FrameworkDependencies` 注入（与 Seam B 测试同一条内部缝）。因此：

- 启动序列（配置 → 元数据 → 实例装配 → 回放 → live）与生产完全一致，被替换的只有「帧写到哪」；
- 桩的代价是「普通调用 + 计数器自增」几 ns，订单/上报**不含**真实帧序列化与 shm 写——这正是 §10 的口径（「不含共享内存写」）；
- 桩是编译期替代（基准不链接 `dztrading` 的 api.cpp）：框架新增被实例化的 SDK 调用而没补桩时**链接失败**，不会静默少测一段路径；
- `bench_series_read` / `bench_relocate` 不碰 SDK：经 `context_internal::ContextAccess` 构造脱离引擎的 ctx（与纯单元测试同一条构造缝）。

`bench_series_read` 的单次读只有几 ns（低于成对读 RDTSC 约 8.8ns 的开销），故它的 p99 是**组均值的 p99**（`[n]` 每 1000 次读一组、`span()` 每 10 次整窗扫描一组），按「聚合成本」读，不要当单操作尾部。`bench_relocate` 有三个场景：单品种 `max_lookback=500`（§10 基准参数）、50 品种 `max_lookback=500`、50 品种 `max_lookback=1440`（与 §4.4 的 6.3MB 预算估算同口径）。`bench_warmup_replay` 用 `steady_clock` 量整段 `on_start`，并以「历史源不可用的冷启动」为基线扣掉与回放无关的固定启动成本；轮数少（15 warm + 5 cold），报 p50 与最大值而不是 p99。

### 运行命令

```bash
./scripts/build.sh                                    # 先构建（基准不进默认 target 即可，build 全量包含）
./scripts/test.sh --label perf                         # 跑全部 perf 基准（Release）
ctest --preset linux-release-perf -V                   # 或经 ctest 并回显输出

# 直接跑单个基准：完整输出最方便；手动运行必须显式给项目内的 DZTRADER_HOME（AGENTS §一.2）
DZTRADER_HOME=$PWD/.dztrader_dev ./build/linux/x86_64/Release/tests/bench_tick_update
DZTRADER_HOME=$PWD/.dztrader_dev ./build/linux/x86_64/Release/tests/bench_bar_close_dual_ma
DZTRADER_HOME=$PWD/.dztrader_dev ./build/linux/x86_64/Release/tests/bench_series_read
DZTRADER_HOME=$PWD/.dztrader_dev ./build/linux/x86_64/Release/tests/bench_relocate
DZTRADER_HOME=$PWD/.dztrader_dev ./build/linux/x86_64/Release/tests/bench_warmup_replay

# 快速自测（改完基准是否还能跑通）：五个基准都接受 --quick，小样本、秒级内跑完，数值不作对比
DZTRADER_HOME=$PWD/.dztrader_dev ./build/linux/x86_64/Release/tests/bench_tick_update --quick
./build/linux/x86_64/Release/tests/bench_series_read --quick   # 纯 ctx 基准不读 DZTRADER_HOME
./build/linux/x86_64/Release/tests/bench_relocate --quick
```

Windows：`.\scripts\build.ps1` → `.\scripts\test.ps1 -Label perf`，可执行文件在 `build\windows\x86_64\Release\tests\`。

**在 CI 手动触发**：#164 的五个框架基准不在 CI 默认门禁（CI 只跑 fast 标签）。需要时手动触发 `CI` workflow 并勾选 `run_perf=true`（Linux job 会执行 `./scripts/test.sh --label perf`，再用 `ctest -V` 回放一次把数值打进日志）：

```bash
gh workflow run CI --ref <branch> -f run_perf=true
```

经框架启动的三个基准（`bench_tick_update` / `bench_bar_close_dual_ma` / `bench_warmup_replay`）把临时配置写在 `$DZTRADER_HOME/bench/<基准名>/`（跑完删除），未设置 `DZTRADER_HOME` 时直接报错退出，不退化到系统临时目录；`bench_series_read` / `bench_relocate` 只经 `ContextAccess` 构造 ctx，不需要 `DZTRADER_HOME`。

**必须用 Release**（`-Werror` 与优化同时开启；Debug 数值无参考价值，基准会在报告头打印 `warning: debug build`）。`perf` 档的 7 个基准都带 `RUN_SERIAL`（串行执行，避免并行进程互相污染 p99）；即便如此，跑之前仍建议关闭其它重负载。

**看完整输出要直接跑可执行文件，或给 ctest 加 `-V`**：`ctest` 默认只在**失败时**回显输出，`--output-on-failure` 同样只在失败时回显（实测通过时 `amortized|fingerprint` 命中 0）；`--label perf` 跑完只给一行 Passed。要读数值加 `-V`（verbose，实测命中 3 处），或直接跑二进制。

### 基准怎么测（读数值前必读）

1. **计时用 RDTSC 不用 `steady_clock`**：本机成对读 `steady_clock` 约 17ns、成对读 RDTSC 约 9ns，而预算是 50ns/tick。TSC 频率在启动时用 `steady_clock` 标定后换算成 ns，故跨机器可比。
2. **tick 块在计时循环之外生成，循环里只重放**。本基准迭代时踩过这个坑：早期版本把 296 字节的 `DzTick` 按值穿过 visitor 逐笔拷贝、并在每轮重放前重新生成整块，update 路径被量成约 46 ns/tick；改成「块只生成一次、循环里只按 const 引用重放」后，同一个组件量到约 13 ns/tick ——**3.5 倍差异全部来自基准自身开销**，与组件无关。判断依据是与独立墙钟测法对照。改动这个基准时，保持「块只生成一次」这个性质。
3. **三套口径互为交叉校验**，三者应同量级（本机实测差 0–2ns）：
   - `batch_mean` —— **最准的 ns/tick 均值**，每轮整段只读一对时钟。
   - `net_p50` —— 逐笔分布，**p99 的来源**。逐笔计时必然扰动管线，且扣减空区间开销（8.8ns）是近似而非精确补偿，故 `net_p50` 略高于 `batch_mean`，应视为**偏保守的上界**。
   - `wall_mean` —— 用 `steady_clock` 量同一段，不依赖 TSC 频率标定。墙钟区间比 TSC 区间多包了两次 `steady_clock` 读（约 17ns）与两次 rdtsc（约 9ns），合计约 26ns/轮；摊到 50–200 笔是 0.13–0.52 ns/tick，故也略高，同为上界。

   三者出现数量级分歧（而非零点几纳秒）时，先怀疑基准本身（计时区间混入非被测操作、块被优化、tick 流落错路径），而不是组件性能变化。

另有**自检**：计时前逐笔断言 builder 公开状态与日程独立推导的期望一致、丢弃计数为 0；再用 `CountingSink` 断言各路径的 sink 事件形态（竞价路径必须每笔竞价 tick 一次 update、事件携带 `is_auction=true`；连续交易路径恒为 false，见 ADR 0032）。任一不符直接退出 1。计时后打印终态指纹，可用来跨机器/跨 commit 快速判断「是否真的跑了同一份工作」——指纹相同 = 结果可比。

### 结果记录方式

**不记录到 CI 产物、不设阈值**。数值稳定后由人工把「机器 + 日期 + 一张结果表」追加到本文件末尾的**基线记录**，供后续对照。

升级为 CI 门禁时的约定（先说清，避免下一个人自己发明口径）：

- **门禁用 `batch_mean` 比上限、用 `net_p99` 比 p99 上限** —— `batch_mean` 是三套口径里最准的水平值（逐笔计时必然扰动管线，`net_p99` 是偏保守的上界）；`wall_mean` 只作交叉校验，不入门禁。
- **每次门禁跑 5 次取中位数**，再与阈值比较；单次运行的 p99 抖动可达 ±5ns，单次比较会产生假红/假绿。本文件下方基线记录取多次运行的中位数（具体次数在各记录中注明），是人工记录的加严做法，不是门禁要求。
- **阈值来自届时 CI runner 的实测基线**，不是 ADR 的 50ns 设计目标，也不是本机基线记录里的数字（机器代差可达数倍）。首次升级时把「哪台 runner、多少样本、算出多少」写进基线记录。

比对本机改动前后时用同一台机器、同一 config，同样取多次运行的中位数。

### 基线记录

格式：每条记录一个四级标题（机器 + 日期），下附基准名与结果表。新增记录追加在末尾，**不修改历史记录**（与 ADR 一致：历史是证据）。

#### 2026-10-23 · Intel Core i7-6700 @ 3.40GHz · Linux x86_64 · GCC 13.4 Release

`dztrader_sdk_bar_perf_bench`，1 分钟周期、单合约、`NullBarSink`，9 次运行取中位数。本机 TSC 3.408 GHz，空区间成对读时钟 p50 = 8.80ns，p99 = 9.39ns。

| 路径 | batch_mean | wall_mean | net_p50 | net_p99 | 50ns 目标（仅 update） |
|---|---|---|---|---|---|
| update（同桶更新） | 12.70 | 13.17 | 14.67 | 16.43 | met（目标的 33%） |
| new-bar（换桶） | 21.93 | 22.08 | 21.13 | 24.65 | 不适用，仅报告 |
| cross-day（跨交易日） | 19.93 | 20.49 | 21.13 | 22.89 | 不适用，仅报告 |
| auction（竞价窗口内） | 11.33 | 11.65 | 13.50 | 14.08 | 不适用，仅报告 |

`raw_p50` 落在 TSC 周期栅格上（1 cycle = 0.2934 ns），所以 `net_p50` 只取离散值：如 update 的 14.67ns 对应 80 cycles，cross-day 的 21.13ns 对应 102 cycles。相邻两次运行的单格跳动通常是整整一个周期栅格（约 0.3–0.6ns），**不是性能变化**——比对时看整行与多次中位数，不要比较相邻两次的单格。

观察：换桶/跨日比同桶更新贵约 7–8ns —— 两者都多一次 `close` + `open` 事件派发（本基准 sink 为空实现，故这里是组件侧的事件分发成本，不含消费方 sink 自身开销）与累计基准重置，量级符合预期。竞价最便宜：窗口内只静默更新 bar 状态、不回调 sink。无需优化项。

50ns 目标按 ADR 0031 决策 10 只覆盖**单周期更新**路径；另外三条路径按同一口径报告数值（供后续优化与 CI 门禁取基线），不套用该阈值。

读表时注意两点，否则会过度解读路径间差异：

- **路径间的工作量不完全对等。** update / auction 的 `setup` 段承担了「建立首根 bar」（`begin_bar`），new-bar / cross-day 的每笔被测 tick 都自带 `close` + `open`；竞价 case 的窗口首笔同样在 `setup`。所以「竞价最便宜」只部分来自「窗口内不回调 sink」，不能直接当作「竞价路径比换桶路径快一倍」的结论。
- **每条路径重放同一条固定流**，分支预测器看到的是完全重复的模式，数值是**最佳情况**；混合负载（多合约、多周期、真实 tick 抖动）会更差。基准用于**同口径纵向对比**（改动前后、跨 commit），不是绝对值承诺。

单次运行的 p99 抖动可达数 ns（机器负载高时实测到 ±5ns，明显大于均值的抖动），所以比对改动前后要用多次运行的中位数，不要拿单次 p99 下结论。

与 ADR 0031 决策 10 的「静态估算约 10ns」对照：实测 `batch_mean` 12.70ns 比静态估算高约 2.7ns，差异来自交易小节/竞价表的定长循环查找、量额差分与 bar 状态更新——同量级，无误判。

> 该机器为 2016 年 4 核桌面 CPU（非 CI runner）。升级 CI 门禁时阈值应以届时 CI runner 的实测基线为准，不能直接搬 50ns 或本表数值。

#### 2026-10-27 · Intel Core i7-6700 @ 3.40GHz · Linux x86_64 · GCC 13.4 Release

`dztrader_sdk_bar_perf_bench`（release，`-O3 -DNDEBUG`），1 分钟周期、单合约、`NullBarSink`，9 次运行取中位数；9 次运行均 exit 0，日程自检与路径身份自检全部通过（auction 路径按 ADR 0032 新事件形态：窗口内每笔竞价 tick 一次 `update`、事件携带 `is_auction=true`）。本机 TSC 3.408 GHz，空区间成对读时钟 p50 = 8.80ns，p99 = 9.39ns。

| 路径 | batch_mean | wall_mean | net_p50 | net_p99 | 50ns 目标（仅 update） |
|---|---|---|---|---|---|
| update（同桶更新） | 11.89 | 12.36 | 14.08 | 15.26 | met（目标的 31%） |
| new-bar（换桶） | 20.29 | 20.45 | 21.13 | 24.06 | 不适用，仅报告 |
| cross-day（跨交易日） | 18.58 | 19.11 | 19.95 | 22.30 | 不适用，仅报告 |
| auction（竞价窗口内） | 11.01 | 11.31 | 12.91 | 12.91 | 不适用，仅报告 |

终态指纹（9 次运行全部一致；跨机器 / 跨 commit 指纹相同即可认为跑了同一份工作）：

| 路径 | 指纹 |
|---|---|
| update | `2340bc521b0d799c` |
| new-bar | `b7f2f58862c765bc` |
| cross-day | `329db9890b32409c` |
| auction | `8821dfb47ea5a88c` |

与上一条基线（2026-10-23，同机 / 同 GCC 13.4 Release / 同口径）对比 update 路径：

| 指标 | 2026-10-23 | 2026-10-27 | 差值 |
|---|---|---|---|
| batch_mean | 12.70 | 11.89 | −0.81ns（−6.4%） |
| wall_mean | 13.17 | 12.36 | −0.81ns（−6.2%） |
| net_p50 | 14.67 | 14.08 | −0.59ns（−4.0%） |
| net_p99 | 16.43 | 15.26 | −1.17ns（−7.1%） |

update 路径与基线同量级、无退化（略快），50ns 目标仍 met。三条连续交易路径（update / new-bar / cross-day）的 batch_mean 相对上一条记录整体低 6.4%–7.5%，方向与「#108 删除了连续交易路径上每笔 tick 一次的待补发队列 flush 循环」一致，同时也含机器/系统状态漂移；auction 行事件形态已变，不参与该区间（见下段）。本轮没有在旧 commit 上做同日 A/B，故只按同量级判定，不把差值精确归因。

auction 行不能与上一条记录的 auction 行按同口径直接比较：事件形态已被 ADR 0032 改变（旧口径窗口内静默、零回调；新口径窗口内每笔 tick 一次 `update`）。旧静默口径的 batch_mean / net_p50 为 11.33 / 13.50，新事件形态为 11.01 / 12.91；batch_mean 差值 0.32ns 落在本轮单次运行抖动内（9 次范围 10.97–11.86ns），只作量级参考，不判定性能变化。

#### 2026-10-29 · Intel Core i7-6700 @ 3.40GHz · Linux x86_64 · GCC 13.4 Release

新增 #164 的五个 BarStrategy 框架基准，各跑 9 次取中位数；45 次运行全部 exit 0（自检通过）。本机 TSC 3.408 GHz，空区间成对读时钟 p50 = 8.80ns，p99 = 9.39ns。口径：Release、单品种（relocate 另有 50 品种预算场景）、单挂靠账户、`period_sec=60`、`max_lookback=500`；tick / bar close / warmup 用双均线基准策略，series / relocate 用双均线的两条用户序列构造窗口与数组。测量边界见上节「五个框架基准的测量边界」（SDK/C API 边界打桩，**不含 shm 写**）。

**`bench_tick_update`**（1,000,000 ticks；每桶 59 update + 1 new-bar）：

| path | mean_raw | net_p50 | net_p99 |
|---|---|---|---|
| update（同桶更新） | 47.55 | 33.45 | 59.27 |
| new-bar（换桶，含一次 close：双均线 + 意图 + fanout） | 198.62 | 175.47 | 305.17 |

stream 交叉校验 `wall_ns/tick=82.11`（上界，含 tick 生成与循环开销）；终态指纹 9/9 次一致 = `4d70714e78adb84a`。§10「A 框架计算路径 p99 ≤ 1µs」→ update **met（5.9%）**；new-bar 无预算（含一次 `on_bar_close` 的完整 B 逻辑），仅报告。单次运行的 p99 抖动约 ±25%（本轮 45.77–75.70ns），比较改动前后用多次中位数。

**`bench_bar_close_dual_ma`**（200,000 closes；每根 bar 一次意图反手 → 每 close 1 单 + 1 上报）：

| measure | mean_raw | net_p50 | net_p99 |
|---|---|---|---|
| close tick | 207.94 | 180.16 | 278.76 |
| B callback body | 116.35 | 98.59 | 137.91 |

close tick = close 分发 + B 回调体（指标 + 意图 + fanout + 上报）+ open/update 分发；B callback body 是 B 在 `on_bar_close` 内自量的「指标 + 意图 + fanout + 上报」。9/9 指纹 = `6b690457e8445381`。§10「bar close 框架路径 p99 ≤ 1µs」→ **met（27.9%）**。交易意图 fanout/上报（每账户 `L_i` 差量 → 1 单 + 1 上报，不含 shm 写）含在上表内；§10 对该项的口径是「含 shm 写与 `notify_subscribers`」，本组基准按 #164 AC 不含 shm 写，含 shm 的端到端数值属真实通道测量、不在 #164 范围（§10 第 4 段「决策 → 订单帧落 shm」单列为后续项）。

**`bench_series_read`**（窗口 500 根填满 + 2 条用户序列）：

| path | mean_raw | net_p50 | net_p99 |
|---|---|---|---|
| `[n]` 回溯读 | 2.470 | 2.375 | 3.505 |
| `span()` 整窗扫描（500 doubles） | 885.99 | 857.51 | 2176.53 |

`span()` 行是未向量化的依赖累加（500 次标量 add，约 1.72ns/元素）。p99 是组均值的 p99（组内 1000 次读 / 10 次扫描），受调度噪声影响较大（9 次 1.47–2.21µs），按「聚合成本」读。§10 无该项独立预算，作为回归基线。

**`bench_relocate`**（三个场景；每个场景 = 先补满窗口到待回卷态（不计时）→ 对全部实例依次 `relocate`（计时））：

| scenario | arrays/instance | moved | net_p50 | net_p99 |
|---|---|---|---|---|
| 单品种 `max_lookback=500`（§10 基准参数） | 9 | 36KB | 971.83ns | 1176.06ns |
| 50 品种 `max_lookback=500` | 9 | 1.8MB | 98.18µs | 147.83µs |
| 50 品种 `max_lookback=1440`（§4.4 预算场景） | 11 | 6.34MB | 562.93µs | 623.93µs |

单品种 = 0.97µs/次（per-array 103ns），按 1440 根折算 p50 ≈ 2.7µs——v1 单品种部署远低于 0.2ms 停顿上限。**50 品种 @1440 的预算场景（≈6.3MB，正是 §4.4 的估算口径）实测 p50 = 0.56ms、p99 = 0.62ms**：有效搬移吞吐 ≈ 11.3GB/s，是 §4.4 估算隐含的 48–105GB/s 的 1/4–1/9，**超出 §10 的 60–130µs/天预算约 4–10 倍，也超出 0.2ms 停顿上限约 3 倍**。偏差原因是估算按「只数被搬字节」的带宽算，而 memmove 的读+写流量翻倍、6.34MB 工作集超过 L3（8MB 的一半以上被挤出）、50 次逐实例调用还叠加缓存冷启动。结论：**该项偏差，需要后续优化项**（§10 已列 B 镜像环形升级路径——延迟平坦、无整窗搬移；或限制单进程品种数 / `max_lookback`；真实部署前须用 50 品种实测复核）。A 的定时器路径另有 map 遍历与一行诊断日志，不属布局搬移成本。

**`bench_warmup_replay`**（`warmup.bars=10,000`、`max_lookback=500`；15 warm + 5 cold 轮；轮数少，报 p50 与最大值而不报 p99）：

| phase | mean_ms | p50_ms | max_ms |
|---|---|---|---|
| warm（带历史回放） | 4.822 | 4.787 | 5.165 |
| cold（历史源不可用，基线） | 0.083 | 0.077 | 0.096 |

派生：replay_only ≈ 4.71ms（warm − cold）→ **2.02M bars/s、0.495µs/bar**（9,500 根进入回调期的 bar，含 38,000 个合成 builder tick 与 57,000 次 B 回调）。§10 估算 = 20.9ms（0.1µs/tick × 38,000 + 0.3µs/回调 × 57,000）→ 实测为估算的 **0.23x（约 4.3 倍快）**；50 品种折算 0.235s（串行，单实例成本 × 50）vs 预算 0.5–1s → **达标（低于预算）**。估算偏保守的原因同 §5.3 的 0.1–0.4µs 区间：实测 builder tick 与回调都在该区间下沿。回放期 B 照常运行、意图只虚拟成交（自检断言零订单），故不含 fanout/上报。

**结论**：§10 的 1µs 两条预算均达标（tick 路径余量约 17 倍、close 路径约 3.6 倍）。预热回放优于预算估算（预算为保守上界）。**回卷是本次唯一的偏差项**：单品种（v1 常态）0.97µs/次无问题，但「50 品种 × max_lookback=1440」的 §4.4 预算场景实测 p99 0.62ms，超 60–130µs 预算 4–10 倍、超 0.2ms 停顿上限 3 倍；原因与后续优化项（§10 的 B 镜像环形升级路径 / 限制单进程品种数或 max_lookback）已记录在组件文档 §10 与 §4.4。本组基准仍为 report-only、未升级为 CI 门禁；升级时按上文「升级为 CI 门禁时的约定」取 CI runner 上的 5 次中位数基线。

<!-- 下一条基线记录（追加到这里，保持时间顺序） -->

---

## 本地 MySQL 测试环境（可选）

MySQL 相关用例（`MysqlDriverTest`、`TdSchemaMysqlTest`、驱动一致性套件的 mysql 部分）在**未配置服务器时 Skip**（GTEST_SKIP，不失败），判定条件为环境变量 `DZ_TEST_MYSQL_HOST` 非空。需要本地跑通时：

1. 安装任意 MySQL 协议兼容服务器（Ubuntu 官方归档示例）：

```bash
sudo apt install mariadb-server
sudo systemctl start mariadb   # apt 通常已自动启动
```

2. 建测试账户（用例自建/自删 `dz_test_*` 库，不污染其他库）：

```bash
sudo mariadb -e "CREATE USER 'dz'@'127.0.0.1' IDENTIFIED BY '<password>'; GRANT ALL PRIVILEGES ON \`dz_test_%\`.* TO 'dz'@'127.0.0.1';"
```

3. 导出环境变量后跑测试：

```bash
export DZ_TEST_MYSQL_HOST=127.0.0.1 DZ_TEST_MYSQL_PORT=3306 \
       DZ_TEST_MYSQL_USER=dz DZ_TEST_MYSQL_PASSWORD='<password>'
./scripts/test.sh
```

Linux 默认 `DZ_WITH_MYSQL=ON`（根 `CMakeLists.txt`）；关掉它时 mysql 相关用例整文件排除。CI 不配置该服务器 → CI 上这些用例保持 Skip（预期）；配置生效后本地 Skip 数应为 0。

## VSCode 任务

`.vscode/tasks.json` 提供 **8** 个任务：`setup`、`setup (Debug)`、`build`、`run-dev`、`run-dev (no build)`、`stop`、`clean`、`test`。

**无 config 粘性模型**（该机制已在 #168 删除，见 [ADR 0037](../adr/0037-release-only-build-and-install-as-sole-artifact.md)）：任务默认 Release，Debug / asan 需显式指定。任务逻辑全部复用 `scripts/` 下的现有脚本，命令行行为不受影响。

---

## install

`cmake --install` 拷到 `install/<plat>/<arch>/<构建目录名>/`，与 build 同结构但无中间产物、**不含测试**（#168 起测试不再 install）。Ninja 单配置，**不需要 `--config`**。`CMAKE_INSTALL_PREFIX` 在 preset 中设为 `${sourceDir}/install`。

路径用**构建目录的叶子名**而非 `$<CONFIG>`：`asan` preset 的 `CMAKE_BUILD_TYPE` 也是 Release，用 `$<CONFIG>` 会与真正的 Release 目录撞车。

**`install/` 是唯一产物源**（#168）：运行、部署、打包都从这里取，`build/` 降为中间产物目录、禁止直接运行。

**调试符号分离**：Release 带 `-g`，`scripts/install-artifacts.sh` 用 `objcopy --only-keep-debug` + `--strip-debug` + `--add-gnu-debuglink` 把符号抽到 `install/<plat>/<arch>/<cfg>-debug-symbols/`（约 345MB，不进部署包）。部署包因此从 401MB 降到约 66MB。崩溃分析用 `addr2line -e <symbols>/<name>.debug`。Windows 的 `.pdb` 同样不进包，由 tag 流水线单独归档。

---

## conan 依赖更新

`conanfile.py` 的选项变更（如增删 spdlog）需要 `--build=<pkg>` 重新编译对应依赖，否则会复用旧包。

**ConanCenter 上本项目依赖只有 Release 二进制**（zlib / openssl / boost / spdlog / drogon / gtest / sqlitecpp / trantor / jsoncpp / c-ares 逐个核实）。切到 Debug 或 RelWithDebInfo 会让 14 个依赖从头源码编译（Release 基线只要 5 个）。这是「只保留一个日常档」的关键原因。

---

## 项目特有约定（非主流）

### 输出布局直接对齐运行布局

一般项目的 build 与 install 布局不同（GNU FHS 风格的 bin/lib/share 分离）。本项目要求**"运行布局 = 生产布局"**（子进程、第三方 DLL 相对主进程的位置是固定的），因此用 `RUNTIME_OUTPUT_DIRECTORY` 让 build 树内已是最终相对布局。

**但 build 树不是运行来源**（#168）：运行/部署一律从 `install/` 取。`build/` 只负责编译，`install/` 由 `cmake --install`（+ `install-artifacts.sh` 分离符号）产出。

### CTP DLL 有两份

CTP 是闭源库，无法用 conan 统一管理：

- `third_party/ctp/<plat>/` —— 编译期链接的 import lib（`.lib` / `.so`）
- `<output>/`（build 根）—— 运行时由 `POST_BUILD` 拷贝（`.dll` / `.so`），与网关 exe 同目录

---

## 前端构建机制

改前端后必须走 `build` 脚本（AGENTS.md 硬门禁 5）。底层机制（`apps/webui/CMakeLists.txt`）：

1. `find_program(NPM_EXECUTABLE)` 定位 npm
2. `add_custom_command` 先经门控脚本按需安装依赖（`scripts/npm-ci-if-needed.cmake`：`node_modules` 不完整 / 戳文件缺失 / `package-lock.json` hash 或 mtime 变化时才 `npm ci`），再 `npm run build`（底层是 `vue-tsc --noEmit && vite build`）
3. `add_custom_target(dzweb_frontend ALL ...)` + `add_dependencies(dzweb dzweb_frontend)`
4. `POST_BUILD` 用 `copy_directory` 把 `frontend/dist` 复制到 `${DZ_OUTPUT_DIR}/web`
5. `install(DIRECTORY .../dist/ DESTINATION .../web)`

这 5 步解释了**为什么直接 `npm run build` 不生效**：产物只落在 `frontend/dist/`，而 `dzweb` 加载的是 `${DZ_OUTPUT_DIR}/web`。

端口分工：**8080** 是 `dzweb.exe`（drogon）监听的生产/集成端口，托管静态文件 + `/api` + `/ws` + `/health`；**5173** 是 vite dev server，仅 dev 模式 HMR 用，并把 `/api` `/ws` `/health` 反代到 8080。用户验证 UI 永远访问 8080。

---

## 部署产物分析

`install/<plat>/<arch>/Release/` 目录**自包含**，可直接复制到目标机运行：

| 条件 | 结论 |
|---|---|
| 静态 CRT（MT/MTd） | 不需要 vcredist |
| conan profile `compiler.runtime=static` | boost / drogon / spdlog / sqlite 等全部静态链接 |
| drogon 自带 HTTP 服务器 | 不需要 nginx / apache |
| 前端 `web/` 纯静态 | 不需要 Node.js 运行时 |

**唯一例外**：开发时修改 `.vue` 文件需要 npm / Node.js，且仅在构建期。

**部署永远从 `install/<plat>/<arch>/Release/` 取**（唯一产物源，见 [ADR 0037](../adr/0037-release-only-build-and-install-as-sole-artifact.md)）。`build/` 是中间产物目录，禁止直接运行。Debug 产物永不部署。

Linux 部署包约 66MB（已 strip，符号在 `Release-debug-symbols/`）；Windows 的 `.pdb` 不进包，由 tag 流水线单独归档。

---

## 跨平台编译期检查模板

仅支持 64 位：Windows x86_64 / MSVC 与 Linux x86_64 / GCC。**策略接口（C API）不在此限制内。**

非上述组合**必须 `#error`，禁止静默回退**（`((void)0)` 之类会让问题推迟到运行时）：

```cpp
#if defined(_WIN32) && defined(_M_X64)
  // Windows MSVC
#elif defined(__x86_64__)
  // Linux GCC
#else
  #error "unsupported: dztrader requires Windows x86_64 (MSVC) or Linux x86_64 (GCC)"
#endif

#if defined(__i386__) || defined(_M_IX86) || defined(__arm__) || defined(_M_ARM)
  #error "32-bit not supported"
#endif
```

常量与枚举值要避免与常用宏冲突。
