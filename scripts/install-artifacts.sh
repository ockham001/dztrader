#!/usr/bin/env bash
# install-artifacts.sh — 把构建树安装成可部署产物，并分离调试符号
#
# 为什么需要（#168）：
#   Release 带 -g 后，二进制膨胀 3–6 倍（dzweb 16M → 103M，install/ 401M）。
#   直接 cmake --install --strip 虽能把体积降到 58M，但会**彻底丢弃行号**，
#   生产崩溃只剩函数名 —— 那正是当初加 -g 要解决的问题。
#
# 做法是 Linux 生态的标准分离方案：
#   1) 正常 install（保留调试信息）
#   2) 每个 ELF 用 objcopy --only-keep-debug 抽出 <name>.debug
#   3) objcopy --strip-debug 压掉调试段
#   4) objcopy --add-gnu-debuglink 把 debuglink 写回二进制
#   => 部署目录 ~58M，符号文件单独躺在 <cfg>-debug-symbols/，addr2line/gdb 仍能
#      按 debuglink 找到行号。符号目录不在部署包路径内，不会被发出去。
#
# Windows 走另一条路（Q25=B 决定）：MSVC 的 .pdb 不进发布包，由 tag 流水线
# 单独归档。本脚本在 Windows/MSVC 下只做 install，不碰 PDB。
#
# 用法:
#   ./scripts/install-artifacts.sh [--config release|debug]
#
# 环境: 需要 objcopy（binutils）。缺失时降级为「不 strip 也不分离」，并告警。

set -euo pipefail

CONFIG="release"
while [ $# -gt 0 ]; do
    case "$1" in
        -c|--config) CONFIG="${2:?--config 需要参数}"; shift 2 ;;
        -h|--help)
            echo "Usage: $0 [--config release|debug]"
            exit 0
            ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done

CONFIG=$(echo "$CONFIG" | tr '[:upper:]' '[:lower:]')
case "$CONFIG" in
    release|debug) ;;
    *) echo "Error: config 必须是 release 或 debug" >&2; exit 1 ;;
esac

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CONFIG_CAP="$(echo "$CONFIG" | sed 's/^./\U&/')"

case "$(uname -s)" in
    Linux) PLATFORM="linux" ;;
    Darwin) PLATFORM="macos" ;;
    *)
        echo "error: unsupported platform | os=$(uname -s)" >&2
        exit 1
        ;;
esac

BUILD_DIR="$REPO_ROOT/build/$PLATFORM/x86_64/$CONFIG_CAP"
INSTALL_ROOT="$REPO_ROOT/install/$PLATFORM/x86_64/$CONFIG_CAP"
SYMBOLS_DIR="$REPO_ROOT/install/$PLATFORM/x86_64/${CONFIG_CAP}-debug-symbols"

if [ ! -d "$BUILD_DIR" ]; then
    echo "error: build dir not found | path=$BUILD_DIR" >&2
    echo "       run ./scripts/setup.sh $CONFIG && ./scripts/build.sh --config $CONFIG" >&2
    exit 1
fi

echo "[1/3] install → $INSTALL_ROOT"
cmake --install "$BUILD_DIR" >/dev/null

if ! command -v objcopy >/dev/null 2>&1; then
    echo "warning: objcopy not found (binutils); keeping full debug info in binaries" >&2
    echo "warning: install dir will be large (~400MB) and NOT stripped" >&2
    exit 0
fi

echo "[2/3] separate debug symbols → $SYMBOLS_DIR"
rm -rf "$SYMBOLS_DIR"
mkdir -p "$SYMBOLS_DIR"

# 只处理 install 产物里的 ELF（可执行文件与共享库）
split_count=0
while IFS= read -r -d '' f; do
    if ! file -b "$f" 2>/dev/null | grep -q '^ELF'; then
        continue
    fi
    rel="${f#"$INSTALL_ROOT"/}"
    symbols="$SYMBOLS_DIR/$rel.debug"
    mkdir -p "$(dirname "$symbols")"

    # 抽调试信息 → 压掉调试段 → 写回 debuglink。
    # debuglink 记录的是**文件名**，查找位置是二进制同目录或同目录/.debug/。
    # 归档目录与部署目录不同，所以归档里另放一份 .debug/ 镜像布局：
    # 把 <SYMBOLS_DIR>/.debug/ 整棵拷到部署目录根即可让 debuglink 生效。
    if objcopy --only-keep-debug "$f" "$symbols" 2>/dev/null &&
       objcopy --strip-debug "$f" 2>/dev/null &&
       objcopy --add-gnu-debuglink="$symbols" "$f" 2>/dev/null; then
        split_count=$((split_count + 1))
    else
        echo "warning: failed to split debug info | path=$rel" >&2
        rm -f "$symbols"
    fi
done < <(find "$INSTALL_ROOT" -type f -print0)

echo "[3/3] done | split=$split_count deploy=$(du -sh "$INSTALL_ROOT" | cut -f1) symbols=$(du -sh "$SYMBOLS_DIR" | cut -f1)"
echo ""
echo "部署包:   $INSTALL_ROOT"
echo "符号归档: $SYMBOLS_DIR"
echo ""
echo "崩溃分析:"
echo "  addr2line -e \"$SYMBOLS_DIR/<binary-path>.debug\" -f -C 0x<addr>"
echo "  或把 $SYMBOLS_DIR 下的 .debug 平铺到部署目录旁的 .debug/ 子目录，"
echo "  再 addr2line -e <binary>（debuglink 会自动找到）"
