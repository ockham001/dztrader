# ============================================================
# dzshm: 共享内存 IPC（STATIC 库）
# 依赖策略接口层头文件 + dzcore（LastError）
# 性能关键路径禁日志禁异常
# ============================================================
add_library(dzshm STATIC)
file(GLOB SHM_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
target_sources(dzshm PRIVATE ${SHM_SOURCES})
target_include_directories(dzshm PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(dzshm
    PUBLIC strategy_api_headers dzcore
    PUBLIC Boost::boost nlohmann_json::nlohmann_json
)
# 可被链入 dztrader_sdk 动态库，静态库对象需 PIC（与 dzcore/dzdb/dztdstore/dztrading 同）
set_target_properties(dzshm PROPERTIES POSITION_INDEPENDENT_CODE ON)
# Windows 平台定义
if(WIN32)
    target_compile_definitions(dzshm PUBLIC
        _CRT_SECURE_NO_WARNINGS
        _WIN32_WINNT=0x0602
    )
endif()

# 测试共用的 shm 清理辅助头（tests/test_util.h），供 platform / master / webui 测试复用。
# test_util.h 引 test_temp_dir.h（唯一化临时目录，#136），故传递 dztest_support 的 include 路径。
add_library(dzshm_test_support INTERFACE)
target_include_directories(dzshm_test_support INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_link_libraries(dzshm_test_support INTERFACE dztest_support)

# ============================================================
# 单元测试：每个 *_test.cpp 独立编译运行
# ============================================================
file(GLOB SHM_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/*_test.cpp")
foreach(testfile ${SHM_TESTS})
    get_filename_component(testname ${testfile} NAME_WE)
    add_executable(dzshm_${testname} ${testfile} tests/test_main.cpp)
    target_link_libraries(dzshm_${testname} PRIVATE dzshm dztest_support GTest::gtest)
    set_target_properties(dzshm_${testname} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${DZ_TESTS_DIR}
    )
    # 锁序/跨进程用例挂死时由 ctest 兜底；正常单例为秒级
    gtest_discover_tests(dzshm_${testname} PROPERTIES LABELS fast TIMEOUT 120)
endforeach()

# ============================================================
# dz_shm_perf_bench: 核心 IPC 路径延迟基准 (写帧→唤醒→读出)
# 非测试断言, 仅输出 median/p99/max (README 30μs 目标的同进程下限参考)
# ============================================================
add_executable(dz_shm_perf_bench tests/perf_bench.cpp)
target_link_libraries(dz_shm_perf_bench PRIVATE dzshm dztest_support)
set_target_properties(dz_shm_perf_bench PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${DZ_TESTS_DIR}
)
add_test(NAME PerfBench COMMAND dz_shm_perf_bench)
# perf = 无阈值基准项, 不进默认 fast 门禁; ctest --preset <plat>-<config>-perf
# RUN_SERIAL: p99 基准不能与其它进程并行 (test.sh 默认并行度 4), 否则数值被 CPU 竞争污染
set_tests_properties(PerfBench PROPERTIES LABELS perf RUN_SERIAL TRUE)
