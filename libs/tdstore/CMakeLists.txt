# ============================================================
# dztdstore: td 库规范记录 + schema 迁移 + store ops（STATIC 库）
# 依赖 dzdb（Database/QueryResult/SQLiteCpp）+ dzcore
# ============================================================
add_library(dztdstore STATIC)
file(GLOB TDSTORE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
target_sources(dztdstore PRIVATE ${TDSTORE_SOURCES})
target_include_directories(dztdstore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(dztdstore
    PUBLIC dzdb
)
if(WIN32)
    target_compile_definitions(dztdstore PUBLIC _CRT_SECURE_NO_WARNINGS _WIN32_WINNT=0x0602)
endif()
# 将被链入 dzstrategy_sdk 动态库，静态库对象需 PIC。
set_target_properties(dztdstore PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ============================================================
# 单元测试：每个 *_test.cpp 独立编译运行
# ============================================================
file(GLOB TDSTORE_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/*_test.cpp")
foreach(testfile ${TDSTORE_TESTS})
    get_filename_component(testname ${testfile} NAME_WE)
    add_executable(dztdstore_${testname} ${testfile} tests/test_main.cpp)
    target_link_libraries(dztdstore_${testname} PRIVATE dztdstore GTest::gtest)
    set_target_properties(dztdstore_${testname} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${DZ_TESTS_DIR})
    gtest_discover_tests(dztdstore_${testname})
    install(TARGETS dztdstore_${testname} RUNTIME DESTINATION ${DZ_INSTALL_DIR}/tests)
endforeach()
