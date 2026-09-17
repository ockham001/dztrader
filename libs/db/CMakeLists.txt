# ============================================================
# dzdb: 数据库统一接口 + SQLite 驱动（STATIC 库）
# 依赖 SQLiteCpp (Conan 提供) + dzcore
# 并发模型: Database 共享, Session 一线程一个（每会话独立连接）
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
# 可被链入 dzstrategy_sdk 动态库，静态库对象需 PIC。
set_target_properties(dzdb PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ============================================================
# 单元测试：每个 *_test.cpp 独立编译运行
# ============================================================
file(GLOB DB_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/*_test.cpp")
foreach(testfile ${DB_TESTS})
    get_filename_component(testname ${testfile} NAME_WE)
    add_executable(dzdb_${testname} ${testfile} tests/test_main.cpp)
    target_link_libraries(dzdb_${testname} PRIVATE dzdb GTest::gtest)
    target_include_directories(dzdb_${testname} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests/conformance)
    set_target_properties(dzdb_${testname} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${DZ_TESTS_DIR})
    gtest_discover_tests(dzdb_${testname})
    install(TARGETS dzdb_${testname} RUNTIME DESTINATION ${DZ_INSTALL_DIR}/tests)
endforeach()
