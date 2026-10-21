# ADR 0025: 新增进程类别 Internal，将 dzsysmon 纳入主进程编排

## Status

Accepted（2026-10-19）

## Context

`dzsysmon` 必须由 `dztraderd` 拉起、监护与关闭。但主进程的进程模型是**闭枚举**，当前不认识这个进程：

| 位置 | 现状 | 后果 |
|---|---|---|
| `libs/process/include/dztrader/process/process_kind.h:16-21` | `ProcessKind { GatewayMd, GatewayTd, WebUI, Unknown }` | 无 `dzsysmon` 的位置 |
| `libs/process/src/exe_scanner.cpp:49-64` | 只识别 `dzmd_*`（stem > 5）、`dztd_*`（stem > 5）、精确 `dzweb` | `dzsysmon` 扫描不到，动态注册也不可能 |
| `apps/master/config.h:22` | `Category { GatewayMd, GatewayTd, Strategy, WebUI }` | 无归属类别 |
| `apps/master/config.cpp:123-231` | `dztraderd.json` 顶级段硬编码为 `log/master/shm/webui/md/td/strategy`，**未知段静默忽略** | 写了 `sysmon` 段也不会生效 |
| `apps/master/process_supervisor.cpp:34-52` | 关闭批次只遍历那 4 个 Category | 新类别不进批次 → 整体关闭时收不到定向 `SHUTDOWN`，只能等 10s 硬超时被强杀 |

另有一条硬约束排除了「绕过编排」：主进程在 spawn 成功后按进程名注册事件通道订阅者（`process_supervisor.cpp:862-870`），子进程必须用**同名** `NamedSemaphore` 才能被唤醒；未经主进程注册的进程拿不到唤醒，且 `is_registered_process` 会拒绝其接入请求。因此 `dzsysmon` 不进入主进程注册表就**无法参与事件通道通信**。

值得注意的是，[dztraderd.md](../components/dztraderd.md) 已经在使用「内部进程」这个概念——「非策略子进程（含数据存储，归内部进程）的拉起由配置决定」——但代码中并无对应类别（`dzweb` 作为唯一的非网关内部进程被硬编码为 `webui` 单例）。

## Decision

1. **新增 `ProcessKind::Internal` 与 `Category::Internal`**，`dzsysmon` 归入此类。这是「非策略、非行情/交易网关的内部子进程」的正式类别。

2. **`exe_scanner` 精确识别 `dzsysmon`**，不做 `dz*` 通配。理由：通配会把测试辅助可执行文件（如 `test_worker`）与其他非进程二进制误认成进程。

3. **`dztraderd.json` 新增 `internal` 段**（map，形制同 `md`/`td`），`dzsysmon` 条目落在此段。默认生成器写入该段的默认条目。

4. **关闭批次顺序改为** `Strategy → GatewayTd → GatewayMd → Internal → WebUI`。`Internal` 插在 `GatewayMd` 之后、`WebUI` 之前：监控进程要活到能记录其他进程的退出过程，但没有必要活过 UI 关闭。

5. **本次不做通用化重构**：不把 `dzweb` 的硬编码单例搬进 map，不消除闭枚举。以最小改动满足本需求，通用化另开 issue 跟进。

## Alternatives

- **(B) 一次性把「非策略子进程」数据驱动化、消除闭枚举**：本次否决。改动会覆盖 `dzweb` 的「UI 最后关闭」批次语义，需要重新验证整体关闭编排；该风险与本需求无关，捆进来只会让本次改动难以验证。另开 issue 跟进。
- **(C) 由部署层（systemd/NSSM）拉起 `dzsysmon`，不进主进程编排**：否决。违背用户要求；且未在 master 注册订阅者的进程不会被信号量唤醒，会游离于系统的生命周期、重启策略、日志与 `instance_id` 体系之外。
- **复用 `Category::WebUI` 承载 `dzsysmon`**：否决。会把它塞进「UI 最后关闭」批次，且 `persist_process_config` 会按类别把它写进错误的配置段落。
- **扫描器接受任意 `dz*` 前缀**：否决。会误认测试辅助可执行文件与其他非进程二进制。
- **让 `dzsysmon` 自己注册（子进程自报）**：否决。现有模型是「主进程是唯一登记者」（无握手、无 PID 自报、无心跳），引入自注册需要改动全部进程的启动协议，代价远超收益。

## Consequences

- **master 侧约 6 处改动**：`process_kind.h`（枚举）、`exe_scanner.cpp`（识别）、`config.h`+`config.cpp`（枚举、`internal` 段解析、`category_str`、`default_restart_policy`）、`process_supervisor.cpp`（关闭批次）、`shm_manager.cpp`（`persist_process_config` 的段落路由）。
- **默认重启策略**：`{enabled: true, max_attempts: 5, backoff_sec: 5}`，与 `md`/`td`/`webui` 一致。注意主进程的重启退避是**线性**（`backoff_sec × 连续崩溃次数`），且 spawn 失败不触发重启、运行期非 0 退出才触发——`dzsysmon` 的「平台版本检查失败 ⇒ 拒绝启动并返回非 0」在旧系统上会消耗 5 次重启后停在 `Crashed`（UI 可见，属预期行为）。
- **重启时会追加 `--recover` 参数**（`process_supervisor.cpp:1123-1125`）。这是所有子进程的统一行为，`dzsysmon` 必须定义其语义，不得忽略。
- 未来任何内部进程（如数据存储）可复用 `Category::Internal`，无需再改枚举——这是本次引入类别而非单列一个 `Sysmon` 类别的主要收益。
- 新增 `internal` 段后，`dztraderd.json` 的「未知段静默忽略」行为保持不变（不引入严格校验），避免影响既有部署的配置文件。

## References

- 讨论日期：2026-10-19（dzsysmon 设计 grill）
- 源码位置：`libs/process/include/dztrader/process/process_kind.h`、`libs/process/src/exe_scanner.cpp`、`apps/master/config.{h,cpp}`、`apps/master/process_supervisor.cpp`、`apps/master/shm_manager.cpp`
- 相关文档：[dztraderd.md](../components/dztraderd.md)（进程编排、启动/关闭顺序、内部进程）、[ADR 0023](0023-sysmon-in-process-periodic-sampling.md)、[ADR 0024](0024-sysmon-psutil-as-reference-only.md)
