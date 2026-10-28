# CTP 发布包 vendored 记录

`third_party/ctp/` 是**随部署物分发**的厂商二进制与头文件；许可与再分发条件见根
[THIRD_PARTY_NOTICES.md](../../THIRD_PARTY_NOTICES.md) 的「上期技术 CTP 行情 / 交易 API」一节。

本文件是 vendored **机械事实的唯一记录**（上游版本、发布包哈希、逐文件 SHA-256），由
`scripts/vendor_ctp.py` 生成 / 校验；**版本与清单字段不允许手工维护**（防止记录与实际文件漂移），
与许可声明分开——法律文档不承载易失的机械细节。

## 上游与版本

| 项 | 内容 |
|---|---|
| 上游 | SimNow 平台（上期技术 CTP API 发布入口）https://www.simnow.com.cn/ |
| 版本 | **6.7.13** |
| 发布包内部版本 | `v6.7.13_20260225` |
| 发布包文件 | `ctp-6.7.13.zip` |
| 发布包 SHA-256 | `9bd5be783fcaf69c0563f9a20a528d7b9eb4d4f96ca9d359183c9b61f19dfd03` |
| 入库位置 | `third_party/ctp/{linux,windows}/` |

## 规范化规则

`--sync` 对发布包的处理（与 `scripts/vendor_ctp.py` docstring 一一对应，不做其它修改）：

1. **二进制**逐字节复制（`*.so` / `*.dll` / `*.lib`）；
2. **文本**（4 个头文件——两平台各 4 份、共 8 个文件——加 `error.xml` / `error.dtd`）
   `GB18030 → UTF-8`、`CRLF → LF`。发布包里 Windows 头文件是 GBK + CRLF、Linux 头文件是
   GBK + LF，两侧正文相同——所以规范化后 `windows/*.h` 与 `linux/*.h` **逐字节相同**，
   这是预期形态，不是误拷贝；
3. **`error.xml` 的 XML 声明** `encoding="gb2312"` → `encoding="utf-8"`（唯一内容偏离，见下）；
4. **回写本文件**：版本 / 发布包内部版本 / 发布包文件 / 发布包 SHA-256 与 18 行清单
   （对应「升级纪律」第 2 步）。

## 已知偏离（与发布包原始字节的差异）

1. **编码与行尾**：全部文本文件转 UTF-8、去 CRLF（规范化规则 2）。复核方式（把
   `"<发布包文件>"` 换成发布包内同名文件的路径）：

   ```bash
   iconv -f GB18030 -t UTF-8 "<发布包文件>" | tr -d '\r'
   ```

   输出应与 `third_party/ctp/` 对应文件一致；二进制直接 `cmp`。
2. **`error.xml` 声明**：发布包内字节为 GBK、声明 `gb2312`；入库后字节为 UTF-8，声明同步改为
   `utf-8`。否则解析器按 GB2312 解释 UTF-8 字节，中文 `prompt` 会乱码。
3. **对 `/utf-8` 编译选项的隐性依赖**：仓库内头文件是无 BOM 的 UTF-8，`CMakeLists.txt` 全局
   `add_compile_options(/W4 /permissive- /utf-8)`（MSVC）。若未来移除 `/utf-8`，中文 Windows 上
   MSVC 会按系统代码页解释这些头文件（中文注释乱码 / 编码告警）。**不要移除该选项，也不要给
   vendored 头文件加 BOM。**

## 文件清单（18）

<!-- vendor-ctp:manifest:start -->
| 文件 | SHA-256 |
|---|---|
| `linux/error.dtd` | `a94aecdb30de057c7a883e88f0c8ad0ce5f76cb0f00cfee5a0dadba6f8264a3a` |
| `linux/error.xml` | `e6f8f216fdff1dd49a2dfb80465b961bf8bad309494ebacc2e5b506c6235c5a8` |
| `linux/ThostFtdcMdApi.h` | `01f88eee8fff335db79cc42a7276a26fa7ed1292ef543dbc7ea76c1444548718` |
| `linux/ThostFtdcTraderApi.h` | `54f124bdd6bceca664f4274517a9ab2f23ac293136a20b2f730c329788a082d3` |
| `linux/ThostFtdcUserApiDataType.h` | `57689527f11c3fd880c4216ccb31df25bab28ce6f45f322a644d2ed9fcd076d2` |
| `linux/ThostFtdcUserApiStruct.h` | `59a07b48a7d072d5fe6312bf1310a2fee5b123f5e00322b17d837f6c51f3da65` |
| `linux/thostmduserapi_se.so` | `204ad48dd082b2282422c5826b3d006d7e865833ae91556d6cf7c6aea8c56a18` |
| `linux/thosttraderapi_se.so` | `8f919d0f75f407b0d7960358acee5f55864744be589f16cb545a91ab8cb80403` |
| `windows/error.dtd` | `a94aecdb30de057c7a883e88f0c8ad0ce5f76cb0f00cfee5a0dadba6f8264a3a` |
| `windows/error.xml` | `e6f8f216fdff1dd49a2dfb80465b961bf8bad309494ebacc2e5b506c6235c5a8` |
| `windows/ThostFtdcMdApi.h` | `01f88eee8fff335db79cc42a7276a26fa7ed1292ef543dbc7ea76c1444548718` |
| `windows/ThostFtdcTraderApi.h` | `54f124bdd6bceca664f4274517a9ab2f23ac293136a20b2f730c329788a082d3` |
| `windows/ThostFtdcUserApiDataType.h` | `57689527f11c3fd880c4216ccb31df25bab28ce6f45f322a644d2ed9fcd076d2` |
| `windows/ThostFtdcUserApiStruct.h` | `59a07b48a7d072d5fe6312bf1310a2fee5b123f5e00322b17d837f6c51f3da65` |
| `windows/thostmduserapi_se.dll` | `12440f65e58d1204b0c1b294658f676765a91a13d2bc9255471dd8d4a747bae3` |
| `windows/thostmduserapi_se.lib` | `a8cb77abe9054c580f63f6929fe4331d53a8e5e048d67dda7b3db6db37f82cde` |
| `windows/thosttraderapi_se.dll` | `abcff74f93a5a017179180a7c4353d42283f633ffbc8947023ff625533afdebe` |
| `windows/thosttraderapi_se.lib` | `739cc01d565f61c9633ef5fe3e070f648934d8274e8b30fa608007e5f9338e08` |
<!-- vendor-ctp:manifest:end -->

## 校验规则（R1–R5）

`--check` 与 `--sync` 收尾都跑这套规则；`--selftest` 逐条注入已知违例验证检出能力。

| 规则 | 校验内容 |
|---|---|
| R1 | 仓库文件集合 = 清单条目 = 固定 18 个文件（多一个少一个都红） |
| R2 | 逐文件 SHA-256 与清单一致 |
| R3 | 文本文件是 UTF-8、无 BOM、无 CRLF |
| R4 | `error.xml`（两平台）的 XML 声明为 `utf-8` |
| R5 | 本文件的 `版本` / `发布包内部版本` / `发布包文件` / `发布包 SHA-256` 行格式合法，且清单区有 markers |

## 升级纪律

**触发条件**：维护者提供新的 CTP 发布包（zip）时执行。AI 会话同样适用——不需要额外口头说明，
读本节与 [AGENTS.md](../../AGENTS.md) §七的入口即可操作。

维护者 / AI 操作序列（在仓库根执行）：

```bash
# 1) 直接同步；--sync 先用包 SHA-256 与记录比对，防止拿错包
python3 scripts/vendor_ctp.py --sync --zip <新发布包路径>

# 若报 `vendor package sha256 mismatch: recorded=… actual=…`：
#   说明换了发布版本。把错误里的 actual 值写进本文件「上游与版本」表的
#   「发布包 SHA-256」行，再重跑上一条命令。
#   版本 / 内部版本 / 文件名由 --sync 回写，不要手改。

# 2) 校验（必须绿）
python3 scripts/vendor_ctp.py --check
```

期望输出：

- `--sync`：首行 `sync: release package <文件名> -> vX.Y.Z_YYYYMMDD (sha256 …)`，
  随后以 Markdown 表格行打印 18 个文件的相对路径与 SHA-256；
- `--check`：`OK: 18 vendor files match third_party/ctp/VENDORED.md`，退出码 0。

硬约束：

1. 清单与版本字段**不许手工维护**（`--check` 的 R2 / R5 会拦）；
2. **版本、文件、本记录同一次提交**（与 nlohmann/json 的 vendored 纪律一致，见
   [nlohmann VENDORED.md](../../libs/strategy_api/include/dztrader/third_party/nlohmann/VENDORED.md)）；
3. 升级后跑 `./scripts/gate.sh preflight --c-only`（纯 vendored 升级不改代码，但文档链接与格式检查要绿）。

## 校验

```bash
python3 scripts/vendor_ctp.py --check      # R1–R5（只读，不需要发布包）
python3 scripts/vendor_ctp.py --selftest   # 自测：注入已知违例 + 合成发布包跑 sync（不碰工作区）
```
