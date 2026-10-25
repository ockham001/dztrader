#!/usr/bin/env bash
# ci-log.sh — 拉取 GitHub Actions 失败日志/测试 artifact，只打印错误行（不把整份日志刷进上下文）
#
# 用法:
#   ./scripts/ci-log.sh <run-id|run-url>          # 自动找失败 job
#   ./scripts/ci-log.sh --job <job-id>            # 直接指定 job
#   ./scripts/ci-log.sh <run-id> --print-full     # 额外打印完整日志
#   ./scripts/ci-log.sh <run-id> --context 12     # 错误行前后各 12 行（默认 8）
#   ./scripts/ci-log.sh <run-id> --refresh        # 忽略 .cache/ci 中已下载的日志，重新拉
#   ./scripts/ci-log.sh <run-id> --no-artifact    # 不优先使用 last-test-log artifact
#   ./scripts/ci-log.sh <run-id> --force          # run 未完成时也尝试（默认直接报错退出）
#
# 优先顺序（复盘实测：从「红」到「定位用例」的耗时差 32min → <1min）：
#   1) 失败 run 先取 ci.yml 上传的 `last-test-log` artifact（约 200KB，秒级给出 gtest 用例名）；
#   2) artifact 不存在才拉失败 job 的完整日志（Windows job 可达数十 MB）；
#   3) 同一个 run/job 的日志已落 `.cache/ci/` 时默认复用，不重复下载。
#
# 环境变量:
#   CI_LOG_ARTIFACT     artifact 名（默认 last-test-log）
#   CI_LOG_MAX_BYTES    日志下载前提示阈值字节数（默认 20971520 = 20MB）
#
# 日志落 .cache/ci/（已 gitignore）。
# 退出码: 0=已保存/已分析日志, 1=没有失败 job, 2=环境/参数错误或 run 未完成

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUTDIR="$ROOT/.cache/ci"
TARGET=""
JOB_ID=""
PRINT_FULL=0
CONTEXT=8
REFRESH=0
USE_ARTIFACT=1
FORCE=0
ARTIFACT="${CI_LOG_ARTIFACT:-last-test-log}"
MAX_BYTES="${CI_LOG_MAX_BYTES:-20971520}"

usage() {
    sed -n '2,/^$/p' "$0"
}

while [ $# -gt 0 ]; do
    case "$1" in
        -h|--help) usage; exit 0 ;;
        --job) JOB_ID="${2:?--job 需要参数}"; shift 2 ;;
        --print-full) PRINT_FULL=1; shift ;;
        --context) CONTEXT="${2:?--context 需要参数}"; shift 2 ;;
        --refresh) REFRESH=1; shift ;;
        --no-artifact) USE_ARTIFACT=0; shift ;;
        --force) FORCE=1; shift ;;
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
case "$CONTEXT" in
    ''|*[!0-9]*) echo "error: --context must be an integer" >&2; exit 2 ;;
esac

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

ERROR_PATTERN='\[  FAILED  \]|The following tests FAILED|FAILED TEST|error C[0-9]{4}|error LNK[0-9]+|fatal error|FAILED:|ninja: build stopped|error MSB[0-9]+|Test Failed|ERROR:'

# 从一份已落盘日志里打印错误行 + 上下文；没有命中时打末尾 60 行。
report_log() {
    local file="$1"
    local label="$2"
    if [ "$PRINT_FULL" -eq 1 ]; then
        cat "$file"
        return 0
    fi
    local matches
    matches=$(grep -n -B"$CONTEXT" -A"$CONTEXT" -E "$ERROR_PATTERN" "$file" | head -n 120 || true)
    if [ -n "$matches" ]; then
        printf -- '--- %s matches (context +/-%s) ---\n' "$label" "$CONTEXT"
        printf '%s\n' "$matches"
    else
        printf -- '--- %s: no known error pattern; last 60 lines ---\n' "$label"
        tail -n 60 "$file"
    fi
}

fetch_job() {
    local job_id="$1"
    local name="${2:-}"
    local out="$OUTDIR/job-${job_id}.log"
    local err="$OUTDIR/job-${job_id}.err"
    if [ "$REFRESH" -eq 0 ] && [ -s "$out" ]; then
        echo "reuse: $out ($(wc -c <"$out") bytes) job=${name:-$job_id}"
        report_log "$out" "job $job_id ${name:+($name)}"
        return 0
    fi
    if ! "$GH_BIN" api "repos/${REPO}/actions/jobs/${job_id}/logs" \
        --allow-escape-sequences >"$out" 2>"$err"; then
        echo "error: fetch job $job_id failed: $(head -n 1 "$err" 2>/dev/null)" >&2
        return 1
    fi
    strip_ansi <"$out" >"${out}.tmp" && mv "${out}.tmp" "$out"
    local size
    size=$(wc -c <"$out")
    echo "saved: $out ($size bytes) job=${name:-$job_id}"
    if [ "$size" -ge "$MAX_BYTES" ]; then
        echo "note: log is large (${size} bytes); artifact '$ARTIFACT' is usually ~200KB when present" >&2
    fi
    report_log "$out" "job $job_id ${name:+($name)}"
}

# 尝试用失败 run 的测试 artifact 定位；成功返回 0（已打印），无 artifact 返回 1。
fetch_artifact() {
    local run_id="$1"
    local dir="$OUTDIR/run-${run_id}-artifact"
    if [ "$REFRESH" -eq 0 ] && [ -d "$dir" ] && [ -n "$(ls -A "$dir" 2>/dev/null)" ]; then
        echo "reuse artifact: $dir"
    else
        rm -rf "$dir"
        mkdir -p "$dir"
        if ! "$GH_BIN" run download "$run_id" --name "$ARTIFACT" --dir "$dir" >/dev/null 2>&1; then
            return 1
        fi
        echo "artifact: '$ARTIFACT' -> $dir"
    fi
    local found=0
    local f
    while IFS= read -r f; do
        found=1
        report_log "$f" "artifact $(basename "$f")"
    done < <(find "$dir" -type f | sort)
    [ "$found" -eq 1 ] && return 0
    return 1
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

STATUS="$("$GH_BIN" run view "$RUN_ID" --json status --jq .status 2>/dev/null || true)"
if [ -n "$STATUS" ] && [ "$STATUS" != "completed" ]; then
    echo "ci-log: run $RUN_ID is still $STATUS; failed-job logs may be incomplete" >&2
fi

if [ "$USE_ARTIFACT" -eq 1 ] && fetch_artifact "$RUN_ID"; then
    exit 0
fi

if [ -n "$STATUS" ] && [ "$STATUS" != "completed" ] && [ "$FORCE" -eq 0 ]; then
    echo "error: run $RUN_ID is still $STATUS and no '$ARTIFACT' artifact is available" >&2
    echo "hint: wait for it to finish (./scripts/wait-ci.sh --run $RUN_ID), then rerun with --refresh" >&2
    echo "hint: or pass --force to try downloading partial failed-job logs now" >&2
    exit 2
fi

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
