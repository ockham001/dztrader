# ADR 0037: Release 单档 + install 作为唯一产物源

## Status

Accepted（2026-10-27）

## Context

构建体系此前是「Release / Debug 双档 + 粘性 config + build 与 install 双产物源」。2026-10-27 的审查（#168）在本机取证后发现四个问题：

1. **Debug 档代价远超收益**：ConanCenter 上本项目的 10 个依赖**只有 Release 二进制**（zlib / openssl / boost / spdlog / drogon / gtest / sqlitecpp / trantor / jsoncpp / c-ares 逐个核实）。切到 Debug 或 RelWithDebInfo，14 个依赖需从头源码编译（Release 基线只要 5 个）。而项目所有者不交互 debug，Debug 的独有价值（单步、MSVC `/RTC1`）用不上；其运行时检查价值由 ASan 以标准方式覆盖。
2. **两个产物真相源**：README 说「部署复制 `build/<plat>/<arch>/Release/`」，但仓内有 29 处 `install(TARGETS ...)`。实测 `install/.../Release/` 453MB / 779 文件中 393MB 是 101 个测试可执行文件；且 `/api/market-sources/available` 会把 `tests/dzmd_ctp_*_test`（4 个）误报为可用行情源 —— `scan_all_exes` 扫 app_root 两层，`dzmd_*` 前缀全部命中。
3. **config 两个真相源**：preset 名已编码 config（`linux-release` / `linux-debug`），`.dztrader_dev/.config` 粘性文件是第二个。已造成事故：`run-dev.ps1 -Stop` 硬编码 Release 路径，粘性 Debug 时停不掉正在跑的进程。
4. **并发构建无保护**：`.ninja_log` 同一时刻最多 137 条边并发（ninja 默认 `jobs = nproc+2 = 10`），1267 条记录里 385 条 start 时间倒退、44 个输出重复 —— 至少两次 ninja 并发写过同一棵树，且污染了耗时统计。`flock` 当时只在 `gate.sh` 里，人类直接跑脚本即绕过。

## Decision

1. **只保留 `release` 一个日常构建档**。`debug` preset 保留但标注「仅本地、不进门禁、禁止部署」；新增 `asan` preset（Release + 显式 `-fsanitize=address,undefined`，非以 Debug 为基础 —— ASan 文档要求 `-O1` 以上）。不用 RelWithDebInfo：它同样触发 14 个依赖源码编译，却拿不到更多检查。
2. **不引入 Conan `compatibility.py` 插件做 build_type 映射**。虽然实测可行（RelWithDebInfo 映射 Release 后缺包数 14→5），但它是私有机制、需要 `conan config install` 分发，而收益（省依赖编译时间）在一个「编译由 AI 跑」的项目里不成立。**为什么不用另一个方案**：用标准档位 + 不要额外档位，比教 Conan 一套私有映射更符合「减少 DIY」。
3. **`install/` 是唯一产物源**：构建/运行/部署/打包全部从 `install/<plat>/<arch>/<cfg>/` 取，`build/` 降为中间产物目录、禁止直接运行。这同时消掉 Windows LNK1168（运行中二进制被重链占用）—— Linux 侧实测链接器会先 unlink 因而侥幸不触发 ETXTBSY，但「跑的是哪一版」仍然说不清。
4. **`-g` 进 Release，调试符号分离归档**：Release 默认 flags 是 `-O3 -DNDEBUG` 无 `-g`，而 MSVC 侧已有 `/Zi`；7×24 系统崩溃需要行号。但 `-g` 让部署包 60M→401M，故用 Linux 标准做法：`objcopy --only-keep-debug` + `--strip-debug` + `--add-gnu-debuglink`，部署包 66M、符号单独归档 345M 不进包。Windows 的 `.pdb` 同样不进包，由 tag 流水线单独归档。
5. **删除粘性 config**：无参数即 `release`，其他档显式给 preset。
6. **锁下沉到叶子脚本**：`build.sh` / `test.sh` / `build.ps1` / `test.ps1` 各自取锁。`gate.sh` 移除锁 —— 它持锁后再调叶子脚本时，子进程打开同一文件是不同的 open file description，`flock` 仍互斥，父子互等必然死锁（实测确认）。

## Consequences

- **正面**：构建/运行/部署同源，性能结论与线上行为必然一致（只有一份二进制）；部署包 453M→66M 且不含测试；`/api/market-sources/available` 不再误报测试 exe；并发构建不再交错写同一棵树；「跑错 config」这一类事故从结构上消失（没有记忆可跑错）。
- **负面**：`install/` 兼作日常运行目录，其 `Release/` 子目录在打 tag 前不可当作发布快照（部署是 tag 时刻的一次性动作）。`asan` 档的 `CMAKE_BUILD_TYPE` 也是 Release，故 `DZ_INSTALL_DIR` 改按**构建目录叶子名**推导，避免与真正的 Release 目录撞车。
- **已知不修的腐烂**：7 处 `#ifndef NDEBUG` 分支（`log.h` 的调试日志默认值、`string_util.h` 的 0xFE 毒化填充）在 Release-only 下永不参与编译。加 CI Debug job 需每周 40 分钟源码编依赖，不值；在 build-appendix 写明「debug preset 可能失效」。
- 历史性能基线（`docs/agents/build-appendix.md`）本就是 Release 测得，不受换档影响。
