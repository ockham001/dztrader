#!/usr/bin/env bash
# check-push-scope.sh — push 前确认上游区间只含本票 commit
#
# 并行波常见事故：主工作区同时有两条线的改动，提交父节点落在另一条线未 push
# 的 commit 上，push 时把别人的票一起发布。本脚本在 push 前做机械检查。
#
# 用法:
#   ./scripts/check-push-scope.sh <issue-number> [--base <ref>]
#
# base 默认 @{u}（无上游时回退 origin/main）。逐个检查 `base..HEAD` 的
# 非 merge commit，subject 必须含 `#<issue>`（全角/半角括号均可）。
#
# 退出码: 0=区间为空或全部匹配, 1=发现不属于本票的 commit, 2=参数/引用错误

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

ISSUE=""
BASE=""

usage() {
    sed -n '2,/^$/p' "$0"
}

while [ $# -gt 0 ]; do
    case "$1" in
        -h|--help) usage; exit 0 ;;
        --base) BASE="${2:?--base 需要参数}"; shift 2 ;;
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

COMMITS="$(git log --no-merges --format='%h %s' "${BASE}..HEAD")"
if [ -z "$COMMITS" ]; then
    echo "ok: ${BASE}..HEAD is empty (nothing to push)"
    exit 0
fi

echo "push scope: ${BASE}..HEAD"
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
