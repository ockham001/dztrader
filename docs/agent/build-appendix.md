# 构建与平台附录

`AGENTS.md` 的补充材料。这里只放**漏读也不会立刻出事**的外围细节：工具链、环境激活、IDE 配置、输出布局、部署分析。日常命令与参数见 [README.md](../../README.md)。

CMake + Conan 2.x + Ninja + GTest。本文档记录**项目特有规则与非主流约定**；通用 CMake/Conan 用法不重复。

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

---

## MSVC 环境激活（仅 Windows）

Ninja 不像 VS 生成器那样自动激活 MSVC，需设 `INCLUDE` / `LIB` / `PATH`。两条路径互补，**缺一不可**：

- **命令行**：`scripts/Activate-MSVC.ps1` 用 `vswhere` 调 `vcvarsall.bat x64`；`setup.ps1` / `build.ps1` / `test.ps1` 会自动 dot-source 它
- **VSCode**：`CMakePresets.json` 的 `win-base` 设 `architecture` / `toolset`（`strategy=external` —— CMake 忽略，但 IDE 会读）+ `.vscode/settings.json` 设 `cmake.useVsDeveloperEnvironment=always`（默认 `auto` 在 Preset 模式下不可靠，见 CMake issue #4117）

---

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

- 每个 `*_test.cpp` 独立编译运行，`gtest_discover_tests` 自动注册，输出到 `${DZ_TESTS_DIR}`。标准模板见各模块 `CMakeLists.txt`
- **部分模块的测试有意只编译源码子集**，换取与外部依赖解耦：
  - `strategy_api` 测试不含 `api.cpp`（测试内部类，避免导出符号）
  - `ctp` 测试不含 `md_main.cpp` / `md_api.cpp` / `md_spi.cpp`（避免链接 CTP 库）
  - 代价：这些源文件被重复编译。**这是有意取舍，不要当成待优化的重复劳动去"修复"**
- 需要 `DZTRADER_HOME` 的测试在 `SetUp` 里创建临时目录并设置该变量、`TearDown` 恢复。它们是 `test-base` 强制注入规则的**唯一豁免**（见 AGENTS.md 硬门禁 2）
- **ctest 测试名不等于可执行文件名**：`gtest_discover_tests` 不设 `TEST_PREFIX`，所以按可执行名过滤（例如 `-R dz_smoke`）会匹配不到任何测试。用 `-R` 时按 `Suite.Test` 形式过滤

---

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
