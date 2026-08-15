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
# Windows 平台定义
if(WIN32)
    target_compile_definitions(dzshm PUBLIC
        _CRT_SECURE_NO_WARNINGS
        _WIN32_WINNT=0x0602
    )
endif()

# ============================================================
# 单元测试：每个 *_test.cpp 独立编译运行
# ============================================================
file(GLOB SHM_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/*_test.cpp")
foreach(testfile ${SHM_TESTS})
    get_filename_component(testname ${testfile} NAME_WE)
    add_executable(dzshm_${testname} ${testfile} tests/test_main.cpp)
    target_link_libraries(dzshm_${testname} PRIVATE dzshm GTest::gtest)
    set_target_properties(dzshm_${testname} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${DZ_TESTS_DIR}
    )
    gtest_discover_tests(dzshm_${testname})
    install(TARGETS dzshm_${testname} RUNTIME DESTINATION ${DZ_INSTALL_DIR}/tests)
endforeach()

# ============================================================
# dz_shm_perf_bench: 核心 IPC 路径延迟基准 (写帧→唤醒→读出)
# 非测试断言, 仅输出 median/p99/max (README 30μs 目标的同进程下限参考)
# ============================================================
add_executable(dz_shm_perf_bench tests/perf_bench.cpp)
target_link_libraries(dz_shm_perf_bench PRIVATE dzshm)
set_target_properties(dz_shm_perf_bench PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${DZ_TESTS_DIR}
)
add_test(NAME PerfBench COMMAND dz_shm_perf_bench)
install(TARGETS dz_shm_perf_bench RUNTIME DESTINATION ${DZ_INSTALL_DIR}/tests)
