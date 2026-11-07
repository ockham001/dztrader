#!/usr/bin/env bash
# preflight.sh — 提交前本地门禁的唯一入口（Linux）
#
# 覆盖：日志格式 / clang-format 21.x / 文档链接（含活跃文档里反引号路径**与代码符号**的
#       存在性，以及 apps/libs 源码注释里显式契约 §N 引用的目标存在性）/ 测试可移植性+
#       临时目录隔离 / 共享测试头登记（#284，§5.4 索引的确定性校验）/ MSVC hazard lint /
#       内置 nlohmann/json 版本一致性 / ci-log 错误模式（#200）/ gtest 发现超时纪律
#       （#227）/ commit subject（#265）/ push-scope 自测（#265）/ 门禁脚本参数自测
#       （#281，build.sh/test.sh 重复 flag 必须累积）/ Windows CI 预算分档（#267）/
#       孤儿导出（#274）/ CI 快路径分类（#289，与 ci.yml scope job 共用一份 denylist）/
#       SYSMON 指标键集合（#309，四处副本 + snapshot_builder 分支覆盖）/ push 纪律
#       （#309，push-ticket.sh 的 scope+验证标记+push 串联）/ 审查验证标记（#289，pre-push
#       读取）/ 前端五件套。
#       MSVC 侧失败（Windows 头顺序、C4101、公开 SDK 头里的弃用 CRT 调用等）
#       Linux 编译器拦不到，由 scripts/check_msvc_hazards.py 做源码级 lint；
#       conanfile.py 与内置 JSON 单头的版本漂移由 scripts/check_json_vendor_version.py 拦住。
# CI 的 linux job 复用同一脚本的 C 段（--c-only），避免门禁命令在
# 人工 prompt、development-checklist 与 ci.yml 三处漂移。
#
# **步骤清单的唯一真相源是本脚本的 `run_step` 行**（#289 AC7）：`--help` 直接打印全部
# 步骤名与数量。AGENTS.md / ticket-closeout.md / development-checklist.md 只留指针——
# 手抄副本漏一项就会让终轮验证判 FAIL（#284 实测）。
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
    -h|--help)
        sed -n '2,/^$/p' "$0"
        # 步骤清单从**本脚本自身**提取（#289 AC7）：覆盖清单原先在 AGENTS / closeout /
        # checklist 三处手抄，漏一项就让终轮验证判 FAIL（#284 实测多耗 229s + 作废 150s
        # 机时）。这里只留一个可执行真相源，文档改为指针，不再逐项枚举。
        # 匹配允许缩进：前端阶段那一步在 `if [ "$C_ONLY" -eq 0 ]` 里；只认 `run_step "`
        # 形态，不把 `run_step() {` 的函数定义行算成步骤。
        STEP_COUNT="$(grep -cE '^[[:space:]]*run_step "' "$0")"
        printf '\n步骤（从本脚本的 run_step 提取，共 %s 步；--c-only 不含前端阶段）:\n' \
            "$STEP_COUNT"
        grep -oE '^[[:space:]]*run_step "[^"]+"' "$0" |
            sed 's/^[[:space:]]*run_step "/  - /; s/"$//'
        exit 0
        ;;
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
    # 显式串联（#289 AC1）：`set -u -o pipefail` 没有 -e，自测失败会被后一条命令的 0
    # 覆盖，step 打印 ok 而检查器已坏——同一失效形态在 #281/#284 修过 3 处，这是漏网。
    # 由 scripts/tests/test_preflight_chaining.py 做静态审计，防再引入。
    "$PYTHON" scripts/check_test_portability.py --selftest || return $?
    "$PYTHON" scripts/check_test_portability.py
}

check_shared_test_headers() {
    # 共享测试头登记（#284 AC4）：§5.4 的索引原本是手工台账——新写一个测试头、两个以上
    # 测试 .cpp 包含它，却忘了同一次提交登记，编译与测试全绿（#281 审查 B2 因此被判
    # BLOCKING）。检查器扫 apps/libs 下任意 tests/ 目录（含嵌套）的 *.h，按 basename
    # 统计测试 .cpp 使用者；使用者 >=2 且未登记（也没有显式豁免标记）即红。
    # 先跑自测（登记绿 / 未登记红 / 单一用途忽略 / 豁免标记绿 / 表格缺失 / 多路径行），
    # 再扫真实树。显式串联：`set -u -o pipefail` 没有 `-e`，自测失败会被后一条的 0 覆盖。
    "$PYTHON" scripts/check_shared_test_headers.py --selftest || return $?
    "$PYTHON" scripts/check_shared_test_headers.py
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

check_ci_scope() {
    # CI 快路径分类器（#289 AC3）：denylist（C/C++ 源、构建系统、前端、workflow、构建脚本）
    # 单一定义在 scripts/ci_scope.py，ci.yml 的 scope job 调同一份脚本——两处定义必然漂移。
    # 先跑自测（docs-only→fast / scripts-only→fast / scripts/build.sh→full / 1 个 cpp→full /
    # 混合→full / 空→fast 等 26 例），再分类本分支。
    # 显式串联：`set -u -o pipefail` 没有 -e，自测失败会被后一条的 0 覆盖。
    "$PYTHON" "$ROOT/scripts/ci_scope.py" --selftest || return $?
    # 三点（merge-base）而非两点：分类口径与 ci.yml 的 scope job 一致。
    if git rev-parse --verify --quiet origin/main >/dev/null 2>&1; then
        "$PYTHON" "$ROOT/scripts/ci_scope.py" --range "origin/main...HEAD"
    else
        # 浅克隆 / 无远端时无法分类；分类不是本步唯一目的（自测才是），显式说明不静默跳过。
        echo "    note: origin/main not found; branch classification skipped"
    fi
}

check_sysmon_metric_keys() {
    # 指标键集合检查器（#309 AC3）：同一份 15 键集合在 5 处有副本（sysmon.h 的
    # SYSMON_METRIC_KEYS / 契约 sysmon §2.5 键列表 / TS 夹具 / 两个 webui 测试夹具），
    # 当前一致但没有任何检查器绑定——#295 审查 finding 5 的「两份夹具 13/15 vs 15/15」
    # 只能靠人眼发现。先跑自测（四源相等 / 少键 / 多键 / 夹具缺键 / 缺分支 / 兜底漂移 /
    # 解析失败），再扫真实树。显式串联：`set -u -o pipefail` 没有 `-e`，自测失败会被
    # 后一条的 0 覆盖，step 打印 ok 而检查器已坏（#281/#284/#289 修过 3 处同类）。
    "$PYTHON" "$ROOT/scripts/check_sysmon_metric_keys.py" --selftest || return $?
    "$PYTHON" "$ROOT/scripts/check_sysmon_metric_keys.py"
}

check_push_ticket() {
    # push 纪律脚本（#309 AC6）：本波唯一违例 #297 的 git push 距首个审查 spawn 仅 19 s，
    # `pre-push` 的「no review marker」warning 是 warning-only，被无视后 `--force-with-lease`
    # 又产生第二个 CI run（19 min）。push-ticket.sh 把 check-push-scope + review_marker
    # 严格判定 + git push 串成一条命令（缺失/不一致默认拒绝，逃生口 DZ_ALLOW_UNREVIEWED_PUSH）；
    # selftest 用假 git/gh 在临时仓库复现三态，不联网、不碰工作区。
    "$ROOT/scripts/push-ticket.sh" --selftest
}

check_review_marker() {
    # 只读验证轮的机器标记（#289 AC6）：#253 的「修复后只读验证轮被宿主 turn 错误中止 →
    # 未验证即合并」只靠文档约束无效（动作点看不见）。脚本把「哪个轴上跑过验证、验证的是
    # 哪个 sha」写成 .cache/review/<issue>-<axis>.done，pre-push 读它只 warning 不拦截
    # （拦截会误伤：marker 可能来自另一台机器）。这里只跑自测——在临时 git 仓库里复现
    # 「同 sha 无 warning / 新 commit 后 warning 且 rc=0 / 无标记 warning」，不碰工作区。
    "$PYTHON" "$ROOT/scripts/review_marker.py" --selftest
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
run_step "shared test headers (#284)" check_shared_test_headers
run_step "msvc hazard lint" check_msvc_hazards
run_step "vendored JSON version consistency" check_json_vendor_version
run_step "ci-log error pattern" check_ci_log_pattern
run_step "gtest discovery timeout (#227)" check_gtest_discovery_timeout
run_step "commit subject rules (#265)" check_commit_subject
run_step "push-scope base resolution (#265)" check_push_scope_selftest
run_step "gate script arg parsing (#281)" check_script_args
run_step "Windows CI budget bands (#267)" check_ci_budget
run_step "orphan exports (#274)" check_orphan_exports
run_step "ci scope fast path (#289)" check_ci_scope
run_step "sysmon metric key set (#309)" check_sysmon_metric_keys
run_step "push ticket discipline (#309)" check_push_ticket
run_step "review verification marker (#289)" check_review_marker
if [ "$C_ONLY" -eq 0 ]; then
    run_step "frontend (type-check/lint/lint:css/test/gen:types)" check_frontend
fi

printf 'preflight: all %d checks passed in %ss\n' "$STEP" "$(( $(date +%s) - START ))"
