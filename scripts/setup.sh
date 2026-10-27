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

# 本地 git hooks（.githooks）：pre-commit 跑 staged 子集机械检查，pre-push 拦直推 main。
# 权威门禁仍是 preflight 与 CI；hooks 只是快速护栏（见 docs/development-checklist.md C 段）。
if [ ! -d "$REPO_ROOT/.githooks" ]; then
    echo "warning: .githooks not found; skip installing git hooks" >&2
elif ! git -C "$REPO_ROOT" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    echo "warning: not a git worktree; skip installing git hooks" >&2
elif git -C "$REPO_ROOT" config --local core.hooksPath .githooks; then
    echo "git hooks: core.hooksPath=.githooks"
else
    echo "warning: failed to install git hooks; continuing without hooks" >&2
fi

# ccache 的 CCACHE_DIR 需要每个调用 ccache 的 shell 自己导出（见 scripts/ccache-env.sh）
# shellcheck source=scripts/ccache-env.sh
. "$REPO_ROOT/scripts/ccache-env.sh"

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
    EXPECT_CCACHE_LAUNCHER=0
    if command -v ccache >/dev/null 2>&1; then
        dz_export_ccache_env "$REPO_ROOT"
        if [ -w "$CCACHE_DIR" ]; then
            CMAKE_EXTRA_ARGS+=("-DCMAKE_CXX_COMPILER_LAUNCHER=ccache")
            EXPECT_CCACHE_LAUNCHER=1
            echo "ccache: enabled (CCACHE_DIR=$CCACHE_DIR)"
        else
            # 目录不可写时不要写 launcher：否则后续 build 每次调用 ccache 都会失败
            echo "warning: ccache cache dir is not writable ($CCACHE_DIR); ccache launcher disabled" >&2
        fi
    fi
    # 预发布标识透传（ADR 0038）：发布流水线设置 DZ_PRERELEASE=rc1，
    # 产物据此得到完整 SemVer "0.1.0-rc1"。本地不设 → 空串 → 正式版串。
    #
    # 必须**无条件**传参（即使为空）：DZ_PRERELEASE 是 CMake cache 变量，
    # 只在非空时传参会让上一次发布构建的 rc1 永久残留在 cache 里，
    # 导致后续所有本地构建都自称 "0.1.0-rc1"。
    CMAKE_EXTRA_ARGS+=("-DDZ_PRERELEASE=${DZ_PRERELEASE:-}")
    if [ -n "${DZ_PRERELEASE:-}" ]; then
        echo "prerelease: ${DZ_PRERELEASE}"
    fi
    echo "cmake --preset $PRESET"
    cmake --preset "$PRESET" "${CMAKE_EXTRA_ARGS[@]}"

    # ccache launcher 校验：曾出现过「configure 时传了 -DCMAKE_CXX_COMPILER_LAUNCHER=ccache，
    # 但那棵 build 树的 CMakeCache 里根本没有该变量」的配置漂移 —— 于是后续所有增量构建
    # 都在裸编，ccache 命中率归零且无人察觉（#168）。这里改成「写进去并复核」：
    # 仅在本次确实传了 launcher 时才要求能读回来（目录不可写的降级路径不算漂移）。
    if [ "$EXPECT_CCACHE_LAUNCHER" -eq 1 ]; then
        CACHE_FILE="$REPO_ROOT/$OUTPUT_FOLDER/CMakeCache.txt"
        CACHED_LAUNCHER=$(grep -E '^CMAKE_CXX_COMPILER_LAUNCHER:' "$CACHE_FILE" 2>/dev/null | cut -d= -f2- || true)
        if [ -n "$CACHED_LAUNCHER" ]; then
            echo "ccache: verified launcher=$CACHED_LAUNCHER (in CMakeCache)"
        else
            echo "error: launcher was passed but is missing from CMakeCache" >&2
            echo "       expected CMAKE_CXX_COMPILER_LAUNCHER in $CACHE_FILE" >&2
            echo "       rebuilds would silently bypass ccache" >&2
            exit 1
        fi
    fi

    # 拷贝 compile_commands.json 到项目根（clangd 用）
    CC="$REPO_ROOT/$OUTPUT_FOLDER/compile_commands.json"
    if [ -f "$CC" ]; then
        cp "$CC" "$REPO_ROOT/compile_commands.json"
        echo "compile_commands.json → 项目根（clangd）"
    fi
fi

echo ""
echo "=== Setup done ==="

if [ "$CONFIG" = "debug" ]; then
    echo ""
    echo "已配置 Debug（仅本地用途，不进门禁、禁止部署）"
    echo "注意: 切到 Debug 会触发 14 个依赖源码编译（ConanCenter 只有 Release 二进制）"
fi

echo ""
echo "Next steps:"
echo "  Build:  ./scripts/build.sh                    # 默认 Release"
echo "  Run:    ./scripts/run-dev.sh                  # build + install + 启动"
echo "  Stop:   ./scripts/run-dev.sh --stop"
echo "  Clean:  ./scripts/clean.sh"
echo ""
echo "Config 选择: 无参数即 Release。其他档显式给 preset:"
echo "  ./scripts/setup.sh debug                      # Debug（仅本地）"
echo "  cmake --preset linux-asan                     # ASan+UBSan（只从 build 树跑）"
