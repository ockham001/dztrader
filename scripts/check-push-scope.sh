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

# --selftest（#265）：在临时仓库里复现 BASE 解析的四种状态，确认兜底生效。
# 存在理由与 check_json_vendor_version.py / ci-log.sh 的 selftest 相同——解析逻辑本身
# 也会写错，而这里写错的代价是「push 被挡 + 提示误导」（#246 实测 2 次）。
# 只读 + 临时目录，不碰工作区。
SELFTEST=0

while [ $# -gt 0 ]; do
    case "$1" in
        -h|--help) usage; exit 0 ;;
        --selftest) SELFTEST=1; shift ;;
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

# resolve_base(): 把「当前分支上游」解析成可用于 `git log base..head` 的 ref。
# 三种失败形态都归到 origin/main：
#   1) 没有上游（rev-parse 非零退出，无输出）
#   2) 上游指向 URL（`git push -u <URL>` 的后果）——rev-parse 打印字面量 "@{u}"
#   3) 上游 ref 不可解析
resolve_base() {
    local resolved
    resolved="$(git rev-parse --abbrev-ref --symbolic-full-name '@{u}' 2>/dev/null || true)"
    case "${resolved:-}" in
        ''|'@{u}') resolved="origin/main" ;;
    esac
    printf '%s' "$resolved"
}

run_selftest() {
    local tmp fails=0 got want
    tmp="$(mktemp -d)"
    trap 'rm -rf "$tmp"' RETURN

    _st_case() {  # <名称> <期望> <setup 命令串>
        local name="$1" want="$2" setup="$3"
        git -C "$tmp" init -q . >/dev/null 2>&1 || { echo "selftest: git init failed" >&2; return 1; }
        git -C "$tmp" config user.email selftest@example.com
        git -C "$tmp" config user.name selftest
        git -C "$tmp" checkout -q -b feat/issue-999-x 2>/dev/null || true
        # shellcheck disable=SC2086
        ( cd "$tmp" && eval "$setup" ) >/dev/null 2>&1 || true
        got="$( cd "$tmp" && resolve_base )"
        if [ "$got" = "$want" ]; then
            echo "selftest ok $name -> $got"
        else
            echo "selftest FAIL $name: expected '$want', got '$got'" >&2
            fails=$((fails + 1))
        fi
        rm -rf "${tmp:?}"/* "${tmp:?}"/.[!.]* 2>/dev/null || true
    }

    _st_case "no-upstream" "origin/main" "true"

    _st_case "upstream-is-url" "origin/main" \
        "git config branch.feat/issue-999-x.remote 'https://user:TOKEN@github.com/o/r.git' && \
         git config branch.feat/issue-999-x.merge refs/heads/feat/issue-999-x"

    # 正常上游：需要真实的提交 + remote.fetch refspec + 远端跟踪 ref，三者齐备时
    # git 才认定它是远程跟踪分支（缺 fetch 会报 "not stored as a remote-tracking
    # branch"，比缺 ref 更早失败——这正是自测要锁住的行为）。
    git -C "$tmp" init -q . >/dev/null 2>&1
    git -C "$tmp" config user.email selftest@example.com
    git -C "$tmp" config user.name selftest
    git -C "$tmp" checkout -q -b feat/issue-999-x 2>/dev/null || true
    ( cd "$tmp" && git commit -q --allow-empty -m "chore(#999): selftest baseline" ) >/dev/null 2>&1
    ( cd "$tmp" && git config remote.origin.url /nonexistent &&
      git config --add remote.origin.fetch '+refs/heads/*:refs/remotes/origin/*' ) >/dev/null 2>&1
    ( cd "$tmp" && git update-ref refs/remotes/origin/feat/issue-999-x HEAD ) >/dev/null 2>&1
    ( cd "$tmp" && git update-ref refs/remotes/origin/main HEAD ) >/dev/null 2>&1
    ( cd "$tmp" && git config branch.feat/issue-999-x.remote origin &&
      git config branch.feat/issue-999-x.merge refs/heads/feat/issue-999-x ) >/dev/null 2>&1
    got="$( cd "$tmp" && resolve_base )"
    if [ "$got" = "origin/feat/issue-999-x" ]; then
        echo "selftest ok upstream-normal -> $got"
    else
        echo "selftest FAIL upstream-normal: expected 'origin/feat/issue-999-x', got '$got'" >&2
        fails=$((fails + 1))
    fi
    rm -rf "${tmp:?}"/* "${tmp:?}"/.[!.]* 2>/dev/null || true

    # 票号匹配：提交标题含本票号才算通过（这是本脚本的主判定，单独自测正则形态）
    _st_subject_case() {  # <名称> <subject> <期望 ok|bad>
        local name="$1" subject="$2" want="$3" rc
        if printf '%s' "$subject" | grep -qE "#265([^0-9]|\$)"; then
            got=ok
        else
            got=bad
        fi
        if [ "$got" = "$want" ]; then
            echo "selftest ok $name -> $got"
        else
            echo "selftest FAIL $name: expected '$want', got '$got' (subject: $subject)" >&2
            fails=$((fails + 1))
        fi
    }
    _st_subject_case "subject-scope-form" "fix(#265): 硬化" "ok"
    _st_subject_case "subject-cn-parens" "feat: 硬化（#265）" "ok"
    _st_subject_case "subject-other-ticket" "fix(#246): 别的票" "bad"
    _st_subject_case "subject-no-ticket" "fix: 忘了票号" "bad"

    if [ "$fails" -ne 0 ]; then
        echo "selftest FAILED: $fails case(s)" >&2
        return 1
    fi
    echo "selftest: base resolution + subject matching behave as expected"
    return 0
}

if [ "$SELFTEST" = "1" ]; then
    run_selftest
    exit $?
fi

if [ -z "$ISSUE" ]; then
    usage >&2
    exit 2
fi
case "$ISSUE" in
    ''|*[!0-9]*) echo "error: issue number must be numeric: $ISSUE" >&2; exit 2 ;;
esac

if [ -z "$BASE" ]; then
    BASE="$(resolve_base)"
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
