#!/usr/bin/env bash
# dztrader 构建脚本（Linux）
# 构建 C++ + 前端（含 dist 复制到 build/.../web/），一次搞定。
#
# config 来源优先级: --config 参数 > .dztrader_dev/.config 粘性文件 > release
#
# 用法:
#   ./scripts/build.sh                           # 跟随粘性 config（默认 release）
#   ./scripts/build.sh --config debug            # 临时构建 Debug
#   ./scripts/build.sh --target dzmd_ctp         # 只构建 dzmd_ctp target
#   ./scripts/build.sh --config debug --target dzcore

set -euo pipefail

CONFIG=""
TARGET=""

while [ $# -gt 0 ]; do
    case "$1" in
        -c|--config) CONFIG="${2:?--config 需要参数}"; shift 2 ;;
        -t|--target) TARGET="${2:?--target 需要参数}"; shift 2 ;;
        -h|--help)
            echo "Usage: $0 [--config release|debug] [--target <name>]"
            echo "  无参数时读 .dztrader_dev/.config，无则 release"
            exit 0
            ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

# 读粘性 config，--config 优先覆盖
if [ -z "$CONFIG" ]; then
    STICKY_FILE="$(cd "$(dirname "$0")/.." && pwd)/.dztrader_dev/.config"
    if [ -f "$STICKY_FILE" ]; then
        CONFIG=$(cat "$STICKY_FILE" | tr '[:upper:]' '[:lower:]')
    fi
    [ -z "$CONFIG" ] && CONFIG="release"
fi

CONFIG=$(echo "$CONFIG" | tr '[:upper:]' '[:lower:]')
case "$CONFIG" in
    release|debug) ;;
    *) echo "Error: config 必须是 release 或 debug"; exit 1 ;;
esac

PRESET="linux-$CONFIG"

CMAKE_ARGS=(--build --preset "$PRESET")
if [ -n "$TARGET" ]; then
    CMAKE_ARGS+=(--target "$TARGET")
fi

echo "cmake ${CMAKE_ARGS[*]}"
cmake "${CMAKE_ARGS[@]}"
