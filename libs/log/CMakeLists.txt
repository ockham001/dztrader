# ============================================================
# dzlog: 日志库（STATIC 库）
# 日志初始化 + 崩溃处理，依赖 spdlog + boost::stacktrace
# 仅平台进程使用，不编译进 dzstrategy_sdk.dll
# ============================================================
add_library(dzlog STATIC)
target_sources(dzlog PRIVATE src/log.cpp)
target_include_directories(dzlog PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(dzlog
    PUBLIC dzcore
    PUBLIC spdlog::spdlog Boost::stacktrace
)
# spdlog 配置：用 std::format + spdlog 默认 level 名（trace/debug/info/warning/error/critical）
# SPDLOG_USE_STD_FORMAT 与 conan recipe 注入重复，保留作为防御性兜底。
target_compile_definitions(dzlog PUBLIC
    SPDLOG_USE_STD_FORMAT
)

# ============================================================
# 单元测试：每个 *_test.cpp 独立编译
# ============================================================
file(GLOB LOG_TESTS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/*_test.cpp")
foreach(testfile ${LOG_TESTS})
    get_filename_component(testname ${testfile} NAME_WE)
    add_executable(dzlog_${testname} ${testfile} tests/test_main.cpp)
    target_link_libraries(dzlog_${testname} PRIVATE dzlog GTest::gtest)
    set_target_properties(dzlog_${testname} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${DZ_TESTS_DIR}
    )
    gtest_discover_tests(dzlog_${testname})
    install(TARGETS dzlog_${testname} RUNTIME DESTINATION ${DZ_INSTALL_DIR}/tests)
endforeach()
