#!/usr/bin/env bash
# dztrader 测试脚本（Linux）
#
# config: 无参数即 Release（preset 名本身已编码 config，无需粘性文件）
# 分层档: 默认 fast（秒级单测）；slow = 穷举/慢速；perf = 无阈值基准
#
# 用法:
#   ./scripts/test.sh                           # Release，fast 档
#   ./scripts/test.sh --config debug            # Debug（仅本地）
#   ./scripts/test.sh --label slow              # 只跑 slow 穷举档
#   ./scripts/test.sh --label perf              # 只跑性能基准（#93 tick→bar + #164 框架五项 + #253 交易库读路径）
#   ./scripts/test.sh --test-name TimeTest      # 只跑匹配 TimeTest 的测试
#   ./scripts/test.sh --test-name TdConfigTest --test-name SmokeTest   # 多个 regex 择一（#281）
#   ./scripts/test.sh --list                    # 只列出用例名（ctest -N），不跑

set -euo pipefail

usage() {
    echo "Usage: $0 [--config release|debug] [--label fast|slow|perf] [--test-name <regex>]... [--list]"
    echo "  无 --config 时默认 release"
    echo "  无 --label 时跑 fast 档；slow/perf 为显式可选档"
    echo "  --test-name 可重复（用 | 合成一个 ctest -R）；regex 匹配 <Fixture>.<Case>，不是源文件名"
    echo "  --list 只打印用例名（ctest -N），用于查 --test-name 该填什么"
}

# 取 flag 的参数；缺参数/空值时报用法。${2:?} 的 bash 内建报错只有 "parameter null
# or not set"、不含用法，且覆盖式赋值让重复 flag 静默丢弃前值（#281/#250 门禁假绿）。
# 空值也要拒（`--test-name ""` 会退化成 `ctest -R ""` 跑全量）：旧 `${2:?}` 同样拒绝空值。
require_value() {  # <实际剩余参数个数 $#> <flag 名> <值>
    if [ "$1" -lt 2 ] || [ -z "${3:-}" ]; then
        echo "error: $2 needs a non-empty value" >&2
        usage >&2
        exit 2
    fi
}

CONFIG=""
LABEL=""
TEST_NAMES=()
LIST=0

while [ $# -gt 0 ]; do
    case "$1" in
        -c|--config) require_value $# "--config" "${2:-}"; CONFIG="$2"; shift 2 ;;
        -L|--label) require_value $# "--label" "${2:-}"; LABEL="$2"; shift 2 ;;
        -t|--test-name) require_value $# "--test-name" "${2:-}"; TEST_NAMES+=("$2"); shift 2 ;;
        --list) LIST=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "error: unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

if [ -z "$CONFIG" ]; then
    CONFIG="release"
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
if [ ${#TEST_NAMES[@]} -gt 0 ]; then
    # ctest 只接受一个 -R：多个 regex 用 | 连接成同一正则的择一分支（#281）。
    # 不给每个分支加括号：| 的优先级低于锚点，`^A\.|^B\.` 各分支的锚点语义不受影响。
    joined_regex=""
    for name in "${TEST_NAMES[@]}"; do
        joined_regex="${joined_regex:+$joined_regex|}${name}"
    done
    CTEST_ARGS+=(-R "$joined_regex")
fi
if [ "$LIST" -eq 1 ]; then
    CTEST_ARGS+=(-N)
fi

# --list 直通 ctest -N：只读、不取构建锁、不重建 test_home。
# 用例名是 <Fixture>.<Case>，猜源文件名会得到 rc=8 "No tests were found"——
# 与「测试红」同形但含义完全不同（#281）。一次调用查全量用例名，替代反复试错。
if [ "$LIST" -eq 1 ]; then
    echo "ctest ${CTEST_ARGS[*]}"
    exec ctest "${CTEST_ARGS[@]}"
fi

# 测试锁：与 build 共用同一把锁（.cache/build.lock），避免「测试跑到一半被并发构建
# 换掉二进制」，也避免两个 test 同时 rm -rf 同一个 test_home 而互相踩。
# shellcheck source=scripts/build-lock.sh
. "$(cd "$(dirname "$0")/.." && pwd)/scripts/build-lock.sh"
dz_acquire_build_lock "$(cd "$(dirname "$0")/.." && pwd)" "test" || exit 2

# 测试 DZTRADER_HOME（.cache/test_home，由 CMakePresets.json 的 test-base 注入）
# 每次运行前清空重建：上一轮残留的 shm meta 会让无关测试一起失败（#134）；
# 在 SetUp 内自管临时 home 的测试不受影响。
#
# DZ_SKIP_TEST_HOME_RESET=1 只给脚本参数自测用（scripts/tests/test_script_args.py）：
# selftest 在 PATH 注入假 flock 以绕开真实构建锁，此时若照旧清 test_home，就会抹掉
# 并行的 `gate.sh test` 正在使用的 DZTRADER_HOME（#281 审查 B1——加锁本要防的正是
# 这个互踩）。普通调用方不要设置该变量。
TEST_HOME="$(cd "$(dirname "$0")/.." && pwd)/.cache/test_home"
if [ -z "${DZ_SKIP_TEST_HOME_RESET:-}" ]; then
    rm -rf "$TEST_HOME"
    mkdir -p "$TEST_HOME"
fi

# ctest 并行度（#136）：gtest_discover_tests 把每个用例注册为独立进程，-j 才能并行；
# 用例级并发安全由「临时资源唯一化」保证（固定名目录会互踩，已全仓整改 + preflight lint 兜底）。
# 显式设置该变量的调用方优先，否则默认 4。fast 档实测 150.7s -> ~110s。
: "${CTEST_PARALLEL_LEVEL:=4}"
export CTEST_PARALLEL_LEVEL

echo "ctest ${CTEST_ARGS[*]} (CTEST_PARALLEL_LEVEL=$CTEST_PARALLEL_LEVEL)"
ctest "${CTEST_ARGS[@]}"
