# ADR 0007: 交易业务帧 payload 末尾增加账户级状态变更序号 seq

## Status

Accepted

## Context

TD 数据同步机制需要把"数据库持久化"与"共享内存实时通道"两条数据通路衔接起来：DB 侧记录账户内各实体的状态变更，SHM 侧每帧承载一次状态变更，两侧各自推进。要判断"某一帧携带的状态是否已落入 DB / DB 的最新状态是否已推入 SHM"，需要一个**单调递增的水位坐标**，作为 DB+SHM 衔接的语义锚点——这就是账户级 `seq`。

`DzOrderReport` / `DzTradeReport` / `DzPositionInfo` / `DzTradingAccount` 是四类 td 业务帧的 payload，每次账户内状态变更都会产生一帧（或其状态落账对应一帧）。它们目前没有统一的序号字段，无法表达"这是账户内第几次状态变更"。

## Decision

在四个 td 业务帧 payload 结构体的**末尾**各增加一个 `uint64_t seq;` 字段，语义为**账户级状态变更序号（账户内全类型共享、跨日累积单调）**：

| 结构体 | 新增位置 |
|--------|----------|
| `DzOrderReport` | 末尾（`DzOrderRemark remark` 之后） |
| `DzTradeReport` | 末尾（`DzTradeId trade_id` 之后） |
| `DzPositionInfo` | 末尾（`char reserved[3]` 之后） |
| `DzTradingAccount` | 末尾（`char reserved[4]` 之后） |

- 字段类型 `uint64_t`，8 字节对齐由 `DZ_DECLARE_ALIGNED_STRUCT` 编译期保证，无需额外对齐补丁。
- 追加在末尾而非中间，避免破坏现有字段偏移；保留原有 `reserved` 字段不动。
- 序号为账户级：同一账户内委托/成交/持仓/资金全类型共享同一递增序列，跨日累积、单调不回退，作为 DB 落账水位与 SHM 帧序的衔接坐标。

## Alternatives

- **字节位置位点（写入方在帧内任意字节写入序号）**：位点无法承载语义，且通道清理（定期删除旧页文件、序号单增不回退）会破坏对"位点值"的持续性读取，无法作为可靠水位坐标。**否决**。
- **帧头扩展字段**：序号是业务状态语义而非通道元数据，帧头由 `DzFrameHeader` 统一管理、不应承担账户业务水位；且四类帧各自需要按账户独立计数，放在 payload 内按结构体承载更自然。**否决**。
- **`reserved` 复用**：现有 `reserved` 仅够按字节对齐，容量与语义均不符（账户级单调序号需要独立 `uint64_t` 语义字段）。**否决**。

## Consequences

- **ABI 变更**：四结构体 `sizeof` 增大，与旧版本进程（旧 md/td/sdk/dzweb 编译产物）混跑时不兼容，需整组升级。
- **td/SDK/dzweb 同仓同步**：后续任务在同一仓库内同步消费新字段（td 写端 Task 5/6、SDK ingest Task 8、dzweb ingest Task 9），同仓编译自动一致。
- **`payload_size_matches` 防御性丢帧**：`strategy_engine.h` 的 `payload_size_matches` 与 td 写端 `write_struct` 均按 `sizeof(T)` 判定，全量重编译后两端自动一致，无需额外登记，天然避免新老布局混跑丢帧。
- 新增测试 `TdBusinessFramesCarrySeq`（`dzmd_ctp_frame_types_test`）防御字段存在性与 8 字节对齐回归。

## References

- 帧契约：[00-general](../../docs/frame_contracts/00-general.md)（帧布局与变更规则）
- [12-td-order](../../docs/frame_contracts/12-td-order.md)
- 类型层真相源：`libs/strategy_api/include/dztrader/struct.h`
