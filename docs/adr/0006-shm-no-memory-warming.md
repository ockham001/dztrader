# ADR 0006: shm 页预加载维持现状——否决周期性内存预热（量化裁决）

## Status

Accepted

## Context

shm 库的页生命周期维护由三部分组成：预读取（创建/打开页文件并 mmap）、定时关闭旧页、触页预热。经全面盘点，"三道防线"在现有代码中已经齐备：

1. 第一道（开盘前批量开）：`MdShmConfig.preload_points`（HH:MM 匹配，自动调度对齐每分钟 :25s 触发 `maintain_md_shm`，见 `apps/ctp/md/md_api_scheduled.cpp`）+ master 广播 `DZ_FRAME_PRELOAD_MD_SHM`（订阅者侧随机 0-5s jitter 预取）。
2. 第二道（盘中巡检）：`check_interval_min` 周期定时器。
3. 第三道（兜底）：`Writer::open_frame` 跨页时 `PagePool::get_page` 慢路径天然兜底。
4. 定时关闭旧页：`close_old_pages`/`release_old_pages` 已随巡检执行。

设计讨论中曾设想增加**周期性内存预热**：从当前写位置向前预热一大块未来内存（forward-only，如 `[nwp, nwp+K]` 跨页连续区间），在 :19/:29/:49 秒错峰触发，以消除跨页首写的 page fault 尖刺。

量化分析否决了该设想：

| 项目 | 数值 |
|---|---|
| minor fault（含零页分配 + 清零 4KB） | ~0.5-1μs/次 |
| 一个 4KB 页容纳 tick 数（~200B/tick） | ~20 |
| 摊销到每个 tick 的 fault 成本 | 几十 ns（相对 30μs 写路径预算为噪声） |
| warm 自身成本（64MB = 16384 次 fault） | ~5-15ms |

warm 并非"额外成本"，只是把写路径迟早要付的缺页挪到空闲时段；但这些缺页摊销后本就是噪声，而 warm 自身成本反超其消除的成本，属负收益。另注意 prefetch（文件+mmap，亚毫秒）与 warm（物理页 fault-in）成本结构不同——按字节算建映射便宜、fault 才贵——大水漫灌式预热不划算。

同步评估的两项关联议题：

- **page_pool 批量优化**：`get_page` 与 `prefetch_page` 存在 ~90 行逐行雷同逻辑；`prefetch_pages` 逐页调 `prefetch_page`（每页各拿一次 meta 进程锁 + stat×2 + 建文件 + mmap）。批量版（单次锁内处理 N 页）收益仅为省 (N-1) 次锁往返 ≈ 无竞争时亚微秒；代价是持锁时间放大 N 倍——密集区巡检与 writer 跨页写入撞锁时，writer 最坏等待从 ≤1×T_page（~6-90μs）放大到 ≤N×T_page（~48-720μs），碰撞概率 ~10⁻⁷ 量级，低但非零且不对称。性能收益为噪声，唯一价值是代码去重，不足以撬动热路径改动风险。
- **touch 跨页安全性**：`Reader::touch_read_position` 只访问已成功映射的成员缓存页，单字节 + `offset_in_page_ < page_size_` 守卫保证永不越界至下一页（否则将是越界 UB）；配合"writer 在推进 nwp 前先创建下一页文件"不变式（`open_frame` 跨页先 `get_page` 建文件，`close_frame` 才 store 新 nwp，均在 meta 锁序内），reader 合法可到达位置的页文件必已存在，不存在 touch 打开未创建页的路径。追平 nwp 时触碰的是稀疏零区，仅产生无害 minor fault。PageCleaner 删除下限取 min(reader/writer page_index, active)；Windows 上映射中删除失败即告警跳过，Linux unlink 后既有映射仍有效。

## Decision

1. **否决周期性内存预热**：不引入任何形式的周期性 warm（含 :19/:29/:49 错峰调度、forward-only 字节区间 warm API）。仅保留现有 `touch_write_position`/`touch_read_position` 单字节触碰（纳秒级，聊胜于无）。
2. **预加载维持现状**：三道防线与关旧页沿用既有机制（`preload_points` / `check_interval_min` / `open_frame` 兜底 / `close_old_pages`），不新增配置字段，不动帧契约与 WebUI。
3. **page_pool 批量/去重暂不动**：热路径代码保持不变；重复代码与逐页加锁行为一并保留。待未来有真实需求再评估——届时建议纯 helper 抽取（保持锁行为逐纳秒等价），或分片批量（每 K 页放一次锁，K=2-4）以兼顾锁收益与尾部风险。

## Consequences

- **Positive**：零代码变更；避免一项被量化证明为负收益的优化；量化账本留档，防止未来翻案重新论证。
- **Negative / Known residual**：
  - `libs/shm/src/page_pool.cpp` 中 `get_page`/`prefetch_page` 的 ~90 行重复代码继续存在（双份维护风险，改页文件规则需同步两处）。
  - MultiWriter 下 `Writer::touch_write_position` 用实时 `nwp % page_size` 算偏移，但 `page_` 可能仍是本写者上次使用的旧页——触碰落在旧页内对应偏移处，仍在映射区间内、内存安全，仅偶尔白碰一次（warm 提示失效一次）。后续如清理，加一行 `page_.page_id() == nwp / page_size` 校验即可。

## References

- 讨论日期：2026-08-23（shm 预加载策略设计讨论结论）
- 源码位置：`libs/shm/src/page_pool.cpp`（get_page/prefetch_page/close_pages_before）、`libs/shm/src/reader.cpp`（prefetch_for_bytes/release_old_pages/touch_read_position）、`libs/shm/src/writer.cpp`（open_frame/prefetch_pages/touch_write_position）、`apps/ctp/md/md_api_scheduled.cpp`（preload_points/check_interval_min 触发）、`libs/shm/include/dztrader/shm/page_cleaner.h`（删除下限策略）
- 相关文档：《帧契约：shm》（docs/frame_contracts/shm.md）、ADR 0004（格式先例）
