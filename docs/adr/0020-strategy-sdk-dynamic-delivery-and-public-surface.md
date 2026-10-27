# ADR 0020: 策略 SDK 交付形态与公开面（自包含动态库 + 纯 C/C++ 头文件分层）

## Status

Accepted（2026-10-18）

修订 [ADR 0016](0016-strategy-sdk-embedded-sql-drivers.md) 的「策略二进制单文件分发」前提（见 Supersedes）。

## Context

- 现状（`docs/architecture.md`）：静态 SDK（`dzstrategy_sdk_static`）为主，动态 SDK（`dzstrategy_sdk`）标注为「分发编译后策略的形态预留」；仓内唯一使用者 `stg_demo` 链静态 SDK；动态 SDK `EXCLUDE_FROM_ALL`、无安装规则。
- ADR 0016 的「单文件分发」指：策略二进制静态内嵌 SQL 驱动、运行时无 DB 依赖；其前提是策略在本仓工具链内构建（依赖 Conan 第三方）。
- 用户诉求（2026-10-18）：面向外部策略作者，交付物必须是**自包含动态库**（Windows `dzstrategy_sdk.dll` + 导入库 `.lib`；Linux `libdzstrategy_sdk.so`），第三方（SQLite、Boost.MySQL/OpenSSL/ZLIB、nlohmann、Boost 等）全部静态吸收。策略作者只需本 SDK 头文件 + 动态库即可构建与运行，不接触任何第三方。静态库仅内部使用，不作为交付物。
- 核查结论（2026-10-18）：
  - 公共头文件已零第三方（只 include 标准库与自身），`<dztrader.h>` 仅凭 SDK include 路径可被 C++20 编译器接受。
  - 现成 `libdzstrategy_sdk.so` 的运行时依赖只有 `libstdc++/libm/libgcc_s/libc/ld-linux`（第三方确已静态吸收），但实测导出 13,035 个动态符号（含 OpenSSL 等），只应导出 `dz_*`；无 SONAME/SOVERSION；无安装规则。
  - C 层未真支持纯 C：`struct.h` 在 C17 下不可编译（对齐/静态断言宏用了 C23 关键字）；工程只有 CXX 语言、无 C 侧编译检查。README 现称「C 接口…可直接用 C 开发」，尚不成立。
  - 未来命令面（UI_INPUT 内容）若采用 JSON，不得把 nlohmann 引入公共头。

## Decision

1. **对外交付形态 = 自包含动态库**：Windows `dzstrategy_sdk.dll` + 导入库；Linux `libdzstrategy_sdk.so`。第三方依赖全部静态吸收；运行时只允许系统基础库（glibc、libstdc++、libgcc、libm；Windows 侧 `/MT` 静态 CRT + 系统库）。静态库 `dzstrategy_sdk_static` 仅仓内/平台内部使用，不作为交付物、不承诺其 CMake 暴露面。
2. **公开面三不变量**（缺一不可）：
   - 头文件集合零第三方：C 层为纯 C（C11）可编译；C++ 层仅依赖标准库（C++20）。
   - 动态库导出符号仅 `dz_*`（C ABI），第三方与内部 C++ 符号不可见。
   - 运行时依赖闭包零第三方。
3. **头文件分层与 C 目标**：C 层（`api.h`/`data_type.h`/`error.h`/`struct.h`/版本头）真支持纯 C，覆盖全部底层能力；C++ 层（`strategy_base.h`/`strategy_engine.h`/`date_time/*`）是可选高级封装。聚合入口 `<dztrader.h>` 双入口：C 编译时只含 C 层，C++ 编译时含全量。
4. **安装/分发包**：headers + 动态库 + 导入库 + 版本/ABI 说明 + 第三方许可声明；动态库带 SONAME/SOVERSION。具体打包、dependents 审计与 C 编译器检查由 #7 实施。**SONAME/SOVERSION 与符号收敛的时序已由 [ADR 0038](0038-version-strategy-and-sdk-artifact-naming.md) 提前（2026-10-27 已实施）**，其余部分仍属 #7。
5. **仓内示例 `stg_demo` 切换为动态 SDK 链接**，作为该交付形态的第一个消费者与端到端验收载体（示例本身仍只 include SDK 公开头文件）。
6. **命令协议约束**：UI_INPUT 内容可定义文本或 JSON wire format，但公共头不得引入第三方解析库（含 nlohmann）；解析实现由策略自选。

## Alternatives

- **继续以静态 SDK 为对外交付**：外部策略作者需要本仓工具链 + Conan 第三方依赖，与「纯净、自包含交付」目标冲突；否决（仓内静态用法保留）。
- **header-only SDK**：策略需自行链接 SQL/传输等第三方实现，无法自包含；否决。
- **静态链入 libstdc++/libgcc（只依赖 glibc）**：体积与异常/内存跨 CRT 边界成本上升、收益低；否决（允许系统 libstdc++/libgcc）。
- **动态库插件式 DB 驱动**：ADR 0019 已否决；本 ADR 不改变该结论。

## Consequences

- ADR 0016 的「策略二进制单文件分发」前提对外部交付路径失效：外部策略部署形态变为「策略 exe + SDK 动态库」（co-located，Linux 需 `$ORIGIN` rpath 或等价搜索路径）；其静态内嵌 SQL 驱动、单文件形态继续作为静态 SDK 的内部用法有效。
- 公共头文件与动态库的改造清单进入 #7：C 兼容宏与 C 编译器检查、`DZ_API` 可见性、`--exclude-libs`/导出表收敛、SONAME/ABI 版本、install 规则与第三方许可声明、`ldd`/`dumpbin` 运行时审计。
- README 的「C 可直接开发」表述在 C 层改造落地前不成立，需随 #7 修正；示例改造只校准策略示例章节。
- 示例 `stg_demo` 由静态 SDK 改为动态 SDK 后，`dzstrategy_sdk_static` 在仓内暂无可消费方（保留，不删除）。
- 策略作者构建面收敛为「SDK include + 动态库（+ Windows 导入库）」，无需第三方。

## Supersedes

- [ADR 0016: 策略进程内嵌 SQL 驱动（SQLite + Boost.MySQL）](0016-strategy-sdk-embedded-sql-drivers.md) — 其「策略二进制单文件分发」前提仅继续适用于静态 SDK 的内部用法；对外交付路径改由本 ADR 定义。

## References

- [ADR 0019: 策略 SDK 数据库后端范围收敛](0019-strategy-sdk-db-backend-scope.md)（策略可见后端范围，不受本 ADR 影响）
- 核查依据：`docs/architecture.md`、`libs/strategy_api/CMakeLists.txt`、`libs/strategy_api/include/dztrader/data_type.h`
- 实施跟踪：#7
