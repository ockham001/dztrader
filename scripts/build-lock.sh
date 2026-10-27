#!/usr/bin/env bash
# build-lock.sh — 被 build.sh / test.sh source（不直接执行）
#
# 为什么需要：ninja 不锁构建树。两个并发 ninja 写同一棵树会交错写 .ninja_deps /
# .ninja_log，产出损坏或半旧的 .o（实测同一台机器上出现过：同一时刻 137 条边并发，
# 而 ninja 默认 jobs = nproc+2 = 10；1267 条 ninja log 记录里 385 条 start 时间倒退）。
#
# 锁的归属：**只有叶子脚本**（build.sh / test.sh）加锁。
# `gate.sh` 曾经持锁后再调叶子脚本 —— 那会自锁：两次打开同一个文件得到不同的
# open file description，flock 互斥，子进程永远等父进程持有的锁。故 gate.sh 已移除锁。
# `run-dev.sh` 不自己加锁，它调用 build.sh（加锁一次），锁在 build.sh 退出时释放。
#
# 锁文件放各自仓的 .cache/：不同 worktree 构建不同的树，无需共享；ccache 自身
# 对并发安全（原子写入），所以缓存仍跨 worktree 共享。

# dz_acquire_build_lock <repo_root> <label>
#   取得该仓的构建锁。已有构建/测试在跑时先打印等待提示再阻塞等待。
#   失败（无 flock 或无法建锁文件）时返回非 0，调用方应退出。
dz_acquire_build_lock() {
    local repo_root="$1"
    local label="${2:-build}"
    local lock_dir="$repo_root/.cache"
    local lock_file="$lock_dir/build.lock"

    if ! command -v flock >/dev/null 2>&1; then
        echo "error: flock not found (util-linux); cannot serialize build/test" >&2
        return 1
    fi

    if ! mkdir -p "$lock_dir" 2>/dev/null; then
        echo "error: cannot create lock dir | path=$lock_dir" >&2
        return 1
    fi

    # fd 9 在本 shell 内保持打开；脚本退出时内核自动释放锁
    if ! exec 9>"$lock_file"; then
        echo "error: cannot open lock file | path=$lock_file" >&2
        return 1
    fi

    if ! flock -n 9; then
        echo "waiting for build lock | label=$label holder=\"another build/test in this worktree\"" >&2
        if ! flock 9; then
            echo "error: failed to acquire build lock | path=$lock_file" >&2
            return 1
        fi
        echo "build lock acquired | label=$label" >&2
    fi
    return 0
}
