---
description: 构建 C++ + 前端（改任何代码后跑这个）
agent: build
---

构建 dztrader 项目。**改任何代码（前端或后端）后都必须走这个命令。**

config 优先级：`--config` 参数 > `.dztrader_dev/.config` 粘性文件 > `release`。

**Windows（PowerShell）**

```powershell
.\scripts\build.ps1                    # 跟随粘性 config
.\scripts\build.ps1 -Config Debug      # 临时切 Debug（不改粘性文件）
.\scripts\build.ps1 -Target dzmd_ctp   # 只构建单个 target
```

**Linux（Bash）**

```bash
./scripts/build.sh                     # 跟随粘性 config
./scripts/build.sh --config debug      # 临时切 Debug
./scripts/build.sh --config release --target dzcore   # 单 target
```

**Linux 必须用 `--config` / `--target` flag，不能用位置参数** —— `build.sh` 对未知参数会直接 `exit 1`。

**禁止绕过本脚本**：

- 不要直接 `cmake --build --preset ...` —— 会静默丢失 config 粘性，每次都退回 release
- 不要直接 `npm run build` —— 产物只落在 `frontend/dist/`，而 dzweb 加载的是 `build/.../web/`，UI 不会更新且不报错

底层等价命令（仅在排查脚本问题时用，日常不要）：`cmake --build --preset {win,linux}-{release,debug}`

若用户给了 target 名（$ARGUMENTS），用 `--target`（Linux）/ `-Target`（Windows）构建该 target。请实际执行并报告结果。
