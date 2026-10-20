#!/usr/bin/env bash
# dztrader 测试脚本（Linux）
#
# config 来源优先级: --config 参数 > .dztrader_dev/.config 粘性文件 > release
# 分层档: 默认 fast（秒级单测）；slow = 穷举/慢速；perf = 无阈值基准
#
# 用法:
#   ./scripts/test.sh                           # 跟随粘性 config；默认 fast 档
#   ./scripts/test.sh --config debug            # 临时测试 Debug
#   ./scripts/test.sh --label slow              # 只跑 slow 穷举档
#   ./scripts/test.sh --label perf              # 只跑 PerfBench
#   ./scripts/test.sh --test-name TimeTest      # 只跑匹配 TimeTest 的测试

set -euo pipefail

CONFIG=""
LABEL=""
TEST_NAME=""

while [ $# -gt 0 ]; do
    case "$1" in
        -c|--config) CONFIG="${2:?--config 需要参数}"; shift 2 ;;
        -L|--label) LABEL="${2:?--label 需要参数}"; shift 2 ;;
        -t|--test-name) TEST_NAME="${2:?--test-name 需要参数}"; shift 2 ;;
        -h|--help)
            echo "Usage: $0 [--config release|debug] [--label fast|slow|perf] [--test-name <regex>]"
            echo "  无 --config 时读 .dztrader_dev/.config，无则 release"
            echo "  无 --label 时跑 fast 档；slow/perf 为显式可选档"
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

case "$LABEL" in
    ""|fast) PRESET="linux-$CONFIG" ;;
    slow) PRESET="linux-$CONFIG-slow" ;;
    perf) PRESET="linux-$CONFIG-perf" ;;
    *) echo "Error: --label 必须是 fast / slow / perf"; exit 1 ;;
esac

CTEST_ARGS=(--preset "$PRESET")
if [ -n "$TEST_NAME" ]; then
    CTEST_ARGS+=(-R "$TEST_NAME")
fi

echo "ctest ${CTEST_ARGS[*]}"
ctest "${CTEST_ARGS[@]}"
