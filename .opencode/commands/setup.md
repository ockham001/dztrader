---
description: 装依赖 + cmake configure（首次克隆 / 改 conan 依赖 / 切 config / 新建 worktree）
agent: build
---

在 dztrader 项目里执行 setup：安装 Conan 依赖并 configure CMake，写入 config 粘性文件。

**何时需要跑**：首次克隆、`conanfile.py` 或 `profiles/` 变更、切换 Release/Debug、新建 git worktree（conan 缓存独立）。日常改代码不需要。

**Windows（PowerShell）**

```powershell
.\scripts\setup.ps1                    # 装 Release + configure Release
.\scripts\setup.ps1 -Config Debug      # 装 Debug + configure Debug
.\scripts\setup.ps1 -SkipConfigure     # 只装依赖不 configure
```

**Linux（Bash）**

```bash
./scripts/setup.sh release             # Release（位置参数）
./scripts/setup.sh debug               # Debug
./scripts/setup.sh release --skip-configure
```

注意两个平台的参数风格不同：PowerShell 用 `-Config Debug`，Bash 用**位置参数** `debug`。

副作用：会激活 MSVC 环境（Windows）并把 `compile_commands.json` 拷到项目根（供 clangd 使用）。写入 `.dztrader_dev/.config` 粘性文件，后续 `build` / `run-dev` 自动跟随该 config。

若用户给了额外参数（$ARGUMENTS），按上述风格套用到对应平台的写法。

请实际执行命令并报告结果，不要只复述命令。
