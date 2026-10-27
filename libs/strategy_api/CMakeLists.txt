# ============================================================
# 策略接口 SDK: 同一源码,两种链接方式
#   dztrader_sdk        -> 对外交付形态: 自包含动态库 dztrader_sdk.dll/.so
#                            (DZ_API_EXPORTS; 第三方依赖全部静态吸收, 默认构建)
#   dztrader_sdk_static -> 仓内/平台内部使用 (DZ_API_COMPILE_STATIC), 非交付物
# 示例策略 stg_demo 是动态库的第一个消费者 (strategies/demo, ADR 0020)
# 测试直接测试内部类（result_set 等），不链接 api.cpp
# ============================================================

# 共享源码列表（static / shared 复用）
set(SDK_SOURCES
    src/api.cpp
    src/cursor_result_set.cpp
    src/vector_result_set.cpp
    src/db_database.cpp
)

# 内部头文件目录（src 下的 .h 是内部实现头）
set(SDK_INTERNAL_INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/src)

# ============================================================
# dztrader_sdk_static: 静态库，仓内/平台内部使用 (ADR 0020 保留)
# 交付形态切到动态库后仓内暂无消费者; 不作为对外交付物, 不承诺 CMake 暴露面。
# ============================================================
add_library(dztrader_sdk_static STATIC ${SDK_SOURCES})
target_include_directories(dztrader_sdk_static PRIVATE ${SDK_INTERNAL_INCLUDES})
target_compile_definitions(dztrader_sdk_static PUBLIC DZ_API_COMPILE_STATIC)
target_link_libraries(dztrader_sdk_static
    PRIVATE dztrading
    PUBLIC strategy_api_headers dzcore dzshm dzdb dztdstore
    PUBLIC Boost::boost nlohmann_json::nlohmann_json SQLiteCpp
)

# ============================================================
# dztrader_sdk: 自包含动态库 dztrader_sdk.dll/.so (对外交付形态, ADR 0020)
# 只有 strategy_api_headers 是公开接口: 内部库与第三方一律 PRIVATE, 消费者
# 只 include 公开头文件 + 链接本库, 不继承任何第三方 include/link 依赖。
# 输出到构建根:Windows DLL 用 RUNTIME_OUTPUT_DIRECTORY,Linux .so 用 LIBRARY_OUTPUT_DIRECTORY
# ============================================================
add_library(dztrader_sdk SHARED ${SDK_SOURCES})
target_include_directories(dztrader_sdk PRIVATE ${SDK_INTERNAL_INCLUDES})
target_compile_definitions(dztrader_sdk PRIVATE DZ_API_EXPORTS)

# SONAME 版本化（ADR 0038）：Linux 产出 libdztrader.so -> .so.${SOVERSION} -> .so.${VERSION}
# SOVERSION 取 MAJOR：C ABI 破坏时 SO 名变化，加载器据此拒绝旧策略二进制。
# 0.x 阶段不承诺 ABI（ADR 0038 决策 1），故 SOVERSION 恒为 0。
# 产物名与 target 名分离（ADR 0038 决策 5）：
#   target  = dztrader_sdk   （CMake 侧唯一标识，仓内链接用）
#   产物    = dztrader       → libdztrader.so / dztrader.dll
# 交付物名与命名空间、聚合头 <dztrader.h>、规划中的 Python 包保持一致。
set_target_properties(dztrader_sdk PROPERTIES
    OUTPUT_NAME dztrader
    RUNTIME_OUTPUT_DIRECTORY ${DZ_OUTPUT_DIR}
    LIBRARY_OUTPUT_DIRECTORY ${DZ_OUTPUT_DIR}
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
)

# 符号收敛（ADR 0038 决策 7）：只导出 dz_* C ABI，内部 C++ 与第三方符号全部本地化。
# 手段是链接器白名单而非 -fvisibility=hidden —— 后者要改公共头的 DZ_API 宏，
# 且对 header-only 模板入口（dztrader::run_strategy）有副作用。
# 实测：13038 -> 50 个导出符号。
if(UNIX AND NOT APPLE)
    target_link_options(dztrader_sdk PRIVATE
        -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/exported_symbols.map
        -Wl,--exclude-libs,ALL
    )
    set_target_properties(dztrader_sdk PROPERTIES
        LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/exported_symbols.map)
endif()

target_link_libraries(dztrader_sdk
    PUBLIC strategy_api_headers
    PRIVATE dztrading dzcore dzshm dzdb dztdstore
    PRIVATE Boost::boost nlohmann_json::nlohmann_json SQLiteCpp
)
# 安装: 动态库与示例可执行同目录 (Linux $ORIGIN rpath / Windows DLL 同目录)。
# 带 SONAME 后 Linux 会装三个文件（.so 符号链接 / .so.N / .so.N.M.P），供链接与运行。
# 头文件、导入库、许可声明等完整分发包仍属 #7。
install(TARGETS dztrader_sdk
    RUNTIME DESTINATION ${DZ_INSTALL_DIR}
    LIBRARY DESTINATION ${DZ_INSTALL_DIR}
)

# ============================================================
# 单元测试：每个 *_test.cpp 独立编译
# 测试只编译内部类（不含 api.cpp，避免导出符号；测试 result_set 等）
# 例外：notify_ui_test / trade_api_test 链接真实 SDK (api.cpp)，
#       覆盖 dz_* C 接口的完整写入/错误路径。
# ============================================================
set(SDK_TEST_SOURCES
    src/vector_result_set.cpp
    src/cursor_result_set.cpp
)

# 链接 api.cpp 的测试 (覆盖 C 接口完整路径)
set(SDK_API_TESTS
    notify_ui_test
    trade_api_test
    md_api_test
    schedule_api_test
    strategy_engine_test
    db_test
    db_instruments_test
    td_ingest_wiring_test
    td_ingest_nodb_test
    td_ingest_watermark_test
    # CloseIsSafe 经公开 API dz_resultset_close 断言 -> 需链接 api.cpp
    result_set_test
)

# 穷举/慢速用例单独成 target, 打 slow 标签; 默认 fast 门禁只跑对应抽样版。
set(SDK_SLOW_TESTS
    date_exhaustive_test
    date_time_exhaustive_test
    # issue #92 差分验收全量档: 旧版查表语义 oracle × 多 seed / 多周期 / 多日流
    # 对应 fast 抽样版是 bar_diff_test.cpp（每条路径各跑一次，进默认门禁）
    bar_diff_exhaustive_test
)
# Series 层的 Debug 断言（DZTRADER_SERIES_ASSERT，组件文档 §4.2「Debug 抛前有 assert」）
# 先于异常触发。系列测试要断言的是「抛 std::out_of_range」这一行为，若断言生效会先 abort，
# 用例永远到不了 EXPECT_THROW —— 因此为 series 测试关闭断言。Release（NDEBUG）本就编译掉，
# 默认门禁因此不受影响；这里只影响 Debug/ASan 档。
set(SDK_SERIES_TESTS
    series_test
    series_window_test
)
# series_debug_assert_test 相反：**不**关闭断言，用 EXPECT_DEATH 验证「抛前有 assert」（AC2）。
# 下方构建时显式校验两个集合不相交——否则把 series_debug_assert_test 误加进
# SDK_SERIES_TESTS 会让断言被关掉、death 测试静默退化成 EXPECT_THROW（AC2 失去覆盖）。
set(SDK_SERIES_ASSERT_TESTS
    series_debug_assert_test
)
foreach(assert_test ${SDK_SERIES_ASSERT_TESTS})
    if(assert_test IN_LIST SDK_SERIES_TESTS)
        message(FATAL_ERROR
            "${assert_test} must not be in SDK_SERIES_TESTS: that set disables the "
            "DZTRADER_SERIES_ASSERT debug assertions this test exists to verify")
    endif()
endforeach()

file(GLOB SDK_TESTS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/*_test.cpp")
foreach(testfile ${SDK_TESTS})
    get_filename_component(testname ${testfile} NAME_WE)
    add_executable(dztrader_sdk_${testname}
        ${testfile}
        tests/test_main.cpp
        ${SDK_TEST_SOURCES})
    target_include_directories(dztrader_sdk_${testname} PRIVATE ${SDK_INTERNAL_INCLUDES})
    target_link_libraries(dztrader_sdk_${testname} PRIVATE
        dztrading strategy_api_headers dzcore dzshm dztest_support GTest::gtest
        Boost::boost nlohmann_json::nlohmann_json)
    if(testname IN_LIST SDK_SERIES_TESTS)
        # Debug 断言会 abort，遮蔽 EXPECT_THROW（见上方 SDK_SERIES_TESTS 注释）
        target_compile_definitions(dztrader_sdk_${testname} PRIVATE
            DZTRADER_SERIES_NO_DEBUG_ASSERT)
    endif()
    if(testname IN_LIST SDK_API_TESTS)
        target_sources(dztrader_sdk_${testname} PRIVATE src/api.cpp src/db_database.cpp)
        # api.cpp 直接编入测试可执行文件（自包含, 不经 DLL 导出）,
        # 语义同 dztrader_sdk_static: DZ_API 展开为空。
        # 缺此定义时 MSVC 落到 dllimport 分支, 库内定义函数体报 C2491。
        target_compile_definitions(dztrader_sdk_${testname} PRIVATE DZ_API_COMPILE_STATIC)
        # api.cpp 引用 dzdb 的 db_database.cpp 需链 dzdb (PUBLIC 传递 SQLiteCpp);
        # db_test 直接用 SQLiteCpp 建临时库, 经 dzdb PUBLIC 依赖满足
        # api.cpp 调 tdstore::query_instruments -> 链 dztdstore
        target_link_libraries(dztrader_sdk_${testname} PRIVATE dzdb dztdstore)
    endif()
    set_target_properties(dztrader_sdk_${testname} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${DZ_TESTS_DIR}
    )
    # SDK API 测试 (md/notify/trade/engine) 使用固定 DZTRADER_HOME + 机器全局 SHM 通道名,
    # gtest_discover_tests 把每个用例注册为独立 ctest 测试, -j 并行时同二进制
    # 多进程共用同一通道文件互相踩踏 -> RUN_SERIAL 串行化;
    # 均为秒级用例, TIMEOUT 60 防挂死兜底失效时拖死 CI
    # 测试分层标签: fast = 默认门禁; slow = 显式穷举/慢速档, 与 RUN_SERIAL/TIMEOUT 正交
    if(testname IN_LIST SDK_SLOW_TESTS)
        gtest_discover_tests(dztrader_sdk_${testname} PROPERTIES LABELS slow)
    elseif(testname IN_LIST SDK_API_TESTS)
        gtest_discover_tests(dztrader_sdk_${testname}
            PROPERTIES LABELS fast RUN_SERIAL TRUE TIMEOUT 60)
    else()
        gtest_discover_tests(dztrader_sdk_${testname} PROPERTIES LABELS fast)
    endif()
endforeach()

# ============================================================
# dztrader_sdk_bar_perf_bench: tick→bar 热路径基准 (#93, ADR 0031 决策 10)
# 分别报告 update / 新 bar / 跨日 / 竞价四条路径的 ns/tick 与 p99,
# 只输出数值不做阈值断言 (report-only, 阈值待数值稳定后升级为 CI 门禁)。
# 被测组件是 header-only, 直接编译进基准, 不链接任何内部库或第三方依赖。
# ============================================================
add_executable(dztrader_sdk_bar_perf_bench tests/bar_perf_bench.cpp)
target_link_libraries(dztrader_sdk_bar_perf_bench PRIVATE strategy_api_headers)
set_target_properties(dztrader_sdk_bar_perf_bench PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${DZ_TESTS_DIR}
)
add_test(NAME BarPerfBench COMMAND dztrader_sdk_bar_perf_bench)
# perf = 无阈值基准项, 不进默认 fast 门禁; ctest --preset <plat>-<config>-perf
set_tests_properties(BarPerfBench PROPERTIES LABELS perf)

# ============================================================
# 「仅凭 SDK include 路径可编译」双模检查 (#154, ADR 0026 决策 6)
#
# builtin 探针只链 strategy_api_headers 也能编译 + 运行, 即证明外部策略作者把 SDK 的
# include 目录加进头文件路径就能解析 JSON, 不需要 Conan 或额外安装:
#   builtin  : 默认模式, 用 SDK 内置单头 (dztrader/json.h 的内置分支); 只链
#              strategy_api_headers (编译命令里没有第三方 include 路径)
#   external : 定义 DZTRADER_JSON_EXTERNAL, 用外部 <nlohmann/json.hpp>; 两个探针之间
#              只有它额外链 nlohmann_json::nlohmann_json 提供外部头路径,
#              模拟策略作者自带版本 (本文件其它 target 也链它, 是内部库用途)
# 两者都跑起来(不只编译)以证明取到的是能工作的 nlohmann, 不是只有预处理通过的空壳。
# 均为百分之一秒级用例, 进默认 fast 门禁; 不用 gtest (无断言框架需求, 减少依赖面)。
# ============================================================
add_executable(dztrader_sdk_json_probe_builtin tests/json_sdk_probe.cpp)
target_link_libraries(dztrader_sdk_json_probe_builtin PRIVATE strategy_api_headers)

add_executable(dztrader_sdk_json_probe_external tests/json_sdk_probe.cpp)
target_link_libraries(dztrader_sdk_json_probe_external PRIVATE
    strategy_api_headers nlohmann_json::nlohmann_json)
# 测试专用宏: 让探针能断言「外部宏真的生效了」而不是静默回退内置模式
target_compile_definitions(dztrader_sdk_json_probe_external PRIVATE
    DZTRADER_JSON_EXTERNAL DZ_JSON_PROBE_EXPECT_EXTERNAL)

# 聚合头隔离检查: <dztrader.h> 不得引入内置 JSON 单头 (#154 验收项)
add_executable(dztrader_sdk_aggregate_header_probe tests/aggregate_header_probe.cpp)
target_link_libraries(dztrader_sdk_aggregate_header_probe PRIVATE strategy_api_headers)

foreach(probe
    dztrader_sdk_json_probe_builtin
    dztrader_sdk_json_probe_external
    dztrader_sdk_aggregate_header_probe)
    set_target_properties(${probe} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${DZ_TESTS_DIR})
    add_test(NAME ${probe} COMMAND ${probe})
    set_tests_properties(${probe} PROPERTIES LABELS fast)
endforeach()
