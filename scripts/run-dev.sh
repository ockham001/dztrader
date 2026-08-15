#!/bin/bash
# 调试启动脚本，自动设置 DZTRADER_HOME 指向项目内 .dztrader_dev
# 默认: build + 启动 master
#
# config 来源优先级: --config 参数 > .dztrader_dev/.config 粘性文件 > release
#
# 用法:
#   ./scripts/run-dev.sh                           # build + 启动 master
#   ./scripts/run-dev.sh --no-build                # 不 build，直接启动
#   ./scripts/run-dev.sh --config debug            # 临时用 Debug
#
# 注: 当前无 --stop 子命令，手动停止: pkill dztraderd; pkill dzweb; pkill dzmd_ctp

set -e

BUILD=1
CONFIG=""

while [ $# -gt 0 ]; do
    case "$1" in
        --no-build) BUILD=0; shift ;;
        --config) CONFIG="${2:?--config 需要参数}"; shift 2 ;;
        -h|--help)
            echo "Usage: $0 [--no-build] [--config release|debug]"
            echo "  默认: build + 启动 master"
            echo "  无 --config 时读 .dztrader_dev/.config，无则 release"
            exit 0
            ;;
        *) echo "错误: 未知参数 $1"; exit 1 ;;
    esac
done

project_root="$(cd "$(dirname "$0")/.." && pwd)"
export DZTRADER_HOME="$project_root/.dztrader_dev"

# 读粘性 config，--config 优先覆盖
if [ -z "$CONFIG" ]; then
    STICKY_FILE="$project_root/.dztrader_dev/.config"
    if [ -f "$STICKY_FILE" ]; then
        CONFIG=$(cat "$STICKY_FILE" | tr '[:upper:]' '[:lower:]')
    fi
    [ -z "$CONFIG" ] && CONFIG="release"
fi

CONFIG=$(echo "$CONFIG" | tr '[:upper:]' '[:lower:]')
case "$CONFIG" in
    release|debug) ;;
    *) echo "错误: config 必须是 release 或 debug"; exit 1 ;;
esac

# 默认: 先增量 build
if [ "$BUILD" -eq 1 ]; then
    "$(dirname "$0")/build.sh" --config "$CONFIG"
fi

# Debug 警告
if [ "$CONFIG" = "debug" ]; then
    echo ""
    echo "=== DEBUG BUILD - 仅本地调试，禁止部署生产 ==="
    echo ""
fi

exe="build/linux/x86_64/$(echo "$CONFIG" | sed 's/^./\U&/')/dztraderd"

if [ ! -f "$exe" ]; then
    echo "错误: 可执行文件不存在: $exe" >&2
    echo "请先运行: ./scripts/setup.sh $CONFIG" >&2
    exit 1
fi

echo "启动 master (config=$CONFIG, DZTRADER_HOME=$DZTRADER_HOME)"
exec "$exe"
