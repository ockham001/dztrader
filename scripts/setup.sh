#!/usr/bin/env bash
# dztrader 项目 setup 脚本（Linux）
# 一键执行 conan install + cmake configure
#
# 用法:
#   ./scripts/setup.sh release        # Release（默认）
#   ./scripts/setup.sh debug          # Debug
#   ./scripts/setup.sh release --skip-configure  # 只装依赖不配置

set -euo pipefail

CONFIG="${1:-release}"
SKIP_CONFIGURE="${2:-}"

# 参数规范化
CONFIG=$(echo "$CONFIG" | tr '[:upper:]' '[:lower:]')
case "$CONFIG" in
    release|debug) ;;
    *)
        echo "Usage: $0 [release|debug] [--skip-configure]"
        echo "  release  Release 构建（默认）"
        echo "  debug    Debug 构建（自动 --build=missing）"
        exit 1
        ;;
esac

case "$SKIP_CONFIGURE" in
    --skip-configure|-s) SKIP_CONFIGURE=1 ;;
    "") SKIP_CONFIGURE=0 ;;
    *) echo "Unknown option: $SKIP_CONFIGURE"; exit 1 ;;
esac

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
mkdir -p "$REPO_ROOT/.cache"

# DSH workspace-write 沙箱下 ~/.conan2 可能只读：回退到仓库内 CONAN_HOME（.cache/ 已 gitignore），
# 避免 worktree setup 卡在权限申请上。回退时先从可读的 ~/.conan2 播种一次（1.8G 本地拷贝几十秒），
# 否则每个新 worktree 都要重新下载全部依赖（本波 #88 实测 ~50 分钟）。
if [ -z "${CONAN_HOME:-}" ]; then
    conan_cache_ok=0
    if mkdir -p "$HOME/.conan2" 2>/dev/null && touch "$HOME/.conan2/.dz_setup_probe" 2>/dev/null; then
        conan_cache_ok=1
    fi
    rm -f "$HOME/.conan2/.dz_setup_probe" 2>/dev/null || true
    if [ "$conan_cache_ok" -eq 0 ]; then
        export CONAN_HOME="$REPO_ROOT/.cache/conan2"
        mkdir -p "$CONAN_HOME"
        if [ ! -d "$CONAN_HOME/p" ] && [ -d "$HOME/.conan2/p" ]; then
            echo "seeding conan cache from ~/.conan2 → $CONAN_HOME (one-time copy) ..." >&2
            cp -a "$HOME/.conan2/." "$CONAN_HOME"/ 2>/dev/null || true
        fi
        echo "warning: ~/.conan2 is not writable; using CONAN_HOME=$CONAN_HOME" >&2
    fi
fi

PROFILE="profiles/linux-gcc"
CONFIG_CAP=$(echo "$CONFIG" | sed 's/^./\U&/')
OUTPUT_FOLDER="build/linux/x86_64/$CONFIG_CAP"

echo "=== dztrader setup (Linux) ==="
echo "Config:        $CONFIG_CAP"
echo "Profile:       $PROFILE"
echo "OutputFolder:  $OUTPUT_FOLDER"
echo ""

# Step 1: conan install
echo "[1/2] conan install"
CONAN_ARGS=(
    install .
    -pr:b "$PROFILE"
    -pr:h "$PROFILE"
    --output-folder "$OUTPUT_FOLDER"
    --build=missing
)
if [ "$CONFIG" = "debug" ]; then
    CONAN_ARGS+=(-s build_type=Debug)
fi
echo "conan ${CONAN_ARGS[*]}"
cd "$REPO_ROOT"
conan "${CONAN_ARGS[@]}"

# Step 2: cmake configure
if [ "$SKIP_CONFIGURE" -eq 0 ]; then
    echo ""
    echo "[2/2] cmake configure"
    PRESET="linux-$CONFIG"
    # 本机有 ccache 时接入编译器缓存（worktree 首建/合并后重建显著加速；Windows 走 setup.ps1 不涉及）
    CMAKE_EXTRA_ARGS=()
    if command -v ccache >/dev/null 2>&1; then
        export CCACHE_DIR="${CCACHE_DIR:-$REPO_ROOT/.cache/ccache}"
        mkdir -p "$CCACHE_DIR"
        CMAKE_EXTRA_ARGS+=("-DCMAKE_CXX_COMPILER_LAUNCHER=ccache")
        echo "ccache: enabled (CCACHE_DIR=$CCACHE_DIR)"
    fi
    echo "cmake --preset $PRESET"
    cmake --preset "$PRESET" "${CMAKE_EXTRA_ARGS[@]}"

    # 拷贝 compile_commands.json 到项目根（clangd 用）
    CC="$REPO_ROOT/$OUTPUT_FOLDER/compile_commands.json"
    if [ -f "$CC" ]; then
        cp "$CC" "$REPO_ROOT/compile_commands.json"
        echo "compile_commands.json → 项目根（clangd）"
    fi
fi

echo ""
echo "=== Setup done ==="

# 写粘性 config 文件（build/run-dev/clean 读这个决定默认 config）
STICKY_DIR="$REPO_ROOT/.dztrader_dev"
mkdir -p "$STICKY_DIR"
echo -n "$CONFIG_CAP" > "$STICKY_DIR/.config"
echo "已写入粘性 config: $STICKY_DIR/.config = $CONFIG_CAP"

if [ "$CONFIG" = "debug" ]; then
    echo ""
    echo "=== 已切到 Debug，后续 build/run-dev 默认 Debug ==="
    echo "=== DEBUG 仅用于本地调试，禁止部署生产 ==="
fi

echo ""
echo "Next steps:"
echo "  Build:  ./scripts/build.sh                    # 自动跟随 config"
echo "  Run:    ./scripts/run-dev.sh                  # build + 启动"
echo "  Stop:   pkill dztraderd; pkill dzweb; pkill dzmd_ctp"
echo "  Clean:  ./scripts/clean.sh"
