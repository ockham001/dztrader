#!/usr/bin/env bash
# push-ticket.sh — 收尾推送的唯一入口：先过 push 纪律检查，再调 git push（#309 AC6）
#
# 背景：本波唯一违例 #297 —— `git push` 发生在相对 4640 s、首个 review `spawn_teammate`
# 在 4659 s（晚 19 s），随后修复又 `--force-with-lease`，产生第二个 CI run（19 min）。
# `.githooks/pre-push` 的 `review_marker.py check` 确实打印了 warning「no review marker for
# #297; the read-only verification round may not have run」，但它是 warning-only，被无视。
#
# 规则写成文档挡不住动作点跳过，所以把「检查 → 拒绝 → 才 push」串成一个脚本：绕过它需要
# 显式设逃生口。串行三步，任一步没通过就停止（不执行 git push）：
#   1) bash scripts/check-push-scope.sh <issue>      上游区间只含本票 commit / CI-busy 判定
#   2) python3 scripts/review_marker.py check <issue> --strict
#      --expect-axis standards --expect-axis spec   两个审查轴的标记必须存在且停在 HEAD
#      （`--strict` 与 `--expect-axis` 是 #309 AC6 给本脚本加的，默认 check 仍是恒 0 的
#      warning-only，pre-push 依赖那个行为——见 review_marker.py 的 docstring）
#   3) git push "$@"                                 原样透传真实退出码
#
# sha 前缀比较口径**只有一处**：review_marker.py 的 same_commit()。本脚本不自己解析
# `.cache/review/*.done`，也不在 sh 里另写一套正则——两套判定漂移时，被挡下的推送会
# 给出互相矛盾的结论（同形教训见 check-push-scope.sh 的 BASE 解析）。
#
# 用法:
#   ./scripts/push-ticket.sh <issue> [git-push-args...]
#   ./scripts/push-ticket.sh 309 -u origin chore/issue-309-retro-hardening
#   ./scripts/push-ticket.sh 309 --force-with-lease     # 仍由 pre-push 拦 force（rails）
#   ./scripts/push-ticket.sh --selftest                 # 临时 git 仓库 + 假 git/gh，不联网不真推
#   ./scripts/push-ticket.sh --help
#
# 逃生口:
#   DZ_ALLOW_UNREVIEWED_PUSH=1 —— review 标记缺失/为空/sha 不匹配时，把两个轴的标记状态与
#   当前 HEAD 一起打印，然后仍放行到 git push（仅覆盖 review 轴；push scope 违规仍是硬拒绝）。
#   只在真故障时用，用完在报告里写明（AGENTS §五「push 分级授权」）。
#
# 退出码:
#   0=检查通过且 git push 成功；非 0=原样透传 git push 的真实退出码
#   3=检查拒绝（push scope 违规 / review 标记缺失、为空或不是 HEAD；已设逃生口时不算）
#   2=参数或环境错误（缺票号 / 非数字票号 / 不在 git 仓库 / 缺依赖脚本）

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PYTHON="${PYTHON:-python3}"
# 两个审查轴：docs/agents/verifier-prompt.md 约定 verifier 写 `standards` / `spec` 标记。
MARKER_AXES=(standards spec)
REFUSED=3
ENV_ERROR=2

usage() {
    sed -n '2,/^$/p' "$0"
}

# ---------------------------------------------------------------------------
# --selftest：临时 git 仓库 + 假 git/gh，锁住三态与逃生口的判定（不联网、不真推）
#
# 假 git 是**转发式**的：只有 `push` 子命令被截获并记录，其余（rev-parse / log /
# merge-base …）转给真 git —— 因为 review_marker.py 与 check-push-scope.sh 的判定依赖
# 真实仓库状态，只有 push 本身必须假（否则会真的联网推走）。假 gh 返回空 run 列表，
# 让 check-push-scope.sh 的 CI-busy 检查走「没有 in-progress run」分支。
# ---------------------------------------------------------------------------

run_selftest() {
    local fails=0 passed=0 real_git tmp bin fake_dir repo base_sha head_sha
    real_git="$(command -v git)" || {
        echo "selftest: git not found" >&2
        return 2
    }
    command -v "$PYTHON" >/dev/null 2>&1 || {
        echo "selftest: $PYTHON not found" >&2
        return 2
    }
    tmp="$(mktemp -d)"
    # shellcheck disable=SC2064
    trap "rm -rf '$tmp'" RETURN
    bin="$tmp/bin"
    fake_dir="$tmp/fake"
    repo="$tmp/repo"
    mkdir -p "$bin" "$fake_dir" "$repo/scripts"

    cat >"$bin/git" <<'FAKE_GIT'
#!/usr/bin/env bash
# 只截获 push（记录 argv 后按 DZ_FAKE_PUSH_RC 退出），其余子命令转发给真 git。
if [ "${1:-}" = "push" ]; then
    printf '%s\n' "$@" >"${DZ_FAKE_DIR:?}/push.log"
    exit "${DZ_FAKE_PUSH_RC:-0}"
fi
exec "${DZ_REAL_GIT:?}" "$@"
FAKE_GIT
    cat >"$bin/gh" <<'FAKE_GH'
#!/usr/bin/env bash
# `gh run list` 的 in-progress 判定：空输出 = 没有 in-progress run，check-push-scope 放行。
exit 0
FAKE_GH
    chmod +x "$bin/git" "$bin/gh"

    cp "$ROOT/scripts/push-ticket.sh" "$repo/scripts/push-ticket.sh"
    cp "$ROOT/scripts/check-push-scope.sh" "$repo/scripts/check-push-scope.sh"
    cp "$ROOT/scripts/review_marker.py" "$repo/scripts/review_marker.py"
    chmod +x "$repo/scripts/push-ticket.sh"

    _se_git() {
        "$real_git" -c user.name=push-ticket-selftest \
            -c user.email=selftest@example.invalid -c commit.gpgsign=false "$@"
    }

    (
        cd "$repo" || exit 1
        _se_git init -q -b main
        _se_git commit -q --allow-empty -m "chore(#999): selftest baseline"
        base_sha="$(_se_git rev-parse HEAD)"
        _se_git commit -q --allow-empty -m "fix(#999): selftest head"
        head_sha="$(_se_git rev-parse HEAD)"
        # 无上游时的 base 回退到 origin/main：指到 baseline，于是 scope 区间只含 head 提交。
        _se_git update-ref refs/remotes/origin/main "$base_sha"
        printf '%s\n%s\n' "$base_sha" "$head_sha" >"$fake_dir/shas"
    ) >/dev/null 2>&1
    base_sha="$(sed -n '1p' "$fake_dir/shas")"
    head_sha="$(sed -n '2p' "$fake_dir/shas")"

    # 清理标记目录（每个用例自己写）。
    _se_clear_markers() {
        rm -rf "$repo/.cache/review"
    }

    # 写标记：<axis> <sha>
    _se_write_marker() {
        local axis="$1" sha="$2"
        mkdir -p "$repo/.cache/review"
        printf '%s\n' "$sha" >"$repo/.cache/review/999-$axis.done"
    }

    # 跑 push-ticket 副本：<allow_unreviewed 0|1> <fake_push_rc> <args...>；stdout+stderr 合并返回
    _se_run() {
        local allow="$1" push_rc="$2"
        shift 2
        (
            cd "$repo" || exit 1
            unset DZ_ALLOW_UNREVIEWED_PUSH DZ_FAKE_PUSH_RC
            export PATH="$bin:$PATH"
            export DZ_FAKE_DIR="$fake_dir" DZ_REAL_GIT="$real_git"
            export DZ_FAKE_PUSH_RC="$push_rc"
            [ "$allow" = "1" ] && export DZ_ALLOW_UNREVIEWED_PUSH=1
            bash "$repo/scripts/push-ticket.sh" "$@"
        ) </dev/null 2>&1
    }

    # 断言：<name> <want_rc> <want_push 0|1> <want_frag> <allow 0|1> <push_rc> <args...>
    _se_case() {
        local name="$1" want_rc="$2" want_push="$3" want_frag="$4" allow="$5" push_rc="$6"
        shift 6
        local out rc=""
        rm -f "$fake_dir/push.log"
        out="$(_se_run "$allow" "$push_rc" "$@")" || rc=$?
        rc="${rc:-0}"
        if [ "$rc" != "$want_rc" ]; then
            echo "selftest FAIL $name: expected rc=$want_rc, got rc=$rc" >&2
            printf '  output: %s\n' "$out" >&2
            fails=$((fails + 1))
            return
        fi
        if [ "$want_push" = "1" ] && [ ! -f "$fake_dir/push.log" ]; then
            echo "selftest FAIL $name: expected git push to run, but it did not" >&2
            printf '  output: %s\n' "$out" >&2
            fails=$((fails + 1))
            return
        fi
        if [ "$want_push" = "0" ] && [ -f "$fake_dir/push.log" ]; then
            echo "selftest FAIL $name: git push ran although the checks refused" >&2
            printf '  output: %s\n' "$out" >&2
            fails=$((fails + 1))
            return
        fi
        if [ -n "$want_frag" ] && ! printf '%s' "$out" | grep -q "$want_frag"; then
            echo "selftest FAIL $name: expected output containing '$want_frag'" >&2
            printf '  output: %s\n' "$out" >&2
            fails=$((fails + 1))
            return
        fi
        passed=$((passed + 1))
        echo "selftest ok $name -> rc=$rc"
    }

    _se_clear_markers
    _se_case "no marker -> refuse" 3 0 "(standards)" 0 0 999 -u origin main
    _se_case "no marker -> spec axis also reported" 3 0 "(spec)" 0 0 999 -u origin main

    _se_clear_markers
    _se_write_marker standards "$base_sha"
    _se_write_marker spec "$base_sha"
    _se_case "stale marker (old sha) -> refuse" 3 0 "HEAD is" 0 0 999 -u origin main

    _se_clear_markers
    _se_write_marker standards "$head_sha"
    _se_write_marker spec "$head_sha"
    _se_case "matching marker -> push" 0 1 "" 0 0 999 -u origin main
    if [ -f "$fake_dir/push.log" ]; then
        if grep -qx -- "-u" "$fake_dir/push.log" && grep -qx "origin" "$fake_dir/push.log" \
            && grep -qx "main" "$fake_dir/push.log"; then
            passed=$((passed + 1))
            echo "selftest ok push argv is passed through"
        else
            echo "selftest FAIL push argv not passed through: $(tr '\n' ' ' <"$fake_dir/push.log")" >&2
            fails=$((fails + 1))
        fi
    fi
    _se_case "git push failure -> rc passthrough" 7 1 "" 0 7 999 -u origin main

    _se_clear_markers
    _se_case "escape hatch -> push despite no marker" 0 1 "unreviewed push allowed by DZ_ALLOW_UNREVIEWED_PUSH=1" 1 0 999 -u origin main
    local escape_out
    escape_out="$(_se_run 1 0 999 -u origin main)"
    if printf '%s' "$escape_out" | grep -q "${head_sha:0:12}"; then
        passed=$((passed + 1))
        echo "selftest ok escape hatch prints current HEAD"
    else
        echo "selftest FAIL escape hatch must print the current HEAD (${head_sha:0:12})" >&2
        printf '  output: %s\n' "$escape_out" >&2
        fails=$((fails + 1))
    fi

    # 参数 / 环境错误 -> 2（不带票号、票号非数字、未知的第一位选项）
    _se_case "usage: no args -> 2" 2 0 "用法" 0 0
    _se_case "usage: non-numeric issue -> 2" 2 0 "issue" 0 0 abc
    _se_case "usage: --help -> 0" 0 0 "" 0 0 --help

    # push scope 违规先于 review 标记生效：#999 之外的 commit 必须挡下（且不 push）
    _se_clear_markers
    _se_write_marker standards "$head_sha"
    _se_write_marker spec "$head_sha"
    (
        cd "$repo" || exit 1
        _se_git commit -q --allow-empty -m "chore: selftest out-of-scope commit"
    ) >/dev/null 2>&1
    _se_case "push scope violation -> refuse before push" 3 0 "push scope" 0 0 999 -u origin main
    (
        cd "$repo" || exit 1
        _se_git reset -q --hard "$head_sha"
    ) >/dev/null 2>&1

    if [ "$fails" -ne 0 ]; then
        echo "selftest FAILED: $fails case(s)" >&2
        return 1
    fi
    echo "selftest: $passed cases ok (temp git repo + fake git/gh; nothing is pushed or fetched)"
    return 0
}

# ---------------------------------------------------------------------------
# 主流程
# ---------------------------------------------------------------------------

if [ "$#" -eq 0 ]; then
    usage >&2
    exit "$ENV_ERROR"
fi

case "${1:-}" in
    -h|--help) usage; exit 0 ;;
    --selftest)
        run_selftest
        exit $?
        ;;
esac

ISSUE="$1"
shift
GIT_PUSH_ARGS=("$@")

case "$ISSUE" in
    ''|*[!0-9]*) echo "error: issue number must be numeric: $ISSUE" >&2; exit "$ENV_ERROR" ;;
esac
if [ "$ISSUE" -le 0 ]; then
    echo "error: issue number must be positive: $ISSUE" >&2
    exit "$ENV_ERROR"
fi

cd "$ROOT" || exit "$ENV_ERROR"
if ! git rev-parse --show-toplevel >/dev/null 2>&1; then
    echo "error: not a git repository: $ROOT" >&2
    exit "$ENV_ERROR"
fi
if [ ! -f "$ROOT/scripts/check-push-scope.sh" ]; then
    echo "error: missing $ROOT/scripts/check-push-scope.sh" >&2
    exit "$ENV_ERROR"
fi
if [ ! -f "$ROOT/scripts/review_marker.py" ]; then
    echo "error: missing $ROOT/scripts/review_marker.py" >&2
    exit "$ENV_ERROR"
fi

head_sha="$(git rev-parse HEAD 2>/dev/null || echo unknown)"

# 1) push scope：上游区间只含本票 commit（含 CI-busy supersede 判定）。
#    违规是硬拒绝（AGENTS 的 push 分级授权前置）；没有逃生口——把别人的票推上去
#    的代价远大于一次等待（PUSH_SCOPE_ALLOW_CI_BUSY=1 只覆盖 CI-busy 子判定）。
scope_out="$(bash "$ROOT/scripts/check-push-scope.sh" "$ISSUE" </dev/null 2>&1)"
scope_rc=$?
case "$scope_rc" in
    0)
        echo "push-ticket: push scope ok (#$ISSUE)"
        ;;
    1|3)
        echo "push-ticket: refused: push scope check failed for #$ISSUE" >&2
        printf '%s\n' "$scope_out" >&2
        echo "push-ticket: nothing was pushed (rerun check-push-scope.sh manually to inspect)" >&2
        exit "$REFUSED"
        ;;
    *)
        printf '%s\n' "$scope_out" >&2
        echo "error: check-push-scope.sh could not run (exit $scope_rc)" >&2
        exit "$ENV_ERROR"
        ;;
esac

# 2) review 标记：两个轴都必须有标记且停在 HEAD（`--strict` + 逐轴点名）。
marker_args=(check "$ISSUE" --strict)
for axis in "${MARKER_AXES[@]}"; do
    marker_args+=(--expect-axis "$axis")
done
marker_out="$("$PYTHON" "$ROOT/scripts/review_marker.py" "${marker_args[@]}" </dev/null 2>&1)"
marker_rc=$?

relay_marker() {  # 把 review_marker.py 的逐轴 warning 原样转出（它自带 `pre-push:` 固定前缀）
    local line
    while IFS= read -r line; do
        [ -n "$line" ] && printf 'push-ticket:   %s\n' "$line"
    done <<<"$marker_out"
}

if [ "$marker_rc" = "0" ]; then
    echo "push-ticket: review markers ok (#$ISSUE: ${MARKER_AXES[*]})"
elif [ "$marker_rc" = "3" ]; then
    if [ "${DZ_ALLOW_UNREVIEWED_PUSH:-0}" = "1" ]; then
        echo "push-ticket: WARNING: unreviewed push allowed by DZ_ALLOW_UNREVIEWED_PUSH=1 (emergency only)" >&2
        echo "push-ticket:   HEAD=$head_sha" >&2
        relay_marker >&2
        echo "push-ticket: proceeding to git push; record this override in the close-out report" >&2
    else
        echo "push-ticket: refused: review markers for #$ISSUE are missing, empty or not at HEAD" >&2
        echo "push-ticket:   HEAD=$head_sha" >&2
        relay_marker >&2
        echo "push-ticket: nothing was pushed; finish the read-only verification round and write both axes" >&2
        echo "push-ticket:   python3 scripts/review_marker.py write $ISSUE standards   (and axis spec)" >&2
        echo "push-ticket:   emergency only: DZ_ALLOW_UNREVIEWED_PUSH=1 ./scripts/push-ticket.sh $ISSUE ..." >&2
        exit "$REFUSED"
    fi
else
    printf '%s\n' "$marker_out" >&2
    echo "error: review marker check could not run (exit $marker_rc)" >&2
    exit "$ENV_ERROR"
fi

# 3) git push：纪律检查全过之后才执行，退出码原样透传（推送结果由 git 自己报告）。
if [ "${#GIT_PUSH_ARGS[@]}" -gt 0 ]; then
    echo "push-ticket: git push ${GIT_PUSH_ARGS[*]}"
    git push "${GIT_PUSH_ARGS[@]}"
else
    echo "push-ticket: git push"
    git push
fi
exit $?
