# ============================================================
# dzcore: 基础设施（STATIC 库）
# 配置 + LastError + Exception
# 依赖策略接口层头文件获取类型/错误码
# ============================================================
add_library(dzcore STATIC)
target_sources(dzcore PRIVATE
    src/encoding.cpp
    src/env.cpp
    src/last_error.cpp
    src/path.cpp
    src/this_process.cpp
    src/timer_queue.cpp
)
target_include_directories(dzcore PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(dzcore
    PUBLIC strategy_api_headers
    PUBLIC Boost::process spdlog::spdlog toml11::toml11
           nlohmann_json::nlohmann_json magic_enum::magic_enum
)

# ============================================================
# 单元测试：每个 *_test.cpp 独立编译运行
# ============================================================
file(GLOB CORE_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/*_test.cpp")
foreach(testfile ${CORE_TESTS})
    get_filename_component(testname ${testfile} NAME_WE)
    add_executable(dzcore_${testname} ${testfile} tests/test_main.cpp)
    target_link_libraries(dzcore_${testname} PRIVATE dzcore GTest::gtest)
    set_target_properties(dzcore_${testname} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${DZ_TESTS_DIR}
    )
    gtest_discover_tests(dzcore_${testname})
    install(TARGETS dzcore_${testname} RUNTIME DESTINATION ${DZ_INSTALL_DIR}/tests)
endforeach()
