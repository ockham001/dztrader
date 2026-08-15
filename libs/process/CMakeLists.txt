# ============================================================
# dzprocess: 进程可执行文件扫描库（STATIC 库）
# 提供 scan_all_exes / find_exe_by_stem, 消除 master/dzweb 重复扫描逻辑
# 依赖 dzcore (this_process::app_root + to_utf8_from_system)
# ============================================================
add_library(dzprocess STATIC)
target_sources(dzprocess PRIVATE
    src/exe_scanner.cpp
)
target_include_directories(dzprocess PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(dzprocess
    PUBLIC dzcore
    PUBLIC spdlog::spdlog
)

# ============================================================
# 单元测试：每个 *_test.cpp 独立编译运行
# ============================================================
file(GLOB PROCESS_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/*_test.cpp")
foreach(testfile ${PROCESS_TESTS})
    get_filename_component(testname ${testfile} NAME_WE)
    add_executable(dzprocess_${testname} ${testfile} tests/test_main.cpp)
    target_link_libraries(dzprocess_${testname} PRIVATE dzprocess GTest::gtest)
    set_target_properties(dzprocess_${testname} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${DZ_TESTS_DIR}
    )
    gtest_discover_tests(dzprocess_${testname})
    install(TARGETS dzprocess_${testname} RUNTIME DESTINATION ${DZ_INSTALL_DIR}/tests)
endforeach()
