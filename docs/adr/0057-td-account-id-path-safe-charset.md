# ADR 0057: td 账户 ID 字符集禁路径分隔符与点号

## Status

Accepted（2026-11-25，#475）

Supersedes（精确到句；未列出的决策与句子全部保留）：[ADR 0056](0056-td-account-identity-investor-code-and-display-name.md) 决策 6 中 dzweb 校验行的 `account_id` 字符集子句「字符集 = ASCII 可见字符 `0x21`–`0x7E`（**禁空格、禁 `:`**、禁控制字符，不限定纯数字）」——**追加禁 `/`、`\`、`.`**。ADR 0056 其余决策（账户身份、展示名、校验分层、`instance_id` 分级、注册表投影等）全部保留；ADR 0056 正文不改，只在 Status 段加引用注记。

契约同步：《帧契约：交易网关配置》§2.2 / §2.3 / §7；契约 rest §2.9.7 预检 4；`GLOSSARY`「账户 ID」词条。变更 checklist 见《帧契约：通用规则》§11.3（本次无帧号 / 帧布局变化，只收窄字段校验规则）。

## Context

[ADR 0056](0056-td-account-identity-investor-code-and-display-name.md)（#468）把 `account_id` 从「平台自造标识」改为**用户输入的柜台真实投资者代码**。而 td 网关在账户 `ext.flow_dir` 为空时用 `flow_dir_ / account_id` 构造每账户 CTP 流文件目录（`TdApi::connect_account_by_id`，契约 td-config §2.3）。

ADR 0056 的字符集 = ASCII 可见字符减空格与 `:`，因此 `/`、`\`、`.` 都合法：

- `/`、`\` 是路径分隔符：`account_id` 可把 CTP 流文件写到 `$DZTRADER_HOME/flow/<进程名>/` 之外，也可让两个账户规范化到同一目录（目录碰撞）；
- `.` 单独作路径分量时，`.` / `..` 会被文件系统归一（`..` 上跳一层）；Windows 还会剥掉文件名的尾点，`A.` 与 `A` 落到同一目录。

风险在 #468 之前现实性低（`account_id` 受控）；改为用户输入后，运维手填或 REST 提交的 `account_id` 即可触发。dzweb 是平台入口的权威校验点、td 网关落盘前用同一份共享校验自检（ADR 0056 决策 6），两处必须逐字节一致。

## Decision

1. **收窄字符集，不做目录名编码**：`account_id` 字符集 = ASCII 可见字符 `0x21`–`0x7E`，**禁空格、`:`、`/`、`\`、`.`**，禁控制字符与非 ASCII；空串仍由各层「非空」判定拒绝。
2. **单一实现**：`libs/platform` 的 `platform::account_id_charset_valid` 是规则唯一实现，dzweb 预检（契约 rest §2.9.7 预检 4）与 td 网关校验（契约 td-config §7）共用；前端表单提示同步（不是安全边界）。
3. **契约同步**：契约 td-config §2.2 / §2.3 / §7、rest §2.9.7 预检 4；`GLOSSARY`「账户 ID」词条。

**为什么不用目录名编码**（#475 的备选方案：对 `account_id` 做可逆无碰撞的 `%XX` 编码）：编码要新增一个纯函数并在文件布局里留一层不可见映射——排查 CTP 流文件时目录名不再等于账户 ID；而禁这三个字节对真实投资者代码零成本（CTP `InvestorID` 是柜台分配的代码，字符集远窄于 ASCII 可见字符），且 #468 之后账户配置处于空库 / 空配置，无迁移成本。`ext.flow_dir` 仍允许用户显式指定目录（用户自担的本地布局），不受本规则影响。

## Consequences

**正面**：`flow_dir_ / account_id` 不需要转义；路径穿越、目录归一与 Windows 尾点碰撞一次性关闭；dzweb / td / 前端三处提示与实现同源。

**负面 / 已知限制**：

- 含 `/`、`\`、`.` 的 `account_id` 从此非法；现存配置（#468 之后为空）无迁移成本，手改配置文件绕过校验属用户自担风险（契约 td-config §5）。
- 本次只处理**路径分量**安全：Windows 设备保留名（`CON` / `NUL` / `COM1` 等）与大小写不敏感碰撞（`ABC` 与 `abc` 落到同一目录）仍可能让 flow 目录创建失败或共享。后果是连接被拒并报错（不是静默串写）；真实投资者代码是柜台分配的数字串，不为此把 Windows 路径细节扩散进账户 ID 规则（编码 / 保留名表）。
- md 侧的 `account_id`（行情登录账号，纯配置字段、不参与唯一性、不作路径分量，契约 md-config）**不使用本函数**，不在本 ADR 范围内。

## References

- 被收窄字符集的来源：[ADR 0056](0056-td-account-identity-investor-code-and-display-name.md) 决策 6
- 路径构造点：`apps/ctp/td/td_api.cpp` 的 `TdApi::connect_account_by_id`；契约 td-config §2.3 `flow_dir`
- 共享校验单一实现：`libs/platform/include/dztrader/platform/account_identity.h`
- 来源票：#475（#468 双轴审查 Spec 轴 (c)6）
