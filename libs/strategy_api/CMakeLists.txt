# ============================================================
# 策略接口 SDK: 同一源码,两种链接方式
#   dzstrategy_sdk        -> 对外交付形态: 自包含动态库 dzstrategy_sdk.dll/.so
#                            (DZ_API_EXPORTS; 第三方依赖全部静态吸收, 默认构建)
#   dzstrategy_sdk_static -> 仓内/平台内部使用 (DZ_API_COMPILE_STATIC), 非交付物
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
# dzstrategy_sdk_static: 静态库，仓内/平台内部使用 (ADR 0020 保留)
# 交付形态切到动态库后仓内暂无消费者; 不作为对外交付物, 不承诺 CMake 暴露面。
# ============================================================
add_library(dzstrategy_sdk_static STATIC ${SDK_SOURCES})
target_include_directories(dzstrategy_sdk_static PRIVATE ${SDK_INTERNAL_INCLUDES})
target_compile_definitions(dzstrategy_sdk_static PUBLIC DZ_API_COMPILE_STATIC)
target_link_libraries(dzstrategy_sdk_static
    PRIVATE dztrading
    PUBLIC strategy_api_headers dzcore dzshm dzdb dztdstore
    PUBLIC Boost::boost nlohmann_json::nlohmann_json SQLiteCpp
)

# ============================================================
# dzstrategy_sdk: 自包含动态库 dzstrategy_sdk.dll/.so (对外交付形态, ADR 0020)
# 只有 strategy_api_headers 是公开接口: 内部库与第三方一律 PRIVATE, 消费者
# 只 include 公开头文件 + 链接本库, 不继承任何第三方 include/link 依赖。
# 输出到构建根:Windows DLL 用 RUNTIME_OUTPUT_DIRECTORY,Linux .so 用 LIBRARY_OUTPUT_DIRECTORY
# ============================================================
add_library(dzstrategy_sdk SHARED ${SDK_SOURCES})
target_include_directories(dzstrategy_sdk PRIVATE ${SDK_INTERNAL_INCLUDES})
target_compile_definitions(dzstrategy_sdk PRIVATE DZ_API_EXPORTS)
set_target_properties(dzstrategy_sdk PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${DZ_OUTPUT_DIR}
    LIBRARY_OUTPUT_DIRECTORY ${DZ_OUTPUT_DIR}
)
target_link_libraries(dzstrategy_sdk
    PUBLIC strategy_api_headers
    PRIVATE dztrading dzcore dzshm dzdb dztdstore
    PRIVATE Boost::boost nlohmann_json::nlohmann_json SQLiteCpp
)
# 安装: 动态库与示例可执行同目录 (Linux $ORIGIN rpath / Windows DLL 同目录)。
# 本票只安装动态库本身以保证「示例随装可运行」; 符号收敛/SONAME/头文件/
# 导入库/许可声明等完整分发包属 #7 —— 在此之前该安装不是对外分发包。
install(TARGETS dzstrategy_sdk
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
file(GLOB SDK_TESTS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/*_test.cpp")
foreach(testfile ${SDK_TESTS})
    get_filename_component(testname ${testfile} NAME_WE)
    add_executable(dzstrategy_sdk_${testname}
        ${testfile}
        tests/test_main.cpp
        ${SDK_TEST_SOURCES})
    target_include_directories(dzstrategy_sdk_${testname} PRIVATE ${SDK_INTERNAL_INCLUDES})
    target_link_libraries(dzstrategy_sdk_${testname} PRIVATE
        dztrading strategy_api_headers dzcore dzshm dztest_support GTest::gtest
        Boost::boost nlohmann_json::nlohmann_json)
    if(testname IN_LIST SDK_API_TESTS)
        target_sources(dzstrategy_sdk_${testname} PRIVATE src/api.cpp src/db_database.cpp)
        # api.cpp 直接编入测试可执行文件（自包含, 不经 DLL 导出）,
        # 语义同 dzstrategy_sdk_static: DZ_API 展开为空。
        # 缺此定义时 MSVC 落到 dllimport 分支, 库内定义函数体报 C2491。
        target_compile_definitions(dzstrategy_sdk_${testname} PRIVATE DZ_API_COMPILE_STATIC)
        # api.cpp 引用 dzdb 的 db_database.cpp 需链 dzdb (PUBLIC 传递 SQLiteCpp);
        # db_test 直接用 SQLiteCpp 建临时库, 经 dzdb PUBLIC 依赖满足
        # api.cpp 调 tdstore::query_instruments -> 链 dztdstore
        target_link_libraries(dzstrategy_sdk_${testname} PRIVATE dzdb dztdstore)
    endif()
    set_target_properties(dzstrategy_sdk_${testname} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${DZ_TESTS_DIR}
    )
    # SDK API 测试 (md/notify/trade/engine) 使用固定 DZTRADER_HOME + 机器全局 SHM 通道名,
    # gtest_discover_tests 把每个用例注册为独立 ctest 测试, -j 并行时同二进制
    # 多进程共用同一通道文件互相踩踏 -> RUN_SERIAL 串行化;
    # 均为秒级用例, TIMEOUT 60 防挂死兜底失效时拖死 CI
    # 测试分层标签: fast = 默认门禁; slow = 显式穷举/慢速档, 与 RUN_SERIAL/TIMEOUT 正交
    if(testname IN_LIST SDK_SLOW_TESTS)
        gtest_discover_tests(dzstrategy_sdk_${testname} PROPERTIES LABELS slow)
    elseif(testname IN_LIST SDK_API_TESTS)
        gtest_discover_tests(dzstrategy_sdk_${testname}
            PROPERTIES LABELS fast RUN_SERIAL TRUE TIMEOUT 60)
    else()
        gtest_discover_tests(dzstrategy_sdk_${testname} PROPERTIES LABELS fast)
    endif()
    install(TARGETS dzstrategy_sdk_${testname} RUNTIME DESTINATION ${DZ_INSTALL_DIR}/tests)
endforeach()

# ============================================================
# dzstrategy_sdk_bar_perf_bench: tick→bar 热路径基准 (#93, ADR 0031 决策 10)
# 分别报告 update / 新 bar / 跨日 / 竞价四条路径的 ns/tick 与 p99,
# 只输出数值不做阈值断言 (report-only, 阈值待数值稳定后升级为 CI 门禁)。
# 被测组件是 header-only, 直接编译进基准, 不链接任何内部库或第三方依赖。
# ============================================================
add_executable(dzstrategy_sdk_bar_perf_bench tests/bar_perf_bench.cpp)
target_link_libraries(dzstrategy_sdk_bar_perf_bench PRIVATE strategy_api_headers)
set_target_properties(dzstrategy_sdk_bar_perf_bench PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${DZ_TESTS_DIR}
)
add_test(NAME BarPerfBench COMMAND dzstrategy_sdk_bar_perf_bench)
# perf = 无阈值基准项, 不进默认 fast 门禁; ctest --preset <plat>-<config>-perf
set_tests_properties(BarPerfBench PROPERTIES LABELS perf)
install(TARGETS dzstrategy_sdk_bar_perf_bench RUNTIME DESTINATION ${DZ_INSTALL_DIR}/tests)
