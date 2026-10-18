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
- **跨平台**：Windows（MSVC 2022）和 Linux（GCC 13）
- **前端**：WebUI 已实现（Vue3 + dzweb）；Qt 为规划项
- **C++20**：Concepts、Ranges、Format

## 界面预览

| 登录 | 仪表盘 | 日志 |
|---|---|---|
| ![登录](docs/images/screenshot-login.png) | ![仪表盘](docs/images/screenshot-dashboard.png) | ![日志](docs/images/screenshot-logs.png) |

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

**唯一入口**，覆盖编译、调试、停止、清理。日常迭代只需要 4 个命令。

```bash
# Windows (PowerShell)
.\scripts\setup.ps1                    # 1. 装依赖 + configure（首次/改依赖时跑，写粘性 config）
.\scripts\build.ps1                    # 2. 构建（C++ + 前端 + dist 复制，一次搞定）
.\scripts\run-dev.ps1                  # 3. build + 设置 DZTRADER_HOME + 启动 master + 子进程
.\scripts\run-dev.ps1 -Stop            # 4. 停止（强杀 master + 清理僵尸子进程）
.\scripts\clean.ps1                    # （可选）清理所有 build 产物，保留 conan cache

# Linux (Bash)
./scripts/setup.sh release             # 1. 装依赖 + configure
./scripts/build.sh                     # 2. 构建
./scripts/run-dev.sh                   # 3. build + 启动 master + 子进程
                                       # 4. 停止: pkill dztraderd; pkill dzweb; pkill dzmd_ctp
./scripts/clean.sh                     # （可选）清理
# 浏览器访问 http://localhost:8080
```

### config 粘性

`setup` 决定 config，后续 `build`/`run-dev` 自动跟随：
- `.\scripts\setup.ps1` → 写 `.dztrader_dev/.config=Release`，后续默认 Release
- `.\scripts\setup.ps1 -Config Debug` → 写 `.dztrader_dev/.config=Debug`，后续默认 Debug
- 临时切 config：`.\scripts\build.ps1 -Config Debug`（不修改粘性文件）
- `clean` 会删 `.dztrader_dev/`（含粘性文件），下次 setup 后重新建立

### 命令详解

**setup** — `.\scripts\setup.ps1 [-Config Release|Debug] [-SkipConfigure]`
- 跑 conan install（装/编译依赖）+ cmake configure（生成 build.ninja）
- 首次/改 conanfile.py/改 CMakeLists.txt 配置层时跑
- 写 `.dztrader_dev/.config` 粘性文件
- Debug 时打印黄色警告

**build** — `.\scripts\build.ps1 [-Config Release|Debug] [-Target <name>]`
- config 来源：`-Config` 参数 > `.dztrader_dev/.config` > Release
- 构建 C++ + 前端（vite build）+ 复制 dist 到 `build/.../web/`
- 改任何代码（前端或后端）后跑这个，**禁止** `npm run build`
- `-Target dzmd_ctp` 只构建指定 target

**run-dev** — `.\scripts\run-dev.ps1 [-Stop] [-NoBuild] [-Config Release|Debug]`
- 默认含 build（先增量 build 再启动）
- 自动设置 `DZTRADER_HOME=.dztrader_dev`（避免污染生产配置）
- 启动 master（读 `$DZTRADER_HOME/configs/dztraderd.json`，缺失时自动生成默认配置；按其中 md/td 段自动拉起 dzweb + 网关等子进程）
- `-Stop`：强杀 master + `dztraderd --cleanup-orphans` 清理僵尸子进程
- `-NoBuild`：跳过 build 直接启动（快，但可能跑旧代码）
- Debug 时打印 `=== DEBUG BUILD - 仅本地调试，禁止部署生产 ===`

**clean** — `.\scripts\clean.ps1 [-IncludeNodeModules]`
- 删 `build/` + `apps/webui/frontend/dist/` + `.dztrader_dev/` + `compile_commands.json`
- 保留 `~/.conan2/`（conan cache，下次 setup 不重编依赖，省 20 分钟）
- `-IncludeNodeModules`：连 `frontend/node_modules/` 一起删

### 调试 Debug 流程

```bash
.\scripts\setup.ps1 -Config Debug      # 切到 Debug（一次性）
.\scripts\run-dev.ps1                  # build Debug + 启动（后续都自动 Debug）
# 改代码...
.\scripts\run-dev.ps1                  # 再跑一次，自动 build + 启动
.\scripts\run-dev.ps1 -Stop            # 停止

# 切回 Release
.\scripts\setup.ps1                    # 切到 Release
.\scripts\run-dev.ps1                  # 后续都自动 Release
```

### 防止 Debug 部署到生产

三层防护：
1. **路径分离**（结构性）：Debug 产物在 `build/.../Debug/`，Release 在 `build/.../Release/`，部署脚本从 Release 路径取
2. **Debug 警告**（视觉）：`run-dev` 启动 Debug 时打印黄色 `=== DEBUG BUILD - 仅本地调试，禁止部署生产 ===`
3. **install 只从 Release 安装**（流程约定）：`cmake --install build/.../Release`；不从 Debug 目录安装

**部署到生产**：永远从 `build/<plat>/<arch>/Release/` 复制，**绝不**从 Debug 复制。

## 策略示例（stg_demo）

`stg_demo`（`strategies/demo/`）是**常驻示例策略**：由 master 按策略生命周期托管启动（绑定行情源就绪后拉起），启动输出 `[ready]` 横幅；全部交互经 `UI_INPUT` 文本命令驱动，结果以 `OUTPUT_UI` 文本行返回（行首 ASCII 类别标签 + `key=value` + 中文正文）。不解析命令行参数、不做控制台交互；回报/成交/持仓/资金/账户状态异步转成文本行。

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

示例策略本体（`stg_demo.cpp` + `stg_demo_strategy.*`）只 include 策略 SDK 公开头文件、链接**自包含动态库** `dzstrategy_sdk`（Linux `libdzstrategy_sdk.so` / Windows `dzstrategy_sdk.dll` + 导入库），不定义静态链接宏 `DZ_API_COMPILE_STATIC`、不接触任何第三方库，也不依赖网关/WebUI 内部库；运行时动态库与策略可执行位于同一目录（Linux `$ORIGIN` rpath，Windows 加载器搜索 exe 所在目录）。UI 输入/输出的端到端验证见 `strategies/demo/tests/stg_demo_test.cpp`（帧边界进程内测试，测试侧额外链接内部库作探针）与冒烟测试。

## 安装与部署

### 本机安装

```bash
# Windows
cmake --install build/windows/x86_64/Release

# Linux
cmake --install build/linux/x86_64/Release
```

安装到 `install/<plat>/<arch>/<config>/`，与 build 同结构但无中间产物。

### 部署到其他电脑（Windows）

复制 `build/windows/x86_64/Release/` 整个目录到目标 Windows x64 机即可运行。
**不需要 npm/Node.js**，不需要 vcredist，不需要 nginx/apache：
- 静态 CRT（MT）→ 无运行时依赖
- boost/drogon/spdlog/sqlite 等静态链接进 exe
- dzweb.exe 自带 HTTP 服务器（drogon），托管 web/ 静态文件

目标机需要：
- Windows 10+ x64
- 数据目录（DZTRADER_HOME 或默认 ~/.dztrader/），内含 configs/

### 部署到其他电脑（Linux）

复制 `build/linux/x86_64/Release/` 整个目录到目标 Linux x64 机。
glibc 版本需 ≥ 构建机（或同等）。

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
直接 `npm run build` 的产物只在 `frontend/dist/`，dzweb 加载的是 `build/.../web/`。

### build/run-dev 跑了错的 config

检查 `.dztrader_dev/.config` 文件内容（`cat .dztrader_dev/.config`）。
- 想永久切：`.\scripts\setup.ps1 -Config Debug`（重写粘性文件）
- 想临时切：`.\scripts\build.ps1 -Config Debug`（不修改粘性文件，仅本次）
- 想重置：`.\scripts\clean.ps1` 删掉 `.dztrader_dev/`，下次 setup 重新建立

### DZTRADER_HOME 是什么

数据目录（配置、共享内存、日志）。
- `run-dev` 脚本自动指向项目内 `.dztrader_dev`（调试用，避免污染生产）
- 未设置时默认 `~/.dztrader/`
- 部署到其他电脑时，目标机需要设置此环境变量或用默认路径

## 贡献

欢迎贡献代码。请阅读 [CONTRIBUTING.md](CONTRIBUTING.md) 了解贡献指南。

## 许可证

MIT License。详见 [LICENSE](LICENSE)。

## 免责声明

本软件仅供教育和研究目的。交易涉及重大财务风险。作者不对任何交易损失负责。使用风险自负。
