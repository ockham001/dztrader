#!/usr/bin/env bash
# gate.sh — agent 侧运行重命令的唯一安全入口（build / test / preflight / wait-ci）
#
# 为什么需要它（本仓复盘实测）：
# 1) `./scripts/build.sh ... 2>&1 | tail -20` 的退出码取自 tail：构建/测试失败被记成 0
#    （#75 的 wait-ci 超时、#81 的 ninja 失败都出现过这种假绿）；
# 2) 同一 build tree 并发 ninja 会互相破坏，此前只有 gitignored 的 .cache/build-lock.sh 可用
#    （#77 现造、#78 重新发现、#81 复制进 worktree）；
# 3) ccache 需要 CCACHE_DIR（见 scripts/ccache-env.sh），否则新 worktree 构建直接失败。
#
# 用法:
#   ./scripts/gate.sh build [--tail N] [build.sh 参数...]
#   ./scripts/gate.sh test  [--tail N] [test.sh 参数...]
#   ./scripts/gate.sh preflight [--tail N] [preflight.sh 参数...]
#   ./scripts/gate.sh wait-ci [wait-ci.sh 参数...]   # 透传，前台阻塞，退出码真实
#
# 行为:
#   - build/test：flock 串行（`.cache/gate-build.lock`），避免并发 ninja；
#   - 输出落 `.cache/gate/<cmd>-<时间戳>.log`（gitignored）；
#   - 成功打末尾 8 行（`--tail N` 改为 N 行）；失败打错误行 + 末尾 max(N,30) 行，
#     并**原样返回被包装命令的退出码**；
#   - `--tail N` 由 gate.sh 自己解析，不传给被包装命令；要短输出就用它，
#     不要给门禁命令接 `| tail` / `| head`（管道会吞掉退出码，见 #75/#81 假绿）。
#
# 退出码: 0=通过；非 0=被包装命令的真实退出码（wait-ci 额外有 1=失败/取消、2=超时）。

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=scripts/ccache-env.sh
. "$ROOT/scripts/ccache-env.sh"

CMD="${1:-}"
if [ -z "$CMD" ]; then
    sed -n '2,/^$/p' "$0" >&2
    exit 2
fi
shift

case "$CMD" in
    -h|--help)
        sed -n '2,/^$/p' "$0"
        exit 0
        ;;
esac

# wait-ci 本就静默、前台阻塞：直接透传，不接受 --tail
if [ "$CMD" = "wait-ci" ]; then
    exec "$ROOT/scripts/wait-ci.sh" "$@"
fi

# 解析 --tail N（gate.sh 自己的参数，不下传给被包装命令）
TAIL_LINES=""
ARGS=()
while [ $# -gt 0 ]; do
    case "$1" in
        --tail)
            if [ $# -lt 2 ]; then
                echo "error: --tail needs a line count (e.g. --tail 20)" >&2
                exit 2
            fi
            TAIL_LINES="$2"
            shift 2
            ;;
        *)
            ARGS+=("$1")
            shift
            ;;
    esac
done
if [ -n "$TAIL_LINES" ]; then
    case "$TAIL_LINES" in
        *[!0-9]*)
            echo "error: --tail needs a positive integer, got: $TAIL_LINES" >&2
            exit 2
            ;;
    esac
    if [ "$TAIL_LINES" -lt 1 ]; then
        echo "error: --tail needs a positive integer, got: $TAIL_LINES" >&2
        exit 2
    fi
fi

LOG_DIR="$ROOT/.cache/gate"
mkdir -p "$LOG_DIR"
LOG="$LOG_DIR/${CMD}-$(date +%Y%m%d-%H%M%S)-$$.log"

run_and_report() {
    local cmd="$1"
    shift
    "$@" >"$LOG" 2>&1
    local rc=$?
    local ok_tail="${TAIL_LINES:-8}"
    local fail_tail="$ok_tail"
    if [ "$fail_tail" -lt 30 ]; then
        fail_tail=30
    fi
    if [ "$rc" -eq 0 ]; then
        echo "gate: OK $cmd (log: .cache/gate/$(basename "$LOG"))"
        tail -n "$ok_tail" "$LOG"
    else
        echo "gate: FAILED $cmd rc=$rc (log: .cache/gate/$(basename "$LOG"))" >&2
        grep -nE '\[  FAILED  \]|error:|FAILED:|fatal error|ninja: build stopped|tests failed|Test Failed|ERROR:' "$LOG" |
            head -n 30 >&2 || true
        echo "--- last $fail_tail lines ---" >&2
        tail -n "$fail_tail" "$LOG" >&2
    fi
    return "$rc"
}

case "$CMD" in
    build|test)
        if ! command -v flock >/dev/null 2>&1; then
            echo "error: flock not found (util-linux); cannot serialize build/test" >&2
            exit 2
        fi
        dz_export_ccache_env "$ROOT"
        exec 9>"$ROOT/.cache/gate-build.lock"
        flock 9
        run_and_report "$CMD" "$ROOT/scripts/$CMD.sh" "${ARGS[@]}"
        ;;
    preflight)
        run_and_report "$CMD" "$ROOT/scripts/preflight.sh" "${ARGS[@]}"
        ;;
    *)
        echo "error: unknown gate command: $CMD (build|test|preflight|wait-ci)" >&2
        exit 2
        ;;
esac
