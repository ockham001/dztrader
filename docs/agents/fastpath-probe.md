# 快路径探针（临时，仅用于验证 #289 AC3 的 mode=fast）

本文件只用于验证 CI 快路径：PR 改动全部落在 `docs/**` 时，`scripts/ci_scope.py` 应判
`mode=fast`，Windows job 与 Linux 的 Build / C++ Tests / 前端步骤跳过，只保留 preflight。
验证完成后本 PR 关闭、分支删除，文件不合并。
