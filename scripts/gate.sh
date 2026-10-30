#!/usr/bin/env bash
# gate.sh — agent 侧运行重命令的唯一安全入口（build / test / preflight / wait-ci）
#
# 为什么需要它（本仓复盘实测）：
# 1) `./scripts/build.sh ... 2>&1 | tail -20` 的退出码取自 tail：构建/测试失败被记成 0
#    （#75 的 wait-ci 超时、#81 的 ninja 失败都出现过这种假绿）；
# 2) 输出落盘 + 失败打关键行：agent 不需要读几 MB 日志；
# 3) ccache 需要 CCACHE_DIR（见 scripts/ccache-env.sh），否则新 worktree 构建直接失败。
#
# 并发串行**不在这里**：build.sh / test.sh 自己取 `.cache/build.lock`（见 scripts/build-lock.sh）。
# 早期版本 gate.sh 持锁后再调叶子脚本，父子进程各持一个 open file description 会互相等待
# —— 死锁（#168 审查发现）。所以「谁真正操作构建树，谁加锁」，gate.sh 只做日志与退出码。
#
# 用法:
#   ./scripts/gate.sh build [--tail N] [build.sh 参数...]
#   ./scripts/gate.sh test  [--tail N] [test.sh 参数...]
#   ./scripts/gate.sh preflight [--tail N] [preflight.sh 参数...]
#   ./scripts/gate.sh ledger [check_ledger.py 参数...]  # 台账体检（只读；spec 母票/悬空承诺/triage）
#   ./scripts/gate.sh wait-ci [wait-ci.sh 参数...]   # 透传，前台阻塞，退出码真实
#
# 行为:
#   - 输出落 `.cache/gate/<cmd>-<时间戳>.log`（gitignored）；
#   - 成功打末尾 8 行（`--tail N` 改为 N 行）；失败打错误行 + 末尾 max(N,30) 行，
#     并**原样返回被包装命令的退出码**；
#   - `--tail N` 由 gate.sh 自己解析，不传给被包装命令；要短输出就用它，
#     不要给门禁命令接 `| tail` / `| head`（管道会吞掉退出码，见 #75/#81 假绿）。
#   - 最终 verdict 行固定打在输出**末尾**（`| tail -2` 也可见）；每次结果原子写入
#     `.cache/gate/last-status`（cmd/rc/head/time），pre-push 对「当前 HEAD 上最近一次
#     门禁失败」直接拦截（issue #190；应急逃生 `DZ_ALLOW_RED_GATE=1`）。
#   - preflight 不取构建锁（只读检查 + 前端 lint），其前端阶段与构建里的 npm 安装
#     可能重叠 —— 由 preflight.sh 自身的 npm 门控脚本处理。
#   - ledger 透传给 `scripts/check_ledger.py`（需要 `gh` 登录态与网络）。它是 spec 母票
#     收口前的必跑体检（issue #213）：子票全关后母票必须单独收口，靠记忆发现不了。
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

# 记录最近一次门禁结果（pre-push 守卫读它；纯文本 k=v，不引入 jq 依赖）。
#
# 为什么需要：复盘 #158-#161 发现 192 次 gate 调用里 157 次接了 `| tail/head`，管道把
# 真实退出码换成了 tail 的状态，agent 看到 `[exit code: 0]`。verdict 行固定在输出末尾
# 只能救「肉眼看输出」，last-status + pre-push 才能把「当前 HEAD 上最近一次门禁失败」
# 变成推不出去的机器事实（详见 issue #190）。
record_last_status() {
    local cmd="$1" rc="$2"
    local head ts tmp
    head="$(git -C "$ROOT" rev-parse HEAD 2>/dev/null || echo unknown)"
    ts="$(date +%s)"
    # 唯一 tmp 名（`$$`）：preflight 可与 build 重叠，两个进程共用固定名会互相覆盖/撕裂。
    tmp="$LOG_DIR/.last-status.$$.tmp"
    printf 'cmd=%s\nrc=%s\nhead=%s\ntime=%s\n' "$cmd" "$rc" "$head" "$ts" >"$tmp" &&
        mv -f "$tmp" "$LOG_DIR/last-status"
}

run_and_report() {
    local cmd="$1"
    shift
    "$@" >"$LOG" 2>&1
    local rc=$?
    record_last_status "$cmd" "$rc"
    local ok_tail="${TAIL_LINES:-8}"
    local fail_tail="$ok_tail"
    if [ "$fail_tail" -lt 30 ]; then
        fail_tail=30
    fi
    if [ "$rc" -eq 0 ]; then
        tail -n "$ok_tail" "$LOG"
        # verdict 行固定打在**末尾**：`./scripts/gate.sh ... 2>&1 | tail -2` 也必须看得见。
        echo "gate: OK $cmd (log: .cache/gate/$(basename "$LOG"))"
    else
        # 有意重复两次 FAILED：第一处紧随命令结束，供日志/输出前部检索；最后一处在摘录之后，
        # 供 `2>&1 | tail -N` 截尾时仍能看到真实结论（管道会吞掉退出码，见 #75/#81）。
        echo "gate: FAILED $cmd rc=$rc (log: .cache/gate/$(basename "$LOG"))" >&2
        grep -nE '\[  FAILED  \]|error:|FAILED:|fatal error|ninja: build stopped|tests failed|Test Failed|ERROR:' "$LOG" |
            head -n 30 >&2 || true
        echo "--- last $fail_tail lines ---" >&2
        tail -n "$fail_tail" "$LOG" >&2
        # 同上：真正的 verdict 放最后，管道截尾也不会丢掉失败信号。
        echo "gate: FAILED $cmd rc=$rc (log: .cache/gate/$(basename "$LOG"))" >&2
    fi
    return "$rc"
}

case "$CMD" in
    ledger)
        # 台账体检：只读、不碰构建树，但同样落日志 + 返回真实退出码。
        # 独立分支是因为它不在 scripts/<cmd>.sh 的命名约定里（脚本叫 check_ledger.py）。
        #
        # **默认加 `--strict`**：check_ledger.py 不带该标志时即使发现 error 级问题也返回 0，
        # 于是 gate 会打出 `gate: OK ledger` 而报告正文写着「结果：FAIL」——门禁假绿（issue
        # #213 实测第一条命令就撞上）。只要纯报告（不看退出码）就直接跑
        # `python3 scripts/check_ledger.py`。
        ledger_args=("--strict")
        if [ ${#ARGS[@]} -gt 0 ]; then
            ledger_args=("${ARGS[@]}")
        fi
        run_and_report ledger "$ROOT/scripts/check_ledger.py" "${ledger_args[@]}"
        ;;
    build|test|preflight)
        # 不在此处持锁：build.sh / test.sh 自己会取 .cache/build.lock。
        # 若这里先 flock 再调它们，子进程打开同一文件是另一个 open file description，
        # flock 仍然互斥 —— 父子互等，必然死锁（#168 审查发现）。
        # preflight.sh 不加锁：它只做只读检查 + 前端 lint，不与 ninja 争构建树。
        dz_export_ccache_env "$ROOT"
        run_and_report "$CMD" "$ROOT/scripts/$CMD.sh" "${ARGS[@]}"
        ;;
    *)
        echo "error: unknown gate command: $CMD (build|test|preflight|ledger|wait-ci)" >&2
        exit 2
        ;;
esac
