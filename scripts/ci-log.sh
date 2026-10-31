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

# 错误模式是**类别登记表**，不是一次性修复：每新增一类真实 CI 红，就把该类的错误串
# 加进来并在注释里登记票号/run 号，同时在下面 selftest_pattern() 里补一条 must-match
# 样本。原因：本脚本一旦漏掉某类错误，report_log() 会退化成「no known error pattern;
# last 60 lines」，agent 只能手工翻几十 MB 日志重找（#200 实测因此多耗约 28 分钟）。
#
#   error C####/LNK####/MSB####  —— MSVC 编译/链接失败
#   CMake Error                  —— CMake 配置期/自定义命令失败（含 gtest_discover_tests
#                                   发现超时，见 #227：Windows runner 冷启动时
#                                   `Result: Process terminated due to timeout` 只出现在
#                                   CMake 的 message 里，没有 error C 前缀）
#   FAILED:                      —— ninja 单条规则失败（CMake 会把真实错误打在其后）
#
# `ERROR:` 必须限定日志级别形态（`ERROR:` / `[ERROR]` / `: ERROR:`），不能裸写 `ERROR:`：
# GitHub Actions 会把 env dump 打进日志，`SCCACHE_IGNORE_SERVER_IO_ERROR: 1` 这类行会被
# 裸 `ERROR:` 命中，用噪声把 report_log 的 `head -120` 窗口占满，真正的根因（在日志尾部
# 数万行处）就被挤掉了（#200 实测那轮 19 处命中里 9 处是这种噪声）。
ERROR_PATTERN='\[  FAILED  \]|The following tests FAILED|FAILED TEST|error C[0-9]{4}|error LNK[0-9]+|fatal error|FAILED:|ninja: build stopped|error MSB[0-9]+|Test Failed|(^|[[:space:]:])ERROR:|\[ERROR\]|CMake Error|Process terminated due to timeout'

# 错误模式自测（--selftest）：把「必须命中」与「必须不命中」的样本行喂给 ERROR_PATTERN。
# 存在理由与 check_json_vendor_version.py --selftest 相同——规则本身也会写错：
# 一个漏掉某类错误的模式会让整条 CI 排查链退化成「翻几十 MB 日志」（#200 实测 28 分钟），
# 一个过宽的模式会被 env dump 噪声占满窗口（SCCACHE_*_ERROR 曾命中裸 `ERROR:`）。
# 该函数只读、不联网，preflight 与 CI 都会跑。
#
# 样本格式为 `理由|样本行`，以 `|` 分隔（只在第一个 `|` 处切分）——因此**样本行本身
# 不能含 `|`**；需要含 `|` 的样本请改写模式或换一条等价样本。
selftest_pattern() {
    local fails=0
    # 必须命中（每行前面是这条样本存在的理由）
    while IFS='|' read -r why sample; do
        [ -z "$why" ] && continue
        if ! printf '%s\n' "$sample" | grep -qE "$ERROR_PATTERN"; then
            printf 'selftest FAIL: 应命中但未命中 [%s] %s\n' "$why" "$sample" >&2
            fails=$((fails + 1))
        fi
    done <<'CASES'
msvc compile error|2026-01-01T00:00:00Z src/foo.cpp(12,3): error C2065: 'x': undeclared identifier
msvc link error|2026-01-01T00:00:00Z foo.obj : error LNK2019: unresolved external symbol
ninja rule failed|2026-01-01T00:00:00Z FAILED: [code=1] tests/dzlog_init_test.exe
cmake error|2026-01-01T00:00:00Z CMake Error at Modules/GoogleTestAddTests.cmake:233 (message):
gtest discovery timeout|2026-01-01T00:00:00Z     Result: Process terminated due to timeout
ctest case failed|2026-01-01T00:00:00Z [  FAILED  ] FooTest.Bar (12 ms)
log-level ERROR|2026-01-01T00:00:00Z ERROR: something went wrong
bracketed ERROR|2026-01-01T00:00:00Z [ERROR] something went wrong
prefixed ERROR|2026-01-01T00:00:00Z 2026-01-01 foo: ERROR: bar
CASES
    # 必须不命中（噪声样本：命中即说明模式过宽，会挤掉真根因）
    while IFS='|' read -r why sample; do
        [ -z "$why" ] && continue
        if printf '%s\n' "$sample" | grep -qE "$ERROR_PATTERN"; then
            printf 'selftest FAIL: 不应命中但命中了 [%s] %s\n' "$why" "$sample" >&2
            fails=$((fails + 1))
        fi
    done <<'CASES'
sccache env dump|2026-01-01T00:00:00Z   SCCACHE_IGNORE_SERVER_IO_ERROR: 1
sccache rw mode|2026-01-01T00:00:00Z   SCCACHE_GHA_RW_MODE: READ_ONLY
plain info line|2026-01-01T00:00:00Z [946/941] Building CXX object libs/log/CMakeFiles/dzlog.dir/x.cpp.obj
CASES
    if [ "$fails" -ne 0 ]; then
        printf 'selftest: %d case(s) failed\n' "$fails" >&2
        return 1
    fi
    printf 'selftest: ERROR_PATTERN ok (9 must-match, 3 must-not-match)\n'
    return 0
}

while [ $# -gt 0 ]; do
    case "$1" in
        -h|--help) usage; exit 0 ;;
        --selftest) SELFTEST=1; shift ;;
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

# --selftest 不联网、不需要 gh：在参数校验之前返回，供 preflight / CI 调用
if [ "${SELFTEST:-0}" -eq 1 ]; then
    selftest_pattern
    exit $?
fi

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
