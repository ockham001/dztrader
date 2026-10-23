# ADR 0029: 逻辑持仓净手数类型对齐（DzLargeVolume）

## Status

Accepted（2026-10-21 评审通过；类型与 API 变更已随 `3f6383e` 提交，见决策与 Consequences）

## Context

- BarStrategy 框架要提供 Tier 1「策略逻辑持仓」查询（净持仓、均价、建仓次数等），需要与平台既有类型口径对齐。
- 类型审计结论：下单/成交/委托量已统一为 `DzVolume`（int32）；账户真实持仓 `DzPositionInfo` 用 `DzLargeVolume`（int64）；但策略逻辑持仓 `DzLogicalPosition.net_volume` 与 `dz_set_logical_position(..., int32_t)` 是裸 `int32_t`，与「持仓是聚合量」的语义不一致，也让 BarStrategy 的持仓查询（int64）调用 SDK 时必须窄化。
- `DzLogicalPosition` 是《帧契约：策略》`DZ_FRAME_SET_LOGICAL_POSITION` 的 payload（basic 帧，类型真相源 `libs/core/include/dztrader/core/core_struct.h`）。ID 前缀 32+88+64=184B，尾部 `int32_t + char[4]` 改为 `DzLargeVolume` 后**总尺寸仍 192B、各字段偏移不变**，只把原本的 4 字节 reserved 吸收为高位。
- 策略 SDK 对外交付为自包含动态库（[ADR 0020](0020-strategy-sdk-dynamic-delivery-and-public-surface.md)），策略作者用 SDK 头文件重编；采用本方案的前提是当前没有不可重编的已发布策略（用户已确认）。

## Decision

1. **`DzLogicalPosition.net_volume` 改为 `DzLargeVolume`**：删除 `char reserved[4]`，struct 尺寸/偏移不变；`DzLogicalPosition` 的语义不变（净手数，正=多、负=空）。
2. **`dz_set_logical_position` 的 `net_volume` 参数改为 `DzLargeVolume`**；通配（清空）语义不变。
3. **C++ 层新增类型别名**，放在 `data_type.h` 末尾的 `#ifdef __cplusplus` 段（`namespace dztrader`）：
   - `using Volume = DzVolume;` —— 下单/成交/盘口量（int32）；
   - `using LargeVolume = DzLargeVolume;` —— 持仓/聚合量（int64）。
   C 层保持 `Dz*` 类型名（ADR 0020 要求 C 层可纯 C 编译），别名只服务 C++ 层。
4. **BarStrategy 按语义使用别名**：下单手数用 `Volume`；逻辑持仓与持仓类查询（`current_contracts()` / `max_contracts()` / `PositionState.net_volume`）用 `LargeVolume`；计数与序号（`current_entries` / `bars_since_*`）保持 `std::int32_t`。**不把 `Volume` 用于持仓**。

## Alternatives

- **保持 `int32_t`**：逻辑持仓实际很难达到 21 亿手，功能上无影响；但类型语义与账户真实持仓（int64）不一致，BarStrategy 调用处要显式窄化，将来组合/跨账户聚合还会碰到边界。否决。
- **只加 C++ 别名、不改帧与 API**：别名不解决 SDK 边界仍是 int32 的问题，BarStrategy 仍要窄化。否决。
- **只改帧不改 API（API 收 int32、写帧时宽化）**：帧语义统一但调用面仍窄，收益不抵协议与心智负担。否决。
- **新增 `dz_set_logical_position64` 并保留旧 API**：两套接口并存违反「同一能力不允许两套实现并存」；项目尚未发布，没有兼容包袱。否决。

## Consequences

- **必须同版本、必须重编**：`dz_set_logical_position` 是 C ABI 导出函数，参数宽度变化后旧策略二进制不得调用新 SDK；`DzLogicalPosition` payload 宽度变化后策略进程与平台/dzweb 必须同版本（`Writer::open_frame` 不零化 payload，旧版 writer 的 4 字节 reserved 不是 0，新读者读 int64 会得到脏高位）。SDK 版本/SONAME 策略由 #7 落地时统一处理。
- **帧契约文字无需改**：`docs/frame_contracts/strategy.md` 的 Payload 段以 `core_struct.h` 为类型真相源、不重复字段表；本次按 `general.md` §11.3 在提交说明中记录「payload 字段宽度变化」。
- **无消费端改动**：dzweb 当前不消费 `SET_LOGICAL_POSITION`、无持久化镜像；仓内使用方只有 `api.cpp` 与测试。
- **BarStrategy 文档同步**：`InstrumentContext` 的 `current_contracts()` / `max_contracts()` 返回 `LargeVolume`；`PositionState.net_volume` 为 `LargeVolume`；组件文档 §3 的公开面已按别名更新。
- **回归测试**：新增 `TradeApiTest.SetLogicalPositionLargeVolumeRoundTrip`（超过 int32 上限的净手数原样往返）；既有通配/负值/零值测试保持通过。

## References

- 讨论日期：2026-10-21（BarStrategy 设计评审，类型审计）
- [ADR 0020: 策略 SDK 交付形态与公开面](0020-strategy-sdk-dynamic-delivery-and-public-surface.md)
- 《帧契约：策略》（`DZ_FRAME_SET_LOGICAL_POSITION`）
- `libs/strategy_api/include/dztrader/data_type.h`、`libs/strategy_api/include/dztrader/api.h`、`libs/core/include/dztrader/core/core_struct.h`
- BarStrategy 设计文档（ADR 0027/0028 与 `docs/components/bar_strategy.md`，同批评审后提交）
