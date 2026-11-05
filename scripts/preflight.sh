#!/usr/bin/env bash
# preflight.sh — 提交前本地门禁的唯一入口（Linux）
#
# 覆盖：日志格式 / clang-format 21.x / 文档链接（含活跃文档里反引号路径**与代码符号**的
#       存在性，以及 apps/libs 源码注释里显式契约 §N 引用的目标存在性）/ 测试可移植性+
#       临时目录隔离 / MSVC hazard lint / 内置 nlohmann/json 版本一致性 / gtest 发现超时
#       纪律（#227）/ 门禁脚本参数自测（#281，build.sh/test.sh 重复 flag 必须累积）/
#       前端五件套。
#       MSVC 侧失败（Windows 头顺序、C4101、公开 SDK 头里的弃用 CRT 调用等）
#       Linux 编译器拦不到，由 scripts/check_msvc_hazards.py 做源码级 lint；
#       conanfile.py 与内置 JSON 单头的版本漂移由 scripts/check_json_vendor_version.py 拦住。
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
    # 先跑规则自测（#266）：R6「自由文本 k=v 必须双引号」的边界是实测出来的——
    # 第一版把 `reason=` 也纳入，而它同时用于枚举标识符（reason=negative_volume），
    # 直接让 8 个 bar_strategy 测试变红。selftest 用正/反样本锁住这条边界，
    # 防止后续有人「顺手把 reason 加回去」。显式串联：`set -u -o pipefail` 没有 -e。
    "$PYTHON" scripts/check_log_format.py --selftest || return $?
    "$PYTHON" scripts/check_log_format.py apps/ libs/
}

check_clang_format() {
    find libs apps \( -name '*.h' -o -name '*.cpp' \) -print0 |
        xargs -0 "$CF" --dry-run --Werror
}

check_docs_links() {
    # 先跑检查器自测：保证「缺失符号必报 / 白名单豁免 / 代码块忽略 / 路径检查不回退 /
    # 源码注释契约引用（缺失契约或小节必报、字符串与代码不算注释）」等形态仍被拦住，
    # 再扫真实文档与源码注释（#191、#213、#221）。
    # 必须显式串联（`|| return $?`）：`set -u -o pipefail` 没有 `-e`，自测失败会被下一条
    # 命令的退出码覆盖，门禁变恒真。
    "$PYTHON" scripts/check_docs_links.py --selftest || return $?
    "$PYTHON" scripts/check_docs_links.py
}

check_test_portability() {
    # 先跑检查器自测：保证已知绕过形态（多级路径 / (ec) 重载 / 折行 / 固定通道名）
    # 仍被拦住，再扫真实测试文件（#136）。
    "$PYTHON" scripts/check_test_portability.py --selftest
    "$PYTHON" scripts/check_test_portability.py
}

check_msvc_hazards() {
    "$PYTHON" scripts/check_msvc_hazards.py
}

check_json_vendor_version() {
    # 先跑自测（改了 conanfile 忘改内置单头 / 改了单头忘改记录等形态仍被拦住），
    # 再校验真实三方一致性（ADR 0026 决策 6 的机器化，#154）。
    # 两条命令必须显式串联：run_step 的形态是「调用一个函数、取它的 $?」，而
    # `set -u -o pipefail` 没有 `-e` —— 少写 `|| return` 时自测的失败会被后一条
    # 命令的 0 覆盖，门禁变恒真（实测：selftest rc=1 → step rc=0 → 仍然打印 ok）。
    "$PYTHON" scripts/check_json_vendor_version.py --selftest || return $?
    "$PYTHON" scripts/check_json_vendor_version.py
}

check_ci_log_pattern() {
    # ci-log.sh 的 ERROR_PATTERN 是「CI 红类别登记表」：漏掉一类错误会让整条排查链
    # 退化成翻几十 MB 日志（#200 实测多耗约 28 分钟），过宽则被 env dump 噪声占满窗口
    # 把真根因挤出 head -120（SCCACHE_*_ERROR 命中裸 `ERROR:`）。
    # 内置「必须命中 / 必须不命中」样本，改模式时先红后绿；--selftest 不联网、不需要
    # gh 登录态，可在 preflight 安全运行。
    "$ROOT/scripts/ci-log.sh" --selftest
}

check_gtest_discovery_timeout() {
    # gtest 用例发现超时纪律（#227）：Windows CI 已两次因 CMake 默认 5 秒发现超时
    # 与改动无关地变红（run 40856647679 / 41167289411）。CMake 没有全局超时变量，
    # 只能靠「所有注册都走 dz_add_test()」来保证不漏参数，本检查器就是这条纪律的机器守卫。
    # 先跑自测（直接调用 / 宏内漏参数 / 默认值被改小三种形态仍被拦住），再扫真实文件。
    # 同样显式串联：`set -u -o pipefail` 没有 `-e`，自测失败会被后一条的 0 覆盖。
    "$PYTHON" scripts/check_gtest_discovery_timeout.py --selftest || return $?
    "$PYTHON" scripts/check_gtest_discovery_timeout.py
}

check_commit_subject() {
    # 提交标题形态（#265）：`fix: #246 x` 既是 check-push-scope 要求的「含票号」，
    # 又是 GitHub 的 closing keyword 形态 —— PR 合并即提前关闭 issue，导致 §6 的
    # 关闭评论被静默丢弃（#246/#260/#243 实测）。票号应写进作用域 `fix(#246): ...`。
    # CI 两个 workflow 与 pre-commit hook 都调用同一份脚本；这里只跑自测，保证规则
    # 本身没坏（真实提交由各调用方按 range / message-file 校验）。
    "$PYTHON" scripts/check_commit_subject.py --selftest
}

check_push_scope_selftest() {
    # check-push-scope.sh 的 BASE 解析兜底（#265）：无上游 / 上游是 URL / 上游正常
    # 三种形态 + 票号匹配正则。历史 bug 是 `@{u}` 解析失败时 git **打印字面量**，
    # `|| true` 吞掉退出码后报出误导性的 "base ref not found: @{u}"（#246 实测 2 次）。
    "$ROOT/scripts/check-push-scope.sh" --selftest
}

check_script_args() {
    # 门禁脚本参数解析自测（#281）：`TARGET="${2:?}"` / `TEST_NAME="${2:?}"` 是覆盖式赋值，
    # 重复传 flag 只有最后一个生效且退出码 0——#250 实测 7 个 target 只构建了 1 个，
    # `gate: OK build` 假绿。已有两道机械防线（#190 verdict + last-status、#75/#81 管道）
    # 都只覆盖**管道**，不覆盖**参数**，这是同一失败类别里未被覆盖的入口。
    # selftest 用假 cmake/ctest/flock 断言真实构造出的命令行包含全部 flag 值；
    # 实现改回「只留最后一个」时多 target / 多 test-name 用例立刻红。
    "$PYTHON" "$ROOT/scripts/tests/test_script_args.py" --selftest
}

check_orphan_exports() {
    # 孤儿导出（#274）：本仓的收敛手法是「抽共用模块 + 保留旧名转发」，这个手法天然生产
    # 死定义，而两处现有护栏都不管——`noUnusedLocals` 不查 `export`，C++ 头文件里未使用的
    # `inline constexpr` 编译器不告警。实测复发 4 处、全靠 reviewer 事后抓出（#246 孤儿函数；
    # #248 两个零消费者常量别名，最后一处到第 2 轮只读验证才发现）。
    # 先跑自测（死定义 / 别名回归 / 过度导出分级 / 测试与 .vue 引用 / C++ / 排除项），
    # 再扫真实树。二级「过度导出」只打 note 不失败（不是死代码，按 AGENTS 改动纪律不删）。
    "$PYTHON" scripts/check_orphan_exports.py --selftest || return $?
    "$PYTHON" scripts/check_orphan_exports.py
}

check_ci_budget() {
    # Windows CI 墙钟分档口径（#267）：档位边界与预算由真实 PR 数据校准，改错了会让
    # 「CI 绿」这个信号失真（#260 的单一 ≤15min 目标就把 35min 的正常大改动判成失败）。
    # 先跑自测（8 条档位样本 + 预算单调性 + #263 回归），再跑 unittest 套件，
    # 最后用当前分支的真实 diff 报一次档位——只输出信息、不判定（没有实测墙钟可比）。
    "$PYTHON" "$ROOT/scripts/ci_budget.py" --selftest || return $?

    # unittest 的退出码必须显式取：写成 `cmd | tail -2` 时 `$?` 是 tail 的（兄弟检查器
    # 都显式串联）。这里先整体捕获输出再判退出码，失败时把完整输出打到 stderr——只留
    # `tail` 会把失败用例名丢掉，排查时等于没有信息。
    local ut_out ut_rc
    ut_out="$("$PYTHON" -m unittest discover -s "$ROOT/scripts/tests" 2>&1)"
    ut_rc=$?
    if [ "$ut_rc" -ne 0 ]; then
        printf '%s\n' "$ut_out" >&2
        return "$ut_rc"
    fi
    # 成功时只留最后一行（OK），避免刷屏；失败已在上面整段打到 stderr。
    printf '%s\n' "$ut_out" | tail -1

    # 三点（merge-base）而非两点：main 前进后两点会把无关提交算进改动规模。
    "$PYTHON" "$ROOT/scripts/ci_budget.py" --from-git "origin/main...HEAD" 2>/dev/null || true
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

    # generated.ts 新鲜度判据 = 「gen:types 是否幂等」，不是工作区与 git 索引/HEAD 的差异。
    # 收尾流程里门禁在提交之前（closeout §2 → §3）：按 git 比，已正确重生成、只是还没
    # add/commit 的合法状态会被误报 stale（#251 实测 124 行新内容被判成漂移，白跑 40s +
    # 一轮诊断）。改比 HEAD 同样不行——`git diff HEAD` 比的是工作区 vs HEAD，暂存与否都
    # 报差异（#281 实测 rc：未暂存 1 / 已暂存 1 / 干净 0），等于要求先提交才能跑门禁。
    # 幂等性判据只问「再生成一次会不会改动文件」，与暂存、提交、HEAD 位置全都无关。
    local types_file="$fe/src/types/generated.ts"
    if [ ! -f "$types_file" ]; then
        echo "error: generated.ts missing; run ./scripts/build.sh to restore it" >&2
        return 1
    fi
    local hash_before hash_after
    hash_before="$(git hash-object -- "$types_file")"
    (
        cd "$fe" || exit 1
        npm run gen:types
    ) || return 1
    hash_after="$(git hash-object -- "$types_file")"
    if [ "$hash_before" != "$hash_after" ]; then
        echo "error: generated.ts was stale and has just been regenerated; include it in the commit" >&2
        git diff --stat HEAD -- "$types_file" >&2 || true
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
run_step "vendored JSON version consistency" check_json_vendor_version
run_step "ci-log error pattern" check_ci_log_pattern
run_step "gtest discovery timeout (#227)" check_gtest_discovery_timeout
run_step "commit subject rules (#265)" check_commit_subject
run_step "push-scope base resolution (#265)" check_push_scope_selftest
run_step "gate script arg parsing (#281)" check_script_args
run_step "Windows CI budget bands (#267)" check_ci_budget
run_step "orphan exports (#274)" check_orphan_exports
if [ "$C_ONLY" -eq 0 ]; then
    run_step "frontend (type-check/lint/lint:css/test/gen:types)" check_frontend
fi

printf 'preflight: all %d checks passed in %ss\n' "$STEP" "$(( $(date +%s) - START ))"
