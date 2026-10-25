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

Linux 的 GCC/clang 门禁（含 Release `-Werror`）**复现不了**下列 MSVC-only 失败，它们实际红过 CI（#77×2、#78×1）。改 Windows 分支前先对照；`scripts/preflight.sh` 的 `msvc hazard lint`（`scripts/check_msvc_hazards.py`）会机械检查 R1–R3：

- **IP Helper 头顺序（R1）**：`winsock2.h → ws2tcpip.h → windows.h → iphlpapi.h`。`ws2tcpip.h` 负责引入 `ws2ipdef.h`（`MIB_IF_ROW2` 依赖的接口类型），缺它会报 `'PMIB_IF_TABLE2': undeclared identifier` 等 31 条级联。**禁止直接 `#include <netioapi.h>`**：用户态它依赖 `iphlpapi.h` 提供的 winnt/NDIS 上下文，单引会报 `missing ';' before '__cdecl'` / `NTSTATUS` 重定义。
- **PDH 状态常量（R2）**：`PDH_CSTATUS_*` 不保证由 `<pdh.h>` 引入，需显式 `#include <pdhmsg.h>`。
- **`SPDLOG_DEBUG/SPDLOG_TRACE` 在 Release 被编译掉（R3）**：若 catch 参数只在该宏里被引用，Release 下成为未引用局部变量 → MSVC `C4101` → `/WX` 升 `C2220`。异常路径改用 `SPDLOG_WARN`，或显式引用参数。
- 排查办法：对比失败 TU 与同 job 通过 TU 的 **`showIncludes` 包含树**（CI 日志搜 `Note: including file:`），差异通常就是根因（#77 靠它定位）。

## clangd / compile_commands.json

- `CMAKE_EXPORT_COMPILE_COMMANDS=ON`，Ninja 在 binaryDir 生成
- `setup` 脚本把它拷到**项目根**
- `.clangd` 显式 `CompilationDatabase: .`，避免误命中 `build/<plat>/x86_64/<config>/` 下的原始文件
- **根目录的 `compile_commands.json` 属于"最后跑 setup 的那个 config"** —— 切 Release/Debug 都会覆盖它。建议最后 configure Debug（调试信息全）
- `.vscode/settings.json` 设 `clangd.arguments: []`，覆盖全局可能存在的过时 `--compile-commands-dir`

---

## 预设（CMakePresets.json）

Ninja 单配置，每个 config 独立 binaryDir：`build/<plat>/x86_64/<Release|Debug>/`。

- conan install 只装单个 config，toolchain 锁定 `build_type`。**一次 setup 装一个 config**，需要另一个就再跑一次
- conan profile 禁用 `CMakeUserPresets.json` 生成（`tools.cmake.cmaketoolchain:user_presets=`），conan 只生成 toolchain

预设名：`win-release` / `win-debug` / `linux-release` / `linux-debug`（build 与 test 共用）。

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

- 每个 `*_test.cpp` 独立编译运行，`gtest_discover_tests` 自动注册，输出到 `${DZ_TESTS_DIR}`。标准模板见各模块 `CMakeLists.txt`
- **部分模块的测试有意只编译源码子集**，换取与外部依赖解耦：
  - `strategy_api` 测试约一半只编译内部类；`SDK_API_TESTS` 名单（notify_ui/trade/md/schedule/engine/db/db_instruments/td_ingest_*）额外编入 `api.cpp` + `db_database.cpp`，覆盖 C 接口完整写读路径（名单以 `libs/strategy_api/CMakeLists.txt` 为准）
  - `ctp` 测试不含 `md_main.cpp` / `md_api.cpp` / `md_spi.cpp`（避免链接 CTP 库）
  - 代价：这些源文件被重复编译。**这是有意取舍，不要当成待优化的重复劳动去"修复"**
- 需要 `DZTRADER_HOME` 的测试在 `SetUp` 里创建临时目录并设置该变量、`TearDown` 恢复。它们是 `test-base` 强制注入规则的**唯一豁免**（见 AGENTS.md 硬门禁 2）
- `libs/log` 的测试文件共用 `tests/test_main.cpp`；其中三个通过 `extern test_root()` 使用进程级唯一临时根目录，`atexit` 清理并在失败时记录 `error_code`。这是[单元测试判据 §5.2](../unit-test-standard.md)「重复基建需有据」的有意先例：daily sink 的文件名含日期，固定目录会跨运行残留、并行 ctest 互相干扰；根目录放系统临时目录，不写 `DZTRADER_HOME`。**死亡测试注意**：Windows 上死亡用例会重新执行 `main()` 并得到另一个临时根目录，日志目录须由父进程经环境变量传给子进程（见 `terminate_handler_test.cpp`），父进程再从自己的根目录读日志断言
- **ctest 测试名不等于可执行文件名**：`gtest_discover_tests` 不设 `TEST_PREFIX`，所以按可执行名过滤（例如 `-R dz_smoke`）会匹配不到任何测试。用 `-R` 时按 `Suite.Test` 形式过滤
- **分层标签**（与 `RUN_SERIAL` / `TIMEOUT` 正交）：
  - `fast`：默认单测门禁，秒级用例；所有普通 `gtest_discover_tests` 注册必须带 `PROPERTIES LABELS fast`
  - `slow`：显式穷举/慢速档。`libs/strategy_api` 的 `SDK_SLOW_TESTS`（`date_exhaustive_test` / `date_time_exhaustive_test` / `bar_diff_exhaustive_test`）单独成 target 并打 `slow`；对应抽样版留在 `date_test.cpp` / `date_time_test.cpp` / `bar_diff_test.cpp` 的 `fast` 门禁。`bar_diff_exhaustive_test` 是 tick→bar 差分验收（ADR 0031 决策 10）的全量档：用例逻辑在 `bar_diff_cases.h` 只写一份，fast 档每条路径各跑一次、slow 档用多 seed/多日/高密度覆盖
  - `perf`：无阈值基准项（`PerfBench`（SHM 往返）、`BarPerfBench`（tick→bar 四路径）），不进默认门禁；数值与记录方式见下节「性能基准」
  - 运行：`./scripts/test.sh`（fast）/ `-L slow` / `-L perf`（Windows 用 `-Label`）；等价直调 `ctest --preset linux-release[-slow|-perf]`
  - `slow` / `perf` 不在 CI 默认门禁内（CI 只跑 fast），按需本地手动运行；`fast` 过滤依赖构建期发现，必须先 build 再 test（未构建 target 的 `_NOT_BUILT` 回退不带标签）

---

## 性能基准

基准与秒级单测分层（[单元测试判据 §4.3](../unit-test-standard.md)）：基准只**报告数值**、不做阈值断言，稳定后才升级为 CI 门禁。当前两项，都在 `perf` 标签下：

| 基准 | 源码 | ctest 名 | 测什么 |
|---|---|---|---|
| `dz_shm_perf_bench` | `libs/shm/tests/perf_bench.cpp` | `PerfBench` | SHM 写帧→唤醒→读出往返延迟（README 30μs 目标的同进程下限） |
| `dzstrategy_sdk_bar_perf_bench` | `libs/strategy_api/tests/bar_perf_bench.cpp` | `BarPerfBench` | tick→bar 四条热路径（update / 新 bar / 跨日 / 竞价）的 ns/tick 与 p99 |

### 运行命令

```bash
./scripts/build.sh                                    # 先构建（基准不进默认 target 即可，build 全量包含）
./scripts/test.sh --label perf                         # 跑全部 perf 基准（Release）
./build/linux/x86_64/Release/tests/dzstrategy_sdk_bar_perf_bench   # 直接跑：完整输出最方便
ctest --preset linux-release-perf -V                                  # 或经 ctest 并回显输出
```

Windows：`.\scripts\build.ps1` → `.\scripts\test.ps1 -Label perf`，可执行文件在 `build\windows\x86_64\Release\tests\`。

**必须用 Release**（`-Werror` 与优化同时开启；Debug 数值无参考价值，基准会在报告头打印 `warning: debug build`）。跑之前关闭其它重负载，避免单次中断污染 p99。

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
- **每次门禁跑 5 次取中位数**，再与阈值比较；单次运行的 p99 抖动可达 ±5ns，单次比较会产生假红/假绿。本文件下方基线记录用 7 次，是人工记录的加严做法，不是门禁要求。
- **阈值来自届时 CI runner 的实测基线**，不是 ADR 的 50ns 设计目标，也不是本机基线记录里的数字（机器代差可达数倍）。首次升级时把「哪台 runner、多少样本、算出多少」写进基线记录。

比对本机改动前后时用同一台机器、同一 config，同样取多次运行的中位数。

### 基线记录

格式：每条记录一个四级标题（机器 + 日期），下附基准名与结果表。新增记录追加在末尾，**不修改历史记录**（与 ADR 一致：历史是证据）。

#### 2026-10-23 · Intel Core i7-6700 @ 3.40GHz · Linux x86_64 · GCC 13.4 Release

`dzstrategy_sdk_bar_perf_bench`，1 分钟周期、单合约、`NullBarSink`，9 次运行取中位数。本机 TSC 3.408 GHz，空区间成对读时钟 p50 = 8.80ns，p99 = 9.39ns。

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

它们是 **config 粘性模型**（不是 Release/Debug 配对）：切 config 用 `setup` / `setup (Debug)`，后续任务自动跟随。任务逻辑全部复用 `scripts/` 下的现有脚本，命令行行为不受影响。

---

## install

`cmake --install` 拷到 `install/<plat>/<arch>/<config>/`，与 build 同结构但无中间产物。Ninja 单配置，**不需要 `--config`**。`CMAKE_INSTALL_PREFIX` 在 preset 中设为 `${sourceDir}/install`。

---

## conan 依赖更新

`conanfile.py` 的选项变更（如增删 spdlog）需要 `--build=<pkg>` 重新编译对应依赖，否则会复用旧包。

---

## 项目特有约定（非主流）

### 构建输出 = 最终布局

一般项目的 build 与 install 布局不同（GNU FHS 风格的 bin/lib/share 分离）。本项目要求**"调试布局 = 生产布局"**（子进程、第三方 DLL 相对主进程的位置是固定的），因此用 `RUNTIME_OUTPUT_DIRECTORY` 直接输出到最终位置，省掉每次构建后的 install 步骤。

### CTP DLL 有两份

CTP 是闭源库，无法用 conan 统一管理：

- `third_party/ctp/<plat>/` —— 编译期链接的 import lib（`.lib` / `.so`）
- `<output>/`（build 根）—— 运行时由 `POST_BUILD` 拷贝（`.dll` / `.so`），与网关 exe 同目录

---

## 前端构建机制

改前端后必须走 `build` 脚本（AGENTS.md 硬门禁 5）。底层机制（`apps/webui/CMakeLists.txt`）：

1. `find_program(NPM_EXECUTABLE)` 定位 npm
2. `add_custom_command` 执行 `npm ci` + `npm run build`（底层是 `vue-tsc --noEmit && vite build`）
3. `add_custom_target(dzweb_frontend ALL ...)` + `add_dependencies(dzweb dzweb_frontend)`
4. `POST_BUILD` 用 `copy_directory` 把 `frontend/dist` 复制到 `${DZ_OUTPUT_DIR}/web`
5. `install(DIRECTORY .../dist/ DESTINATION .../web)`

这 5 步解释了**为什么直接 `npm run build` 不生效**：产物只落在 `frontend/dist/`，而 `dzweb` 加载的是 `${DZ_OUTPUT_DIR}/web`。

端口分工：**8080** 是 `dzweb.exe`（drogon）监听的生产/集成端口，托管静态文件 + `/api` + `/ws` + `/health`；**5173** 是 vite dev server，仅 dev 模式 HMR 用，并把 `/api` `/ws` `/health` 反代到 8080。用户验证 UI 永远访问 8080。

---

## 部署产物分析

`build/<plat>/<arch>/<config>/` 目录**自包含**，可直接复制到目标机运行：

| 条件 | 结论 |
|---|---|
| 静态 CRT（MT/MTd） | 不需要 vcredist |
| conan profile `compiler.runtime=static` | boost / drogon / spdlog / sqlite 等全部静态链接 |
| drogon 自带 HTTP 服务器 | 不需要 nginx / apache |
| 前端 `web/` 纯静态 | 不需要 Node.js 运行时 |

**唯一例外**：开发时修改 `.vue` 文件需要 npm / Node.js，且仅在构建期。

**部署永远从 `build/<plat>/<arch>/Release/` 取，绝不从 Debug 取。** 三层防护见 README 的"防止 Debug 部署到生产"。

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
