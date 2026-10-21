#!/usr/bin/env bash
# wait-ci.sh — 静默等待 GitHub PR checks，只在状态变化时输出
#
# 避免 gh run watch 每次刷新把整棵 job 树刷进上下文；失败时只打印失败项和链接。
#
# 用法:
#   ./scripts/wait-ci.sh <pr-number> [--interval seconds]
#
# 环境变量:
#   GH                  gh 可执行路径（默认 gh，缺失时回退 ~/.local/bin/gh）
#   WAIT_CI_TIMEOUT     总超时秒数（默认 3600）
#
# 退出码: 0=全部通过, 1=有失败/取消, 2=超时或 gh 不可用

set -uo pipefail

PR=""
INTERVAL=60

usage() {
    echo "Usage: $0 <pr-number> [--interval seconds]"
    echo "  env: GH=/path/to/gh WAIT_CI_TIMEOUT=3600"
}

while [ $# -gt 0 ]; do
    case "$1" in
        -h|--help) usage; exit 0 ;;
        -i|--interval) INTERVAL="${2:?--interval 需要参数}"; shift 2 ;;
        *) if [ -z "$PR" ]; then PR="$1"; shift; else usage; exit 1; fi ;;
    esac
done

if [ -z "$PR" ]; then
    usage
    exit 1
fi

case "$INTERVAL" in
    ''|*[!0-9]*) echo "error: interval must be an integer" >&2; exit 1 ;;
esac
if [ "$INTERVAL" -lt 1 ]; then
    echo "error: interval must be >= 1" >&2
    exit 1
fi

TIMEOUT="${WAIT_CI_TIMEOUT:-3600}"
case "$TIMEOUT" in
    ''|*[!0-9]*) echo "error: WAIT_CI_TIMEOUT must be an integer" >&2; exit 1 ;;
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

START=$(date +%s)
LAST=""
EMPTY_COUNT=0

while :; do
    RC=0
    OUT="$("$GH_BIN" pr checks "$PR" --json name,bucket,link \
        --jq '.[] | [.name,.bucket,.link] | @tsv' 2>/dev/null)" || RC=$?

    if [ -z "$OUT" ]; then
        EMPTY_COUNT=$((EMPTY_COUNT + 1))
        if [ "$RC" -ne 0 ] && [ "$RC" -ne 8 ]; then
            echo "wait-ci: gh pr checks failed (exit $RC); retrying..." >&2
        elif [ "$RC" -eq 0 ] && [ "$EMPTY_COUNT" -ge 3 ]; then
            echo "wait-ci: no checks found for PR #$PR" >&2
            exit 2
        fi
    else
        EMPTY_COUNT=0
        SUMMARY="$(printf '%s\n' "$OUT" | awk -F '\t' '{printf "%s=%s ", $1, $2}')"
        if [ "$SUMMARY" != "$LAST" ]; then
            printf '[%s] %s\n' "$(date +%H:%M:%S)" "$SUMMARY"
            LAST="$SUMMARY"
        fi

        FAILED="$(printf '%s\n' "$OUT" | awk -F '\t' '$2 == "fail" || $2 == "cancel" {print}')"
        if [ -n "$FAILED" ]; then
            printf '%s\n' "$FAILED" | while IFS=$'\t' read -r name bucket link; do
                printf 'FAILED: %s | %s\n' "$name" "$link"
            done
            exit 1
        fi

        PENDING="$(printf '%s\n' "$OUT" | awk -F '\t' '$2 == "pending" {c++} END {print c+0}')"
        if [ "$PENDING" -eq 0 ]; then
            echo "CI green: PR #$PR"
            exit 0
        fi
    fi

    NOW=$(date +%s)
    if [ $((NOW - START)) -ge "$TIMEOUT" ]; then
        echo "wait-ci: timeout after ${TIMEOUT}s; last status: ${LAST:-unknown}" >&2
        exit 2
    fi
    sleep "$INTERVAL"
done
