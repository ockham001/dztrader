# 帧契约：日志

本文件覆盖 `DZ_FRAME_SET_LOG_CONFIG`、`DZ_FRAME_FLUSH_LOG`、`DZ_FRAME_RTN_LOG_CONFIG` 三个帧。总则见《帧契约：通用规则》。

三个帧均使用 `DzExtInstFrameHeader` 扩展头，`instance_id` = 目标/来源逻辑实例（各进程使用进程名，见总则 §5）。

类型层真相源：`libs/platform/include/dztrader/platform/log_config.h`（`LogConfig`，校验/规范化/持久化唯一真相源）。

---

## DZ_FRAME_SET_LOG_CONFIG

**语义**：设置目标实例日志配置，RFC 7386 JSON Merge Patch 增量更新
**方向**：dzweb → 指定实例（帧头 `instance_id` 匹配者）。dzweb 自身为目标时不走 SHM，直调 `LogConfig::set_log_config`（等价语义）
**Payload**：JSON

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `level` | string | 否 | 过滤级别，缺失=不修改 |
| `flush_on` | string | 否 | flush 阈值级别，缺失=不修改 |

```json
{"level": "info"}
{"level": "info", "flush_on": "warning"}
{}                                    // 空对象：无操作，仍回 RTN（当前值）
```

**时序**：前端提交触发 → 必回 `RTN_LOG_CONFIG`（总则 §7 极端情况兜底适用）
**校验**：

- `level`/`flush_on` 有效值为 spdlog 规范全称（全小写、大小写敏感）：`trace`/`debug`/`info`/`warning`/`error`/`critical`/`off`。`warn`/`err` 为输入别名，接受但存储时规范化为 `warning`/`error`；空字符串非法；大写（如 `INFO`）非法
- `null` 语义、未知字段忽略、空对象语义、校验失败四件套：见总则 §8
- 失败时回 RTN（回滚后的旧值），`RTN` 中永不出现 `warn`/`err`

---

## DZ_FRAME_FLUSH_LOG

**语义**：触发目标实例立即 flush 日志缓冲
**方向**：dzweb → 指定实例。dzweb 自身为目标时直接 `spdlog::default_logger()->flush()`
**Payload**：空

**时序**：前端"刷新日志"按钮触发 → **无 RTN**
**约束**：非 SET 类请求：不修改配置、不更新镜像、不回报；目标实例不存在/无响应时无任何提示（前端不做 pending，见下）

---

## DZ_FRAME_RTN_LOG_CONFIG

**语义**：上报当前日志配置
**方向**：各进程 → dzweb
**Payload**：JSON，**始终全量**

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `level` | string | 是 | 当前生效级别，始终为规范全称 |
| `flush_on` | string | 是 | 当前生效 flush 阈值，始终为规范全称 |

```json
{"level": "info", "flush_on": "warning"}
```

**触发场景**：

1. `SET_LOG_CONFIG` 应用后（成功=新值，失败=回滚旧值）
2. 进程启动初始化完成后上报（时点由各进程自行安排）
3. `QUERY_FULL_SNAPSHOT` 响应（总则 §7）

**镜像**：
- dzweb 以 `instance_id` 为 key，更新镜像 `log_config` 域并 WS 推送（消息名 `log_config`，见契约 10）
- dzweb 自身 `log_config` 同样纳入镜像（dzweb 自身 SET 成功/失败后经同路径回推）

**前端义务**：
- SET 后进入 pending，以对应实例的 `log_config` 推送清除；超时按总则 §7 兜底（提示 master/目标不可达）
- FLUSH 无反馈：前端不设 pending

## 持久化与默认值

- SET 应用成功后持久化到目标进程配置文件（路径/section 由实现决定，默认 section `/log`）
- 启动加载：文件/section 缺失或内容非法时用默认值并修复文件（自愈），不抛异常、不发 RTN
- 默认值：`{"level": "debug", "flush_on": "info"}`
