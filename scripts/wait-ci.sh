#!/usr/bin/env bash
# wait-ci.sh — 静默等待 GitHub CI，只在状态变化时输出
#
# 避免 gh run watch 每次刷新把整棵 job 树刷进上下文；失败时只打印失败项和链接。
# **本仓 CI 的触发条件**：ci.yml 只在 `pull_request` 与 push 到 `main` 时运行；
# feature 分支只 push 不开 PR 时**永远不会有 run**——`--sha` 会在 180s 内报错退出，
# 而不是像以前那样空等到 60 分钟总超时。
#
# 用法:
#   ./scripts/wait-ci.sh <pr-number>          # 等 PR checks（推荐；closeout §4）
#   ./scripts/wait-ci.sh --sha <commit-sha>   # 等该 commit 的最新 run（无 run 时 ~180s 失败）
#   ./scripts/wait-ci.sh --run <run-id|url>   # 等指定 run
#   ./scripts/wait-ci.sh --branch <branch>    # 等该分支的最新 run（main 合并后用）
#   以上均可加 --interval <seconds>（默认 60）、--fail-on-cancel（把 cancelled 当失败）
#
#   ./scripts/wait-ci.sh --status --pr <n>    # **单次快照**，立即返回：
#                                             # 0 绿 / 1 红（打印失败 job）/ 2 无 run 或错误 / 3 仍在跑
#
# 为什么需要 --status（复盘 #158-#161 → #192）：等待期间在会话里 `sleep N` 再查 gh 是纯浪费
# （#159/#160 各 6 次、共约 13 分钟）。DSH 下正确的做法是：等待进程用宿主的后台作业跑
# （`run_in_background`），拿结论时用 `--status` 快速查一次；宿主的 shell 会回收直接 fork
# 的孙进程，所以本脚本不提供 `--detach`（实测 setsid 子进程在工具调用结束即被杀）。
#
# 环境变量:
#   GH                        gh 可执行路径（默认 gh，缺失时回退 ~/.local/bin/gh）
#   WAIT_CI_TIMEOUT           总超时秒数（默认 3600）
#   WAIT_CI_INTERVAL          轮询间隔秒数（默认 60；--interval 覆盖）
#   WAIT_CI_APPEAR_TIMEOUT    等 run 出现的超时秒数（默认 180；用于 --sha/--branch）
#   WAIT_CI_HEARTBEAT         心跳输出间隔秒数（默认 300；长时间等待时提示仍在等）
#
# 退出码: 0=全部通过, 1=有失败（--fail-on-cancel 时取消也算）, 2=超时/无 run/参数或环境错误,
#         3=仅 --status 下：检查仍在跑（非失败；稍后再查一次即可）

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODE=""      # pr | run | sha | branch
TARGET=""
STATUS=0     # --status：单次快照后立即返回（不等待）
INTERVAL="${WAIT_CI_INTERVAL:-60}"
TIMEOUT="${WAIT_CI_TIMEOUT:-3600}"
APPEAR_TIMEOUT="${WAIT_CI_APPEAR_TIMEOUT:-180}"
HEARTBEAT="${WAIT_CI_HEARTBEAT:-300}"
FAIL_ON_CANCEL=0

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
        --branch) MODE=branch; TARGET="${2:?--branch needs a branch name}"; shift 2 ;;
        --fail-on-cancel) FAIL_ON_CANCEL=1; shift ;;
        --status) STATUS=1; shift ;;
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

for name in INTERVAL TIMEOUT APPEAR_TIMEOUT HEARTBEAT; do
    value="${!name}"
    case "$value" in
        ''|*[!0-9]*) echo "error: $name must be an integer (got: $value)" >&2; exit 2 ;;
    esac
done
if [ "$INTERVAL" -lt 1 ] || [ "$HEARTBEAT" -lt 1 ]; then
    echo "error: interval/heartbeat must be >= 1" >&2
    exit 2
fi

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
NEXT_HEARTBEAT=$((START + HEARTBEAT))
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

# 长时间无状态变化时打一行心跳，避免 agent 误以为卡死。
maybe_heartbeat() {
    local now
    now=$(date +%s)
    if [ "$now" -ge "$NEXT_HEARTBEAT" ]; then
        printf 'wait-ci: still waiting (%sm elapsed, last: %s)\n' \
            "$(((now - START) / 60))" "${LAST:-unknown}"
        NEXT_HEARTBEAT=$((now + HEARTBEAT))
    fi
}

print_failed_jobs() {
    local run_id="$1"
    "$GH_BIN" run view "$run_id" --json jobs \
        --jq '.jobs[] | select(.conclusion=="failure" or .conclusion=="cancelled" or .conclusion=="timed_out") | "FAILED: " + .name + " | " + (.html_url // "")' \
        2>/dev/null || true
}

# ---- 共享的低层查询（前台等待与 --status 同一实现，#192 审查发现两套会漂移） ----

# 抓一次 PR checks 的表格行；结果放全局 PR_CHECKS_OUT，退出码放 PR_CHECKS_RC。
# 8 是 gh 的「有 check 未完成」约定，调用方按需区分。
fetch_pr_checks() {
    local pr="$1"
    PR_CHECKS_RC=0
    PR_CHECKS_OUT="$("$GH_BIN" pr checks "$pr" --json name,bucket,link \
        --jq '.[] | [.name,.bucket,.link] | @tsv' 2>/dev/null)" || PR_CHECKS_RC=$?
}

wait_pr() {
    local pr="$1"
    while :; do
        local rc=0 out
        fetch_pr_checks "$pr"
        rc="$PR_CHECKS_RC"
        out="$PR_CHECKS_OUT"

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
            local summary summary_changed=0
            summary="$(printf '%s\n' "$out" | awk -F '\t' '{printf "%s=%s ", $1, $2}')"
            if [ "$summary" != "$LAST" ]; then
                printf '[%s] %s\n' "$(date +%H:%M:%S)" "$summary"
                LAST="$summary"
                summary_changed=1
            fi

            # cancelled 的 check 通常是「同一分支新 push 取代了旧 run」，不是失败：
            # 默认继续等新 check；--fail-on-cancel 才当失败（#77/#78 实测）。
            local failed superseded=""
            if [ "$FAIL_ON_CANCEL" -eq 1 ]; then
                failed="$(printf '%s\n' "$out" | awk -F '\t' '$2 == "fail" || $2 == "cancel" {print}')"
            else
                failed="$(printf '%s\n' "$out" | awk -F '\t' '$2 == "fail" {print}')"
                superseded="$(printf '%s\n' "$out" | awk -F '\t' '$2 == "cancel" {print}')"
                if [ -n "$superseded" ] && [ "$summary_changed" -eq 1 ]; then
                    printf '%s\n' "$superseded" | while IFS=$'\t' read -r name _bucket link; do
                        printf 'SUPERSEDED: %s | %s\n' "$name" "$link"
                    done
                fi
            fi
            if [ -n "$failed" ]; then
                printf '%s\n' "$failed" | while IFS=$'\t' read -r name _bucket link; do
                    printf 'FAILED: %s | %s\n' "$name" "$link"
                done
                exit 1
            fi

            local pending
            pending="$(printf '%s\n' "$out" | awk -F '\t' '$2 == "pending" {c++} END {print c+0}')"
            if [ "$pending" -eq 0 ]; then
                if [ -n "$superseded" ]; then
                    # 只有被取代的旧 check、没有 pending：等新 run 的 check 出现后再判绿。
                    [ "$summary_changed" -eq 1 ] &&
                        echo "wait-ci: only superseded checks remain; waiting for the newer run's checks..." >&2
                else
                    echo "CI green: PR #$pr"
                    exit 0
                fi
            fi
        fi

        check_timeout
        sleep "$INTERVAL"
        maybe_heartbeat
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
                    cancelled)
                        if [ "$FAIL_ON_CANCEL" -eq 1 ]; then
                            print_failed_jobs "$run_id"
                            echo "CI failed: run $run_id conclusion=cancelled (--fail-on-cancel)" >&2
                            exit 1
                        fi
                        # 显式指定的 run 被取消，一般是被同分支更新的 run 取代；
                        # 提示改用 --branch/--pr 去等取代它的新 run，避免对着旧 run 空等。
                        echo "wait-ci: run $run_id was cancelled (superseded); use --branch/--pr to wait for the newer run, or --fail-on-cancel to treat it as failure" >&2
                        exit 2
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
        maybe_heartbeat
    done
}

# 最多等 APPEAR_TIMEOUT 秒让 run 出现；等不到说明触发条件不满足（典型：feature 分支没开 PR）。
await_appear_or_die() {
    local waited="$1"
    local what="$2"
    if [ "$waited" -ge "$APPEAR_TIMEOUT" ]; then
        echo "error: no CI run for $what after ${APPEAR_TIMEOUT}s" >&2
        echo "hint: ci.yml runs only on pull_request and push-to-main; a feature branch push without a PR never starts CI" >&2
        echo "hint: open the PR first (docs/agents/ticket-closeout.md §4), then rerun wait-ci" >&2
        exit 2
    fi
}

wait_sha() {
    local sha="$1"
    local full_sha
    full_sha="$(git -C "$ROOT" rev-parse --verify --quiet "${sha}^{commit}" 2>/dev/null || true)"
    if [ -z "$full_sha" ]; then
        echo "error: cannot resolve commit: $sha" >&2
        exit 2
    fi

    local announced=0 waited=0
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
            echo "wait-ci: no run for $full_sha yet; waiting up to ${APPEAR_TIMEOUT}s for one to appear..." >&2
            announced=1
        fi
        await_appear_or_die "$waited" "commit $full_sha"
        check_timeout
        sleep "$INTERVAL"
        waited=$((waited + INTERVAL))
        maybe_heartbeat
    done
}

wait_branch() {
    local branch="$1"
    local announced=0 waited=0
    while :; do
        local rid=""
        # 优先取仍在跑的 run（被取代的旧 run 用 cancelled 表示），没有则取最新一个。
        rid="$("$GH_BIN" run list --branch "$branch" --limit 20 \
            --json databaseId,status \
            --jq '([.[] | select(.status != "completed")][0].databaseId) // (.[0].databaseId) // empty' 2>/dev/null || true)"
        if [ -n "$rid" ]; then
            echo "wait-ci: found run $rid for branch $branch"
            wait_run "$rid"
            return
        fi
        if [ "$announced" -eq 0 ]; then
            echo "wait-ci: no run for branch $branch yet; waiting up to ${APPEAR_TIMEOUT}s for one to appear..." >&2
            announced=1
        fi
        await_appear_or_die "$waited" "branch $branch"
        check_timeout
        sleep "$INTERVAL"
        waited=$((waited + INTERVAL))
        maybe_heartbeat
    done
}

# ---- --status：单次快照（#192；不 fork、不循环、立即返回） ----

# 打印一个 run 的状态并返回 0/1/3（run 维度的单次查询，pr/sha/branch 共用）。
status_run() {
    local rid="$1" rc=0 raw status conclusion
    # 与 wait_run 同一 jq 形状（#192 审查：单次查询、同一惯例；jobs 详情只在失败时再拉）。
    raw="$("$GH_BIN" run view "$rid" --json status,conclusion \
        --jq '[.status, (.conclusion // "")] | @tsv' 2>/dev/null)" || rc=$?
    if [ "$rc" -ne 0 ] || [ -z "$raw" ]; then
        echo "wait-ci: cannot read run $rid (exit $rc)" >&2
        return 2
    fi
    IFS=$'\t' read -r status conclusion <<<"$raw"
    if [ "$status" != "completed" ]; then
        echo "wait-ci: run $rid status=$status"
        return 3
    fi
    if [ "$conclusion" = "success" ]; then
        echo "wait-ci: run $rid success"
        return 0
    fi
    echo "wait-ci: run $rid conclusion=$conclusion"
    print_failed_jobs "$rid"
    return 1
}

status_once() {
    case "$MODE" in
        run)
            status_run "${TARGET##*/}"
            return $?
            ;;
        sha)
            local full_sha rid
            # 与 wait_sha 一致：短 sha 先解析成完整 commit，否则 gh 返回空。
            full_sha="$(git -C "$ROOT" rev-parse --verify --quiet "${TARGET}^{commit}" 2>/dev/null || true)"
            if [ -z "$full_sha" ]; then
                echo "wait-ci: cannot resolve commit: $TARGET" >&2
                return 2
            fi
            rid="$("$GH_BIN" run list --commit "$full_sha" --limit 10 \
                --json databaseId --jq '.[0].databaseId // empty' 2>/dev/null || true)"
            if [ -z "$rid" ]; then
                echo "wait-ci: no run found for sha $TARGET"
                return 2
            fi
            status_run "$rid"
            return $?
            ;;
        branch)
            local rid
            rid="$("$GH_BIN" run list --branch "$TARGET" --limit 1 \
                --json databaseId --jq '.[0].databaseId // empty' 2>/dev/null || true)"
            if [ -z "$rid" ]; then
                echo "wait-ci: no run found for branch $TARGET"
                return 2
            fi
            status_run "$rid"
            return $?
            ;;
        pr)
            local rc out buckets
            fetch_pr_checks "$TARGET"
            rc="$PR_CHECKS_RC"
            out="$PR_CHECKS_OUT"
            if [ -z "$out" ]; then
                if [ "$rc" -ne 0 ] && [ "$rc" -ne 8 ]; then
                    echo "wait-ci: gh pr checks failed (exit $rc)" >&2
                    return 2
                fi
                echo "wait-ci: no checks found for PR #$TARGET (not pushed/opened yet?)"
                return 2
            fi
            buckets="$(printf '%s\n' "$out" | cut -f2 | sort -u)"
            if printf '%s\n' "$buckets" | grep -qx "fail"; then
                echo "wait-ci: PR #$TARGET checks FAILED"
                printf '%s\n' "$out" | awk -F'\t' '$2=="fail" {print "FAILED: "$1" | "$3}'
                return 1
            fi
            # cancel = 同分支新 push 取代了旧 run（wait_pr 默认也继续等新 check），
            # 因此算「仍在跑」而不是绿，更不是红；skipping（如 cache warm）忽略。
            if printf '%s\n' "$buckets" | grep -qx "pending" ||
                printf '%s\n' "$buckets" | grep -qx "cancel"; then
                echo "wait-ci: PR #$TARGET checks still running"
                return 3
            fi
            echo "wait-ci: PR #$TARGET checks all pass"
            return 0
            ;;
    esac
    return 2
}

if [ "$STATUS" = "1" ]; then
    if [ -z "$MODE" ]; then
        echo "error: --status needs a target (e.g. --status --pr <n>)" >&2
        exit 2
    fi
    status_once
    exit $?
fi

case "$MODE" in
    pr) wait_pr "$TARGET" ;;
    run)
        case "${TARGET##*/}" in
            ''|*[!0-9]*) echo "error: not a run id or run url: $TARGET" >&2; exit 2 ;;
        esac
        wait_run "${TARGET##*/}"
        ;;
    sha) wait_sha "$TARGET" ;;
    branch) wait_branch "$TARGET" ;;
esac
