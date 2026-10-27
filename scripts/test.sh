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
#   ./scripts/test.sh --label perf              # 只跑性能基准（PerfBench + BarPerfBench）
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

# 测试锁：与 build 共用同一把锁（.cache/build.lock），避免「测试跑到一半被并发构建
# 换掉二进制」，也避免两个 test 同时 rm -rf 同一个 test_home 而互相踩。
# shellcheck source=scripts/build-lock.sh
. "$(cd "$(dirname "$0")/.." && pwd)/scripts/build-lock.sh"
dz_acquire_build_lock "$(cd "$(dirname "$0")/.." && pwd)" "test" || exit 2

# 测试 DZTRADER_HOME（.cache/test_home，由 CMakePresets.json 的 test-base 注入）
# 每次运行前清空重建：上一轮残留的 shm meta 会让无关测试一起失败（#134）；
# 在 SetUp 内自管临时 home 的测试不受影响。
TEST_HOME="$(cd "$(dirname "$0")/.." && pwd)/.cache/test_home"
rm -rf "$TEST_HOME"
mkdir -p "$TEST_HOME"

# ctest 并行度（#136）：gtest_discover_tests 把每个用例注册为独立进程，-j 才能并行；
# 用例级并发安全由「临时资源唯一化」保证（固定名目录会互踩，已全仓整改 + preflight lint 兜底）。
# 显式设置该变量的调用方优先，否则默认 4。fast 档实测 150.7s -> ~110s。
: "${CTEST_PARALLEL_LEVEL:=4}"
export CTEST_PARALLEL_LEVEL

echo "ctest ${CTEST_ARGS[*]} (CTEST_PARALLEL_LEVEL=$CTEST_PARALLEL_LEVEL)"
ctest "${CTEST_ARGS[@]}"
