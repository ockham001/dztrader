#!/usr/bin/env python3
"""快路径探针（临时，仅用于验证 #289 AC3 的 mode=fast，验完删除）。

本文件落在 `scripts/**` 且不在 `scripts/ci_scope.py` 的 denylist 内 → PR 应判 `mode=fast`：
`scope` job 输出 fast、Windows job 跳过、Linux 只跑 preflight（Build / C++ Tests / 前端
四步跳过）。纯 `docs/**` 的 PR 走不到这里——它们被 ci.yml 的 `paths-ignore` 整体跳过。
"""


def main() -> int:
    print("fastpath probe")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
