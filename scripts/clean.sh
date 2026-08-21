#!/usr/bin/env bash
# 清理所有 build 产物（保留 conan cache，下次 setup 不重编依赖）
#
# 清理范围:
#   - build/                            # 所有 build 产物（含 dist 复制的 web/）
#   - apps/webui/frontend/dist/         # 前端 build 产物
#   - apps/webui/frontend/node_modules/ # 前端依赖（--include-node-modules 时）
#   - .dztrader_dev/                    # 调试 home（含 .config 粘性文件）
#   - compile_commands.json             # 项目根 clangd 索引
#
# 保留:
#   - ~/.conan2/                        # conan cache（避免下次 setup 重编 20 分钟）
#
# 用法:
#   ./scripts/clean.sh                          # 清理 build 产物
#   ./scripts/clean.sh --include-node-modules   # 连 node_modules 一起删

set -e

PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$PROJECT_ROOT"

INCLUDE_NM=0

while [ $# -gt 0 ]; do
    case "$1" in
        --include-node-modules) INCLUDE_NM=1; shift ;;
        -h|--help)
            echo "Usage: $0 [--include-node-modules]"
            echo "  清理 build/ + frontend/dist/ + .dztrader_dev/ + compile_commands.json"
            echo "  --include-node-modules: 同时删 frontend/node_modules/"
            echo "  保留 ~/.conan2/（下次 setup 不重编依赖）"
            exit 0
            ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

echo "=== dztrader clean ==="

TARGETS=(
    "build"
    "apps/webui/frontend/dist"
    ".dztrader_dev"
    "compile_commands.json"
)
[ "$INCLUDE_NM" -eq 1 ] && TARGETS+=("apps/webui/frontend/node_modules")

for t in "${TARGETS[@]}"; do
    if [ -e "$t" ]; then
        echo "  删除 $t"
        rm -rf "$t"
    else
        echo "  跳过 $t（不存在）"
    fi
done

echo ""
echo "=== Clean done ==="
echo "保留: ~/.conan2/（下次 setup 不重编依赖）"
echo ""
echo "Next steps:"
echo "  Setup:  ./scripts/setup.sh                    # 重新 configure"
echo "  Build:  ./scripts/build.sh"
echo "  Run:    ./scripts/run-dev.sh"
