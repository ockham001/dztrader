# 第三方归属声明

本文件记录本项目分发物中第三方组件 / 参考实现的归属与许可声明。本项目自身代码以 [MIT License](LICENSE) 发布（Copyright (c) 2026 ockham）。

> 说明：[ADR 0024](docs/adr/0024-sysmon-psutil-as-reference-only.md) 中提到的 `NOTICE` 即本文件；落地文件名以本文件与[《帧契约：系统监控》](docs/frame_contracts/sysmon.md)为准。

## psutil（BSD-3-Clause）

| 项 | 内容 |
|---|---|
| 项目 | psutil —— 跨平台系统与进程信息库（Python / C） |
| 上游 | https://github.com/giampaolo/psutil |
| 许可 | BSD-3-Clause |
| 在本项目中的角色 | **规格与算法参考**，不是依赖，也不随本项目分发 |

### 借鉴内容

`dzsysmon` 的采集实现以 psutil 为参考来确定「该调哪个 OS API、必须处理哪些边界」，再用本项目 C++20 代码自行实现（[ADR 0024](docs/adr/0024-sysmon-psutil-as-reference-only.md)）：

- OS API 选择：Windows 的 `GetSystemTimes`、`NtQuerySystemInformation`、`GetPerformanceInfo`、`IOCTL_DISK_PERFORMANCE`、`GetIfEntry2`；Linux 的 `/proc/{stat,meminfo,vmstat,diskstats,net/dev,cpuinfo}` 等。
- 边界处理语义：32 位计数器回绕、`/proc` 字段在老内核缺失、`/proc/diskstats` 的历史行格式、Linux 内存「可用」口径（`MemAvailable`）及其失真情形、Windows 磁盘计数器需使能。
- 测试不变量：百分比落在合法区间、累计计数器单调不减、每核序列数等于逻辑核数、各核时间和与总量一致（防重复计数）、内存三量关系自洽。

采集实现文件在文件头注明「规格与算法参考 psutil（BSD-3）」，并指向本声明；本项目未翻译其代码，因此不逐函数标注对应关系。

### 不包含 psutil 代码

- **不翻译** psutil 的 C 代码，**不嵌入** CPython 运行时，**不引入** psutil 作为构建、链接或运行时依赖。
- 参考版本：发布版 `7.2.2` 与本地 `8.0.0` 开发快照（见 ADR 0024）。

### 许可全文（BSD-3-Clause）

以下为 psutil 的 BSD 3-Clause 许可原文与免责声明，按第 2 条要求在随附文档中复现：

```
BSD 3-Clause License

Copyright (c) 2009, Jay Loden, Dave Daeschler, Giampaolo Rodola
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

 * Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

 * Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

 * Neither the name of the psutil authors nor the names of its contributors
   may be used to endorse or promote products derived from this software without
   specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

### 不得背书（BSD-3 第 3 条）

- 不得以 psutil 作者或贡献者的名义推广本项目，或以任何方式暗示本项目与 psutil 存在隶属、合作或背书关系。
- 文档、UI 与发布物中**禁止**出现「基于 psutil 构建」「psutil 官方合作」等暗示背书的措辞。本项目对 psutil 的使用仅为设计参考（ADR 0024）。

## 范围说明

- 本文件记录 psutil（`dzsysmon` 的参考实现）的归属与许可，以及**随源码入库**的第三方源码。
- 本文件**当前不完整**：仓内 `third_party/` 下还有一处随源码入库的第三方二进制/头文件
  ——上期技术（CTP）行情/交易 API（`third_party/ctp/{windows,linux}/`，含 `.so` / `.dll` /
  `.lib` 与 `ThostFtdc*` 头文件，经 `apps/ctp` 的 IMPORTED 目标链接并随部署物安装）。
  它由第三方厂商以自有许可提供、不属于本项目，其许可与再分发条件**尚未在本文件收录**，
  待核实后补齐（不阻断当前开发；**分发前必须有结论**）。跟踪：issue #166。
- 随源码入库的**另一类** vendored 源码（无独立第三方许可文本者）是 nlohmann/json 官方单头
  （`libs/strategy_api/include/dztrader/third_party/nlohmann/json.hpp`，MIT，版本 / SHA-256 /
  升级纪律见同目录 `VENDORED.md`）。其 SPDX 许可头随文件本身入库；**随 SDK 分发包**的许可
  声明与打包归 issue #7（[ADR 0026](docs/adr/0026-strategy-sdk-nlohmann-json.md) 决策 5）。
- 其他第三方依赖（Boost、Drogon、spdlog、SQLite、msgpack-cxx 等）以依赖包形式引入、不随源码
  入库，其许可与归属由各自上游 / 依赖包提供，不在本文件重复；本项目自身代码以 MIT 许可发布。
