# 内置 nlohmann/json 单头（vendored）

本目录的 `json.hpp` 是 nlohmann/json 官方**单头发布物**，原样入库、不得局部改写。
策略 SDK 取用 JSON 的唯一入口是 `dztrader/json.h`（默认模式包含本文件，定义
`DZTRADER_JSON_EXTERNAL` 时改用外部版本）——**不要从框架代码直接包含本目录的路径**。

| 项 | 值 |
|---|---|
| 组件 | nlohmann/json（JSON for Modern C++） |
| 版本 | **3.12.0** |
| 上游 | https://github.com/nlohmann/json |
| 发布物 | https://github.com/nlohmann/json/releases/download/v3.12.0/json.hpp |
| SHA-256 | `aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63` |
| 许可 | MIT（SPDX 头在上游文件内；随 SDK 分发包的许可声明归 issue #7） |
| 取用日期 | 2026-10-27 |
| 版本一致性校验 | `scripts/check_json_vendor_version.py`（preflight / CI） |

## 升级纪律

1. `conanfile.py` 的 `nlohmann_json/<version>` 与整份 `json.hpp` **同一次提交**升级。
   `scripts/check_json_vendor_version.py` 校验 conanfile.py、内置单头版本宏与本文件的
   版本三方一致，改漏一边即红（ADR 0026 决策 6）。
2. **整文件替换**，不做局部补丁——局部改会让上游 diff 无法对齐，也掩盖真实改动。
3. 替换命令（把 `<version>` 换成目标版本）：

   ```bash
   curl -sSL -o libs/strategy_api/include/dztrader/third_party/nlohmann/json.hpp \
       https://github.com/nlohmann/json/releases/download/v<version>/json.hpp
   sha256sum libs/strategy_api/include/dztrader/third_party/nlohmann/json.hpp
   ```

4. 同步更新本文件的「版本 / 发布物 / SHA-256 / 取用日期」四行；版本号不在其它源硬编码
   （`dztrader/json.h` 的注释只写纪律，不写版本号；`scripts/check_json_vendor_version.py`
   的自测锚点也**从当前文件内容推导**，不需要随版本手工改——它不硬编码任何版本号）。
5. 同一次提交跑门禁：`./scripts/gate.sh build` + `./scripts/gate.sh test`——双模探针
   `libs/strategy_api/tests/json_sdk_probe.cpp` 会把内置与外部两种模式各编译运行一次。
6. 许可文案：本文件只记录版本与哈希，MIT 全文随 `json.hpp` 的 SPDX 头入库；随 SDK 分发包
   的许可声明归 issue #7。仓内 vendored 源码清单见根 `THIRD_PARTY_NOTICES.md`「范围说明」。

## 为什么禁改格式

`json.hpp` 是两万余行的生成物：clang-format 重排既无收益（上游格式稳定），也会让升级
时的整文件替换产生无意义 diff。本目录的 `.clang-format`（`DisableFormat: true`）让
pre-commit 的格式检查直接跳过本文件，且避免对该文件做无谓的格式化开销。
