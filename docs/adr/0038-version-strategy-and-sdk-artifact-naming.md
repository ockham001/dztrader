# ADR 0038: 版本策略与策略 SDK 交付物命名

## Status

Accepted（2026-10-27）

Supersedes：ADR 0020 决策 4 中「SONAME/SOVERSION 由 #7 实施」的**时序**（提前到本 ADR 落地）；符号收敛同样提前实施。ADR 0020 的其余决策（自包含动态库、公开面三不变量、头文件分层）继续有效。

## Context

项目已对外提供策略 SDK，且已产出首个 tag `v0.1.0-rc1`。核查发现六处不一致：

1. **版本号两份真相源**：`CMakeLists.txt` 的 `project(VERSION 0.1.0)` 与 `conanfile.py` 的 `version = "0.1.0"` 各自硬编码，无自动校验。
2. **预发布标识不可见**：`v0.1.0-rc1` 的产物 `dz_version_string()` 返回 `"0.1.0"`，线上跑的是 rc 还是正式版从二进制查不出来。
3. **SDK 无 SONAME 版本化**：`readelf -d` 显示 `SONAME: libdzstrategy_sdk.so`，无 `.so.N`。策略 exe 的 `NEEDED` 正是该名字 —— C ABI 破坏后，旧策略二进制会静默加载新 SDK。
4. **SDK 导出面失控**：`libdzstrategy_sdk.so` 导出 13038 个符号，其中仅 50 个是 `dz_*`；592 个 `dztrader::` 内部 C++ 符号、约 12400 个第三方符号（OpenSSL / zlib / SQLite / Boost / nlohmann）全部泄漏。策略作者可意外调用到随时会消失的内部函数。
5. **`dz_version_hex` 零消费者**：位打包格式（`major<<16|minor<<8|patch`），MAJOR 超过 255 或需要四段即失效，且全仓 grep 仅有定义与声明。
6. **交付物命名不统一**：库叫 `dzstrategy_sdk`，而命名空间、聚合头、未来的 Python 包、主进程都叫 `dztrader*`。

## Decision

1. **版本规范 = Semantic Versioning 2.0.0**。`0.x` 阶段**明确不承诺任何 ABI 稳定性**（含 SDK 的 C ABI）；兼容承诺从 `1.0.0` 起生效，何时发布 1.0 由项目作者判断，**不设外部门槛**。
2. **单一真相源 = 根 `CMakeLists.txt` 的 `project(VERSION)`**。`conanfile.py` 从其读取，消除第二份硬编码。
3. **预发布标识**：CMake 变量 `DZ_PRERELEASE`（如 `rc1`），由 release 流水线从 tag 注入。**`dz_version_string()` 返回完整 SemVer**（`"0.1.0-rc1"` 或 `"0.1.0"`）。不提供独立的 prerelease 函数 —— 那会制造「调用方拼接两个字符串」的重复来源，与主流 C 库（`zlibVersion` / `sqlite3_libversion` / `OpenSSL_version` 均返回完整串）相反。**为什么不用另一个方案**：拆成 `string` + `prerelease` 两函数时，调用方必须自己补连字符并处理空串，任一处写错就得到 `0.1.0rc1` 或 `0.1.0--rc1`。
4. **版本 API 表面**：保留 `dz_version_major/minor/patch`（供数值比较，避免调用方解析字符串）+ `dz_version_string`（完整串）；**删除 `dz_version_hex`**（无消费者、位打包不可扩展）。
5. **SDK 交付物命名 = `dztrader`**：`libdztrader.so` / `dztrader.dll`，CMake target 为 `dztrader_sdk`。理由：与命名空间 `dztrader`、聚合头 `<dztrader.h>`、规划中的 Python 包 `dztrader` 一致，一条命名线对人与 agent 都更省认知。**`DZ_API` 宏名与 `dz_*` 函数前缀不改** —— 它们已是公开的 C ABI 契约，改名会让所有已写好的策略编译失败，与「为统一而改名」的初衷相反。**为什么不用另一个方案**：保留 `dzstrategy_sdk` 的代价是「target 名 ≠ 产物名」以外的长期认知成本；而 `libdztrader.so.0` 带版本后缀后，与二进制 `dztraderd` 在视觉上已不难区分。
6. **SONAME 版本化**：`VERSION ${PROJECT_VERSION} SOVERSION ${PROJECT_VERSION_MAJOR}`，产出 `libdztrader.so → .so.0 → .so.0.1.0`。这是决策 1 的物理载体 —— 没有它，「ABI 变更升 MAJOR」只是纸面约定。
7. **符号收敛提前实施**：Linux 用链接器 `--version-script` 白名单（`dz_*` 之外全 local）+ `--exclude-libs,ALL`；Windows 用 CI 断言（`dumpbin /exports` 数量须为 50）防「漏标 `DZ_API`」。实测可把 13038 收敛到 50。**为什么不用 `-fvisibility=hidden`**：它需要改公共头的 `DZ_API` 宏，且对 header-only 模板入口（`run_strategy`）有微妙副作用；version-script 在链接层工作，不碰源码与头文件。
8. **一步到位迁移，不留兼容别名**：仓库内唯一调用者是 `stg_demo` / `stg_demo_test`；无任何常驻进程链接 SDK 动态库（`dztraderd.json` 无 `strategy` 段）；无外部策略作者。留别名只会违反「同一能力不允许两套实现并存」。

## Consequences

- **正面**：版本号一处可改；线上 rc 与正式版可从二进制区分；C ABI 破坏时加载器能拦（`.so.0` vs `.so.1`）；SDK 导出面从 13038 收敛到 50，第三方与内部 C++ 符号不再泄漏；命名线与命名空间/Python 包统一。
- **负面**：`0.x` 期间的 ABI 破坏不会体现在 SONAME 上（SOVERSION 恒为 0），这是决策 1「不承诺」的直接后果，升级须整包替换（与 ADR 0020 的 co-located 部署一致）。`DZ_PRERELEASE` 由 release 流水线注入，本地构建恒为空串 —— 本地产物的版本串不含 rc 信息。
- **历史文档**：ADR 0020/0021/0022 中的 `dzstrategy_sdk` 作为历史名字保留不改（ADR 一经 Accepted 不修改）；新旧名映射记录于本 ADR。
- 本 ADR 未覆盖、仍属 #7：头文件与导入库的分发打包、`dumpbin`/`ldd` 运行时依赖审计、C 侧编译器检查、第三方许可声明。
