#!/usr/bin/env bash
# dztrader 构建脚本（Linux）
# 构建 C++ + 前端（含 dist 复制到 build/.../web/），一次搞定。
#
# config: 无参数即 Release（preset 名本身已编码 config，无需粘性文件）
#
# 用法:
#   ./scripts/build.sh                           # Release
#   ./scripts/build.sh --config debug            # Debug（仅本地）
#   ./scripts/build.sh --target dzmd_ctp         # 只构建 dzmd_ctp target
#   ./scripts/build.sh --target dzcore --target dzweb   # 多个 target 累积（#281）
#   ./scripts/build.sh --config debug --target dzcore

set -euo pipefail

usage() {
    echo "Usage: $0 [--config release|debug|asan] [--target <name>]..."
    echo "  无参数时默认 release；--target 可重复，累积后一次透传给 cmake --build"
}

# 取 flag 的参数；缺参数时报用法。${2:?} 的 bash 内建报错只有 "parameter null or
# not set"、不含用法，且覆盖式赋值让重复 flag 静默丢弃前值（#281/#250 门禁假绿）。
require_value() {  # <实际剩余参数个数 $#> <flag 名>
    if [ "$1" -lt 2 ]; then
        echo "error: $2 needs a value" >&2
        usage >&2
        exit 2
    fi
}

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# ccache 环境：launcher 已写进 CMakeCache（setup.sh），但 CCACHE_DIR 不持久，
# 必须在每个 build shell 重新导出，否则退回只读的 ~/.cache/ccache 直接构建失败。
# shellcheck source=scripts/ccache-env.sh
if command -v ccache >/dev/null 2>&1; then
    . "$REPO_ROOT/scripts/ccache-env.sh"
    dz_export_ccache_env "$REPO_ROOT"
fi

# 构建锁：ninja 不锁构建树，两个并发 ninja 会交错写 .ninja_deps / .ninja_log，
# 产出陈旧或交错的 .o 与不可信的耗时记录。锁只在叶子脚本加（见 build-lock.sh 注释：
# gate.sh 持锁后再调本脚本会父子互等死锁，故 gate.sh 不再持锁）。
# shellcheck source=scripts/build-lock.sh
. "$REPO_ROOT/scripts/build-lock.sh"
dz_acquire_build_lock "$REPO_ROOT" "build" || exit 2

# DSH workspace-write 沙箱下 ~/.npm 可能只读：回退到仓库内 npm cache（.cache/ 已 gitignore）
if [ -z "${npm_config_cache:-}" ]; then
    npm_cache_ok=0
    if mkdir -p "$HOME/.npm" 2>/dev/null && touch "$HOME/.npm/.dz_build_probe" 2>/dev/null; then
        npm_cache_ok=1
    fi
    rm -f "$HOME/.npm/.dz_build_probe" 2>/dev/null || true
    if [ "$npm_cache_ok" -eq 0 ]; then
        export npm_config_cache="$REPO_ROOT/.cache/npm"
        mkdir -p "$npm_config_cache"
        echo "warning: ~/.npm is not writable; using npm_config_cache=$npm_config_cache" >&2
    fi
fi

CONFIG=""
TARGETS=()

while [ $# -gt 0 ]; do
    case "$1" in
        -c|--config) require_value $# "--config"; CONFIG="$2"; shift 2 ;;
        -t|--target) require_value $# "--target"; TARGETS+=("$2"); shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "error: unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

# config 默认 Release；要其他档显式传 --config debug / asan
if [ -z "$CONFIG" ]; then
    CONFIG="release"
fi

CONFIG=$(echo "$CONFIG" | tr '[:upper:]' '[:lower:]')
case "$CONFIG" in
    release|debug|asan) ;;
    *) echo "Error: config 必须是 release / debug / asan"; exit 1 ;;
esac

PRESET="linux-$CONFIG"

CMAKE_ARGS=(--build --preset "$PRESET")
if [ ${#TARGETS[@]} -gt 0 ]; then
    # 一个 --target 跟多个值：cmake --build 支持多 target，不需要重复 flag。
    CMAKE_ARGS+=(--target "${TARGETS[@]}")
fi

echo "cmake ${CMAKE_ARGS[*]}"
cmake "${CMAKE_ARGS[@]}"
