# ============================================================
# dztdstore: 交易库 schema v1 平台自有声明 + 建库/重建门面 + 规范记录 + store facade
# （STATIC 库；schema v1 声明与建库直连 SQLiteCpp，不经 dzdb 公共端口）
#
# 过渡期：td 写端与 SDK 仍经 dzdb 端口读写同一份 schema，由 schema_dzdb.h 投影，
# 故本目标暂保留 dzdb 依赖；#245 / #252 删除投影后移除（ADR 0041 决策 5）。
# ============================================================
add_library(dztdstore STATIC)
file(GLOB TDSTORE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
target_sources(dztdstore PRIVATE ${TDSTORE_SOURCES})
target_include_directories(dztdstore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(dztdstore
    PUBLIC dzdb
    PUBLIC dzcore
    PUBLIC SQLiteCpp
)
if(WIN32)
    target_compile_definitions(dztdstore PUBLIC _CRT_SECURE_NO_WARNINGS _WIN32_WINNT=0x0602)
endif()
# 将被链入 dztrader_sdk 动态库，静态库对象需 PIC。
set_target_properties(dztdstore PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ============================================================
# 单元测试：每个 *_test.cpp 独立编译运行
# ============================================================
file(GLOB TDSTORE_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/*_test.cpp")
foreach(testfile ${TDSTORE_TESTS})
    get_filename_component(testname ${testfile} NAME_WE)
    dz_add_test(dztdstore_${testname} SOURCES ${testfile} tests/test_main.cpp LABELS fast)
    target_link_libraries(dztdstore_${testname} PRIVATE dztdstore dztest_support GTest::gtest)
endforeach()
