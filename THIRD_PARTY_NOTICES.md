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

## 上期技术 CTP 行情 / 交易 API（厂商二进制，未附许可文本）

| 项 | 内容 |
|---|---|
| 项目 | 上期技术综合交易平台（CTP）行情 API `thostmduserapi_se`、交易 API `thosttraderapi_se` |
| 厂商 | 上海期货信息技术有限公司（SFIT，上海期货交易所全资子公司） |
| 上游 | SimNow 平台（厂商官方仿真 / API 下载入口）https://www.simnow.com.cn/ ；接口文档 http://www.sfit.com.cn/5_2_DocumentDown.htm |
| 版本 | 见 [third_party/ctp/VENDORED.md](third_party/ctp/VENDORED.md)「上游与版本」（不在此重复，避免升级时漂移） |
| 许可 | **厂商未提供许可条款文本**：发布包内不含许可或使用协议；官方站点（SimNow）的下载与协议页面在本次核实中未能直接读取，未取得可引用的再分发授权（见「核实记录」）。CTP API 为闭源专有软件，其使用与分发受厂商及期货公司 / SimNow 各自协议约束 |
| 在本项目中的角色 | **运行时依赖**（非参考实现）：`apps/ctp` 以 IMPORTED 目标链接，二进制随 `install/` 部署物安装 |
| 本项目的分发方式 | **原样随附**：随本项目部署物一起分发，详见「再分发条件」 |

### 结论

**采用「原样随附分发」口径，不登记为「不可分发项」**，现有部署流程（随 `install/` 分发）不变。

口径依据与理由：

1. **项目所有者决策**（2026-10-27，`/implement #166` 会话；同日记入 issue #166 评论）：同意按
   社区先例采用可分发口径，不采用票内「无法确认 → 不可分发」的兜底；
2. CTP API 的既定用途就是被集成进交易终端、随终端软件交付给使用者——厂商通过 SimNow 与
   期货公司向开发者 / 终端厂商发放 API 包；
3. 发布包内不含禁止再分发条款，也未附需签署的许可文本；
4. 同类开源项目长期如此分发（见「社区先例」）；本项目不修改厂商二进制、不单独售卖。

「对外正式分发前建议取得厂商书面确认」只是合规建议，**不改变**上述分发口径，也不需要构建 /
启动侧的任何限制。

### 入库文件

`third_party/ctp/{windows,linux}/` 共 18 个受版本控制的文件：

- 二进制：`thostmduserapi_se.{dll,lib}`、`thosttraderapi_se.{dll,lib}`（`windows/`），
  `thostmduserapi_se.so`、`thosttraderapi_se.so`（`linux/`）
- 头文件（编译期使用）与错误码表（文档性数据；两者均不入部署物）：`ThostFtdc{Md,Trader}Api.h`、
  `ThostFtdcUserApi{DataType,Struct}.h`、`error.dtd`、`error.xml`（两平台各一份）

### 来源可复核

全部文件来自厂商发布包（发布包文件 / 内部版本 / SHA-256 与逐文件哈希见
[third_party/ctp/VENDORED.md](third_party/ctp/VENDORED.md)；发布包不随仓库入库）：

- 6 个二进制与发布包**逐字节相同**（取 `*64_se_windows` / `*_linux64` 目录）；
- 文本文件只做编码与行尾归一：`GB18030 → UTF-8`、`CRLF → LF`；`error.xml` 的 XML 声明随之
  改为 `utf-8`（唯一内容偏离，避免解析器按 GB2312 解释 UTF-8 字节时中文乱码）；
- 机械步骤、逐文件 SHA-256、校验规则与升级纪律见上述 `VENDORED.md`（由
  `scripts/vendor_ctp.py` 生成 / 校验，`--check` 即「仓库文件 = 记录」）；
- 发布包内只有《隐私政策》《合规使用说明》两份看穿式终端采集库文档（见「随包文档（附注）」），
  没有 API 许可 / 再分发条款。

### 随包文档（附注）

发布包内另附（不在本仓库）：

- `PrivacyNotice/隐私政策_看穿式终端采集库-20250530.pdf`
- `PrivacyNotice/合规使用说明_看穿式终端采集库-20250530.pdf`

两份文档是**使用 CTP 接入实盘**时的合规要求（面向期货公司 / 终端厂商的看穿式终端信息采集
告知义务），与是否分发本项目无关；本项目不替代集成方履行该义务。列此仅为说明：厂商包内没有
API 许可文本，只有采集库合规文档。

### 再分发条件（本项目口径）

1. 只能随本项目部署物分发：厂商二进制不修改、不改名、不逆向，不单独出售或单独提供 CTP API。
2. 保留厂商版权信息：`ThostFtdc*Api.h` 的 `@company` 版权注释不变（头文件仅编译期使用，
   不入部署物）；厂商二进制不重命名以掩盖来源。
3. 不得以厂商（上海期货信息技术有限公司 / SimNow）名义推广本项目，或暗示隶属、合作、背书
   关系（同 psutil 节的「不得背书」要求）。
4. 分发对象自行遵守与厂商 / 期货公司 / SimNow 的协议；SimNow 仿真与实盘分别适用各注册 /
   开户协议。
5. 若厂商书面要求停止分发：改为「用户自备 CTP 动态库」，从 `third_party/ctp/` 移除厂商
   二进制并调整构建（应急预案，本票登记）。

**残余风险**：以上是工程判断与社区实践，**不是厂商书面授权**；本项目不就 CTP 二进制的合法性
与可用性作任何保证。跨组织、商业化等正式对外分发前建议取得厂商书面确认。

### 社区先例（事实陈述，不构成授权）

`vnpy_ctp`（MIT 的 CTP 封装）在 [GitHub 仓库](https://github.com/vnpy/vnpy_ctp)的
`vnpy_ctp/api/` 内直接入库 CTP 二进制（`thostmduserapi_se.dll` / `thosttraderapi_se.dll` /
`libs/*.lib` / `lib*.so`）；[PyPI 发布物](https://pypi.org/project/vnpy_ctp/)同样随包
（实测 6.7.11.4：sdist 12,580,717 字节含 6 个 CTP 二进制，win_amd64 wheel 3,600,801 字节含
`thostmduserapi_se.dll` / `thosttraderapi_se.dll`）；其 README 写明「接口中自带的是【穿透式
实盘、评测环境合并】的 dll 文件」。

### 核实记录

| 日期 | 内容 |
|---|---|
| 2026-10-27 | 比对 `third_party/ctp/` 全部 18 个文件与厂商发布包（SHA-256 见 `VENDORED.md`）：二进制逐字节一致，文本转码与行尾归一后一致（`error.xml` 声明属已知偏离）；包内无许可文件 |
| 2026-10-27 | 官方许可条文未取得：SimNow 站点有反爬验证（HTTP 202 挑战页），上期技术站点在核实环境不可达，未取得官方许可原文 |
| 2026-10-27 | 社区先例核实：`vnpy_ctp` 6.7.11.4 的 sdist / win_amd64 wheel 下载后核对文件列表，均含 CTP 二进制 |
| 2026-10-27 | 规范化流程固化（#178）：`scripts/vendor_ctp.py` + `third_party/ctp/VENDORED.md`（逐文件 SHA-256 / 升级纪律）；`error.xml` 声明改为 `utf-8`，`--check` / `--selftest` 通过 |

> 本口径的决策与证据记录同时见 issue #166（2026-10-27 评论）。

## 范围说明

- 本文件记录第三方内容与参考实现的归属与许可，覆盖三类：
  - **仅作规格参考、不随本项目分发**：psutil（`dzsysmon` 的参考实现）；
  - **随部署物分发**：上期技术 CTP API——`third_party/ctp/` 的厂商二进制（见「上期技术 CTP
    行情 / 交易 API」一节；头文件仅编译期使用，不入部署物）；
  - **随源码入库**：nlohmann/json 官方单头（`libs/strategy_api/include/dztrader/third_party/nlohmann/json.hpp`，
    MIT）：其 SPDX 许可头随文件本身入库，版本 / SHA-256 / 升级纪律见同目录 `VENDORED.md`；
    **随 SDK 分发包**的许可声明与打包归 issue #7
    （[ADR 0026](docs/adr/0026-strategy-sdk-nlohmann-json.md) 决策 5）。
- 其他第三方依赖（Boost、Drogon、spdlog、SQLite、msgpack-cxx 等）以依赖包形式引入、不随源码
  入库，其许可与归属由各自上游 / 依赖包提供，不在本文件重复；本项目自身代码以 MIT 许可发布。
