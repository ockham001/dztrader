# ADR 0048: 顶栏资源徽章的取景原则——时钟入常驻条、卷按固定顺序、按宽度自适应折叠

## Status

Accepted（2026-11-12，#318 顶栏微调）

Supersedes（**仅这一条**）：[ADR 0047](0047-sysmon-clock-self-probe.md) Consequences 中「**顶栏不加时钟徽章**：既有『无 NTP 徽章』断言保持有效；信息在监控页时间卡片」这一句。ADR 0047 的其余决策（自测而非读守护进程、只读不校时、error 档弹窗、不进历史库）全部保留。

契约同步：《帧契约：系统监控》§2.4（卷列表展示顺序 + 顶栏收敛形态）、§2.9（新增「顶栏徽章」一条，**无 payload 变更**）。

## Context

顶栏（`.topbar`，32px 常驻条）此前累积了三条互相冲突的取景规则：

1. **时钟不入口**（ADR 0047）：理由成立——顶栏当时只有一个写死的假 `NTP` 徽章，删除它是为了「不许假数字回归」。但 #302 落地后时钟偏差已是**真实**快照域，且是「所有壁钟判断系统性偏移」的唯一可观测项。它只在监控页卡片里，而盯盘时人不在那个页面。
2. **卷只平铺前 2 个 + 「其余 N 个」**（#127）：那是 `#124` 删除「关注路径」之后的补位措辞，2 这个数字没有依据，只是当时窄条的宽度假设。
3. **卷序按使用率降序**（#122）：导致同一台机器上「系统盘」会随使用率在徽章之间来回换位——每 5 秒推送一次就重排一次，扫读时位置不稳定；而 `df`/Explorer/Netdata/Zabbix/bottom 全部按固定顺序排，没有一家按使用率或「角色」排。

以及一条必须当场否掉的诱因：用户最初要求「系统盘第一、home 盘第二」。**跨平台没有可靠的 role 判定**——Linux 的 `$HOME` 常在 `/` 之内，Windows 的 `%USERPROFILE%` 可能重定向到 UNC 或另一块盘，LVM/btrfs 子卷让「一块盘」本身可争议。加一个 `volumes[].role` 字段意味着把一个**不可靠的推断**写进帧契约并永久维护。而「系统盘在最前」是**固定顺序的副产品**：`/` 与 `C:\` 的挂载深度都是 0，天然排第一。

## Decision

**1. 时钟偏差入口，位于 CPU / MEM 之后、卷之前。** 值 = 紧凑带符号整数毫秒（`+12ms`），状态色按 `clock.offset_warn_ms` / `offset_error_ms` 定档（复用 `thresholdLevel` 与 `clockOffsetLevel` 的既有唯一实现），精确值（1 位小数）+ 误差界 + 来源 + 上次探测时间全部在悬停 title。四态：快照未到 `—`；未探测 `—`；全部失败 `不可用`；配置关闭**整枚不渲染**。

**2. 卷序改为固定顺序：挂载深度升序 → 同深度按挂载点字符串升序。** 与使用率、可读性无关，推送多少次都不换位。顶栏与监控页正文共用 store 的同一个 `volumes` computed，不各自排序。

**3. 卷徽章改「状态点 + 短标签」，不再平铺百分比。** 完整挂载点、使用率、不可用原因在 title 里。CPU / MEM / NTP 三个**数量固定**的徽章保留数值——它们不存在被卷挤走的压力。

**4. 资源组按可用宽度自适应折叠，不再有固定上限。** 参与折叠的只有 CPU / MEM / NTP / 各卷；**MD / TD / STG 服务徽章永不折叠**。放不下时尾部收进一个 `▾ N` 按钮，点击弹出浮层列出被收起项，再点收起（不持久化）。`<lg` 仍收敛为单个状态图标，色标取 CPU / 内存 / NTP / 全部卷中最差。

**为什么不用另外三种方案**：

- **(A) 加 `volumes[].role`（system / home / other）由发送方判定**：否决。role 本身不可靠（见 Context），且为「排序」这一纯展示诉求给帧契约增加永久字段——契约变更 checklist 的五个消费方全部要跟着改，收益只是换个顺序。
- **(B) 按使用率排但把系统盘钉在最前**：否决。等于两套主键（钉住的 + 动态的），换台机器、换个使用率分布就出现「为什么这个在第二、那个在第三」无法解释的顺序；用户在 #318 定稿时也确认改选固定顺序。
- **(C) 沿用「前 2 个 + 其余 N 个」的固定上限**：否决。2 是历史宽度假设而非可解释规则；用户明确要求「能显示全就显示全，显示不全才隐藏」。主流做法（[HIG 工具栏 system-managed overflow menu](https://developer.apple.com/design/human-interface-guidelines/toolbars)、[GitLab show more](https://design.gitlab.com/patterns/show-more/)、[marigold OverflowRegion](https://github.com/marigold-ui/marigold/blob/98076e6fc0b7a88728a0b5b7b3b189bbe1efea64/docs/content/components/layout/overflow-region/index.mdx)）都是**按容器实测宽度**折叠、尾部一个 overflow 入口。

## Alternatives

见上（三个「为什么不用」已覆盖本次的全部候选）。补充两条被否决的次要选项：

- **卷徽章保留百分比、只做宽度折叠**：否决。卷数不定，数字让每个徽章宽一倍，同样的宽度能显示的卷直接少一半；状态条只给颜色不给数字是 Windows 任务栏资源监视器 / macOS 菜单栏的既有形态。
- **CSS 容器查询（`@container`）代替 JS 实测**：否决。容器查询只能做档位式分支（如「<400px 时显示 3 个」），无法回答「这几个到底放不放得下」，而「尽量显示全」正是要这个答案。

## Consequences

- **顶栏与监控页的卷序一致且稳定**：扫读位置不再随使用率变化。副作用是「使用率最高的卷」不再自动排第一——需要看使用率时看颜色（≥70% 黄 / ≥90% 红）与监控页正文。
- **契约 §2.4 改写两段**：卷列表展示顺序、顶栏收敛形态；「不可读卷排末尾」的旧口径随固定顺序取消（`used_ratio = null` 的卷按挂载点参与固定顺序，监控页正文仍按可读性分池折叠）。
- **ADR 0047 的「顶栏不加时钟徽章」作废**：《帧契约：系统监控》§2.9 增一条顶栏徽章口径；`Topbar.spec.ts` 的「不得回归假数字」断言保留（改为断言**真实数据**与四态），`MonitorClockCard.spec.ts` 中「顶栏无徽章」的措辞同步。
- **新增一个前端纯函数**：`sysmonClock.clockBadge`（四态 → 徽章模型），与 `format.ts` 的紧凑毫秒格式化器；两者都有单测，组件只做渲染。
- **`formatAge` 上移**：顶栏也需要「上次探测多久以前」，把 `MonitorView` 的本地 `formatAge` 迁入 `format.ts`（相对时间只留一份实现），监控页改为引用。
- **无协议变更**：快照 / 配置 / REST / WS 全部不动，`schema` 与 `generated.ts` 不变；dzweb 领域服务不需要动。
- **风险**：宽度实测依赖 DOM 布局（jsdom 无布局），因此折叠算法抽成纯函数单测，组件测试用桩替身验证「放不下时出现 ▾」。
- **实测宽度的取景必须是资源组自己的行，不能是整个资源区**：MD / TD / STG 与分隔线同在一个 flex 容器里，若直接拿该容器的 `clientWidth` 当可用宽度，服务簇的宽度会被误算成资源项的空间，折叠晚一步触发、尾部徽章被 `overflow: hidden` 静默截断（实测 1440px、8 项，容器已溢出却一个都没折）。实现上资源项独占一条 `flex: 1 1 auto; min-width: 0` 的子行，折的是这一行（其宽度与折叠结果无关，因此不会出现「折叠 → 变宽 → 再折叠」抖动）。
- **溢出浮层必须逃出容器裁剪**：资源组行为 `overflow: hidden`，容器内绝对定位的浮层会被裁成看不见；因此浮层 Teleport 到 `body` + `position: fixed`（与 `shared/DatePicker.vue` 同一套既有做法），并分别监听浮层与入口两处的点击外关闭。

## References

- spec：#318（WebUI 顶栏资源徽章微调：NTP 徽章 / 卷固定顺序 / 宽度自适应折叠）
- 讨论日期：2026-11-12（`grill-with-docs` 会话，三轮收敛）
- 外部依据：Apple HIG Toolbars（system-managed overflow menu）；GitLab Pajamas「Show more」（`+N more` 形态）；marigold `OverflowRegion`（按容器实测宽度、尾项先隐）；`df -h` / Windows Explorer / Netdata / Zabbix / bottom 的磁盘顺序均为固定顺序
- 相关 ADR：[0047](0047-sysmon-clock-self-probe.md)（时钟自测，本 ADR 只取代其「顶栏不加徽章」一句）、[0034](0034-sysmon-resource-thresholds-log-only.md)（资源类只写日志与色标）、[0035](0035-sysmon-no-volume-disk-mapping.md)（不做卷/盘映射）
- 相关文档：[契约 sysmon](../frame_contracts/sysmon.md)、[dzsysmon 组件设计](../components/dzsysmon.md)
