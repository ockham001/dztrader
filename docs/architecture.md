# Architecture Reference

## 进程模型：一策略一进程

| 进程 | 职责 | 稳定性 |
|------|------|--------|
| dztraderd | 子进程启停、shm管理、信号量分配 | 最高 |
| 行情进程 | 每行情源一个，独占md channel | 高 |
| 交易进程 | 每接口一个，同接口多账户同进程 | 高 |
| 策略进程 | 每实例一个，单线程事件驱动 | 策略自保 |
| 数据存储 | 特殊策略进程，订阅所有tick持久化 | 高 |
| Qt/Web后端 | 各单端口，可分别启动 | 中 |

## 依赖方向

策略接口是底层，头文件零内部依赖。所有内部模块头文件依赖策略接口层。
源码依赖方向相反：策略接口.cpp依赖core/shm/db。

## 静态库策略

| 类别 | 链接方式 | 原因 |
|------|---------|------|
| 项目内部库 | 静态 | 稳定优先 |
| 策略接口库 | 静态为主(`dzstrategy_sdk_static`, 链接进平台进程与示例策略)；动态(`dzstrategy_sdk`)仅按需 `--target` 构建 | 分发编译后策略的形态预留，ABI 稳定边界 |
| Qt | 动态 | LGPL |
| 闭源SDK | 动态 | 无选择 |
| CRT(MSVC) | 静态(/MT) | 避免vcredist |
| CRT(GCC/Linux) | 动态 | glibc静态链接有兼容性问题 |

## DZTRADER_HOME

环境变量DZTRADER_HOME指定数据根，未设置用系统默认(Win:%USERPROFILE%\.dztrader，Linux:$HOME/.dztrader)。
子目录：configs/ shm/ logs/ cache/ strategies/ db/

## IPC通道详情

1个event channel + N-1个md channel：

| 通道 | 写入 | 锁 | 数据 |
|------|------|-----|------|
| event(1个) | 多进程 | 进程锁 | 交易回报/下单/调度/定时器/订阅请求等 |
| md(N-1个) | 单行情进程 | 自旋锁(仅订阅者列表) | tick/深度 |

### 读写语义

event(1个,多进程写,进程锁)：加锁→读next_write_pos→写帧→原子更新next_write_pos→信号量通知→解锁
md(N-1个,单进程写,自旋锁仅订阅者列表)：写帧→原子更新next_write_pos(release)→自旋锁→通知订阅者信号量→释放自旋锁
读：比较next_write_pos与本地read_pos，有新数据直接读(无锁)
策略用单个信号量，dz_wait()唤醒后轮询各通道next_write_pos判断来源
通道命名：event=`dzevent`（`CHANNEL_NAME_EVENT`），md=`<行情源名>`（与行情进程名一致）
订阅：策略经 `dz_subscribe`/`dz_unsubscribe` 直发行情进程（`REQUEST_MD_SUBSCRIBE`，`instance_id`=行情进程名，契约 md-subscription），不再经 master 转发
行情源：公开策略 SDK 单行情源，内嵌 `DzContext`（`dz_next_md(ctx)`/`dz_preload_md(ctx)`/`dz_md_source_name(ctx)`）。源名由 master 经策略配置 `md_source` 注入环境变量 `DZTRADER_MD_SOURCE`，`dz_init` 必选（缺失/为空返回 NULL + `DZ_EC_MD_SOURCE_NOT_CONFIGURED`）；无 `DzMdSource` 句柄与 create/destroy API。订阅期望集合由 SDK 维护，md 重启后 `dz_on_md_started` 全量补订阅（不重开 reader）；策略 reader 与进程同生命周期，master 在 spawn 前预注册、退出后清理（1013–1016 保留给手工/外部进程）

## 策略接口约束

- 纯C(dz_前缀,DZ_API导出),二进制兼容
- 唯一允许暴露的第三方库:nlohmann/json(内置)
- 不提供日志,提供dz_strategy_home()返回策略exe所在目录
- 实盘/回测不放进C接口,回测通过回放进程往shm写历史数据
- K线各策略自行计算,C接口只提供tick
- 驱动:事件驱动(默认)+硬轮询(后期)

## 依赖禁止

- `dzstrategy_sdk` 面向策略分发，仅允许暴露 `nlohmann/json`；实现依赖 dzcore/dzshm（`api.cpp` 使用 `LastError`、shm writer/reader），依赖方向为内部库，非 ABI 暴露面
- dzlog独立库,仅平台进程使用

## Reference Projects

- vnpy：接口数量和设计参考
- kungfu量化：架构参考（但实际性能不佳）
- 掘金量化：接口参考（不开源）
