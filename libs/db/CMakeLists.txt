# ============================================================
# dzdb: SQLite 持久化封装（STATIC 库）
# 依赖 SQLiteCpp (Conan 提供) + dzcore
# 单线程独占 (dztd_ctp 的 PersistWriter 线程独占, 无并发)
# ============================================================
add_library(dzdb STATIC)
file(GLOB DB_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
target_sources(dzdb PRIVATE ${DB_SOURCES})
target_include_directories(dzdb PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(dzdb
    PUBLIC dzcore
    PUBLIC SQLiteCpp
)
if(WIN32)
    target_compile_definitions(dzdb PUBLIC _CRT_SECURE_NO_WARNINGS _WIN32_WINNT=0x0602)
endif()

# ============================================================
# 单元测试：每个 *_test.cpp 独立编译运行
# ============================================================
file(GLOB DB_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/*_test.cpp")
foreach(testfile ${DB_TESTS})
    get_filename_component(testname ${testfile} NAME_WE)
    add_executable(dzdb_${testname} ${testfile} tests/test_main.cpp)
    target_link_libraries(dzdb_${testname} PRIVATE dzdb GTest::gtest)
    set_target_properties(dzdb_${testname} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${DZ_TESTS_DIR})
    gtest_discover_tests(dzdb_${testname})
    install(TARGETS dzdb_${testname} RUNTIME DESTINATION ${DZ_INSTALL_DIR}/tests)
endforeach()
