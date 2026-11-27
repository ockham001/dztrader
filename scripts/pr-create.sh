#!/usr/bin/env bash
# pr-create.sh — 建 PR 的唯一入口：先过标题 + 正文 auto-close 守卫，再执行 gh pr create（#504）
#
# 为什么需要（retro #492–#497）：PR 标题/正文与 commit subject 共用同一套 auto-close 规则
# （scripts/check_commit_subject.py 的 R3/R4）。守卫早已存在，但「可用」不是动作点——
# #495 的 PR 正文写了 `Closes #495`，本地没人拦，CI 的 `PR body (no closing keywords)`
# job 红掉，改正文后整轮 CI 重跑（wait-ci 1,127s = 19min）。这与 push 侧同构：把守卫串进
# 正常路径（push-ticket.sh 把 scope/标记/md-lock/push 串起来），而不是等 CI 抓。
#
# 用法:
#   ./scripts/pr-create.sh --title "feat(#495): ..." --body-file .cache/pr-495.md \
#       --base main --head feat/issue-495-login-failed
#   ./scripts/pr-create.sh --selftest
#
# 行为:
#   1) 标题：python3 scripts/check_commit_subject.py --subject "$TITLE"（前缀 / 长度 / closing keyword）
#   2) 正文：python3 scripts/check_commit_subject.py --pr-body-file "$BODY"（closing keyword）
#   3) 两道全绿才 `gh pr create --title ... --body-file ... "$@"`（其余参数原样透传，gh 输出直通）
#   4) 任一守卫失败 → exit 1 且**不调用 gh**（--selftest 覆盖）
#
# 退出码: 0=PR 已创建; 1=守卫失败; 2=参数/环境错误
#
# 环境变量: GH（默认 gh，与 wait-ci.sh 一致；selftest 用假 gh）。

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SCRIPT="$ROOT/scripts/pr-create.sh"
SUBJECT_CHECK="$ROOT/scripts/check_commit_subject.py"

GH_BIN="${GH:-gh}"

usage() {
    sed -n '2,/^$/p' "$0"
}

die() {
    echo "error: $*" >&2
    exit 2
}

resolve_python() {
    local py="${PYTHON:-}"
    if [ -z "$py" ]; then
        if command -v python3 >/dev/null 2>&1; then
            py=python3
        elif command -v python >/dev/null 2>&1; then
            py=python
        fi
    fi
    [ -n "$py" ] || die "no python interpreter found (python3/python)"
    printf '%s\n' "$py"
}

# --selftest 的用例跑法：<name> <expected rc> <gh 是否应被调用 0|1> <case 参数...>。
# 每个用例前清掉假 gh 的参数落盘文件，用 FAKE_GH_ARGS/GH 注入到 pr-create 的环境。
run_case() {
    local name="$1" want_rc="$2" want_gh="$3"
    shift 3
    local log="$SELFTEST_TMP/gh-args"
    rm -f "$log"
    FAKE_GH_ARGS="$log" GH="$SELFTEST_TMP/bin/gh" "$SCRIPT" "$@" >/dev/null 2>&1
    local rc=$?
    local called=0
    [ -f "$log" ] && called=1
    if [ "$rc" -ne "$want_rc" ] || [ "$called" -ne "$want_gh" ]; then
        echo "selftest FAIL [$name]: rc=$rc (want $want_rc), gh-called=$called (want $want_gh)" >&2
        return 1
    fi
    echo "selftest ok [$name]"
    return 0
}

run_selftest() {
    SELFTEST_TMP="$(mktemp -d)"
    trap 'rm -rf "$SELFTEST_TMP"' EXIT

    mkdir -p "$SELFTEST_TMP/bin"
    cat >"$SELFTEST_TMP/bin/gh" <<'FAKE_GH'
#!/usr/bin/env bash
printf '%s\n' "$@" > "$FAKE_GH_ARGS"
echo "https://example.invalid/pull/1"
FAKE_GH
    chmod +x "$SELFTEST_TMP/bin/gh"

    printf 'Part of #504\n\n正文里没有 closing keyword。\n' >"$SELFTEST_TMP/good-body.md"
    printf 'body\n\nCloses #504\n' >"$SELFTEST_TMP/bad-body.md"

    local fails=0
    run_case "合法标题+正文 → 建 PR" 0 1 \
        --title "chore(#504): retro hardening" --body-file "$SELFTEST_TMP/good-body.md" \
        --base main --head chore/issue-504-retro-spec-06-hardening || fails=$((fails + 1))
    run_case "正文含 closing keyword → 拒建" 1 0 \
        --title "chore(#504): retro hardening" --body-file "$SELFTEST_TMP/bad-body.md" || fails=$((fails + 1))
    run_case "标题含 closing keyword → 拒建" 1 0 \
        --title "fix: #504 retro hardening" --body-file "$SELFTEST_TMP/good-body.md" || fails=$((fails + 1))
    run_case "正文文件缺失 → 参数错误" 2 0 \
        --title "chore(#504): retro hardening" --body-file "$SELFTEST_TMP/missing.md" || fails=$((fails + 1))
    run_case "缺 --title → 参数错误" 2 0 \
        --body-file "$SELFTEST_TMP/good-body.md" || fails=$((fails + 1))

    if [ "$fails" -ne 0 ]; then
        echo "selftest FAILED: $fails case(s)" >&2
        return 1
    fi
    echo "selftest: 5 cases behave as expected"
    return 0
}

TITLE=""
BODY=""
SELFTEST=0
SELFTEST_TMP=""
PASSTHRU=()

while [ $# -gt 0 ]; do
    case "$1" in
        --title)
            [ $# -ge 2 ] && [ -n "${2:-}" ] || die "--title needs a non-empty value"
            TITLE="$2"
            shift 2
            ;;
        --body-file)
            [ $# -ge 2 ] && [ -n "${2:-}" ] || die "--body-file needs a non-empty value"
            BODY="$2"
            shift 2
            ;;
        --selftest)
            SELFTEST=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        --)
            shift
            PASSTHRU+=("$@")
            break
            ;;
        *)
            PASSTHRU+=("$1")
            shift
            ;;
    esac
done

if [ "$SELFTEST" -eq 1 ]; then
    run_selftest
    exit $?
fi

[ -n "$TITLE" ] || die "missing --title"
[ -n "$BODY" ] || die "missing --body-file"
[ -f "$BODY" ] || die "PR body file not found: $BODY"
[ -s "$BODY" ] || die "PR body file is empty: $BODY"

PY="$(resolve_python)" || exit $?

# 标题守卫（R1 前缀 / R2 长度 / R3 closing keyword）——不通过则不建 PR
if ! "$PY" "$SUBJECT_CHECK" --subject "$TITLE"; then
    echo "error: PR title 未通过校验；标题应为 '<type>(#<issue>): <描述>'" >&2
    exit 1
fi

# 正文守卫（R4 closing keyword）——不通过则不建 PR
if ! "$PY" "$SUBJECT_CHECK" --pr-body-file "$BODY"; then
    echo "error: PR 正文含 GitHub auto-close 形态；删除 closing keyword 后再建 PR（#495 曾因此多跑一轮 CI）" >&2
    exit 1
fi

exec "$GH_BIN" pr create --title "$TITLE" --body-file "$BODY" "${PASSTHRU[@]}"
