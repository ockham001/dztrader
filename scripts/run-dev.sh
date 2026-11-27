#!/bin/bash
# 调试启动脚本：build → install → 从 install 目录启动 master
#
# 为什么先 install 再启动（#168）：
#   build/ 是中间产物目录，二进制在那里会被下次构建重链；Windows 上这会导致
#   LNK1168（文件被运行中的进程占用），Linux 上虽因链接器先 unlink 而侥幸成功，
#   但「跑的是哪一版」就说不清了。install/ 是唯一产物源：运行、部署、打包同源。
#
# DZTRADER_HOME 默认 <repo>/.dztrader_dev；已设置则尊重调用方（并行跑多个栈时
# 直接 DZTRADER_HOME=... ./scripts/run-dev.sh，不需要额外参数）。
#
# 用法:
#   ./scripts/run-dev.sh                # build + install + 启动
#   ./scripts/run-dev.sh --no-build     # 跳过 build/install，直接启动
#   ./scripts/run-dev.sh --stop         # 停止 master 及其子进程
#
# 启动前比对 install/<plat>/<arch>/<cfg>/BUILD_INFO 与当前 HEAD（#515）：不一致或缺失时
# 打 warning（含两侧 commit 与 built_at）并提示重建，**不阻断启动**——旧产物仍可启动，
# 但「跑的是哪一版」必须可见。校验逻辑在 scripts/check_build_info.py。
#
# 停止: ./scripts/run-dev.sh --stop（或 pkill dztraderd）

set -euo pipefail

BUILD=1
STOP=0

while [ $# -gt 0 ]; do
    case "$1" in
        --no-build) BUILD=0; shift ;;
        --stop) STOP=1; shift ;;
        -h|--help)
            echo "Usage: $0 [--no-build] [--stop]"
            echo "  默认: build + install + 启动 master"
            echo "  --no-build: 跳过 build/install，直接启动"
            echo "  --stop: 停止 master 及其子进程"
            echo "  DZTRADER_HOME 已设置时尊重调用方（默认 <repo>/.dztrader_dev）"
            exit 0
            ;;
        *) echo "错误: 未知参数 $1" >&2; exit 1 ;;
    esac
done

project_root="$(cd "$(dirname "$0")/.." && pwd)"
install_dir="$project_root/install/linux/x86_64/Release"
exe="$install_dir/dztraderd"

# DZTRADER_HOME 必须在任何 dz* 可执行文件之前设置（含 --cleanup-orphans）：
# 漏设会让进程在真实 home 下建 shm/db/configs（AGENTS.md 硬门禁 2）。
# 优先尊重调用方已设置的值：并行跑多个栈时靠它隔离。
export DZTRADER_HOME="${DZTRADER_HOME:-$project_root/.dztrader_dev}"

# --stop：先礼后兵。master 有 --cleanup-orphans 负责清理僵尸子进程。
if [ "$STOP" -eq 1 ]; then
    if [ -f "$exe" ]; then
        echo "停止 master 并清理孤儿进程..."
        "$exe" --cleanup-orphans 2>/dev/null || true
    fi
    # 用 pkill -x（精确匹配进程名）而不是 pkill -f（匹配整个命令行）：
    # -f 会命中「命令行里提到 dztraderd 的其他进程」——包括本脚本自身
    # （因此曾出现 --stop 把自己杀掉、后续步骤全部丢失的问题）。
    for name in dztraderd dzweb dzmd_ctp dztd_ctp dzsysmon stg_demo; do
        pkill -x "$name" 2>/dev/null || true
    done
    # 网关/策略可能有带后缀的其他实现（dzmd_xxx / dztd_xxx / stg_xxx），
    # 用 -f 但限定「路径以 /<前缀> 开头」，避免误伤命令行文本。
    pkill -f '/dzmd_[a-z0-9_]*$' 2>/dev/null || true
    pkill -f '/dztd_[a-z0-9_]*$' 2>/dev/null || true
    pkill -f '/stg_[a-z0-9_]*$' 2>/dev/null || true
    echo "已停止"
    exit 0
fi

if [ "$BUILD" -eq 1 ]; then
    # 构建必须停在 install 之前：install 会替换 install/ 下的文件，
    # 而正在运行的进程栈还占着这些文件（尤其 web/ 目录是逐文件写入的）。
    # 先停栈 → 再 install → 再启动，避免读到半更新状态。
    # 用 pgrep -x 精确匹配进程名（-f 会命中脚本自身命令行里的 "dztraderd" 字样）。
    if pgrep -x 'dztraderd' >/dev/null 2>&1; then
        echo "检测到 master 正在运行，先停止再重建（避免 install 覆盖运行中的文件）"
        "$0" --stop
    fi

    "$(dirname "$0")/build.sh"

    # install + 分离调试符号（部署包 ~65M，符号单独归档）
    "$(dirname "$0")/install-artifacts.sh"
fi

if [ ! -f "$exe" ]; then
    echo "错误: 可执行文件不存在: $exe" >&2
    echo "请先运行: ./scripts/setup.sh release && ./scripts/build.sh" >&2
    exit 1
fi

# 构建身份校验（#515）：install 树里的二进制/前端是否对应当前 HEAD。
# 背景（#508 关单时实测）：install/ 产物原先没有任何 commit 标识，只能靠 mtime/sha256
# 手工推断，结论是宿主栈跑着旧二进制、前端静态文件却已被刷新——新旧混用难以解释。
# 只 warning 不阻断（issue #515 AC2）：旧产物仍可启动，但「跑的是哪一版」必须可见。
# --no-build 时尤其重要：build 树、install 树都可能停在旧提交。
check_build_identity() {
    local py=""
    if command -v python3 >/dev/null 2>&1; then
        py=python3
    elif command -v python >/dev/null 2>&1; then
        py=python
    else
        echo "warning: no python interpreter; build identity check skipped" >&2
        return 0
    fi
    local rc=0
    "$py" "$project_root/scripts/check_build_info.py" check \
        --info "$install_dir/BUILD_INFO" --repo "$project_root" || rc=$?
    # 结论行（ok/stale/missing/unknown）始终打出来：这份日志就是「启动栈含新二进制」的取证。
    if [ "$rc" -ne 0 ]; then
        echo "warning: install tree does not match HEAD; the running stack may be stale" >&2
    fi
    # 校验永不阻断启动（#515 AC2）：check_build_info 的退出码只用于日志分类。
    return 0
}

check_build_identity

echo "启动 master | home=$DZTRADER_HOME exe=$exe"
exec "$exe"
