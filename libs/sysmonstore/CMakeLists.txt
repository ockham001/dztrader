# ============================================================
# dzsysmonstore：dzsysmon 历史库的共享存储层（STATIC 库）
#
# 职责：历史库 schema 的**唯一**定义 + 行编解码 + 写入方门面 + 只读读取方门面
#       + 落盘线程（有界队列 + 批量提交）。
#
# 为什么单独成库而不是塞进 libs/db 或 apps/sysmon：历史库由 `dzsysmon` 唯一写入、
# `dzweb` 只读（#41 spec「历史存储」），schema 与行编解码必须只有一份定义。放进任一
# app 都会迫使另一方跨 app 依赖，而 libs/db 是 sqlite/mysql 可切换的通用端口，
# 历史库只要本地 SQLite（保留与压缩需要 WITHOUT ROWID 复合主键、DELETE..LIMIT 等
# 端口不暴露的能力）。
#
# 依赖方向：SQLiteCpp + dzcore（日志/异常）；不依赖 dzdb。
# ============================================================
add_library(dzsysmonstore STATIC)
file(GLOB SYSMONSTORE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
target_sources(dzsysmonstore PRIVATE ${SYSMONSTORE_SOURCES})
target_include_directories(dzsysmonstore PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(dzsysmonstore
    PUBLIC dzcore
    PUBLIC SQLiteCpp
)
# Conan 的 sqlite3 recipe 不传递 include dir（与 apps/webui 同一处理）：
# 测试直接用 sqlite3 C API 造异常库验证降级路径。
find_path(SQLITE3_INCLUDE_DIR NAMES sqlite3.h REQUIRED)
if(WIN32)
    target_compile_definitions(dzsysmonstore PUBLIC _CRT_SECURE_NO_WARNINGS)
endif()
# 可被链入动态库的场景需要 PIC（与 dzdb / dztdstore 保持一致）。
set_target_properties(dzsysmonstore PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ============================================================
# 单元测试：每个 *_test.cpp 独立编译运行
# ============================================================
file(GLOB SYSMONSTORE_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/*_test.cpp")
foreach(testfile ${SYSMONSTORE_TESTS})
    get_filename_component(testname ${testfile} NAME_WE)
    dz_add_test(dzsysmonstore_${testname} SOURCES ${testfile} tests/test_main.cpp LABELS fast)
    target_link_libraries(dzsysmonstore_${testname} PRIVATE dzsysmonstore dztest_support GTest::gtest)
    # 测试直接用 sqlite3 造「版本不符/无 schema」的库来验证读取方降级路径
    # （写入方 API 故意不提供改版本的能力）。
    target_include_directories(dzsysmonstore_${testname} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/tests
        ${SQLITE3_INCLUDE_DIR}
    )
endforeach()
