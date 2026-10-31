# dztrader

[![CI](https://github.com/ockham001/dztrader/actions/workflows/ci.yml/badge.svg)](https://github.com/ockham001/dztrader/actions/workflows/ci.yml)

> 高性能量化交易系统，微秒级延迟，7×24 稳定运行，多进程架构 + 共享内存 IPC

> [!NOTE]
> 项目处于积极开发中（pre-1.0），API 与配置格式可能变化。欢迎 Star/Issue，但暂不建议生产使用。

## 特性

- **微秒级延迟**：行情到策略目标延迟 < 30μs
- **多进程架构**：一策略一进程，崩溃隔离
- **共享内存 IPC**：零拷贝进程间通信
- **多接口架构**：已实现 CTP（行情 + 交易）；XTP 等 20+ 券商 API 为规划项
- **C/C++ 策略接口**：C 接口保证 ABI 兼容（`dz_*` 前缀、`DZ_API` 导出）；C++ 提供 header-only 引擎（`run_strategy`/`StrategyBase`，见 `dztrader/strategy_engine.h`）；C 层纯 C 编译支持随 #7 落地（当前仅按 C++20 编译验证）；策略模板/脚手架后续按需补充
- **Python 绑定**：pybind11，规划中（尚未实现）
- **7×24 运行**：健壮的进程管理、崩溃恢复、过期数据保护
- **资源监控**：内部进程 `dzsysmon` 采集本机资源指标并经事件通道推送 WebUI；WebUI 资源监控页与顶栏 CPU / 内存 / 各盘卷徽章已接实时快照（已实现：系统信息、CPU 总量 / 每核与饱和标记、系统负载、上下文切换 / 中断速率、温度（按芯片分组显示 CPU / GPU / 无线网卡 / 主板 ACPI 等友好名、原始标识见悬停提示）与每核频率（Linux 可用时）、内存与交换（含缓存 / 缓冲）、各卷容量、共享内存目录占用、磁盘 I/O、网络；数值单位与精度统一（字节用 B/KiB/MiB/GiB/TiB，百分比/负载/延迟/温度/计数各有固定小数位；顶栏徽章刻意用整数百分比以适配常驻窄条，见组件文档）；主要标量指标旁有内联趋势小图（历史库 REST 补齐最近 15 分钟 + 实时快照推进，末点即当前值；实际覆盖不足 15 分钟时按实际覆盖标注，补拉降级保留上一轮并标未刷新；历史不可用时静默不显示），点击数值或趋势图直接进入该指标的历史视图；每核 / 每传感器 / 每盘 / 每网卡等次要明细默认收起、点开展开（折叠状态不持久化），顶栏在 ≥lg 只平铺使用率最高的 2 个卷徽章（其余折叠为「其余 N 个」，可点击展开）、<lg 收起为单个状态图标；监控页列数 ≥xl 三列 / md–xl 双列 / <md 单列；历史曲线可查（实时 / 历史视图切换，标量指标的时间范围与粒度选择、min / max 包络、缺口与时钟跳变标记；每核 / 每卷 / 每盘 / 每网卡等维度历史经 REST 端点可查，UI 选择器随后续票）；阈值越限（内存 / 交换 / 全部卷剩余空间 / CPU 饱和）只写日志并按间隔重记、不弹窗（进入 `warning`、重记与恢复 `info`），UI 上以使用率色标表达（监控页内存 / 交换 / 卷与顶栏徽章共用同一组 70% / 90% 阈值）；运行期配置可就地修改（监控页「配置」区展示当前生效配置，admin 可改采样 / 记录 / 推送间隔、阈值与日志重记间隔、指标大类开关、历史开关与保留、磁盘设备范围、虚拟网卡开关等，不重启进程）
- **跨平台**：Windows（MSVC 2022）和 Linux（GCC 13）
- **前端**：WebUI 已实现（Vue3 + dzweb）；Qt 为规划项
- **C++20**：Concepts、Ranges、Format

## 界面预览

| 登录 | 仪表盘（演示数据） | 日志 |
|---|---|---|
| ![登录](docs/images/screenshot-login.png) | ![仪表盘](docs/images/screenshot-dashboard.png) | ![日志](docs/images/screenshot-logs.png) |

> 仪表盘截图中的**账户 / 持仓 / 委托 / 策略 / 消息为演示数据**——页面顶部有常驻「演示数据」横幅，五个面板各带「演示」标签；交易数据链路尚未接入，真实数据由 #6 承接。仅「行情」面板为实时数据。

## 前置条件

| 依赖 | 版本 | 用途 |
| ------- | ------------------- | ----------- |
| CMake | 3.28+ | 构建系统 |
| Conan | 2.x | 包管理器 |
| Ninja | 任意 | 构建生成器（Windows + Linux 统一） |
| Visual Studio | 2022 | Windows MSVC 编译器（需 C++ 桌面开发工作负载，含 vcvarsall） |
| GCC | 13（需 `/usr/bin/gcc-13`） | Linux 编译器。`profiles/linux-gcc` 用 `tools.build:compiler_executables` 把编译器钉死在该路径，以保证 conan 源码包的 `package_id` 与 CI 的 gcc-13 预编译包一致 —— 换成别的版本会混链 |
| Python | 3.9+ | pybind11 绑定（规划中，当前构建不需要） |
| Node.js | 18+ | 前端构建（仅开发期，运行时不需要） |

## 快速开始（4 命令工作流）

**唯一入口**，覆盖编译、调试、停止、清理。日常迭代只需要这几个命令。
**只有一个构建档：Release**（构建/运行/部署同一份二进制）。

```bash
# Windows (PowerShell)
.\scripts\setup.ps1                    # 1. 装依赖 + configure（首次/改依赖时跑）
.\scripts\build.ps1                    # 2. 构建（C++ + 前端 + dist 复制，一次搞定）
.\scripts\run-dev.ps1                  # 3. build + install + 启动 master + 子进程
.\scripts\run-dev.ps1 -Stop            # 4. 停止（强杀 master + 清理僵尸子进程）
.\scripts\clean.ps1                    # （可选）清理 build/ + install/，保留 conan cache

# Linux (Bash)
./scripts/setup.sh                     # 1. 装依赖 + configure
./scripts/build.sh                     # 2. 构建
./scripts/run-dev.sh                   # 3. build + install + 启动 master + 子进程
./scripts/run-dev.sh --stop            # 4. 停止
./scripts/clean.sh                     # （可选）清理
# 浏览器访问 http://localhost:8080
```

### build / install / run 三层

| 目录 | 角色 |
|---|---|
| `build/<plat>/<arch>/Release/` | 中间产物。**不要直接运行**，会被下次构建重链 |
| `install/<plat>/<arch>/Release/` | **唯一产物源**。运行、部署、打包都从这里取（约 66MB） |
| `install/<plat>/<arch>/Release-debug-symbols/` | 调试符号（约 345MB），不进部署包，崩溃分析用 |

`run-dev` 会自动 `build` → `install` → 从 `install/` 启动。

### 切换构建档

无参数即 Release。其他档**显式指定**（不存在「记住上次选择」的粘性文件）：

- `.\scripts\setup.ps1 -Config Debug` / `./scripts/setup.sh debug` — Debug，**仅本地**，不进门禁、禁止部署。
  注意：切到 Debug 会触发 14 个依赖源码编译（ConanCenter 只有 Release 二进制）。
- `cmake --preset linux-asan` — ASan + UBSan，只从 build 树运行，永不 install。

### 并发安全

`build` / `test` 脚本自带构建锁（同一 worktree 串行），人类与 AI 同时操作不会交错写同一棵
Ninja 树。`test` 与 `build` 共用同一把锁。并行跑多个服务栈时用 `DZTRADER_HOME` 隔离：

```bash
DZTRADER_HOME=.cache/home-b ./scripts/run-dev.sh
```

### 调试符号

Release 带 `-g`（不改变代码生成、不影响性能基线）。部署包经过 strip 分离符号，
崩溃时用符号归档解析：

```bash
addr2line -e install/linux/x86_64/Release-debug-symbols/dztraderd.debug -f -C 0x<addr>
```

Windows 侧的 `.pdb` 不进部署包，由 tag 流水线单独归档。

### 命令详解

**setup** — `.\scripts\setup.ps1 [-Config Release|Debug] [-SkipConfigure]`
- 跑 conan install（装/编译依赖）+ cmake configure（生成 build.ninja）
- 首次/改 conanfile.py/改 CMakeLists.txt 配置层时跑
- 会校验 ccache launcher 确实写进了 CMakeCache（否则报错，避免静默裸编）
- Debug 时打印黄色警告

**build** — `.\scripts\build.ps1 [-Config Release|Debug] [-Target <name>]`
- config 默认 Release；要其他档显式 `-Config`
- 构建 C++ + 前端（vite build）+ 复制 dist 到 `build/.../web/`
- 改任何代码（前端或后端）后跑这个，**禁止** `npm run build`
- `-Target dzmd_ctp` 只构建指定 target

**run-dev** — `.\scripts\run-dev.ps1 [-Stop] [-NoBuild] [-Config Release|Debug]`
- 默认含 build（先增量 build 再启动）
- 自动设置 `DZTRADER_HOME=.dztrader_dev`（避免污染生产配置）
- 启动 master（读 `$DZTRADER_HOME/configs/dztraderd.json`，缺失时自动生成默认配置；按其中 md/td/internal 段自动拉起 dzweb + 网关 + 内部进程等子进程）
- `-Stop`：强杀 master + `dztraderd --cleanup-orphans` 清理僵尸子进程
- `-NoBuild`：跳过 build/install 直接启动（快，但可能跑旧代码）
- 启动前若检测到 master 在跑会先停栈（install 不能在运行中替换文件）
- Debug 时打印 `=== DEBUG BUILD - 仅本地调试，禁止部署生产 ===`

**clean** — `.\scripts\clean.ps1 [-IncludeNodeModules]`
- 删 `build/` + `install/` + `apps/webui/frontend/dist/` + `.dztrader_dev/` + `compile_commands.json`
- 保留 `~/.conan2/`（conan cache，下次 setup 不重编依赖，省 20 分钟）
- `-IncludeNodeModules`：连 `frontend/node_modules/` 一起删

### 调试 Debug 流程

```bash
.\scripts\setup.ps1 -Config Debug      # 配置 Debug（会触发 14 个依赖源码编译）
.\scripts\run-dev.ps1 -Config Debug    # build + install + 启动 Debug
# 改代码...
.\scripts\run-dev.ps1 -Config Debug    # 再跑一次
.\scripts\run-dev.ps1 -Stop -Config Debug   # 停止

# 切回 Release（无参数即 Release）
.\scripts\setup.ps1
.\scripts\run-dev.ps1
```

Debug 的定位是「仅本地、按需」：不进门禁、不进 CI、可能随时间腐烂
（7 处 `#ifndef NDEBUG` 分支在 Release-only 下不参与编译）。
需要运行时检查时优先用 `linux-asan` / `win-asan` preset。

### 防止 Debug 部署到生产

结构上已经很难发生：

1. **只有一个日常档**：无参数即 Release，不存在「忘了切回来」的状态
2. **路径分离**：Debug 产物在 `install/.../Debug/`，Release 在 `install/.../Release/`
3. **Debug 警告**：`run-dev` 启动 Debug 时打印黄色 `=== DEBUG BUILD ===`

**部署到生产**：永远从 `install/<plat>/<arch>/Release/` 取（该目录是唯一产物源）。

## 策略示例（stg_demo）

`stg_demo`（`strategies/demo/`）是**常驻示例策略**：由 master 按策略生命周期托管启动（绑定行情源就绪后拉起），启动输出 `[ready]` 横幅；全部交互经 `UI_INPUT` 文本命令驱动，结果以 `OUTPUT_UI` 文本行返回（行首 ASCII 类别标签 + `key=value` + 中文正文）。不解析命令行参数、不做终端交互；回报/成交/持仓/资金/账户状态异步转成文本行。

| 命令 | 说明 |
|---|---|
| `help` / `info` | 命令列表 / 策略身份、行情源、SDK 版本与策略目录 |
| `md add <合约...>` / `md del <合约...>` | 增量订阅 / 退订（退订同时清空该合约 tick 缓存） |
| `md list` / `md dump [n]` | 当前订阅集合 / 最近 tick（每合约缓存上限 100 条、每合约时间正序，`n` 限制每合约条数） |
| `order <账户> <合约> <价格> <手数> [long\|short]` | 下限价开仓单并输出 `order_id`；回报以 `[report]` 行回显（空账户为 `status=REJECTED(拒单)`） |
| `cancel <账户> <订单ID>` / `account [账户]` | 发送撤单请求 / 查询账户登录状态 |
| `quit` | 策略自主退出（exit 0），`on_stop` 输出 `[stop]` 收尾行 |

运行前提：master（`dztraderd`）已启动且 `DZTRADER_HOME` 一致；在 `dztraderd.json` 的 `strategy` 段注册（`name` 必须与可执行文件名一致，`md_source` 必填、无 `args`）：

```json
{"name": "stg_demo", "exe": "<绝对路径>/stg_demo", "md_source": "dzmd_ctp"}
```

示例策略本体（`stg_demo.cpp` + `stg_demo_strategy.*`）只 include 策略 SDK 公开头文件、链接**自包含动态库** `dztrader`（Linux `libdztrader.so` / Windows `dztrader.dll`；带 SONAME 版本化，见 ADR 0038），不定义静态链接宏 `DZ_API_COMPILE_STATIC`、不接触任何第三方库，也不依赖网关/WebUI 内部库；运行时动态库与策略可执行位于同一目录（Linux `$ORIGIN` rpath，Windows 加载器搜索 exe 所在目录）。UI 输入/输出的端到端验证见 `strategies/demo/tests/stg_demo_test.cpp`（帧边界进程内测试，测试侧额外链接内部库作探针）与冒烟测试。

策略初始化失败（`dz_init` 失败）时还没有 UI 会话，`on_error` 无法写 `OUTPUT_UI`；该分支的诊断写 stderr（`std::cerr`，由 master 捕获转发进日志），进程以非零码退出。

## 安装与部署

### 本机安装

```bash
# Windows
cmake --install build/windows/x86_64/Release

# Linux（推荐：自动分离调试符号，部署包 ~66MB 而非 ~400MB）
./scripts/install-artifacts.sh
```

安装到 `install/<plat>/<arch>/Release/`，与 build 同结构但无中间产物、不含测试。
Linux 下 `install-artifacts.sh` 额外把调试符号抽到
`install/<plat>/<arch>/Release-debug-symbols/`（不进部署包）。

> **部署物含第三方 CTP 动态库**：`thostmduserapi_se` / `thosttraderapi_se`（Windows 的 `.dll`、
> Linux 的 `.so`）随 `dzmd_ctp` / `dztd_ctp` 安装在同一目录；`.lib` 仅编译期链接用，不入部署物。
> 它们由上海期货信息技术有限公司提供、不属于本项目；归属、许可与再分发条件见
> [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) 的 CTP 一节。

### 部署到其他电脑（Windows）

复制 `install/windows/x86_64/Release/` 整个目录到目标 Windows x64 机即可运行。
**不需要 npm/Node.js**，不需要 vcredist，不需要 nginx/apache：
- 静态 CRT（MT）→ 无运行时依赖
- boost/drogon/spdlog/sqlite 等静态链接进 exe
- dzweb.exe 自带 HTTP 服务器（drogon），托管 web/ 静态文件

目标机需要：
- Windows 10+ x64
- 数据目录（DZTRADER_HOME 或默认 ~/.dztrader/），内含 configs/

### 部署到其他电脑（Linux）

复制 `install/linux/x86_64/Release/` 整个目录到目标 Linux x64 机。
glibc 版本需 ≥ 构建机（或同等）。

**符号文件不进部署包**：崩溃时用 `Release-debug-symbols/` 里的同名 `.debug`
解析（`addr2line -e <symbols>/dztraderd.debug -f -C 0x<addr>`）。

### 管理面暴露面（默认监听 `0.0.0.0`，无 TLS）

`dzweb` 的默认配置是 `server.listen = "0.0.0.0"`、`server.port = 8080`（`$DZTRADER_HOME/configs/webui.json`）：
**启动即对全部网卡开放**，这是有意为之的局域网用法（多机盯盘）。相应地要知道：

- **无 TLS**：只有 `addListener`，没有证书配置；口令、JWT、全部 REST/WS 流量都是明文
- **JWT 存浏览器 `localStorage`**（XSS 可读）；默认管理员口令是 `88888888`，首次登录会
  **强制改密**（口令最小长度 12，改密后既有 JWT/WS 会话立即失效）
- **非回环监听时启动日志会打一条 WARN**，说明管理面暴露在网络上；它只是提醒，不改变监听
  行为、退出码与 `/health`。该行受 `log.level` 约束（设为 `error` 时不打）；日志原文与判定
  规则见 [docs/components/dzweb.md](docs/components/dzweb.md) 的「监听与暴露面」节
- 缓解措施按场景选：仅本机使用 → `listen` 改 `"127.0.0.1"`；跨机使用 → 内网隔离 +
  强口令（口令强度、会话吊销与强制改密的完整口径见
  [docs/components/dzweb.md](docs/components/dzweb.md) 的「口令与会话」节）+ 前置反向代理做
  TLS。`listen` 只能在配置文件里改，重启生效（运行期不可热改）

> ⚠️ `listen` 只接受 **IP 字面量**（drogon 的限制）：写 `localhost` 之类的主机名会让
> `dzweb` 启动即 FATAL 退出（`Failed to parse IP address`），不是"静默不生效"。

### 版本号

版本遵循 [SemVer 2.0.0](https://semver.org/)：`MAJOR.MINOR.PATCH`，预发布加后缀（`0.1.0-rc1`）。

- **`0.x` 阶段不承诺 ABI 稳定性**（含策略 SDK 的 C ABI）；兼容承诺从 `1.0.0` 起
- 单一真相源：根 `CMakeLists.txt` 的 `project(VERSION)`；`conanfile.py` 从中读取
- 预发布标识由发布流水线从 tag 注入（`DZ_PRERELEASE`），本地构建恒为正式串
- 策略 SDK 的 SONAME 为 `libdztrader.so.<MAJOR>`，ABI 破坏时文件名变化、加载器可拦截
- 运行时查询：`dz_version_string()` 返回完整 SemVer；数值比较用 `dz_version_major/minor/patch`

决策记录见 [ADR 0038](docs/adr/0038-version-strategy-and-sdk-artifact-naming.md)。

**注意**：td 网关（dztd_ctp 等）与策略 SDK 必须同版本**整包**升级并重启 master——混布旧/新版本时 `payload_size_matches` 防御会静默丢弃 TD 数据帧（见 `docs/frame_contracts/td-data-sync.md`）。

## 常见问题

### npm not found / 前端未构建

前端构建需要 Node.js 18+。装好 Node 后重跑 `.\scripts\build.ps1`。
若 cmake configure 时报 "npm not found - frontend will not be built"，说明 npm 不在 PATH。

### 访问 5173 没反应 / 端口对不上

- **8080**：dzweb.exe 监听，生产/集成模式，永远访问这里
- **5173**：vite dev server 监听，仅 dev 模式（`cd apps/webui/frontend && npm run dev`）启用 HMR 热重载
- 用户验证 UI **永远访问 http://localhost:8080**

### 8080 端口被占用

检查 dzweb 进程：
- Windows: `Get-Process dzweb`
- Linux: `pgrep dzweb`

杀掉后重新 `run-dev.ps1`。若仍被占用，检查其他占用 8080 的程序。

### 改了前端但浏览器看不到变化

确认用的是 `build.ps1`（或 `build.sh`），**不是** `npm run build`。
直接 `npm run build` 的产物只在 `frontend/dist/`，dzweb 加载的是 `build/.../web/`
（再经 `install` 到 `install/.../web/`）。

### 构建时报 "waiting for build lock"

同一 worktree 里已有 build/test 在跑（可能是 AI 的会话）。等它结束即可 ——
这是特性不是故障：两个 ninja 写同一棵构建树会产出不可信的结果。
不同 worktree 各有各的锁，互不影响。

### DZTRADER_HOME 是什么

数据目录（配置、共享内存、日志）。
- `run-dev` 脚本默认指向项目内 `.dztrader_dev`（调试用，避免污染生产）
- **已设置则尊重调用方**：`DZTRADER_HOME=.cache/home-b ./scripts/run-dev.sh`
  可并行跑第二个栈
- 未设置且不经过脚本时默认 `~/.dztrader/`
- 部署到其他电脑时，目标机需要设置此环境变量或用默认路径

## 贡献

欢迎贡献代码。请阅读 [CONTRIBUTING.md](CONTRIBUTING.md) 了解贡献指南。

## 许可证

MIT License。详见 [LICENSE](LICENSE)。

## 免责声明

本软件仅供教育和研究目的。交易涉及重大财务风险。作者不对任何交易损失负责。使用风险自负。
