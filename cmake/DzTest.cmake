# ============================================================
# dz_add_test(): 测试目标 + gtest 用例发现的唯一构造入口（#227）
# ============================================================
# 为什么需要这个宏（不是风格偏好，是故障修复）：
#
# `gtest_discover_tests` 在 **POST_BUILD** 阶段执行测试二进制枚举用例。CMake 的
# 默认发现超时只有 5 秒（`Modules/GoogleTest.cmake`：
# `if(NOT arg_DISCOVERY_TIMEOUT) set(arg_DISCOVERY_TIMEOUT 5)`），而 CMake **只**提供
# 全局的 `CMAKE_GTEST_DISCOVER_TESTS_DISCOVERY_MODE`，没有全局超时变量 —— 超时只能
# 逐个调用点显式传参。漏传即退回 5 秒，且这个退化在 Linux 上永远不可见（从不接近
# 边界），只在 Windows CI 冷启动（sccache READ_ONLY + 近千 target 并发链接 + 杀毒/IO
# 抖动）时炸成：
#
#     CMake Error at .../GoogleTestAddTests.cmake:233 (message):
#       Error running test executable.
#         Result: Process terminated due to timeout
#         Timeout: '5'
#
# 已两次与改动内容无关地让 Windows CI 变红：
#   - 2026-10-29 feat/issue-162-dynamic-commands，run 40856647679（attempt 2 通过）
#   - 2026-10-31 fix/issue-200-dashboard-demo-data，run 41167289411（attempt 3 通过）
# 每次红 ≈ 14 分钟 CI + 排查时间，并让「CI 绿」这个门禁信号失真（#227）。
#
# 因此把「建 target + 输出目录 + 发现超时」收进一个宏，调用点只写一行
# `dz_add_test(...)`：超时参数就不可能被漏掉。这也符合仓库纪律里「同一事实只定义
# 一处」—— 之前同一个 3~4 行的 target 模板被复制了 20 多份，任何一处漏参数都没人
# 发现（正确形态就是让它无处可漏）。
#
# 30 秒的依据：实测正常发现耗时是毫秒级（最慢的 SDK 测试二进制也只几十毫秒），
# 30 秒是数百倍余量，同时远小于用例级 `TIMEOUT`（60/120s），不会把真挂死伪装成
# 正常等待。改小等于把已知 flaky 重新引入，`scripts/check_gtest_discovery_timeout.py`
# 的 R3 会拦下。
# ============================================================

set(DZ_GTEST_DISCOVERY_TIMEOUT 30 CACHE STRING
    "gtest_discover_tests discovery timeout in seconds (#227); do not lower below 30")

# dz_add_test(<target> SOURCES <src...> [LABELS <label...>] [PROPERTIES <key value...>]
#             [POST_BUILD_COMMAND <command...>] [POST_BUILD_COMMENT <text>])
#
#   SOURCES             必需；交给 add_executable 的源文件（含各模块自己的 tests/test_main.cpp）
#   LABELS              ctest 标签，默认 fast（单元测试判据 §4.3 的分层口径）
#   PROPERTIES          透传给 gtest_discover_tests 的用例属性（RUN_SERIAL TRUE / TIMEOUT 60 ...）
#   POST_BUILD_COMMAND  测试目标自己的 POST_BUILD 命令，**必须**走这里而不是在宏调用后再写
#                       add_custom_command：gtest_discover_tests 也是 POST_BUILD，同一目标的
#                       POST_BUILD 按声明顺序执行，后发现者在前者之后（strategies/demo 依赖
#                       这个顺序把 dztrader_sdk DLL 复制到测试 exe 旁边，否则 Windows 上发现
#                       阶段加载 DLL 失败）
#   POST_BUILD_COMMENT  上条命令的构建期回显文本（可选）
function(dz_add_test target)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "POST_BUILD_COMMENT"
                          "SOURCES;LABELS;PROPERTIES;POST_BUILD_COMMAND")
    if(NOT arg_SOURCES)
        message(FATAL_ERROR "dz_add_test(${target}): SOURCES is required")
    endif()

    add_executable(${target} ${arg_SOURCES})
    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${DZ_TESTS_DIR})

    if(arg_POST_BUILD_COMMAND)
        if(arg_POST_BUILD_COMMENT)
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${arg_POST_BUILD_COMMAND}
                COMMENT "${arg_POST_BUILD_COMMENT}")
        else()
            add_custom_command(TARGET ${target} POST_BUILD COMMAND ${arg_POST_BUILD_COMMAND})
        endif()
    endif()

    if(NOT arg_LABELS)
        set(arg_LABELS fast)
    endif()
    gtest_discover_tests(${target}
        PROPERTIES LABELS ${arg_LABELS} ${arg_PROPERTIES}
        DISCOVERY_TIMEOUT ${DZ_GTEST_DISCOVERY_TIMEOUT})
endfunction()
