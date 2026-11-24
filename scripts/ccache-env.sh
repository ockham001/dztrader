#!/usr/bin/env bash
# ccache-env.sh — 被 setup.sh / build.sh source（不直接执行）
#
# 为什么需要单独一个文件：`CCACHE_DIR` 是**每个调用 ccache 的进程**的环境变量，
# 不存在于 CMakeCache 里。`setup.sh` 只在 configure 那次 shell 里 export，
# 之后 `build.sh` 在全新的 shell 中运行 ccache 时会退回默认 `~/.cache/ccache`；
# DSH workspace-write 沙箱下该目录只读 → `ccache: error: Read-only file system`
# → 整个构建失败（#81 第一次在新 worktree 构建实测）。
#
# 缓存目录统一取**主仓**的 `.cache/ccache`（git common dir 的父目录），
# 而不是各自 worktree 内的 `.cache/ccache`：所有 worktree 共享同一份缓存，
# 新 worktree 首建才能命中主仓历史缓存（retro #83–#93 引入 ccache 的本意）。

dz_resolve_shared_repo_root() {
    # 主仓根（worktree 共享）：git common dir 的父目录；解析失败回落入参 repo_root。
    # 被 setup.sh（Conan 共享缓存）与 dz_export_ccache_env 共用——同一套 git common dir 解析
    # 不再各写一份（#465）。
    local repo_root="$1" common_dir shared_root
    common_dir="$(git -C "$repo_root" rev-parse --path-format=absolute --git-common-dir 2>/dev/null || true)"
    if [ -z "$common_dir" ]; then
        common_dir="$(git -C "$repo_root" rev-parse --git-common-dir 2>/dev/null || true)"
        case "$common_dir" in
            /*) ;;
            ?*) common_dir="$(cd "$repo_root/$common_dir" 2>/dev/null && pwd || true)" ;;
        esac
    fi
    if [ -n "$common_dir" ]; then
        shared_root="$(cd "$(dirname "$common_dir")" 2>/dev/null && pwd || true)"
    fi
    if [ -z "${shared_root:-}" ]; then
        shared_root="$repo_root"
    fi
    printf '%s\n' "$shared_root"
}

dz_export_ccache_env() {
    local repo_root="$1"
    local shared_root ccache_dir
    shared_root="$(dz_resolve_shared_repo_root "$repo_root")"
    ccache_dir="$shared_root/.cache/ccache"
    CCACHE_DIR="${CCACHE_DIR:-$ccache_dir}"
    export CCACHE_DIR
    mkdir -p "$CCACHE_DIR" 2>/dev/null || true
}
