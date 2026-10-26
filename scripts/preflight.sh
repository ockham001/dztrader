#!/usr/bin/env bash
# preflight.sh — 提交前本地门禁的唯一入口（Linux）
#
# 覆盖：日志格式 / clang-format 21.x / 文档链接 / 测试可移植性+临时目录隔离 / MSVC hazard lint /
#       前端五件套。MSVC 侧失败（Windows 头顺序、C4101 等）Linux 编译器拦不到，
#       由 scripts/check_msvc_hazards.py 做源码级 lint。
# CI 的 linux job 复用同一脚本的 C 段（--c-only），避免门禁命令在
# 人工 prompt、development-checklist 与 ci.yml 三处漂移。
#
# 用法:
#   ./scripts/preflight.sh              # 全量（C 段 + 前端；需要 node_modules）
#   ./scripts/preflight.sh --c-only     # 只跑 C 段（CI 用，不需要 node_modules）
#   ./scripts/preflight.sh --help
#
# 退出码: 0=全绿, 1=有检查失败, 2=环境不满足（缺工具/缺 node_modules）
#
# 环境变量:
#   PYTHON          python 解释器（默认自动找 python3，再回退 python）
#   CLANG_FORMAT    clang-format 可执行文件（默认自动找 clang-format-21 / clang-format）

set -u -o pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

C_ONLY=0
case "${1:-}" in
    "") ;;
    --c-only) C_ONLY=1 ;;
    -h|--help) sed -n '2,/^$/p' "$0"; exit 0 ;;
    *) echo "error: unknown option: $1" >&2; exit 1 ;;
esac

START=$(date +%s)
STEP=0

run_step() {
    local name="$1"
    shift
    STEP=$((STEP + 1))
    printf '[%d] %s\n' "$STEP" "$name"
    local t0
    t0=$(date +%s)
    "$@"
    local rc=$?
    if [ "$rc" -eq 0 ]; then
        printf '    ok (%ss)\n' "$(( $(date +%s) - t0 ))"
    else
        printf '    FAILED (%ss)\n' "$(( $(date +%s) - t0 ))" >&2
        exit "$rc"
    fi
}

# ---- 工具解析 -------------------------------------------------------------

if [ -n "${PYTHON:-}" ]; then
    :
elif command -v python3 >/dev/null 2>&1; then
    PYTHON=python3
elif command -v python >/dev/null 2>&1; then
    PYTHON=python
else
    echo "error: no python interpreter found (python3/python)" >&2
    exit 2
fi

CF="${CLANG_FORMAT:-}"
if [ -z "$CF" ]; then
    for candidate in clang-format-21 clang-format; do
        if command -v "$candidate" >/dev/null 2>&1; then
            CF=$(command -v "$candidate")
            break
        fi
    done
fi
if [ -z "$CF" ]; then
    echo "error: clang-format 21.x not found; install it or set CLANG_FORMAT" >&2
    exit 2
fi
CF_VERSION="$("$CF" --version 2>&1 | head -n 1)"
case "$CF_VERSION" in
    *"clang-format version 21."*) ;;
    *)
        echo "error: need clang-format 21.x, got: $CF_VERSION" >&2
        exit 2
        ;;
esac

# ---- C 段 ----------------------------------------------------------------

check_log_format() {
    "$PYTHON" scripts/check_log_format.py apps/ libs/
}

check_clang_format() {
    find libs apps \( -name '*.h' -o -name '*.cpp' \) -print0 |
        xargs -0 "$CF" --dry-run --Werror
}

check_docs_links() {
    "$PYTHON" scripts/check_docs_links.py
}

check_test_portability() {
    "$PYTHON" scripts/check_test_portability.py
}

check_msvc_hazards() {
    "$PYTHON" scripts/check_msvc_hazards.py
}

# ---- 前端 ----------------------------------------------------------------

check_frontend() {
    local fe="$ROOT/apps/webui/frontend"
    if ! command -v npm >/dev/null 2>&1; then
        echo "error: npm not found; install Node.js or run --c-only" >&2
        return 2
    fi
    if [ ! -d "$fe/node_modules" ]; then
        echo "error: $fe/node_modules missing; run ./scripts/build.sh first" >&2
        return 2
    fi

    # DSH workspace-write 沙箱下 ~/.npm 可能只读：回退到仓库内 cache（.cache/ 已 gitignore）
    if [ -z "${npm_config_cache:-}" ]; then
        local npm_cache_ok=0
        if mkdir -p "$HOME/.npm" 2>/dev/null &&
            touch "$HOME/.npm/.dz_preflight_probe" 2>/dev/null; then
            npm_cache_ok=1
        fi
        rm -f "$HOME/.npm/.dz_preflight_probe" 2>/dev/null || true
        if [ "$npm_cache_ok" -eq 0 ]; then
            export npm_config_cache="$ROOT/.cache/npm"
            mkdir -p "$npm_config_cache"
            echo "    note: ~/.npm is not writable; npm_config_cache=$npm_config_cache"
        fi
    fi

    (
        cd "$fe" || exit 1
        npm run type-check && npm run lint && npm run lint:css && npm test
    ) || return 1

    (
        cd "$fe" || exit 1
        npm run gen:types
    ) || return 1
    if ! git diff --exit-code -- apps/webui/frontend/src/types/generated.ts >/dev/null; then
        echo "error: generated.ts is stale; commit the regenerated file" >&2
        git diff --stat -- apps/webui/frontend/src/types/generated.ts >&2
        return 1
    fi
}

# ---- 执行 ----------------------------------------------------------------

echo "preflight: python=$PYTHON clang-format=$CF ($CF_VERSION)"
run_step "log format (apps/ libs/)" check_log_format
run_step "clang-format 21.x dry-run (libs apps)" check_clang_format
run_step "docs links" check_docs_links
run_step "test portability + temp-dir isolation" check_test_portability
run_step "msvc hazard lint" check_msvc_hazards
if [ "$C_ONLY" -eq 0 ]; then
    run_step "frontend (type-check/lint/lint:css/test/gen:types)" check_frontend
fi

printf 'preflight: all %d checks passed in %ss\n' "$STEP" "$(( $(date +%s) - START ))"
