#!/usr/bin/env bash
# dztrader 测试脚本（Linux）
#
# config 来源优先级: --config 参数 > .dztrader_dev/.config 粘性文件 > release
#
# 用法:
#   ./scripts/test.sh                           # 跟随粘性 config
#   ./scripts/test.sh --config debug            # 临时测试 Debug
#   ./scripts/test.sh --test-name TimeTest      # 只跑匹配 TimeTest 的测试

set -euo pipefail

CONFIG=""
TEST_NAME=""

while [ $# -gt 0 ]; do
    case "$1" in
        -c|--config) CONFIG="${2:?--config 需要参数}"; shift 2 ;;
        -t|--test-name) TEST_NAME="${2:?--test-name 需要参数}"; shift 2 ;;
        -h|--help)
            echo "Usage: $0 [--config release|debug] [--test-name <regex>]"
            echo "  无 --config 时读 .dztrader_dev/.config，无则 release"
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

CTEST_ARGS=(--preset "$PRESET")
if [ -n "$TEST_NAME" ]; then
    CTEST_ARGS+=(-R "$TEST_NAME")
fi

echo "ctest ${CTEST_ARGS[*]}"
ctest "${CTEST_ARGS[@]}"
