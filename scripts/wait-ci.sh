#!/usr/bin/env bash
# wait-ci.sh — 静默等待 GitHub CI，只在状态变化时输出
#
# 避免 gh run watch 每次刷新把整棵 job 树刷进上下文；失败时只打印失败项和链接。
# 本仓库推 main 时没有 PR，用 --sha / --run；有 PR 时仍可直接传 PR 号。
#
# 用法:
#   ./scripts/wait-ci.sh <pr-number>          # 等 PR checks
#   ./scripts/wait-ci.sh --sha <commit-sha>   # 等该 commit 的最新 run
#   ./scripts/wait-ci.sh --run <run-id|url>   # 等指定 run
#   以上均可加 --interval <seconds>（默认 60）
#
# 环境变量:
#   GH                  gh 可执行路径（默认 gh，缺失时回退 ~/.local/bin/gh）
#   WAIT_CI_TIMEOUT     总超时秒数（默认 3600）
#   WAIT_CI_INTERVAL    轮询间隔秒数（默认 60；--interval 覆盖）
#
# 退出码: 0=全部通过, 1=有失败/取消, 2=超时或 gh 不可用

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODE=""      # pr | run | sha
TARGET=""
INTERVAL="${WAIT_CI_INTERVAL:-60}"
TIMEOUT="${WAIT_CI_TIMEOUT:-3600}"

usage() {
    sed -n '2,/^$/p' "$0"
}

while [ $# -gt 0 ]; do
    case "$1" in
        -h|--help) usage; exit 0 ;;
        -i|--interval) INTERVAL="${2:?--interval needs a value}"; shift 2 ;;
        --run) MODE=run; TARGET="${2:?--run needs a run id}"; shift 2 ;;
        --sha|--commit) MODE=sha; TARGET="${2:?$1 needs a commit sha}"; shift 2 ;;
        --pr) MODE=pr; TARGET="${2:?--pr needs a number}"; shift 2 ;;
        -*) echo "error: unknown option: $1" >&2; usage >&2; exit 2 ;;
        *)
            if [ -z "$MODE" ]; then
                MODE=pr
                TARGET="$1"
                shift
            else
                echo "error: unexpected argument: $1" >&2
                usage >&2
                exit 2
            fi
            ;;
    esac
done

if [ -z "$MODE" ]; then
    usage >&2
    exit 2
fi

case "$INTERVAL" in
    ''|*[!0-9]*) echo "error: interval must be an integer" >&2; exit 2 ;;
esac
if [ "$INTERVAL" -lt 1 ]; then
    echo "error: interval must be >= 1" >&2
    exit 2
fi
case "$TIMEOUT" in
    ''|*[!0-9]*) echo "error: WAIT_CI_TIMEOUT must be an integer" >&2; exit 2 ;;
esac

GH_BIN="${GH:-gh}"
if ! command -v "$GH_BIN" >/dev/null 2>&1; then
    if [ -x "$HOME/.local/bin/gh" ]; then
        GH_BIN="$HOME/.local/bin/gh"
    else
        echo "error: gh not found; set GH=/path/to/gh" >&2
        exit 2
    fi
fi

# gh 缓存：DSH 沙箱下 ~/.cache/gh 可能只读，回退到仓库 .cache/（gitignored）
gh_cache_ok=0
if mkdir -p "$HOME/.cache/gh" 2>/dev/null &&
    touch "$HOME/.cache/gh/.dz_wait_ci_probe" 2>/dev/null; then
    gh_cache_ok=1
fi
rm -f "$HOME/.cache/gh/.dz_wait_ci_probe" 2>/dev/null || true
if [ "$gh_cache_ok" -eq 0 ] && [ -z "${XDG_CACHE_HOME:-}" ]; then
    export XDG_CACHE_HOME="$ROOT/.cache"
    mkdir -p "$XDG_CACHE_HOME"
fi

START=$(date +%s)
LAST=""
EMPTY_COUNT=0

check_timeout() {
    local now
    now=$(date +%s)
    if [ $((now - START)) -ge "$TIMEOUT" ]; then
        echo "wait-ci: timeout after ${TIMEOUT}s; last status: ${LAST:-unknown}" >&2
        exit 2
    fi
}

print_failed_jobs() {
    local run_id="$1"
    "$GH_BIN" run view "$run_id" --json jobs \
        --jq '.jobs[] | select(.conclusion=="failure" or .conclusion=="cancelled" or .conclusion=="timed_out") | "FAILED: " + .name + " | " + (.html_url // "")' \
        2>/dev/null || true
}

wait_pr() {
    local pr="$1"
    while :; do
        local rc=0 out
        out="$("$GH_BIN" pr checks "$pr" --json name,bucket,link \
            --jq '.[] | [.name,.bucket,.link] | @tsv' 2>/dev/null)" || rc=$?

        if [ -z "$out" ]; then
            EMPTY_COUNT=$((EMPTY_COUNT + 1))
            if [ "$rc" -ne 0 ] && [ "$rc" -ne 8 ]; then
                echo "wait-ci: gh pr checks failed (exit $rc); retrying..." >&2
            elif [ "$rc" -eq 0 ] && [ "$EMPTY_COUNT" -ge 3 ]; then
                echo "wait-ci: no checks found for PR #$pr" >&2
                exit 2
            fi
        else
            EMPTY_COUNT=0
            local summary
            summary="$(printf '%s\n' "$out" | awk -F '\t' '{printf "%s=%s ", $1, $2}')"
            if [ "$summary" != "$LAST" ]; then
                printf '[%s] %s\n' "$(date +%H:%M:%S)" "$summary"
                LAST="$summary"
            fi

            local failed
            failed="$(printf '%s\n' "$out" | awk -F '\t' '$2 == "fail" || $2 == "cancel" {print}')"
            if [ -n "$failed" ]; then
                printf '%s\n' "$failed" | while IFS=$'\t' read -r name bucket link; do
                    printf 'FAILED: %s | %s\n' "$name" "$link"
                done
                exit 1
            fi

            local pending
            pending="$(printf '%s\n' "$out" | awk -F '\t' '$2 == "pending" {c++} END {print c+0}')"
            if [ "$pending" -eq 0 ]; then
                echo "CI green: PR #$pr"
                exit 0
            fi
        fi

        check_timeout
        sleep "$INTERVAL"
    done
}

wait_run() {
    local run_id="$1"
    while :; do
        local rc=0 raw
        raw="$("$GH_BIN" run view "$run_id" --json status,conclusion,jobs --jq '
            [ .status,
              (.conclusion // ""),
              ((.jobs // []) | map(.name + "=" + (if (.conclusion // "") == "" then .status else .conclusion end)) | join(" | "))
            ] | @tsv' 2>/dev/null)" || rc=$?

        if [ -z "$raw" ]; then
            EMPTY_COUNT=$((EMPTY_COUNT + 1))
            if [ "$EMPTY_COUNT" -ge 3 ]; then
                echo "wait-ci: cannot read run $run_id (invalid id or API error, exit $rc)" >&2
                exit 2
            fi
        else
            EMPTY_COUNT=0
            local status conclusion jobs
            IFS=$'\t' read -r status conclusion jobs <<<"$raw"
            local display="status=$status conclusion=${conclusion:--}"
            if [ -n "$jobs" ]; then
                display="$display | $jobs"
            fi
            if [ "$display" != "$LAST" ]; then
                printf '[%s] %s\n' "$(date +%H:%M:%S)" "$display"
                LAST="$display"
            fi

            if [ "$status" = "completed" ]; then
                case "$conclusion" in
                    success)
                        echo "CI green: run $run_id"
                        exit 0
                        ;;
                    *)
                        print_failed_jobs "$run_id"
                        echo "CI failed: run $run_id conclusion=$conclusion" >&2
                        exit 1
                        ;;
                esac
            fi
        fi

        check_timeout
        sleep "$INTERVAL"
    done
}

wait_sha() {
    local sha="$1"
    local full_sha
    full_sha="$(git -C "$ROOT" rev-parse --verify --quiet "${sha}^{commit}" 2>/dev/null || true)"
    if [ -z "$full_sha" ]; then
        echo "error: cannot resolve commit: $sha" >&2
        exit 2
    fi

    local announced=0
    while :; do
        local rid=""
        rid="$("$GH_BIN" run list --commit "$full_sha" --limit 10 \
            --json databaseId,headSha --jq '.[0].databaseId // empty' 2>/dev/null || true)"
        if [ -n "$rid" ]; then
            echo "wait-ci: found run $rid for $full_sha"
            wait_run "$rid"
            return
        fi
        if [ "$announced" -eq 0 ]; then
            echo "wait-ci: no run for $full_sha yet; waiting..."
            announced=1
        fi
        check_timeout
        sleep "$INTERVAL"
    done
}

case "$MODE" in
    pr) wait_pr "$TARGET" ;;
    run)
        case "${TARGET##*/}" in
            ''|*[!0-9]*) echo "error: not a run id or run url: $TARGET" >&2; exit 2 ;;
        esac
        wait_run "${TARGET##*/}"
        ;;
    sha) wait_sha "$TARGET" ;;
esac
