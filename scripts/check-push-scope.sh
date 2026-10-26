#!/usr/bin/env bash
# check-push-scope.sh — push 前确认上游区间只含本票 commit，且不在 CI 进行中重复 push
#
# 并行波常见事故：主工作区同时有两条线的改动，提交父节点落在另一条线未 push
# 的 commit 上，push 时把别人的票一起发布。本脚本在 push 前做机械检查。
#
# 第二个检查：同一分支已有 in-progress CI run 时拒绝 push——新 push 会取消/排队
# 正在跑的 PR run（每次 ≈35–40 分钟），review 修复应攒成一个 commit 再推。
#
# 用法:
#   ./scripts/check-push-scope.sh <issue-number> [--base <ref>] [--head <sha>] [--branch <name>]
#
# base 默认 @{u}（无上游时回退 origin/main）；head 默认 HEAD（pre-push hook 传被推的 local_sha）。
# `--branch` 只影响 CI-busy 检查（默认当前分支；pre-push hook 传被推的远端分支名）。
# 逐个检查 `base..head` 的非 merge commit，subject 必须含 `#<issue>`（全角/半角括号均可）。
# CI 进行中时可用 PUSH_SCOPE_ALLOW_CI_BUSY=1 跳过第二个检查。
#
# 退出码: 0=区间为空或全部匹配, 1=发现不属于本票的 commit, 2=参数/引用错误, 3=分支 CI 进行中

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

ISSUE=""
BASE=""
HEAD_SHA=""
BRANCH_OVERRIDE=""

usage() {
    sed -n '2,/^$/p' "$0"
}

while [ $# -gt 0 ]; do
    case "$1" in
        -h|--help) usage; exit 0 ;;
        --base) BASE="${2:?--base 需要参数}"; shift 2 ;;
        --head) HEAD_SHA="${2:?--head 需要参数}"; shift 2 ;;
        --branch) BRANCH_OVERRIDE="${2:?--branch 需要参数}"; shift 2 ;;
        -*) echo "error: unknown option: $1" >&2; usage >&2; exit 2 ;;
        *)
            if [ -z "$ISSUE" ]; then ISSUE="$1"; shift
            else echo "error: unexpected argument: $1" >&2; exit 2; fi
            ;;
    esac
done

if [ -z "$ISSUE" ]; then
    usage >&2
    exit 2
fi
case "$ISSUE" in
    ''|*[!0-9]*) echo "error: issue number must be numeric: $ISSUE" >&2; exit 2 ;;
esac

if [ -z "$BASE" ]; then
    BASE="$(git rev-parse --abbrev-ref --symbolic-full-name '@{u}' 2>/dev/null || true)"
fi
if [ -z "$BASE" ]; then
    BASE="origin/main"
fi
if ! git rev-parse --verify --quiet "$BASE" >/dev/null; then
    echo "error: base ref not found: $BASE" >&2
    exit 2
fi

if [ -z "$HEAD_SHA" ]; then
    HEAD_SHA="$(git rev-parse HEAD)"
fi
if ! git rev-parse --verify --quiet "${HEAD_SHA}^{commit}" >/dev/null; then
    echo "error: head ref not found: $HEAD_SHA" >&2
    exit 2
fi

COMMITS="$(git log --no-merges --format='%h %s' "${BASE}..${HEAD_SHA}")"
if [ -z "$COMMITS" ]; then
    echo "ok: ${BASE}..${HEAD_SHA} is empty (nothing to push)"
    exit 0
fi

check_ci_busy() {
    local gh_bin="$1" branch active
    branch="${BRANCH_OVERRIDE:-$(git branch --show-current 2>/dev/null || true)}"
    [ -n "$branch" ] || return 0
    command -v "$gh_bin" >/dev/null 2>&1 || return 0
    active="$("$gh_bin" run list --branch "$branch" --limit 10 \
        --json databaseId,status,workflowName \
        --jq '.[] | select(.status != "completed") | "\(.databaseId) \(.status) \(.workflowName)"' \
        2>/dev/null || true)"
    [ -n "$active" ] || return 0
    echo "error: CI run(s) still in progress on branch '$branch'; a push will queue/cancel them (~35-40min each):" >&2
    printf '  %s\n' "$active" >&2
    echo "  wait with ./scripts/wait-ci.sh --run <id>, or set PUSH_SCOPE_ALLOW_CI_BUSY=1 to override" >&2
    return 3
}

if [ "${PUSH_SCOPE_ALLOW_CI_BUSY:-0}" != "1" ]; then
    GH_BIN="${GH:-gh}"
    if ! command -v "$GH_BIN" >/dev/null 2>&1 && [ -x "$HOME/.local/bin/gh" ]; then
        GH_BIN="$HOME/.local/bin/gh"
    fi
    # gh 缓存只读时回退仓库 .cache（与 wait-ci.sh 一致）
    if [ -d "$HOME/.cache/gh" ]; then
        if touch "$HOME/.cache/gh/.dz_scope_probe" 2>/dev/null; then
            rm -f "$HOME/.cache/gh/.dz_scope_probe"
        else
            export XDG_CACHE_HOME="${XDG_CACHE_HOME:-$ROOT/.cache}"
        fi
    fi
    check_ci_busy "$GH_BIN" || exit $?
fi

echo "push scope: ${BASE}..${HEAD_SHA}"
bad=0
while IFS= read -r line; do
    printf '  %s\n' "$line"
    if ! printf '%s' "$line" | grep -qE "#${ISSUE}([^0-9]|$)"; then
        printf '    ^ missing #%s\n' "$ISSUE" >&2
        bad=1
    fi
done <<<"$COMMITS"

if [ "$bad" -ne 0 ]; then
    echo "error: push range contains commits not for #${ISSUE}; rebase/cherry-pick only this ticket's commits" >&2
    exit 1
fi
echo "ok: push range contains only #${ISSUE} commits"
