#!/usr/bin/env bash
# ci-log.sh — 拉取 GitHub Actions 失败日志，只打印错误行（不把整份日志刷进上下文）
#
# 用法:
#   ./scripts/ci-log.sh <run-id|run-url>        # 自动找失败的 job
#   ./scripts/ci-log.sh --job <job-id>          # 直接指定 job
#   ./scripts/ci-log.sh <target> --print-full   # 额外打印完整日志
#
# 日志落在 .cache/ci/（已 gitignore）。gh 缓存回退到仓库 .cache/，
# 避免 DSH 沙箱下 ~/.cache/gh 只读导致 gh run view --log-failed 失败。
#
# 退出码: 0=已保存日志, 1=没有失败 job, 2=环境/参数错误

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUTDIR="$ROOT/.cache/ci"
TARGET=""
JOB_ID=""
PRINT_FULL=0

usage() {
    sed -n '2,/^$/p' "$0"
}

while [ $# -gt 0 ]; do
    case "$1" in
        -h|--help) usage; exit 0 ;;
        --job) JOB_ID="${2:?--job 需要参数}"; shift 2 ;;
        --print-full) PRINT_FULL=1; shift ;;
        -*) echo "error: unknown option: $1" >&2; usage >&2; exit 2 ;;
        *)
            if [ -z "$TARGET" ]; then TARGET="$1"; shift
            else echo "error: unexpected argument: $1" >&2; exit 2; fi
            ;;
    esac
done

if [ -z "$TARGET" ] && [ -z "$JOB_ID" ]; then
    usage >&2
    exit 2
fi

GH_BIN="${GH:-gh}"
if ! command -v "$GH_BIN" >/dev/null 2>&1; then
    if [ -x "${HOME}/.local/bin/gh" ]; then
        GH_BIN="${HOME}/.local/bin/gh"
    else
        echo "error: gh not found; set GH=/path/to/gh" >&2
        exit 2
    fi
fi

# gh 缓存：~/.cache/gh 只读时回退到仓库 .cache/（XDG_CACHE_HOME）
gh_cache_ok=0
if mkdir -p "$HOME/.cache/gh" 2>/dev/null &&
    touch "$HOME/.cache/gh/.dz_ci_log_probe" 2>/dev/null; then
    gh_cache_ok=1
fi
rm -f "$HOME/.cache/gh/.dz_ci_log_probe" 2>/dev/null || true
if [ "$gh_cache_ok" -eq 0 ] && [ -z "${XDG_CACHE_HOME:-}" ]; then
    export XDG_CACHE_HOME="$ROOT/.cache"
fi
mkdir -p "$OUTDIR" "${XDG_CACHE_HOME:-$HOME/.cache}"

REPO="$("$GH_BIN" repo view --json nameWithOwner --jq .nameWithOwner 2>/dev/null)" || {
    echo "error: cannot resolve repo (run inside the git clone)" >&2
    exit 2
}

strip_ansi() {
    sed -E 's/\x1b\[[0-9;]*[A-Za-z]//g'
}

fetch_job() {
    local job_id="$1"
    local name="${2:-}"
    local out="$OUTDIR/job-${job_id}.log"
    local err="$OUTDIR/job-${job_id}.err"
    if ! "$GH_BIN" api "repos/${REPO}/actions/jobs/${job_id}/logs" \
        --allow-escape-sequences >"$out" 2>"$err"; then
        echo "error: fetch job $job_id failed: $(head -n 1 "$err" 2>/dev/null)" >&2
        return 1
    fi
    strip_ansi <"$out" >"${out}.tmp" && mv "${out}.tmp" "$out"
    echo "saved: $out ($(wc -c <"$out") bytes) job=${name:-$job_id}"

    if [ "$PRINT_FULL" -eq 1 ]; then
        cat "$out"
        return 0
    fi
    local matches
    matches=$(grep -nE 'error C[0-9]{4}|error LNK[0-9]+|fatal error|FAILED:|ninja: build stopped|error MSB[0-9]+|Test Failed|FAILED TEST|ERROR:' "$out" | head -n 80 || true)
    if [ -n "$matches" ]; then
        printf '%s\n' "$matches"
    else
        echo "--- no known error pattern; last 80 lines ---"
        tail -n 80 "$out"
    fi
}

if [ -n "$JOB_ID" ]; then
    fetch_job "$JOB_ID" || exit 1
    exit 0
fi

# 允许传 run URL；取最后一段数字作为 run id
RUN_ID="${TARGET##*/}"
case "$RUN_ID" in
    ''|*[!0-9]*)
        echo "error: not a run id or run url: $TARGET" >&2
        exit 2
        ;;
esac

mapfile -t FAILED < <(
    "$GH_BIN" run view "$RUN_ID" --json jobs \
        --jq '.jobs[] | select(.conclusion=="failure" or .conclusion=="cancelled" or .conclusion=="timed_out") | "\(.databaseId)\t\(.name)"' 2>/dev/null
)

if [ "${#FAILED[@]}" -eq 0 ]; then
    echo "no failed jobs in run $RUN_ID (green, still running, or logs not ready)"
    echo "hint: run may still be in progress; wait for completion first"
    exit 1
fi

rc=0
for entry in "${FAILED[@]}"; do
    job_id=""
    job_name=""
    IFS=$'\t' read -r job_id job_name <<<"$entry"
    fetch_job "$job_id" "$job_name" || rc=1
done
exit "$rc"
